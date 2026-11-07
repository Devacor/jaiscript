#pragma once

// Math band: the math kinds (math::vec2, vec3, vec4, ivec2, ivec3, color; docs/site/guide/15-math.html) as typed, auto and var
// locals, array elements, map values, class fields, statics, namespace variables and parameters, read and written through every lvalue kind
// (component stores and compounds, whole-value compounds, reference parameters, range-for aliases and copies, [&] and
// [=] captures, a function writing a global) inside counted loops, so a lane that takes a math value for a scalar or a
// store that lands in a copy diverges; values flow through the operators, the component-wise and color libraries and the
// geometry library and its methods. Its own RNG stream and name prefix `u`: the main program's draws are unchanged.
// Operands pair the same kind and ivec scalars are ints, so the band raises only on its rare deliberate errors, and
// those must raise alike. The result is [uout, every value].

#include "fuzz_harness.hpp"

#include <string>
#include <vector>

namespace jai::fuzz {

	class math_band {
	public:
		explicit math_band(uint64_t seed) : rng_(mix64(seed + 0x2545F4914F6CDD1DULL)) {}

		bool roll() { return rng_.chance(25); }

		std::string emit() {
			// inside a function every value is a frame local (slots, the vm's component_base_slots); at program scope a global
			wrapped_ = rng_.chance(50);
			head_.push_back("var uout = \"\";");
			const int locals = static_cast<int>(rng_.range(2, 4));
			for (int i = 0; i < locals; ++i) declare_local();
			declare_array();
			declare_map();
			declare_class();
			const int n = static_cast<int>(rng_.range(6, 14));
			for (int i = 0; i < n; ++i) {
				if (rng_.chance(30)) loop();
				else main_.push_back(statement(""));
			}
			std::string result = "[uout";
			for (const auto& v : values_) {
				if (v.result) result += ", " + v.text;
			}
			result += "]";
			std::string out;
			for (const auto& l : classes_) out += l + "\n";
			for (const auto& l : funcs_) out += l + "\n";
			if (wrapped_) out += "function umain() -> var {\n";
			for (const auto& l : head_) out += l + "\n";
			for (const auto& l : main_) out += l + "\n";
			if (wrapped_) out += "return " + result + ";\n}\numain();\n";
			else out += result + ";\n";
			return out;
		}

	private:
		enum kind { v2, v3, i2, i3, v4, c4 };
		struct value { std::string text; int k; bool local = false; bool result = false; };

		fuzz_rng rng_;
		int names_ = 0;
		bool wrapped_ = false;
		std::vector<std::string> classes_, head_, funcs_, main_;
		std::vector<value> values_;
		std::vector<int> by_value_fn_, by_ref_fn_;
		std::string array_name_, map_name_;
		int array_kind_ = 0, array_size_ = 0;

		std::string fresh(const char* prefix) { return prefix + std::to_string(names_++); }
		static bool integral(int k) { return k == i2 || k == i3; }
		static int dims(int k) { return (k == v4 || k == c4) ? 4 : (k == v3 || k == i3) ? 3 : 2; }
		static int other_precision(int k) { switch (k) { case v2: return i2; case v3: return i3; case i2: return v2; case i3: return v3; case c4: return c4; default: return v4; } }
		static const char* type_name(int k) {
			switch (k) { case v2: return "math::vec2"; case v3: return "math::vec3"; case v4: return "math::vec4"; case c4: return "math::color"; case i2: return "math::ivec2"; default: return "math::ivec3"; }
		}
		int random_kind() { return static_cast<int>(rng_.below(6)); }

		std::string int_literal() { return std::to_string(rng_.range(-3, 9)); }
		std::string float_literal() {
			static constexpr const char* const f[] = {"0.5", "1.5", "-2.25", "3.0", "0.1", "-0.0", "7.75"};
			return f[rng_.below(7)];
		}
		std::string ctor(int k) {
			std::string s = std::string(type_name(k)) + "(";
			const int n = rng_.chance(10) ? 0 : dims(k);
			for (int i = 0; i < n; ++i) {
				if (i) s += ", ";
				s += (integral(k) || rng_.chance(40)) ? int_literal() : float_literal();
			}
			return s + ")";
		}
		std::string component(int k) {
			static constexpr const char* const two[] = {"x", "y", "x", "y", "width", "height"};
			static constexpr const char* const three[] = {"x", "y", "z", "x", "y", "z", "width", "height", "depth"};
			static constexpr const char* const four[] = {"x", "y", "z", "w", "w", "width", "height", "depth"};
			static constexpr const char* const rgba[] = {"r", "g", "b", "a"};
			if (k == c4) return rgba[rng_.below(4)];
			return dims(k) == 4 ? four[rng_.below(8)] : dims(k) == 3 ? three[rng_.below(9)] : two[rng_.below(6)];
		}

