#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/serialization/polymorphic.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace multi_engine_detail {
	struct edge_base { virtual ~edge_base() = default; };
	template<int N> struct edge_derived : edge_base { int n = N; };
	constexpr int k_edge_types = 64;

	template<int... Ns>
	void bind_edge_types(engine& a_eng, std::integer_sequence<int, Ns...>) {
		(dynamic_binder<edge_derived<Ns>>(a_eng, "MtEdge" + std::to_string(Ns)).template constructor<>().template base_class<edge_base>().build(), ...);
	}

	template<int... Ns>
	int count_edges(std::integer_sequence<int, Ns...>) {
		auto& reg = jai::serialization::polymorphic_registry::instance();
		return (0 + ... + (reg.is_assignable(std::type_index(typeid(edge_derived<Ns>)), std::type_index(typeid(edge_base))) ? 1 : 0));
	}
}

class multi_engine_thread_tests : public suite {
public:
	multi_engine_thread_tests() : suite("Multi Engine Threads") {}

	void forge_tests() override {
		auto fresh_directory = [](const std::string& a_name) {
			std::filesystem::path result = jai::foundry::test_temp_directory() / a_name;
			std::error_code ec;
			std::filesystem::remove_all(result, ec);
			std::filesystem::create_directories(result);
			return result;
		};
		auto put_file = [](const std::filesystem::path& a_path, const std::string& a_content) {
			std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
			file << a_content;
		};

		test("engines_on_threads_share_nothing_but_the_files", [=, this]() {
			// the engines write and read the modules' jaibite siblings concurrently
			const auto dir = fresh_directory("mt_share");
			put_file(dir / "mt_lib.jai", R"(
				class Vec {
					int x = 0;
					int y = 0;
					Vec(int a, int b) { x = a; y = b; }
					int dot(Vec o) { return x * o.x + y * o.y; }
				}
				int lib_sum(int n) { int s = 0; for (int i = 0; i < n; ++i) { s += i; } return s; }
				string lib_tag(int k) { return "tag" + to_string(k); }
			)");
			put_file(dir / "mt_more.jai", R"(
				import "mt_lib.jai";
				coroutine int mt_counter(int n) { for (int i = 0; i < n; ++i) { yield i * 2; } return -1; }
				int more_value() { return lib_sum(10) + Vec(1, 2).dot(Vec(3, 4)); }
			)");
			const std::string program = R"(
				import "mt_more.jai";
				var acc = 0;
				var make_adder = function(int k) { return [k](int v) { return v + k; }; };
				var add5 = make_adder(5);
				for (int i = 0; i < 200; ++i) { acc = add5(acc) % 1000003; }
				acc += Vec(3, 4).dot(Vec(5, 6)) + more_value();
				var m = {"k": [1, 2, 3], "s": "hello", "n": {"deep": [4.5, "x"]}};
				var back = from_json(to_json(m));
				acc += back["k"][2] + back["s"].size() + back["n"]["deep"][1].size();
				string s = "";
				for (int i = 0; i < 100; ++i) { s += lib_tag(i % 7); }
				acc += s.size() + s.find("tag6");
				auto c = mt_counter(5);
				for (int i = 0; i < 5; ++i) { acc += c.next(); }
				array<int> xs = [];
				for (int i = 0; i < 300; i++) { xs.push(i); }
				parallel_for (auto& x : xs) { x = x * 3 + 1; }
				for (int i = 0; i < 300; i++) { acc += xs[i] % 17; }
				acc;
			)";
			auto run_one = [&](std::string& a_error) -> script_int {
				try {
					auto eng = make_engine();
					jai::stdlib::register_all(eng);
					eng->parallel_thread_count(2);   // eight engines with a pool per core each oversubscribed the machine many times over
					eng->add_include_path(dir.string());
					script_int result = 0;
					for (int round = 0; round < 4; ++round) {
						result += eng->execute(program).as_int();
					}
					return result;
				} catch (const std::exception& e) {
					a_error = e.what();
				} catch (...) {
					a_error = "unknown exception";
				}
				return -1;
			};

			std::string expected_error;
			const script_int expected = run_one(expected_error);
			check_eq(std::string(), expected_error);

			constexpr int k_threads = 8;
			for (int wave = 0; wave < 3; ++wave) {
				std::error_code ec;
				std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / "mt_lib.jai"), ec);
				std::filesystem::remove(jai::foundry::jaibite_cache_beside(dir / "mt_more.jai"), ec);
				std::vector<script_int> results(k_threads, 0);
				std::vector<std::string> errors(k_threads);
				std::atomic<int> ready{0};
				std::vector<std::thread> threads;
				for (int t = 0; t < k_threads; ++t) {
					threads.emplace_back([&, t]() {
						++ready;
						while (ready.load() < k_threads) { std::this_thread::yield(); }   // a bare spin starves the threads still starting on a loaded machine
						results[t] = run_one(errors[t]);
					});
				}
				for (auto& thread : threads) { thread.join(); }
				for (int t = 0; t < k_threads; ++t) {
					check_eq(std::string(), errors[t]);
					check_eq(expected, results[t]);
				}
			}
		});

		test("a_jaibite_sibling_written_by_two_engines_at_once_is_never_read_torn", [=, this]() {
			// writers of two different-sized programs keep replacing one sibling; a reader adopts a sibling whole or parses
			const auto dir = fresh_directory("mt_torn");
			const std::string path = (dir / "torn.jai").string();
			auto source_for = [](int a_which) {
				std::string result = "int torn_value() { return " + std::to_string(a_which == 0 ? 111 : 222) + "; }\n";
				const int functions = a_which == 0 ? 40 : 400;
				for (int i = 0; i < functions; ++i) {
					result += "int filler_" + std::to_string(i) + "(int a) { return a * " + std::to_string(i) + " + " +
					          std::to_string(a_which) + "; }\n";
				}
				result += "torn_value() + filler_3(1) - 3 - " + std::to_string(a_which) + ";\n";
				return result;
			};
			const std::string sources[2] = { source_for(0), source_for(1) };
			const script_int values[2] = { 111, 222 };
			std::atomic<bool> stop{false};
			std::atomic<int> wrong{0};
			std::atomic<int> failures{0};
			std::atomic<int> reads{0};
			auto worker = [&](int a_which, bool a_writer) {
				try {
					while (!stop.load()) {
						auto eng = make_engine();
						const script_int got = eng->execute_file_source(path, sources[a_which]).as_int();
						if (got != values[a_which]) { ++wrong; }
						if (!a_writer) { ++reads; }
					}
				} catch (...) {
					++failures;
				}
			};
			std::vector<std::thread> threads;
			for (int t = 0; t < 6; ++t) { threads.emplace_back(worker, t % 2, t < 2); }
			while (reads.load() < 400 && failures.load() == 0) {
				std::this_thread::yield();
			}
			stop = true;
			for (auto& thread : threads) { thread.join(); }
			check_eq(0, failures.load());
			check_eq(0, wrong.load());
			check_true(reads.load() > 0);
		});

		test("engines_binding_base_classes_on_threads_record_every_relation", [this]() {
			// base_class<>() records the process-wide serialization relation from whichever thread builds the engine
			using namespace multi_engine_detail;
			constexpr int k_threads = 8;
			std::vector<std::string> errors(k_threads);
			std::atomic<int> ready{0};
			std::vector<std::thread> threads;
			for (int t = 0; t < k_threads; ++t) {
				threads.emplace_back([&, t]() {
					try {
						auto eng = make_engine();
						++ready;
						while (ready.load() < k_threads) { std::this_thread::yield(); }
						if (t % 2 == 0) {
							bind_edge_types(*eng, std::make_integer_sequence<int, k_edge_types>{});
						} else {
							for (int i = 0; i < 64; ++i) { (void)count_edges(std::make_integer_sequence<int, k_edge_types>{}); }
							bind_edge_types(*eng, std::make_integer_sequence<int, k_edge_types>{});
						}
					} catch (const std::exception& e) {
						errors[t] = e.what();
					}
				});
			}
			for (auto& thread : threads) { thread.join(); }
			for (const auto& error : errors) { check_eq(std::string(), error); }
			check_eq(k_edge_types, count_edges(std::make_integer_sequence<int, k_edge_types>{}));
		});
	}
};

}

using multi_engine_thread_tests = jai::foundry::tests::multi_engine_thread_tests;
FOUNDRY_REGISTER(multi_engine_thread_tests)
