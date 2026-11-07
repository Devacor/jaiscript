// Lambda value captures and auto declarations follow C++: a value object (script class or bound C++ class) copies through
// its copy constructor, a shared_ptr handle (new T, or a host shared_ptr) copies the handle and shares the object.
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include "../case_table.hpp"

#include <functional>
#include <memory>
#include <string>

namespace capture_auto_copy {

struct CopyCountedVec {
	static inline int copies = 0;
	int64_t x = 1;
	CopyCountedVec() = default;
	explicit CopyCountedVec(int64_t a_x) : x(a_x) {}
	CopyCountedVec(const CopyCountedVec& a_other) : x(a_other.x) { ++copies; }
	CopyCountedVec& operator=(const CopyCountedVec& a_other) = default;
	int64_t get() const { return x; }
	void set(int64_t a_x) { x = a_x; }
};

struct VecHolder {
	CopyCountedVec inner;
	CopyCountedVec& child() { return inner; }
};

struct VecCallbacks {
	std::function<void(CopyCountedVec&)> byReference;
	std::function<void(std::shared_ptr<CopyCountedVec>)> byHandle;
};

}

namespace jai::foundry::tests {

class capture_auto_copy_tests : public suite {
public:
	capture_auto_copy_tests() : suite("Capture And Auto Copies") {}

	void pre_test() override { capture_auto_copy::CopyCountedVec::copies = 0; }

	std::shared_ptr<engine> make_bound_engine() {
		using capture_auto_copy::CopyCountedVec;
		using capture_auto_copy::VecHolder;
		auto result = make_engine();
		jai::stdlib::register_all(result);
		dynamic_binder<CopyCountedVec>(*result, "Vec")
			.constructor<>()
			.constructor<int64_t>()
			.property("x", &CopyCountedVec::x)
			.method("get", &CopyCountedVec::get)
			.method("set", &CopyCountedVec::set)
			.build();
		dynamic_binder<VecHolder>(*result, "VecHolder")
			.constructor<>()
			.property("inner", &VecHolder::inner)
			.method("child", &VecHolder::child)
			.build();
		dynamic_binder<capture_auto_copy::VecCallbacks>(*result, "VecCallbacks")
			.constructor<>()
			.property("byReference", &capture_auto_copy::VecCallbacks::byReference)
			.property("byHandle", &capture_auto_copy::VecCallbacks::byHandle)
			.build();
		result->add_function("make_vec", [](int64_t a_x) { return CopyCountedVec{a_x}; });
		result->add_function("make_shared_vec", [](int64_t a_x) { return std::make_shared<CopyCountedVec>(a_x); });
		return result;
	}

	int64_t run_int(engine& a_engine, const std::string& a_source) {
		auto value = a_engine.execute(a_source);
		check_true(!value.is_null(), "the script returned a value");
		return value.as_int();
	}