		void declare_local() {
			const int k = random_kind();
			const std::string name = fresh("uv");
			static constexpr const char* const spell[] = {"", "var ", "auto "};
			const int s = static_cast<int>(rng_.below(4));
			const std::string decl = s == 3 || s == 0 ? std::string(type_name(k)) + " " : spell[s];
			if (s == 3 && rng_.chance(50)) head_.push_back(decl + name + ";");
			else head_.push_back(decl + name + " = " + ctor(k) + ";");
			values_.push_back({name, k, true, true});
		}
		void declare_array() {
			array_kind_ = random_kind();
			array_name_ = fresh("uA");
			array_size_ = static_cast<int>(rng_.range(2, 4));
			std::string s = rng_.chance(70) ? std::string("array<") + type_name(array_kind_) + "> " : "var ";
			s += array_name_ + " = [";
			for (int i = 0; i < array_size_; ++i) { if (i) s += ", "; s += ctor(array_kind_); }
			head_.push_back(s + "];");
			for (int i = 0; i < array_size_; ++i) values_.push_back({array_name_ + "[" + std::to_string(i) + "]", array_kind_});
			values_.push_back({array_name_, -1, false, true});
		}
		void declare_map() {
			const int k = random_kind();
			map_name_ = fresh("uM");
			head_.push_back(std::string("map<string, ") + type_name(k) + "> " + map_name_ + " = {\"a\": " + ctor(k) + ", \"b\": " + ctor(k) + "};");
			values_.push_back({map_name_ + "[\"a\"]", k});
			values_.push_back({map_name_ + "[\"b\"]", k});
			values_.push_back({map_name_, -1, false, true});
		}
		void declare_class() {
			const int kp = random_kind(), kq = random_kind(), ks = random_kind(), kn = random_kind();
			const std::string cls = fresh("uC"), obj = fresh("uo"), ns = fresh("uN");
			classes_.push_back("class " + cls + " { " + type_name(kp) + " p = " + ctor(kp) + "; " + type_name(kq) + " q; int n = 0; static " +
			                   type_name(ks) + " s = " + ctor(ks) + "; }");
			classes_.push_back("namespace " + ns + " { " + type_name(kn) + " w = " + ctor(kn) + "; }");
			head_.push_back(cls + " " + obj + " = " + cls + "();");
			values_.push_back({obj + ".p", kp, false, true});
			values_.push_back({obj + ".q", kq, false, true});
			values_.push_back({cls + "::s", ks, false, true});
			values_.push_back({ns + "::w", kn, false, true});
		}

		const value* pick_value(int k) {
			std::vector<const value*> c;
			for (const auto& v : values_) if (v.k == k) c.push_back(&v);
			return c.empty() ? nullptr : c[rng_.below(static_cast<uint32_t>(c.size()))];
		}
		const value& any_value() {
			std::vector<const value*> c;
			for (const auto& v : values_) if (v.k >= 0) c.push_back(&v);
			return *c[rng_.below(static_cast<uint32_t>(c.size()))];
		}
		const value* any_local() {
			std::vector<const value*> c;
			for (const auto& v : values_) if (v.local) c.push_back(&v);
			return c.empty() ? nullptr : c[rng_.below(static_cast<uint32_t>(c.size()))];
		}

