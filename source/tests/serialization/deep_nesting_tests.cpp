// Values nest as deep as memory allows: parsing, from_json/from_binary, to_json/to_binary, deep copy, == and <=> walk them
// on work lists, so a host thread's ordinary stack (1 MB here, as a host's main thread) never bounds the depth

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/serialization/archive.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/serialization/binary_archive.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/core/dynamic_binder.hpp>

#include <exception>
#include <stdexcept>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "foundry/small_stack.hpp"

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace deep_detail {
	constexpr int k_depth = 100000;
	// a class chain this long overflows k_small_stack under any walk spending 64 bytes of native stack a link (a host record's load spends more)
	constexpr int k_class_chain_depth = 20000;
	static_assert(static_cast<size_t>(k_class_chain_depth) * 64 > k_small_stack);

	// a host record's native frames differ by build (Release, Debug, ASan): no build fills the roomy stack before the 1024-record
	// backstop, and the tight one is the stack margin (256 KB on Windows) and one allocation unit, which no chain of records fits
	constexpr size_t k_roomy_stack = size_t(512) << 20;
	constexpr size_t k_tight_stack = 320 * 1024;

	std::string nested_arrays(int depth, const std::string& leaf = "") { return std::string(depth, '[') + leaf + std::string(depth, ']'); }
	std::string nested_maps(int depth, const std::string& leaf = "1") {
		std::string result;
		result.reserve(static_cast<size_t>(depth) * 6 + leaf.size());
		for (int i = 0; i < depth; ++i) { result += "{\"a\":"; }
		result += leaf;
		result.append(static_cast<size_t>(depth), '}');
		return result;
	}
	std::string nested_links(int depth, int last) {
		std::string result;
		for (int i = 0; i < depth; ++i) { result += "{\"$type\":\"Link\",\"$val\":{\"n\":" + std::to_string(i == depth - 1 ? last : i) + ",\"next\":"; }
		result += "null";
		result.append(static_cast<size_t>(depth) * 2, '}');
		return result;
	}

	// A copyable host type whose copy constructor reports its id: the order of the reports is the order a copy visits
	struct copy_hook {
		engine* eng = nullptr;
		script_int id = 0;
		copy_hook() = default;
		explicit copy_hook(engine* a_engine) : eng(a_engine) {}
		copy_hook(const copy_hook& a_other) : eng(a_other.eng), id(a_other.id) { detail::engine_access::get_output_stream(*eng) << id << " "; }
		copy_hook& operator=(const copy_hook&) = default;
	};

	// Ids in the order a copy of the copy test's nest reaches them: each level's first hook, the level below, then its last
	// hook (and, for a script class over the host type, its own C++ object after its fields)
	std::string expected_copy_order(int depth, bool own_object) {
		std::string result;
		for (int i = 0; i <= depth; ++i) { result += std::to_string(i) + " "; }
		for (int i = depth; i >= 0; --i) {
			if (i < depth || own_object) { result += std::to_string(1000 + i) + " "; }
			if (own_object) { result += std::to_string(2000 + i) + " "; }
		}
		return result;
	}

	// A chain of host records: their own serialize recurses through host code, so the archive's guard bounds it
	struct host_link {
		int n = 0;
		std::shared_ptr<host_link> next;
		template<typename Archive>
		void serialize(Archive& ar) {
			ar("n", n);
			ar("next", next);
		}
	};

	std::string host_chain_json(int depth) {
		std::string result = "{\"value0\":";
		for (int i = 0; i < depth; ++i) { result += "{\"$id\":" + std::to_string(i + 1) + ",\"$val\":{\"n\":" + std::to_string(i) + ",\"next\":"; }
		result += "null";
		for (int i = 0; i < depth; ++i) { result += "}}"; }
		result += "}";
		return result;
	}

	// The binary wire form of the same chain, built without the writer (which recurses once per level itself)
	std::vector<uint8_t> host_chain_binary(size_t depth) {
		std::vector<uint8_t> out;
		auto u8 = [&out](uint8_t v) { out.push_back(v); };
		auto u32 = [&out](uint32_t v) { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>(v >> (8 * i))); } };
		auto str = [&](const std::string& s) { u32(static_cast<uint32_t>(s.size())); out.insert(out.end(), s.begin(), s.end()); };
		const uint32_t link_overhead = 39;     // 01, "", version, count, "n", "next", two sizes, the int, 02
		const uint32_t record_overhead = 41;   // 01, "", version, count, "$id", "$val", two sizes, the id, 02
		std::vector<uint32_t> next_size(depth), value_size(depth);
		uint32_t below = 1;   // the deepest next: the null marker
		for (size_t level = depth; level-- > 0;) {
			next_size[level] = below;
			value_size[level] = link_overhead + below;
			below = record_overhead + value_size[level];
		}
		u8(0x01); str(""); u32(1); u32(1); str("value0"); u32(below);
		for (size_t level = 0; level < depth; ++level) {
			u8(0x01); str(""); u32(0); u32(2); str("$id"); str("$val"); u32(4); u32(value_size[level]);
			u32(static_cast<uint32_t>(level + 1));
			u8(0x01); str(""); u32(0); u32(2); str("n"); str("next"); u32(4); u32(next_size[level]);
			u32(static_cast<uint32_t>(level));
		}
		u8(0x00);
		for (size_t level = 0; level < depth; ++level) { u8(0x02); u8(0x02); }
		u8(0x02);
		return out;
	}

	size_t host_chain_length(std::shared_ptr<host_link> head) {
		size_t result = 0;
		while (head) { ++result; head = std::move(head->next); }
		return result;
	}
}

