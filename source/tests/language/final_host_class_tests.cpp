// A host class bound final (dynamic_binder::final, docs/site/embedding/17-host-api.html#final-host): no script class derives from
// it, at load and when the class declaration runs, on every backend

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/testing/temporary_directory.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace jai::foundry::tests {

namespace {
struct sealed_probe { int64_t value = 7; };
struct open_probe { int64_t value = 8; };

// the library, fh::Sealed bound final, and OpenProbe bound without it
void bind_probes(engine& a_engine) {
	stdlib::register_all(a_engine);
	dynamic_binder<sealed_probe>(a_engine, "fh::Sealed").final().constructor<>().property("value", &sealed_probe::value).build();
	dynamic_binder<open_probe>(a_engine, "OpenProbe").constructor<>().property("value", &open_probe::value).build();
}
}

class final_host_class_tests : public suite {
public:
	final_host_class_tests() : suite("Final Host Classes") {}

	// the text an execute throws, "(no error)" when it runs
	static std::string failure(engine& a_engine, const std::string& a_code) {
		try {
			a_engine.execute(a_code);
		} catch (const std::exception& error) {
			return error.what();
		}
		return "(no error)";
	}
	static bool contains(const std::string& a_text, const std::string& a_part) { return a_text.find(a_part) != std::string::npos; }
	static size_t error_lines(const std::string& a_text) {
		size_t count = 0;
		for (size_t at = a_text.find(": error: "); at != std::string::npos; at = a_text.find(": error: ", at + 1)) { ++count; }
		return count;
	}

	void forge_tests() override {
		// one error per derivation, at the class declaration, in words that say what to write instead
		test("a_script_class_cannot_derive_from_a_final_host_class", [this]() {
			const std::pair<const char*, const char*> refused[] = {
				{"class S : fh::Sealed { }", "Class 'S' cannot derive from 'fh::Sealed': 'fh::Sealed' is final; give 'S' a field of type 'fh::Sealed' instead"},
				{"class S : fh::Sealed { int extra = 1; S() : super() { } }", "Class 'S' cannot derive from 'fh::Sealed': 'fh::Sealed' is final"},
				{"class Mine { }\nclass S : Mine, fh::Sealed { }", "Class 'S' cannot derive from 'fh::Sealed': 'fh::Sealed' is final"},
				{"namespace art { class S : fh::Sealed { } }", "Class 'art::S' cannot derive from 'fh::Sealed': 'fh::Sealed' is final"},
				{"class G<T> : fh::Sealed { T held; }\nG<int> g;", "Class 'G<int>' cannot derive from 'fh::Sealed': 'fh::Sealed' is final"},
			};
			for (const auto& [code, text] : refused) {
				auto e = make_engine();
				bind_probes(*e);
				const std::string got = failure(*e, code);
				check_true(contains(got, text), std::string(code) + " -> " + got);
				check_eq((size_t)1, error_lines(got), std::string(code) + ": one error -> " + got);
			}
			// what is not final still derives: a bound class without final(), and a script class over a script class
			auto e = make_engine();
			bind_probes(*e);
			check_eq(std::string("8|3"), e->execute("class P : OpenProbe { }\nclass A { int a = 1; }\nclass B : A { int b = 2; }\nclass C : B { }\n"
			                                        "P p;\nC c;\nto_string(p.value) + \"|\" + to_string(c.a + c.b);").to_string());
			check_eq(std::string("7"), e->execute("class Holder { fh::Sealed held; }\nHolder h;\nh.held.value;").to_string(), "a field of the final class");
		});

		// the declaration's own run refuses it in the checker's words, on every backend
		test("the_run_time_refuses_a_derivation_the_load_did_not_check", [this]() {
			auto e = make_engine();
			bind_probes(*e);
			check_eq(std::string("Class 'S' cannot derive from 'fh::Sealed': 'fh::Sealed' is final; give 'S' a field of type 'fh::Sealed' instead"),
			         failure(*e, "[[warn_mode]];\nclass S : fh::Sealed { }"));
			check_eq(std::string("2"), e->execute("1 + 1").to_string(), "the engine runs on");
		});

		// final is part of the registration surface: a bite checked clean against a class that was not final is checked again
		test("finality_moves_the_registration_fingerprint", [this]() {
			auto plain = make_engine();
			dynamic_binder<sealed_probe>(*plain, "fh::Sealed").constructor<>().build();
			auto sealed = make_engine();
			dynamic_binder<sealed_probe>(*sealed, "fh::Sealed").final().constructor<>().build();
			check_ne(plain->registration_fingerprint(), sealed->registration_fingerprint());
			auto again = make_engine();
			dynamic_binder<sealed_probe>(*again, "fh::Sealed").constructor<>().build();
			check_eq(plain->registration_fingerprint(), again->registration_fingerprint());
		});

		// a reload whose new text derives from a final host class is refused whole, as any definition error is
		test("a_reload_that_derives_from_a_final_host_class_is_refused", [this]() {
			const auto dir = jai::foundry::test_temp_directory();
			int tick = 2600;
			auto write_module = [&](const std::string& a_name, const std::string& a_body) {
				const auto path = dir / a_name;
				{ std::ofstream(path, std::ios::binary | std::ios::trunc) << a_body; }
				std::error_code ec;
				std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * ++tick), ec);
			};
			auto e = make_engine();
			bind_probes(*e);
			e->add_include_path(dir.string());
			e->import_recheck_interval(0);
			write_module("fh_mod.jai", "class Frame { fh::Sealed held; int v = 1; }\nint fh_v() { Frame f; return f.v; }\n");
			// the path is a variable, so the reload is decided by its own check, not by the load of the unit that imports it
			check_eq(std::string("1"), e->execute("string FH_MOD = \"fh_mod.jai\";\nimport(FH_MOD);\nfh_v();").to_string());
			write_module("fh_mod.jai", "class Frame : fh::Sealed { int v = 2; }\nint fh_v() { Frame f; return f.v; }\n");
			const std::string failed = e->execute("string m = \"\";\ntry { import(FH_MOD); } catch (err) { m = err; }\nm;").to_string();
			check_true(failed.rfind("static check failed:\n", 0) == 0 && contains(failed, "fh_mod.jai") &&
			           contains(failed, "Class 'Frame' cannot derive from 'fh::Sealed': 'fh::Sealed' is final"), failed);
			check_eq((size_t)1, error_lines(failed), "one error: " + failed);
			check_eq(std::string("1"), e->execute("fh_v();").to_string(), "nothing applied");
			check_eq(std::string("false"), e->execute("to_string(import(FH_MOD));").to_string(), "the edit waits");
			write_module("fh_mod.jai", "class Frame { fh::Sealed held; int v = 3; }\nint fh_v() { Frame f; return f.v; }\n");
			check_eq(std::string("3"), e->execute("import(FH_MOD);\nfh_v();").to_string(), "the fixed edit applies");
			for (const char* f : {"fh_mod.jai", "fh_mod.jaibite"}) {
				std::error_code ec;
				std::filesystem::remove(dir / f, ec);
			}
		});
	}
};

}

using final_host_class_tests = jai::foundry::tests::final_host_class_tests;
FOUNDRY_REGISTER(final_host_class_tests)