		std::string scalar(int k, const std::string& ivar) {
			switch (rng_.below(integral(k) ? 5 : 7)) {
				case 0: case 1: return int_literal();
				case 2: if (!ivar.empty()) return ivar; return int_literal();
				case 3: {
					const value* v = pick_value(integral(k) ? (rng_.chance(50) ? i2 : i3) : random_kind());
					return v ? v->text + "." + component(v->k) : int_literal();
				}
				case 4: return geometry_scalar(integral(k));
				default: return float_literal();
			}
		}
		std::string nonzero_divisor(int k) {
			if (integral(k)) { static constexpr const char* const d[] = {"2", "3", "-2"}; return d[rng_.below(3)]; }
			static constexpr const char* const d[] = {"2", "0.5", "-4.0", "3"}; return d[rng_.below(4)];
		}
		std::string mexpr(int k, const std::string& ivar, int depth = 0) {
			const value* v = pick_value(k);
			const std::string self = v ? v->text : ctor(k);
			switch (depth > 1 ? 0 : rng_.below(10)) {
				case 0: return self;
				case 1: return ctor(k);
				case 2: return "(" + self + " + " + mexpr(k, ivar, depth + 1) + ")";
				case 3: return "(" + self + " - " + mexpr(k, ivar, depth + 1) + ")";
				case 4: return rng_.chance(50) ? "(" + self + " * " + scalar(k, ivar) + ")" : "(" + scalar(k, ivar) + " * " + self + ")";
				case 5: return "-" + self;
				case 6: return by_value_call(k) + "(" + self + ")";
				case 7: case 8: return library_call(k, self, ivar, depth);
				default: {
					const value* o = pick_value(other_precision(k));
					return std::string(type_name(k)) + "(" + (o ? o->text : ctor(other_precision(k))) + ")";
				}
			}
		}
		// a scalar from the geometry library: an exact int from ivecs where the site needs an int, any float otherwise
		std::string geometry_scalar(bool need_int) {
			const value* v = pick_value(need_int ? (rng_.chance(50) ? i2 : i3) : (rng_.chance(50) ? v3 : (rng_.chance(50) ? v2 : i3)));
			if (!v) return int_literal();
			const std::string w = ctor(v->k);
			switch (rng_.below(need_int ? 3 : 7)) {
				case 0: return v->text + ".length_squared()";
				case 1: return "math::dot(" + v->text + ", " + w + ")";
				case 2: return "math::distance_squared(" + v->text + ", " + w + ")";
				case 3: return v->text + ".length()";
				case 4: return "math::distance(" + v->text + ", " + w + ")";
				case 5: return "math::angle_between(" + v->text + ", " + w + ")";
				default: return v->text + ".dot(" + w + ")";
			}
		}
		// the component-wise library: an ivec kind keeps its kind only through abs/min/max/clamp with int bounds (the
		// interpolation family gives a vec), so an ivec site draws from those; the geometry rows keep the kind where they do
		std::string library_call(int k, const std::string& self, const std::string& ivar, int depth) {
			const std::string other = mexpr(k, ivar, depth + 1);
			const std::string ib = int_literal(), ib2 = std::to_string(rng_.range(10, 20));
			if (integral(k)) {
				switch (rng_.below(5)) {
					case 0: return "math::abs(" + self + ")";
					case 1: return "math::min(" + self + ", " + other + ")";
					case 2: return "math::max(" + self + ", " + (rng_.chance(50) ? other : ib) + ")";
					case 3:
						if (k == i3) return rng_.chance(50) ? "math::cross(" + self + ", " + other + ")" : "(" + self + ").cross(" + other + ")";
						return "math::perpendicular(" + self + ")";
					default: return "math::clamp(" + self + ", " + ib + ", " + ib2 + ")";
				}
			}
			if (k == c4) {
				switch (rng_.below(6)) {
					case 0: return "math::mix(" + self + ", " + other + ", " + float_literal() + ")";
					case 1: return "math::mix_rgb(" + self + ", " + other + ", " + float_literal() + ")";
					case 2: return "math::mix_hsv(" + self + ", " + other + ", " + float_literal() + ")";
					case 3: return "math::mix_in_out(" + self + ", " + other + ", 0.25, 2.0)";
					case 4: return "math::clamp(" + self + ", 0.0, 1.0)";
					default: return "math::abs(" + self + ")";
				}
			}
			if (rng_.chance(30)) {
				switch (rng_.below(4)) {
					case 0: return rng_.chance(50) ? "math::normalize(" + self + ")" : "(" + self + ").normalized()";
					case 1: return "math::reflect(" + self + ", " + other + ")";
					case 2: return "math::project(" + self + ", " + other + ")";
					default:
						if (k == v2) return rng_.chance(50) ? "math::rotate(" + self + ", 0.5)" : rng_.chance(50) ? "(" + self + ").rotated(1.25)" : "math::perpendicular(" + self + ")";
						if (k == v3) return rng_.chance(50) ? "math::cross(" + self + ", " + other + ")" : rng_.chance(50) ? "math::rotate(" + self + ", " + other + ", 0.75)" : "(" + self + ").rotated(" + other + ", 0.75)";
						return "math::normalize(" + self + ")";
				}
			}
			const std::string t = rng_.chance(70) ? float_literal() : ctor(k);
			static constexpr const char* const eased[] = {"mix_in", "mix_out", "mix_in_out", "mix_out_in"};
			switch (rng_.below(9)) {
				case 0: return "math::mix(" + self + ", " + other + ", " + t + ")";
				case 1: return "math::lerp(" + self + ", " + other + ", " + t + ")";
				case 2: return "math::" + std::string(eased[rng_.below(4)]) + "(" + self + ", " + other + ", 0.25, 2.0)";
				case 3: return "math::clamp(" + self + ", " + float_literal() + ", 8.0)";
				case 4: return "math::min(" + self + ", " + other + ")";
				case 5: return "math::max(" + self + ", " + float_literal() + ")";
				case 6: return "math::abs(" + self + ")";
				case 7: { static constexpr const char* const r[] = {"floor", "ceil", "round"}; return "math::" + std::string(r[rng_.below(3)]) + "(" + self + ")"; }
				default: return "math::remap(" + self + ", 0.0, 4.0, " + ctor(k) + ", " + other + ")";
			}
		}
		std::string by_value_call(int k) {
			for (int f : by_value_fn_) if (f / 16 == k) return "uf" + std::to_string(f % 16);
			const int id = static_cast<int>(by_value_fn_.size());
			const std::string c = component(k);
			funcs_.push_back("function uf" + std::to_string(id) + "(" + type_name(k) + " p) -> " + type_name(k) + " { p." + c + " = p." + c +
			                 " + 1; return p + p; }");
			by_value_fn_.push_back(k * 16 + id);
			return "uf" + std::to_string(id);
		}
		std::string by_ref_call(int k) {
			for (int f : by_ref_fn_) if (f / 16 == k) return "ug" + std::to_string(f % 16);
			const int id = static_cast<int>(by_ref_fn_.size());
			const std::string c = component(k);
			funcs_.push_back("function ug" + std::to_string(id) + "(" + type_name(k) + "& p) -> void { p." + c + " += 2; p = p + p; }");
			by_ref_fn_.push_back(k * 16 + id);
			return "ug" + std::to_string(id);
		}