	void forge_tests() override {
		test("script_class_value_capture_copies", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				class P { int x = 1; }
				int run() {
					P p = P();
					auto f = [p]() { return p.x; };
					p.x = 5;
					return f();
				}
				run()
			)"), "[p] holds a copy taken at creation");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_default() {
					P p = P();
					auto f = [=]() { return p.x; };
					p.x = 5;
					return f();
				}
				run_default()
			)"), "[=] holds a copy taken at creation");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_write() {
					P p = P();
					auto f = [p]() { p.x = 7; return p.x; };
					f();
					return p.x;
				}
				run_write()
			)"), "a write to the captured copy does not reach the original");
		});

		test("script_class_reference_and_handle_captures_share", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)5, run_int(*eng, R"(
				class P { int x = 1; }
				int run() {
					P p = P();
					auto f = [&p]() { return p.x; };
					auto g = [&]() { return p.x; };
					p.x = 5;
					return f() * 0 + g();
				}
				run()
			)"), "[&p] and [&] share the local");
			check_eq((int64_t)5, run_int(*eng, R"(
				int run_handle() {
					auto h = new P();
					auto f = [h]() { return h.x; };
					auto g = [=]() { return h.x; };
					h.x = 5;
					return f() * 0 + g();
				}
				run_handle()
			)"), "a captured handle shares its object");
		});

		test("script_class_auto_copies_a_value_and_shares_a_handle", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				class P { int x = 1; }
				int run() {
					P p = P();
					auto y = p;
					[[warn_mode]] y.x = 9;
					return p.x;
				}
				run()
			)"), "auto from a value copies");
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_handle() {
					auto h = new P();
					auto y = h;
					y.x = 9;
					return h.x;
				}
				run_handle()
			)"), "auto from a handle shares");
		});

		test("bound_value_capture_copies_through_the_copy_constructor", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					Vec v = Vec(1);
					auto f = [v]() { return v.x; };
					v.x = 5;
					return f();
				}
				run()
			)"), "[v] holds a copy of the C++ object");
			check_eq(1, capture_auto_copy::CopyCountedVec::copies, "the capture ran the copy constructor once");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_default() {
					Vec v = Vec(1);
					auto f = [=]() { return v.get(); };
					v.set(5);
					return f();
				}
				run_default()
			)"), "[=] holds a copy of the C++ object");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_write() {
					Vec v = Vec(1);
					auto f = [v]() { v.x = 7; return v.x; };
					f();
					return v.x;
				}
				run_write()
			)"), "a write to the captured copy does not reach the original");
			check_eq((int64_t)7, run_int(*eng, R"(
				int run_kept() {
					Vec v = Vec(1);
					auto f = [v]() { v.x = v.x + 3; return v.x; };
					f();
					return f();
				}
				run_kept()
			)"), "the lambda's copy is its own and keeps its writes between calls");
		});

		test("bound_program_scope_named_capture_copies", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				Vec gv = Vec(1);
				auto f = [gv]() { return gv.x; };
				gv.x = 5;
				f()
			)"), "[g] copies a global at creation");
		});

		test("bound_reference_capture_shares", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)5, run_int(*eng, R"(
				int run() {
					Vec v = Vec(1);
					auto f = [&v]() { return v.x; };
					auto g = [&]() { return v.x; };
					v.x = 5;
					return f() * 0 + g();
				}
				run()
			)"), "[&v] and [&] share the C++ object");
			check_eq(0, capture_auto_copy::CopyCountedVec::copies, "a reference capture copies nothing");
		});

		test("bound_handle_capture_shares", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)5, run_int(*eng, R"(
				int run() {
					auto h = new Vec(1);
					auto f = [h]() { return h.x; };
					auto g = [=]() { return h.x; };
					h.x = 5;
					return f() * 0 + g();
				}
				run()
			)"), "a captured new'd handle shares its object");
			check_eq((int64_t)5, run_int(*eng, R"(
				int run_host() {
					auto h = make_shared_vec(1);
					auto f = [h]() { return h.x; };
					h.x = 5;
					return f();
				}
				run_host()
			)"), "a captured host shared_ptr shares its object");
			check_eq((int64_t)5, run_int(*eng, R"(
				int run_typed() {
					shared_ptr<Vec> h = Vec(1);
					auto f = [h]() { return h.x; };
					h.x = 5;
					return f();
				}
				run_typed()
			)"), "a captured shared_ptr<Vec> shares its object");
			check_eq(0, capture_auto_copy::CopyCountedVec::copies, "sharing a handle copies no object");
		});

		test("bound_auto_copies_a_value_and_shares_a_handle", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					Vec v = Vec(1);
					auto y = v;
					y.x = 9;
					return v.x;
				}
				run()
			)"), "auto from a Vec value copies");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_host_value() {
					auto a = make_vec(1);
					auto b = a;
					b.x = 9;
					return a.x;
				}
				run_host_value()
			)"), "auto from a host value result copies");
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_new() {
					auto h = new Vec(1);
					auto y = h;
					y.x = 9;
					return h.x;
				}
				run_new()
			)"), "auto from new shares");
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_host_handle() {
					auto h = make_shared_vec(1);
					auto y = h;
					y.x = 9;
					return h.x;
				}
				run_host_handle()
			)"), "auto from a host shared_ptr shares");
		});

		test("bound_auto_from_a_member_copies", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					VecHolder holder = VecHolder();
					auto a = holder.inner;
					a.x = 9;
					return holder.inner.x;
				}
				run()
			)"), "auto from a C++ member copies it");
		});

		test("bound_auto_from_a_reference_return_copies", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					VecHolder holder = VecHolder();
					auto a = holder.child();
					a.x = 9;
					return holder.child().x;
				}
				run()
			)"), "auto from a call returning Vec& copies, as C++ auto drops the reference");
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_ref() {
					VecHolder holder = VecHolder();
					auto& a = holder.child();
					a.x = 9;
					return holder.child().x;
				}
				run_ref()
			)"), "auto& from a call returning Vec& names the member");
		});

		test("value_declarations_from_a_handle_copy_the_object", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_host() {
					auto h = make_shared_vec(1);
					Vec a = h;
					a.x = 9;
					return h.x;
				}
				run_host()
			)"), "Vec a = host handle copies the object, as Vec a = *h does");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_new() {
					auto h = new Vec(1);
					Vec a = h;
					a.x = 9;
					return h.x;
				}
				run_new()
			)"), "Vec a = new'd handle copies the object");
			check_eq((int64_t)1, run_int(*eng, R"(
				class P { int x = 1; }
				int run_script() {
					auto h = new P();
					P a = h;
					a.x = 9;
					return h.x;
				}
				run_script()
			)"), "P a = handle copies the script object");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_typed_ref() {
					VecHolder holder = VecHolder();
					Vec a = holder.child();
					a.x = 9;
					return holder.child().x;
				}
				run_typed_ref()
			)"), "Vec a = a call returning Vec& copies");
		});

		test("shared_ptr_passes_by_value_as_a_handle_and_a_value_slot_copies", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)9, run_int(*eng, R"(
				void mutate(var i) { i.x = 9; }
				int run_param() {
					shared_ptr<Vec> m = make_shared_vec(1);
					mutate(m);
					return m.x;
				}
				run_param()
			)"), "a shared_ptr passed to a by-value var parameter shares");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_copy() {
					shared_ptr<Vec> m = make_shared_vec(1);
					Vec b = m;
					b.x = 9;
					return m.x;
				}
				run_copy()
			)"), "Vec b = m copies the object a shared_ptr names");
		});

		test("host_shared_ptr_results_are_handles", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_var() {
					var h = make_shared_vec(1);
					var y = h;
					y.x = 9;
					return h.x;
				}
				run_var()
			)"), "var copies of a host shared_ptr share the object");
			check_eq((int64_t)3, run_int(*eng, R"(
				int run_weak() {
					auto h = make_shared_vec(3);
					weak_ptr<Vec> w = h;
					return w.lock().x;
				}
				run_weak()
			)"), "a host shared_ptr seeds a weak_ptr");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_value() {
					auto v = make_vec(1);
					var y = v;
					y.x = 9;
					return v.x;
				}
				run_value()
			)"), "a host result returned by value is a value");
			check_eq(2, capture_auto_copy::CopyCountedVec::copies, "the by-value result's boxing and the var copy ran the copy constructor; the handles copied none");
		});

		test("literal_elements_copy_like_captures", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					Vec v = Vec(1);
					auto arr = [v];
					auto f = [v]() { return v.x; };
					v.x = 5;
					return arr[0].x * 10 + f() - 10;
				}
				run()
			)"), "a literal element and a value capture each hold their own copy");
			check_eq(2, capture_auto_copy::CopyCountedVec::copies, "one copy for the element, one for the capture");
			check_eq((int64_t)5, run_int(*eng, R"(
				int run_handle() {
					auto h = new Vec(1);
					auto arr = [h];
					h.x = 5;
					return arr[0].x;
				}
				run_handle()
			)"), "a literal element of a handle shares the object");
		});

		test("typed_container_elements_copy_the_object_a_handle_names", [this]() {
			auto eng = make_bound_engine();
			const char* paths[] = {
				"array<Vec> a = [h]; h.x = 5; return a[0].x;",
				"array<Vec> a; a.push(h); h.x = 5; return a[0].x;",
				"array<Vec> a = [Vec(1)]; a.insert(0, h); h.x = 5; return a[0].x;",
				"array<Vec> a = [Vec(1)]; a[0] = h; h.x = 5; return a[0].x;",
				"array<Vec> a; a.append([h]); h.x = 5; return a[0].x;",
				"array<Vec> a; a.concat([h]); h.x = 5; return a[0].x;",
				"var src = [h]; array<Vec> a = src; h.x = 5; return a[0].x;",
				"map<string, Vec> m = {\"k\": h}; h.x = 5; return m[\"k\"].x;",
				"map<string, Vec> m; m[\"k\"] = h; h.x = 5; return m[\"k\"].x;",
				"array<array<Vec>> a = [[h]]; h.x = 5; return a[0][0].x;",
			};
			int index = 0;
			for (const char* body : paths) {
				capture_auto_copy::CopyCountedVec::copies = 0;
				const std::string name = "typed_path_" + std::to_string(index++);
				check_eq((int64_t)1, run_int(*eng, "int " + name + "() { auto h = make_shared_vec(1); " + body + " } " + name + "()"),
					std::string("a host handle stored into a Vec element is a copy: ") + body);
				check_eq(1, capture_auto_copy::CopyCountedVec::copies, std::string("one copy-constructor call: ") + body);
			}
		});

		test("untyped_and_handle_containers_keep_the_handle", [this]() {
			auto eng = make_bound_engine();
			const char* paths[] = {
				"var a = [h]; h.x = 5; return a[0].x;",
				"array<var> a = [h]; h.x = 5; return a[0].x;",
				"array<shared_ptr<Vec>> a = [h]; h.x = 5; return a[0].x;",
				"array<shared_ptr<Vec>> a; a.push(h); h.x = 5; return a[0].x;",
				"map<string, shared_ptr<Vec>> m = {\"k\": h}; h.x = 5; return m[\"k\"].x;",
				"var m = {\"k\": h}; h.x = 5; return m[\"k\"].x;",
			};
			int index = 0;
			for (const char* body : paths) {
				capture_auto_copy::CopyCountedVec::copies = 0;
				const std::string name = "shared_path_" + std::to_string(index++);
				check_eq((int64_t)5, run_int(*eng, "int " + name + "() { auto h = make_shared_vec(1); " + body + " } " + name + "()"),
					std::string("an untyped or handle element shares the object: ") + body);
				check_eq(0, capture_auto_copy::CopyCountedVec::copies, std::string("no copy: ") + body);
			}
		});

		test("script_class_typed_elements_are_new_objects", [this]() {
			auto eng = make_bound_engine();
			check_eq(std::string("false false false false true true"), eng->execute(R"(
				class P { int x = 1; }
				auto h = new P();
				array<P> lit = [h];
				array<P> pushed;
				pushed.push(h);
				map<string, P> m = {"k": h};
				array<array<P>> nested = [[h]];
				var untyped = [h];
				array<shared_ptr<P>> handles = [h];
				to_string(same_object(h, lit[0])) + " " + to_string(same_object(h, pushed[0])) + " " + to_string(same_object(h, m["k"])) + " " +
					to_string(same_object(h, nested[0][0])) + " " + to_string(same_object(h, untyped[0])) + " " + to_string(same_object(h, handles[0]))
			)").as<std::string>(), "a class element holds its own object; an untyped or handle element is the handle's object");
			check_eq((int64_t)1, run_int(*eng, R"(
				array<P> values = [new P()];
				auto taken = values[0];
				taken.x = 9;
				values[0].x
			)"), "a value element copies on the way out, as any value does");
		});

		test("nested_lambdas_copy_a_captured_value", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					Vec v = Vec(1);
					auto outer = [v]() {
						auto inner = [v]() { return v.x; };
						return inner;
					};
					auto made = outer();
					v.x = 5;
					return made();
				}
				run()
			)"), "the inner lambda's copy is independent of the outer frame's later writes");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_inner_write() {
					Vec v = Vec(1);
					auto outer = [v]() {
						auto inner = [v]() { v.x = 8; return v.x; };
						inner();
						return v.x;
					};
					return outer();
				}
				run_inner_write()
			)"), "a write to the inner lambda's copy does not reach the outer lambda's copy");
			check_eq((int64_t)5, run_int(*eng, R"(
				int run_shared() {
					auto h = new Vec(1);
					auto outer = [h]() {
						auto inner = [h]() { return h.x; };
						return inner;
					};
					auto made = outer();
					h.x = 5;
					return made();
				}
				run_shared()
			)"), "a handle stays shared through nested captures");
		});

		test("captures_of_a_field_holding_a_cpp_value_copy", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				class Owner {
					Vec v;
					Owner() { v = Vec(1); }
				}
				int run() {
					Owner o = Owner();
					auto f = [o]() { return o.v.x; };
					o.v.x = 5;
					return f();
				}
				run()
			)"), "a captured script object copies its C++ field");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_init() {
					Owner o = Owner();
					auto f = [w = o.v]() { return w.x; };
					o.v.x = 5;
					return f();
				}
				run_init()
			)"), "an init-capture of a C++ field copies it");
			check_eq((int64_t)1, run_int(*eng, R"(
				class Keeper {
					Vec v;
					Keeper() { v = Vec(1); }
					auto snapshot() {
						auto held = v;
						return [held]() { return held.x; };
					}
				}
				int run_member() {
					Keeper k = Keeper();
					auto f = k.snapshot();
					k.v.x = 5;
					return f();
				}
				run_member()
			)"), "a capture of a local copied from a C++ field holds its own copy");
		});

		test("var_from_a_reference_return_copies", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				int run() {
					VecHolder holder = VecHolder();
					var a = holder.child();
					a.x = 9;
					return holder.child().x;
				}
				run()
			)"), "var from a call returning Vec& copies, as auto does");
			check_eq(1, capture_auto_copy::CopyCountedVec::copies, "the var ran the copy constructor once");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_member() {
					VecHolder holder = VecHolder();
					var a = holder.inner;
					a.x = 9;
					return holder.inner.x;
				}
				run_member()
			)"), "var from a C++ member copies it");
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_ref() {
					VecHolder holder = VecHolder();
					var& a = holder.child();
					a.x = 9;
					return holder.child().x;
				}
				run_ref()
			)"), "var& from a call returning Vec& names the member");
			check_eq((int64_t)9, run_int(*eng, R"(
				int run_handle() {
					var h = make_shared_vec(1);
					var y = h;
					y.x = 9;
					return h.x;
				}
				run_handle()
			)"), "var from a host shared_ptr still shares");
		});

		test("by_value_parameters_copy_a_value_object", [this]() {
			auto eng = make_bound_engine();
			eng->execute(R"(
				class P { int x = 1; }
				void mutate(var p) { p.x = 9; }
				void mutate_vec(Vec p) { p.x = 9; }
				void mutate_script(P p) { p.x = 9; }
				void mutate_ref(var& p) { p.x = 9; }
			)");
			const char* copies[] = {
				"Vec v = Vec(1); mutate(v); return v.x;",
				"Vec v = Vec(1); mutate_vec(v); return v.x;",
				"P v = P(); mutate(v); return v.x;",
				"P v = P(); mutate_script(v); return v.x;",
				"VecHolder holder = VecHolder(); mutate(holder.child()); return holder.child().x;",
				"VecHolder holder = VecHolder(); mutate_vec(holder.child()); return holder.child().x;",
				"VecHolder holder = VecHolder(); mutate(holder.inner); return holder.inner.x;",
			};
			int index = 0;
			for (const char* body : copies) {
				const std::string name = "param_copy_" + std::to_string(index++);
				check_eq((int64_t)1, run_int(*eng, "int " + name + "() { " + body + " } " + name + "()"),
					std::string("a by-value parameter holds a copy: ") + body);
			}
			const char* shares[] = {
				"Vec v = Vec(1); mutate_ref(v); return v.x;",
				"VecHolder holder = VecHolder(); var& held = holder.child(); mutate_ref(held); return holder.child().x;",
				"shared_ptr<Vec> m = Vec(1); mutate(m); return m.x;",
				"auto m = make_shared_vec(1); mutate(m); return m.x;",
				"auto m = new Vec(1); mutate(m); return m.x;",
				"auto m = new P(); mutate(m); return m.x;",
			};
			index = 0;
			for (const char* body : shares) {
				const std::string name = "param_share_" + std::to_string(index++);
				check_eq((int64_t)9, run_int(*eng, "int " + name + "() { " + body + " } " + name + "()"),
					std::string("a reference parameter or a handle argument shares: ") + body);
			}
		});

		test("a_call_returning_a_reference_binds_a_reference_parameter", [this]() {
			auto eng = make_bound_engine();
			eng->execute(R"(
				class P { int x = 1; }
				P kept = P();
				P& kept_ref() { return kept; }
				P kept_value() { return kept; }
				void mutate_ref(var& p) { p.x = 9; }
				void mutate_vec(Vec& p) { p.x = 9; }
				void mutate_script(P& p) { p.x = 9; }
			)");
			const char* lvalues[] = {
				"VecHolder holder = VecHolder(); mutate_ref(holder.child()); return holder.child().x;",
				"VecHolder holder = VecHolder(); mutate_vec(holder.child()); return holder.inner.x;",
				"var holder = new VecHolder(); mutate_ref(holder.child()); return holder.inner.x;",
				"kept.x = 1; mutate_ref(kept_ref()); return kept.x;",
				"kept.x = 1; mutate_script(kept_ref()); return kept.x;",
			};
			int index = 0;
			for (const char* body : lvalues) {
				const std::string name = "ref_call_" + std::to_string(index++);
				check_eq((int64_t)9, run_int(*eng, "int " + name + "() { " + body + " } " + name + "()"),
					std::string("a call returning T& is an lvalue a reference parameter binds: ") + body);
			}
			std::string refused;
			try {
				eng->execute("int by_value_call() { kept.x = 1; mutate_ref(kept_value()); return kept.x; } by_value_call()");
			} catch (const std::exception& error) {
				refused = error.what();
			}
			check_true(refused.find("Cannot pass non-lvalue to reference parameter") != std::string::npos ||
				refused.find("pass a variable") != std::string::npos, "a by-value call result stays a temporary: " + refused);
		});

		test("by_value_returns_copy_a_value_object", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				var pick(VecHolder& h) { return h.child(); }
				Vec pick_vec(VecHolder& h) { return h.child(); }
				var pick_member(VecHolder& h) { return h.inner; }
				int run() {
					VecHolder holder = VecHolder();
					var a = pick(holder);
					a.x = 9;
					Vec b = pick_vec(holder);
					b.x = 9;
					pick(holder).x = 9;
					pick_member(holder).x = 9;
					return holder.child().x;
				}
				run()
			)"), "a by-value return of a host Vec& hands out a copy");
			check_eq((int64_t)9, run_int(*eng, R"(
				shared_ptr<Vec> pass(shared_ptr<Vec> p) { return p; }
				var pass_var(var p) { return p; }
				int run_handle() {
					auto h = make_shared_vec(1);
					pass(h).x = 4;
					pass_var(h).x = 9;
					return h.x;
				}
				run_handle()
			)"), "a returned handle shares its object");
		});

		test("make_object_of_a_shared_ptr_is_a_handle", [this]() {
			using capture_auto_copy::CopyCountedVec;
			auto eng = make_bound_engine();
			auto host = std::make_shared<CopyCountedVec>(1);
			eng->add_global("made", eng->make_object(host));
			eng->add_global("added", host);
			check_eq((int64_t)9, run_int(*eng, R"(
				var a = made;
				a.x = 4;
				auto b = added;
				b.x = b.x + 5;
				made.x
			)"), "make_object(shared_ptr) and add_global(shared_ptr) both share the host object");
			check_eq((int64_t)9, host->x, "the host sees the writes");
			check_eq(0, CopyCountedVec::copies, "a handle copies no object");
			eng->add_global("boxed", eng->make_object<CopyCountedVec>(1));
			check_eq((int64_t)1, run_int(*eng, R"(
				var c = boxed;
				c.x = 9;
				boxed.x
			)"), "make_object<T>(args) is a value");
		});

		test("host_callbacks_copy_a_reference_argument_into_a_by_value_parameter", [this]() {
			using capture_auto_copy::CopyCountedVec;
			using capture_auto_copy::VecCallbacks;
			auto eng = make_bound_engine();
			auto callbacks = std::make_shared<VecCallbacks>();
			eng->add_global("callbacks", callbacks);
			CopyCountedVec target{1};
			eng->execute("callbacks.byReference = [](var p) -> void { p.x = 9; };");
			callbacks->byReference(target);
			check_eq((int64_t)1, target.x, "a var parameter holds a copy of the host's Vec&");
			eng->execute("callbacks.byReference = [](Vec p) -> void { p.x = 9; };");
			callbacks->byReference(target);
			check_eq((int64_t)1, target.x, "a Vec parameter holds a copy of the host's Vec&");
			eng->execute("callbacks.byReference = [](var& p) -> void { p.x = 9; };");
			callbacks->byReference(target);
			check_eq((int64_t)9, target.x, "a var& parameter names the host's object");
			auto handle = std::make_shared<CopyCountedVec>(1);
			eng->execute("callbacks.byHandle = [](var p) -> void { p.x = 9; };");
			callbacks->byHandle(handle);
			check_eq((int64_t)9, handle->x, "a host shared_ptr argument shares");
		});

		test("script_class_deriving_a_bound_class_captures_by_copy", [this]() {
			auto eng = make_bound_engine();
			check_eq((int64_t)1, run_int(*eng, R"(
				class Derived : Vec {
					int extra = 2;
				}
				int run() {
					Derived d = Derived();
					auto f = [d]() { return d.x; };
					d.x = 5;
					return f();
				}
				run()
			)"), "a value capture of a script class deriving a C++ class copies it");
			check_eq((int64_t)1, run_int(*eng, R"(
				int run_auto() {
					Derived d = Derived();
					auto y = d;
					y.x = 9;
					return d.x;
				}
				run_auto()
			)"), "auto from a script class deriving a C++ class copies it");
		});

		test("a_this_capture_in_a_method_reads_the_instance", [this]() {
			// the auto field and auto constructor parameter take the argument's type; a void method's own return still yields its value
			const std::pair<const char*, int> rows[] = {
				{R"(
					class TestClass {
						auto value;
						TestClass(auto v) { value = v; }
						auto test_lambda() {
							auto lambda = [this]() { return value * 2; };
							auto result = lambda();
							return result;
						}
					}
					auto obj = TestClass(42);
					obj.test_lambda()
				)", 84},
				{R"(
					class TestClass {
						auto value;
						TestClass(auto v) { value = v; }
						void call_lambda() {
							auto lambda = [this]() { return value; };
							auto result = lambda();
							return result;
						}
					}
					auto obj = TestClass(42);
					obj.call_lambda()
				)", 42},
				{R"(
					class TestClass {
						auto value;
						TestClass(auto v) { value = v; }
						void method_void() {
							auto lambda = [this]() { print(`lambda: value = ${value}`); };
							lambda();
						}
					}
					auto obj = TestClass(99);
					obj.method_void();
					1
				)", 1},
			};
			check_each(rows, [&](const auto& row) {
				auto eng = make_engine();
				jai::stdlib::register_all(*eng);
				check_eq(row.second, eng->execute(row.first).template as<int>());
			});
			// a global the method already called resolves again from inside the lambda, beside the field it reads
			const std::pair<const char*, const char*> printed[] = {
				{"class TestClass { auto value; TestClass(auto v) { value = v; } "
				 "void call_lambda() { print(\"start\"); auto lambda = [this]() { print(`value = ${value}`); return value; }; print(lambda()); } } "
				 "TestClass(42).call_lambda();", "start\nvalue = 42\n42"},
			};
			check_outputs(printed);
		});
	}
};

}

using capture_auto_copy_tests = jai::foundry::tests::capture_auto_copy_tests;
FOUNDRY_REGISTER(capture_auto_copy_tests)
