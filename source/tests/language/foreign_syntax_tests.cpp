// C++ spellings JaiScript does not have: each reports one error, and that error names the replacement
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/foreign_keyword.hpp>
#include <string>
#include <string_view>

namespace jai::foundry::tests {

class foreign_syntax_tests : public suite {
public:
	foreign_syntax_tests() : suite("Foreign Syntax") {}

	static size_t occurrences(std::string_view text, std::string_view needle) {
		size_t count = 0;
		for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) { ++count; }
		return count;
	}

	void forge_tests() override {
		test("each_spelling_reports_one_error_with_its_replacement", [this]() {
			using detail::foreign_word;
			using detail::foreign_word_text;
			struct row { const char* code; std::string_view text; };
			const row rows[] = {
				{"struct P { int x; int y; };", foreign_word_text(foreign_word::struct_declaration)},
				{"struct P : Q { void f() { return; } };", foreign_word_text(foreign_word::struct_declaration)},
				{"int* p = null;", foreign_word_text(foreign_word::pointer_type)},
				{"class Foo {} Foo* p;", foreign_word_text(foreign_word::pointer_type)},
				{"class Foo {} Foo* p = null;", foreign_word_text(foreign_word::pointer_type)},
				{"class Foo {} Foo* make() { return null; }", foreign_word_text(foreign_word::pointer_type)},
				{"class Foo {} void f(Foo* p) { int a = 1; }", foreign_word_text(foreign_word::pointer_type)},
				{"class Foo { Foo* next; int v; }", foreign_word_text(foreign_word::pointer_type)},
				{"#define N 4\nint a = 1;", detail::foreign_define_text},
				{"#define TWICE(x) \\\n  (x + x)\nint a = 1;", detail::foreign_define_text},
				{"typedef int myint;", foreign_word_text(foreign_word::typedef_declaration)},
				{"using X = int;", foreign_word_text(foreign_word::using_alias)},
				{"unsigned int a = 1;", foreign_word_text(foreign_word::unsigned_type)},
				{"unsigned a = 1;", foreign_word_text(foreign_word::unsigned_type)},
				{"void f(unsigned a) { }", foreign_word_text(foreign_word::unsigned_type)},
				{"long a = 1;", foreign_word_text(foreign_word::long_type)},
				{"long long a = 1;", foreign_word_text(foreign_word::long_type)},
				{"short a = 1;", foreign_word_text(foreign_word::short_type)},
				{"std::vector<int> v;", foreign_word_text(foreign_word::std_namespace)},
				{"std::string s = \"x\";", foreign_word_text(foreign_word::std_namespace)},
				{"std::cout << 1;", foreign_word_text(foreign_word::std_namespace)},
				{"array<int> a; a.push_back(1);", "no member 'push_back' on 'array<int>': write a.push(v)"},
				{"array<int> a = [1]; a.pop_back();", "no member 'pop_back' on 'array<int>': write a.pop()"},
				{"array<int> a; a.emplace_back(1);", "no member 'emplace_back' on 'array<int>': construct the value and push it: a.push(T(args))"},
				{"string s = \"x\"; s.c_str();", "no member 'c_str' on 'string': a string is already its text: pass s itself"},
				{"var a = []; a.push_back(1);", "array has no member 'push_back': write a.push(v)"},
			};
			for (const row& r : rows) {
				auto e = make_engine();
				std::string message;
				try {
					e->execute(r.code);
				} catch (const std::exception& error) {
					message = error.what();
				}
				check(occurrences(message, r.text) == 1 && occurrences(message, "error:") <= 1, std::string(r.code) + " reported: " + message);
			}
		});

		test("conditions_refuse_assignment_and_strings", [this]() {
			const auto message = [this](const char* code) {
				auto e = make_engine();
				try { e->execute(code); } catch (const std::exception& error) { return std::string(error.what()); }
				return std::string();
			};
			const std::string assigned = "assignment used as a condition: write '==' to compare, or '((a = b))' with parentheses of its own to assign";
			check(message("int a = 0; int b = 1; if (a = b) { }").find(assigned) != std::string::npos);
			check(message("int a = 1; while (a = 0) { }").find(assigned) != std::string::npos);
			check(message("string s = \"x\"; if (s) { }").find("a string is not a condition: write !s.empty()") != std::string::npos);
			check(message("string s = \"x\"; while (s) { }").find("a string is not a condition: write !s.empty()") != std::string::npos);
			auto e = make_engine();
			check_eq((int64_t)1, e->execute("int a = 0; int b = 1; if ((a = b)) { a; } else { 9; } a;").as_int());
			check_eq((int64_t)2, e->execute("string s = \"x\"; int n = 0; if (!s.empty()) { n = 2; } n;").as_int());
		});

		test("the_words_stay_ordinary_names", [this]() {
			auto e = make_engine();
			check_eq((int64_t)10, e->execute(
				"int a = 2; int b = 3; int struct = 1; int typedef = 1; int unsigned = 1; int long = 1; int short = 1;\n"
				"int c = a * b;\n"
				"struct + typedef + unsigned + long + short + (a * b) - 1 - 0;").as_int());
			check_eq((int64_t)6, e->execute("class long { int v = 6; } long x; x.v;").as_int());
			check_eq((int64_t)7, e->execute("namespace std { int seven() { return 7; } } std::seven();").as_int());
			check_eq((int64_t)8, e->execute("class Stack { int n = 8; int push_back(int v) { return n; } } Stack s; s.push_back(1);").as_int());
		});
	}
};

}

using foreign_syntax_tests = jai::foundry::tests::foreign_syntax_tests;
FOUNDRY_REGISTER(foreign_syntax_tests)
