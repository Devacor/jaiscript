// Generic instantiation is limited in everything that can grow (docs/site/guide/21-generics.html "Limits and what is not here"):
// how deep it nests, how long one instance's name is, how many instances one use sets off. Passing a limit is a load error
// that names what grew, never a load that does not return
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/generics.hpp>
#include "../case_table.hpp"
#include "../input_deadline.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace jai::foundry::tests {

class generic_limits_tests : public suite {
public:
	generic_limits_tests() : suite("Generic Limits") {}

	struct limit_row {
		std::string name;
		std::string code;
		std::string expected;      // a part of the load error
		size_t errors = 1;         // how many errors the listing holds
		bool short_lines = true;   // no line of the listing spells a long instance name in full
		bool both = true;          // loaded on both backends in one test; a row that makes thousands of instances loads on the runner's
	};

	// what a step gets before the run ends as hung: the slowest here takes seconds in a Debug build on a loaded machine
	static constexpr int k_seconds = 60;

	static bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
	static size_t count(const std::string& text, const std::string& part) {
		size_t found = 0;
		for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) { ++found; }
		return found;
	}
	static std::string load_error(engine& eng, const std::string& code) {
		try {
			eng.execute(code);
		} catch (const std::exception& error) {
			return error.what();
		}
		return "(no error)";
	}
	// the errors of a check that runs nothing
	static std::vector<std::string> check_errors(engine& eng, const std::string& code) {
		std::vector<std::string> texts;
		for (const auto& d : eng.check(code).diagnostics) {
			if (d.severity == check_diagnostic::level::error) { texts.push_back(d.message); }
		}
		return texts;
	}
	static std::shared_ptr<engine> engine_on(backend_type backend) {
		auto e = make_default_engine();
		e->set_backend(backend);
		return e;
	}

	static std::string long_name() { return "generic instantiation names an instance of more than " + std::to_string(detail::k_generic_name_limit) + " characters: "; }
	static std::string many() { return "generic instantiation makes more than " + std::to_string(detail::k_generic_instance_limit) + " instances from one use: "; }
	static std::string deep() { return "generic instantiation nests more than " + std::to_string(detail::k_generic_depth_limit) + " deep: "; }

	// Top<int>: a full binary tree of instances `levels` deep under one use, each level its own generic, and `extra` more
	static std::string tree_of_instances(size_t levels, size_t extra) {
		std::string code = "class L<T> { } class R<T> { } class X<T> { }\nclass T0<T> { T leaf; }\n";
		for (size_t i = 1; i <= levels; ++i) {
			const std::string below = "T" + std::to_string(i - 1);
			code += "class T" + std::to_string(i) + "<T> { " + below + "<L<T>> a; " + below + "<R<T>> b; }\n";
		}
		code += "class Top<T> { T" + std::to_string(levels) + "<T> tree;";
		std::string argument = "T";
		for (size_t i = 0; i < extra; ++i) {
			argument = "array<" + argument + ">";
			code += " X<" + argument + "> x" + std::to_string(i) + ";";
		}
		return code + " }\nTop<int> top;\n";
	}
	// Top<int>'s instances: itself, the tree's (2^(levels+1) - 1), an L and an R under each of the tree's inner ones, the extras
	static size_t tree_instances(size_t levels, size_t extra) { return 1 + ((size_t(1) << (levels + 1)) - 1) + 2 * ((size_t(1) << levels) - 1) + extra; }

	static std::vector<limit_row> rows() {
		const std::string pair = "class Pair<A, B> { A a; B b; }\n", sides = "class L<T> { } class R<T> { }\n";
		const std::string longer = ", and so on, each name longer than the last";
		const std::string grow_chain = long_name() + "'Grow<int>' needs 'Grow<Pair<int, int>>' needs 'Grow<Pair<Pair<int, int>, Pair<int, int>>>'" + longer;
		const std::string tree_chain = deep() + "'Tree<int>' needs 'Tree<L<int>>' needs 'Tree<L<L<int>>>', and so on without end";
		const std::string parameter = "'T' takes no type arguments: it is a type parameter of ";
		return {
			// an instance that multiplies per level: its name passes the limit in a few levels
			{"a_class_value_member_that_doubles", pair + "class Grow<T> { Grow<Pair<T, T>> next; }\nGrow<int> g;\n", grow_chain},
			{"a_class_handle_member_that_doubles", pair + "class Grow<T> { shared_ptr<Grow<Pair<T, T>>> next; }\nGrow<int> g;\n", grow_chain},
			{"a_class_weak_handle_member_that_doubles", pair + "class Grow<T> { weak_ptr<Grow<Pair<T, T>>> up; }\nGrow<int> g;\n", grow_chain},
			{"a_class_container_member_that_doubles", pair + "class Grow<T> { array<Grow<Pair<T, T>>> kids; }\nGrow<int> g;\n", grow_chain},
			{"a_class_member_that_doubles_through_a_builtin", "class Grow<T> { Grow<map<T, T>> next; }\nGrow<int> g;\n",
			 long_name() + "'Grow<int>' needs 'Grow<map<int,int>>' needs 'Grow<map<map<int,int>,map<int,int>>>'" + longer},
			{"a_class_member_that_quadruples", "class Wide<A, B, C, D> { }\nclass W<T> { W<Wide<T, T, T, T>> next; }\nW<int> w;\n",
			 long_name() + "'W<int>' needs 'W<Wide<int, int, int, int>>' needs 'W<Wide<Wide<int, int, int, int>, Wid ... nt, int>, Wide<int, int, int, int>>>'" + longer},
			{"a_method_body_that_doubles", pair + "class Grow<T> { void f() { Grow<Pair<T, T>> inner; } }\nGrow<int> g;\n", grow_chain},
			{"a_class_that_doubles_under_a_bound", "class Base { } class Leaf : Base { } class Two<A, B> : Base { }\nclass Grow<T : Base> { Grow<Two<T, T>> next; }\nGrow<Leaf> g;\n",
			 long_name() + "'Grow<Leaf>' needs 'Grow<Two<Leaf, Leaf>>' needs 'Grow<Two<Two<Leaf, Leaf>, Two<Leaf, Leaf>>>'" + longer},
			{"a_function_that_doubles_by_written_arguments", pair + "void grow<T>(T x, int n) { if (n > 0) { Pair<T, T> p; grow<Pair<T, T>>(p, n - 1); } }\ngrow(1, 3);\n",
			 long_name() + "'grow<int>' needs 'grow<Pair<int, int>>' needs 'grow<Pair<Pair<int, int>, Pair<int, int>>>'" + longer},
			{"a_function_that_doubles_by_deduced_arguments", "void grow<T>(T x, int n) { if (n > 0) { map<T, T> m; grow(m, n - 1); } }\ngrow(1, 3);\n",
			 long_name() + "'grow<int>' needs 'grow<map<int,int>>' needs 'grow<map<map<int,int>,map<int,int>>>'" + longer},
			{"a_method_that_doubles", pair + "class Host { void grow<T>(T x, int n) { if (n > 0) { Pair<T, T> p; grow<Pair<T, T>>(p, n - 1); } } }\nHost h;\nh.grow(1, 3);\n",
			 long_name() + "'Host::grow<int>' needs 'Host::grow<Pair<int, int>>' needs 'Host::grow<Pair<Pair<int, int>, Pair<int, int>>>'" + longer},
			// each level's Nope<...> is an unknown type's own error, which spells its name in full
			{"a_class_that_doubles_through_an_unknown_name", "class List<T> { void f() { List<Nope<T, T>> r; } }\nList<int> l;\n",
			 long_name() + "'List<int>' needs 'List<Nope<int, int>>' needs 'List<Nope<Nope<int, int>, Nope<int, int>>>'" + longer, 9, false},

			// a chain that branches per level: a class's nests to the depth limit down its first branch; a function's is made a
			// level at a time and passes the count, as does a class whose branches end
			{"a_class_value_member_that_branches", sides + "class Tree<T> { Tree<L<T>> a; Tree<R<T>> b; }\nTree<int> t;\n", tree_chain},
			{"a_class_handle_member_that_branches", sides + "class Tree<T> { shared_ptr<Tree<L<T>>> a; shared_ptr<Tree<R<T>>> b; }\nTree<int> t;\n", tree_chain},
			{"a_class_member_that_branches_through_builtins", "class Tree<T> { Tree<array<T>> a; Tree<map<string, T>> b; }\nTree<int> t;\n",
			 deep() + "'Tree<int>' needs 'Tree<array<int>>' needs 'Tree<array<array<int>>>', and so on without end"},
			{"a_function_that_branches_by_written_arguments", sides + "void walk<T>(int n) { if (n > 0) { walk<L<T>>(n - 1); walk<R<T>>(n - 1); } }\nwalk<int>(2);\n",
			 many() + "'walk<int>' needs 'walk<L<int>>' needs 'walk<L<L<int>>>', and so on", 1, true, false},
			{"a_function_that_branches_by_deduced_arguments",
			 sides + "void walk<T>(T x, int n) { if (n > 0) { L<T> l; R<T> r; walk(l, n - 1); walk(r, n - 1); } }\nwalk(1, 2);\n",
			 many() + "'walk<int>' needs 'walk<L<int>>' needs 'walk<L<L<int>>>', and so on", 1, true, false},
			{"a_method_that_branches", sides + "class Host { void walk<T>(int n) { if (n > 0) { walk<L<T>>(n - 1); walk<R<T>>(n - 1); } } }\nHost h;\nh.walk<int>(2);\n",
			 many() + "'Host::walk<int>' needs 'Host::walk<L<int>>' needs 'Host::walk<L<L<int>>>', and so on", 1, true, false},
			{"a_class_whose_one_use_makes_a_tree_of_instances", tree_of_instances(13, 0), many() + "'Top<int>' needs 'T13<int>' needs 'T12<L<int>>', and so on", 1, true, false},

			// type arguments on a type parameter: an error where the generic is declared, used or not (a use below it is then
			// the second error, as below any generic whose declaration does not parse)
			{"type_arguments_on_a_type_parameter_in_a_method_body", "class List<T> { void f() { List<T<T>> r; } }\nList<int> l;\n", parameter + "'List'", 2},
			{"type_arguments_on_a_type_parameter_in_a_result", "class List<T> { List<List<T>> chunks() { List<List<T<T, U>>> r; return r; } }\nList<int> l;\n", parameter + "'List'", 2},
			{"type_arguments_on_a_type_parameter_two_deep", "class List<T> { void f() { List<List<T<T, U>>> r; } }\nList<int> l;\n", parameter + "'List'", 2},
			{"type_arguments_on_a_type_parameter_of_a_generic_nothing_uses", "class List<T> { void f() { List<T<T>> r; } }\n", parameter + "'List'"},
			{"type_arguments_on_a_type_parameter_in_a_field", "class Box<T> { T<int> held; }\n", parameter + "'Box'"},
			{"type_arguments_on_a_type_parameter_in_a_function", "T id<T>(T x) { T<int> y; return x; }\n", parameter + "'id'"},
			{"type_arguments_on_a_type_parameter_in_a_parameter", "void take<T>(T<int> x) { }\n", parameter + "'take'"},
			{"type_arguments_on_a_type_parameter_in_a_generic_method", "class C { void f<T>() { array<T<T>> xs; } }\n", parameter + "'f'"},
			{"type_arguments_on_a_class_type_parameter_in_its_generic_method", "class Box<T> { U as<U>(U other) { T<U> made; return other; } }\n", parameter + "'Box'"},
			{"type_arguments_on_a_type_parameter_in_an_expression", "void f<T>() { auto x = T<int>(1); }\n", parameter + "'f'"},
			{"type_arguments_on_a_type_parameter_in_a_base", "class D<T> : T<int> { }\n", parameter + "'D'"},
			{"type_arguments_on_a_type_parameter_in_a_bound", "class Base<A> { }\nclass E<T, U : T<int>> { }\n", parameter + "'E'"},

			// the texts these had before the other limits
			{"a_chain_through_an_unknown_name_nests_to_the_depth_limit", "class List<T> { void f() { List<Nope<T>> r; } }\nList<int> l;\n",
			 deep() + "'List<int>' needs 'List<Nope<int>>' needs 'List<Nope<Nope<int>>>', and so on without end", 65},
			{"a_member_built_from_its_own_instance_fails_at_the_first_step", "class List<T> { List<List<T>> chunks() { List<List<T>> r; return r; } }\nList<int> l;\n",
			 "'List<int>' would need 'List<List<int>>' and so on without end: the member 'chunks' names an instance of 'List' built from the instance itself; a class's "
			 "members are made with it and a generic function only where it is called, so write it as one outside the class: chunks<T>(List<T> self)"},
		};
	}

	void forge_tests() override {
		// each row: the load returns the row's error, the same text on both backends, and the engine still runs
		for (const limit_row& row : rows()) {
			test(row.name, [this, row]() {
				input_deadline deadline("a load", k_seconds);
				std::vector<std::string> texts;
				for (const backend_type backend : {backend_type::vm, backend_type::interpreter}) {
					auto e = row.both ? engine_on(backend) : make_engine();
					deadline.taking(row.code);
					texts.push_back(load_error(*e, row.code));
					check_eq((int64_t)2, e->execute("1 + 1").as_int(), "the engine runs after the error");
					if (!row.both) { break; }
				}
				check_eq(texts.front(), texts.back(), row.code);
				const std::string& text = texts.front();
				check_true(contains(text, row.expected), text);
				check_eq(row.errors, count(text, ": error: "), text);
				size_t longest = 0;
				for (size_t line = 0; line < text.size();) {
					const size_t end = std::min(text.find('\n', line), text.size());
					longest = std::max(longest, end - line);
					line = end + 1;
				}
				check_true(!row.short_lines || longest < 600, "no line of the listing spells a long instance name in full: " + std::to_string(longest));
			});
		}

		test("the_limits_are_the_documented_ones", [this]() {
			check_eq((size_t)64, detail::k_generic_depth_limit);
			check_eq((size_t)4096, detail::k_generic_name_limit);
			check_eq((size_t)4096, detail::k_generic_instance_limit);
		});

		// a name of exactly the limit is an instance; one character more is the error, with no chain to show for a name the
		// program's own text writes
		test("an_instance_name_of_the_limit_loads_and_one_longer_does_not", [this]() {
			input_deadline deadline("a load", k_seconds);
			const auto program = [](size_t name_length) {
				const std::string held(name_length - std::string("Box<>").size(), 'A');
				return "class Box<T> { T v; }\nclass " + held + " { int n = 7; }\nBox<" + held + "> b;\nb.v.n;\n";
			};
			for (const backend_type backend : {backend_type::vm, backend_type::interpreter}) {
				auto e = engine_on(backend);
				deadline.taking("a name of the limit");
				check_eq((int64_t)7, e->execute(program(detail::k_generic_name_limit)).as_int());
				auto refused = engine_on(backend);
				deadline.taking("a name one past the limit");
				const std::string text = load_error(*refused, program(detail::k_generic_name_limit + 1));
				check_true(contains(text, long_name() + "'Box<AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA ... AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA>'\n"), text);
				check_eq((size_t)1, count(text, ": error: "), text);
			}
		});

		// one use that makes exactly the limit is sound; one instance more is the error
		test("one_use_makes_the_limit_of_instances_and_no_more", [this]() {
			input_deadline deadline("a check", k_seconds);
			check_eq(detail::k_generic_instance_limit, tree_instances(10, 2));
			auto e = make_engine();
			deadline.taking("a tree of instances, the limit of them");
			check_true(check_errors(*e, tree_of_instances(10, 2)).empty());
			auto refused = make_engine();
			deadline.taking("a tree of instances, one past the limit");
			const std::vector<std::string> errors = check_errors(*refused, tree_of_instances(10, 3));
			check_eq((size_t)1, errors.size());
			check_eq(many() + "'Top<int>' needs 'X<array<array<array<int>>>>'", errors.empty() ? std::string() : errors.front());
		});

		// the count is one use's: a program whose own text names more instances than the limit is under none, and what one
		// use makes is counted for that use alone
		test("many_unrelated_uses_are_not_one_use", [this]() {
			input_deadline deadline("a check", k_seconds);
			const size_t classes = 70;
			std::string code = "class Pair<A, B> { A a; B b; }\n";
			for (size_t i = 0; i < classes; ++i) { code += "class C" + std::to_string(i) + " { }\n"; }
			for (size_t i = 0; i < classes; ++i) {
				for (size_t k = 0; k < classes; ++k) { code += "Pair<C" + std::to_string(i) + ", C" + std::to_string(k) + "> p" + std::to_string(i) + "_" + std::to_string(k) + ";\n"; }
			}
			check_true(classes * classes > detail::k_generic_instance_limit);
			auto e = make_engine();
			deadline.taking("more instances than the limit, each the program's own use");
			check_true(check_errors(*e, code).empty());
			check_true(2 * tree_instances(9, 253) > detail::k_generic_instance_limit && tree_instances(9, 253) < detail::k_generic_instance_limit);
			auto two = make_engine();
			deadline.taking("two uses that together pass the limit");
			check_true(check_errors(*two, tree_of_instances(9, 253) + "Top<string> other;\n").empty());
		});

		// what the limits leave alone: a chain 60 deep, each level its own generic, and a member read that spells a parameter's name
		test("a_deep_finite_chain_and_a_member_named_as_a_parameter_load", [this]() {
			input_deadline deadline("a load", k_seconds);
			std::string code = "class C0<T> { T v; }\n";
			for (size_t i = 1; i < 60; ++i) { code += "class C" + std::to_string(i) + "<T> { C" + std::to_string(i - 1) + "<array<T>> next; }\n"; }
			code += "C59<int> c;\n";
			auto e = make_engine();
			deadline.taking(code);
			check_eq(std::string("(no error)"), load_error(*e, code));
			deadline.taking("a member read");
			check_true(e->execute("class P { int T = 1; }\nbool small<T>(P p, T limit) { return p.T < limit; }\nsmall(P(), 3);\n").as_bool());
		});

		// a generic declared again makes the engine's instances again: one that then grows past a limit keeps the declaration
		// it has, with the warning any instance the new declaration cannot make gets
		test("a_generic_declared_again_that_grows_leaves_its_instances_as_they_are", [this]() {
			input_deadline deadline("a load", k_seconds);
			for (const std::string again : {"class Grow<T> { Grow<Pair<T, T>> next; }\n", "class Grow<T> { Grow<L<T>> a; Grow<R<T>> b; }\n"}) {
				auto e = make_engine();
				e->execute("class Pair<A, B> { A a; B b; }\nclass L<T> { } class R<T> { }\nclass Grow<T> { T v; }\nGrow<int> g;\ng.v = 7;\n");
				deadline.taking(again);
				check_eq(std::string("(no error)"), load_error(*e, again));
				std::string warned;
				for (const auto& d : e->last_check_diagnostics().diagnostics) {
					if (d.severity == check_diagnostic::level::warning) { warned += d.message + "\n"; }
				}
				check_true(contains(warned, "'Grow<int>' keeps its earlier declaration: this declaration of 'Grow' cannot make it: generic instantiation n"), warned);
				check_eq((size_t)1, count(warned, "keeps its earlier declaration"), warned);
				check_eq((int64_t)7, e->execute("g.v;").as_int());
			}
		});

		// Every program one token away from these checks to an end: a token dropped, doubled, replaced by one of the words
		// beside the program, or one of those put before it. The programs name their own instances (a list of lists, a
		// function that calls itself, two classes that hold each other), so an edit can make a chain that grows (walk(p, ...)
		// doubles, List<T<T>> puts type arguments on a parameter); each stops at a limit.
		test("every_one_token_edit_of_a_self_referential_generic_program_checks_to_an_end", [this]() {
			struct edited {
				std::string program;
				std::vector<std::string> placed;
			};
			const std::vector<edited> programs = {
				{"class List < T > { T item ; shared_ptr < List < T >> next ; List < T > rest ( ) { List < T > out ; return out ; } } List < List < int >> l ;",
				 {"<", ">", ",", "T", "List", "int"}},
				{"class Pair < A , B > { A a ; B b ; } T walk < T > ( T x , int n ) { if ( n > 0 ) { Pair < T , T > p ; return walk ( x , n - 1 ) ; } return x ; } "
				 "walk ( 1 , 2 ) ;",
				 {"<", ">", ",", "T", "Pair", "p", "x", "int"}},
				{"class Even < T > { shared_ptr < Odd < T >> next ; } class Odd < T > { array < Even < T >> kids ; U as < U > ( U other ) { return other ; } } "
				 "Odd < int > o ; o . as < Even < int >> ( o . kids [ 0 ] ) ;",
				 {"<", ">", ",", "T", "U", "Even", "Odd", "int"}},
			};
			input_deadline deadline("a check", k_seconds);
			size_t checked = 0, refused = 0, outgrown = 0;
			for (const edited& unit : programs) {
				std::vector<std::string> words;
				for (size_t at = 0; at < unit.program.size();) {
					const size_t end = std::min(unit.program.find(' ', at), unit.program.size());
					words.push_back(unit.program.substr(at, end - at));
					at = end + 1;
				}
				const auto check_edit = [&](size_t at, size_t dropped, const std::vector<std::string>& put) {
					std::string code;
					for (size_t i = 0; i <= words.size(); ++i) {
						if (i == at) { for (const std::string& word : put) { code += word + ' '; } }
						if (i < words.size() && (i < at || i >= at + dropped)) { code += words[i] + ' '; }
					}
					deadline.taking(code);
					++checked;
					auto eng = make_engine();
					bool sound = false;
					try {
						const std::vector<std::string> errors = check_errors(*eng, code);
						sound = errors.empty();
						outgrown += std::any_of(errors.begin(), errors.end(), [](const std::string& e) { return contains(e, "generic instantiation "); }) ? 1 : 0;
					} catch (const std::exception& error) {
						outgrown += contains(error.what(), "it is a type parameter of") ? 1 : 0;
					}
					if (!sound) { ++refused; }
					return sound;
				};
				check_true(check_edit(0, 0, {}), "the program itself is sound: " + unit.program);
				for (size_t at = 0; at < words.size(); ++at) {
					check_edit(at, 1, {});
					check_edit(at, 0, {words[at]});
					for (const std::string& word : unit.placed) {
						if (word != words[at]) { check_edit(at, 1, {word}); }
						check_edit(at, 0, {word});
					}
				}
			}
			check_true(checked > 2500, std::to_string(checked));
			check_true(refused > 1000, std::to_string(refused));
			check_true(outgrown > 10, "edits that grow or put type arguments on a parameter are among them: " + std::to_string(outgrown));
		});
	}
};

} // namespace jai::foundry::tests

using generic_limits_tests = jai::foundry::tests::generic_limits_tests;
FOUNDRY_REGISTER(generic_limits_tests)
