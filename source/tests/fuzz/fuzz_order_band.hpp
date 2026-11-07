#pragma once

// Ordering band (jaiscript_fuzz --order-band): programs whose operands read storage that a call in a sibling operand writes (a
// global, a captured local, an array element, a map entry, a field), inside counted loops, while loops, straight-line spans of
// scalar statements and program scope, with small leaves the vm grafts. Three oracles run over each program: the backends agree
// (the harness), the program agrees with its metamorphic variant (every operand holding a call or a storage read evaluated into
// a fresh local, left to right, the right side of an assignment ahead of its target), and every run agrees with the output the
// model below computes from the language's rules (left-to-right operands, C++17 assignment sequencing, the assignment table,
// value semantics). The model walks the generator's own tree; it never runs the engine.

#include "fuzz_harness.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jai::fuzz::order {

	enum class ty : uint8_t { i, f, b, v, a, none, untyped, s };

	struct mval {
		char k = 'n';
		int64_t i = 0;
		double f = 0.0;
		bool b = false;
		std::string s;
		bool dyn = false;   // the value carries a var's declared type (read from var storage): an auto declaration from it is a var
	};
	inline mval mi(int64_t x) { mval m; m.k = 'i'; m.i = x; return m; }
	inline mval mf(double x) { mval m; m.k = 'f'; m.f = x; return m; }
	inline mval mb(bool x) { mval m; m.k = 'b'; m.b = x; return m; }
	inline mval ms(std::string x) { mval m; m.k = 's'; m.s = std::move(x); return m; }

	struct abstain { std::string why; };

	inline std::string float_text(double v) {
		char buf[64];
		auto r = std::to_chars(buf, buf + sizeof(buf), v);
		std::string s(buf, r.ptr);
		if (s.find_first_of(".en") == std::string::npos) { s += ".0"; }
		return s;
	}
	inline std::string value_text(const mval& v) {
		switch (v.k) {
		case 'i': return std::to_string(v.i);
		case 'f': return float_text(v.f);
		case 'b': return v.b ? "true" : "false";
		case 's': return v.s;
		default: return "null";
		}
	}
	inline std::string literal_text(const mval& v) {
		if (v.k == 's') return "\"" + v.s + "\"";
		std::string s = v.k == 'f' ? float_text(v.f) : value_text(v);
		return s[0] == '-' ? "(" + s + ")" : s;
	}
	inline const char* type_text(ty t) {
		switch (t) {
		case ty::i: return "int";
		case ty::f: return "float";
		case ty::b: return "bool";
		case ty::s: return "string";
		case ty::a: return "auto";
		case ty::none: return "void";
		default: return "var";
		}
	}

	struct expr;
	using eptr = std::shared_ptr<expr>;

	enum class pk : uint8_t { var, elem, map, field };
	struct place {
		pk kind = pk::var;
		bool global = true;
		std::string name;
		eptr index;
		std::string key;
		std::string field;
		bool bare = false;
	};

	enum class ek : uint8_t { lit, read, bin, neg, lnot, tern, call, math, alias };
	struct expr {
		ek kind = ek::lit;
		mval lit;
		place p;
		std::string op;
		eptr a, b, c;
		int callee = -1;
		std::vector<eptr> args;
		std::string recv;
		bool recv_global = true;
	};

	enum class sk : uint8_t { decl, assign, compound, eval, if_, for_, while_, print, ret, arr_decl, map_decl, obj_decl, lambda_decl };
	struct stmt;
	using sptr = std::shared_ptr<stmt>;
	struct stmt {
		sk kind = sk::eval;
		ty t = ty::v;
		std::string name;
		place target;
		std::string op;
		eptr e;
		std::vector<eptr> list;
		std::vector<std::string> keys;
		std::vector<sptr> body, other;
		int n = 0;
		std::string tag;
		int callee = -1;
		bool global = false;
	};

	struct param { std::string name; ty t = ty::i; bool ref = false; };
	enum class ck : uint8_t { function, lambda, method };
	struct callable {
		std::string name;
		ck kind = ck::function;
		bool by_ref = true;
		std::vector<param> params;
		ty ret = ty::i;
		std::vector<sptr> body;
		std::string cls;
		std::vector<place> writes;
		ty result = ty::i;
		bool writes_ref = false;
		std::string local;      // a typed local the body declares (a graft local: its landing converts)
		ty local_t = ty::i;
		bool ret_ref = false;   // `int& g() { return G; }`: the result is a reference the caller reads where the call stands
	};
	struct klass {
		std::string name;
		std::vector<std::pair<std::string, ty>> fields;
		std::vector<mval> init;
		std::vector<int> methods;
	};
	struct prog {
		std::vector<klass> classes;
		std::vector<sptr> globals;
		std::vector<callable> calls;
		std::vector<int> functions;
		std::vector<sptr> top;
	};

	inline eptr lit_e(const mval& v) { auto e = std::make_shared<expr>(); e->kind = ek::lit; e->lit = v; return e; }
	inline eptr read_e(const place& p) { auto e = std::make_shared<expr>(); e->kind = ek::read; e->p = p; return e; }
	inline eptr bin_e(const std::string& op, eptr a, eptr b) { auto e = std::make_shared<expr>(); e->kind = ek::bin; e->op = op; e->a = std::move(a); e->b = std::move(b); return e; }
	inline eptr tern_e(eptr c, eptr a, eptr b) { auto e = std::make_shared<expr>(); e->kind = ek::tern; e->a = std::move(c); e->b = std::move(a); e->c = std::move(b); return e; }
	inline eptr un_e(ek k, eptr a) { auto e = std::make_shared<expr>(); e->kind = k; e->a = std::move(a); return e; }
	inline eptr math_e(ek k, const std::string& fn, std::vector<eptr> args) { auto e = std::make_shared<expr>(); e->kind = k; e->op = fn; e->args = std::move(args); return e; }
	inline place var_p(const std::string& n, bool global) { place p; p.kind = pk::var; p.name = n; p.global = global; return p; }
	inline place elem_p(const std::string& n, bool global, eptr idx) { place p; p.kind = pk::elem; p.name = n; p.global = global; p.index = std::move(idx); return p; }
	inline place map_p(const std::string& n, bool global, const std::string& key) { place p; p.kind = pk::map; p.name = n; p.global = global; p.key = key; return p; }
	inline place field_p(const std::string& n, bool global, const std::string& f) { place p; p.kind = pk::field; p.name = n; p.global = global; p.field = f; return p; }
	inline place bare_p(const std::string& f) { place p; p.kind = pk::field; p.bare = true; p.field = f; p.global = false; return p; }

	// ---------------------------------------------------------------- text

	class renderer {
	public:
		renderer(const prog& p, bool variant) : p_(p), variant_(variant) {}

		std::string render() {
			std::string out;
			for (const klass& k : p_.classes) {
				out += "class " + k.name + " {\n";
				for (size_t f = 0; f < k.fields.size(); ++f) { out += "\t" + std::string(type_text(k.fields[f].second)) + " " + k.fields[f].first + " = " + literal_text(k.init[f]) + ";\n"; }
				for (int m : k.methods) {
					const callable& c = p_.calls[static_cast<size_t>(m)];
					out += "\t" + std::string(type_text(c.ret)) + " " + c.name + "(" + params(c) + ") {\n";
					block(c.body, 2, out, false);
					out += "\t}\n";
				}
				out += "}\n";
			}
			for (const sptr& s : p_.globals) { statement(s, 0, out, false); }
			for (int f : p_.functions) {
				const callable& c = p_.calls[static_cast<size_t>(f)];
				if (c.ret_ref) { out += std::string(type_text(c.ret)) + "& " + c.name + "(" + params(c) + ") {\n"; }
				else {
					out += "function " + c.name + "(" + params(c) + ")";
					if (c.ret != ty::untyped) { out += std::string(" -> ") + type_text(c.ret); }
					out += " {\n";
				}
				block(c.body, 1, out, c.name.rfind("d", 0) == 0);
				out += "}\n";
			}
			for (const sptr& s : p_.top) { statement(s, 0, out, true); }
			out += "\"done\";\n";
			return out;
		}

	private:
		const prog& p_;
		bool variant_;
		int temps_ = 0;

		std::string params(const callable& c) {
			std::string s;
			for (size_t i = 0; i < c.params.size(); ++i) {
				if (i) { s += ", "; }
				s += std::string(type_text(c.params[i].t)) + (c.params[i].ref ? "& " : " ") + c.params[i].name;
			}
			return s;
		}
		static std::string tabs(int n) { return std::string(static_cast<size_t>(n), '\t'); }

		std::string place_text(const place& p) {
			switch (p.kind) {
			case pk::var: return p.name;
			case pk::elem: return p.name + "[" + expr_text(p.index) + "]";
			case pk::map: return p.name + "[\"" + p.key + "\"]";
			default: return p.bare ? p.field : p.name + "." + p.field;
			}
		}
		std::string call_text(const expr& e, const std::vector<std::string>& args) {
			const callable& c = p_.calls[static_cast<size_t>(e.callee)];
			std::string s = c.kind == ck::method ? e.recv + "." + c.name + "(" : c.name + "(";
			for (size_t i = 0; i < args.size(); ++i) { if (i) { s += ", "; } s += args[i]; }
			return s + ")";
		}
		std::string expr_text(const eptr& e) {
			switch (e->kind) {
			case ek::lit: return literal_text(e->lit);
			case ek::read: return place_text(e->p);
			case ek::bin: return "(" + expr_text(e->a) + " " + e->op + " " + expr_text(e->b) + ")";
			case ek::neg: return "(-" + expr_text(e->a) + ")";
			case ek::lnot: return "(!" + expr_text(e->a) + ")";
			case ek::tern: return "(" + expr_text(e->a) + " ? " + expr_text(e->b) + " : " + expr_text(e->c) + ")";
			case ek::call: {
				std::vector<std::string> a;
				for (const auto& x : e->args) { a.push_back(expr_text(x)); }
				return call_text(*e, a);
			}
			case ek::math: case ek::alias: {
				std::string s = (e->kind == ek::math ? "math::" : "host_") + e->op + "(";
				for (size_t i = 0; i < e->args.size(); ++i) { if (i) { s += ", "; } s += expr_text(e->args[i]); }
				return s + ")";
			}
			}
			return "0";
		}

		// the variant: every operand holding a call or a storage read lands in a fresh local, in the language's order; an `auto` local
		// takes its initializer's declared type, so a declaration fed from the temp sees the type the operand had in place
		std::string temp() { return "mt" + std::to_string(temps_++); }
		std::string hoist(const eptr& e, int depth, std::string& out) {
			switch (e->kind) {
			case ek::lit: return literal_text(e->lit);
			case ek::read: {
				std::string idx;
				if (e->p.kind == pk::elem) { idx = hoist(e->p.index, depth, out); }
				place q = e->p;
				std::string text = q.kind == pk::elem ? q.name + "[" + idx + "]" : place_text(q);
				const std::string t = temp();
				out += tabs(depth) + "auto " + t + " = " + text + ";\n";
				return t;
			}
			case ek::bin: {
				if (e->op == "&&" || e->op == "||") {
					const std::string l = hoist(e->a, depth, out);
					const std::string t = temp();
					out += tabs(depth) + "bool " + t + " = " + l + ";\n";
					out += tabs(depth) + (e->op == "&&" ? "if (" + t + ") {\n" : "if (!" + t + ") {\n");
					const std::string r = hoist(e->b, depth + 1, out);
					out += tabs(depth + 1) + t + " = " + r + ";\n";
					out += tabs(depth) + "}\n";
					return t;
				}
				const std::string l = hoist(e->a, depth, out);
				const std::string r = hoist(e->b, depth, out);
				return "(" + l + " " + e->op + " " + r + ")";
			}
			case ek::neg: return "(-" + hoist(e->a, depth, out) + ")";
			case ek::lnot: return "(!" + hoist(e->a, depth, out) + ")";
			case ek::tern: {
				const std::string c = hoist(e->a, depth, out);
				std::string arms;
				const std::string at = hoist(e->b, depth + 1, arms), bt = hoist(e->c, depth + 1, arms);
				if (arms.empty()) {   // arms with nothing to evaluate ahead: the chosen arm's value as the ternary yields it
					const std::string t = temp();
					out += tabs(depth) + "auto " + t + " = (" + c + " ? " + at + " : " + bt + ");\n";
					return t;
				}
				const std::string t = temp();
				out += tabs(depth) + "var " + t + ";\n";
				out += tabs(depth) + "if (" + c + ") {\n";
				const std::string a = hoist(e->b, depth + 1, out);
				out += tabs(depth + 1) + t + " = " + a + ";\n";
				out += tabs(depth) + "} else {\n";
				const std::string b = hoist(e->c, depth + 1, out);
				out += tabs(depth + 1) + t + " = " + b + ";\n";
				out += tabs(depth) + "}\n";
				return t;
			}
			case ek::call: {
				const callable& c = p_.calls[static_cast<size_t>(e->callee)];
				std::vector<std::string> a;
				for (size_t i = 0; i < e->args.size(); ++i) {
					if (i < c.params.size() && c.params[i].ref) { a.push_back(expr_text(e->args[i])); }
					else { a.push_back(hoist(e->args[i], depth, out)); }
				}
				if (c.ret == ty::none) return call_text(*e, a);   // a void call stands alone in its statement
				const std::string t = temp();
				out += tabs(depth) + "auto " + t + " = " + call_text(*e, a) + ";\n";
				return t;
			}
			case ek::math: case ek::alias: {
				std::string s = (e->kind == ek::math ? "math::" : "host_") + e->op + "(";
				for (size_t i = 0; i < e->args.size(); ++i) { if (i) { s += ", "; } s += hoist(e->args[i], depth, out); }
				return s + ")";
			}
			}
			return "0";
		}
		std::string target_text(const place& p, int depth, std::string& out) {
			if (!variant_ || p.kind != pk::elem) { return place_text(p); }
			return p.name + "[" + hoist(p.index, depth, out) + "]";
		}

		void block(const std::vector<sptr>& b, int depth, std::string& out, bool hoisting) {
			for (const sptr& s : b) { statement(s, depth, out, hoisting); }
		}
		void statement(const sptr& s, int depth, std::string& out, bool hoisting) {
			const bool h = variant_ && hoisting;
			auto value = [&](const eptr& e) { return h ? hoist(e, depth, out) : expr_text(e); };
			switch (s->kind) {
			case sk::decl: {
				const std::string v = value(s->e);
				out += tabs(depth) + type_text(s->t) + " " + s->name + " = " + v + ";\n";
				return;
			}
			case sk::assign: {
				const std::string v = value(s->e);
				const std::string t = h ? target_text(s->target, depth, out) : place_text(s->target);
				out += tabs(depth) + t + " = " + v + ";\n";
				return;
			}
			case sk::compound: {   // the right side first, then every part of the target (docs/site/guide/04-operators.html "Evaluation order is a contract")
				const std::string v = value(s->e);
				const std::string t = h ? target_text(s->target, depth, out) : place_text(s->target);
				out += tabs(depth) + t + " " + s->op + "= " + v + ";\n";
				return;
			}
			case sk::eval: out += tabs(depth) + value(s->e) + ";\n"; return;
			case sk::ret: {
				if (!s->e) { out += tabs(depth) + "return;\n"; return; }
				const std::string v = value(s->e);
				out += tabs(depth) + "return " + v + ";\n";
				return;
			}
			case sk::if_: {
				const std::string c = value(s->e);
				out += tabs(depth) + "if (" + c + ") {\n";
				block(s->body, depth + 1, out, hoisting);
				if (!s->other.empty()) { out += tabs(depth) + "} else {\n"; block(s->other, depth + 1, out, hoisting); }
				out += tabs(depth) + "}\n";
				return;
			}
			case sk::for_:
				out += tabs(depth) + "for (int " + s->name + " = 0; " + s->name + " < " + std::to_string(s->n) + "; " + s->name + "++) {\n";
				block(s->body, depth + 1, out, hoisting);
				out += tabs(depth) + "}\n";
				return;
			case sk::while_:
				out += tabs(depth) + "int " + s->name + " = 0;\n";
				out += tabs(depth) + "while (" + s->name + " < " + std::to_string(s->n) + ") {\n";
				block(s->body, depth + 1, out, hoisting);
				out += tabs(depth + 1) + s->name + " = " + s->name + " + 1;\n";
				out += tabs(depth) + "}\n";
				return;
			case sk::print: {
				std::string line = tabs(depth) + "print(\"" + s->tag + "\"";
				for (const auto& a : s->list) { line += ", " + expr_text(a); }
				out += line + ");\n";
				return;
			}
			case sk::arr_decl: case sk::map_decl: {
				std::vector<std::string> items;
				for (const auto& e : s->list) { items.push_back(value(e)); }
				std::string text = s->kind == sk::arr_decl ? "[" : "{";
				for (size_t i = 0; i < items.size(); ++i) {
					if (i) { text += ", "; }
					if (s->kind == sk::map_decl) { text += "\"" + s->keys[i] + "\": "; }
					text += items[i];
				}
				text += s->kind == sk::arr_decl ? "]" : "}";
				std::string decl = s->kind == sk::arr_decl ? (s->t == ty::v ? std::string("var") : "array<" + std::string(type_text(s->t)) + ">")
				                                           : (s->t == ty::v ? std::string("var") : "map<string, " + std::string(type_text(s->t)) + ">");
				out += tabs(depth) + decl + " " + s->name + " = " + text + ";\n";
				return;
			}
			case sk::obj_decl: out += tabs(depth) + s->tag + " " + s->name + " = " + s->tag + "();\n"; return;
			case sk::lambda_decl: {
				const callable& c = p_.calls[static_cast<size_t>(s->callee)];
				out += tabs(depth) + "var " + c.name + " = " + (c.by_ref ? "[&]" : "[=]") + "(" + params(c) + ")";
				if (c.ret != ty::untyped) { out += std::string(" -> ") + type_text(c.ret); }
				out += " {\n";
				block(c.body, depth + 1, out, false);
				out += tabs(depth) + "};\n";
				return;
			}
			}
		}
	};

	// ---------------------------------------------------------------- model

	class model {
	public:
		explicit model(const prog& p) : p_(p) {}

		std::string run() {
			frame top;
			top.top = true;
			for (const sptr& s : p_.globals) { exec(s, top); }
			for (const sptr& s : p_.top) { exec(s, top); }
			return out_;
		}

	private:
		struct cell { ty t = ty::v; mval v; };
		using cellp = std::shared_ptr<cell>;
		struct arr { ty elem = ty::v; std::vector<mval> v; };
		using arrp = std::shared_ptr<arr>;
		struct objv { std::string cls; std::map<std::string, cell> fields; };
		using objp = std::shared_ptr<objv>;
		struct mapv { ty vt = ty::v; std::map<std::string, mval> v; };
		using mapp = std::shared_ptr<mapv>;
		struct lam { int callee = -1; std::map<std::string, cellp> vars; std::map<std::string, arrp> arrays; std::map<std::string, objp> objs; };
		struct scope {
			std::map<std::string, cellp> vars;
			std::map<std::string, arrp> arrays;
			std::map<std::string, mapp> maps;
			std::map<std::string, objp> objs;
			std::map<std::string, std::shared_ptr<lam>> lams;
			std::map<std::string, std::pair<mval*, ty>> refs;
		};
		struct frame { scope s; objv* self = nullptr; bool top = false; bool returned = false; mval ret; };
		struct slot { mval* v = nullptr; ty t = ty::v; };

		const prog& p_;
		scope g_;
		std::string out_;
		int depth_ = 0;

		[[noreturn]] static void no(const std::string& why) { throw abstain{why}; }

		scope& scope_of(const place& p, frame& f) { return p.global || f.top ? g_ : f.s; }

		static bool truthy(const mval& v) {
			switch (v.k) {
			case 'i': return v.i != 0;
			case 'f': return v.f != 0.0;
			case 'b': return v.b;
			default: no("truthiness of null");
			}
		}
		static int64_t checked_trunc(double d) {
			if (!std::isfinite(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0) { no("float out of int range"); }
			return static_cast<int64_t>(d);
		}
		static mval convert(ty t, const mval& v) {
			if (t == ty::i || t == ty::f || t == ty::b || t == ty::s) { mval c = convert_typed(t, v); c.dyn = false; return c; }
			return v;
		}
		static mval convert_typed(ty t, const mval& v) {
			switch (t) {
			case ty::i:
				if (v.k == 'i') return v;
				if (v.k == 'f') return mi(checked_trunc(v.f));
				if (v.k == 'b') return mi(v.b ? 1 : 0);
				no("null into int");
			case ty::f:
				if (v.k == 'f') return v;
				if (v.k == 'i') return mf(static_cast<double>(v.i));
				if (v.k == 'b') return mf(v.b ? 1.0 : 0.0);
				no("null into float");
			case ty::b: return mb(truthy(v));
			case ty::s: if (v.k == 's') return v; no("a non-string into a string");
			default: return v;
			}
		}
		static mval convert_element(ty t, const mval& v) {
			if (t == ty::f && v.k == 'b') { no("bool into a float element"); }
			return convert(t, v);
		}
		static ty kind_of(const mval& v) { return v.k == 'i' ? ty::i : v.k == 'f' ? ty::f : v.k == 'b' ? ty::b : v.k == 's' ? ty::s : ty::v; }

		static mval arith(const std::string& op, const mval& a, const mval& b) {
			if (a.k == 's' && b.k == 's' && op == "+") return ms(a.s + b.s);
			if ((a.k != 'i' && a.k != 'f') || (b.k != 'i' && b.k != 'f')) { no("arithmetic on a non-number"); }
			if (a.k == 'i' && b.k == 'i') {
				const int64_t x = a.i, y = b.i;
				int64_t r = 0;
				if (op == "+") { if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) no("overflow"); r = x + y; }
				else if (op == "-") { if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) no("overflow"); r = x - y; }
				else if (op == "*") {
					if (x != 0 && y != 0) {
						if ((x == -1 && y == INT64_MIN) || (y == -1 && x == INT64_MIN)) no("overflow");
						const int64_t w = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y));
						if (w / y != x) no("overflow");
					}
					r = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y));
				}
				else if (op == "/") { if (y == 0 || (x == INT64_MIN && y == -1)) no("division"); r = x / y; }
				else if (op == "%") { if (y == 0) no("modulo"); r = y == -1 ? 0 : x % y; }
				else if (op == "&") r = x & y;
				else if (op == "|") r = x | y;
				else if (op == "^") r = x ^ y;
				else if (op == "<<") { if (y < 0 || y > 63) no("shift"); r = static_cast<int64_t>(static_cast<uint64_t>(x) << y); }
				else if (op == ">>") { if (y < 0 || y > 63) no("shift"); r = x >> y; }
				else no("operator");
				return mi(r);
			}
			if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") { no("bitwise on a float"); }
			const double x = a.k == 'i' ? static_cast<double>(a.i) : a.f, y = b.k == 'i' ? static_cast<double>(b.i) : b.f;
			if (op == "+") return mf(x + y);
			if (op == "-") return mf(x - y);
			if (op == "*") return mf(x * y);
			if (op == "/") { if (y == 0.0) no("division"); return mf(x / y); }
			if (op == "%") { if (y == 0.0) no("modulo"); return mf(std::fmod(x, y)); }
			no("operator");
		}
		static mval compare(const std::string& op, const mval& a, const mval& b) {
			if (a.k == 's' && b.k == 's') {
				if (op == "==") return mb(a.s == b.s);
				if (op == "!=") return mb(a.s != b.s);
				no("ordering strings");
			}
			if (a.k == 'b' && b.k == 'b') {
				if (op == "==") return mb(a.b == b.b);
				if (op == "!=") return mb(a.b != b.b);
				no("ordering bools");
			}
			if ((a.k != 'i' && a.k != 'f') || (b.k != 'i' && b.k != 'f')) { no("comparison of a non-number"); }
			bool r = false;
			if (a.k == 'i' && b.k == 'i') {
				const int64_t x = a.i, y = b.i;
				r = op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : op == ">=" ? x >= y : op == "==" ? x == y : x != y;
			} else {
				const double x = a.k == 'i' ? static_cast<double>(a.i) : a.f, y = b.k == 'i' ? static_cast<double>(b.i) : b.f;
				r = op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : op == ">=" ? x >= y : op == "==" ? x == y : x != y;
			}
			return mb(r);
		}
		static bool is_compare(const std::string& op) { return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!="; }

		slot resolve(const place& p, frame& f) {
			switch (p.kind) {
			case pk::var: {
				if (!p.global && !f.top) {
					auto r = f.s.refs.find(p.name);
					if (r != f.s.refs.end()) return {r->second.first, r->second.second};
				}
				scope& s = scope_of(p, f);
				auto it = s.vars.find(p.name);
				if (it == s.vars.end()) { no("unknown variable " + p.name); }
				return {&it->second->v, it->second->t};
			}
			case pk::elem: {
				const mval idx = eval(p.index, f);
				scope& s = scope_of(p, f);
				auto it = s.arrays.find(p.name);
				if (it == s.arrays.end()) { no("unknown array " + p.name); }
				if (idx.k != 'i' || idx.i < 0 || static_cast<size_t>(idx.i) >= it->second->v.size()) { no("index"); }
				return {&it->second->v[static_cast<size_t>(idx.i)], it->second->elem == ty::v ? ty::v : it->second->elem};
			}
			case pk::map: {
				scope& s = scope_of(p, f);
				auto it = s.maps.find(p.name);
				if (it == s.maps.end()) { no("unknown map " + p.name); }
				auto e = it->second->v.find(p.key);
				if (e == it->second->v.end()) { no("missing key"); }
				return {&e->second, it->second->vt};
			}
			default: {
				objv* o = nullptr;
				if (p.bare) { o = f.self; }
				else {
					scope& s = scope_of(p, f);
					auto it = s.objs.find(p.name);
					if (it == s.objs.end()) { no("unknown object " + p.name); }
					o = it->second.get();
				}
				if (!o) { no("no receiver"); }
				auto fi = o->fields.find(p.field);
				if (fi == o->fields.end()) { no("unknown field"); }
				return {&fi->second.v, fi->second.t};
			}
			}
		}

		// an expression of unknown static type (an auto declared from it is a var): a read of var storage, or arithmetic, a
		// negation, a conditional or min/max/abs over such an operand
		bool static_dyn(const eptr& e, frame& f) {
			switch (e->kind) {
			case ek::read: return e->p.kind == pk::var && resolve(e->p, f).t == ty::v;
			case ek::bin: return !is_compare(e->op) && e->op != "&&" && e->op != "||" && (static_dyn(e->a, f) || static_dyn(e->b, f));
			case ek::neg: return static_dyn(e->a, f);
			case ek::tern: return static_dyn(e->b, f) || static_dyn(e->c, f);
			case ek::math:
				if (e->op == "abs") return static_dyn(e->args[0], f);
				if (e->op == "min" || e->op == "max") return static_dyn(e->args[0], f) || static_dyn(e->args[1], f);
				return false;
			default: return false;
			}
		}

		mval eval(const eptr& e, frame& f) {
			switch (e->kind) {
			case ek::lit: return e->lit;
			case ek::read: {
				const slot sl = resolve(e->p, f);
				mval r = *sl.v;
				if (e->p.kind == pk::var) { r.dyn = sl.t == ty::v; }   // an element or an entry keeps the type its value was stored with
				else if (e->p.kind == pk::field) { r.dyn = false; }
				return r;
			}
			case ek::bin: {
				if (e->op == "&&") { if (!truthy(eval(e->a, f))) return mb(false); return mb(truthy(eval(e->b, f))); }
				if (e->op == "||") { if (truthy(eval(e->a, f))) return mb(true); return mb(truthy(eval(e->b, f))); }
				const mval a = eval(e->a, f);
				const mval b = eval(e->b, f);
				if (is_compare(e->op)) { return compare(e->op, a, b); }
				mval r = arith(e->op, a, b);
				r.dyn = a.dyn || b.dyn;   // an operand of unknown static type makes the result's unknown (an auto from it is a var)
				return r;
			}
			case ek::neg: {
				const mval a = eval(e->a, f);
				mval r;
				if (a.k == 'i') { if (a.i == INT64_MIN) no("negation"); r = mi(-a.i); }
				else if (a.k == 'f') { r = mf(-a.f); }
				else { no("negation of a non-number"); }
				r.dyn = a.dyn;
				return r;
			}
			case ek::lnot: return mb(!truthy(eval(e->a, f)));
			case ek::tern: {   // the common type of both arms: unknown when either arm's is
				const bool first = truthy(eval(e->a, f));
				mval r = eval(first ? e->b : e->c, f);
				r.dyn = r.dyn || static_dyn(first ? e->c : e->b, f);
				return r;
			}
			case ek::call: return call(*e, f);
			case ek::math: {
				std::vector<mval> a;
				for (const auto& x : e->args) { a.push_back(eval(x, f)); }
				for (const auto& x : a) { if (x.k != 'i' && x.k != 'f') no("math on a non-number"); }
				auto d = [](const mval& x) { return x.k == 'i' ? static_cast<double>(x.i) : x.f; };
				if (e->op == "abs") {   // the operand's type: unknown when the operand's is
					if (a[0].k == 'i' && a[0].i == INT64_MIN) no("abs");
					mval r = a[0].k == 'i' ? mi(a[0].i < 0 ? -a[0].i : a[0].i) : mf(std::fabs(a[0].f));
					r.dyn = a[0].dyn;
					return r;
				}
				if (e->op == "itrunc") { if (a[0].k == 'i') return mi(a[0].i); return mi(checked_trunc(a[0].f)); }
				if (e->op == "ifloor") { if (a[0].k == 'i') return mi(a[0].i); return mi(checked_trunc(std::floor(a[0].f))); }
				if (e->op == "sqrt") return mf(std::sqrt(d(a[0])));
				if (e->op == "floor") return mf(std::floor(d(a[0])));
				if (e->op == "ceil") return mf(std::ceil(d(a[0])));
				if (e->op == "min" || e->op == "max") {
					const bool mn = e->op == "min";
					const bool pick_a = a[0].k == 'i' && a[1].k == 'i' ? (mn ? a[0].i <= a[1].i : a[0].i >= a[1].i) : (mn ? d(a[0]) <= d(a[1]) : d(a[0]) >= d(a[1]));
					mval r = pick_a ? a[0] : a[1];
					r.dyn = a[0].dyn || a[1].dyn;   // the operands' type: unknown when either operand's is
					return r;
				}
				no("math function");
			}
			case ek::alias: {
				const mval a = eval(e->args[0], f);
				if (a.k != 'i' && a.k != 'f') { no("alias on a non-number"); }
				const double x = a.k == 'i' ? static_cast<double>(a.i) : a.f;
				if (e->op == "sqrt") return mf(std::sqrt(x));
				if (e->op == "abs") return mf(std::fabs(x));
				if (e->op == "floor") return mf(std::floor(x));
				if (e->op == "ceil") return mf(std::ceil(x));
				no("alias");
			}
			}
			no("expression");
		}

		mval call(const expr& e, frame& caller) {
			const callable& c = p_.calls[static_cast<size_t>(e.callee)];
			if (++depth_ > 64) { no("depth"); }
			frame f;
			if (c.kind == ck::lambda) {
				scope& s = caller.top ? g_ : caller.s;
				auto it = s.lams.find(c.name);
				if (it == s.lams.end()) { no("unknown lambda " + c.name); }
				f.s.vars = it->second->vars;
				f.s.arrays = it->second->arrays;
				f.s.objs = it->second->objs;
			} else if (c.kind == ck::method) {
				scope& s = e.recv_global || caller.top ? g_ : caller.s;
				auto it = s.objs.find(e.recv);
				if (it == s.objs.end()) { no("unknown receiver " + e.recv); }
				f.self = it->second.get();
			}
			for (size_t i = 0; i < c.params.size(); ++i) {
				const param& pa = c.params[i];
				if (pa.ref) {
					if (e.args[i]->kind != ek::read) { no("reference argument"); }
					slot sl = resolve(e.args[i]->p, caller);
					f.s.refs[pa.name] = {sl.v, sl.t};
					continue;
				}
				auto cp = std::make_shared<cell>();
				cp->t = pa.t;
				cp->v = convert(pa.t, eval(e.args[i], caller));
				f.s.vars[pa.name] = cp;
			}
			for (const sptr& s : c.body) { exec(s, f); if (f.returned) break; }
			--depth_;
			if (c.ret == ty::none) { return mval{}; }
			if (!f.returned) { no("fell out"); }
			return c.ret == ty::untyped ? f.ret : convert(c.ret, f.ret);
		}

		void store(const slot& s, const mval& v, bool element) {
			*s.v = element ? convert_element(s.t, v) : convert(s.t, v);
		}

		void exec(const sptr& s, frame& f) {
			if (f.returned) return;
			switch (s->kind) {
			case sk::decl: {
				const mval v = eval(s->e, f);
				auto cp = std::make_shared<cell>();
				cp->t = s->t == ty::a ? (v.dyn ? ty::v : kind_of(v)) : s->t;   // auto takes its initializer's declared type
				cp->v = convert(cp->t, v);
				scope_of(var_p(s->name, s->global), f).vars[s->name] = cp;
				return;
			}
			case sk::assign: {
				const mval v = eval(s->e, f);
				const slot sl = resolve(s->target, f);
				store(sl, v, s->target.kind == pk::elem || s->target.kind == pk::map);
				return;
			}
			case sk::compound: {   // the right side first, then every part of the target and its read (docs/site/guide/04-operators.html "Evaluation order is a contract")
				const mval v = eval(s->e, f);
				const slot sl = resolve(s->target, f);
				store(sl, arith(s->op, *sl.v, v), s->target.kind == pk::elem || s->target.kind == pk::map);
				return;
			}
			case sk::eval: (void)eval(s->e, f); return;
			case sk::ret: f.ret = s->e ? eval(s->e, f) : mval{}; f.returned = true; return;
			case sk::if_: {
				const bool c = truthy(eval(s->e, f));
				for (const sptr& b : c ? s->body : s->other) { exec(b, f); if (f.returned) return; }
				return;
			}
			case sk::for_: case sk::while_: {
				auto counter = std::make_shared<cell>();
				counter->t = ty::i;
				counter->v = mi(0);
				scope_of(var_p(s->name, false), f).vars[s->name] = counter;
				while (counter->v.i < s->n) {
					for (const sptr& b : s->body) { exec(b, f); if (f.returned) return; }
					counter->v.i += 1;
				}
				return;
			}
			case sk::print: {
				std::string line = s->tag;
				for (const auto& a : s->list) { line += " " + value_text(eval(a, f)); }
				out_ += line + "\n";
				if (out_.size() > 30 * 1024) { no("output past the harness's print cap"); }
				return;
			}
			case sk::arr_decl: {
				auto a = std::make_shared<arr>();
				a->elem = s->t;
				for (const auto& e : s->list) { a->v.push_back(convert_element(s->t, eval(e, f))); }
				scope_of(var_p(s->name, s->global), f).arrays[s->name] = a;
				return;
			}
			case sk::map_decl: {
				auto m = std::make_shared<mapv>();
				m->vt = s->t;
				std::vector<mval> vals;
				for (const auto& e : s->list) { vals.push_back(eval(e, f)); }
				for (size_t i = 0; i < vals.size(); ++i) { m->v[s->keys[i]] = convert_element(s->t, vals[i]); }
				scope_of(var_p(s->name, s->global), f).maps[s->name] = m;
				return;
			}
			case sk::obj_decl: {
				auto o = std::make_shared<objv>();
				o->cls = s->tag;
				for (const klass& k : p_.classes) {
					if (k.name != s->tag) continue;
					for (size_t i = 0; i < k.fields.size(); ++i) { o->fields[k.fields[i].first] = cell{k.fields[i].second, k.init[i]}; }
				}
				scope_of(var_p(s->name, s->global), f).objs[s->name] = o;
				return;
			}
			case sk::lambda_decl: {
				const callable& c = p_.calls[static_cast<size_t>(s->callee)];
				auto l = std::make_shared<lam>();
				l->callee = s->callee;
				scope& from = f.top ? g_ : f.s;
				if (c.by_ref) {
					l->vars = from.vars; l->arrays = from.arrays; l->objs = from.objs;
				} else {
					for (const auto& [n, v] : from.vars) { l->vars[n] = std::make_shared<cell>(*v); }
					for (const auto& [n, v] : from.arrays) { l->arrays[n] = std::make_shared<arr>(*v); }
					for (const auto& [n, v] : from.objs) { l->objs[n] = std::make_shared<objv>(*v); }
				}
				from.lams[c.name] = l;
				return;
			}
			}
		}
	};

	// ---------------------------------------------------------------- generator

	struct generated {
		program original;
		program variant;
		std::string expected;
		bool model_ok = false;
		std::string abstained;
	};

	class generator {
	public:
		generator(uint64_t seed, bool argument_hazards) : rng_(mix64(seed + 0x2545F4914F6CDD1DULL)), args_(argument_hazards), seed_(seed) {}

		generated generate() {
			block_ = rng_.chance(55);
			emit_globals();
			emit_classes_objects();
			emit_callees();
			const int drivers = static_cast<int>(rng_.range(1, 3));
			std::vector<int> ds;
			for (int d = 0; d < drivers; ++d) { ds.push_back(emit_driver(d)); }
			for (int d : ds) {
				p_.top.push_back(eval_s(call0(d)));
				if (rng_.chance(35)) { p_.top.push_back(eval_s(call0(d))); }
			}
			if (rng_.chance(60)) { emit_top_hazards(); }
			emit_final_prints();
			generated g;
			g.original.seed = seed_;
			g.original.host_math_aliases = true;
			g.original.segments.push_back(renderer(p_, false).render());
			g.variant.seed = seed_;
			g.variant.host_math_aliases = true;
			g.variant.segments.push_back(renderer(p_, true).render());
			try {
				g.expected = model(p_).run();
				g.model_ok = true;
			} catch (const abstain& a) {
				g.abstained = a.why;
			}
			return g;
		}

	private:
		struct store_info { std::string name; ty t = ty::i; bool global = true; int len = 0; bool index_var = false; };
		struct scope_info {
			std::vector<store_info> scalars, arrays, objects, strings;
			std::vector<int> lambdas, string_lambdas;
			std::string ivar;
			int ivar_n = 0;
		};

		// draws never share an unsequenced expression (call arguments, + operands) so every compiler generates the same program per seed; a split draws the right operand first, the order GCC and MSVC used, so seeds keep their programs
		fuzz_rng rng_;
		bool args_;
		bool block_ = false;   // the program keeps to what the vm closes into cell blocks and regions: typed scalars and arrays, grafted leaves
		uint64_t seed_;
		prog p_;
		int names_ = 0;
		scope_info g_;
		scope_info* local_ = nullptr;
		std::vector<int> writers_, pures_, refs_, string_writers_;
		int rec_ = -1;

		std::string fresh(const char* prefix) { return prefix + std::to_string(names_++); }
		int pick(const std::vector<uint32_t>& w) {
			uint32_t total = 0;
			for (auto x : w) total += x;
			uint32_t roll = rng_.below(total);
			for (size_t i = 0; i < w.size(); ++i) { if (roll < w[i]) return static_cast<int>(i); roll -= w[i]; }
			return 0;
		}
		mval small_int() { return mi(rng_.range(-3, 9)); }
		mval small_float() {
			static constexpr double v[] = {0.5, 1.5, 2.25, 0.25, 3.0, 1.0, -0.5, -1.25, 4.5};
			return mf(v[rng_.below(9)]);
		}
		mval literal_of(ty t) {
			if (t == ty::i) return small_int();
			if (t == ty::f) return small_float();
			if (t == ty::b) return mb(rng_.chance(50));
			return rng_.chance(60) ? small_int() : small_float();
		}
		static bool numeric(ty t) { return t == ty::i || t == ty::f || t == ty::v; }
		mval string_literal() {
			static constexpr const char* v[] = {"a", "bc", "", "q r", "7"};
			return ms(v[rng_.below(5)]);
		}

		static sptr decl_s(ty t, const std::string& n, eptr e, bool global) { auto s = std::make_shared<stmt>(); s->kind = sk::decl; s->t = t; s->name = n; s->e = std::move(e); s->global = global; return s; }
		static sptr assign_s(const place& p, eptr e) { auto s = std::make_shared<stmt>(); s->kind = sk::assign; s->target = p; s->e = std::move(e); return s; }
		static sptr compound_s(const place& p, const std::string& op, eptr e) { auto s = std::make_shared<stmt>(); s->kind = sk::compound; s->target = p; s->op = op; s->e = std::move(e); return s; }
		static sptr eval_s(eptr e) { auto s = std::make_shared<stmt>(); s->kind = sk::eval; s->e = std::move(e); return s; }
		static sptr ret_s(eptr e) { auto s = std::make_shared<stmt>(); s->kind = sk::ret; s->e = std::move(e); return s; }
		static sptr print_s(const std::string& tag, std::vector<eptr> list) { auto s = std::make_shared<stmt>(); s->kind = sk::print; s->tag = tag; s->list = std::move(list); return s; }
		eptr call0(int callee) { auto e = std::make_shared<expr>(); e->kind = ek::call; e->callee = callee; return e; }

		bool in_driver() const { return local_ != nullptr; }
		scope_info& here() { return local_ ? *local_ : g_; }

		// ---------------- storage

		void emit_globals() {
			const int scalars = static_cast<int>(rng_.range(3, 5));
			for (int k = 0; k < scalars; ++k) {
				static constexpr ty kinds[] = {ty::i, ty::i, ty::f, ty::b, ty::v, ty::i};
				static constexpr ty block_kinds[] = {ty::i, ty::i, ty::f, ty::b, ty::f, ty::i};
				const ty t = block_ ? block_kinds[rng_.below(6)] : kinds[rng_.below(6)];
				store_info s{fresh("G"), t, true};
				const mval init = t == ty::v ? literal_of(ty::v) : literal_of(t);
				p_.globals.push_back(decl_s(t, s.name, lit_e(init), true));
				if (t == ty::v) { s.t = ty::v; }
				g_.scalars.push_back(s);
			}
			if (rng_.chance(35)) {
				store_info s{fresh("S"), ty::s, true};
				p_.globals.push_back(decl_s(ty::s, s.name, lit_e(string_literal()), true));
				g_.strings.push_back(s);
			}
			const int arrays = static_cast<int>(rng_.range(1, 3));
			for (int k = 0; k < arrays; ++k) {
				static constexpr ty kinds[] = {ty::i, ty::i, ty::f, ty::v};
				const ty t = block_ ? (rng_.chance(65) ? ty::i : ty::f) : kinds[rng_.below(4)];
				store_info a{fresh("A"), t, true, static_cast<int>(rng_.range(3, 6))};
				auto s = std::make_shared<stmt>();
				s->kind = sk::arr_decl; s->t = t; s->name = a.name; s->global = true;
				for (int i = 0; i < a.len; ++i) { s->list.push_back(lit_e(literal_of(t))); }
				p_.globals.push_back(s);
				g_.arrays.push_back(a);
			}
			if (!block_ && rng_.chance(60)) {
				auto s = std::make_shared<stmt>();
				s->kind = sk::map_decl; s->t = ty::i; s->name = "M0"; s->global = true;
				for (const char* k : {"a", "b", "c"}) { s->keys.push_back(k); s->list.push_back(lit_e(small_int())); }
				p_.globals.push_back(s);
				has_map_ = true;
			}
		}
		bool has_map_ = false;

		void emit_classes_objects() {
			if (block_ || !rng_.chance(65)) return;
			klass k;
			k.name = "K0";
			k.fields = {{"v", ty::i}, {"w", ty::f}};
			k.init = {small_int(), small_float()};
			p_.classes.push_back(k);
			const size_t ki = p_.classes.size() - 1;
			const int methods = static_cast<int>(rng_.range(1, 2));
			for (int m = 0; m < methods; ++m) {
				callable c;
				c.kind = ck::method;
				c.cls = "K0";
				c.name = fresh("m");
				const bool on_v = rng_.chance(60);
				const std::string fld = on_v ? "v" : "w";
				const ty ft = on_v ? ty::i : ty::f;
				c.ret = rng_.chance(70) ? ty::i : ty::f;
				c.result = c.ret;
				if (rng_.chance(40)) { c.params.push_back({"k", ty::i}); }
				eptr delta = !c.params.empty() ? read_e(var_p("k", false)) : lit_e(ft == ty::i ? mi(rng_.range(1, 4)) : small_float());
				c.body.push_back(assign_s(bare_p(fld), bin_e(rng_.chance(70) ? "+" : "-", read_e(bare_p(fld)), delta)));
				c.body.push_back(ret_s(rng_.chance(50) ? read_e(bare_p(fld)) : lit_e(literal_of(c.ret))));
				c.writes.push_back(bare_p(fld));
				p_.calls.push_back(c);
				p_.classes[ki].methods.push_back(static_cast<int>(p_.calls.size() - 1));
			}
			store_info o{"O0", ty::v, true};
			auto s = std::make_shared<stmt>();
			s->kind = sk::obj_decl; s->tag = "K0"; s->name = "O0"; s->global = true;
			p_.globals.push_back(s);
			g_.objects.push_back(o);
		}

		// a place of the storage visible here (the driver's locals, then the globals)
		struct placed { place p; ty t = ty::i; };
		struct wcall { eptr e; std::vector<placed> writes; ty result = ty::i; };
		std::vector<placed> readable(bool want_bool, bool writable_int_only = false) {
			std::vector<placed> out;
			auto add_scope = [&](scope_info& s, bool global) {
				for (const auto& v : s.scalars) {
					if (want_bool != (v.t == ty::b)) continue;
					if (writable_int_only && v.t != ty::i) continue;
					out.push_back({var_p(v.name, global), v.t});
				}
				if (want_bool) return;
				for (const auto& a : s.arrays) {
					if (writable_int_only && a.t != ty::i) continue;
					out.push_back({elem_p(a.name, global, index_for(a.len)), a.t});
				}
				for (const auto& o : s.objects) {
					out.push_back({field_p(o.name, global, "v"), ty::i});
					if (!writable_int_only) { out.push_back({field_p(o.name, global, "w"), ty::f}); }
				}
				if (global && has_map_) { out.push_back({map_p("M0", true, std::string(1, static_cast<char>('a' + rng_.below(3)))), ty::i}); }
			};
			if (local_) { add_scope(*local_, false); }
			add_scope(g_, true);
			return out;
		}
		eptr index_for(int len) {
			const scope_info& s = here();
			std::vector<eptr> opts;
			opts.push_back(lit_e(mi(rng_.range(0, len - 1))));
			if (!s.ivar.empty()) { opts.push_back(bin_e("%", read_e(var_p(s.ivar, false)), lit_e(mi(len)))); }
			if (local_) {
				for (const auto& v : local_->scalars) { if (v.index_var) { opts.push_back(bin_e("%", read_e(var_p(v.name, false)), lit_e(mi(len)))); } }
			}
			return opts[rng_.below(static_cast<uint32_t>(opts.size()))];
		}

		// ---------------- callees

		void emit_callees() {
			{
				callable r;
				r.name = "rec0";
				r.params.push_back({"n", ty::i});
				r.ret = ty::i;
				auto s = std::make_shared<stmt>();
				s->kind = sk::if_;
				s->e = bin_e("<=", read_e(var_p("n", false)), lit_e(mi(0)));
				s->body.push_back(ret_s(lit_e(mi(0))));
				r.body.push_back(s);
				auto c = std::make_shared<expr>(); c->kind = ek::call; c->callee = static_cast<int>(p_.calls.size());
				c->args.push_back(bin_e("-", read_e(var_p("n", false)), lit_e(mi(1))));
				r.body.push_back(ret_s(c));
				p_.calls.push_back(r);
				rec_ = static_cast<int>(p_.calls.size() - 1);
				p_.functions.push_back(rec_);
			}
			const int writers = static_cast<int>(rng_.range(2, 5));
			for (int k = 0; k < writers; ++k) { emit_writer(); }
			if (rng_.chance(70)) { emit_ref_writer(); }
			const int pures = static_cast<int>(rng_.range(0, 2));
			for (int k = 0; k < pures; ++k) { emit_pure(); }
			emit_getters();
			if (!g_.strings.empty()) { const int n = static_cast<int>(rng_.range(1, 2)); for (int k = 0; k < n; ++k) emit_string_writer(); }
		}
		// `int& g() { return G; }` over an int global or an int element: an operand that reads through the call's reference result
		std::vector<std::pair<int, place>> getters_;
		void emit_getters() {
			std::vector<place> opts;
			for (const auto& v : g_.scalars) { if (v.t == ty::i) opts.push_back(var_p(v.name, true)); }
			for (const auto& a : g_.arrays) { if (a.t == ty::i) opts.push_back(elem_p(a.name, true, lit_e(mi(rng_.range(0, a.len - 1))))); }
			const int n = rng_.chance(60) ? static_cast<int>(rng_.range(1, 2)) : 0;
			for (int k = 0; k < n && !opts.empty(); ++k) {
				const place t = opts[rng_.below(static_cast<uint32_t>(opts.size()))];
				callable c;
				c.name = fresh("g");
				c.ret = ty::i; c.ret_ref = true; c.result = ty::i;
				c.body.push_back(ret_s(read_e(t)));
				p_.calls.push_back(c);
				const int idx = static_cast<int>(p_.calls.size() - 1);
				getters_.push_back({idx, t});
				p_.functions.push_back(idx);
			}
		}
		// a writer of a global string: appends to it (sometimes through a string local), hands back a string or nothing
		void emit_string_writer() {
			callable c;
			c.name = fresh("ws");
			const store_info& t = g_.strings[rng_.below(static_cast<uint32_t>(g_.strings.size()))];
			const place tp = var_p(t.name, true);
			c.ret = rng_.chance(75) ? ty::s : ty::none;
			c.result = ty::s;
			if (rng_.chance(40)) {
				c.local = "tl"; c.local_t = ty::s;
				c.body.push_back(decl_s(ty::s, "tl", bin_e("+", read_e(tp), lit_e(string_literal())), false));
				c.body.push_back(assign_s(tp, read_e(var_p("tl", false))));
			} else {
				c.body.push_back(assign_s(tp, rng_.chance(50) ? bin_e("+", read_e(tp), lit_e(string_literal())) : bin_e("+", lit_e(string_literal()), read_e(tp))));
			}
			c.writes.push_back(tp);
			if (c.ret == ty::s) { c.body.push_back(ret_s(rng_.chance(50) ? read_e(tp) : lit_e(string_literal()))); }
			p_.calls.push_back(c);
			string_writers_.push_back(static_cast<int>(p_.calls.size() - 1));
			p_.functions.push_back(string_writers_.back());
		}
		// a string writer for an operand: a global writer or a lambda of this driver that appends to a captured string
		bool string_writer_call(bool allow_void, wcall& out) {
			std::vector<int> cands;
			for (int w : string_writers_) { if (allow_void || p_.calls[static_cast<size_t>(w)].ret != ty::none) cands.push_back(w); }
			if (local_) { for (int l : local_->string_lambdas) cands.push_back(l); }
			if (cands.empty()) return false;
			const int w = cands[rng_.below(static_cast<uint32_t>(cands.size()))];
			out.e = call0(w);
			out.result = ty::s;
			out.writes.clear();
			for (const place& p : p_.calls[static_cast<size_t>(w)].writes) out.writes.push_back({p, ty::s});
			return true;
		}
		eptr string_read(const wcall& w) {
			if (!w.writes.empty() && rng_.chance(75)) return read_e(w.writes[rng_.below(static_cast<uint32_t>(w.writes.size()))].p);
			std::vector<place> opts;
			if (local_) { for (const auto& v : local_->strings) opts.push_back(var_p(v.name, false)); }
			for (const auto& v : g_.strings) opts.push_back(var_p(v.name, true));
			if (opts.empty()) return lit_e(string_literal());
			return read_e(opts[rng_.below(static_cast<uint32_t>(opts.size()))]);
		}
		// the operand patterns around a call that writes a string another operand reads
		eptr hazard_str() {
			wcall w;
			if (!string_writer_call(false, w)) return lit_e(string_literal());
			const eptr r = string_read(w);
			switch (rng_.below(5)) {
			case 0: return bin_e("+", r, w.e);
			case 1: return bin_e("+", w.e, r);
			case 2: return bin_e("+", bin_e("+", r, lit_e(string_literal())), w.e);
			case 3: {
				const eptr no = lit_e(string_literal());
				const eptr yes = lit_e(string_literal());
				const eptr probe = lit_e(string_literal());
				const char* op = rng_.chance(50) ? "==" : "!=";
				return bin_e("+", r, tern_e(bin_e(op, w.e, probe), yes, no));
			}
			default: return bin_e("+", bin_e("+", r, w.e), string_read(w));
			}
		}
		void string_statement(std::vector<sptr>& b, bool in_function) {
			wcall w;
			switch (rng_.below(5)) {
			case 0: case 1: {
				const std::string n = fresh("r");
				const eptr init = hazard_str();
				b.push_back(decl_s(rng_.chance(70) ? ty::s : ty::v, n, init, !in_function));
				if (in_function) { b.push_back(assign_s(var_p("as", false), bin_e("+", read_e(var_p("as", false)), read_e(var_p(n, false))))); }
				else { printed_.push_back(n); }
				return;
			}
			case 2: {
				if (!string_writer_call(false, w) || w.writes.empty()) { string_statement_decl(b, in_function); return; }
				const place t = w.writes[rng_.below(static_cast<uint32_t>(w.writes.size()))].p;
				b.push_back(assign_s(t, rng_.chance(50) ? bin_e("+", read_e(t), w.e) : bin_e("+", w.e, read_e(t))));
				return;
			}
			case 3: {
				if (!string_writer_call(false, w) || w.writes.empty()) { string_statement_decl(b, in_function); return; }
				b.push_back(compound_s(w.writes[rng_.below(static_cast<uint32_t>(w.writes.size()))].p, "+", w.e));
				return;
			}
			default: {
				if (!string_writer_call(false, w)) { string_statement_decl(b, in_function); return; }
				const std::string n = fresh("r");
				const eptr read = string_read(w);
				b.push_back(decl_s(ty::b, n, bin_e(rng_.chance(50) ? "==" : "!=", read, w.e), !in_function));
				if (in_function) accumulate(b, n, ty::b); else printed_.push_back(n);
				return;
			}
			}
		}
		void string_statement_decl(std::vector<sptr>& b, bool in_function) {
			const std::string n = fresh("r");
			b.push_back(decl_s(ty::s, n, hazard_str(), !in_function));
			if (in_function) { b.push_back(assign_s(var_p("as", false), bin_e("+", read_e(var_p("as", false)), read_e(var_p(n, false))))); }
			else { printed_.push_back(n); }
		}
		bool strings_in_reach() const { return !string_writers_.empty() || (local_ && !local_->string_lambdas.empty()); }
		static bool same_place(const place& a, const place& b) {
			if (a.kind != b.kind || a.name != b.name || a.global != b.global) return false;
			if (a.kind != pk::elem) return a.kind == pk::var;
			return a.index && b.index && a.index->kind == ek::lit && b.index->kind == ek::lit && a.index->lit.i == b.index->lit.i;
		}

		// the body statements a writer runs: stores into the targets, sometimes under an if on a parameter, sometimes behind a call
		// that keeps it off the grafts (rec0)
		void writer_body(callable& c, const std::vector<placed>& targets, bool locals_ok) {
			(void)locals_ok;
			if (rng_.chance(block_ ? 10 : 22)) { c.body.push_back(eval_s(rec_call())); }
			if (rng_.chance(40)) {
				c.local = "tl";
				c.local_t = rng_.chance(50) ? ty::f : ty::i;
				eptr init;
				const bool numeric_param = !c.params.empty() && !c.params[0].ref && c.params[0].t != ty::b;
				std::vector<placed> reads;
				for (const placed& t : targets) { if (t.t == ty::i || t.t == ty::f) reads.push_back(t); }
				switch (rng_.below(3)) {
				case 0: init = numeric_param ? read_e(var_p(c.params[0].name, false)) : lit_e(literal_of(c.local_t == ty::f ? ty::i : ty::f)); break;
				case 1: init = !reads.empty() ? read_e(reads[0].p) : lit_e(small_int()); break;
				default: {
					const eptr rhs = lit_e(literal_of(rng_.chance(50) ? ty::i : ty::f));
					init = bin_e("+", numeric_param ? read_e(var_p(c.params[0].name, false)) : lit_e(small_int()), rhs);
					break;
				}
				}
				c.body.push_back(decl_s(c.local_t, c.local, init, false));
			}
			for (const placed& t : targets) {
				eptr delta;
				if (!c.local.empty() && rng_.chance(35)) { delta = read_e(var_p(c.local, false)); }
				else if (!c.params.empty() && rng_.chance(40) && c.params[0].t != ty::b && !c.params[0].ref) { delta = read_e(var_p(c.params[0].name, false)); }
				else { delta = lit_e(t.t == ty::i ? mi(rng_.range(1, 5)) : t.t == ty::f ? small_float() : literal_of(ty::v)); }
				sptr st;
				if (t.t == ty::b) {
					st = assign_s(t.p, rng_.chance(50) ? un_e(ek::lnot, read_e(t.p)) : lit_e(mb(rng_.chance(50))));
				} else {
					switch (rng_.below(4)) {
					case 0: st = assign_s(t.p, bin_e("-", delta, read_e(t.p))); break;
					case 1: st = compound_s(t.p, rng_.chance(50) ? "+" : "-", delta); break;
					default: st = assign_s(t.p, bin_e(rng_.chance(70) ? "+" : "-", read_e(t.p), delta)); break;
					}
				}
				if (!c.params.empty() && !c.params[0].ref && c.params[0].t == ty::i && rng_.chance(20)) {
					auto s = std::make_shared<stmt>();
					s->kind = sk::if_;
					s->e = bin_e(">", read_e(var_p(c.params[0].name, false)), lit_e(mi(rng_.range(0, 3))));
					s->body.push_back(st);
					c.body.push_back(s);
				} else {
					c.body.push_back(st);
				}
				c.writes.push_back(t.p);
			}
		}
		eptr rec_call() {
			auto e = call0(rec_);
			e->args.push_back(lit_e(mi(rng_.range(0, 1))));
			return e;
		}
		// what the writer hands back: a literal, a target read after the store, a parameter, a comparison (bool) or a ternary
		eptr writer_result(callable& c, const std::vector<placed>& targets) {
			const placed& t = targets[rng_.below(static_cast<uint32_t>(targets.size()))];
			if (c.result == ty::b) {
				if (t.t == ty::b) return read_e(t.p);
				const eptr bound = lit_e(literal_of(t.t == ty::f ? ty::f : ty::i));
				return bin_e(rng_.chance(50) ? ">" : "<=", read_e(t.p), bound);
			}
			if (!c.local.empty() && rng_.chance(35)) { return rng_.chance(50) ? read_e(var_p(c.local, false)) : bin_e("*", read_e(var_p(c.local, false)), lit_e(mi(2))); }
			switch (pick({30, 35, 15, 20})) {
			case 0: return lit_e(literal_of(c.result == ty::f ? ty::f : ty::i));
			case 1: if (t.t != ty::b) return read_e(t.p); return lit_e(small_int());
			case 2: if (!c.params.empty() && !c.params[0].ref && c.params[0].t != ty::b) return bin_e("*", read_e(var_p(c.params[0].name, false)), lit_e(mi(2))); return lit_e(small_int());
			default: {
				const eptr no = lit_e(mi(rng_.range(0, 5)));
				const eptr yes = lit_e(mi(rng_.range(0, 5)));
				return tern_e(t.t == ty::b ? read_e(t.p) : bin_e(">", read_e(t.p), lit_e(mi(2))), yes, no);
			}
			}
		}
		std::vector<placed> global_targets(int n) {
			scope_info* keep = local_;
			local_ = nullptr;
			std::vector<placed> out;
			for (int k = 0; k < n; ++k) {
				auto opts = readable(rng_.chance(15));
				if (opts.empty()) { opts = readable(false); }
				if (opts.empty()) break;
				placed t = opts[rng_.below(static_cast<uint32_t>(opts.size()))];
				if (t.p.kind == pk::elem) { t.p.index = lit_e(mi(literal_index(t.p.name))); }
				out.push_back(t);
			}
			local_ = keep;
			return out;
		}
		int64_t literal_index(const std::string& array) {
			for (const auto& a : g_.arrays) { if (a.name == array) return rng_.range(0, a.len - 1); }
			return 0;
		}
		void emit_writer() {
			callable c;
			c.name = fresh("w");
			const std::vector<placed> targets = global_targets(static_cast<int>(rng_.range(1, 2)));
			if (targets.empty()) return;
			if (rng_.chance(40)) { c.params.push_back({"p", rng_.chance(75) ? ty::i : ty::f}); }
			switch (block_ ? pick({40, 25, 5, 15, 5, 10}) : pick({40, 18, 14, 10, 10, 8})) {
			case 0: c.ret = ty::i; break;
			case 1: c.ret = ty::f; break;
			case 2: c.ret = ty::untyped; break;
			case 3: c.ret = ty::b; break;
			case 4: c.ret = ty::v; break;
			default: c.ret = ty::none; break;
			}
			c.result = c.ret == ty::untyped || c.ret == ty::v ? (rng_.chance(70) ? ty::i : ty::f) : c.ret;
			writer_body(c, targets, false);
			if (c.ret != ty::none) { c.body.push_back(ret_s(writer_result(c, targets))); }
			p_.calls.push_back(c);
			writers_.push_back(static_cast<int>(p_.calls.size() - 1));
			p_.functions.push_back(writers_.back());
		}
		void emit_ref_writer() {
			callable c;
			c.name = fresh("r");
			c.params.push_back({"x", ty::i, true});
			c.ret = rng_.chance(80) ? ty::i : ty::untyped;
			c.result = ty::i;
			c.writes_ref = true;
			if (rng_.chance(20)) { c.body.push_back(eval_s(rec_call())); }
			c.body.push_back(assign_s(var_p("x", false), bin_e("+", read_e(var_p("x", false)), lit_e(mi(rng_.range(1, 6))))));
			c.body.push_back(ret_s(rng_.chance(50) ? lit_e(mi(rng_.range(1, 4))) : read_e(var_p("x", false))));
			p_.calls.push_back(c);
			refs_.push_back(static_cast<int>(p_.calls.size() - 1));
			p_.functions.push_back(refs_.back());
		}
		void emit_pure() {
			callable c;
			c.name = fresh("q");
			const ty pt = rng_.chance(75) ? ty::i : ty::f;
			c.params.push_back({"a", pt});
			if (rng_.chance(50)) { c.params.push_back({"b", ty::i}); }
			c.ret = pt;
			c.result = pt;
			eptr body = bin_e("*", read_e(var_p("a", false)), lit_e(pt == ty::i ? mi(rng_.range(2, 3)) : mf(0.5)));
			if (c.params.size() > 1) { body = bin_e("+", body, read_e(var_p("b", false))); }
			if (rng_.chance(50)) {
				const ty lt = pt == ty::i ? ty::f : ty::i;   // the other kind: the declaration converts
				c.body.push_back(decl_s(lt, "t", bin_e("+", read_e(var_p("a", false)), lit_e(lt == ty::f ? mf(0.5) : mi(1))), false));
				body = bin_e("+", body, read_e(var_p("t", false)));
			}
			c.body.push_back(ret_s(body));
			p_.calls.push_back(c);
			pures_.push_back(static_cast<int>(p_.calls.size() - 1));
			p_.functions.push_back(pures_.back());
		}

		// ---------------- drivers

		int emit_driver(int d) {
			callable c;
			c.name = "d" + std::to_string(d);
			c.ret = ty::none;
			scope_info s;
			local_ = &s;
			std::vector<sptr>& b = c.body;
			const int scalars = static_cast<int>(rng_.range(2, 4));
			for (int k = 0; k < scalars; ++k) {
				static constexpr ty kinds[] = {ty::i, ty::i, ty::f, ty::v, ty::a, ty::b};
				static constexpr ty block_kinds[] = {ty::i, ty::i, ty::f, ty::f, ty::a, ty::i};
				const ty t = block_ ? block_kinds[rng_.below(6)] : kinds[rng_.below(6)];
				store_info v{fresh("x"), t == ty::a ? ty::i : t, false};
				b.push_back(decl_s(t, v.name, lit_e(t == ty::a ? small_int() : literal_of(t)), false));
				s.scalars.push_back(v);
			}
			if (rng_.chance(50)) {
				store_info j{fresh("j"), ty::i, false};
				j.index_var = true;
				b.push_back(decl_s(ty::i, j.name, lit_e(mi(rng_.range(0, 2))), false));
				s.scalars.push_back(j);
			}
			if (rng_.chance(50)) {
				const ty t = rng_.chance(70) ? ty::i : (rng_.chance(block_ ? 100 : 50) ? ty::f : ty::v);
				store_info a{fresh("la"), t, false, static_cast<int>(rng_.range(3, 5))};
				auto st = std::make_shared<stmt>();
				st->kind = sk::arr_decl; st->t = t; st->name = a.name;
				for (int i = 0; i < a.len; ++i) { st->list.push_back(lit_e(literal_of(t))); }
				b.push_back(st);
				s.arrays.push_back(a);
			}
			if (!p_.classes.empty() && rng_.chance(40)) {
				store_info o{fresh("lo"), ty::v, false};
				auto st = std::make_shared<stmt>();
				st->kind = sk::obj_decl; st->tag = "K0"; st->name = o.name;
				b.push_back(st);
				s.objects.push_back(o);
			}
			if (rng_.chance(40)) {
				store_info v{fresh("xs"), ty::s, false};
				b.push_back(decl_s(ty::s, v.name, lit_e(string_literal()), false));
				s.strings.push_back(v);
			}
			const int lambdas = static_cast<int>(rng_.range(1, 3));
			for (int k = 0; k < lambdas; ++k) { emit_lambda(b); }
			b.push_back(decl_s(ty::i, "ai", lit_e(mi(0)), false));
			b.push_back(decl_s(ty::f, "af", lit_e(mf(0.0)), false));
			b.push_back(decl_s(ty::v, "av", lit_e(mi(0)), false));
			b.push_back(decl_s(ty::s, "as", lit_e(ms("")), false));
			const int shapes = static_cast<int>(rng_.range(1, 2));
			for (int k = 0; k < shapes; ++k) {
				switch (pick({38, 26, 36})) {
				case 0: emit_loop(b, sk::for_); break;
				case 1: emit_loop(b, sk::while_); break;
				default: emit_span(b); break;
				}
			}
			std::vector<eptr> out{read_e(var_p("ai", false)), read_e(var_p("af", false)), read_e(var_p("av", false)), read_e(var_p("as", false))};
			for (const auto& v : s.strings) { out.push_back(read_e(var_p(v.name, false))); }
			for (const auto& v : s.scalars) { out.push_back(read_e(var_p(v.name, false))); }
			for (const auto& a : s.arrays) { for (int i = 0; i < a.len; ++i) { out.push_back(read_e(elem_p(a.name, false, lit_e(mi(i))))); } }
			for (const auto& o : s.objects) { out.push_back(read_e(field_p(o.name, false, "v"))); out.push_back(read_e(field_p(o.name, false, "w"))); }
			b.push_back(print_s(c.name, out));
			local_ = nullptr;
			p_.calls.push_back(c);
			p_.functions.push_back(static_cast<int>(p_.calls.size() - 1));
			return static_cast<int>(p_.calls.size() - 1);
		}

		void emit_lambda(std::vector<sptr>& b) {
			callable c;
			c.kind = ck::lambda;
			c.name = fresh("L");
			if (!local_->strings.empty() && rng_.chance(35)) {
				// appends to a captured string and hands back a string
				c.by_ref = true;
				c.ret = rng_.chance(70) ? ty::untyped : ty::s;
				c.result = ty::s;
				const place t = var_p(local_->strings[rng_.below(static_cast<uint32_t>(local_->strings.size()))].name, false);
				c.body.push_back(assign_s(t, bin_e("+", read_e(t), lit_e(string_literal()))));
				c.body.push_back(ret_s(rng_.chance(50) ? read_e(t) : lit_e(string_literal())));
				c.writes.push_back(t);
				p_.calls.push_back(c);
				const int idx = static_cast<int>(p_.calls.size() - 1);
				auto st = std::make_shared<stmt>();
				st->kind = sk::lambda_decl; st->callee = idx;
				b.push_back(st);
				local_->string_lambdas.push_back(idx);
				return;
			}
			c.by_ref = rng_.chance(block_ ? 80 : 70);
			c.ret = rng_.chance(block_ ? 55 : 75) ? ty::untyped : (rng_.chance(50) ? ty::i : ty::f);
			std::vector<placed> targets;
			if (c.by_ref) {
				std::vector<placed> mine;
				for (const auto& v : local_->scalars) { mine.push_back({var_p(v.name, false), v.t}); }
				for (const auto& a : local_->arrays) { mine.push_back({elem_p(a.name, false, lit_e(mi(rng_.range(0, a.len - 1)))), a.t}); }
				for (const auto& o : local_->objects) { mine.push_back({field_p(o.name, false, rng_.chance(60) ? "v" : "w"), ty::i}); if (mine.back().p.field == "w") mine.back().t = ty::f; }
				if (!mine.empty() && rng_.chance(80)) {
					placed t = mine[rng_.below(static_cast<uint32_t>(mine.size()))];
					for (const auto& v : local_->scalars) { if (t.p.kind == pk::var && v.name == t.p.name && v.index_var) t.t = ty::i; }
					targets.push_back(t);
				}
			}
			c.result = c.ret == ty::untyped ? (rng_.chance(70) ? ty::i : ty::f) : c.ret;
			if (!c.by_ref) {
				// a by-value closure snapshots every name it spells, a global included (both backends; docs/site/guide/06-functions.html "Lambdas and captures" says a
				// global is never captured): it writes through a free writer, whose globals are live, and reads a captured local
				std::vector<int> ws;
				for (int w : writers_) { if (p_.calls[static_cast<size_t>(w)].params.empty()) ws.push_back(w); }
				if (ws.empty()) return;
				const int w = ws[rng_.below(static_cast<uint32_t>(ws.size()))];
				c.body.push_back(eval_s(call0(w)));
				for (const place& t : p_.calls[static_cast<size_t>(w)].writes) c.writes.push_back(t);
				std::vector<const store_info*> nums;
				for (const auto& v : local_->scalars) { if (v.t == c.result || (c.ret == ty::untyped && numeric(v.t) && v.t != ty::v)) nums.push_back(&v); }
				if (!nums.empty() && rng_.chance(70)) c.body.push_back(ret_s(read_e(var_p(nums[rng_.below(static_cast<uint32_t>(nums.size()))]->name, false))));
				else c.body.push_back(ret_s(lit_e(literal_of(c.result))));
				p_.calls.push_back(c);
				const int idx = static_cast<int>(p_.calls.size() - 1);
				auto st = std::make_shared<stmt>();
				st->kind = sk::lambda_decl; st->callee = idx;
				b.push_back(st);
				local_->lambdas.push_back(idx);
				return;
			}
			if (targets.empty() || rng_.chance(25)) {
				auto g = global_targets(1);
				for (auto& t : g) targets.push_back(t);
			}
			if (targets.empty()) return;
			for (const placed& t : targets) {
				bool idx = false;
				for (const auto& v : local_->scalars) { if (t.p.kind == pk::var && v.name == t.p.name && v.index_var) idx = true; }
				sptr st;
				if (idx) { st = assign_s(t.p, bin_e("+", read_e(t.p), lit_e(mi(rng_.range(1, 3))))); }
				else if (t.t == ty::b) { st = assign_s(t.p, un_e(ek::lnot, read_e(t.p))); }
				else {
					const mval d = t.t == ty::i ? mi(rng_.range(1, 5)) : t.t == ty::f ? small_float() : literal_of(ty::v);
					st = rng_.chance(70) ? assign_s(t.p, bin_e(rng_.chance(70) ? "+" : "-", read_e(t.p), lit_e(d))) : assign_s(t.p, bin_e("-", lit_e(d), read_e(t.p)));
				}
				c.body.push_back(st);
				c.writes.push_back(t.p);
			}
			const placed& t = targets.front();
			if (c.ret == ty::untyped) {
				if (t.t == c.result && rng_.chance(50)) { c.body.push_back(ret_s(read_e(t.p))); }
				else { c.body.push_back(ret_s(lit_e(literal_of(c.result)))); }
			} else {
				c.body.push_back(ret_s(t.t != ty::b && rng_.chance(50) ? read_e(t.p) : lit_e(literal_of(c.result))));
			}
			p_.calls.push_back(c);
			const int idx = static_cast<int>(p_.calls.size() - 1);
			auto st = std::make_shared<stmt>();
			st->kind = sk::lambda_decl; st->callee = idx;
			b.push_back(st);
			local_->lambdas.push_back(idx);
		}

		// a writer call for an operand: a free writer, a lambda of this driver, a method of an object in reach, or a reference writer
		// over an int place; `writes` is what it stores
		bool writer_call(bool want_bool, bool allow_void, wcall& out) {
			std::vector<std::pair<int, int>> cands;   // (kind, index)
			for (int w : writers_) {
				const callable& c = p_.calls[static_cast<size_t>(w)];
				if (c.ret == ty::none && !allow_void) continue;
				if (want_bool != (c.result == ty::b) && c.ret != ty::none) continue;
				cands.push_back({0, w});
			}
			if (local_ && !want_bool) { for (int l : local_->lambdas) cands.push_back({1, l}); }
			if (!want_bool) {
				for (const klass& k : p_.classes) { for (int m : k.methods) cands.push_back({2, m}); }
				for (int r : refs_) cands.push_back({3, r});
			}
			if (cands.empty()) return false;
			const auto [kind, index] = cands[rng_.below(static_cast<uint32_t>(cands.size()))];
			const callable& c = p_.calls[static_cast<size_t>(index)];
			auto e = call0(index);
			out.result = c.result;
			if (kind == 2) {
				std::vector<std::pair<std::string, bool>> objs;
				if (local_) { for (const auto& o : local_->objects) objs.push_back({o.name, false}); }
				for (const auto& o : g_.objects) objs.push_back({o.name, true});
				if (objs.empty()) return false;
				const auto& o = objs[rng_.below(static_cast<uint32_t>(objs.size()))];
				e->recv = o.first; e->recv_global = o.second;
				for (const place& w : c.writes) { out.writes.push_back({field_p(o.first, o.second, w.field), w.field == "v" ? ty::i : ty::f}); }
			} else if (kind == 3) {
				auto opts = readable(false, true);
				if (opts.empty()) return false;
				placed t = opts[rng_.below(static_cast<uint32_t>(opts.size()))];
				e->args.push_back(read_e(t.p));
				out.writes.push_back(t);
			} else {
				for (const place& w : c.writes) {
					placed t{w, ty::i};
					t.t = place_type(w);
					out.writes.push_back(t);
				}
			}
			for (size_t i = e->args.size(); i < c.params.size(); ++i) { e->args.push_back(plain_arg(c.params[i].t)); }
			out.e = e;
			return true;
		}
		ty place_type(const place& p) {
			auto from = [&](scope_info& s) -> ty {
				for (const auto& v : s.scalars) if (p.kind == pk::var && v.name == p.name) return v.t;
				for (const auto& a : s.arrays) if (p.kind == pk::elem && a.name == p.name) return a.t;
				if (p.kind == pk::field) return p.field == "v" ? ty::i : ty::f;
				if (p.kind == pk::map) return ty::i;
				return ty::v;
			};
			if (!p.global && local_) return from(*local_);
			return from(g_);
		}
		// an argument that no call in the statement can change: a literal of the parameter's type or a local of it
		eptr plain_arg(ty t) {
			if (local_ && rng_.chance(40)) {
				for (const auto& v : local_->scalars) { if (v.t == t && !v.index_var) return read_e(var_p(v.name, false)); }
			}
			return lit_e(literal_of(t == ty::b ? ty::i : t));
		}

		// a read of what the writer writes (mostly), else of any storage of the wanted class
		placed read_for(const wcall& w, bool want_bool) {
			std::vector<placed> hits;
			for (const auto& t : w.writes) { if ((t.t == ty::b) == want_bool) hits.push_back(t); }
			if (!hits.empty() && rng_.chance(75)) {
				placed t = hits[rng_.below(static_cast<uint32_t>(hits.size()))];
				if (t.p.kind == pk::elem && rng_.chance(40)) {
					for (const auto& a : (t.p.global ? g_.arrays : local_->arrays)) { if (a.name == t.p.name) t.p.index = index_for(a.len); }
				}
				return t;
			}
			auto opts = readable(want_bool);
			if (opts.empty()) return {lit_place(), want_bool ? ty::b : ty::i};
			return opts[rng_.below(static_cast<uint32_t>(opts.size()))];
		}
		place lit_place() { return var_p("", true); }
		eptr read_of(const placed& p, bool want_bool) {
			if (p.p.name.empty() && p.p.kind == pk::var) return lit_e(want_bool ? mb(true) : small_int());
			if (!want_bool && rng_.chance(35)) {
				for (const auto& g : getters_) { if (same_place(g.second, p.p)) return call0(g.first); }
			}
			return read_e(p.p);
		}

		const char* arith_op() { static constexpr const char* ops[] = {"+", "-", "*", "+", "-"}; return ops[rng_.below(5)]; }
		const char* cmp_op() { static constexpr const char* ops[] = {"<", "<=", ">", ">=", "==", "!="}; return ops[rng_.below(6)]; }

		// the operand patterns around a writing call (the numeric ones; want_bool builds a comparison or a logical)
		eptr hazard(bool want_bool) {
			wcall w;
			if (want_bool) {
				switch (pick({50, 30, 20})) {
				case 0: {
					if (!writer_call(false, false, w)) return filler_value(true);
					const placed r = read_for(w, false);
					const bool read_left = rng_.chance(50);
					const eptr rd = read_of(r, false);
					const char* op = cmp_op();
					return read_left ? bin_e(op, rd, w.e) : bin_e(op, w.e, rd);
				}
				case 1: {
					if (!writer_call(true, false, w)) return filler_value(true);
					const placed r = read_for(w, true);
					const eptr rb = read_of(r, true);
					return rng_.chance(50) ? bin_e(rng_.chance(50) ? "&&" : "||", rb, w.e) : bin_e(rng_.chance(50) ? "&&" : "||", w.e, rb);
				}
				default: {
					if (!writer_call(false, false, w)) return filler_value(true);
					const placed r = read_for(w, false);
					const eptr right = read_of(r, false);
					const eptr left = read_of(r, false);
					return bin_e(cmp_op(), bin_e("+", left, w.e), right);
				}
				}
			}
			const bool bool_writer = rng_.chance(15);
			if (!writer_call(bool_writer, false, w)) {
				if (!bool_writer || !writer_call(false, false, w)) return filler_value(false);
			}
			const placed r = read_for(w, false);
			const eptr R = read_of(r, false);
			eptr W = w.e;
			if (w.result == ty::b) {
				const eptr no = lit_e(mi(rng_.range(5, 9)));
				const eptr yes = lit_e(mi(rng_.range(1, 4)));
				W = tern_e(w.e, yes, no);
			}
			switch (pick({16, 12, 12, 8, 10, 8, 6, 8, 5, 6, 5, 4})) {
			case 0: return bin_e(arith_op(), R, W);
			case 1: return bin_e(arith_op(), W, R);
			case 2: {
				const eptr twice = bin_e("*", read_of(r, false), lit_e(mi(2)));
				const eptr inner = bin_e(arith_op(), twice, W);
				return bin_e(arith_op(), R, inner);
			}
			case 3: {
				const eptr right = read_of(r, false);
				const eptr inner = bin_e(arith_op(), R, W);
				return bin_e(arith_op(), inner, right);
			}
			case 4: {
				eptr cond = w.result == ty::b ? w.e : bin_e(">", w.e, lit_e(mi(rng_.range(0, 4))));
				const eptr no = lit_e(literal_of(r.t == ty::f ? ty::f : ty::i));
				const eptr yes = lit_e(literal_of(r.t == ty::f ? ty::f : ty::i));
				eptr t = tern_e(cond, yes, no);
				if (rng_.chance(40)) {
					const eptr twice = bin_e("*", read_of(r, false), lit_e(mi(2)));
					const eptr inner = bin_e(arith_op(), twice, t);
					return bin_e(arith_op(), R, inner);
				}
				return rng_.chance(50) ? bin_e(arith_op(), R, t) : bin_e(arith_op(), t, R);
			}
			case 5: {
				auto opts = readable(true);
				eptr c = opts.empty() ? bin_e(">", read_of(r, false), lit_e(mi(2))) : read_e(opts[rng_.below(static_cast<uint32_t>(opts.size()))].p);
				const eptr no = lit_e(small_int());
				return bin_e(arith_op(), R, tern_e(c, W, no));
			}
			case 6: return bin_e(arith_op(), R, un_e(ek::neg, W));
			case 7: {
				switch (rng_.below(5)) {
				case 0: return bin_e(arith_op(), math_e(ek::math, "abs", {R}), W);
				case 1: return bin_e(arith_op(), R, math_e(ek::math, "abs", {W}));
				case 2: return bin_e(arith_op(), math_e(ek::alias, "sqrt", {math_e(ek::math, "abs", {R})}), W);
				case 3: {
					const eptr rounded = math_e(ek::alias, rng_.chance(50) ? "abs" : "floor", {R});
					return bin_e(arith_op(), rounded, W);
				}
				default: {
					const eptr truncated = math_e(ek::math, rng_.chance(50) ? "itrunc" : "ifloor", {bin_e("*", W, lit_e(mf(0.5)))});
					return bin_e(arith_op(), R, truncated);
				}
				}
			}
			case 8: {
				wcall w2;
				if (!writer_call(false, false, w2)) return bin_e(arith_op(), R, W);
				const eptr inner = bin_e(arith_op(), R, W);
				return bin_e(arith_op(), inner, w2.e);
			}
			case 9: {
				if (pures_.empty()) return bin_e(arith_op(), W, R);
				const int q = pures_[rng_.below(static_cast<uint32_t>(pures_.size()))];
				const callable& qc = p_.calls[static_cast<size_t>(q)];
				auto call = call0(q);
				for (size_t i = 0; i < qc.params.size(); ++i) { call->args.push_back(i == 0 && rng_.chance(60) ? read_of(r, false) : plain_arg(qc.params[i].t)); }
				return bin_e(arith_op(), W, call);
			}
			case 10: {
				auto opts = readable(false);
				if (opts.empty()) return bin_e(arith_op(), R, W);
				const placed r2 = opts[rng_.below(static_cast<uint32_t>(opts.size()))];
				const eptr right = read_of(r2, false);
				const eptr inner = bin_e(arith_op(), R, right);
				return bin_e(arith_op(), inner, W);
			}
			default: {
				if (!args_) return bin_e(arith_op(), R, W);
				switch (rng_.below(3)) {
				case 0: return math_e(ek::math, rng_.chance(50) ? "min" : "max", {R, W});
				case 1: {
					if (pures_.empty()) return math_e(ek::math, "max", {R, W});
					const int q = pures_[rng_.below(static_cast<uint32_t>(pures_.size()))];
					const callable& qc = p_.calls[static_cast<size_t>(q)];
					auto call = call0(q);
					call->args.push_back(R);
					for (size_t i = 1; i < qc.params.size(); ++i) { call->args.push_back(W); }
					if (qc.params.size() == 1) return bin_e("+", call, W);
					return call;
				}
				default: return math_e(ek::math, "max", {W, R});
				}
			}
			}
		}

		// scalar work with no call: arithmetic over locals, literals and global reads
		eptr filler_value(bool want_bool, bool scalars_only = false) {
			auto opts = readable(want_bool);
			if (scalars_only) {
				std::vector<placed> keep;
				for (const auto& o : opts) { if (o.p.kind == pk::var) keep.push_back(o); }
				opts = keep;
			}
			auto leaf = [&]() -> eptr {
				if (!opts.empty() && rng_.chance(70)) return read_e(opts[rng_.below(static_cast<uint32_t>(opts.size()))].p);
				return lit_e(want_bool ? mb(rng_.chance(50)) : small_int());
			};
			if (want_bool) {
				auto nums = readable(false);
				if (nums.empty() || rng_.chance(30)) return leaf();
				const eptr bound = lit_e(small_int());
				const eptr num = read_e(nums[rng_.below(static_cast<uint32_t>(nums.size()))].p);
				return bin_e(cmp_op(), num, bound);
			}
			eptr e = leaf();
			const int n = static_cast<int>(rng_.range(0, 2));
			for (int k = 0; k < n; ++k) {
				const eptr next = leaf();
				e = bin_e(arith_op(), e, next);
			}
			if (rng_.chance(15)) e = math_e(ek::math, "abs", {e});
			return e;
		}

		// ---------------- statements

		void accumulate(std::vector<sptr>& b, const std::string& name, ty t) {
			if (t == ty::b) {
				auto s = std::make_shared<stmt>();
				s->kind = sk::if_;
				s->e = read_e(var_p(name, false));
				s->body.push_back(assign_s(var_p("ai", false), bin_e("+", read_e(var_p("ai", false)), lit_e(mi(1)))));
				b.push_back(s);
				return;
			}
			if (t == ty::i) { b.push_back(assign_s(var_p("ai", false), bin_e("%", bin_e("+", bin_e("*", read_e(var_p("ai", false)), lit_e(mi(7))), read_e(var_p(name, false))), lit_e(mi(100003))))); return; }
			if (t == ty::f) { b.push_back(assign_s(var_p("af", false), bin_e("+", read_e(var_p("af", false)), read_e(var_p(name, false))))); return; }
			b.push_back(assign_s(var_p("av", false), bin_e("+", read_e(var_p("av", false)), read_e(var_p(name, false)))));
		}
		place writable_place(bool want_bool) {
			auto opts = readable(want_bool);
			std::vector<placed> ok;
			for (const auto& o : opts) { if (!(o.p.kind == pk::var && is_index_var(o.p.name))) ok.push_back(o); }
			if (ok.empty()) return var_p("av", false);
			return ok[rng_.below(static_cast<uint32_t>(ok.size()))].p;
		}
		bool is_index_var(const std::string& n) const {
			if (!local_) return false;
			for (const auto& v : local_->scalars) { if (v.name == n && v.index_var) return true; }
			return false;
		}

		void hazard_statement(std::vector<sptr>& b, bool in_function) {
			if (strings_in_reach() && rng_.chance(18)) { string_statement(b, in_function); return; }
			switch (block_ ? pick({38, 18, 14, 10, 0, 0, 12, 8}) : pick({34, 16, 14, 10, 7, 6, 6, 7})) {
			case 0: {
				const bool bl = rng_.chance(20);
				static constexpr ty num_types[] = {ty::i, ty::i, ty::f, ty::v, ty::a};
				static constexpr ty bool_types[] = {ty::b, ty::b, ty::v, ty::a};
				const ty t = bl ? bool_types[rng_.below(4)] : num_types[rng_.below(5)];
				const std::string n = fresh("r");
				eptr e = hazard(bl);
				b.push_back(decl_s(t, n, e, !in_function));
				if (in_function) { accumulate(b, n, t == ty::a || t == ty::v ? (bl ? ty::b : ty::v) : t); }
				else { printed_.push_back(n); }
				return;
			}
			case 1: {
				wcall w;
				if (!writer_call(false, false, w)) { hazard_statement_decl(b, in_function); return; }
				const placed r = read_for(w, false);
				if (r.p.name.empty()) { hazard_statement_decl(b, in_function); return; }
				if (r.p.kind == pk::var && is_index_var(r.p.name)) { hazard_statement_decl(b, in_function); return; }
				b.push_back(assign_s(r.p, rng_.chance(50) ? bin_e(rng_.chance(70) ? "+" : "-", read_e(r.p), w.e) : bin_e("+", w.e, read_e(r.p))));
				return;
			}
			case 2: {
				wcall w;
				if (!writer_call(false, false, w)) { hazard_statement_decl(b, in_function); return; }
				const placed r = read_for(w, false);
				if (r.p.name.empty() || (r.p.kind == pk::var && is_index_var(r.p.name))) { hazard_statement_decl(b, in_function); return; }
				const eptr rhs = rng_.chance(70) ? w.e : bin_e("+", w.e, read_e(r.p));
				b.push_back(compound_s(r.p, rng_.chance(70) ? "+" : "-", rhs));
				return;
			}
			case 3: {
				auto s = std::make_shared<stmt>();
				s->kind = sk::if_;
				s->e = hazard(true);
				const place t = writable_place(false);
				s->body.push_back(assign_s(t, bin_e("+", read_e(t), lit_e(mi(1)))));
				if (rng_.chance(50)) { s->other.push_back(assign_s(var_p(in_function ? "ai" : t.name, !in_function && t.global), bin_e("-", read_e(var_p(in_function ? "ai" : t.name, !in_function && t.global)), lit_e(mi(1))))); if (!in_function && t.kind != pk::var) s->other.clear(); }
				b.push_back(s);
				return;
			}
			case 4: {
				wcall w;
				if (!writer_call(false, false, w)) { hazard_statement_decl(b, in_function); return; }
				const placed r = read_for(w, false);
				auto s = std::make_shared<stmt>();
				s->kind = sk::arr_decl; s->t = ty::v; s->name = fresh("t"); s->global = !in_function;
				s->list = {read_of(r, false), w.e, read_of(r, false)};
				b.push_back(s);
				if (in_function) {
					for (int i = 0; i < 3; ++i) b.push_back(assign_s(var_p("av", false), bin_e("+", read_e(var_p("av", false)), read_e(elem_p(s->name, false, lit_e(mi(i)))))));
				} else {
					for (int i = 0; i < 3; ++i) printed_elems_.push_back({s->name, i});
				}
				return;
			}
			case 5: {
				wcall w;
				if (!writer_call(false, false, w)) { hazard_statement_decl(b, in_function); return; }
				const placed r = read_for(w, false);
				auto s = std::make_shared<stmt>();
				s->kind = sk::map_decl; s->t = ty::v; s->name = fresh("mm"); s->global = !in_function;
				s->keys = {"p", "q", "r"};
				s->list = {read_of(r, false), w.e, read_of(r, false)};
				b.push_back(s);
				if (in_function) {
					for (const char* k : {"p", "q", "r"}) b.push_back(assign_s(var_p("av", false), bin_e("+", read_e(var_p("av", false)), read_e(map_p(s->name, false, k)))));
				} else {
					for (const char* k : {"p", "q", "r"}) printed_keys_.push_back({s->name, k});
				}
				return;
			}
			case 6: {
				// the store's target index is read after its right side: a lambda that moves the index, or any writer
				std::vector<std::pair<const store_info*, bool>> arrays;
				if (local_) { for (const auto& a : local_->arrays) arrays.push_back({&a, false}); }
				for (const auto& a : g_.arrays) arrays.push_back({&a, true});
				wcall w;
				if (arrays.empty() || !writer_call(false, false, w)) { hazard_statement_decl(b, in_function); return; }
				const auto [a, global] = arrays[rng_.below(static_cast<uint32_t>(arrays.size()))];
				const place t = elem_p(a->name, global, index_for(a->len));
				if (rng_.chance(60)) b.push_back(assign_s(t, w.e));
				else b.push_back(compound_s(t, rng_.chance(50) ? "+" : "-", w.e));
				return;
			}
			default: {
				wcall w;
				if (!writer_call(false, true, w)) { hazard_statement_decl(b, in_function); return; }
				b.push_back(eval_s(w.e));
				return;
			}
			}
		}
		void hazard_statement_decl(std::vector<sptr>& b, bool in_function) {
			const std::string n = fresh("r");
			b.push_back(decl_s(ty::v, n, hazard(false), !in_function));
			if (in_function) accumulate(b, n, ty::v); else printed_.push_back(n);
		}
		void filler_statement(std::vector<sptr>& b, bool scalars_only = false) {
			if (!local_->scalars.empty() && rng_.chance(35)) {
				std::vector<const store_info*> nums;
				for (const auto& v : local_->scalars) { if (v.t != ty::b && !v.index_var) nums.push_back(&v); }
				if (!nums.empty()) {
					const store_info* v = nums[rng_.below(static_cast<uint32_t>(nums.size()))];
					const eptr step = lit_e(v->t == ty::f ? small_float() : mi(rng_.range(1, 4)));
					b.push_back(assign_s(var_p(v->name, false), bin_e(rng_.chance(50) ? "+" : "-", read_e(var_p(v->name, false)), step)));
					return;
				}
			}
			static constexpr ty types[] = {ty::i, ty::i, ty::f, ty::v, ty::a};
			static constexpr ty block_types[] = {ty::i, ty::i, ty::f, ty::f, ty::a};
			const ty t = block_ ? block_types[rng_.below(5)] : types[rng_.below(5)];
			const std::string n = fresh("s");
			b.push_back(decl_s(t, n, filler_value(false, scalars_only), false));
			local_->scalars.push_back({n, t == ty::a ? ty::v : t, false});
		}

		void emit_loop(std::vector<sptr>& b, sk kind) {
			auto s = std::make_shared<stmt>();
			s->kind = kind;
			s->name = fresh("i");
			s->n = static_cast<int>(rng_.range(2, 6));
			local_->ivar = s->name;
			local_->ivar_n = s->n;
			const size_t mark = local_->scalars.size();
			const int stmts = static_cast<int>(rng_.range(2, 5));
			for (int k = 0; k < stmts; ++k) {
				if (rng_.chance(70)) hazard_statement(s->body, true);
				else filler_statement(s->body);
			}
			if (!block_ && rng_.chance(25)) {
				std::vector<eptr> out{read_e(var_p(s->name, false)), read_e(var_p("ai", false))};
				s->body.push_back(print_s("it", out));
			}
			local_->scalars.resize(mark);
			local_->ivar.clear();
			b.push_back(s);
		}
		void emit_span(std::vector<sptr>& b) {
			const int n = static_cast<int>(rng_.range(16, 26));
			const int hazards = static_cast<int>(rng_.range(1, 3));
			std::vector<int> at;
			for (int k = 0; k < hazards; ++k) at.push_back(static_cast<int>(rng_.range(2, n - 2)));
			for (int k = 0; k < n; ++k) {
				bool h = false;
				for (int a : at) h |= a == k;
				if (h) hazard_statement(b, true);
				else filler_statement(b, block_ && rng_.chance(85));
			}
		}

		std::vector<std::string> printed_;
		std::vector<std::pair<std::string, int>> printed_elems_;
		std::vector<std::pair<std::string, std::string>> printed_keys_;

		void emit_top_hazards() {
			const int n = static_cast<int>(rng_.range(1, 3));
			for (int k = 0; k < n; ++k) hazard_statement(p_.top, false);
		}
		void emit_final_prints() {
			std::vector<eptr> g;
			for (const auto& v : g_.scalars) g.push_back(read_e(var_p(v.name, true)));
			for (const auto& v : g_.strings) g.push_back(read_e(var_p(v.name, true)));
			p_.top.push_back(print_s("G", g));
			for (const auto& a : g_.arrays) {
				std::vector<eptr> e;
				for (int i = 0; i < a.len; ++i) e.push_back(read_e(elem_p(a.name, true, lit_e(mi(i)))));
				p_.top.push_back(print_s(a.name, e));
			}
			if (has_map_) p_.top.push_back(print_s("M0", {read_e(map_p("M0", true, "a")), read_e(map_p("M0", true, "b")), read_e(map_p("M0", true, "c"))}));
			for (const auto& o : g_.objects) p_.top.push_back(print_s(o.name, {read_e(field_p(o.name, true, "v")), read_e(field_p(o.name, true, "w"))}));
			std::vector<eptr> t;
			for (const auto& n : printed_) t.push_back(read_e(var_p(n, true)));
			for (const auto& [n, i] : printed_elems_) t.push_back(read_e(elem_p(n, true, lit_e(mi(i)))));
			for (const auto& [n, k] : printed_keys_) t.push_back(read_e(map_p(n, true, k)));
			if (!t.empty()) p_.top.push_back(print_s("T", t));
		}
	};

	inline generated generate(uint64_t seed, bool argument_hazards = false) { return generator(seed, argument_hazards).generate(); }

	// the three oracles over one seed (the smoke suite's check): empty when the backends agree on the program and on its variant, the
	// program agrees with its variant, and every run prints what the model computes (when the model does not abstain)
	inline std::string order_seed_problem(uint64_t seed, double budget_seconds, bool proofs = true) {
		const generated g = generate(seed);
		const compare_result a = compare_program(g.original, budget_seconds, proofs);
		const compare_result b = compare_program(g.variant, budget_seconds, proofs);
		if (a.kind == compare_result::budget_skip || b.kind == compare_result::budget_skip) return {};
		if (a.kind == compare_result::diverge) return "backends disagree on the program:\n" + a.diff;
		if (b.kind == compare_result::diverge) return "backends disagree on the variant:\n" + b.diff;
		if (a.interp.printed != b.interp.printed) return "the program and its variant disagree:\n" + first_difference(a.interp.printed, b.interp.printed);
		if (g.model_ok && a.interp.printed != g.expected) return "the model disagrees:\n" + first_difference(g.expected, a.interp.printed);
		return {};
	}

} // namespace jai::fuzz::order
