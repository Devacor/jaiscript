#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

// Every append spelling builds in place (amortized O(1) per append) and stays a value: a copy taken before an append keeps its text,
// a reference sees it, and each spelling reads, charges, raises and stores exactly as the expression it stands for.
namespace jai::foundry::tests {

class string_builder_tests : public suite {
public:
	string_builder_tests() : suite("String Builder") {}

	// the script's result on both backends, each against the expected text
	void both(const std::string& src, const std::string& expected, const std::string& what) {
		for (bool use_vm : {false, true}) {
			auto e = jai::foundry::make_default_engine();
			e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
			jai::stdlib::register_all(e);
			std::string got;
			try { got = e->execute(src).to_string(); } catch (const std::exception& ex) { got = std::string("threw: ") + ex.what(); }
			check_eq(expected, got, std::string(use_vm ? "vm: " : "interp: ") + what);
		}
	}

	void forge_tests() override {
		test("a_copy_taken_before_an_append_keeps_its_text", [this]() {
			both(R"(
				class Holder { string s = "x"; var v = "x"; utf8_string u = "é"; static string st = "x"; }
				string g = "x";
				string out = "";
				string a = "x"; string ka = a; a += "y"; out += ka + a + "|";
				string b = "x"; string kb = b; b = b + "y" + "z"; out += kb + b + "|";
				string c = "x"; string kc = c; c += 'y'; out += kc + c + "|";
				string d = "x"; string kd = d; d = d + "y"; out += kd + d + "|";
				Holder h; string kh = h.s; h.s += "y"; out += kh + h.s + "|";
				var kv = h.v; h.v += "y"; out += kv + h.v + "|";
				utf8_string ku = h.u; h.u += "y"; out += ku + h.u + "|";
				string kst = Holder::st; Holder::st += "y"; out += kst + Holder::st + "|";
				array<string> arr = ["x"]; string ke = arr[0]; arr[0] += "y"; out += ke + arr[0] + "|";
				array<string> arr2 = arr; arr2[0] += "z"; out += arr[0] + arr2[0] + "|";
				array<var> va = ["x"]; var kva = va[0]; va[0] += "y"; out += kva + va[0] + "|";
				map<string, string> m = {"k": "x"}; string km = m["k"]; m["k"] += "y"; out += km + m["k"] + "|";
				string kg = g; g = g + "y" + "z"; out += kg + g + "|";
				out
			)", "xxy|xxyz|xxy|xxy|xxy|xxy|ééy|xxy|xxy|xyxyz|xxy|xxy|xxyz|", "every spelling copies on write, never through a shared node");
		});

		test("functions_build_in_place_through_every_target", [this]() {
			both(R"(
				class Holder { string s; static string st = ""; }
				class Builder {
					string html;
					void bare(string x) { html += x; }
					void self(string x) { this.html += x; }
					void plus(string x) { html = html + x; }
					void chain(string x) { html = html + "<" + x + ">"; }
				}
				void app(string& out, string x) { out += x; }
				void app_plus(string& out, string x) { out = out + x; }
				string g = "";
				string run() {
					string s = ""; var v = ""; utf8_string u = ""; Holder h; Builder b;
					array<string> a = ["", ""]; array<var> av = [""]; map<string, string> m = {"k": "", "0": "", "1": ""};
					for (int i = 0; i < 50; ++i) {
						s += "a"; s += 'b'; s = s + "c"; s = s + to_string(i % 10) + "-" + to_string(i % 3);
						v += "a"; v = v + "b" + to_string(1);
						u += "é"; u += 'x';
						h.s += "a"; h.s += 'b'; Holder::st += "c";
						b.bare("a"); b.self("b"); b.plus("c"); b.chain("d");
						a[i & 1] += "a"; av[0] += "b"; m["k"] += "c"; m[to_string(i % 2)] += 'd';
						app(s, "!"); app_plus(s, "?"); app(h.s, "!"); app(a[0], "!");
						g = g + "a" + to_string(i % 10);
					}
					return to_string(s.size()) + " " + to_string(v.size()) + " " + to_string(u.size()) + "/" + to_string(u.byte_size()) + " " +
					       to_string(h.s.size()) + " " + to_string(Holder::st.size()) + " " + to_string(b.html.size()) + " " +
					       to_string(a[0].size()) + "," + to_string(a[1].size()) + " " + to_string(av[0].size()) + " " +
					       to_string(m["k"].size()) + "," + to_string(m["0"].size()) + " " + to_string(g.size()) + " " +
					       s.substr(0, 12) + " " + b.html.substr(0, 9) + " " + g.substr(0, 6);
				}
				run()
			)", "400 150 100/150 150 50 300 75,25 50 50,25 100 abc0-0!?abc1 abc<d>abc a0a1a2", "every target appends");
		});

		test("references_see_the_appends_of_their_storage", [this]() {
			both(R"(
				string s = "a";
				string& r = s;
				r += "b";
				r = r + "c" + "d";
				r += 'e';
				string t = s;
				r += "f";
				s + "|" + t + "|" + r
			)", "abcdef|abcde|abcdef", "a reference shares the storage, a copy does not");
		});

		test("self_appends_read_the_text_before_the_append", [this]() {
			both(R"(
				string a = "ab"; a += a;
				string b = "ab"; b = b + b;
				string c = "ab"; c = c + c + c;
				string d = "ab"; d = d + "-" + d;
				class H { string s = "xy"; }
				H h; h.s += h.s;
				array<string> e = ["q"]; e[0] += e[0];
				a + " " + b + " " + c + " " + d + " " + h.s + " " + e[0]
			)", "abab abab ababab ab-ab xyxy qq", "the operand is the value before the append");
		});

		test("a_chain_reads_its_target_first_and_stores_last", [this]() {
			both(R"(
				string g = "g";
				string clobber() { g = "zz"; return "1"; }
				g = g + "a" + clobber() + "b";
				string s = "xy";
				s = s + "a" + to_string(s.size()) + s;
				string order = "";
				string note(string x) { order += x; return x; }
				string t = "t";
				t = t + note("1") + note("2") + note("3");
				g + " " + s + " " + t + " " + order
			)", "ga1b xya2xy t123 123", "an operand that writes or reads the target sees the value before the store");
		});

		test("a_chain_that_raises_leaves_its_target", [this]() {
			both(R"(
				string boom() { throw "boom"; return ""; }
				string s = "keep";
				string caught = "";
				try { s = s + "a" + boom() + "b"; } catch (e) { caught = e; }
				string local_case() {
					string l = "keep";
					try { [[warn_mode]] l = l + "a" + boom(); } catch (e) { return l; }
					return "no";
				}
				s + " " + caught + " " + local_case()
			)", "keep boom keep", "a raise mid-chain stores nothing");
		});

		test("a_chain_joins_objects_and_containers_and_refuses_a_scalar_where_the_concatenation_does", [this]() {
			both(R"(
				class P { string to_string() { return "P"; } }
				P p;
				string s = "x";
				s = s + p + "|" + to_string(1) + to_string(2.5) + to_string(null);
				var n = 1;
				n = n + 2 + 3;
				var m = 1;
				string err = "";
				try { [[warn_mode]] m = m + "a" + 2; } catch (e) { err = e; }
				string q = "q";
				q = q + [1, 2] + {"a": 1};
				s + " " + to_string(n) + " " + to_string(m) + " " + q + " " + err
			)", "xP|12.5null 6 1 q[1, 2]{\"a\": 1} cannot apply '+' to 'int' and 'string': stringify it with to_string(x) or `${x}`",
			   "objects and containers join their texts; a number beside a string raises at its step and stores nothing");
		});

		test("an_object_operand_runs_its_to_string_where_the_chain_does", [this]() {
			both(R"(
				string log = "";
				class P { string tag; string to_string() { log += tag; return tag; } }
				string mark(string x) { log += x; return x; }
				P a; a.tag = "A";
				P b; b.tag = "B";
				string s = "s";
				s = s + mark("1") + a + mark("2") + b + mark("3");
				s + " " + log
			)", "s1A2B3 1A2B3", "user code in the chain keeps its order");
		});

		test("a_chain_reads_each_operand_at_its_step", [this]() {
			both(R"(
				string b = "b0";
				class P { string to_string() { b = "b1"; return "P"; } }
				P p;
				string a = "a";
				int n = 7;
				string t = a + "-" + p + b + to_string(n) + to_string('c') + to_string(2.5) + to_string(true);
				string u = `${a}|${p}|${b}|${n}`;
				string w = "x" + a + [1] + b;
				t + " " + u + " " + w
			)", "a-Pb17c2.5true a|P|b1|7 xa[1]b1", "an operand after an object's to_string reads the value it left");
		});

		test("lambdas_capture_a_builder_by_reference_or_by_value", [this]() {
			both(R"(
				string s = "a";
				auto by_ref = [&]() { s += "r"; s = s + "R"; };
				auto by_val = [s]() { return s; };
				by_ref(); by_ref();
				s += "!";
				by_val() + " " + s
			)", "a arRrR!", "a [&] capture appends to the variable, a [s] capture keeps its copy");
		});

		test("serialization_reads_a_string_mid_build", [this]() {
			both(R"(
				class Doc { string body; }
				Doc d;
				string snaps = "";
				for (int i = 0; i < 3; ++i) {
					d.body += "x";
					d.body = d.body + to_string(i);
					Doc back = from_json(to_json(d));
					snaps += back.body + ";";
				}
				snaps + " " + to_json(d)
			)", "x0;x0x1;x0x1x2; {\"$type\":\"Doc\",\"$val\":{\"body\":\"x0x1x2\"}}", "a saved builder holds its text so far");
		});

		test("map_keys_stay_put_when_their_source_grows", [this]() {
			both(R"(
				map<string, int> m = {};
				string k = "a";
				m[k] = 1;
				k += "b";
				k = k + "c";
				m[k] = 2;
				string keys = "";
				for (auto kv : m) { keys += kv.first + "=" + to_string(kv.second) + ","; }
				keys
			)", "a=1,abc=2,", "a key is its own copy");
		});

		test("map_entries_append_through_their_bindings", [this]() {
			both(R"(
				map<string, string> m = {"a": "", "b": "x"};
				string keep = m["b"];
				for (int i = 0; i < 3; ++i) {
					for (auto& [k, v] : m) { v += k; v = v + "-" + to_string(i); }
					for (auto& kv : m) { kv.second += "."; }
				}
				var vm = {"q": ""};
				for (auto& [k, v] : vm) { v += "1"; v = v + "2"; }
				m["a"] + " " + m["b"] + " " + keep + " " + vm["q"]
			)", "a-0.a-1.a-2. xb-0.b-1.b-2. x 12", "a reference to an entry writes the entry, a copy keeps its text");
		});

		test("parallel_bodies_append_to_their_own_elements", [this]() {
			both(R"(
				array<string> words = ["a", "b", "c", "d"];
				array<string> keep = words;
				parallel_for (auto& w : words) { w += "!"; w = w + "?"; }
				string out = "";
				for (auto w : words) { out += w; }
				for (auto w : keep) { out += w; }
				out
			)", "a!?b!?c!?d!?abcd", "an element appends in place; the copy keeps its text");
		});

		test("hot_reload_keeps_a_live_builder", [this]() {
			for (bool use_vm : {false, true}) {
				auto e = jai::foundry::make_default_engine();
				e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
				jai::stdlib::register_all(e);
				e->execute("class Page { string html; void add(string x) { html += x; } }\nPage p;\nstring g = \"\";\nvoid emit(string x) { g = g + x; }\nfor (int i = 0; i < 3; ++i) { p.add(\"a\"); emit(\"a\"); }");
				e->execute("class Page { string html; string title = \"t\"; void add(string x) { html = html + \"<\" + x + \">\"; } }\nvoid emit(string x) { g = g + \"[\" + x + \"]\"; }");
				auto r = e->execute("for (int i = 0; i < 2; ++i) { p.add(\"b\"); emit(\"b\"); }\np.html + \" \" + p.title + \" \" + g");
				check_eq(std::string("aaa<b><b> t aaa[b][b]"), r.to_string(), std::string(use_vm ? "vm" : "interp") + ": a reload keeps the text built so far");
			}
		});

		test("the_memory_cap_trips_where_the_concatenation_charges", [this]() {
			const std::string spellings[] = {
				"s += \"xxxxxxxx\";", "s += 'x';", "s = s + \"xxxxxxxx\";", "s = s + \"x\" + to_string(7) + \"y\";", "h.s += \"xxxxxxxx\";",
				"h.s += 'x';", "a[0] += \"xxxxxxxx\";", "m[\"k\"] += \"xxxxxxxx\";", "g = g + \"xx\" + \"yy\";", "Holder::st += \"xxxxxxxx\";",
			};
			for (const auto& step : spellings) {
				std::string texts[2];
				for (int b = 0; b < 2; ++b) {
					auto e = jai::foundry::make_default_engine();
					e->set_backend(b ? jai::backend_type::vm : jai::backend_type::interpreter);
					jai::stdlib::register_all(e);
					e->memory_cap(256 * 1024);
					auto r = e->execute("class Holder { string s; static string st = \"\"; }\nstring g = \"\";\nstring last = \"\";\n"
					                    "int run() { string s = \"\"; Holder h; array<string> a = [\"\"]; map<string, string> m = {\"k\": \"\"}; int n = 0;\n"
					                    "try { while (true) { " + step + " ++n; } } catch (err) { last = err; }\nreturn n; }\nint n = run();\nto_string(n) + \"|\" + last");
					texts[b] = r.to_string();
				}
				check_eq(texts[0], texts[1], "both backends trip at the same step: " + step);
				check_true(texts[0].find("Script memory cap exceeded") != std::string::npos, "the cap's text: " + step + " -> " + texts[0]);
			}
		});

		test("a_long_build_is_linear", [this]() {
			// 200k appends of every in-place spelling on the vm: a copy per append would move tens of GB and run for minutes (the
			// interpreter, the oracle, copies the `s = s + x` spellings)
			auto e = jai::foundry::make_default_engine();
			e->set_backend(jai::backend_type::vm);
			jai::stdlib::register_all(e);
			auto r = e->execute(R"(
				class Holder { string s; }
				int run() {
					string s = ""; Holder h; array<string> a = [""]; map<string, string> m = {"k": ""};
					for (int i = 0; i < 200000; ++i) { s += "x"; s = s + "y"; s = s + "z" + to_string(i % 10); h.s += 'q'; a[0] += "w"; m["k"] += "v"; }
					return s.size() + h.s.size() + a[0].size() + m["k"].size();
				}
				run()
			)");
			check_eq(static_cast<script_int>(1400000), r.as<script_int>(), "every spelling stays linear");
		});
	}
};

}

using string_builder_tests = jai::foundry::tests::string_builder_tests;
FOUNDRY_REGISTER(string_builder_tests)