class deep_nesting_tests : public suite {
public:
	deep_nesting_tests() : suite("Deep Nesting") {}

	void forge_tests() override {
		using namespace deep_detail;

		test("deep_arrays_and_maps_round_trip_through_every_archive", [this]() {
			on_small_stack([this]() {
				for (const std::string& text : {nested_arrays(k_depth), nested_maps(k_depth)}) {
					auto eng = make_engine();
					stdlib::register_all(*eng);
					eng->execution_budget(0);
					eng->add_global("text", eng->make_value(text));
					const std::string result = eng->execute(R"(
						var v = from_json(text);
						string out = to_string(to_json(v) == text);
						out += " " + to_string(to_json(from_binary(to_binary(v))) == text);
						out += " " + to_string(to_json(from_base64(to_base64(v))) == text);
						var copy = v;
						out += " " + to_string(to_json(copy) == text);
						v = null;
						copy = null;
						out;
					)").as<std::string>();
					check_eq(std::string("true true true true"), result, text.substr(0, 12));
				}
			});
		});

		test("deep_script_class_chains_round_trip_and_copy", [this]() {
			on_small_stack([this]() {
				auto eng = make_engine();
				stdlib::register_all(*eng);
				eng->execution_budget(0);
				eng->add_global("depth", eng->make_value(static_cast<script_int>(k_class_chain_depth)));
				const std::string result = eng->execute(R"(
					class Link { int n = 0; var next = null; }
					function length_of(var p) -> int { int count = 0; while (p != null) { count += 1; p = p.next; } return count; }
					var head = new Link();
					var at = head;
					for (int i = 1; i < depth; i++) { var link = new Link(); link.n = i; at.next = link; at = link; }
					at = null;
					string out = to_string(length_of(from_json(to_json(head))));
					out += " " + to_string(length_of(from_binary(to_binary(head))));
					string nested = "";
					for (int i = 0; i < depth; i++) { nested += "{\"$type\":\"Link\",\"$val\":{\"next\":"; }
					nested += "null";
					for (int i = 0; i < depth; i++) { nested += "}}"; }
					var values = from_json(nested);
					var copied = values;
					out += " " + to_string(to_json(copied) == to_json(values));   // a walk by value would copy the rest of the chain per step
					head = null;
					values = null;
					copied = null;
					out;
				)").as<std::string>();
				const std::string d = std::to_string(k_class_chain_depth);
				check_eq(d + " " + d + " true", result);
			});
		});

		test("deep_values_compare_and_order", [this]() {
			on_small_stack([this]() {
				struct nest { std::string same; std::string other; bool ordered; };
				const nest nests[] = {
					{nested_arrays(k_depth, "1"), nested_arrays(k_depth, "2"), true},
					{nested_maps(k_depth, "1"), nested_maps(k_depth, "2"), true},
					{nested_links(k_depth, 1), nested_links(k_depth, 2), false},
				};
				for (const nest& n : nests) {
					auto eng = make_engine();
					stdlib::register_all(*eng);
					eng->execution_budget(0);
					eng->add_global("same", eng->make_value(n.same));
					eng->add_global("other", eng->make_value(n.other));
					eng->add_global("ordered", eng->make_value(n.ordered));
					const std::string result = eng->execute(R"(
						class Link { int n = 0; var next = null; }
						var a = from_json(same);
						var b = from_json(same);
						var c = from_json(other);
						string out = to_string(a == b) + " " + to_string(a != c) + " " + to_string(a == c);
						if (ordered) { out += " " + to_string(a <=> b) + " " + to_string(a <=> c) + " " + to_string(a < c) + " " + to_string(c >= a); }
						a = null;
						b = null;
						c = null;
						out;
					)").as<std::string>();
					check_eq(std::string(n.ordered ? "true true false 0 -1 true true" : "true true false"), result, n.same.substr(0, 12));
				}
			});
		});

		test("deep_members_keep_their_operators", [this]() {
			// past the native depth each member pair still runs the full ==/<=>: a class's operators, and a throw that leaves
			auto eng = make_engine();
			stdlib::register_all(*eng);
			const std::string result = eng->execute(R"(
				class V { int a = 0; V() {} V(int x) { a = x; } bool operator==(V o) { return a == o.a; } int operator<=>(V o) { if (a < 0) { throw "bad"; } return a - o.a; } }
				function nest(var leaf, int n, bool maps) { var x = leaf; for (int i = 0; i < n; i++) { if (maps) { x = {"a": x, "b": i}; } else { x = [x, i]; } } return x; }
				string out = "";
				for (bool maps : [false, true]) {
					var a = nest(V(1), 100, maps); var b = nest(V(1), 100, maps); var c = nest(V(2), 100, maps);
					out += to_string(a == b) + " " + to_string(a == c) + " " + to_string(a <=> c) + " " + to_string(c > a);
					try { var r = nest(V(-1), 100, maps) < c; out += " ordered|"; } catch (e) { out += " " + e + "|"; }
				}
				out;
			)").as<std::string>();
			check_eq(std::string("true false -1 true bad|true false -1 true bad|"), result);
		});

		test("a_copy_runs_copy_hooks_in_order_at_any_depth", [this]() {
			// a copy visits a value as C++ copies it: members in declaration order, a member's own members before the next
			// member, a script class's C++ object after its fields, past the depth where the copy leaves the native stack too
			const int depth = 3 * static_cast<int>(detail::k_inline_copy_depth) / 2;
			struct shape { const char* build; bool own_object; };
			const shape shapes[] = {
				{"var cur = [hook(depth)]; for (int i = depth - 1; i >= 0; i--) { cur = [hook(i), cur, hook(1000 + i)]; }", false},
				{"var cur = {\"a\": hook(depth)}; for (int i = depth - 1; i >= 0; i--) { cur = {\"a\": hook(i), \"b\": cur, \"c\": hook(1000 + i)}; }", false},
				{"var cur = node(depth, null); for (int i = depth - 1; i >= 0; i--) { cur = node(i, cur); }", true},
			};
			for (const shape& s : shapes) {
				auto eng = make_engine();
				stdlib::register_all(*eng);
				dynamic_binder<copy_hook>(*eng, "Hook").property("id", &copy_hook::id).build();
				auto out = std::make_shared<std::ostringstream>();
				eng->set_output_stream(out);
				eng->add_global("depth", eng->make_value(static_cast<script_int>(depth)));
				eng->execute(R"(
					class Node : Hook { var before = null; var next = null; var after = null; }
					function hook(int id) { var h = Hook(); h.id = id; return h; }
					function node(int i, var next) { var n = Node(); n.id = 2000 + i; n.before = hook(i); n.next = next; n.after = hook(1000 + i); return n; }
				)");
				eng->execute(s.build);
				out->str("");
				eng->execute("var copy = cur;");
				check_eq(expected_copy_order(depth, s.own_object), out->str(), s.build);
			}
		});

		test("a_deep_host_record_chain_meets_the_record_guard_on_load", [this]() {
			// a host type's own load recurses natively once per record: an ordinary chain loads, and a deep one meets the
			// 1024-record backstop on a roomy stack and the stack margin on a tight one, whatever the build's frame size
			auto load = [](engine& eng, int depth, bool binary) {
				std::string error;
				size_t length = 0;
				try {
					length = binary ? host_chain_length(jai::from_binary<std::shared_ptr<host_link>>(eng, host_chain_binary(static_cast<size_t>(depth))))
					                : host_chain_length(jai::from_json<std::shared_ptr<host_link>>(eng, host_chain_json(depth)));
				} catch (const serialization_error& e) {
					error = e.what();
				}
				return error.empty() ? std::to_string(length) : error;
			};
			auto eng = make_engine();
			for (bool binary : {false, true}) {
				const std::string format = binary ? "binary" : "json";
				on_small_stack([&]() { check_eq(std::string("200"), load(*eng, 200, binary), format); }, k_roomy_stack);
				on_small_stack([&]() {
					check_eq(std::string("Maximum record nesting depth (1024) exceeded"), load(*eng, 1500, binary), format);
				}, k_roomy_stack);
#if defined(_WIN32)
				on_small_stack([&]() {
					check_eq(std::string("Record nesting depth exceeds the host thread's stack"), load(*eng, 1000, binary), format);
				}, k_tight_stack);
#endif
			}
		});

		test("a_deep_host_record_chain_meets_the_record_guard_on_save", [this]() {
			auto chain = [](int depth) {
				std::shared_ptr<host_link> head;
				for (int i = depth; i-- > 0;) {
					auto link = std::make_shared<host_link>();
					link->n = i;
					link->next = std::move(head);
					head = std::move(link);
				}
				return head;
			};
			auto save = [](engine& eng, const std::shared_ptr<host_link>& head, bool binary) {
				try {
					if (binary) { jai::to_binary(eng, head); } else { jai::to_json(eng, head); }
				} catch (const serialization_error& e) {
					return std::string(e.what());
				}
				return std::string("saved");
			};
			auto eng = make_engine();
			for (bool binary : {false, true}) {
				const std::string format = binary ? "binary" : "json";
				auto ordinary = chain(200);
				on_small_stack([&]() { check_eq(std::string("saved"), save(*eng, ordinary, binary), format); }, k_roomy_stack);
				host_chain_length(std::move(ordinary));
				auto deep = chain(1500);
				on_small_stack([&]() {
					check_eq(std::string("Maximum record nesting depth (1024) exceeded"), save(*eng, deep, binary), format);
				}, k_roomy_stack);
#if defined(_WIN32)
				on_small_stack([&]() {
					check_eq(std::string("Record nesting depth exceeds the host thread's stack"), save(*eng, deep, binary), format);
				}, k_tight_stack);
#endif
				host_chain_length(std::move(deep));
			}
		});

		// Source nests as deep as the walks over it (the parser, the checker, the compiler, the evaluation) can go on the host
		// thread's stack: each shape loads and runs, or fails with the one nesting error, at every depth, never 0xC00000FD
		test("source_nesting_meets_the_nesting_error_on_a_host_stack", [this]() {
			using shape = std::string (*)(int);
			static const std::pair<const char*, shape> k_shapes[] = {
				{"arrays", [](int n) { return "var x = " + std::string(n, '[') + std::string(n, ']') + "; 1;"; }},
				{"maps", [](int n) { std::string r = "var x = "; for (int i = 0; i < n; ++i) { r += "{\"a\":"; } return r + "1" + std::string(n, '}') + "; 1;"; }},
				{"lambdas", [](int n) { std::string r = "var f = "; for (int i = 0; i < n; ++i) { r += "[]() { return "; } r += "1"; for (int i = 0; i < n; ++i) { r += "; }"; } return r + "; 1;"; }},
				{"casts", [](int n) { std::string r = "var a = "; for (int i = 0; i < n; ++i) { r += "cast<int>("; } return r + "1" + std::string(n, ')') + "; 1;"; }},
				{"calls", [](int n) { std::string r = "int f(int a) { return a; } "; for (int i = 0; i < n; ++i) { r += "f("; } return r + "1" + std::string(n, ')') + "; 1;"; }},
				{"subscripts", [](int n) { std::string r = "var a = [0]; a["; for (int i = 0; i < n; ++i) { r += "a["; } return r + "0" + std::string(n, ']') + "]; 1;"; }},
				{"types", [](int n) { std::string r; for (int i = 0; i < n; ++i) { r += "array<"; } return r + "int" + std::string(n, '>') + " x; 1;"; }},
				{"parens", [](int n) { return "var x = " + std::string(n, '(') + "1" + std::string(n, ')') + "; 1;"; }},
				{"assignments", [](int n) { std::string r = "var a = 0; "; for (int i = 0; i < n; ++i) { r += "a = "; } return r + "1; 1;"; }},
				{"ternaries", [](int n) { std::string r = "var x = "; for (int i = 0; i < n; ++i) { r += "true ? 1 : "; } return r + "0; 1;"; }},
				{"and_chain", [](int n) { std::string r = "bool t = true; var x = t"; for (int i = 0; i < n; ++i) { r += " && t"; } return r + "; 1;"; }},
				{"plus_chain", [](int n) { std::string r = "int g(int y) { return y"; for (int i = 0; i < n; ++i) { r += " + y"; } return r + "; } g(1); 1;"; }},
				{"ifs", [](int n) { std::string r; for (int i = 0; i < n; ++i) { r += "if (true) "; } return r + "{} 1;"; }},
				{"else_ifs", [](int n) { std::string r = "int x = 5; if (x == -1) {}"; for (int i = 0; i < n; ++i) { r += " else if (x == " + std::to_string(i) + ") {}"; } return r + " 1;"; }},
				{"blocks", [](int n) { return "void g() " + std::string(n, '{') + std::string(n, '}') + " g(); 1;"; }},
				{"namespaces", [](int n) { std::string r; for (int i = 0; i < n; ++i) { r += "namespace a { "; } return r + "int x = 1;" + std::string(n, '}') + " 1;"; }},
			};
			for (const auto& [name, make] : k_shapes) {
				for (int depth : {8, 30, 60, 100, 250, 1000, 20000}) {
					const std::string source = make(depth);
					std::string outcome;
					on_small_stack([&]() {
						auto eng = make_engine();
						try {
							outcome = std::to_string(eng->execute(source).as_int());
						} catch (const std::exception& e) {
							outcome = e.what();
						}
					});
					const std::string at = std::string(name) + " at " + std::to_string(depth) + ": " + outcome.substr(0, 300);
					const bool nesting = outcome.find("esting too deep for the host thread's stack") != std::string::npos;
					check_true(outcome == "1" || nesting, at);
					if (depth == 8) { check_eq(std::string("1"), outcome, at); }
					if (depth == 20000) { check_true(nesting, at); }
				}
			}
		});
	}
};

}

using deep_nesting_tests = jai::foundry::tests::deep_nesting_tests;
FOUNDRY_REGISTER(deep_nesting_tests)
