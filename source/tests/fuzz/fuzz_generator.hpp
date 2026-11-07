#pragma once

// Grammar-driven JaiScript program synthesis. Builds random-but-mostly-valid
// source text from a weighted grammar, biased toward semantically spicy
// constructs: classes (inheritance / ctor delegation / statics / operator
// methods), lambdas with captures, coroutines, ref params (incl. field and
// element lvalue args), compound assigns, ++/-- in conditions, try/catch/throw,
// switch with fallthrough, the int/auto/var type ladder, builtin string/array/
// map methods, int64 overflow edges, hot-reload shapes (multi-segment programs
// on one engine), nested functions, and small recursion.
//
// Occasional runtime errors are deliberate: matching error text on both
// backends is agreement; only mismatches are findings.

#include "fuzz_harness.hpp"
#include "fuzz_math_band.hpp"
#include "fuzz_regex_band.hpp"
#include "fuzz_callargs_band.hpp"
#include "fuzz_block_band.hpp"
#include "fuzz_local_alias_band.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace jai::fuzz {

	enum class vt { i, f, b, s, arr, map, obj, any };

	struct var_info {
		std::string name;
		vt type = vt::any;
		int class_idx = -1; // when type == obj
		bool is_var_decl = false; // declared with `var` (retype allowed)
	};

	struct param_info {
		std::string name;
		std::string type_text; // "int", "int&", "var", "var&", "auto", "string", "C0", "C0&"
		vt type = vt::any;
		bool is_ref = false;
		int class_idx = -1;    // when type == obj: a by-value class parameter (the read-only elision's shape)
	};

	struct func_info {
		std::string name;
		vt ret = vt::any;
		std::vector<param_info> params;
		bool is_coroutine = false;
	};

	struct method_info {
		std::string name;
		vt ret = vt::i;
		int arity = 0;
	};

	struct class_info {
		std::string name;
		int base_idx = -1;
		std::vector<std::pair<std::string, vt>> fields;   // own + inherited (flattened for lookup)
		std::vector<method_info> methods;                 // own + inherited
		std::vector<int> ctor_arities;                    // int params only
		std::vector<std::string> statics;                 // int statics, accessed Class::name
		bool has_op_plus = false;
		bool has_op_lt = false;
		std::string coroutine_method;                     // own or inherited (empty = none)
		std::string typed_int_field;                      // declared-int field (typed writes convert)
	};

	enum class band_mode { sometimes, always, never };

	// Element-argument band: mixed-kind flat arrays and arrays of rows, read by index as
	// SEVERAL operands of one consumer (a call's arguments, print, a concatenation, a push, a
	// math:: call) inside loops the vm closes into cell blocks. A cell the block declines
	// (a string, a nested array, a bool) at one operand position must not leave an earlier
	// operand stale. Its own RNG stream and name prefix: the main program's draws and text
	// are unchanged, the band runs as a segment of its own ahead of them when its roll comes
	// up (band_mode::sometimes, the jaiscript_fuzz default; generate_program's default is never,
	// so the gates that replay seeds keep their programs), and its result is [zout, zp] (the harness engine has no to_string: strings build by
	// concatenation). Callee parameter types are chosen from the kinds each argument can hold,
	// so a band raises only where the backends disagree.
	class element_arg_band {
	public:
		explicit element_arg_band(uint64_t seed) : rng_(mix64(seed + 0x5851F42D4C957F2DULL)) {}

		bool wanted(band_mode mode) {
			const bool roll = rng_.chance(30);
			return mode == band_mode::always || (mode == band_mode::sometimes && roll);
		}

		std::string emit() {
			flat_ = make_table("zA", false);
			rows_ = make_table("zR", true);
			std::vector<std::string> head{"var zout = \"\";", "var zp = [];", "map<string, int> zh;", decl_text(flat_), decl_text(rows_)};
			const int drivers = static_cast<int>(rng_.range(1, 3));
			std::vector<std::string> calls;
			for (int d = 0; d < drivers; ++d) {
				std::vector<std::string> body = loop_lines();
				if (rng_.chance(15)) {
					for (auto& l : body) main_.push_back(std::move(l));
					continue;
				}
				std::string name = "zd" + std::to_string(names_++);
				main_.push_back("function " + name + "() -> void {");
				for (auto& l : body) main_.push_back(std::move(l));
				main_.push_back("}");
				calls.push_back(name + "();");
				if (rng_.chance(25)) calls.push_back(name + "();");
			}
			std::string out;
			for (const auto& l : head) out += l + "\n";
			for (const auto& l : callees_) out += l + "\n";
			for (const auto& l : main_) out += l + "\n";
			for (const auto& l : calls) out += l + "\n";
			out += "[zout, zp];\n";
			return out;
		}

	private:
		static constexpr unsigned K_I = 1, K_F = 2, K_B = 4, K_S = 8, K_A = 16, K_NUM = K_I | K_F;
		struct table { std::string name; bool rows = false; std::vector<std::vector<unsigned>> cells; };
		struct operand { std::string text; unsigned kinds = 0; bool element = false; };
		struct loop_ctx {
			int shape = 0;
			std::string ivar;               // int index/counter in scope ("" = none)
			std::string evar;               // range-for element variable
			const table* t = nullptr;
			std::vector<int> visits;        // flat: i values; rows: row indices
			int mul = 1, max_off = 0;
		};

		fuzz_rng rng_;
		int names_ = 0;
		table flat_, rows_;
		std::vector<std::string> callees_, main_;

		unsigned random_kind() {
			switch (rng_.below(10)) {
				case 0: case 1: case 2: case 3: return K_I;
				case 4: case 5: return K_S;
				case 6: return K_F;
				case 7: return K_B;
				default: return rng_.chance(50) ? K_A : K_S;
			}
		}
		std::string cell_text(unsigned k) {
			switch (k) {
				case K_I: return std::to_string(rng_.range(-3, 40));
				case K_F: { static constexpr const char* const f[] = {"2.5", "0.5", "-1.25", "3.0", "7.75"}; return f[rng_.below(5)]; }
				case K_B: return rng_.chance(50) ? "true" : "false";
				case K_S: { static constexpr const char* const s[] = {"\"a\"", "\"bb\"", "\"q r\"", "\"\"", "\"7\""}; return s[rng_.below(5)]; }
				default: { static constexpr const char* const a[] = {"[1, \"x\"]", "[2.5]", "[]", "[[3], \"y\"]"}; return a[rng_.below(4)]; }
			}
		}
		table make_table(const char* prefix, bool rows) {
			table t;
			t.name = prefix + std::to_string(names_++);
			t.rows = rows;
			const bool clean = rng_.chance(15);
			const int deviate = clean ? 0 : static_cast<int>(rng_.range(10, 35));
			if (rows) {
				const int n = static_cast<int>(rng_.range(2, 5)), w = static_cast<int>(rng_.range(2, 4));
				std::vector<unsigned> home;
				for (int j = 0; j < w; ++j) home.push_back(random_kind());
				for (int r = 0; r < n; ++r) {
					t.cells.emplace_back();
					for (int j = 0; j < w; ++j) t.cells.back().push_back(rng_.chance(static_cast<uint32_t>(deviate)) ? random_kind() : home[static_cast<size_t>(j)]);
				}
			} else {
				const int len = static_cast<int>(rng_.range(4, 12)), period = static_cast<int>(rng_.range(1, 3));
				std::vector<unsigned> home;
				for (int j = 0; j < period; ++j) home.push_back(random_kind());
				t.cells.emplace_back();
				for (int i = 0; i < len; ++i) t.cells.back().push_back(rng_.chance(static_cast<uint32_t>(deviate)) ? random_kind() : home[static_cast<size_t>(i % period)]);
			}
			return t;
		}
		std::string decl_text(const table& t) {
			auto row_text = [&](const std::vector<unsigned>& row) {
				std::string s = "[";
				for (size_t j = 0; j < row.size(); ++j) { if (j) s += ", "; s += cell_text(row[j]); }
				return s + "]";
			};
			if (!t.rows) return "var " + t.name + " = " + row_text(t.cells[0]) + ";";
			std::string s = "var " + t.name + " = [";
			for (size_t r = 0; r < t.cells.size(); ++r) { if (r) s += ", "; s += row_text(t.cells[r]); }
			return s + "];";
		}
		int flat_len() const { return static_cast<int>(flat_.cells[0].size()); }
		int row_count() const { return static_cast<int>(rows_.cells.size()); }
		int row_width() const { return static_cast<int>(rows_.cells[0].size()); }

		std::vector<std::string> loop_lines() {
			loop_ctx c;
			c.shape = static_cast<int>(rng_.below(7)); // 0 cfor flat, 1 cfor rows, 2 range rows, 3 range& rows, 4 while flat, 5 while rows, 6 range flat
			std::vector<std::string> out;
			std::string local = "zl" + std::to_string(names_++);
			out.push_back("int " + local + " = " + std::to_string(rng_.range(0, 9)) + ";");
			std::string head, tail;
			const std::string iv = "zi" + std::to_string(names_++);
			if (c.shape == 0 || c.shape == 4) {
				c.t = &flat_;
				c.mul = rng_.chance(30) ? 2 : 1;
				const int stride = c.mul == 2 ? 1 : static_cast<int>(rng_.range(1, 3));
				c.max_off = static_cast<int>(rng_.range(0, 2));
				if (c.max_off >= flat_len()) c.max_off = flat_len() - 1;
				const int n = (flat_len() - 1 - c.max_off) / c.mul + 1;
				for (int i = 0; i < n; i += stride) c.visits.push_back(i);
				std::string bound = (c.mul == 1 && rng_.chance(35)) ? flat_.name + ".size() - " + std::to_string(c.max_off) : std::to_string(n);
				std::string step = stride == 1 ? (rng_.chance(50) ? iv + "++" : "++" + iv) : iv + " += " + std::to_string(stride);
				c.ivar = iv;
				if (c.shape == 0) head = "for (int " + iv + " = 0; " + iv + " < " + bound + "; " + step + ") {";
				else { out.push_back("int " + iv + " = 0;"); head = "while (" + iv + " < " + bound + ") {"; tail = step + ";"; }
			} else if (c.shape == 1 || c.shape == 5) {
				c.t = &rows_;
				for (int r = 0; r < row_count(); ++r) c.visits.push_back(r);
				std::string bound = rng_.chance(40) ? rows_.name + ".size()" : std::to_string(row_count());
				c.ivar = iv;
				if (c.shape == 1) head = "for (int " + iv + " = 0; " + iv + " < " + bound + "; " + iv + "++) {";
				else { out.push_back("int " + iv + " = 0;"); head = "while (" + iv + " < " + bound + ") {"; tail = iv + " += 1;"; }
			} else {
				c.t = c.shape == 6 ? &flat_ : &rows_;
				const int n = c.shape == 6 ? flat_len() : row_count();
				for (int r = 0; r < n; ++r) c.visits.push_back(r);
				c.evar = "zr" + std::to_string(names_++);
				c.ivar = iv;
				out.push_back("int " + iv + " = 0;");
				const char* decl = c.shape == 3 ? "auto&" : (rng_.chance(70) ? "auto" : "var");
				head = std::string("for (") + decl + " " + c.evar + " : " + c.t->name + ") {";
				tail = rng_.chance(50) ? iv + "++;" : iv + " += 1;";
			}
			out.push_back(head);
			const int consumers = static_cast<int>(rng_.range(1, 2));
			for (int k = 0; k < consumers; ++k) out.push_back(consumer(c, local));
			if (!tail.empty()) out.push_back(tail);
			out.push_back("}");
			return out;
		}

		unsigned flat_kinds(const loop_ctx& c, int off) const {
			unsigned m = 0;
			for (int i : c.visits) m |= flat_.cells[0][static_cast<size_t>(c.mul * i + off)];
			return m;
		}
		unsigned column_kinds(int j) const {
			unsigned m = 0;
			for (const auto& row : rows_.cells) m |= row[static_cast<size_t>(j)];
			return m;
		}
		operand element(const loop_ctx& c) {
			if (c.shape == 0 || c.shape == 4) {
				if (rng_.chance(12)) {
					const int at = static_cast<int>(rng_.below(static_cast<uint32_t>(flat_len())));
					return {flat_.name + "[" + std::to_string(at) + "]", flat_.cells[0][static_cast<size_t>(at)], true};
				}
				const int off = static_cast<int>(rng_.range(0, c.max_off));
				std::string idx = c.mul == 2 ? c.ivar + " * 2" : c.ivar;
				if (off) idx += " + " + std::to_string(off);
				return {flat_.name + "[" + idx + "]", flat_kinds(c, off), true};
			}
			if (c.shape == 6) {
				if (rng_.chance(40)) return {flat_.name + "[" + c.ivar + "]", flat_kinds(c, 0), true};
				return {c.evar, flat_kinds(c, 0), true};
			}
			const int j = static_cast<int>(rng_.below(static_cast<uint32_t>(row_width())));
			const std::string row = c.evar.empty() ? rows_.name + "[" + c.ivar + "]" : (rng_.chance(20) ? rows_.name + "[" + c.ivar + "]" : c.evar);
			return {row + "[" + std::to_string(j) + "]", column_kinds(j), true};
		}
		operand literal() {
			const unsigned k = random_kind();
			return {cell_text(k), k, false};
		}
		operand arg(const loop_ctx& c, const std::string& local) {
			switch (pick({62, 7, 6, 6, 7, 6, 6})) {
				case 0: return element(c);
				case 1: return {c.ivar, K_I, false};
				case 2: return {local, K_I, false};
				case 3: return literal();
				case 4: {
					operand e = element(c);
					if ((e.kinds & ~K_NUM) == 0) return {"(" + e.text + (rng_.chance(50) ? " + 1)" : " * 2)"), e.kinds, true};
					return {"(" + c.ivar + " + " + std::to_string(rng_.range(1, 3)) + ")", K_I, false};
				}
				case 5: {
					operand e = element(c);
					if ((e.kinds & ~K_NUM) == 0) {
						switch (rng_.below(3)) {
							case 0: return {"math::abs(" + e.text + ")", e.kinds, true};
							case 1: return {"math::max(" + e.text + ", " + c.ivar + ")", e.kinds | K_I, true};
							default: return {"math::min(" + e.text + ", 10)", e.kinds | K_I, true};
						}
					}
					return {"math::max(" + c.ivar + ", " + local + ")", K_I, false};
				}
				default: {
					operand e = element(c);
					return {rng_.chance(50) ? "(`${" + e.text + "}`)" : "cast<string>(" + e.text + ")", K_S, true};
				}
			}
		}
		int pick(const std::vector<uint32_t>& w) {
			uint32_t total = 0;
			for (auto x : w) total += x;
			uint32_t roll = rng_.below(total);
			for (size_t i = 0; i < w.size(); ++i) { if (roll < w[i]) return static_cast<int>(i); roll -= w[i]; }
			return 0;
		}
		std::vector<operand> args(const loop_ctx& c, const std::string& local, int lo, int hi) {
			std::vector<operand> a;
			const int n = static_cast<int>(rng_.range(lo, hi));
			for (int i = 0; i < n; ++i) a.push_back(arg(c, local));
			bool any_element = false;
			for (const auto& o : a) any_element |= o.element;
			if (!any_element) a[rng_.below(static_cast<uint32_t>(a.size()))] = element(c);
			return a;
		}
		static std::string joined(const std::vector<operand>& a, const char* sep) {
			std::string s;
			for (size_t i = 0; i < a.size(); ++i) { if (i) s += sep; s += a[i].text; }
			return s;
		}
		std::string param_type(unsigned kinds) {
			if (rng_.chance(40)) { switch (rng_.below(3)) { case 0: return ""; case 1: return "var "; default: return "auto "; } }
			if ((kinds & ~(K_NUM | K_B)) == 0) { static constexpr const char* const t[] = {"int ", "float ", "string "}; return t[rng_.below(3)]; }
			return "string ";
		}
		std::string callee(const std::vector<operand>& a, bool& returns) {
			const std::string name = "zf" + std::to_string(names_++);
			returns = rng_.chance(35);
			std::string sig = "function " + name + "(";
			std::string body;
			const bool templ = rng_.chance(50);
			for (size_t i = 0; i < a.size(); ++i) {
				const std::string p = "zq" + std::to_string(i);
				const std::string ty = param_type(a[i].kinds);
				if (i) sig += ", ";
				sig += ty + p;
				std::string use = p;
				if ((ty == "int " || ty == "float ") && rng_.chance(30)) use = "(" + p + " + 1)";
				if (templ) body += "${" + use + "}" + (i + 1 < a.size() ? " " : "");
				else body += std::string(i ? " + \",\" + " : "") + "`${" + use + "}`";
			}
			const std::string text = templ ? "`" + body + ";`" : "\"\" + " + body + " + \";\"";
			// a template-string key under a compound op or ++/-- (the vm's index-tree landing once raised an internal error for it)
			std::string keyed;
			if (templ && rng_.chance(40)) {
				static constexpr const char* const ops[] = {"++", "--", " += 2", " -= 1"};
				const std::string key = "zh[`" + body + "`]";
				const uint32_t op = rng_.below(5);
				keyed = (op == 4 ? "++" + key : key + ops[op]) + "; zout = zout + `${" + key + "};`; ";
			}
			if (returns) callees_.push_back(sig + ") -> string { " + keyed + "return " + text + "; }");
			else callees_.push_back(sig + (rng_.chance(50) ? ") -> void" : ")") + " { " + keyed + "zout = zout + " + text + "; }");
			return name;
		}
		std::string consumer(const loop_ctx& c, const std::string& local) {
			switch (pick({55, 10, 12, 12, 11})) {
				case 0: {
					std::vector<operand> a = args(c, local, 2, 4);
					bool returns = false;
					const std::string f = callee(a, returns);
					const std::string call = f + "(" + joined(a, ", ") + ")";
					return returns ? "zout = zout + " + call + ";" : call + ";";
				}
				case 1: return "print(" + joined(args(c, local, 2, 3), ", ") + ");";
				case 2: {
					std::vector<operand> a = args(c, local, 2, 3);
					std::string s = "zout = zout";
					for (const auto& o : a) s += " + `${" + o.text + "},`";
					return s + ";";
				}
				case 3: {
					if (rng_.chance(50)) return "zp.push(" + element(c).text + ");";
					return "zp.push([" + joined(args(c, local, 2, 3), ", ") + "]);";
				}
				default: {
					operand x = element(c), y = element(c);
					if ((x.kinds & ~K_NUM) == 0 && (y.kinds & ~K_NUM) == 0)
						return std::string("zout = zout + `${math::") + (rng_.chance(50) ? "max(" : "min(") + x.text + ", " + y.text + ")};`;";
					return "zout = zout + `${" + x.text + "}${" + y.text + "}`;";
				}
			}
		}
	};

	class generator {
	public:
		generator(uint64_t seed, int max_expr_depth = 4, band_mode band = band_mode::never, band_mode math_mode = band_mode::never,
		          band_mode call_args_mode = band_mode::never, band_mode blocks_mode = band_mode::never, band_mode local_alias_mode = band_mode::never,
		          band_mode regex_mode = band_mode::never)
			: rng_(mix64(seed + 0xD1B54A32D192ED03ULL)), max_depth_(max_expr_depth), seed_(seed), band_(band), math_(math_mode), call_args_(call_args_mode),
			  blocks_(blocks_mode), local_alias_(local_alias_mode), regex_(regex_mode), block_(seed) {}

		program generate() {
			program p;
			p.seed = seed_;
			classes_.clear();
			funcs_.clear();
			scopes_.clear();
			names_ = 0;
			lines_.clear();
			scopes_.push_back({});

			int n_classes = pick_weighted({55, 30, 15}); // 0,1,2
			for (int i = 0; i < n_classes; ++i) emit_class();
			const bool block_roll = block_.roll();
			blocks_on_ = blocks_ == band_mode::always || (blocks_ == band_mode::sometimes && block_roll);
			int n_funcs = pick_weighted({25, 35, 25, 15}); // 0..3
			for (int i = 0; i < n_funcs; ++i) emit_function();

			const size_t stmt_begin = begin_statements();
			int n_stmts = static_cast<int>(rng_.range(4, 12));
			for (int i = 0; i < n_stmts; ++i) emit_stmt(0);

			// Final expression: the program's result value.
			end_statements(stmt_begin, "zbmain", expr(random_value_type(), max_depth_));
			element_arg_band band(seed_);
			if (band.wanted(band_)) { p.segments.push_back(band.emit()); p.labels.push_back("element"); }
			math_band mband(seed_);
			const bool math_roll = mband.roll();
			if (math_ == band_mode::always || (math_ == band_mode::sometimes && math_roll)) {
				std::string text = mband.emit();
				if (blocks_on_) {   // a lowered-math loop function joins the band's result
					const size_t last = text.rfind('\n', text.size() - 2) + 1;
					const std::string tail = text.substr(last, text.size() - last - 2);   // the result expression without ";\n"
					text = text.substr(0, last) + block_.math_function("zbm0") + "[" + tail + ", zbm0()];\n";
				}
				p.segments.push_back(std::move(text));
				p.labels.push_back("math");
			}
			call_args_band cband(seed_);
			const bool call_args_roll = cband.roll();
			if (call_args_ == band_mode::always || (call_args_ == band_mode::sometimes && call_args_roll)) { p.segments.push_back(cband.emit()); p.labels.push_back("call-args"); }
			local_alias_band lband(seed_);
			const bool local_alias_roll = lband.roll();
			if (local_alias_ == band_mode::always || (local_alias_ == band_mode::sometimes && local_alias_roll)) { p.segments.push_back(lband.emit()); p.labels.push_back("local-alias"); }
			regex_band rband(seed_);
			const bool regex_roll = rband.roll();
			if (regex_ == band_mode::always || (regex_ == band_mode::sometimes && regex_roll)) { p.segments.push_back(rband.emit()); p.labels.push_back("regex"); }
			p.segments.push_back(join());
			p.labels.push_back("main");

			// Hot-reload shape: a second execute() on the same engine that
			// redefines a function or class, then uses surviving state.
			if ((n_classes > 0 || n_funcs > 0) && rng_.chance(22)) {
				lines_.clear();
				if (n_classes > 0 && rng_.chance(60)) emit_class_redefinition();
				else if (n_funcs > 0) emit_function_redefinition();
				const size_t stmt_begin = begin_statements();
				int extra = static_cast<int>(rng_.range(1, 4));
				for (int i = 0; i < extra; ++i) emit_stmt(0);
				end_statements(stmt_begin, "zbreload", expr(random_value_type(), max_depth_));
				p.segments.push_back(join());
				p.labels.push_back("reload");
			}
			return p;
		}

	private:
		// draws never share an unsequenced expression (call arguments, + operands) so every compiler generates the same program per seed; a split draws the right operand first, the order GCC and MSVC used, so seeds keep their programs
		fuzz_rng rng_;
		int max_depth_;
		uint64_t seed_;
		band_mode band_;
		band_mode math_;
		band_mode call_args_;
		band_mode blocks_;
		band_mode local_alias_;
		band_mode regex_;
		block_band block_;
		bool blocks_on_ = false;
		bool wrapped_ = false;
		size_t prelude_emitted_ = 0;
		int names_ = 0;
		std::vector<class_info> classes_;
		std::vector<func_info> funcs_;
		std::vector<std::vector<var_info>> scopes_;
		std::vector<std::string> lines_;
		int loop_depth_ = 0;
		int stmt_nesting_ = 0;
		bool in_function_ = false;
		vt current_ret_ = vt::any;

		// ---------- helpers ----------
		std::string fresh(const char* prefix) { return std::string(prefix) + std::to_string(names_++); }
		// a reserved word as a member's name, reached only qualified (o.default, this.catch); fresh when the class holds them all.
		// Picked from the name counter, never the rng, so a seed's program keeps its shape and only its member names change
		static const std::vector<std::string>& keyword_member_words() {
			static const std::vector<std::string> words {"default", "catch", "new", "for", "if", "import", "class", "return", "switch", "map", "string", "inline"};
			return words;
		}
		std::string keyword_member_name(const class_info& a_class, const char* a_prefix) {
			const auto& words = keyword_member_words();
			const size_t start = static_cast<size_t>(names_);
			for (size_t i = 0; i < words.size(); ++i) {
				const std::string& word = words[(start + i) % words.size()];
				const bool field_taken = std::any_of(a_class.fields.begin(), a_class.fields.end(), [&](const auto& f) { return f.first == word; });
				const bool method_taken = std::any_of(a_class.methods.begin(), a_class.methods.end(), [&](const auto& m) { return m.name == word; });
				if (!field_taken && !method_taken) { return word; }
			}
			return fresh(a_prefix);
		}
		static std::string bare_member(const std::string& a_name) {
			const auto& words = keyword_member_words();
			return std::find(words.begin(), words.end(), a_name) != words.end() ? "this." + a_name : a_name;
		}
		std::string join() const {
			std::string out;
			for (const auto& l : lines_) { out += l; out += '\n'; }
			return out;
		}
		int pick_weighted(const std::vector<uint32_t>& weights) {
			uint32_t total = 0;
			for (auto w : weights) total += w;
			uint32_t roll = rng_.below(total);
			for (size_t i = 0; i < weights.size(); ++i) {
				if (roll < weights[i]) return static_cast<int>(i);
				roll -= weights[i];
			}
			return 0;
		}

		vt random_value_type() {
			switch (pick_weighted({30, 12, 12, 18, 12, 8, 8})) {
				case 0: return vt::i;
				case 1: return vt::f;
				case 2: return vt::b;
				case 3: return vt::s;
				case 4: return vt::arr;
				case 5: return vt::map;
				default: return classes_.empty() ? vt::i : vt::obj;
			}
		}

		std::vector<const var_info*> vars_of(vt t) const {
			std::vector<const var_info*> out;
			for (const auto& scope : scopes_)
				for (const auto& v : scope)
					if (v.type == t) out.push_back(&v);
			return out;
		}
		const var_info* random_var(vt t) {
			auto vs = vars_of(t);
			if (vs.empty()) return nullptr;
			return vs[rng_.below(static_cast<uint32_t>(vs.size()))];
		}
		bool is_global_var(const var_info* v) const {
			for (const auto& g : scopes_.front()) if (&g == v) return true;
			return false;
		}
		// ::name spelling for reads of GLOBAL-scope vars (~8%): the escape-hatch
		// grammar. Inner-scope vars never gain the qualifier (::local is an
		// undefined-variable error by contract, not a divergence generator).
		std::string spell(const var_info* v) {
			if (rng_.chance(8) && is_global_var(v)) return "::" + v->name;
			return v->name;
		}
		void declare(const std::string& name, vt t, int class_idx = -1, bool is_var = false) {
			scopes_.back().push_back({name, t, class_idx, is_var});
		}

		// ---------- literals ----------
		std::string int_literal() {
			switch (pick_weighted({55, 18, 10, 7, 5, 5})) {
				case 0: return std::to_string(rng_.range(0, 12));
				case 1: return std::to_string(rng_.range(-20, 120));
				case 2: return std::to_string(rng_.range(-100000, 100000));
				case 3: return "9223372036854775807";           // INT64_MAX
				case 4: return "(-9223372036854775807 - 1)";    // INT64_MIN without literal-overflow
				case 5: return std::to_string(rng_.range(0, 1) ? 4611686018427387904LL : -4611686018427387904LL);
				default: return "1";
			}
		}
		std::string float_literal() {
			static constexpr const char* const lits[] = {"0.0", "1.0", "-1.5", "2.5", "3.25", "0.5", "100.125", "-0.25", "1000000.0", "0.001"};
			return lits[rng_.below(10)];
		}
		std::string string_literal() {
			static constexpr const char* const lits[] = {"\"a\"", "\"b\"", "\"xy\"", "\"hello\"", "\"\"", "\"one,two,three\"", "\"  pad  \"", "\"zZz\""};
			return lits[rng_.below(8)];
		}
		std::string bool_literal() { return rng_.chance(50) ? "true" : "false"; }

		// ---------- expressions ----------
		std::string expr(vt want, int depth) {
			if (want == vt::any) {
				switch (pick_weighted({35, 15, 12, 20, 10, 8})) {
					case 0: want = vt::i; break;
					case 1: want = vt::f; break;
					case 2: want = vt::b; break;
					case 3: want = vt::s; break;
					case 4: want = vt::arr; break;
					default: want = vt::map; break;
				}
			}
			switch (want) {
				case vt::i: return int_expr(depth);
				case vt::f: return float_expr(depth);
				case vt::b: return bool_expr(depth);
				case vt::s: return string_expr(depth);
				case vt::arr: return array_expr(depth);
				case vt::map: return map_expr(depth);
				case vt::obj: return obj_expr(depth);
				default: return int_expr(depth);
			}
		}

		std::string int_expr(int depth) {
			if (depth <= 0) {
				if (rng_.chance(45)) {
					if (const var_info* v = random_var(vt::i)) return spell(v);
				}
				return int_literal();
			}
			switch (pick_weighted({28, 14, 8, 7, 6, 8, 6, 6, 5, 4, 4, 4})) {
				case 0: { // arithmetic
					static constexpr const char* const ops[] = {"+", "-", "*", "/", "%"};
					const char* op = ops[rng_.below(5)];
					std::string rhs = int_expr(depth - 1);
					if ((op[0] == '/' || op[0] == '%') && rng_.chance(85))
						rhs = "(" + rhs + " + " + std::to_string(rng_.range(1, 7)) + ")"; // usually nonzero
					return "(" + int_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 1: return int_leaf_or_var();
				case 2: return "(-(" + int_expr(depth - 1) + "))";
				case 3: { // bitwise / shifts (small shift amounts)
					static constexpr const char* const ops[] = {"&", "|", "^", "<<", ">>"};
					const char* op = ops[rng_.below(5)];
					std::string rhs = (op[0] == '<' || op[0] == '>') ? std::to_string(rng_.range(0, 8)) : int_expr(depth - 1);
					return "(" + int_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 4: {
					const std::string no = int_expr(depth - 1);
					const std::string yes = int_expr(depth - 1);
					const std::string cond = bool_expr(depth - 1);
					return "(" + cond + " ? " + yes + " : " + no + ")";
				}
				case 5: { // container/string size
					if (const var_info* a = random_var(vt::arr)) return a->name + (rng_.chance(50) ? ".size()" : ".length()");
					if (const var_info* s = random_var(vt::s)) return s->name + ".size()";
					return int_leaf_or_var();
				}
				case 6: { // array element: literal index, computed flat-binary index, or an
					// array field off an object (the fused-INDEX shapes; OOB draws are
					// differential coverage, not noise)
					if (rng_.chance(25)) {
						if (const var_info* o = random_var(vt::obj)) {
							const class_info& ci = classes_[o->class_idx];
							for (const auto& fld : ci.fields)
								if (fld.second == vt::arr)
									return o->name + "." + fld.first + "[" + std::to_string(rng_.range(0, 2)) + "]";
						}
					}
					if (const var_info* a = random_var(vt::arr)) {
						if (rng_.chance(30)) {
							if (const var_info* iv = random_var(vt::i)) {
								static constexpr const char* const iops[] = {"+", "-", "*", "%", "/"};
								const int by = static_cast<int>(rng_.range(1, 2));
								std::string idx = iv->name + " " + iops[rng_.below(5)] + " " + std::to_string(by);
								if (rng_.chance(30)) {   // left-spine chain (fused-INDEX chain shape)
									const int by2 = static_cast<int>(rng_.range(1, 2));
									idx += std::string(" ") + iops[rng_.below(5)] + " " + std::to_string(by2);
								}
								return a->name + "[" + idx + "]";
							}
						}
						return a->name + "[" + std::to_string(rng_.range(0, 2)) + "]";
					}
					return int_leaf_or_var();
				}
				case 7: { // int-returning call
					std::string c = call_returning(vt::i);
					return c.empty() ? int_leaf_or_var() : c;
				}
				case 8: { // object int field / method
					if (const var_info* o = random_var(vt::obj)) {
						const class_info& ci = classes_[o->class_idx];
						std::vector<std::string> opts;
						for (const auto& fld : ci.fields) if (fld.second == vt::i) opts.push_back(o->name + "." + fld.first);
						for (const auto& m : ci.methods) if (m.ret == vt::i && m.arity == 0) opts.push_back(o->name + "." + m.name + "()");
						if (!opts.empty()) return opts[rng_.below(static_cast<uint32_t>(opts.size()))];
					}
					return int_leaf_or_var();
				}
				case 9: { // static member read
					for (const auto& ci : classes_)
						if (!ci.statics.empty())
							return ci.name + "::" + ci.statics[rng_.below(static_cast<uint32_t>(ci.statics.size()))];
					return int_leaf_or_var();
				}
				case 10: {
					const std::string rhs = int_expr(depth - 1);
					return "(" + int_expr(depth - 1) + " <=> " + rhs + ")";
				}
				case 11: { // string method returning int
					if (const var_info* s = random_var(vt::s)) {
						switch (rng_.below(3)) {
							case 0: return "(" + s->name + ".find(" + string_literal() + "))";
							case 1: return "(" + s->name + ".count(" + string_literal() + "))";
							default: return s->name + ".length()";
						}
					}
					return int_leaf_or_var();
				}
				default: return int_leaf_or_var();
			}
		}
		std::string int_leaf_or_var() {
			if (rng_.chance(50)) {
				if (const var_info* v = random_var(vt::i)) return spell(v);
			}
			return int_literal();
		}

		std::string float_expr(int depth) {
			if (depth <= 0) {
				if (rng_.chance(40)) {
					if (const var_info* v = random_var(vt::f)) return spell(v);
				}
				return float_literal();
			}
			switch (pick_weighted({35, 20, 15, 12, 10, 8})) {
				case 0: {
					static constexpr const char* const ops[] = {"+", "-", "*", "/"};
					const std::string rhs = float_expr(depth - 1);
					const char* op = ops[rng_.below(4)];
					return "(" + float_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 1: { const var_info* v = random_var(vt::f); return v ? v->name : float_literal(); }
				case 2: { // int/float mixing
					const std::string scale = float_literal();
					return "(" + int_expr(depth - 1) + " * " + scale + ")";
				}
				case 3: {
					const std::string no = float_expr(depth - 1);
					const std::string yes = float_expr(depth - 1);
					const std::string cond = bool_expr(depth - 1);
					return "(" + cond + " ? " + yes + " : " + no + ")";
				}
				case 4: return "(-(" + float_expr(depth - 1) + "))";
				default: {
					std::string c = call_returning(vt::f);
					return c.empty() ? float_literal() : c;
				}
			}
		}

		std::string bool_expr(int depth) {
			if (depth <= 0) {
				if (rng_.chance(35)) {
					if (const var_info* v = random_var(vt::b)) return spell(v);
				}
				return bool_literal();
			}
			switch (pick_weighted({30, 18, 12, 10, 10, 8, 6, 6})) {
				case 0: {
					static constexpr const char* const ops[] = {"<", ">", "<=", ">=", "==", "!="};
					const std::string rhs = int_expr(depth - 1);
					const char* op = ops[rng_.below(6)];
					return "(" + int_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 1: {
					static constexpr const char* const ops[] = {"&&", "||"};
					const std::string rhs = bool_expr(depth - 1);
					const char* op = ops[rng_.below(2)];
					return "(" + bool_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 2: return "(!" + bool_expr(depth - 1) + ")";
				case 3: {
					static constexpr const char* const ops[] = {"==", "!=", "<"};
					const std::string rhs = string_expr(depth - 1);
					const char* op = ops[rng_.below(3)];
					return "(" + string_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 4: {
					static constexpr const char* const ops[] = {"<", ">", "==", "!="};
					const std::string rhs = float_expr(depth - 1);
					const char* op = ops[rng_.below(4)];
					return "(" + float_expr(depth - 1) + " " + op + " " + rhs + ")";
				}
				case 5: { // container predicates
					if (const var_info* a = random_var(vt::arr)) {
						switch (rng_.below(4)) {
							case 0: return a->name + ".empty()";
							case 1: return "(" + a->name + ".contains(" + int_literal() + "))";
							case 2:
								// bare element read as truthiness (element reads reach
								// conditions as reference wrappers - is_truthy must deref)
								return "(" + a->name + ".size() > 0 && " + a->name + "[0])";
							default: return "(" + a->name + ".has(" + int_literal() + "))";
						}
					}
					if (const var_info* m = random_var(vt::map)) return "(" + m->name + ".has(" + string_literal() + "))";
					return bool_literal();
				}
				case 6: { // string predicates
					if (const var_info* s = random_var(vt::s)) {
						switch (rng_.below(3)) {
							case 0: return "(" + s->name + ".contains(" + string_literal() + "))";
							case 1: return "(" + s->name + ".starts_with(" + string_literal() + "))";
							default: return s->name + ".empty()";
						}
					}
					return bool_literal();
				}
				default: { // object ordering via operator<
					if (const var_info* o = random_var(vt::obj)) {
						const class_info& ci = classes_[o->class_idx];
						if (ci.has_op_lt) {
							if (const var_info* o2 = random_var(vt::obj)) {
								if (o2->class_idx == o->class_idx)
									return "(" + o->name + " < " + o2->name + ")";
							}
						}
					}
					return bool_literal();
				}
			}
		}

		std::string string_expr(int depth) {
			if (depth <= 0) {
				if (rng_.chance(40)) {
					if (const var_info* v = random_var(vt::s)) return spell(v);
				}
				return string_literal();
			}
			switch (pick_weighted({30, 20, 15, 15, 12, 8})) {
				case 0: {
					const std::string rhs = string_expr(depth - 1);
					return "(" + string_expr(depth - 1) + " + " + rhs + ")";
				}
				case 1: {
					const std::string n = int_expr(depth - 1);
					return "(" + string_expr(depth - 1) + " + to_string(" + n + "))";
				}
				case 2: { const var_info* v = random_var(vt::s); return v ? v->name : string_literal(); }
				case 3: { // builtin string methods
					std::string base = random_var(vt::s) ? random_var(vt::s)->name : string_literal();
					switch (rng_.below(8)) {
						case 0: return changed_copy(base, "to_upper()");
						case 1: return changed_copy(base, "to_lower()");
						case 2: return changed_copy(base, "trim()");
						case 3: return changed_copy(base, "reverse()");
						case 4: return base + ".substr(0, " + std::to_string(rng_.range(0, 3)) + ")";
						case 5: return changed_copy(base, "repeat(" + std::to_string(rng_.range(0, 3)) + ")");
						case 6: {
							const std::string to = string_literal();
							return changed_copy(base, "replace(" + string_literal() + ", " + to + ")");
						}
						default: return changed_copy(base, "pad_left(" + std::to_string(rng_.range(0, 6)) + ", \"*\")");
					}
				}
				case 4: return "to_string(" + expr(rng_.chance(50) ? vt::i : vt::f, depth - 1) + ")";
				default: {
					std::string c = call_returning(vt::s);
					return c.empty() ? string_literal() : c;
				}
			}
		}

		// an in-place builtin returns nothing: the value is a copy the method changed, through a by-value parameter
		static std::string changed_copy(const std::string& base, const std::string& call) {
			return "((t) => { t." + call + "; return t; })(" + base + ")";
		}

		std::string array_expr(int depth) {
			if (depth <= 0 || rng_.chance(40)) {
				if (const var_info* v = random_var(vt::arr)) if (rng_.chance(70)) return v->name;
				int n = static_cast<int>(rng_.range(1, 4));
				std::string out = "[";
				vt elem = rng_.chance(70) ? vt::i : (rng_.chance(50) ? vt::s : vt::f);
				for (int i = 0; i < n; ++i) {
					if (i) out += ", ";
					out += expr(elem, 0);
				}
				return out + "]";
			}
			if (const var_info* a = random_var(vt::arr)) {
				switch (rng_.below(3)) {
					case 0: return changed_copy(a->name, "slice(0, " + std::to_string(rng_.range(0, 2)) + ")");
					case 1: return changed_copy(a->name, "reverse()");
					default: return a->name;
				}
			}
			if (const var_info* s = random_var(vt::s)) return s->name + ".split(\",\")";
			return array_expr(0);
		}

		std::string typed_array_decl(const std::string& name) {
			if (!classes_.empty() && rng_.chance(25)) {
				// class-typed buffer (kind obj): same class for every element - the exact-class
				// rule makes any other class (or a subclass) an error-parity draw later
				const class_info& ci = classes_[rng_.below(static_cast<uint32_t>(classes_.size()))];
				int n = static_cast<int>(rng_.range(0, 4));
				std::string out = "array<" + ci.name + "> " + name + " = [";
				for (int i = 0; i < n; ++i) {
					if (i) out += ", ";
					int arity = ci.ctor_arities.empty() ? 0 : ci.ctor_arities[rng_.below(static_cast<uint32_t>(ci.ctor_arities.size()))];
					out += ci.name + "(";
					for (int a = 0; a < arity; ++a) { if (a) out += ", "; out += int_literal(); }
					out += ")";
				}
				return out + "];";
			}
			const uint32_t pick = rng_.below(10);
			// the one-byte slot splits three ways (char, uint8, int8) on a value derived from the seed and the name count, so
			// the random stream, and with it every other seed's program, stays what it was before the byte buffers existed
			const unsigned byte_slot = pick < 9 ? 0u : static_cast<unsigned>((((seed_ ^ 0x9E3779B97F4A7C15ull) * (names_ + 1)) >> 33) % 3);
			const char* elem_type = pick < 4 ? "int" : pick < 6 ? "float" : pick < 8 ? "string" : pick < 9 ? "bool" : byte_slot == 0 ? "char" : byte_slot == 1 ? "uint8" : "int8";
			int n = static_cast<int>(rng_.range(0, 4));
			std::string out = std::string("array<") + elem_type + "> " + name + " = [";
			for (int i = 0; i < n; ++i) {
				if (i) out += ", ";
				switch (pick < 4 ? 0 : pick < 6 ? 1 : pick < 8 ? 2 : pick < 9 ? 3 : 4) {
					case 0: out += int_literal(); break;
					case 1: out += float_literal(); break;
					case 2: out += string_literal(); break;
					case 3: out += rng_.chance(50) ? "true" : "false"; break;
					default: {
						// one draw per element whatever the kind; a byte buffer's values sit on its edges, one past an edge
						// now and then (error parity)
						static const int uint8_values[] = {0, 7, 128, 255, 256};
						static const int int8_values[] = {-128, -1, 0, 127, 128};
						const uint32_t d = rng_.below(5);
						if (byte_slot == 0) { out += std::string("'") + static_cast<char>('a' + d) + "'"; }
						else { out += std::to_string(byte_slot == 1 ? uint8_values[d] : int8_values[d]); }
						break;
					}
				}
			}
			return out + "];";
		}

		std::string map_expr(int depth) {
			if (const var_info* v = random_var(vt::map)) if (rng_.chance(50)) return v->name;
			(void)depth;
			int n = static_cast<int>(rng_.range(1, 3));
			std::string out = "{";
			for (int i = 0; i < n; ++i) {
				if (i) out += ", ";
				out += "\"k" + std::to_string(i) + "\": " + expr(rng_.chance(70) ? vt::i : vt::s, 0);
			}
			return out + "}";
		}

		std::string obj_expr(int depth) {
			(void)depth;
			if (classes_.empty()) return int_literal();
			if (rng_.chance(55)) {
				if (const var_info* v = random_var(vt::obj)) return spell(v);
			}
			const class_info& ci = classes_[rng_.below(static_cast<uint32_t>(classes_.size()))];
			int arity = ci.ctor_arities.empty() ? 0 : ci.ctor_arities[rng_.below(static_cast<uint32_t>(ci.ctor_arities.size()))];
			std::string out = ci.name + "(";
			for (int i = 0; i < arity; ++i) {
				if (i) out += ", ";
				out += int_expr(1);
			}
			return out + ")";
		}

		// Call a previously declared function returning `want` ("" when none fits).
		std::string call_returning(vt want) {
			std::vector<const func_info*> fits;
			for (const auto& f : funcs_)
				if (!f.is_coroutine && (f.ret == want || (f.ret == vt::any && want == vt::i)) && !has_ref_param(f))
					fits.push_back(&f);
			if (fits.empty()) return {};
			const func_info* f = fits[rng_.below(static_cast<uint32_t>(fits.size()))];
			return call_text(*f);
		}
		static bool has_ref_param(const func_info& f) {
			for (const auto& p : f.params) if (p.is_ref) return true;
			return false;
		}
		std::string call_text(const func_info& f) {
			std::string out = f.name + "(";
			for (size_t i = 0; i < f.params.size(); ++i) {
				if (i) out += ", ";
				out += f.params[i].class_idx >= 0 ? obj_of_class(f.params[i].class_idx) : expr(f.params[i].type, 1);
			}
			return out + ")";
		}
		// An object of exactly this class (a variable in scope, else a construction) - mostly;
		// one draw in eight hands over any object so the exact-class rejection stays exercised
		std::string obj_of_class(int class_idx) {
			if (class_idx < 0 || static_cast<size_t>(class_idx) >= classes_.size() || rng_.chance(12)) { return obj_expr(1); }
			if (rng_.chance(60)) {
				std::vector<const var_info*> same;
				for (const auto& scope : scopes_) {
					for (const auto& v : scope) { if (v.type == vt::obj && v.class_idx == class_idx) { same.push_back(&v); } }
				}
				if (!same.empty()) { return spell(same[rng_.below(static_cast<uint32_t>(same.size()))]); }
			}
			const class_info& ci = classes_[static_cast<size_t>(class_idx)];
			int arity = ci.ctor_arities.empty() ? 0 : ci.ctor_arities[rng_.below(static_cast<uint32_t>(ci.ctor_arities.size()))];
			std::string out = ci.name + "(";
			for (int i = 0; i < arity; ++i) { if (i) out += ", "; out += int_expr(1); }
			return out + ")";
		}

		// An int lvalue (plain var / obj int field / array element); declares a
		// temp when nothing suitable is in scope. Used for ref args and ++/--.
		std::string int_lvalue() {
			std::vector<std::string> opts;
			if (const var_info* v = random_var(vt::i)) opts.push_back(v->name);
			if (const var_info* o = random_var(vt::obj)) {
				const class_info& ci = classes_[o->class_idx];
				for (const auto& fld : ci.fields)
					if (fld.second == vt::i) { opts.push_back(o->name + "." + fld.first); break; }
				for (const auto& fld : ci.fields)
					if (fld.second == vt::arr) { opts.push_back(o->name + "." + fld.first + "[" + std::to_string(rng_.range(0, 2)) + "]"); break; }
			}
			if (const var_info* a = random_var(vt::arr)) {
				opts.push_back(a->name + "[" + std::to_string(rng_.range(0, 2)) + "]");
				if (rng_.chance(25)) {
					if (const var_info* iv = random_var(vt::i))
						opts.push_back(a->name + "[" + iv->name + " % 2]");
				}
			}
			if (!opts.empty()) return opts[rng_.below(static_cast<uint32_t>(opts.size()))];
			std::string t = fresh("t");
			lines_.push_back("int " + t + " = " + int_literal() + ";");
			declare(t, vt::i);
			return t;
		}

		// ---------- classes ----------
		void emit_class() {
			class_info ci;
			ci.name = fresh("C");
			int base = -1;
			if (!classes_.empty() && rng_.chance(45)) base = static_cast<int>(rng_.below(static_cast<uint32_t>(classes_.size())));
			ci.base_idx = base;

			std::string header = "class " + ci.name;
			if (base >= 0) header += " : " + classes_[base].name;
			lines_.push_back(header + " {");

			if (base >= 0) { // inherit flattened field/method view
				ci.fields = classes_[base].fields;
				ci.methods = classes_[base].methods;
				ci.coroutine_method = classes_[base].coroutine_method;
				ci.typed_int_field = classes_[base].typed_int_field;
			}

			int n_fields = static_cast<int>(rng_.range(1, 3));
			std::string first_int_field;
			for (int i = 0; i < n_fields; ++i) {
				std::string fname = fresh("m");
				const int field_kind = pick_weighted({45, 15, 15, 10, 15});
				if (!(field_kind == 0 && first_int_field.empty()) && names_ % 4 == 0) { fname = keyword_member_name(ci, "m"); }
				switch (field_kind) {
					case 0:
						lines_.push_back("int " + fname + " = " + std::to_string(rng_.range(0, 20)) + ";");
						ci.fields.push_back({fname, vt::i});
						if (first_int_field.empty()) first_int_field = fname;
						if (ci.typed_int_field.empty()) ci.typed_int_field = fname;
						break;
					case 1:
						lines_.push_back("string " + fname + " = " + string_literal() + ";");
						ci.fields.push_back({fname, vt::s});
						break;
					case 2:
						lines_.push_back("var " + fname + " = " + std::to_string(rng_.range(0, 9)) + ";");
						ci.fields.push_back({fname, vt::i});
						break;
					case 3: { // array field: member-base subscripts read/store through it
						const auto third = rng_.range(0, 9);
						const auto second = rng_.range(0, 9);
						const auto first = rng_.range(0, 9);
						lines_.push_back("var " + fname + " = [" + std::to_string(first) + ", " +
						                 std::to_string(second) + ", " + std::to_string(third) + "];");
						ci.fields.push_back({fname, vt::arr});
						break;
					}
					default:
						lines_.push_back("auto " + fname + " = " + float_literal() + ";");
						ci.fields.push_back({fname, vt::f});
						break;
				}
			}
			if (first_int_field.empty()) {
				first_int_field = fresh("m");
				lines_.push_back("int " + first_int_field + " = 1;");
				ci.fields.push_back({first_int_field, vt::i});
				if (ci.typed_int_field.empty()) ci.typed_int_field = first_int_field;
			}

			if (rng_.chance(35)) {
				std::string sname = fresh("s");
				lines_.push_back("static int " + sname + " = " + std::to_string(rng_.range(0, 100)) + ";");
				ci.statics.push_back(sname);
			}

			// Constructors
			bool base_needs_super = base >= 0 && !classes_[base].ctor_arities.empty() &&
				classes_[base].ctor_arities[0] != 0;
			auto super_call = [&]() -> std::string {
				if (base < 0) return {};
				const auto& arities = classes_[base].ctor_arities;
				int a = arities.empty() ? 0 : arities[rng_.below(static_cast<uint32_t>(arities.size()))];
				std::string s = " : super(";
				for (int i = 0; i < a; ++i) { if (i) s += ", "; s += std::to_string(rng_.range(0, 9)); }
				return s + ")";
			};
			int ctor_kind = pick_weighted({30, 30, 25, 15}); // none / (int) / both / delegating
			if (ctor_kind == 0 && !base_needs_super) {
				ci.ctor_arities.push_back(0); // implicit default ctor
			} else {
				bool used_super = base >= 0 && rng_.chance(70);
				std::string sup = used_super ? super_call() : std::string();
				if (ctor_kind == 3) { // delegation: C() : this(k) {}  +  C(int a)
					lines_.push_back(ci.name + "(int a)" + sup + " { " + first_int_field + " = a; }");
					lines_.push_back(ci.name + "() : this(" + std::to_string(rng_.range(1, 9)) + ") { }");
					ci.ctor_arities.push_back(0);
					ci.ctor_arities.push_back(1);
				} else if (ctor_kind == 2) {
					lines_.push_back(ci.name + "()" + sup + " { }");
					lines_.push_back(ci.name + "(int a)" + (used_super ? super_call() : std::string()) + " { " + first_int_field + " = a; }");
					ci.ctor_arities.push_back(0);
					ci.ctor_arities.push_back(1);
				} else {
					lines_.push_back(ci.name + "(int a)" + sup + " { " + first_int_field + " = a; }");
					ci.ctor_arities.push_back(1); // explicit ctor: no implicit default
				}
				if (rng_.chance(20)) { // ctor that throws
					lines_.push_back(ci.name + "(string boom) { throw \"ctor-boom-" + ci.name + "\"; }");
				}
			}

			// Methods
			{
				std::string g = fresh("get");
				lines_.push_back("int " + g + "() { return " + first_int_field + "; }");
				ci.methods.push_back({g, vt::i, 0});
			}
			if (rng_.chance(60)) {
				std::string a = fresh("add");
				lines_.push_back("void " + a + "(int n) { " + first_int_field + " = " + first_int_field + " + n; }");
				ci.methods.push_back({a, vt::any, 1});
			}
			if (rng_.chance(30)) {
				std::string d = names_ % 4 == 0 ? keyword_member_name(ci, "desc") : fresh("desc");
				lines_.push_back("string " + d + "() { return \"<\" + to_string(" + first_int_field + ") + \">\"; }");
				ci.methods.push_back({d, vt::s, 0});
			}

			// Coroutine method: yields this-fields across suspensions, sometimes with a
			// float final return (typed conversion) or a field mutation mid-flight
			if (rng_.chance(30)) {
				std::string cm = fresh("cog");
				ci.coroutine_method = cm;
				lines_.push_back("coroutine int " + cm + "() {");
				lines_.push_back("yield " + first_int_field + ";");
				if (rng_.chance(50)) lines_.push_back(first_int_field + " = " + first_int_field + " + 1;");
				lines_.push_back("yield " + first_int_field + " + " + std::to_string(rng_.range(1, 5)) + ";");
				if (rng_.chance(30)) lines_.push_back("return " + float_literal() + ";");
				else lines_.push_back("return " + first_int_field + ";");
				lines_.push_back("}");
			}

			// Operator methods (throwing variants exercise the operator-throw surface;
			// the named-class-return spelling covers the typed-operator parse path that
			// only `auto operator+` / `bool operator<` shapes left blind)
			if (rng_.chance(35)) {
				ci.has_op_plus = true;
				if (rng_.chance(20))
					lines_.push_back("auto operator+(" + ci.name + " other) { throw \"op-plus-boom\"; }");
				else if (rng_.chance(35))
					lines_.push_back(ci.name + " operator+(" + ci.name + " other) { var r = other; r." + first_int_field + " = " + first_int_field + " + other." + first_int_field + "; return r; }");
				else
					lines_.push_back("auto operator+(" + ci.name + " other) { return " + first_int_field + " + other." + first_int_field + "; }");
			}
			if (rng_.chance(30)) {
				ci.has_op_lt = true;
				if (rng_.chance(20))
					lines_.push_back("bool operator<(" + ci.name + " other) { throw \"op-lt-boom\"; }");
				else
					lines_.push_back("bool operator<(" + ci.name + " other) { return " + first_int_field + " < other." + first_int_field + "; }");
			}

			lines_.push_back("}");
			classes_.push_back(std::move(ci));
		}

		void emit_class_redefinition() {
			// Redefine an existing class with a compatible-but-different shape;
			// instances created in segment 1 must migrate.
			const class_info& old = classes_[rng_.below(static_cast<uint32_t>(classes_.size()))];
			std::string keep_field, keep_method;
			for (const auto& f : old.fields) if (f.second == vt::i) { keep_field = f.first; break; }
			for (const auto& m : old.methods) if (m.ret == vt::i && m.arity == 0) { keep_method = m.name; break; }
			lines_.push_back("class " + old.name + " {");
			if (!keep_field.empty())
				lines_.push_back("int " + keep_field + " = " + std::to_string(rng_.range(0, 9)) + ";");
			std::string newf = fresh("m");
			lines_.push_back("int " + newf + " = " + std::to_string(rng_.range(50, 99)) + ";");
			if (!keep_method.empty() && !keep_field.empty())
				lines_.push_back("int " + keep_method + "() { return " + bare_member(keep_field) + " + " + newf + "; }");
			lines_.push_back("}");
		}

		void emit_function_redefinition() {
			// Redefine an existing non-coroutine function with the same signature shape.
			std::vector<const func_info*> fits;
			for (const auto& f : funcs_) if (!f.is_coroutine) fits.push_back(&f);
			if (fits.empty()) return;
			const func_info& f = *fits[rng_.below(static_cast<uint32_t>(fits.size()))];
			std::string header = "function " + f.name + "(";
			for (size_t i = 0; i < f.params.size(); ++i) {
				if (i) header += ", ";
				header += f.params[i].type_text + " " + f.params[i].name;
			}
			header += ")";
			lines_.push_back(header + " {");
			if (f.ret == vt::s) lines_.push_back("return \"reloaded\";");
			else if (f.ret == vt::f) lines_.push_back("return 9.5;");
			else lines_.push_back("return " + std::to_string(rng_.range(500, 999)) + ";");
			lines_.push_back("}");
		}

		// ---------- functions ----------
		void emit_function() {
			switch (pick_weighted({30, 15, 18, 12, 25, classes_.empty() ? 0u : 25u})) {
				case 0: emit_simple_function(); break;
				case 1: emit_recursive_function(); break;
				case 2: emit_ref_mutator(); break;
				case 3: emit_var_ref_mutator(); break;
				case 4: emit_coroutine(); break;
				default: emit_object_reader(); break;
			}
		}

		// A function taking an object of a generated class BY VALUE, its body drawn from the
		// shapes the read-only parameter elision proves (field reads, read-only builtins on
		// fields) and refuses (field stores, self comparison, aliasing, passing it on, a
		// global write): proven and refused callees must agree with the elision on and off
		void emit_object_reader() {
			if (classes_.empty()) { emit_simple_function(); return; }
			func_info f;
			f.name = fresh("rd");
			f.ret = vt::i;
			param_info p; p.name = fresh("o"); p.type = vt::obj;
			p.class_idx = static_cast<int>(rng_.below(static_cast<uint32_t>(classes_.size())));
			const class_info& ci = classes_[static_cast<size_t>(p.class_idx)];
			p.type_text = ci.name;
			f.params.push_back(p);
			std::string ifield;
			for (const auto& [fname, ft] : ci.fields) { if (ft == vt::i) { ifield = fname; break; } }
			if (ifield.empty()) { ifield = ci.typed_int_field; }
			lines_.push_back("function " + f.name + "(" + ci.name + " " + p.name + ") -> int {");
			begin_function_scope(f.params);
			std::string acc = fresh("acc");
			lines_.push_back("int " + acc + " = " + int_expr(1) + ";");
			const int body = static_cast<int>(rng_.range(1, 3));
			for (int i = 0; i < body; ++i) {
				switch (pick_weighted({35, ifield.empty() ? 0u : 15u, 10, 10, 10, 10, 10})) {
					case 0: if (!ifield.empty()) { lines_.push_back(acc + " = " + acc + " + " + p.name + "." + ifield + ";"); } break;
					case 1: lines_.push_back(p.name + "." + ifield + " = " + p.name + "." + ifield + " + " + std::to_string(rng_.range(1, 5)) + ";"); break;
					case 2: lines_.push_back("if (" + p.name + " == " + p.name + ") { " + acc + " = " + acc + " + 1; }"); break;
					case 3: if (!ifield.empty()) { lines_.push_back("var& " + fresh("al") + " = " + p.name + "; " + acc + " = " + acc + " + " + p.name + "." + ifield + ";"); } break;
					case 4: {
						const func_info* other = nullptr;
						for (const auto& g : funcs_) { if (g.params.size() == 1 && g.params[0].class_idx == p.class_idx && g.ret == vt::i) { other = &g; } }
						if (other) { lines_.push_back(acc + " = " + acc + " + " + other->name + "(" + p.name + ");"); }
						break;
					}
					case 5: if (!ci.methods.empty() && ci.methods[0].arity == 0) { lines_.push_back(acc + " = " + acc + " + " + p.name + "." + ci.methods[0].name + "();"); } break;
					default: lines_.push_back(acc + " = " + acc + " + " + int_expr(1) + ";"); break;
				}
			}
			lines_.push_back("return " + acc + ";");
			end_function_scope();
			lines_.push_back("}");
			funcs_.push_back(std::move(f));
		}

		std::vector<param_info> gen_params(int max_params, bool allow_ref) {
			std::vector<param_info> params;
			int n = static_cast<int>(rng_.range(0, max_params));
			for (int i = 0; i < n; ++i) {
				param_info p;
				p.name = fresh("p");
				switch (pick_weighted({40, 15, 15, 10, allow_ref ? 20u : 0u, classes_.empty() ? 0u : 15u})) {
					case 0: p.type_text = "int"; p.type = vt::i; break;
					case 1: p.type_text = "auto"; p.type = vt::i; break;
					case 2: p.type_text = "var"; p.type = vt::any; break;
					case 3: p.type_text = "string"; p.type = vt::s; break;
					case 4: p.type_text = "int&"; p.type = vt::i; p.is_ref = true; break;
					default: {
						// By-value class parameter: bodies read fields, compare, mutate or pass it on,
						// so the proven and refused shapes of the read-only elision both arise
						p.class_idx = static_cast<int>(rng_.below(static_cast<uint32_t>(classes_.size())));
						p.type_text = classes_[static_cast<size_t>(p.class_idx)].name; p.type = vt::obj;
						break;
					}
				}
				params.push_back(std::move(p));
			}
			return params;
		}

		void begin_function_scope(const std::vector<param_info>& params) {
			scopes_.push_back({});
			for (const auto& p : params) declare(p.name, p.type == vt::any ? vt::i : p.type, p.class_idx);
			in_function_ = true;
		}
		void end_function_scope() {
			scopes_.pop_back();
			in_function_ = false;
		}

		std::string params_text(const std::vector<param_info>& params) {
			std::string out;
			for (size_t i = 0; i < params.size(); ++i) {
				if (i) out += ", ";
				out += params[i].type_text + " " + params[i].name;
			}
			return out;
		}

		void emit_simple_function() {
			func_info f;
			f.name = fresh("fn");
			f.params = gen_params(3, true);
			int rk = pick_weighted({50, 20, 20, 10});
			f.ret = rk == 0 ? vt::i : rk == 1 ? vt::s : rk == 2 ? vt::f : vt::b;
			const char* ret_text = rk == 0 ? "int" : rk == 1 ? "string" : rk == 2 ? "float" : "bool";
			lines_.push_back("function " + f.name + "(" + params_text(f.params) + ") -> " + ret_text + " {");
			begin_function_scope(f.params);
			int body = static_cast<int>(rng_.range(1, 3));
			vt saved_ret = current_ret_;
			current_ret_ = f.ret;
			for (int i = 0; i < body; ++i) emit_stmt(1);
			lines_.push_back("return " + expr(f.ret, 2) + ";");
			current_ret_ = saved_ret;
			end_function_scope();
			lines_.push_back("}");
			funcs_.push_back(std::move(f));
		}

		void emit_recursive_function() {
			func_info f;
			f.name = fresh("rec");
			f.ret = vt::i;
			param_info p; p.name = fresh("n"); p.type_text = "int"; p.type = vt::i;
			f.params.push_back(p);
			lines_.push_back("function " + f.name + "(int " + p.name + ") -> int {");
			lines_.push_back("if (" + p.name + " <= 0) {");
			lines_.push_back("return " + std::to_string(rng_.range(0, 3)) + ";");
			lines_.push_back("}");
			lines_.push_back("return " + p.name + " + " + f.name + "(" + p.name + " - 1);");
			lines_.push_back("}");
			funcs_.push_back(std::move(f));
		}

		void emit_ref_mutator() {
			func_info f;
			f.name = fresh("bump");
			f.ret = vt::any;
			param_info p; p.name = fresh("x"); p.type_text = "int&"; p.type = vt::i; p.is_ref = true;
			f.params.push_back(p);
			lines_.push_back("function " + f.name + "(int& " + p.name + ") {");
			if (rng_.chance(30))
				lines_.push_back(p.name + " += " + std::to_string(rng_.range(1, 9)) + ";");
			else
				lines_.push_back(p.name + " = " + p.name + " + " + std::to_string(rng_.range(1, 9)) + ";");
			lines_.push_back("}");
			funcs_.push_back(std::move(f));
		}

		void emit_var_ref_mutator() {
			func_info f;
			f.name = fresh("vset");
			f.ret = vt::any;
			param_info p; p.name = fresh("x"); p.type_text = "var&"; p.type = vt::any; p.is_ref = true;
			f.params.push_back(p);
			lines_.push_back("function " + f.name + "(var& " + p.name + ") {");
			lines_.push_back(p.name + " = " + expr(vt::any, 1) + ";");
			lines_.push_back("}");
			funcs_.push_back(std::move(f));
		}

		void emit_coroutine() {
			func_info f;
			f.name = fresh("co");
			f.ret = vt::i;
			f.is_coroutine = true;
			bool takes_arg = rng_.chance(60);
			std::string pname;
			if (takes_arg) {
				param_info p; p.name = fresh("a"); p.type_text = "int"; p.type = vt::i;
				pname = p.name;
				f.params.push_back(p);
			}
			lines_.push_back("coroutine int " + f.name + "(" + (takes_arg ? "int " + pname : std::string()) + ") {");
			std::string acc = takes_arg ? pname : int_literal();
			int yields = static_cast<int>(rng_.range(1, 3));
			for (int i = 0; i < yields; ++i) {
				// statement-level yields plus yields in sub-expression positions (argument,
				// operand, condition): the backends must agree on where each one suspends
				switch (rng_.range(0, 4)) {
					case 1:
						lines_.push_back("int y" + std::to_string(i) + " = ((yield " + acc + " + " + std::to_string(i) +
						                 ") == null ? 1 : 0) + " + acc + ";");
						break;
					case 2:
						lines_.push_back("int y" + std::to_string(i) + " = to_string(yield " + acc + " + " + std::to_string(i) +
						                 ").size();");
						break;
					case 3:
						lines_.push_back("if ((yield " + acc + " + " + std::to_string(i) + ") == null) { " + acc + " = " + acc + " + 1; }");
						break;
					default:
						lines_.push_back("yield " + acc + " + " + std::to_string(i) + ";");
						break;
				}
			}
			if (rng_.chance(70)) {
				// typed final return, sometimes a float needing conversion (spicy)
				if (rng_.chance(25)) lines_.push_back("return " + float_literal() + ";");
				else lines_.push_back("return " + acc + " * 2;");
			}
			lines_.push_back("}");
			funcs_.push_back(std::move(f));
		}

		// ---------- statements ----------
		// block shapes: the statements run inside a function (the vm translates no program-scope loop); their declarations stay
		// out of the global scope, so no read takes the ::name spelling
		size_t begin_statements() {
			wrapped_ = blocks_on_;
			if (wrapped_) scopes_.push_back({});
			return lines_.size();
		}
		void end_statements(size_t begin, const char* fn, const std::string& result) {
			if (wrapped_) {
				lines_.push_back("return " + result + ";");
				const std::vector<std::string> first = block_.loop({});   // ahead of the statements: a program that raises early (many do) still runs a block
				lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(begin), first.begin(), first.end());
				lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(begin), std::string("function ") + fn + "() {");
				lines_.push_back("}");
				lines_.push_back(std::string(fn) + "();");
				scopes_.pop_back();
				wrapped_ = false;
			} else {
				lines_.push_back(result + ";");
			}
			if (blocks_on_) {   // the program-scope declarations this segment's loops added
				const auto& pre = block_.prelude();
				lines_.insert(lines_.begin(), pre.begin() + static_cast<std::ptrdiff_t>(prelude_emitted_), pre.end());
				prelude_emitted_ = pre.size();
			}
		}

		void emit_block_loop() {
			std::vector<std::string> outer;
			for (const auto& scope : scopes_)
				for (const auto& v : scope)
					if (v.type == vt::i && !v.is_var_decl) outer.push_back(v.name);
			for (auto& l : block_.loop(outer)) lines_.push_back(std::move(l));
		}

		void emit_stmt(int nesting) {
			if (blocks_on_ && (in_function_ || wrapped_) && block_.loop_here()) emit_block_loop();
			if (nesting > 3) { emit_leaf_stmt(); return; }
			switch (pick_weighted({26, 12, 8, 7, 7, 6, 6, 5, 5, 5, 4, 4, 5})) {
				case 0: emit_leaf_stmt(); break;
				case 1: emit_decl(); break;
				case 2: emit_if(nesting); break;
				case 3: emit_cfor(nesting); break;
				case 4: emit_range_for(nesting); break;
				case 5: emit_while(nesting); break;
				case 6: emit_try_catch(nesting); break;
				case 7: emit_switch(nesting); break;
				case 8: emit_lambda_stmt(); break;
				case 9: emit_coroutine_drive(); break;
				case 10: emit_container_mutation(); break;
				case 11: emit_ref_call(); break;
				default: emit_object_stmt(); break;
			}
		}

		void emit_leaf_stmt() {
			switch (pick_weighted({25, 22, 14, 12, 10, 9, 8})) {
				case 0: emit_decl(); break;
				case 1: { // assignment (occasionally deliberately ill-typed: matching errors = agreement)
					const var_info* v = nullptr;
					for (int tries = 0; tries < 3 && !v; ++tries) {
						auto t = random_value_type();
						if (t == vt::obj) t = vt::i;
						v = random_var(t);
					}
					if (!v) { emit_decl(); break; }
					vt rhs_t = v->type;
					if (v->is_var_decl && rng_.chance(35)) rhs_t = vt::any; // var may retype
					else if (rng_.chance(5)) rhs_t = (v->type == vt::s) ? vt::i : vt::s; // type-ladder violation
					lines_.push_back(v->name + " = " + expr(rhs_t, max_depth_ - 1) + ";");
					break;
				}
				case 2: { // compound assign
					if (const var_info* v = random_var(rng_.chance(70) ? vt::i : vt::s)) {
						if (v->type == vt::i) {
							static constexpr const char* const ops[] = {"+=", "-=", "*=", "/=", "%="};
							const char* op = ops[pick_weighted({40, 25, 20, 8, 7})];
							std::string rhs = int_expr(1);
							if (op[0] == '/' || op[0] == '%') rhs = "(" + rhs + " + 3)";
							lines_.push_back(v->name + " " + op + " " + rhs + ";");
						} else {
							lines_.push_back(v->name + " += " + string_expr(1) + ";");
						}
					} else {
						emit_decl();
					}
					break;
				}
				case 3: { // print
					int n = static_cast<int>(rng_.range(1, 3));
					std::string out = "print(";
					for (int i = 0; i < n; ++i) {
						if (i) out += ", ";
						out += expr(vt::any, 2);
					}
					lines_.push_back(out + ");");
					break;
				}
				case 4: { // ++/--
					std::string lv = int_lvalue();
					static constexpr const char* const forms[] = {"++%;", "--%;", "%++;", "%--;"};
					std::string f = forms[rng_.below(4)];
					size_t pos = f.find('%');
					lines_.push_back(f.substr(0, pos) + lv + f.substr(pos + 1));
					break;
				}
				case 5: { // call as statement
					if (!funcs_.empty()) {
						const func_info& f = funcs_[rng_.below(static_cast<uint32_t>(funcs_.size()))];
						if (!f.is_coroutine && !has_ref_param(f)) {
							lines_.push_back(call_text(f) + ";");
							break;
						}
					}
					emit_ref_call();
					break;
				}
				default: emit_object_stmt(); break;
			}
		}

		void emit_decl() {
			std::string name = fresh("v");
			switch (pick_weighted({22, 16, 16, 10, 10, 10, 8, 8})) {
				case 0:
					lines_.push_back("int " + name + " = " + int_expr(max_depth_ - 1) + ";");
					declare(name, vt::i);
					break;
				case 1: {
					vt t = random_value_type();
					if (t == vt::obj && classes_.empty()) t = vt::i;
					lines_.push_back("auto " + name + " = " + expr(t, max_depth_ - 1) + ";");
					int cidx = -1;
					if (t == vt::obj) cidx = last_obj_class_;
					declare(name, t, cidx);
					break;
				}
				case 2: {
					vt t = random_value_type();
					if (t == vt::obj && classes_.empty()) t = vt::i;
					lines_.push_back("var " + name + " = " + expr(t, max_depth_ - 1) + ";");
					declare(name, t, t == vt::obj ? last_obj_class_ : -1, true);
					break;
				}
				case 3:
					lines_.push_back("string " + name + " = " + string_expr(max_depth_ - 1) + ";");
					declare(name, vt::s);
					break;
				case 4:
					lines_.push_back("double " + name + " = " + float_expr(max_depth_ - 1) + ";");
					declare(name, vt::f);
					break;
				case 5:
					if (rng_.chance(50)) {
						// Typed buffer declarations (the representation rule): every
						// primitive element kind, uniform literals; later pushes/stores of other
						// types are error-parity draws, not noise
						lines_.push_back(typed_array_decl(name));
					} else {
						lines_.push_back("auto " + name + " = " + array_expr(2) + ";");
					}
					declare(name, vt::arr);
					break;
				case 6:
					lines_.push_back("auto " + name + " = " + map_expr(1) + ";");
					declare(name, vt::map);
					break;
				default:
					if (!classes_.empty()) {
						int cidx = static_cast<int>(rng_.below(static_cast<uint32_t>(classes_.size())));
						const class_info& ci = classes_[cidx];
						int arity = ci.ctor_arities.empty() ? 0 : ci.ctor_arities[rng_.below(static_cast<uint32_t>(ci.ctor_arities.size()))];
						std::string args;
						for (int i = 0; i < arity; ++i) { if (i) args += ", "; args += int_expr(1); }
						lines_.push_back("auto " + name + " = " + ci.name + "(" + args + ");");
						declare(name, vt::obj, cidx);
						last_obj_class_ = cidx;
					} else {
						lines_.push_back("bool " + name + " = " + bool_expr(2) + ";");
						declare(name, vt::b);
					}
					break;
			}
		}
		int last_obj_class_ = 0;

		void emit_block(int nesting, int min_stmts = 1, int max_stmts = 3) {
			scopes_.push_back({});
			int n = static_cast<int>(rng_.range(min_stmts, max_stmts));
			for (int i = 0; i < n; ++i) emit_stmt(nesting + 1);
			scopes_.pop_back();
		}

		void emit_if(int nesting) {
			lines_.push_back("if (" + bool_expr(max_depth_ - 1) + ") {");
			emit_block(nesting);
			if (rng_.chance(45)) {
				lines_.push_back("} else {");
				emit_block(nesting);
			}
			lines_.push_back("}");
		}

		void emit_cfor(int nesting) {
			std::string iv = fresh("i");
			std::string bound = std::to_string(rng_.range(1, 6));
			std::string step = rng_.chance(70) ? iv + "++" : "++" + iv;
			lines_.push_back("for (int " + iv + " = 0; " + iv + " < " + bound + "; " + step + ") {");
			scopes_.push_back({});
			declare(iv, vt::i);
			++loop_depth_;
			int n = static_cast<int>(rng_.range(1, 3));
			for (int i = 0; i < n; ++i) emit_stmt(nesting + 1);
			// spice: redeclare an outer variable inside the loop body (one the body itself declared would be a redeclaration error)
			if (rng_.chance(18)) {
				const var_info* outer = random_var(vt::i);
				const auto& body = scopes_.back();
				if (outer && std::none_of(body.begin(), body.end(), [&](const var_info& own) { return own.name == outer->name; }))
					lines_.push_back("var " + outer->name + " = " + int_expr(1) + ";");
			}
			// spice: capture the loop var in a lambda, call it in-body
			if (rng_.chance(15)) {
				std::string lam = fresh("lam");
				lines_.push_back("auto " + lam + " = [=]() -> auto { return " + iv + " * 10; };");
				lines_.push_back("print(" + lam + "());");
			}
			if (loop_depth_ > 0 && rng_.chance(12)) lines_.push_back(rng_.chance(50) ? "break;" : "continue;");
			--loop_depth_;
			scopes_.pop_back();
			lines_.push_back("}");
		}

		void emit_range_for(int nesting) {
			const var_info* arr = random_var(vt::arr);
			std::string target;
			if (arr && rng_.chance(70)) target = arr->name;
			else target = array_expr(1);
			bool by_ref = rng_.chance(30) && !target.empty() && target[0] != '[';
			std::string ev = fresh("e");
			lines_.push_back("for (auto" + std::string(by_ref ? "&" : "") + " " + ev + " : " + target + ") {");
			scopes_.push_back({});
			declare(ev, vt::any);
			++loop_depth_;
			if (by_ref) lines_.push_back(ev + " = " + ev + " + " + std::to_string(rng_.range(1, 5)) + ";");
			else lines_.push_back("print(" + ev + ");");
			if (rng_.chance(35)) emit_stmt(nesting + 1);
			--loop_depth_;
			scopes_.pop_back();
			lines_.push_back("}");
			if (by_ref && arr) lines_.push_back("print(" + arr->name + "[0]);");
			// map iteration variant
			if (rng_.chance(25)) {
				if (const var_info* m = random_var(vt::map)) {
					std::string kv = fresh("kv");
					lines_.push_back("for (auto " + kv + " : " + m->name + ") {");
					lines_.push_back("print(" + kv + ".first, " + kv + ".second);");
					lines_.push_back("}");
				}
			}
		}

		void emit_while(int nesting) {
			std::string g = fresh("g");
			lines_.push_back("int " + g + " = 0;");
			declare(g, vt::i);
			if (rng_.chance(40)) {
				// ++ in the condition
				lines_.push_back("while (++" + g + " < " + std::to_string(rng_.range(2, 7)) + ") {");
				scopes_.push_back({});
				++loop_depth_;
				emit_stmt(nesting + 1);
				--loop_depth_;
				scopes_.pop_back();
				lines_.push_back("}");
			} else {
				const auto limit = rng_.range(2, 7);
				lines_.push_back("while (" + bool_expr(2) + " && " + g + " < " + std::to_string(limit) + ") {");
				scopes_.push_back({});
				++loop_depth_;
				lines_.push_back(g + " = " + g + " + 1;");
				emit_stmt(nesting + 1);
				--loop_depth_;
				scopes_.pop_back();
				lines_.push_back("}");
			}
		}

		void emit_try_catch(int nesting) {
			lines_.push_back("try {");
			scopes_.push_back({});
			switch (pick_weighted({30, 20, 15, 20, 15})) {
				case 0:
					emit_stmt(nesting + 1);
					lines_.push_back("throw " + (rng_.chance(60) ? string_literal() : int_literal()) + ";");
					break;
				case 1: { // overflow edge inside try
					const std::string add = int_expr(1);
					lines_.push_back("int " + fresh("ov") + " = 9223372036854775807 + " + add + ";");
					break;
				}
				case 2: // INT64_MIN / -1
					lines_.push_back("int " + fresh("dv") + " = (-9223372036854775807 - 1) / (-1);");
					break;
				case 3: { // throwing operator / ctor
					bool emitted = false;
					for (size_t c = 0; c < classes_.size(); ++c) {
						if (classes_[c].has_op_plus) {
							std::string a = fresh("oa"), b2 = fresh("ob");
							int arity = classes_[c].ctor_arities.empty() ? 0 : classes_[c].ctor_arities[0];
							std::string args = arity == 1 ? std::to_string(rng_.range(0, 9)) : std::string();
							lines_.push_back("auto " + a + " = " + classes_[c].name + "(" + args + ");");
							lines_.push_back("auto " + b2 + " = " + classes_[c].name + "(" + args + ");");
							if (rng_.chance(25)) // obj + primitive (mismatched operand)
								lines_.push_back("print(" + a + " + 1);");
							else
								lines_.push_back("print(" + a + " + " + b2 + ");");
							emitted = true;
							break;
						}
					}
					if (!emitted) {
						emit_stmt(nesting + 1);
						lines_.push_back("throw \"plain-boom\";");
					}
					break;
				}
				default:
					emit_stmt(nesting + 1);
					emit_stmt(nesting + 1);
					break;
			}
			scopes_.pop_back();
			std::string ex = fresh("ex");
			lines_.push_back("} catch (" + ex + ") {");
			scopes_.push_back({});
			declare(ex, vt::any);
			lines_.push_back("print(\"caught:\", " + ex + ");");
			scopes_.pop_back();
			lines_.push_back("}");
			// rare top-level uncaught throw at the very end of a program is added
			// by the driver via final expression errors instead (keeps programs alive).
		}

		void emit_switch(int nesting) {
			(void)nesting;
			std::string subject = random_var(vt::i) ? random_var(vt::i)->name : int_expr(1);
			lines_.push_back("switch ((" + subject + ") % 4) {");
			int n_cases = static_cast<int>(rng_.range(2, 4));
			for (int c = 0; c < n_cases; ++c) {
				lines_.push_back("case " + std::to_string(c) + ":");
				lines_.push_back("print(\"case" + std::to_string(c) + "\");");
				switch (pick_weighted({40, 35, 25})) {
					case 0: lines_.push_back("break;"); break;
					case 1: lines_.push_back("fallthrough;"); break;
					default: break; // break-by-default semantics
				}
			}
			if (rng_.chance(70)) {
				lines_.push_back("default:");
				lines_.push_back("print(\"default\");");
			}
			lines_.push_back("}");
		}

		void emit_lambda_stmt() {
			std::string lam = fresh("lam");
			const var_info* cap = random_var(vt::i);
			if (cap && rng_.chance(45)) {
				// [&] mutating capture
				lines_.push_back("auto " + lam + " = [&]() -> auto { " + cap->name + " = " + cap->name + " + 1; return " + cap->name + "; };");
				lines_.push_back("print(" + lam + "());");
				if (rng_.chance(50)) lines_.push_back("print(" + cap->name + ");");
			} else if (cap) {
				const char* capture = rng_.chance(50) ? "[=]" : "";
				std::string p = fresh("q");
				lines_.push_back("auto " + lam + " = " + capture + "(auto " + p + ") -> auto { return " + p + " + " + cap->name + "; };");
				lines_.push_back("print(" + lam + "(" + int_literal() + "));");
			} else {
				lines_.push_back("auto " + lam + " = [=](auto q) -> auto { return q * 2; };");
				lines_.push_back("print(" + lam + "(" + int_literal() + "));");
			}
		}

		void emit_coroutine_drive() {
			const func_info* co = nullptr;
			for (const auto& f : funcs_) if (f.is_coroutine) co = &f;
			if (!co) { emit_leaf_stmt(); return; }
			std::string args = co->params.empty() ? "" : int_expr(1);
			if (rng_.chance(45)) {
				// range-for drive
				std::string yv = fresh("y");
				lines_.push_back("for (auto " + yv + " : " + co->name + "(" + args + ")) {");
				lines_.push_back("print(" + yv + ");");
				lines_.push_back("}");
			} else {
				std::string h = fresh("h");
				lines_.push_back("auto " + h + " = " + co->name + "(" + args + ");");
				int resumes = static_cast<int>(rng_.range(1, 4));
				for (int i = 0; i < resumes; ++i) {
					std::string r = fresh("r");
					lines_.push_back("auto " + r + " = " + h + ".next();");
					lines_.push_back("print(" + r + ");");
				}
				declare(h, vt::any);
			}
		}

		void emit_container_mutation() {
			if (const var_info* a = random_var(vt::arr)) {
				switch (rng_.below(5)) {
					case 0: lines_.push_back(a->name + ".push(" + int_expr(1) + ");"); break;
					case 1: lines_.push_back(a->name + ".pop();"); break;
					case 2: {
						const std::string value = int_expr(1);
						lines_.push_back(a->name + "[" + std::to_string(rng_.range(0, 2)) + "] = " + value + ";");
						break;
					}
					case 3: lines_.push_back(a->name + ".sort();"); break;
					default: lines_.push_back("print(" + a->name + ".join(\",\"));"); break;
				}
				return;
			}
			if (const var_info* m = random_var(vt::map)) {
				switch (rng_.below(3)) {
					case 0: {
						const std::string value = expr(vt::any, 1);
						lines_.push_back(m->name + "[" + string_literal() + "] = " + value + ";");
						break;
					}
					case 1: lines_.push_back(m->name + ".erase(" + string_literal() + ");"); break;
					default: lines_.push_back("print(" + m->name + ".size());"); break;
				}
				return;
			}
			emit_decl();
		}

		void emit_ref_call() {
			const func_info* rf = nullptr;
			for (const auto& f : funcs_) if (has_ref_param(f)) rf = &f;
			if (!rf) { emit_decl(); return; }
			std::string args;
			for (size_t i = 0; i < rf->params.size(); ++i) {
				if (i) args += ", ";
				if (rf->params[i].is_ref) args += int_lvalue(); // plain var / obj.field / arr[i]
				else args += expr(rf->params[i].type, 1);
			}
			lines_.push_back(rf->name + "(" + args + ");");
			if (rng_.chance(50)) lines_.push_back("print(\"after-ref-call\");");
		}

		void emit_object_stmt() {
			const var_info* o = random_var(vt::obj);
			if (!o) { emit_decl(); return; }
			const class_info& ci = classes_[o->class_idx];
			switch (rng_.below(6)) {
				case 4: { // coroutine method drive (handle pins the instance)
					if (ci.coroutine_method.empty()) { break; }
					if (rng_.chance(40)) {
						std::string yv = fresh("y");
						lines_.push_back("for (auto " + yv + " : " + o->name + "." + ci.coroutine_method + "()) {");
						lines_.push_back("print(" + yv + ");");
						lines_.push_back("}");
					} else {
						std::string h = fresh("h");
						lines_.push_back("auto " + h + " = " + o->name + "." + ci.coroutine_method + "();");
						int resumes = static_cast<int>(rng_.range(1, 3));
						for (int i = 0; i < resumes; ++i) {
							std::string r = fresh("r");
							lines_.push_back("auto " + r + " = " + h + ".next();");
							lines_.push_back("print(" + r + ");");
						}
						lines_.push_back("print(" + h + ".done());");
						declare(h, vt::any);
					}
					return;
				}
				case 5: { // typed-field converting writes (int field: float assigns/compounds truncate)
					if (ci.typed_int_field.empty()) { break; }
					switch (rng_.below(3)) {
						case 0: lines_.push_back(o->name + "." + ci.typed_int_field + " = " + float_literal() + ";"); break;
						case 1: lines_.push_back(o->name + "." + ci.typed_int_field + " += " + float_literal() + ";"); break;
						default: lines_.push_back(o->name + "." + ci.typed_int_field + " %= " + std::to_string(rng_.range(2, 7)) + ";"); break;
					}
					lines_.push_back("print(" + o->name + "." + ci.typed_int_field + ");");
					return;
				}
				default: break;
			}
			switch (rng_.below(4)) {
				case 0: { // method call
					for (const auto& m : ci.methods) {
						if (m.arity == 1) { lines_.push_back(o->name + "." + m.name + "(" + int_expr(1) + ");"); return; }
					}
					if (!ci.methods.empty())
						lines_.push_back("print(" + o->name + "." + ci.methods[0].name + "());");
					return;
				}
				case 1: { // field assign
					for (const auto& f : ci.fields) {
						if (f.second == vt::i) { lines_.push_back(o->name + "." + f.first + " = " + int_expr(2) + ";"); return; }
					}
					return;
				}
				case 2: { // static assign / read
					if (!ci.statics.empty()) {
						lines_.push_back(ci.name + "::" + ci.statics[0] + " = " + int_expr(1) + ";");
						lines_.push_back("print(" + ci.name + "::" + ci.statics[0] + ");");
						return;
					}
					if (!ci.fields.empty())
						lines_.push_back("print(" + o->name + "." + ci.fields[0].first + ");");
					return;
				}
				default: { // print a field
					if (!ci.fields.empty())
						lines_.push_back("print(" + o->name + "." + ci.fields[rng_.below(static_cast<uint32_t>(ci.fields.size()))].first + ");");
					return;
				}
			}
		}
	};

	inline program generate_program(uint64_t seed, int max_expr_depth = 4, band_mode band = band_mode::never, band_mode math_mode = band_mode::never,
	                                band_mode call_args_mode = band_mode::never, band_mode blocks_mode = band_mode::never,
	                                band_mode local_alias_mode = band_mode::never, band_mode regex_mode = band_mode::never) {
		return generator(seed, max_expr_depth, band, math_mode, call_args_mode, blocks_mode, local_alias_mode, regex_mode).generate();
	}

} // namespace jai::fuzz