		std::string statement(const std::string& ivar) {
			const value& t = any_value();
			const int k = t.k;
			switch (pick({18, 16, 12, 10, 14, 8, 8, 6, 6, 5, 5, 3, 3, 3, 2, 4})) {
				case 0: return t.text + "." + component(k) + " = " + scalar(k, ivar) + ";";
				case 1: {
					switch (rng_.below(4)) {
						case 0: return t.text + "." + component(k) + " += " + scalar(k, ivar) + ";";
						case 1: return t.text + "." + component(k) + " -= " + scalar(k, ivar) + ";";
						case 2: return t.text + "." + component(k) + " *= " + std::to_string(rng_.range(-2, 3)) + ";";
						default: return t.text + "." + component(k) + " /= " + nonzero_divisor(k) + ";";
					}
				}
				case 2: {
					switch (rng_.below(integral(k) ? 5 : 4)) {
						case 0: return t.text + " += " + mexpr(k, ivar) + ";";
						case 1: return t.text + " -= " + mexpr(k, ivar) + ";";
						case 2: return t.text + " *= " + std::to_string(rng_.range(-2, 3)) + ";";
						case 3: return t.text + " /= " + nonzero_divisor(k) + ";";
						default: return t.text + " %= " + nonzero_divisor(k) + ";";
					}
				}
				case 3: return t.text + " = " + mexpr(k, ivar) + ";";
				case 4: return "uout = uout + `${" + mexpr(k, ivar) + "};`;";
				case 5: {
					const value* o = pick_value(rng_.chance(50) ? k : other_precision(k));
					const std::string rhs = o ? o->text : ctor(k);
					switch (rng_.below(4)) {
						case 0: return "if (" + t.text + " == " + rhs + ") { uout = uout + \"=\"; } else { uout = uout + \"!\"; }";
						case 1: return "if (" + t.text + ") { uout = uout + \"t\"; }";
						case 2: return "if (!" + t.text + ") { uout = uout + \"f\"; }";
						default: return "uout = uout + `${(" + t.text + " != " + rhs + ")};`;";
					}
				}
				case 6: return "uout = uout + `${(" + t.text + "." + component(k) + " + " + scalar(k, ivar) + ")};`;";
				case 7: return rng_.chance(50) ? t.text + " = " + by_value_call(k) + "(" + t.text + ");"
				                               : "uout = uout + `${" + by_value_call(k) + "(" + t.text + ")};`;";
				case 8:   // a static or a namespace variable does not bind a reference parameter yet (any type)
					if (t.text.find("::") != std::string::npos) return t.text + "." + component(k) + " += 1;";
					return by_ref_call(k) + "(" + t.text + ");";
				case 9: {
					const value* l = any_local();
					if (!l) return "uout = uout + `${" + t.text + "};`;";
					const std::string f = fresh("ul");
					if (rng_.chance(50)) return "var " + f + " = [&]() { " + l->text + "." + component(l->k) + " += 1; }; " + f + "(); " + f + "();";
					return "var " + f + " = [=]() { return " + l->text + " * 2; }; " + l->text + "." + component(l->k) + " = 5; uout = uout + `${" + f + "()};`;";
				}
				case 10: {
					const int o = other_precision(k);
					const std::string f = fresh("ut");
					return std::string(type_name(o)) + " " + f + " = " + (rng_.chance(50) ? t.text : std::string(type_name(o)) + "(" + t.text + ")") +
					       "; uout = uout + `${" + f + "};`;";
				}
				case 11: {
					const std::string m = fresh("uk"), key = mexpr(k, ivar);
					return "map<var, int> " + m + "; " + m + "[" + key + "] = 1; " + m + "[" + key + "] += 2; " + m + "[" + ctor(k) + "] = 4; uout = uout + `${" + m +
					       ".size()};${" + m + "[" + key + "]};`;";
				}
				case 12: return array_name_ + ".push(" + ctor(array_kind_) + "); uout = uout + `${" + array_name_ + ".size()};`;";
				case 13: {
					if (wrapped_) return t.text + "." + component(k) + " -= 1;";
					const std::string h = fresh("uh");
					funcs_.push_back("function " + h + "() -> void { " + t.text + "." + component(k) + " += 3; }");
					return h + "();";
				}
				case 15: return "uout = uout + `${" + geometry_scalar(false) + "};`;";
				default: {
					switch (rng_.below(4)) {
						case 0: return "uout = uout + `${" + t.text + ".w}`;";
						case 1: return "uout = uout + (" + t.text + " + " + ctor(dims(k) == 3 ? v2 : v3) + ");";
						case 2: return "uout = uout + (" + t.text + " * 0.5);";
						default: return by_value_call(k) + "(" + t.text + ")." + component(k) + " = 1;";
					}
				}
			}
		}

		void loop() {
			const std::string iv = fresh("ui");
			const int reps = static_cast<int>(rng_.range(1, 4));
			const int body = static_cast<int>(rng_.range(1, 3));
			switch (rng_.below(4)) {
				case 0: case 1: {
					main_.push_back("for (int " + iv + " = 0; " + iv + " < " + std::to_string(reps) + "; " + iv + "++) {");
					for (int i = 0; i < body; ++i) main_.push_back("\t" + statement(iv));
					if (rng_.chance(40)) main_.push_back("\t" + array_name_ + "[" + iv + " % " + std::to_string(array_size_) + "]." + component(array_kind_) + " += " + iv + ";");
					if (rng_.chance(50)) {
						// an element named by an int local: its components read, stored and unpacked into a typed local (the vm's
						// element lanes, the lowering's unpack)
						const std::string j = fresh("uj"), u = fresh("uu");
						static constexpr const char* const ops[] = {" = ", " += ", " -= ", " *= "};
						main_.push_back("\tint " + j + " = " + iv + " % " + std::to_string(array_size_) + ";");
						main_.push_back("\t" + array_name_ + "[" + j + "]." + component(array_kind_) + ops[rng_.below(4)] + scalar(array_kind_, "") + ";");
						main_.push_back("\t" + std::string(type_name(array_kind_)) + " " + u + " = " + array_name_ + "[" + j + "]; " + u + "." + component(array_kind_) +
						                " += " + array_name_ + "[" + j + "]." + component(array_kind_) + "; uout = uout + `${" + u + "};`;");
					}
					main_.push_back("}");
					return;
				}
				case 2: {
					const std::string e = fresh("ue");
					main_.push_back("for (auto& " + e + " : " + array_name_ + ") {");
					main_.push_back("\t" + e + "." + component(array_kind_) + " += " + scalar(array_kind_, "") + ";");
					if (rng_.chance(50)) main_.push_back("\t" + e + " = " + e + " + " + ctor(array_kind_) + ";");
					main_.push_back("}");
					return;
				}
				default: {
					const std::string e = fresh("ue");
					main_.push_back("for (auto " + e + " : " + array_name_ + ") {");
					main_.push_back("\t" + e + "." + component(array_kind_) + " = 99;");
					main_.push_back("\tuout = uout + `${" + e + "};`;");
					main_.push_back("}");
					return;
				}
			}
		}

		int pick(const std::vector<uint32_t>& w) {
			uint32_t total = 0;
			for (auto x : w) total += x;
			uint32_t r = rng_.below(total);
			for (size_t i = 0; i < w.size(); ++i) { if (r < w[i]) return static_cast<int>(i); r -= w[i]; }
			return 0;
		}
	};

}
