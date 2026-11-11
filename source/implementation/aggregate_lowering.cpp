// The aggregate lowering (detail/aggregate_lowering.hpp).
//
// Per function or lambda body: an analysis walk finds the typed math locals and by-value math parameters, resolves every
// identifier to them by scope (and the parser's slot), and lets a local escape wherever its storage could be named
// (a call or constructor argument, a reference cast, a range-for container, an increment, a store in expression
// position, any use inside a nested function or lambda or parallel body). A rewrite walk then copies the body along
// every path that changes (untouched subtrees stay shared) and replaces the lowered locals:
//   - a declaration becomes one declaration per component (or keeps its source slot and unpacks, when some store of
//     the local needs the typed slot's conversion);
//   - `v.x` reads a component slot; `v.x = e`, `v.x op= e` store one narrowed through the store's kernel;
//   - `v = e`, `v op= e` store every component (through temporaries when the value reads v or may raise midway);
//   - a vector expression over lowered locals, constructors, the operators, the component-wise family and the geometry
//     rows becomes component expressions: binary32 rows compute in binary64 and land through f32$ (innocuous double
//     rounding: + - * / of two binary32 values rounded once to binary32 are the binary32 operations), ivec rows run
//     the int64 operation narrowed by i32add$ .. i32neg$ (the kernel's int32 narrowing and texts), the family runs
//     vcomp$ - each the boxed kernel's own step, so bits and texts agree;
//   - a vector the program needs as a value is rebuilt with the constructor from its components.
// Component expressions evaluate component by component, where the boxed kernel evaluates operation by operation, so
// an inline form admits at most one node that may raise or run an effect (the orders then raise and act alike). Shared
// subexpressions (a length, a normalize's divisor) hoist into temporaries ahead of the statement only when the
// statement is pure.

#include <jaiscript/detail/aggregate_lowering.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/detail/expr_children.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace jai::detail {

// named, not anonymous: jaiscript.cpp compiles every implementation file as one unit
namespace aggregate_lowering_impl {

using vk = script_value_type;

enum : uint8_t { c_unknown = 0, c_int = 1, c_float = 2, c_bool = 3 };

uint8_t class_of_type(const type_info* t) {
	if (!t) return c_unknown;
	switch (t->base_type) {
		case vk::jai_int_type: return c_int;
		case vk::jai_float_type: return c_float;
		case vk::jai_bool_type: return c_bool;
		default: return c_unknown;
	}
}

bool math_kind_type(const type_info* t) { return t && math::is_vec_type(t->base_type); }
// array<int> / array<float> / array<bool>: the class of every element (the buffer follows its declared type)
uint8_t element_class_of_type(const type_info* t) {
	return t && t->base_type == vk::jai_array_type ? class_of_type(t->element_type()) : c_unknown;
}
// array<math::K>: K, else invalid
vk vec_buffer_kind(const type_info* t) {
	if (!t || t->base_type != vk::jai_array_type || !t->element_type()) return vk::jai_invalid_type;
	const vk e = t->element_type()->base_type;
	return math::is_vec_type(e) ? e : vk::jai_invalid_type;
}

struct candidate {
	uint64_t symbol = UINT64_MAX;
	size_t slot = SIZE_MAX;
	vk kind = vk::jai_invalid_type;
	bool is_param = false;
	bool escaped = false;
	bool stored = false;       // a store of the whole value or a component
	size_t refs = 0;
	size_t loop_refs = 0;
	bool needs_box = false;    // a store falls back to the source slot's typed conversion: the declaration keeps the slot
	size_t comp_slot[4] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
	uint64_t comp_symbol[4] = {0, 0, 0, 0};
	std::string_view comp_name[4];
	std::string_view name;
	const variable_decl* decl = nullptr;
};

// ---- analysis -------------------------------------------------------------------------------------------------------

enum class pos : uint8_t { value, call_arg, intrinsic_arg, member_base, target_stmt, target_expr, incdec, range_container, ref_cast };

struct analysis {
	std::vector<candidate>& cands;
	std::unordered_map<const identifier_expr*, int> resolved;
	std::unordered_map<const identifier_expr*, uint8_t> local_class;
	std::unordered_map<const identifier_expr*, vk> vec_buffer;   // an identifier naming a local array<math::K>: K
	std::unordered_map<const identifier_expr*, uint8_t> element_class;   // an identifier naming a local array of scalars: their class
	struct entry { uint64_t symbol; int cand; size_t slot; uint8_t cls; vk buffer; uint8_t element; };
	std::vector<std::vector<entry>> scopes;
	int nested = 0;       // inside a nested function, lambda or parallel body
	int loop_depth = 0;

	explicit analysis(std::vector<candidate>& c) : cands(c) {}

	void declare(uint64_t symbol, int cand, size_t slot, uint8_t cls, vk buffer = vk::jai_invalid_type, uint8_t element = c_unknown) {
		if (scopes.empty()) scopes.emplace_back();
		scopes.back().push_back({symbol, cand, slot, cls, buffer, element});
	}
	void escape(int cand) { if (cand >= 0) cands[static_cast<size_t>(cand)].escaped = true; }

	int resolve(const identifier_expr* id) {
		if (id->global_qualified) return -1;
		for (auto s = scopes.rbegin(); s != scopes.rend(); ++s) {
			for (auto e = s->rbegin(); e != s->rend(); ++e) {
				if (e->symbol != id->symbol_id) continue;
				if (e->cand >= 0) {
					if (nested > 0 || id->slot_index != e->slot) { escape(e->cand); return -1; }
					return e->cand;
				}
				if (nested == 0 && id->slot_index == e->slot) {
					local_class[id] = e->cls;
					if (e->buffer != vk::jai_invalid_type) vec_buffer[id] = e->buffer;
					if (e->element != c_unknown) element_class[id] = e->element;
				}
				for (size_t c = 0; c < cands.size(); ++c) {   // the parser named a candidate this scope walk does not see
					if (cands[c].symbol == id->symbol_id && cands[c].slot == id->slot_index && id->slot_index != SIZE_MAX) { cands[c].escaped = true; }
				}
				return -1;
			}
		}
		for (size_t c = 0; c < cands.size(); ++c) {
			if (cands[c].symbol == id->symbol_id && cands[c].slot == id->slot_index && id->slot_index != SIZE_MAX) { cands[c].escaped = true; }
		}
		return -1;
	}

	void use(int cand, bool store) {
		if (cand < 0) return;
		candidate& c = cands[static_cast<size_t>(cand)];
		++c.refs;
		if (loop_depth > 0) ++c.loop_refs;
		if (store) c.stored = true;
	}

	void identifier(const identifier_expr* id, pos p) {
		const int cand = resolve(id);
		if (cand < 0) return;
		resolved[id] = cand;
		switch (p) {
			case pos::value: case pos::intrinsic_arg: use(cand, false); break;
			case pos::target_stmt: use(cand, true); break;
			default: escape(cand); break;
		}
	}

	static bool math_static_call(const call_expr* c) {
		return c->callee && c->callee->get_type() == node_type::member_expr &&
		       math_intrinsic_for_call(static_cast<const member_expr*>(c->callee.get())) != math_fn::none;
	}

	void expr(const expression* e, pos p = pos::value) {
		if (!e) return;
		switch (e->get_type()) {
			case node_type::identifier_expr: identifier(static_cast<const identifier_expr*>(e), p); break;
			case node_type::member_expr: {
				auto* m = static_cast<const member_expr*>(e);
				if (m->is_static) { if (m->object && m->object->get_type() != node_type::identifier_expr) expr(m->object.get()); break; }
				if (m->object && m->object->get_type() == node_type::identifier_expr) {
					const int cand = resolve(static_cast<const identifier_expr*>(m->object.get()));
					if (cand >= 0) {
						resolved[static_cast<const identifier_expr*>(m->object.get())] = cand;
						if (m->is_arrow || m->null_safe) { escape(cand); break; }
						switch (p) {
							case pos::value: case pos::intrinsic_arg: case pos::member_base: use(cand, false); break;
							case pos::target_stmt: use(cand, true); break;
							default: escape(cand); break;
						}
						break;
					}
				}
				expr(m->object.get(), pos::member_base);
				break;
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e);
				expr(b->left.get()); expr(b->right.get());
				break;
			}
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e);
				const bool incdec = u->op.type == token_type::plus_plus || u->op.type == token_type::minus_minus;
				expr(u->operand.get(), incdec ? pos::incdec : pos::value);
				break;
			}
			case node_type::assignment_expr: {
				auto* a = static_cast<const assignment_expr*>(e);
				expr(a->value.get());
				expr(a->target.get(), p == pos::target_stmt ? pos::target_stmt : pos::target_expr);
				break;
			}
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e);
				const bool intrinsic = math_static_call(c);
				if (c->callee && c->callee->get_type() == node_type::member_expr) {
					auto* m = static_cast<const member_expr*>(c->callee.get());
					if (!m->is_static) {
						// a method on a lowered local reads it (the vector methods take no reference to their receiver)
						bool vector_receiver = false;
						if (m->object && m->object->get_type() == node_type::identifier_expr) {
							const int cand = resolve(static_cast<const identifier_expr*>(m->object.get()));
							if (cand >= 0) { resolved[static_cast<const identifier_expr*>(m->object.get())] = cand; use(cand, false); vector_receiver = true; }
							else expr(m->object.get(), pos::member_base);
						} else {
							expr(m->object.get(), pos::member_base);
							vector_receiver = vector_valued(m->object.get());
						}
						// the vector library's by-value arguments only when the receiver is a vector: any other receiver's method
						// may take a reference, whatever its name
						const bool vec_method = vector_receiver && vec_method_name(m->member);
						for (const auto& a : c->arguments) expr(a.get(), vec_method ? pos::intrinsic_arg : pos::call_arg);
						break;
					}
				} else if (c->callee) {
					expr(c->callee.get());
				}
				for (const auto& a : c->arguments) expr(a.get(), intrinsic ? pos::intrinsic_arg : pos::call_arg);
				break;
			}
			case node_type::lambda_expr: {
				auto* l = static_cast<const lambda_expr*>(e);
				++nested;
				scopes.emplace_back();
				for (const auto& p2 : l->parameters) declare(symbol_of(p2), -1, p2.slot_index, class_of_type(p2.type.get()));
				for (const auto& cap : l->captures) {
					if (cap.initializer) { expr(cap.initializer.get()); continue; }
					for (auto& c : cands) { if (c.name == cap.name) c.escaped = true; }
				}
				stmt(l->body.get());
				scopes.pop_back();
				--nested;
				break;
			}
			case node_type::new_expr: for (const auto& a : static_cast<const new_expr*>(e)->arguments) expr(a.get(), pos::call_arg); break;
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e);
				expr(t->condition.get()); expr(t->then_expression.get()); expr(t->else_expression.get());
				break;
			}
			case node_type::array_literal_expr: for (const auto& a : static_cast<const array_literal_expr*>(e)->elements) expr(a.get()); break;
			case node_type::map_literal_expr: for (const auto& [k, v] : static_cast<const map_literal_expr*>(e)->entries) { expr(k.get()); expr(v.get()); } break;
			case node_type::throw_expr: expr(static_cast<const throw_expr*>(e)->value.get()); break;
			case node_type::yield_expr: expr(static_cast<const yield_expr*>(e)->value.get()); break;
			case node_type::include_expr: expr(static_cast<const include_expr*>(e)->path_expr.get(), pos::call_arg); break;
			case node_type::cast_expr: {
				auto* c = static_cast<const cast_expr*>(e);
				expr(c->operand.get(), c->by_reference ? pos::ref_cast : pos::value);
				break;
			}
			default: for_each_child_expression(e, [&](const expression* child) { expr(child, pos::call_arg); }); break;   // import(...) and any other kind: its arguments run as a call's
		}
	}

	// a receiver whose methods are the vector library's: an element of a local vector buffer or a math:: call's result
	bool vector_valued(const expression* e) const {
		if (!e) return false;
		if (e->get_type() == node_type::binary_expr) {
			auto* b = static_cast<const binary_expr*>(e);
			return b->op.type == token_type::left_bracket && b->left && b->left->get_type() == node_type::identifier_expr &&
			       vec_buffer.count(static_cast<const identifier_expr*>(b->left.get())) != 0;
		}
		return e->get_type() == node_type::call_expr && math_static_call(static_cast<const call_expr*>(e));
	}

	static bool vec_method_name(std::string_view n) {
		return vec_method_fn(vk::jai_vec3_type, n) != math_fn::none || vec_method_fn(vk::jai_vec2_type, n) != math_fn::none;
	}
	static uint64_t symbol_of(const parameter& p) { return p.symbol_id; }

	// a statement-position expression: a top-level assignment's target is a whole store
	void stmt_expr(const expression* e) {
		if (e && e->get_type() == node_type::assignment_expr) { expr(e, pos::target_stmt); return; }
		expr(e);
	}

	void block_body(const std::vector<declaration_ptr>& decls) {
		scopes.emplace_back();
		for (const auto& d : decls) decl(d.get());
		scopes.pop_back();
	}

	void decl(const declaration* d) {
		if (!d) return;
		switch (d->get_type()) {
			case node_type::variable_decl: {
				auto* v = static_cast<const variable_decl*>(d);
				// a reference declaration binds its initializer (`auto& r = v.x` names v's storage)
				const bool binds = v->type && v->type->base_type == script_value_type::jai_reference_type;
				expr(v->initializer.get(), binds ? pos::ref_cast : pos::value);
				int cand = -1;
				if (nested == 0 && math_kind_type(v->type.get()) && !v->ref_escaping && !v->is_static && v->slot_index != SIZE_MAX && v->name_id != UINT64_MAX) {
					candidate c;
					c.symbol = v->name_id; c.slot = v->slot_index; c.kind = v->type->base_type; c.name = v->name; c.decl = v;
					cands.push_back(c);
					cand = static_cast<int>(cands.size() - 1);
				}
				declare(v->name_id, cand, v->slot_index, class_of_type(v->type.get()), vec_buffer_kind(v->type.get()), element_class_of_type(v->type.get()));
				break;
			}
			case node_type::expression_decl: stmt_expr(static_cast<const expression_decl*>(d)->expression.get()); break;
			case node_type::statement_decl: stmt(static_cast<const statement_decl*>(d)->statement.get()); break;
			case node_type::function_decl: {
				auto* f = static_cast<const function_decl*>(d);
				++nested;
				scopes.emplace_back();
				for (const auto& p : f->parameters) declare(p.symbol_id, -1, p.slot_index, class_of_type(p.type.get()));
				if (f->body) stmt(f->body.get());
				scopes.pop_back();
				--nested;
				declare(f->name_id, -1, SIZE_MAX, c_unknown);
				break;
			}
			case node_type::destructuring_decl: {
				auto* dd = static_cast<const destructuring_decl*>(d);
				expr(dd->initializer.get());
				for (size_t i = 0; i < dd->names.size(); ++i) declare(dd->names[i].second, -1, i < dd->slot_indices.size() ? dd->slot_indices[i] : SIZE_MAX, c_unknown);
				break;
			}
			case node_type::class_decl: case node_type::namespace_decl: {
				++nested;
				if (d->get_type() == node_type::class_decl) { for (const auto& m : static_cast<const class_decl*>(d)->members) decl(m.declaration.get()); }
				else { for (const auto& m : static_cast<const namespace_decl*>(d)->declarations) decl(m.get()); }
				--nested;
				break;
			}
			case node_type::include_decl: expr(static_cast<const include_decl*>(d)->path_expr.get(), pos::call_arg); break;
			case node_type::import_decl: expr(static_cast<const import_decl*>(d)->path_expr.get(), pos::call_arg); break;
			default: break;
		}
	}

	void stmt(const statement* s) {
		if (!s) return;
		switch (s->get_type()) {
			case node_type::expression_stmt: stmt_expr(static_cast<const expression_stmt*>(s)->expression.get()); break;
			case node_type::block_stmt: block_body(static_cast<const block_stmt*>(s)->declarations); break;
			case node_type::if_stmt: {
				auto* i = static_cast<const if_stmt*>(s);
				expr(i->condition.get());
				scopes.emplace_back(); stmt(i->then_statement.get()); scopes.pop_back();
				scopes.emplace_back(); stmt(i->else_statement.get()); scopes.pop_back();
				break;
			}
			case node_type::while_stmt: {
				auto* w = static_cast<const while_stmt*>(s);
				++loop_depth;
				expr(w->condition.get());
				scopes.emplace_back(); stmt(w->body.get()); scopes.pop_back();
				--loop_depth;
				break;
			}
			case node_type::for_stmt: {
				auto* f = static_cast<const for_stmt*>(s);
				scopes.emplace_back();
				if (f->initializer) {
					// a vector declared in the loop head stays boxed (the head holds one declaration)
					if (f->initializer->get_type() == node_type::variable_decl) {
						auto* v = static_cast<const variable_decl*>(f->initializer.get());
						expr(v->initializer.get());
						declare(v->name_id, -1, v->slot_index, class_of_type(v->type.get()));
					} else {
						decl(f->initializer.get());
					}
				}
				++loop_depth;
				expr(f->condition.get());
				expr(f->update.get());
				scopes.emplace_back(); stmt(f->body.get()); scopes.pop_back();
				--loop_depth;
				scopes.pop_back();
				break;
			}
			case node_type::range_for_stmt: {
				auto* r = static_cast<const range_for_stmt*>(s);
				expr(r->container.get(), pos::range_container);
				scopes.emplace_back();
				declare(r->variable_name_id, -1, r->variable_slot_index, class_of_type(r->element_type.get()));
				++loop_depth;
				stmt(r->body.get());
				--loop_depth;
				scopes.pop_back();
				break;
			}
			case node_type::parallel_for_stmt: {
				auto* p = static_cast<const parallel_for_stmt*>(s);
				expr(p->container.get(), pos::range_container);
				for (const auto& c : p->extra_containers) expr(c.get(), pos::range_container);
				++nested;
				scopes.emplace_back();
				declare(p->loop_param.symbol_id, -1, p->loop_param.slot_index, c_unknown);
				for (const auto& ep : p->extra_params) declare(ep.symbol_id, -1, ep.slot_index, c_unknown);
				stmt(p->body.get());
				scopes.pop_back();
				--nested;
				break;
			}
			case node_type::return_stmt: {
				auto* r = static_cast<const return_stmt*>(s);
				expr(r->value.get(), r->binds_reference ? pos::ref_cast : pos::value);   // a reference return names the storage
				break;
			}
			case node_type::try_stmt: {
				auto* t = static_cast<const try_stmt*>(s);
				scopes.emplace_back(); stmt(t->try_block.get()); scopes.pop_back();
				scopes.emplace_back();
				if (t->catch_var_id != UINT64_MAX) declare(t->catch_var_id, -1, t->catch_slot_index, c_unknown);
				stmt(t->catch_block.get());
				scopes.pop_back();
				break;
			}
			case node_type::switch_stmt: {
				auto* sw = static_cast<const switch_stmt*>(s);
				expr(sw->condition.get());
				scopes.emplace_back();
				for (const auto& c : sw->cases) { expr(c->value.get()); for (const auto& b : c->body) stmt(b.get()); }
				if (sw->default_case) { for (const auto& b : sw->default_case->body) stmt(b.get()); }
				scopes.pop_back();
				break;
			}
			default:
				if (s->get_type() >= node_type::variable_decl) decl(static_cast<const declaration*>(s));
				break;
		}
	}
};

// Does the body name a coroutine yield (its locals then live across suspensions the lowering does not model)?
bool contains_yield(const statement* s);
bool contains_yield_expr(const expression* e) {
	if (!e) return false;
	if (e->get_type() == node_type::yield_expr) return true;
	bool found = false;
	for_each_child_expression(e, [&](const expression* child) { found = found || contains_yield_expr(child); });
	return found;
}
bool contains_yield(const statement* s) {
	if (!s) return false;
	switch (s->get_type()) {
		case node_type::expression_stmt: return contains_yield_expr(static_cast<const expression_stmt*>(s)->expression.get());
		case node_type::expression_decl: return contains_yield_expr(static_cast<const expression_decl*>(s)->expression.get());
		case node_type::variable_decl: return contains_yield_expr(static_cast<const variable_decl*>(s)->initializer.get());
		case node_type::statement_decl: return contains_yield(static_cast<const statement_decl*>(s)->statement.get());
		case node_type::block_stmt: for (const auto& d : static_cast<const block_stmt*>(s)->declarations) if (contains_yield(d.get())) return true; return false;
		case node_type::if_stmt: { auto* i = static_cast<const if_stmt*>(s); return contains_yield_expr(i->condition.get()) || contains_yield(i->then_statement.get()) || contains_yield(i->else_statement.get()); }
		case node_type::while_stmt: { auto* w = static_cast<const while_stmt*>(s); return contains_yield_expr(w->condition.get()) || contains_yield(w->body.get()); }
		case node_type::for_stmt: { auto* f = static_cast<const for_stmt*>(s); return contains_yield(f->initializer.get()) || contains_yield_expr(f->condition.get()) || contains_yield_expr(f->update.get()) || contains_yield(f->body.get()); }
		case node_type::range_for_stmt: { auto* r = static_cast<const range_for_stmt*>(s); return contains_yield_expr(r->container.get()) || contains_yield(r->body.get()); }
		case node_type::return_stmt: return contains_yield_expr(static_cast<const return_stmt*>(s)->value.get());
		case node_type::try_stmt: { auto* t = static_cast<const try_stmt*>(s); return contains_yield(t->try_block.get()) || contains_yield(t->catch_block.get()); }
		case node_type::switch_stmt: {
			auto* sw = static_cast<const switch_stmt*>(s);
			if (contains_yield_expr(sw->condition.get())) return true;
			for (const auto& c : sw->cases) for (const auto& b : c->body) if (contains_yield(b.get())) return true;
			if (sw->default_case) for (const auto& b : sw->default_case->body) if (contains_yield(b.get())) return true;
			return false;
		}
		default: return false;
	}
}

// ---- rewrite --------------------------------------------------------------------------------------------------------

// A vector expression as component expressions
struct vform {
	vk kind = vk::jai_invalid_type;
	size_t n = 0;
	expression_ptr c[4];
	int sensitive = 0;           // nodes that may raise or run an effect
	int element_reads = 0;       // of those, vector buffer element reads (an out-of-range index raises at the first component)
	std::vector<int> reads;      // the lowered locals whose components it reads
	void read(int cand) { if (std::find(reads.begin(), reads.end(), cand) == reads.end()) reads.push_back(cand); }
	void merge(const vform& o) { sensitive += o.sensitive; element_reads += o.element_reads; for (int r : o.reads) read(r); }
};

struct stmt_ctx {
	std::vector<declaration_ptr>* prelude = nullptr;   // hoisted temporaries run here, ahead of the statement (pure statements only)
};

class rewriter {
public:
	rewriter(engine& eng, std::vector<candidate>& cands, const analysis& an, size_t first_temp, bool dry)
		: eng_(eng), sym_(*engine_access::get_symbolizer(eng)), cands_(cands), an_(an), next_slot_(first_temp), dry_(dry) {
		t_float_ = engine_access::get_type_info_float(eng_);
		t_int_ = engine_access::get_type_info_int(eng_);
		auto [mid, mview] = sym_.intern_with_view("math");
		math_id_ = mid; math_view_ = mview;
	}

	size_t slots_used() const { return next_slot_; }

	std::vector<declaration_ptr> block_list(const std::vector<declaration_ptr>& in, bool* changed) {
		std::vector<declaration_ptr> out;
		out.reserve(in.size());
		for (const auto& d : in) {
			const size_t before = out.size();
			decl_into(d, out);
			if (out.size() != before + 1 || out.back() != d) *changed = true;
		}
		return out;
	}

	// the parameters' components unpack from their slots at the body's entry
	void param_prologue(std::vector<declaration_ptr>& out, const source_location& loc) {
		for (size_t i = 0; i < cands_.size(); ++i) {
			const candidate& c = cands_[i];
			if (!c.is_param || c.escaped) continue;
			for (size_t k = 0; k < math::dims(c.kind); ++k) {
				out.push_back(comp_decl(c, k, unpack(c, k, loc), loc));
			}
		}
	}

	statement_ptr stmt(const statement_ptr& s) {
		if (!s) return s;
		std::vector<declaration_ptr> list;
		stmt_into(s, list);
		if (list.size() == 1) {
			if (list[0]->get_type() == node_type::statement_decl) return static_cast<statement_decl*>(list[0].get())->statement;
			if (list[0].get() == s.get()) return s;
		}
		if (list.size() == 1 && list[0]->get_type() == node_type::expression_decl && s->get_type() == node_type::expression_stmt) {
			auto e = std::make_shared<expression_stmt>(s->location, static_cast<expression_decl*>(list[0].get())->expression);
			return e;
		}
		return std::make_shared<block_stmt>(s->location, std::move(list));
	}

private:
	engine& eng_;
	string_symbolizer& sym_;
	std::vector<candidate>& cands_;
	const analysis& an_;
	size_t next_slot_;
	bool dry_;
	type_info_ptr t_float_, t_int_;
	uint64_t math_id_ = 0;
	std::string_view math_view_;
	size_t temp_serial_ = 0;

	// ---- node builders ----
	int cand_of(const expression* e) const {
		if (!e || e->get_type() != node_type::identifier_expr) return -1;
		auto it = an_.resolved.find(static_cast<const identifier_expr*>(e));
		if (it == an_.resolved.end()) return -1;
		return cands_[static_cast<size_t>(it->second)].escaped ? -1 : it->second;
	}
	expression_ptr ident(size_t slot, uint64_t symbol, std::string_view name, const source_location& loc) {
		auto id = std::make_shared<identifier_expr>(loc, name, symbol);
		id->slot_index = slot;
		id->names_value_decl = true;
		id->names_known_value = true;
		return id;
	}
	expression_ptr comp_ident(const candidate& c, size_t k, const source_location& loc) {
		return ident(c.comp_slot[k], c.comp_symbol[k], c.comp_name[k], loc);
	}
	expression_ptr source_ident(const candidate& c, const source_location& loc) { return ident(c.slot, c.symbol, c.name, loc); }
	static expression_ptr lit_float(double d, const source_location& loc) { return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_float(d))); }
	static expression_ptr lit_int(int64_t i, const source_location& loc) { return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_int(i))); }
	expression_ptr math_callee(std::string_view name, const source_location& loc) {
		auto [id, view] = sym_.intern_with_view(name);
		auto m = std::make_shared<identifier_expr>(loc, math_view_, math_id_);
		return std::make_shared<member_expr>(loc, m, view, id, false, true);
	}
	expression_ptr math_call(std::string_view name, std::vector<expression_ptr> args, const source_location& loc) {
		return std::make_shared<call_expr>(loc, math_callee(name, loc), std::move(args));
	}
	static token op_token(token_type t, const source_location& loc) {
		switch (t) {
			case token_type::plus: return token(t, "+", loc);
			case token_type::minus: return token(t, "-", loc);
			case token_type::star: return token(t, "*", loc);
			case token_type::slash: return token(t, "/", loc);
			case token_type::percent: return token(t, "%", loc);
			case token_type::equal_equal: return token(t, "==", loc);
			case token_type::bang_equal: return token(t, "!=", loc);
			case token_type::ampersand_ampersand: return token(t, "&&", loc);
			case token_type::pipe_pipe: return token(t, "||", loc);
			case token_type::bang: return token(t, "!", loc);
			case token_type::equal: return token(t, "=", loc);
			default: return token(t, "", loc);
		}
	}
	static expression_ptr bin(expression_ptr l, token_type t, expression_ptr r, const source_location& loc) {
		return std::make_shared<binary_expr>(loc, std::move(l), op_token(t, loc), std::move(r));
	}
	static expression_ptr neg(expression_ptr e, const source_location& loc) {
		return std::make_shared<unary_expr>(loc, op_token(token_type::minus, loc), std::move(e), false);
	}
	expression_ptr f32(expression_ptr e, const source_location& loc) {
		if (e->get_type() == node_type::literal_expr) {   // a literal rounds here, once
			const script_value& v = static_cast<const literal_expr*>(e.get())->value;
			if (v.raw_storage_index() == script_value::TYPEID_FLOAT) return lit_float(static_cast<double>(math::to_f32(v.unchecked_as_float())), loc);
			if (v.raw_storage_index() == script_value::TYPEID_INT) return lit_float(static_cast<double>(math::to_f32(v.unchecked_as_int())), loc);
			if (v.raw_storage_index() == script_value::TYPEID_BOOL) return lit_float(v.unchecked_as_bool() ? 1.0 : 0.0, loc);
		}
		return math_call("f32$", {std::move(e)}, loc);
	}
	expression_ptr i32(expression_ptr e, const source_location& loc, bool* raising) {
		if (e->get_type() == node_type::literal_expr) {
			const script_value& v = static_cast<const literal_expr*>(e.get())->value;
			if (v.raw_storage_index() == script_value::TYPEID_INT && v.unchecked_as_int() >= INT32_MIN && v.unchecked_as_int() <= INT32_MAX) return e;
			if (v.raw_storage_index() == script_value::TYPEID_BOOL) return lit_int(v.unchecked_as_bool() ? 1 : 0, loc);
		}
		*raising = true;
		return math_call("i32$", {std::move(e)}, loc);
	}
	expression_ptr rebox(vk kind, const expression_ptr* comps, const source_location& loc) {
		std::vector<expression_ptr> args(comps, comps + math::dims(kind));
		return math_call(std::string_view(math_type_name(kind)).substr(6), std::move(args), loc);
	}
	expression_ptr rebox_cand(const candidate& c, const source_location& loc) {
		expression_ptr comps[4];
		for (size_t k = 0; k < math::dims(c.kind); ++k) comps[k] = comp_ident(c, k, loc);
		return rebox(c.kind, comps, loc);
	}
	expression_ptr unpack(const candidate& c, size_t k, const source_location& loc) {
		auto [id, view] = sym_.intern_with_view(math::component_name(c.kind, k));
		return std::make_shared<member_expr>(loc, source_ident(c, loc), view, id, false, false);
	}
	declaration_ptr comp_decl(const candidate& c, size_t k, expression_ptr init, const source_location& loc) {
		auto d = std::make_shared<variable_decl>(loc, math::integral(c.kind) ? t_int_ : t_float_, c.comp_name[k], c.comp_symbol[k], std::move(init));
		d->slot_index = c.comp_slot[k];
		return d;
	}
	declaration_ptr store(expression_ptr target, expression_ptr value, const source_location& loc) {
		return std::make_shared<expression_decl>(loc, std::make_shared<assignment_expr>(loc, std::move(target), op_token(token_type::equal, loc), std::move(value)));
	}
	// a fresh temporary slot declared with `init` into `out`; the identifier reading it
	expression_ptr temp(bool integral, expression_ptr init, std::vector<declaration_ptr>& out, const source_location& loc) {
		const std::string name = "$t" + std::to_string(temp_serial_++);
		auto [id, view] = sym_.intern_with_view(name);
		auto d = std::make_shared<variable_decl>(loc, integral ? t_int_ : t_float_, view, id, std::move(init));
		d->slot_index = next_slot_++;
		out.push_back(d);
		return ident(d->slot_index, id, view, loc);
	}
	static bool simple(const expression_ptr& e) { return e && (e->get_type() == node_type::identifier_expr || e->get_type() == node_type::literal_expr); }

	// ---- scalar classes ----
	uint8_t scalar_class(const expression* e) {
		if (!e) return c_unknown;
		switch (e->get_type()) {
			case node_type::literal_expr: {
				const size_t r = static_cast<const literal_expr*>(e)->value.raw_storage_index();
				return r == script_value::TYPEID_INT ? c_int : r == script_value::TYPEID_FLOAT ? c_float : r == script_value::TYPEID_BOOL ? c_bool : c_unknown;
			}
			case node_type::identifier_expr: {
				auto it = an_.local_class.find(static_cast<const identifier_expr*>(e));
				return it == an_.local_class.end() ? c_unknown : it->second;
			}
			case node_type::member_expr: {
				auto* m = static_cast<const member_expr*>(e);
				const int cand = cand_of(m->object.get());
				if (cand >= 0 && !m->is_static && math::component_of(cands_[static_cast<size_t>(cand)].kind, m->member) >= 0) {
					return math::integral(cands_[static_cast<size_t>(cand)].kind) ? c_int : c_float;
				}
				return c_unknown;
			}
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e);
				if (u->op.type != token_type::minus || u->is_postfix) return u->op.type == token_type::bang ? c_bool : c_unknown;
				const uint8_t k = scalar_class(u->operand.get());
				return k == c_int || k == c_float ? k : c_unknown;
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type == token_type::left_bracket) {
					if (!b->left || b->left->get_type() != node_type::identifier_expr) return c_unknown;
					auto it = an_.element_class.find(static_cast<const identifier_expr*>(b->left.get()));
					return it == an_.element_class.end() ? c_unknown : it->second;
				}
				if (b->returns_bool()) return c_bool;
				switch (b->op.type) {
					case token_type::plus: case token_type::minus: case token_type::star: case token_type::slash: case token_type::percent: {
						const uint8_t l = scalar_class(b->left.get()), r = scalar_class(b->right.get());
						if (l == c_int && r == c_int) return c_int;
						if ((l == c_int || l == c_float) && (r == c_int || r == c_float)) return c_float;
						return c_unknown;
					}
					default: return c_unknown;
				}
			}
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e);
				if (!c->callee || c->callee->get_type() != node_type::member_expr) return c_unknown;
				auto* m = static_cast<const member_expr*>(c->callee.get());
				if (!m->is_static) {
					const int cand = cand_of(m->object.get());
					if (cand < 0) return c_unknown;
					const vk kind = cands_[static_cast<size_t>(cand)].kind;
					const math_fn fn = vec_method_fn(kind, m->member);
					switch (fn) {
						case math_fn::length_: case math_fn::distance: case math_fn::angle_: return c_float;
						case math_fn::length_squared_: case math_fn::dot_: return math::integral(kind) ? c_unknown : c_float;
						default: return c_unknown;
					}
				}
				switch (math_intrinsic_for_call(m)) {
					case math_fn::sqrt_: case math_fn::sin_: case math_fn::cos_: case math_fn::tan_: case math_fn::pow_: case math_fn::atan2_:
					case math_fn::radians_to_degrees: case math_fn::degrees_to_radians: case math_fn::f32_: case math_fn::angle_: return c_float;
					case math_fn::itrunc: case math_fn::ifloor: case math_fn::i32_:
					case math_fn::i32_add_: case math_fn::i32_sub_: case math_fn::i32_mul_: case math_fn::i32_div_: case math_fn::i32_neg_: return c_int;
					default: return c_unknown;
				}
			}
			default: return c_unknown;
		}
	}
	// a scalar a vector row may read once per component: no effect, no raise
	bool pure_scalar(const expression* e) {
		if (!e) return false;
		switch (e->get_type()) {
			case node_type::literal_expr: return true;
			case node_type::identifier_expr: return an_.local_class.count(static_cast<const identifier_expr*>(e)) != 0;
			case node_type::member_expr: return scalar_class(e) != c_unknown;
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e);
				return u->op.type == token_type::minus && !u->is_postfix && scalar_class(u->operand.get()) == c_float && pure_scalar(u->operand.get());
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type != token_type::plus && b->op.type != token_type::minus && b->op.type != token_type::star) return false;
				return scalar_class(b) == c_float && pure_scalar(b->left.get()) && pure_scalar(b->right.get());
			}
			default: return false;
		}
	}
	// no statement part stores, calls anything but the math kernel or a vector's method, or reads an object member
	bool pure_statement(const expression* e) {
		if (!e) return true;
		switch (e->get_type()) {
			case node_type::literal_expr: case node_type::identifier_expr: case node_type::this_expr: case node_type::lambda_expr: return true;
			case node_type::member_expr: {
				auto* m = static_cast<const member_expr*>(e);
				if (m->is_static) return true;
				return cand_of(m->object.get()) >= 0;
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e);
				if (b->op.type == token_type::left_bracket) return false;
				return pure_statement(b->left.get()) && pure_statement(b->right.get());
			}
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e);
				if (u->op.type == token_type::plus_plus || u->op.type == token_type::minus_minus) return false;
				return pure_statement(u->operand.get());
			}
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e);
				return pure_statement(t->condition.get()) && pure_statement(t->then_expression.get()) && pure_statement(t->else_expression.get());
			}
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e);
				if (!c->callee || c->callee->get_type() != node_type::member_expr) return false;
				auto* m = static_cast<const member_expr*>(c->callee.get());
				if (m->is_static) {
					const math_fn fn = math_intrinsic_for_call(m);
					if (fn == math_fn::none) return false;
				} else if (const int cand = cand_of(m->object.get()); cand < 0 || vec_method_fn(cands_[static_cast<size_t>(cand)].kind, m->member) == math_fn::none) {
					return false;
				}
				for (const auto& a : c->arguments) if (!pure_statement(a.get())) return false;
				return true;
			}
			default: return false;
		}
	}

	// ---- vector forms ----
	static vk family(vk kind) { return math::family_kind(false, math::dims(kind)); }

	bool vec_of(const expression_ptr& e, vform& f, stmt_ctx& ctx) {
		if (!e) return false;
		const source_location& loc = e->location;
		switch (e->get_type()) {
			case node_type::identifier_expr: {
				const int cand = cand_of(e.get());
				if (cand < 0) return false;
				const candidate& c = cands_[static_cast<size_t>(cand)];
				f.kind = c.kind; f.n = math::dims(c.kind);
				for (size_t k = 0; k < f.n; ++k) f.c[k] = comp_ident(c, k, loc);
				f.read(cand);
				return true;
			}
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e.get());
				if (u->op.type != token_type::minus || u->is_postfix) return false;
				vform o;
				if (!vec_of(u->operand, o, ctx)) return false;
				f = o;
				for (size_t k = 0; k < f.n; ++k) {
					if (math::integral(f.kind)) f.c[k] = math_call("i32neg$", {neg(o.c[k], loc)}, loc);
					else f.c[k] = neg(o.c[k], loc);
				}
				if (math::integral(f.kind)) ++f.sensitive;
				return true;
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e.get());
				if (b->op.type == token_type::left_bracket) return vec_element(e, b, f);
				return vec_binary(b, f, ctx);
			}
			case node_type::call_expr: return vec_call(static_cast<const call_expr*>(e.get()), f, ctx);
			default: return false;
		}
	}

	// the scalar side of a vector row: a pure number of a known class, rewritten
	bool row_scalar(const expression_ptr& e, uint8_t& cls, expression_ptr& out) {
		cls = scalar_class(e.get());
		if (cls == c_unknown || !pure_scalar(e.get())) return false;
		out = scalar_expr(e);
		return true;
	}

	// `a[i]` over a local vector buffer with an int literal or int local index: one component read per component (the reads name the
	// same element; an out-of-range index raises at the first, before anything is stored)
	bool vec_element(const expression_ptr& e, const binary_expr* b, vform& f) {
		if (!b->left || !b->right || b->left->get_type() != node_type::identifier_expr) return false;
		auto it = an_.vec_buffer.find(static_cast<const identifier_expr*>(b->left.get()));
		if (it == an_.vec_buffer.end()) return false;
		const expression* r = b->right.get();
		const bool index_ok = (r->get_type() == node_type::literal_expr && static_cast<const literal_expr*>(r)->value.raw_storage_index() == script_value::TYPEID_INT) ||
		                      (r->get_type() == node_type::identifier_expr && scalar_class(r) == c_int);
		if (!index_ok) return false;
		f = vform{};
		f.kind = it->second;
		f.n = math::dims(f.kind);
		f.sensitive = 1;
		f.element_reads = 1;
		for (size_t k = 0; k < f.n; ++k) {
			auto [id, view] = sym_.intern_with_view(math::component_name(f.kind, k));
			f.c[k] = std::make_shared<member_expr>(e->location, e, view, id, false, false);
		}
		return true;
	}

	bool vec_binary(const binary_expr* b, vform& f, stmt_ctx& ctx) {
		const token_type t = b->op.type;
		if (t != token_type::plus && t != token_type::minus && t != token_type::star && t != token_type::slash && t != token_type::percent) return false;
		vform l, r;
		const bool lv = vec_of(b->left, l, ctx);
		const bool rv = vec_of(b->right, r, ctx);
		return vec_row(t, lv ? &l : nullptr, lv ? nullptr : &b->left, rv ? &r : nullptr, rv ? nullptr : &b->right, b->location, f, &ctx);
	}

	// `l op r` over forms (a null form: that side is the scalar expression)
	bool vec_row(token_type t, vform* l, const expression_ptr* ls, vform* r, const expression_ptr* rs, const source_location& loc, vform& f, stmt_ctx* ctx = nullptr) {
		if (!l && !r) return false;
		const vform& v = l ? *l : *r;
		const vk kind = v.kind;
		if (l && r && l->kind != r->kind) return false;
		const bool integral = math::integral(kind);
		if (t == token_type::percent && !integral) return false;
		uint8_t scls = c_unknown;
		expression_ptr s;
		if (!(l && r)) {
			if (!row_scalar(l ? *rs : *ls, scls, s)) return false;
			if (integral && scls != c_int && scls != c_bool) return false;   // the row's invalid operands: keep the boxed text
		}
		f = vform{};
		f.kind = kind; f.n = v.n;
		if (l) f.merge(*l);
		if (r) f.merge(*r);
		if (integral) {
			// the int64 operation over int32 components narrowed with the operator's text (a remainder always fits); a scalar
			// side narrows to int32 first, in every component, as the kernel's row does
			bool raising = false;
			expression_ptr s32 = (l && r) ? nullptr : i32(s, loc, &raising);
			const char* narrow = t == token_type::plus ? "i32add$" : t == token_type::minus ? "i32sub$" : t == token_type::star ? "i32mul$" : t == token_type::slash ? "i32div$" : nullptr;
			for (size_t k = 0; k < f.n; ++k) {
				expression_ptr x = bin(l ? l->c[k] : s32, t, r ? r->c[k] : s32, loc);
				f.c[k] = narrow ? math_call(narrow, {std::move(x)}, loc) : std::move(x);
			}
			++f.sensitive;
			return true;
		}
		expression_ptr s32 = (l && r) ? nullptr : f32(s, loc);
		if (s32 && !simple(s32) && ctx && ctx->prelude) s32 = temp(false, s32, *ctx->prelude, loc);   // a pure scalar narrows once per statement
		for (size_t k = 0; k < f.n; ++k) f.c[k] = f32(bin(l ? l->c[k] : s32, t, r ? r->c[k] : s32, loc), loc);
		if (t == token_type::slash) {
			const bool literal_divisor = !r && s32->get_type() == node_type::literal_expr && static_cast<const literal_expr*>(s32.get())->value.unchecked_as_float() != 0.0;
			if (!literal_divisor) ++f.sensitive;
		}
		return true;
	}

	bool vec_call(const call_expr* c, vform& f, stmt_ctx& ctx) {
		if (!c->callee || c->callee->get_type() != node_type::member_expr) return false;
		auto* m = static_cast<const member_expr*>(c->callee.get());
		const source_location& loc = c->location;
		if (!m->is_static) {
			// a vector method: the free function with the receiver first
			vform recv;
			if (!vec_of(m->object, recv, ctx)) return false;
			const math_fn fn = vec_method_fn(recv.kind, m->member);
			if (fn == math_fn::none || c->arguments.size() != vec_method_arity(fn, recv.kind)) return false;
			return geometry_vector(fn, recv, c->arguments, 0, loc, f, ctx);
		}
		const math_fn fn = math_intrinsic_for_call(m);
		if (!math_arity_accepts(fn, c->arguments.size())) return false;   // the boxed call raises the table's text
		switch (fn) {
			case math_fn::vec2_: case math_fn::vec3_: case math_fn::vec4_: case math_fn::ivec2_: case math_fn::ivec3_: case math_fn::color_:
				return vec_ctor(fn, c, f, ctx);
			case math_fn::floor_: case math_fn::ceil_: case math_fn::round_: case math_fn::abs_: case math_fn::min_: case math_fn::max_:
			case math_fn::clamp_: case math_fn::lerp_: case math_fn::mix_: case math_fn::unmix_: case math_fn::remap_: case math_fn::mix_in_:
			case math_fn::mix_out_: case math_fn::mix_in_out_: case math_fn::mix_out_in_: case math_fn::unmix_in_: case math_fn::unmix_out_:
			case math_fn::unmix_in_out_: case math_fn::unmix_out_in_:
				return vec_family(fn, c, f, ctx);
			case math_fn::from_angle_: {
				// (cos, sin), each times the length when one is given: the angle and the length are pure, read once per component
				uint8_t cls = c_unknown;
				expression_ptr angle, length;
				if (!row_scalar(c->arguments[0], cls, angle) || (cls != c_int && cls != c_float)) return false;
				if (c->arguments.size() == 2 && (!row_scalar(c->arguments[1], cls, length) || (cls != c_int && cls != c_float))) return false;
				f = vform{}; f.kind = vk::jai_vec2_type; f.n = 2;
				expression_ptr cs = math_call("cos", {angle}, loc), sn = math_call("sin", {angle}, loc);
				f.c[0] = f32(length ? bin(cs, token_type::star, length, loc) : cs, loc);
				f.c[1] = f32(length ? bin(sn, token_type::star, length, loc) : sn, loc);
				return true;
			}
			case math_fn::normalize_: case math_fn::cross_: case math_fn::perpendicular_: case math_fn::reflect_: case math_fn::project_: case math_fn::rotate_:
			case math_fn::rotate_around_: {
				if (c->arguments.empty()) return false;
				vform first;
				if (!vec_of(c->arguments[0], first, ctx)) return false;
				return geometry_vector(fn, first, c->arguments, 1, loc, f, ctx);
			}
			default: return false;
		}
	}

	bool vec_ctor(math_fn fn, const call_expr* c, vform& f, stmt_ctx& ctx) {
		const vk kind = fn == math_fn::vec2_ ? vk::jai_vec2_type : fn == math_fn::vec3_ ? vk::jai_vec3_type : fn == math_fn::vec4_ ? vk::jai_vec4_type
		              : fn == math_fn::color_ ? vk::jai_color_type : fn == math_fn::ivec2_ ? vk::jai_ivec2_type : vk::jai_ivec3_type;
		const size_t n = math::dims(kind), argc = c->arguments.size();
		const bool integral = math::integral(kind), color = math::is_color(kind);
		const source_location& loc = c->location;
		f = vform{}; f.kind = kind; f.n = n;
		if (argc == 0) {
			for (size_t k = 0; k < n; ++k) f.c[k] = integral ? lit_int(0, loc) : lit_float(color ? 1.0 : 0.0, loc);
			return true;
		}
		if (argc == 1) {   // a vector of the same dimensions (color only from a color), converted
			vform v;
			if (!vec_of(c->arguments[0], v, ctx) || v.n != n || math::is_color(v.kind) != color) return false;
			f.merge(v);
			return convert_into(v, kind, loc, f);
		}
		if (argc > n || (color && argc != 3 && argc != 4)) return false;
		size_t at = 0;
		bool effects = false, raising = false;
		vform head;
		if (!color && vec_of(c->arguments[0], head, ctx)) {   // (v, scalars...): a narrower vector of the family completed by scalars
			const vk narrow = math::family_kind(integral, head.n);
			if (head.n >= n || head.n + (argc - 1) != n || narrow == vk::jai_invalid_type || math::is_color(head.kind)) return false;
			vform conv; conv.kind = narrow; conv.n = head.n;
			if (!convert_into(head, narrow, loc, conv)) return false;
			f.merge(head);
			f.sensitive += conv.sensitive;
			for (size_t k = 0; k < head.n; ++k) f.c[k] = conv.c[k];
			at = head.n;
		} else if (!color && argc < 2) {
			return false;
		}
		for (size_t a = at == 0 ? 0 : 1; a < argc; ++a, ++at) {
			const expression_ptr& arg = c->arguments[a];
			const uint8_t cls = scalar_class(arg.get());
			if (cls == c_unknown) return false;   // a non-number is the constructor's text: keep the boxed call
			if (!pure_scalar(arg.get())) effects = true;
			expression_ptr v = scalar_expr(arg);
			f.c[at] = integral ? i32(v, loc, &raising) : f32(v, loc);
		}
		for (; at < n; ++at) f.c[at] = integral ? lit_int(0, loc) : lit_float(color ? 1.0 : 0.0, loc);
		if (effects && raising) return false;   // the constructor evaluates every argument before it narrows any
		if (effects || raising) ++f.sensitive;
		return true;
	}

	// v's components as kind `to`: the same kind passes, an ivec widens through f32$, and the constructor's vector argument
	// narrows through i32$ (the checked float-to-int kernel, which may raise); a slot takes no narrowing, so its store stays boxed
	// and raises the slot's text
	bool slot_convert_into(const vform& v, vk to, const source_location& loc, vform& f) {
		return !math::narrows(v.kind, to) && convert_into(v, to, loc, f);
	}
	bool convert_into(const vform& v, vk to, const source_location& loc, vform& f) {
		if (!math::converts(v.kind, to)) return false;
		f.kind = to; f.n = v.n;
		bool raising = false;
		for (size_t k = 0; k < v.n; ++k) {
			if (v.kind == to) f.c[k] = v.c[k];
			else if (math::integral(to)) f.c[k] = i32(v.c[k], loc, &raising);
			else f.c[k] = f32(v.c[k], loc);
		}
		if (raising) ++f.sensitive;
		return true;
	}

	bool vec_family(math_fn fn, const call_expr* c, vform& f, stmt_ctx& ctx) {
		const size_t argc = c->arguments.size();
		if (argc == 0 || argc > 5 || !math_arity_accepts(fn, argc)) return false;
		const source_location& loc = c->location;
		vform vs[5];
		bool is_vec[5] = {false, false, false, false, false};
		expression_ptr sc[5];
		size_t dims = 0;
		bool any_color = false, all_color = true, all_integral = true;
		f = vform{};
		for (size_t a = 0; a < argc; ++a) {
			if (vec_of(c->arguments[a], vs[a], ctx)) {
				is_vec[a] = true;
				if (dims != 0 && vs[a].n != dims) return false;
				dims = vs[a].n;
				any_color = any_color || math::is_color(vs[a].kind);
				all_color = all_color && math::is_color(vs[a].kind);
				all_integral = all_integral && math::integral(vs[a].kind);
				f.merge(vs[a]);
			} else {
				uint8_t cls = c_unknown;
				if (!row_scalar(c->arguments[a], cls, sc[a]) || (cls != c_int && cls != c_float)) return false;
				all_integral = all_integral && cls == c_int;
			}
		}
		if (dims == 0) return false;
		if (any_color) {
			switch (fn) {
				case math_fn::lerp_: case math_fn::mix_: case math_fn::mix_in_: case math_fn::mix_out_: case math_fn::mix_in_out_: case math_fn::mix_out_in_: return false;   // OKLab
				default: break;
			}
			if (!all_color) return false;
		}
		const bool exact = all_integral && (fn == math_fn::abs_ || fn == math_fn::min_ || fn == math_fn::max_ || fn == math_fn::clamp_);
		f.kind = any_color ? vk::jai_color_type : math::family_kind(exact, dims);
		f.n = dims;
		const int code = static_cast<int>(fn) | (exact ? k_vcomp_exact : 0);
		for (size_t k = 0; k < dims; ++k) {
			std::vector<expression_ptr> args;
			args.push_back(lit_int(code, loc));
			for (size_t a = 0; a < argc; ++a) args.push_back(is_vec[a] ? vs[a].c[k] : sc[a]);
			f.c[k] = math_call("vcomp$", std::move(args), loc);
		}
		if (exact) ++f.sensitive;
		return true;
	}

	// normalize, cross (3D) and perpendicular; `first` is the receiver or the first argument, the rest from `args` at `from`
	bool geometry_vector(math_fn fn, vform first, const std::vector<expression_ptr>& args, size_t from, const source_location& loc, vform& f, stmt_ctx& ctx) {
		if (math::is_color(first.kind)) return false;
		f = vform{};
		switch (fn) {
			case math_fn::perpendicular_: {
				if (first.n != 2 || args.size() != from) return false;
				f.merge(first);
				f.kind = first.kind; f.n = 2;
				if (math::integral(first.kind)) {
					bool raising = false;
					f.c[0] = i32(neg(first.c[1], loc), loc, &raising);
					f.c[1] = first.c[0];
					++f.sensitive;
				} else {
					f.c[0] = neg(first.c[1], loc);
					f.c[1] = first.c[0];
				}
				return true;
			}
			case math_fn::normalize_: {
				if (args.size() != from || !ctx.prelude || first.sensitive != 0) return false;
				widen(first);
				if (!materialize(first, *ctx.prelude, loc)) return false;
				f.merge(first);
				f.kind = family(first.kind); f.n = first.n;
				expression_ptr len = temp(false, math_call("sqrt", {dot_expr(first, first, loc)}, loc), *ctx.prelude, loc);
				for (size_t k = 0; k < f.n; ++k) {
					auto nonzero = bin(len, token_type::bang_equal, lit_float(0.0, loc), loc);
					f.c[k] = std::make_shared<ternary_expr>(loc, nonzero, f32(bin(first.c[k], token_type::slash, len, loc), loc), lit_float(0.0, loc));
				}
				return true;
			}
			case math_fn::reflect_: case math_fn::project_: {
				// d = dot(v, n); v - 2 d n (reflect) | d = dot(b, b); b * (dot(a, b) / d), zero when d is (project)
				if (args.size() != from + 1 || !ctx.prelude || first.sensitive != 0) return false;
				vform second;
				if (!vec_of(args[from], second, ctx) || second.n != first.n || math::is_color(second.kind) || second.sensitive != 0) return false;
				widen(first); widen(second);
				if (!materialize(first, *ctx.prelude, loc) || !materialize(second, *ctx.prelude, loc)) return false;
				f.merge(first); f.merge(second);
				f.kind = family(first.kind); f.n = first.n;
				if (fn == math_fn::reflect_) {
					expression_ptr d = temp(false, dot_expr(first, second, loc), *ctx.prelude, loc);
					for (size_t k = 0; k < f.n; ++k) f.c[k] = f32(bin(first.c[k], token_type::minus, bin(bin(lit_float(2.0, loc), token_type::star, d, loc), token_type::star, second.c[k], loc), loc), loc);
					return true;
				}
				expression_ptr d = temp(false, dot_expr(second, second, loc), *ctx.prelude, loc);
				auto nonzero = [&] { return bin(d, token_type::bang_equal, lit_float(0.0, loc), loc); };
				expression_ptr s = temp(false, std::make_shared<ternary_expr>(loc, nonzero(), bin(dot_expr(first, second, loc), token_type::slash, d, loc), lit_float(0.0, loc)), *ctx.prelude, loc);
				for (size_t k = 0; k < f.n; ++k) f.c[k] = std::make_shared<ternary_expr>(loc, nonzero(), f32(bin(second.c[k], token_type::star, s, loc), loc), lit_float(0.0, loc));
				return true;
			}
			case math_fn::rotate_: {
				// vec2: (x cos - y sin, x sin + y cos); vec3: Rodrigues about the normalized axis, the vector itself about a zero axis
				const bool three = first.n == 3;
				if (!ctx.prelude || first.sensitive != 0 || args.size() != from + (three ? 2 : 1) || (first.n != 2 && !three)) return false;
				vform axis;
				if (three && (!vec_of(args[from], axis, ctx) || axis.n != 3 || math::is_color(axis.kind) || axis.sensitive != 0)) return false;
				uint8_t cls = c_unknown;
				expression_ptr angle;
				if (!row_scalar(args[args.size() - 1], cls, angle) || (cls != c_int && cls != c_float)) return false;
				widen(first);
				if (!materialize(first, *ctx.prelude, loc)) return false;
				if (three) { widen(axis); if (!materialize(axis, *ctx.prelude, loc)) return false; f.merge(axis); }
				f.merge(first);
				f.kind = family(first.kind); f.n = first.n;
				expression_ptr cs = temp(false, math_call("cos", {angle}, loc), *ctx.prelude, loc);
				expression_ptr sn = temp(false, math_call("sin", {angle}, loc), *ctx.prelude, loc);
				if (!three) {
					f.c[0] = f32(bin(bin(first.c[0], token_type::star, cs, loc), token_type::minus, bin(first.c[1], token_type::star, sn, loc), loc), loc);
					f.c[1] = f32(bin(bin(first.c[0], token_type::star, sn, loc), token_type::plus, bin(first.c[1], token_type::star, cs, loc), loc), loc);
					return true;
				}
				expression_ptr len = temp(false, math_call("sqrt", {dot_expr(axis, axis, loc)}, loc), *ctx.prelude, loc);
				auto nonzero = [&] { return bin(len, token_type::bang_equal, lit_float(0.0, loc), loc); };
				expression_ptr k[3];
				for (size_t c = 0; c < 3; ++c) k[c] = temp(false, std::make_shared<ternary_expr>(loc, nonzero(), bin(axis.c[c], token_type::slash, len, loc), lit_float(0.0, loc)), *ctx.prelude, loc);
				expression_ptr kv = temp(false, bin(bin(bin(k[0], token_type::star, first.c[0], loc), token_type::plus, bin(k[1], token_type::star, first.c[1], loc), loc), token_type::plus, bin(k[2], token_type::star, first.c[2], loc), loc), *ctx.prelude, loc);
				static constexpr size_t p[3] = {1, 2, 0}, q[3] = {2, 0, 1};
				for (size_t c = 0; c < 3; ++c) {
					auto kx = bin(bin(k[p[c]], token_type::star, first.c[q[c]], loc), token_type::minus, bin(k[q[c]], token_type::star, first.c[p[c]], loc), loc);
					auto turned = bin(bin(bin(first.c[c], token_type::star, cs, loc), token_type::plus, bin(kx, token_type::star, sn, loc), loc), token_type::plus,
					                  bin(bin(k[c], token_type::star, kv, loc), token_type::star, bin(lit_float(1.0, loc), token_type::minus, cs, loc), loc), loc);
					f.c[c] = std::make_shared<ternary_expr>(loc, nonzero(), f32(turned, loc), f32(first.c[c], loc));
				}
				return true;
			}
			case math_fn::rotate_around_: {
				// d = v - pivot; pivot + (d.x cos - d.y sin, d.x sin + d.y cos)
				if (!ctx.prelude || first.sensitive != 0 || first.n != 2 || args.size() != from + 2) return false;
				vform pivot;
				if (!vec_of(args[from], pivot, ctx) || pivot.n != 2 || math::is_color(pivot.kind) || pivot.sensitive != 0) return false;
				uint8_t cls = c_unknown;
				expression_ptr angle;
				if (!row_scalar(args[from + 1], cls, angle) || (cls != c_int && cls != c_float)) return false;
				widen(first); widen(pivot);
				if (!materialize(first, *ctx.prelude, loc) || !materialize(pivot, *ctx.prelude, loc)) return false;
				f.merge(first); f.merge(pivot);
				f.kind = family(first.kind); f.n = 2;
				expression_ptr cs = temp(false, math_call("cos", {angle}, loc), *ctx.prelude, loc);
				expression_ptr sn = temp(false, math_call("sin", {angle}, loc), *ctx.prelude, loc);
				expression_ptr dx = temp(false, bin(first.c[0], token_type::minus, pivot.c[0], loc), *ctx.prelude, loc);
				expression_ptr dy = temp(false, bin(first.c[1], token_type::minus, pivot.c[1], loc), *ctx.prelude, loc);
				f.c[0] = f32(bin(pivot.c[0], token_type::plus, bin(bin(dx, token_type::star, cs, loc), token_type::minus, bin(dy, token_type::star, sn, loc), loc), loc), loc);
				f.c[1] = f32(bin(pivot.c[1], token_type::plus, bin(bin(dx, token_type::star, sn, loc), token_type::plus, bin(dy, token_type::star, cs, loc), loc), loc), loc);
				return true;
			}
			case math_fn::cross_: {
				if (args.size() != from + 1) return false;
				vform second;
				if (!vec_of(args[from], second, ctx) || second.n != first.n || first.n != 3 || math::is_color(second.kind)) return false;
				if (!simple_form(first) || !simple_form(second)) {
					if (!ctx.prelude || first.sensitive + second.sensitive != 0) return false;
					if (!materialize(first, *ctx.prelude, loc) || !materialize(second, *ctx.prelude, loc)) return false;
				}
				f.merge(first); f.merge(second);
				const bool exact = math::integral(first.kind) && math::integral(second.kind);
				f.kind = exact ? vk::jai_ivec3_type : vk::jai_vec3_type; f.n = 3;
				static constexpr size_t p[3] = {1, 2, 0}, q[3] = {2, 0, 1};
				bool raising = false;
				for (size_t k = 0; k < 3; ++k) {
					auto d = bin(bin(first.c[p[k]], token_type::star, second.c[q[k]], loc), token_type::minus, bin(first.c[q[k]], token_type::star, second.c[p[k]], loc), loc);
					f.c[k] = exact ? i32(d, loc, &raising) : f32(d, loc);
				}
				if (exact) ++f.sensitive;
				return true;
			}
			default: return false;
		}
	}

	// a component read twice: a slot, a literal, or a widened one of those (evaluating it twice is exact and has no effect)
	static bool dupable(const expression_ptr& e) {
		if (simple(e)) return true;
		if (!e || e->get_type() != node_type::binary_expr) return false;
		auto* b = static_cast<const binary_expr*>(e.get());
		return b->op.type == token_type::star && simple(b->left) && b->right && b->right->get_type() == node_type::literal_expr;
	}
	static bool simple_form(const vform& v) {
		for (size_t k = 0; k < v.n; ++k) if (!dupable(v.c[k])) return false;
		return true;
	}
	// an ivec form read by a binary64 row: each component widened exactly (the kernel reads int32 components as doubles)
	static void widen(vform& v) {
		if (!math::integral(v.kind)) return;
		for (size_t k = 0; k < v.n; ++k) v.c[k] = bin(v.c[k], token_type::star, lit_float(1.0, v.c[k]->location), v.c[k]->location);
		v.kind = family(v.kind);
	}
	// every component that is not a slot or a literal lands in a temporary ahead of the statement (a pure form only)
	bool materialize(vform& v, std::vector<declaration_ptr>& prelude, const source_location& loc) {
		if (v.sensitive != 0) return false;
		for (size_t k = 0; k < v.n; ++k) {
			if (!simple(v.c[k])) v.c[k] = temp(math::integral(v.kind), v.c[k], prelude, loc);
		}
		return true;
	}
	// sum of the products in the kernel's order: ((a0 b0 + a1 b1) + a2 b2) ...
	expression_ptr dot_expr(const vform& a, const vform& b, const source_location& loc) {
		expression_ptr s = bin(a.c[0], token_type::star, b.c[0], loc);
		for (size_t k = 1; k < a.n; ++k) s = bin(s, token_type::plus, bin(a.c[k], token_type::star, b.c[k], loc), loc);
		return s;
	}

	// ---- scalar geometry: dot, length, length_squared, distance, distance_squared, the 2D cross, angle_between, angle ----
	expression_ptr geometry_scalar(const call_expr* c, stmt_ctx& ctx) {
		if (!c->callee || c->callee->get_type() != node_type::member_expr) return nullptr;
		auto* m = static_cast<const member_expr*>(c->callee.get());
		const source_location& loc = c->location;
		math_fn fn;
		vform a, b;
		bool have_b = false;
		if (m->is_static) {
			fn = math_intrinsic_for_call(m);
			const size_t argc = c->arguments.size();
			const bool two = fn == math_fn::dot_ || fn == math_fn::distance || fn == math_fn::distance_squared || fn == math_fn::cross_ || fn == math_fn::angle_between_;
			const bool one = fn == math_fn::length_ || fn == math_fn::length_squared_ || fn == math_fn::angle_;
			if (!(two && argc == 2) && !(one && argc == 1)) return nullptr;
			if (!vec_of(c->arguments[0], a, ctx)) return nullptr;
			if (two) { if (!vec_of(c->arguments[1], b, ctx)) return nullptr; have_b = true; }
		} else {
			if (!vec_of(m->object, a, ctx)) return nullptr;
			fn = vec_method_fn(a.kind, m->member);
			if (fn == math_fn::none || c->arguments.size() != vec_method_arity(fn, a.kind)) return nullptr;
			if (fn == math_fn::dot_ || fn == math_fn::distance || fn == math_fn::cross_) {
				if (!vec_of(c->arguments[0], b, ctx)) return nullptr;
				have_b = true;
			} else if (fn != math_fn::length_ && fn != math_fn::length_squared_ && fn != math_fn::angle_) {
				return nullptr;
			}
		}
		if (math::is_color(a.kind) || (have_b && (math::is_color(b.kind) || b.n != a.n))) return nullptr;
		if ((fn == math_fn::cross_ || fn == math_fn::angle_) && a.n != 2) return nullptr;
		const bool exact = math::integral(a.kind) && (!have_b || math::integral(b.kind)) && fn != math_fn::length_ && fn != math_fn::distance && fn != math_fn::angle_between_ && fn != math_fn::angle_;
		if (!exact) { widen(a); if (have_b) widen(b); }
		const int operand_sensitive = a.sensitive + (have_b ? b.sensitive : 0);
		if (operand_sensitive > (exact ? 0 : 1)) return nullptr;
		// a component read twice must be a slot or a literal, or land in a temporary first
		const bool twice = fn == math_fn::length_ || fn == math_fn::length_squared_ || fn == math_fn::distance || fn == math_fn::distance_squared || fn == math_fn::cross_ || fn == math_fn::angle_;
		if (twice && (!simple_form(a) || (have_b && !simple_form(b)))) {
			if (!ctx.prelude || operand_sensitive != 0) return nullptr;
			if (!materialize(a, *ctx.prelude, loc) || (have_b && !materialize(b, *ctx.prelude, loc))) return nullptr;
		}
		if (fn == math_fn::angle_between_) {
			// the lengths once, a zero one gives 0, else the clamped acos of the cosine (the kernel's order)
			if (!ctx.prelude || operand_sensitive != 0) return nullptr;
			if (!materialize(a, *ctx.prelude, loc) || !materialize(b, *ctx.prelude, loc)) return nullptr;
			expression_ptr la = temp(false, math_call("sqrt", {dot_expr(a, a, loc)}, loc), *ctx.prelude, loc);
			expression_ptr lb = temp(false, math_call("sqrt", {dot_expr(b, b, loc)}, loc), *ctx.prelude, loc);
			auto zero = bin(bin(la, token_type::equal_equal, lit_float(0.0, loc), loc), token_type::pipe_pipe, bin(lb, token_type::equal_equal, lit_float(0.0, loc), loc), loc);
			auto cosine = bin(dot_expr(a, b, loc), token_type::slash, bin(la, token_type::star, lb, loc), loc);
			return std::make_shared<ternary_expr>(loc, zero, lit_float(0.0, loc), math_call("acos$", {cosine}, loc));
		}
		if (fn == math_fn::angle_) {
			// atan2(y, x) with a zero y read as +0, and 0 for the zero vector (the kernel's geometry::direction)
			auto is_zero = [&](const expression_ptr& comp) { return bin(comp, token_type::equal_equal, lit_float(0.0, loc), loc); };
			expression_ptr y = std::make_shared<ternary_expr>(loc, is_zero(a.c[1]), lit_float(0.0, loc), a.c[1]);
			return std::make_shared<ternary_expr>(loc, bin(is_zero(a.c[0]), token_type::ampersand_ampersand, is_zero(a.c[1]), loc), lit_float(0.0, loc), math_call("atan2", {y, a.c[0]}, loc));
		}
		switch (fn) {
			case math_fn::dot_: return dot_expr(a, b, loc);
			case math_fn::length_squared_: return dot_expr(a, a, loc);
			case math_fn::length_: return math_call("sqrt", {dot_expr(a, a, loc)}, loc);
			case math_fn::cross_: return bin(bin(a.c[0], token_type::star, b.c[1], loc), token_type::minus, bin(a.c[1], token_type::star, b.c[0], loc), loc);
			default: {   // distance, distance_squared: d = b - a, summed in the kernel's order
				vform d; d.n = a.n;
				for (size_t k = 0; k < a.n; ++k) d.c[k] = bin(b.c[k], token_type::minus, a.c[k], loc);
				expression_ptr s = bin(d.c[0], token_type::star, d.c[0], loc);
				for (size_t k = 1; k < a.n; ++k) s = bin(s, token_type::plus, bin(d.c[k], token_type::star, d.c[k], loc), loc);
				if (fn == math_fn::distance_squared) return s;
				return math_call("sqrt", {s}, loc);
			}
		}
	}

	// ---- expressions ----
	expression_ptr scalar_expr(const expression_ptr& e) {
		stmt_ctx none;
		return expr(e, none);
	}

	// A vector-looking expression: a lowered local, a math constructor, library or geometry call, a vector method, or an
	// operator over one of those
	bool vectorish(const expression* e) const {
		if (!e) return false;
		switch (e->get_type()) {
			case node_type::identifier_expr: return cand_of(e) >= 0;
			case node_type::unary_expr: return static_cast<const unary_expr*>(e)->op.type == token_type::minus && vectorish(static_cast<const unary_expr*>(e)->operand.get());
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e);
				return vectorish(b->left.get()) || vectorish(b->right.get());
			}
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e);
				if (!c->callee || c->callee->get_type() != node_type::member_expr) return false;
				auto* m = static_cast<const member_expr*>(c->callee.get());
				if (!m->is_static) return vectorish(m->object.get());
				return math_intrinsic_for_call(m) != math_fn::none;
			}
			default: return false;
		}
	}

	expression_ptr expr(const expression_ptr& e, stmt_ctx& ctx) {
		if (!e) return e;
		const source_location& loc = e->location;
		switch (e->get_type()) {
			case node_type::identifier_expr: {
				const int cand = cand_of(e.get());
				if (cand < 0) return e;
				return rebox_cand(cands_[static_cast<size_t>(cand)], loc);
			}
			case node_type::member_expr: {
				auto* m = static_cast<const member_expr*>(e.get());
				if (m->is_static) return e;
				const int cand = cand_of(m->object.get());
				if (cand >= 0) {
					const candidate& c = cands_[static_cast<size_t>(cand)];
					const int comp = math::component_of(c.kind, m->member);
					if (comp >= 0) return comp_ident(c, static_cast<size_t>(comp), loc);
				} else if (vectorish(m->object.get())) {
					vform f;
					if (vec_of(m->object, f, ctx) && f.sensitive <= 1) {
						const int comp = math::component_of(f.kind, m->member);
						if (comp >= 0 && f.sensitive == 0) return f.c[comp];   // the other components are not evaluated: nothing they do may be lost
					}
				}
				auto obj = expr(m->object, ctx);
				if (obj == m->object) return e;
				auto copy = std::make_shared<member_expr>(*m);
				copy->object = obj;
				return copy;
			}
			case node_type::binary_expr: {
				auto* b = static_cast<const binary_expr*>(e.get());
				if ((b->op.type == token_type::equal_equal || b->op.type == token_type::bang_equal) && (vectorish(b->left.get()) || vectorish(b->right.get()))) {
					vform l, r;
					stmt_ctx no_prelude;
					if (vec_of(b->left, l, no_prelude) && vec_of(b->right, r, no_prelude) && l.n == r.n && math::is_color(l.kind) == math::is_color(r.kind) &&
					    l.sensitive + r.sensitive == 0) {
						expression_ptr all = bin(l.c[0], token_type::equal_equal, r.c[0], loc);
						for (size_t k = 1; k < l.n; ++k) all = bin(all, token_type::ampersand_ampersand, bin(l.c[k], token_type::equal_equal, r.c[k], loc), loc);
						if (b->op.type == token_type::bang_equal) all = std::make_shared<unary_expr>(loc, op_token(token_type::bang, loc), all, false);
						return all;
					}
				}
				if (vectorish(e.get())) {
					if (auto whole = vector_value(e, ctx)) return whole;
				}
				auto l = expr(b->left, ctx), r = expr(b->right, ctx);
				if (l == b->left && r == b->right) return e;
				auto copy = std::make_shared<binary_expr>(*b);
				copy->left = l; copy->right = r;
				return copy;
			}
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e.get());
				if (vectorish(e.get())) {
					if (auto whole = vector_value(e, ctx)) return whole;
				}
				auto o = expr(u->operand, ctx);
				if (o == u->operand) return e;
				auto copy = std::make_shared<unary_expr>(*u);
				copy->operand = o;
				return copy;
			}
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e.get());
				if (vectorish(e.get())) {
					if (auto s = geometry_scalar(c, ctx)) return s;
					if (auto whole = vector_value(e, ctx)) return whole;
				}
				bool changed = false;
				expression_ptr callee = c->callee;
				if (c->callee && c->callee->get_type() == node_type::member_expr) {
					auto* m = static_cast<const member_expr*>(c->callee.get());
					if (!m->is_static) {
						auto obj = expr(m->object, ctx);
						if (obj != m->object) {
							auto mc = std::make_shared<member_expr>(*m);
							mc->object = obj;
							callee = mc;
							changed = true;
						}
					}
				} else if (c->callee) {
					callee = expr(c->callee, ctx);
					changed = callee != c->callee;
				}
				std::vector<expression_ptr> args;
				args.reserve(c->arguments.size());
				for (const auto& a : c->arguments) {
					args.push_back(expr(a, ctx));
					if (args.back() != a) changed = true;
				}
				if (!changed) return e;
				auto copy = std::make_shared<call_expr>(*c);
				copy->callee = callee;
				copy->arguments = std::move(args);
				return copy;
			}
			case node_type::assignment_expr: {
				auto* a = static_cast<const assignment_expr*>(e.get());
				auto t = expr(a->target, ctx), v = expr(a->value, ctx);
				if (t == a->target && v == a->value) return e;
				auto copy = std::make_shared<assignment_expr>(*a);
				copy->target = t; copy->value = v;
				return copy;
			}
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e.get());
				stmt_ctx branch;   // a branch runs conditionally: nothing of it hoists
				auto c = expr(t->condition, ctx), th = expr(t->then_expression, branch), el = expr(t->else_expression, branch);
				if (c == t->condition && th == t->then_expression && el == t->else_expression) return e;
				return std::make_shared<ternary_expr>(t->location, c, th, el);
			}
			case node_type::array_literal_expr: {
				auto* a = static_cast<const array_literal_expr*>(e.get());
				bool changed = false;
				std::vector<expression_ptr> el;
				for (const auto& x : a->elements) { el.push_back(expr(x, ctx)); if (el.back() != x) changed = true; }
				if (!changed) return e;
				auto copy = std::make_shared<array_literal_expr>(*a);
				copy->elements = std::move(el);
				return copy;
			}
			case node_type::map_literal_expr: {
				auto* mp = static_cast<const map_literal_expr*>(e.get());
				bool changed = false;
				std::vector<std::pair<expression_ptr, expression_ptr>> en;
				for (const auto& [k, v] : mp->entries) {
					auto nk = expr(k, ctx), nv = expr(v, ctx);
					if (nk != k || nv != v) changed = true;
					en.emplace_back(nk, nv);
				}
				if (!changed) return e;
				auto copy = std::make_shared<map_literal_expr>(*mp);
				copy->entries = std::move(en);
				return copy;
			}
			case node_type::new_expr: {
				auto* n = static_cast<const new_expr*>(e.get());
				bool changed = false;
				std::vector<expression_ptr> args;
				for (const auto& a : n->arguments) { args.push_back(expr(a, ctx)); if (args.back() != a) changed = true; }
				if (!changed) return e;
				auto copy = std::make_shared<new_expr>(*n);
				copy->arguments = std::move(args);
				return copy;
			}
			case node_type::cast_expr: {
				auto* c = static_cast<const cast_expr*>(e.get());
				auto o = expr(c->operand, ctx);
				if (o == c->operand) return e;
				auto copy = std::make_shared<cast_expr>(*c);
				copy->operand = o;
				return copy;
			}
			case node_type::throw_expr: {
				auto* t = static_cast<const throw_expr*>(e.get());
				auto v = expr(t->value, ctx);
				if (v == t->value) return e;
				return std::make_shared<throw_expr>(t->location, v);
			}
			case node_type::yield_expr: {
				auto* y = static_cast<const yield_expr*>(e.get());
				auto v = expr(y->value, ctx);
				if (v == y->value) return e;
				return std::make_shared<yield_expr>(y->location, v);
			}
			case node_type::include_expr: {
				auto* in = static_cast<const include_expr*>(e.get());
				auto p = expr(in->path_expr, ctx);
				if (p == in->path_expr) return e;
				auto copy = std::make_shared<include_expr>(*in);
				copy->path_expr = p;
				return copy;
			}
			case node_type::import_expr: {
				auto* im = static_cast<const import_expr*>(e.get());
				bool changed = false;
				std::vector<expression_ptr> paths;
				for (const auto& a : im->path_exprs) { paths.push_back(expr(a, ctx)); if (paths.back() != a) changed = true; }
				if (!changed) return e;
				auto copy = std::make_shared<import_expr>(*im);
				copy->path_exprs = std::move(paths);
				return copy;
			}
			default: return e;   // no children (a lambda's init-captures read no lowered local: the analysis lets them escape)
		}
	}

	// a whole vector the program needs as a value: its components rebuilt by the constructor
	expression_ptr vector_value(const expression_ptr& e, stmt_ctx& ctx) {
		vform f;
		if (!vec_of(e, f, ctx) || f.sensitive > 1) return nullptr;
		return rebox(f.kind, f.c, e->location);
	}

	// ---- statements ----
	void stmt_into(const statement_ptr& s, std::vector<declaration_ptr>& out) {
		if (!s) { out.push_back(nullptr); return; }
		if (s->get_type() >= node_type::variable_decl) { decl_into(std::static_pointer_cast<declaration>(s), out); return; }
		switch (s->get_type()) {
			case node_type::expression_stmt: {
				auto* es = static_cast<const expression_stmt*>(s.get());
				std::vector<declaration_ptr> list;
				statement_expression(es->expression, list, s->location);
				if (list.size() == 1 && list[0]->get_type() == node_type::expression_decl &&
				    static_cast<expression_decl*>(list[0].get())->expression == es->expression) {
					out.push_back(std::make_shared<statement_decl>(s->location, s));
					return;
				}
				for (auto& d : list) out.push_back(std::move(d));
				return;
			}
			default: {
				std::vector<declaration_ptr> prelude;
				statement_ptr ns = statement_node(s, prelude);
				for (auto& d : prelude) out.push_back(std::move(d));
				out.push_back(std::make_shared<statement_decl>(s->location, ns));
				return;
			}
		}
	}

	statement_ptr statement_node(const statement_ptr& s, std::vector<declaration_ptr>& prelude) {
		switch (s->get_type()) {
			case node_type::block_stmt: {
				auto* b = static_cast<const block_stmt*>(s.get());
				bool changed = false;
				auto list = block_list(b->declarations, &changed);
				if (!changed) return s;
				auto copy = std::make_shared<block_stmt>(*b);
				copy->declarations = std::move(list);
				return copy;
			}
			case node_type::if_stmt: {
				auto* i = static_cast<const if_stmt*>(s.get());
				stmt_ctx ctx; if (pure_statement(i->condition.get())) ctx.prelude = &prelude;
				auto c = expr(i->condition, ctx);
				auto th = stmt(i->then_statement), el = stmt(i->else_statement);
				if (c == i->condition && th == i->then_statement && el == i->else_statement) return s;
				auto copy = std::make_shared<if_stmt>(*i);
				copy->condition = c; copy->then_statement = th; copy->else_statement = el;
				return copy;
			}
			case node_type::while_stmt: {
				auto* w = static_cast<const while_stmt*>(s.get());
				stmt_ctx ctx;
				auto c = expr(w->condition, ctx);
				auto b = stmt(w->body);
				if (c == w->condition && b == w->body) return s;
				auto copy = std::make_shared<while_stmt>(*w);
				copy->condition = c; copy->body = b;
				return copy;
			}
			case node_type::for_stmt: {
				auto* f = static_cast<const for_stmt*>(s.get());
				declaration_ptr init = f->initializer;
				if (init) {
					std::vector<declaration_ptr> list;
					decl_into(init, list);
					if (list.size() == 1) init = list[0];
					else { for (size_t k = 0; k + 1 < list.size(); ++k) prelude.push_back(list[k]); init = list.back(); }
				}
				stmt_ctx ctx;
				auto c = expr(f->condition, ctx);
				stmt_ctx uctx;
				expression_ptr u = expr(f->update, uctx);
				auto b = stmt(f->body);
				if (init == f->initializer && c == f->condition && u == f->update && b == f->body) return s;
				auto copy = std::make_shared<for_stmt>(*f);
				copy->initializer = init; copy->condition = c; copy->update = u; copy->body = b;
				return copy;
			}
			case node_type::range_for_stmt: {
				auto* r = static_cast<const range_for_stmt*>(s.get());
				stmt_ctx ctx; if (pure_statement(r->container.get())) ctx.prelude = &prelude;
				auto c = expr(r->container, ctx);
				auto b = stmt(r->body);
				if (c == r->container && b == r->body) return s;
				auto copy = std::make_shared<range_for_stmt>(*r);
				copy->container = c; copy->body = b;
				return copy;
			}
			case node_type::return_stmt: {
				auto* r = static_cast<const return_stmt*>(s.get());
				stmt_ctx ctx; if (pure_statement(r->value.get())) ctx.prelude = &prelude;
				auto v = expr(r->value, ctx);
				if (v == r->value) return s;
				auto copy = std::make_shared<return_stmt>(*r);
				copy->value = v;
				return copy;
			}
			case node_type::try_stmt: {
				auto* t = static_cast<const try_stmt*>(s.get());
				auto tb = stmt(t->try_block), cb = stmt(t->catch_block);
				if (tb == t->try_block && cb == t->catch_block) return s;
				auto copy = std::make_shared<try_stmt>(*t);
				copy->try_block = tb; copy->catch_block = cb;
				return copy;
			}
			case node_type::switch_stmt: {
				auto* sw = static_cast<const switch_stmt*>(s.get());
				stmt_ctx ctx; if (pure_statement(sw->condition.get())) ctx.prelude = &prelude;
				auto cond = expr(sw->condition, ctx);
				bool changed = cond != sw->condition;
				std::vector<case_stmt_ptr> cases;
				for (const auto& c : sw->cases) {
					stmt_ctx none;
					auto v = expr(c->value, none);
					bool case_changed = v != c->value;
					std::vector<statement_ptr> body = case_body(c->body, &case_changed);
					if (case_changed) {
						auto cc = std::make_shared<case_stmt>(*c);
						cc->value = v; cc->body = std::move(body);
						cases.push_back(cc);
						changed = true;
					} else {
						cases.push_back(c);
					}
				}
				default_stmt_ptr def = sw->default_case;
				if (def) {
					bool dc = false;
					auto body = case_body(def->body, &dc);
					if (dc) { auto d = std::make_shared<default_stmt>(*def); d->body = std::move(body); def = d; changed = true; }
				}
				if (!changed) return s;
				auto copy = std::make_shared<switch_stmt>(*sw);
				copy->condition = cond; copy->cases = std::move(cases); copy->default_case = def;
				return copy;
			}
			case node_type::parallel_for_stmt: {   // the containers run here; the body is the worker's function and keeps its node
				auto* p = static_cast<const parallel_for_stmt*>(s.get());
				stmt_ctx none;
				auto c = expr(p->container, none);
				bool changed = c != p->container;
				std::vector<expression_ptr> extra;
				for (const auto& x : p->extra_containers) { extra.push_back(expr(x, none)); if (extra.back() != x) changed = true; }
				if (!changed) return s;
				auto copy = std::make_shared<parallel_for_stmt>(*p);
				copy->container = c; copy->extra_containers = std::move(extra);
				return copy;
			}
			default: return s;   // no expression of its own (break, continue, fallthrough)
		}
	}

	// a case body holds statements only (no declarations): a rewrite into several becomes a block (its temporaries are its own)
	std::vector<statement_ptr> case_body(const std::vector<statement_ptr>& in, bool* changed) {
		std::vector<statement_ptr> out;
		for (const auto& st : in) {
			statement_ptr ns = stmt(st);
			if (ns != st) *changed = true;
			out.push_back(std::move(ns));
		}
		return out;
	}

	void decl_into(const declaration_ptr& d, std::vector<declaration_ptr>& out) {
		if (!d) { out.push_back(d); return; }
		switch (d->get_type()) {
			case node_type::variable_decl: {
				auto* v = static_cast<const variable_decl*>(d.get());
				for (size_t i = 0; i < cands_.size(); ++i) {
					const candidate& c = cands_[i];
					if (c.decl == v && !c.escaped) { lower_declaration(c, d, out); return; }
				}
				stmt_ctx ctx;
				std::vector<declaration_ptr> prelude;
				if (pure_statement(v->initializer.get())) ctx.prelude = &prelude;
				auto init = expr(v->initializer, ctx);
				for (auto& p : prelude) out.push_back(std::move(p));
				if (init == v->initializer) { out.push_back(d); return; }
				auto copy = std::make_shared<variable_decl>(*v);
				copy->initializer = init;
				out.push_back(copy);
				return;
			}
			case node_type::expression_decl: {
				auto* ed = static_cast<const expression_decl*>(d.get());
				std::vector<declaration_ptr> list;
				statement_expression(ed->expression, list, d->location);
				if (list.size() == 1 && list[0]->get_type() == node_type::expression_decl &&
				    static_cast<expression_decl*>(list[0].get())->expression == ed->expression) { out.push_back(d); return; }
				if (list.size() == 1 && list[0]->get_type() == node_type::expression_decl && ed->implicit_return) {
					auto copy = std::make_shared<expression_decl>(*ed);
					copy->expression = static_cast<expression_decl*>(list[0].get())->expression;
					out.push_back(copy);
					return;
				}
				for (auto& x : list) out.push_back(std::move(x));
				return;
			}
			case node_type::statement_decl: {
				auto* sd = static_cast<const statement_decl*>(d.get());
				const size_t before = out.size();
				stmt_into(sd->statement, out);
				if (out.size() == before + 1 && out.back()->get_type() == node_type::statement_decl &&
				    static_cast<statement_decl*>(out.back().get())->statement == sd->statement) {
					out.back() = d;   // unchanged: keep the node (and its check-mode attribute)
				} else if (sd->check_mode_attr != check_mode_attr_none && out.size() > before) {
					out[before]->check_mode_attr = sd->check_mode_attr;
				}
				return;
			}
			case node_type::destructuring_decl: {
				auto* dd = static_cast<const destructuring_decl*>(d.get());
				stmt_ctx ctx;
				auto init = expr(dd->initializer, ctx);
				if (init == dd->initializer) { out.push_back(d); return; }
				auto copy = std::make_shared<destructuring_decl>(*dd);
				copy->initializer = init;
				out.push_back(copy);
				return;
			}
			case node_type::include_decl: case node_type::import_decl: {
				const expression_ptr& path = d->get_type() == node_type::include_decl ? static_cast<const include_decl*>(d.get())->path_expr
				                                                                       : static_cast<const import_decl*>(d.get())->path_expr;
				stmt_ctx ctx;
				auto np = expr(path, ctx);
				if (np == path) { out.push_back(d); return; }
				if (d->get_type() == node_type::include_decl) {
					auto copy = std::make_shared<include_decl>(*static_cast<const include_decl*>(d.get()));
					copy->path_expr = np;
					out.push_back(copy);
				} else {
					auto copy = std::make_shared<import_decl>(*static_cast<const import_decl*>(d.get()));
					copy->path_expr = np;
					out.push_back(copy);
				}
				return;
			}
			default: out.push_back(d); return;   // nested functions, classes, namespaces: their own sites
		}
	}

	// `math::K v = e;`
	void lower_declaration(const candidate& c, const declaration_ptr& d, std::vector<declaration_ptr>& out) {
		const auto* v = static_cast<const variable_decl*>(d.get());
		const source_location& loc = v->location;
		std::vector<declaration_ptr> prelude;
		stmt_ctx ctx;
		if (pure_statement(v->initializer.get())) ctx.prelude = &prelude;
		vform f;
		if (v->initializer && vec_of(v->initializer, f, ctx)) {
			vform conv;
			if (slot_convert_into(f, c.kind, loc, conv)) {
				conv.sensitive += f.sensitive;
				if (conv.sensitive <= 1) {
					for (auto& p : prelude) out.push_back(std::move(p));
					for (size_t k = 0; k < math::dims(c.kind); ++k) out.push_back(comp_decl(c, k, conv.c[k], loc));
					if (c.needs_box) {
						auto copy = std::make_shared<variable_decl>(*v);
						copy->initializer = rebox_cand(c, loc);
						out.push_back(copy);
					}
					return;
				}
			}
		}
		// the declaration keeps its slot (the typed conversion and its texts), and the components unpack from it
		std::vector<declaration_ptr> prelude2;
		stmt_ctx ctx2;
		if (pure_statement(v->initializer.get())) ctx2.prelude = &prelude2;
		auto init = expr(v->initializer, ctx2);
		for (auto& p : prelude2) out.push_back(std::move(p));
		if (init == v->initializer) out.push_back(d);
		else { auto copy = std::make_shared<variable_decl>(*v); copy->initializer = init; out.push_back(copy); }
		for (size_t k = 0; k < math::dims(c.kind); ++k) out.push_back(comp_decl(c, k, unpack(c, k, loc), loc));
	}

	// An expression in statement position: a store of a lowered local lowers here, anything else rewrites
	void statement_expression(const expression_ptr& e, std::vector<declaration_ptr>& out, const source_location& loc) {
		if (e && e->get_type() == node_type::assignment_expr) {
			auto* a = static_cast<const assignment_expr*>(e.get());
			if (lower_store(a, out)) return;
		}
		std::vector<declaration_ptr> prelude;
		stmt_ctx ctx;
		if (pure_statement(e.get())) ctx.prelude = &prelude;
		auto ne = expr(e, ctx);
		for (auto& p : prelude) out.push_back(std::move(p));
		out.push_back(std::make_shared<expression_decl>(loc, ne));
	}

	// a store component by component is safe unless a later component reads a component stored before it
	// a component slot is the lowered function's own: only its identifiers read it (a callee cannot name it)
	static bool reads_slot(const expression* e, size_t slot) {
		if (!e) return false;
		switch (e->get_type()) {
			case node_type::identifier_expr: return static_cast<const identifier_expr*>(e)->slot_index == slot;
			case node_type::lambda_expr: return true;
			default: {
				bool found = false;
				for_each_child_expression(e, [&](const expression* child) { found = found || reads_slot(child, slot); });
				return found;
			}
		}
	}
	static bool cross_reads(const vform& f, const candidate& c) {
		for (size_t j = 1; j < f.n; ++j) {
			for (size_t k = 0; k < j; ++k) if (reads_slot(f.c[j].get(), c.comp_slot[k])) return true;
		}
		return false;
	}

	static token_type base_op(token_type t) {
		switch (t) {
			case token_type::plus_equal: return token_type::plus;
			case token_type::minus_equal: return token_type::minus;
			case token_type::star_equal: return token_type::star;
			case token_type::slash_equal: return token_type::slash;
			case token_type::percent_equal: return token_type::percent;
			default: return token_type::eof;
		}
	}
	static bool has_store(const expression* e) {
		if (!e) return false;
		switch (e->get_type()) {
			case node_type::assignment_expr: case node_type::import_expr: case node_type::include_expr: case node_type::yield_expr: return true;
			case node_type::unary_expr: {
				auto* u = static_cast<const unary_expr*>(e);
				if (u->op.type == token_type::plus_plus || u->op.type == token_type::minus_minus) return true;
				return has_store(u->operand.get());
			}
			default: {
				bool found = false;
				for_each_child_expression(e, [&](const expression* child) { found = found || has_store(child); });
				return found;
			}
		}
	}

	// whether e may name `symbol` (an identifier with that symbol anywhere; an unknown node may)
	static bool mentions(const expression* e, uint64_t symbol) {
		if (!e) return false;
		switch (e->get_type()) {
			case node_type::literal_expr: return false;
			case node_type::identifier_expr: return static_cast<const identifier_expr*>(e)->symbol_id == symbol;
			case node_type::binary_expr: return mentions(static_cast<const binary_expr*>(e)->left.get(), symbol) || mentions(static_cast<const binary_expr*>(e)->right.get(), symbol);
			case node_type::unary_expr: return mentions(static_cast<const unary_expr*>(e)->operand.get(), symbol);
			case node_type::member_expr: return mentions(static_cast<const member_expr*>(e)->object.get(), symbol);
			case node_type::ternary_expr: {
				auto* t = static_cast<const ternary_expr*>(e);
				return mentions(t->condition.get(), symbol) || mentions(t->then_expression.get(), symbol) || mentions(t->else_expression.get(), symbol);
			}
			case node_type::call_expr: {
				auto* c = static_cast<const call_expr*>(e);
				if (mentions(c->callee.get(), symbol)) return true;
				for (const auto& a : c->arguments) if (mentions(a.get(), symbol)) return true;
				return false;
			}
			case node_type::cast_expr: return mentions(static_cast<const cast_expr*>(e)->operand.get(), symbol);
			default: return true;
		}
	}

	// `a[i] = e;` / `a[i] op= e;` over a local vector buffer: component stores into the element. A float row the right side
	// cannot disturb (+ - *, no raise, no read of `a`) stores component by component; anything else computes every component
	// into temporaries first - the right side's, then the result's - so it raises before anything is stored, as the boxed
	// store does
	bool lower_element_store(const assignment_expr* a, bool compound, token_type op, std::vector<declaration_ptr>& out) {
		const source_location& loc = a->location;
		auto* sub = static_cast<const binary_expr*>(a->target.get());
		vform self;
		if (!vec_element(a->target, sub, self)) return false;
		if (has_store(a->value.get())) return false;
		const token_type bop = compound ? base_op(op) : token_type::eof;
		if (compound && bop == token_type::percent && !math::integral(self.kind)) return false;
		std::vector<declaration_ptr> prelude;
		stmt_ctx ctx;
		if (pure_statement(a->value.get())) ctx.prelude = &prelude;
		vform rhs;
		const bool rv = vec_of(a->value, rhs, ctx);
		if (!compound && !rv) return false;
		if (rv && rhs.sensitive > 1) return false;
		const size_t n = math::dims(self.kind);
		const uint64_t array_symbol = static_cast<const identifier_expr*>(sub->left.get())->symbol_id;
		auto component_target = [&](size_t k) -> expression_ptr {
			auto [id, view] = sym_.intern_with_view(math::component_name(self.kind, k));
			return std::make_shared<member_expr>(loc, a->target, view, id, false, false);
		};
		const bool direct = !math::integral(self.kind) && rv && rhs.sensitive == rhs.element_reads && rhs.kind == self.kind && !mentions(a->value.get(), array_symbol) &&
		                    (!compound || bop == token_type::plus || bop == token_type::minus || bop == token_type::star);
		if (direct) {
			for (auto& p : prelude) out.push_back(std::move(p));
			for (size_t k = 0; k < n; ++k) {
				auto s = std::make_shared<assignment_expr>(loc, component_target(k), op_token(compound ? op : token_type::equal, loc), rhs.c[k]);
				out.push_back(std::make_shared<expression_decl>(loc, s));
			}
			return true;
		}
		std::vector<declaration_ptr> staged;
		if (rv) {
			for (size_t k = 0; k < rhs.n; ++k) rhs.c[k] = temp(math::integral(rhs.kind), rhs.c[k], staged, loc);
			rhs.sensitive = 0;
		}
		vform f;
		if (compound) {
			if (!vec_row(bop, &self, nullptr, rv ? &rhs : nullptr, rv ? nullptr : &a->value, loc, f, &ctx)) return false;
		} else {
			f = rhs;
		}
		vform conv;
		if (!slot_convert_into(f, self.kind, loc, conv)) return false;
		conv.sensitive += f.sensitive;
		if (conv.sensitive > 1) return false;   // the element read and one raising row at most, as a local's store takes
		for (auto& p : prelude) out.push_back(std::move(p));
		for (auto& d : staged) out.push_back(std::move(d));
		expression_ptr t[4];
		for (size_t k = 0; k < n; ++k) t[k] = temp(math::integral(self.kind), conv.c[k], out, loc);
		for (size_t k = 0; k < n; ++k) out.push_back(store(component_target(k), t[k], loc));
		return true;
	}

	bool lower_store(const assignment_expr* a, std::vector<declaration_ptr>& out) {
		const source_location& loc = a->location;
		const token_type op = a->op.type;
		const bool compound = op != token_type::equal;
		if (compound && base_op(op) == token_type::eof) return false;
		if (a->target->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(a->target.get())->op.type == token_type::left_bracket) {
			return lower_element_store(a, compound, op, out);
		}
		if (a->target->get_type() == node_type::identifier_expr) {
			const int cand = cand_of(a->target.get());
			if (cand < 0) return false;
			candidate& c = cands_[static_cast<size_t>(cand)];
			std::vector<declaration_ptr> prelude;
			stmt_ctx ctx;
			if (pure_statement(a->value.get())) ctx.prelude = &prelude;
			vform f;
			bool ok = false;
			bool hazard = false;
			if (!compound) {
				ok = vec_of(a->value, f, ctx);
				hazard = ok && cross_reads(f, c);   // by slot: a scalar component read (math::vec2(p.y, p.x)) is not in f.reads
			} else if (!has_store(a->value.get())) {
				vform self;
				vec_of(a->target, self, ctx);
				vform rhs;
				const bool rv = vec_of(a->value, rhs, ctx);
				ok = vec_row(base_op(op), &self, nullptr, rv ? &rhs : nullptr, rv ? nullptr : &a->value, loc, f, &ctx);
				hazard = ok && cross_reads(f, c);   // p *= p.x reads the stored x in y's product
			}
			vform conv;
			if (ok && slot_convert_into(f, c.kind, loc, conv)) {
				conv.sensitive += f.sensitive;
				if (conv.sensitive <= 1) {
					for (auto& p : prelude) out.push_back(std::move(p));
					const size_t n = math::dims(c.kind);
					if (!hazard && conv.sensitive == 0) {
						for (size_t k = 0; k < n; ++k) out.push_back(store(comp_ident(c, k, loc), conv.c[k], loc));
					} else {
						expression_ptr t[4];
						for (size_t k = 0; k < n; ++k) t[k] = temp(math::integral(c.kind), conv.c[k], out, loc);
						for (size_t k = 0; k < n; ++k) out.push_back(store(comp_ident(c, k, loc), t[k], loc));
					}
					return true;
				}
			}
			// the source slot's typed store carries the conversion and its texts; the components unpack after it
			c.needs_box = true;
			if (compound) out.push_back(store(source_ident(c, loc), rebox_cand(c, loc), loc));
			{
				std::vector<declaration_ptr> prelude2;
				stmt_ctx ctx2;
				if (pure_statement(a->value.get())) ctx2.prelude = &prelude2;
				auto v = expr(a->value, ctx2);
				for (auto& p : prelude2) out.push_back(std::move(p));
				auto copy = std::make_shared<assignment_expr>(*a);
				copy->target = source_ident(c, loc);
				copy->value = v;
				out.push_back(std::make_shared<expression_decl>(loc, copy));
			}
			for (size_t k = 0; k < math::dims(c.kind); ++k) out.push_back(store(comp_ident(c, k, loc), unpack(c, k, loc), loc));
			return true;
		}
		if (a->target->get_type() == node_type::member_expr) {
			auto* m = static_cast<const member_expr*>(a->target.get());
			if (m->is_static) return false;
			const int cand = cand_of(m->object.get());
			if (cand < 0) return false;
			candidate& c = cands_[static_cast<size_t>(cand)];
			const int comp = math::component_of(c.kind, m->member);
			if (comp < 0) {   // no such component: the boxed store's text, on a rebuilt value
				stmt_ctx ctx;
				auto copy = std::make_shared<assignment_expr>(*a);
				auto mc = std::make_shared<member_expr>(*m);
				mc->object = rebox_cand(c, loc);
				copy->target = mc;
				copy->value = expr(a->value, ctx);
				out.push_back(std::make_shared<expression_decl>(loc, copy));
				return true;
			}
			if (compound && has_store(a->value.get())) {
				// the right side stores first (C++17): run the source slot's compound store on a rebuilt value
				c.needs_box = true;
				stmt_ctx ctx;
				out.push_back(store(source_ident(c, loc), rebox_cand(c, loc), loc));
				auto copy = std::make_shared<assignment_expr>(*a);
				auto mc = std::make_shared<member_expr>(*m);
				mc->object = source_ident(c, loc);
				copy->target = mc;
				copy->value = expr(a->value, ctx);
				out.push_back(std::make_shared<expression_decl>(loc, copy));
				for (size_t k = 0; k < math::dims(c.kind); ++k) out.push_back(store(comp_ident(c, k, loc), unpack(c, k, loc), loc));
				return true;
			}
			std::vector<declaration_ptr> prelude;
			stmt_ctx ctx;
			if (pure_statement(a->value.get())) ctx.prelude = &prelude;
			expression_ptr value = expr(a->value, ctx);
			uint8_t cls = scalar_class(a->value.get());
			if (compound) {
				const uint8_t self = math::integral(c.kind) ? c_int : c_float;
				value = bin(comp_ident(c, static_cast<size_t>(comp), loc), base_op(op), value, loc);
				cls = (cls == c_int && self == c_int && base_op(op) != token_type::slash) ? c_int
				    : (cls == c_int || cls == c_float) ? (self == c_float || cls == c_float || base_op(op) == token_type::slash ? (self == c_int && cls == c_int ? c_int : c_float) : c_int)
				    : c_unknown;
			}
			for (auto& p : prelude) out.push_back(std::move(p));
			expression_ptr narrowed;
			if (cls == c_int || cls == c_float || cls == c_bool) {
				bool raising = false;
				narrowed = math::integral(c.kind) ? i32(value, loc, &raising) : f32(value, loc);
			} else {
				narrowed = math_call("cstore$", {value, lit_int(static_cast<int64_t>(c.kind), loc)}, loc);
			}
			out.push_back(store(comp_ident(c, static_cast<size_t>(comp), loc), narrowed, loc));
			return true;
		}
		return false;
	}
};

// ---- sites ----------------------------------------------------------------------------------------------------------

struct site_input {
	function_decl* decl = nullptr;
	lambda_expr* lambda = nullptr;
};

void collect_sites_expr(const expression* e, std::vector<site_input>& out);
void collect_sites_stmt(const statement* s, std::vector<site_input>& out) {
	if (!s) return;
	switch (s->get_type()) {
		case node_type::function_decl: {
			auto* f = const_cast<function_decl*>(static_cast<const function_decl*>(s));
			out.push_back({f, nullptr});
			for (const auto& p : f->parameters) collect_sites_expr(p.default_value.get(), out);
			if (f->body) collect_sites_stmt(f->body.get(), out);
			return;
		}
		case node_type::class_decl: for (const auto& m : static_cast<const class_decl*>(s)->members) collect_sites_stmt(m.declaration.get(), out); return;
		case node_type::namespace_decl: for (const auto& d : static_cast<const namespace_decl*>(s)->declarations) collect_sites_stmt(d.get(), out); return;
		case node_type::variable_decl: collect_sites_expr(static_cast<const variable_decl*>(s)->initializer.get(), out); return;
		case node_type::expression_decl: collect_sites_expr(static_cast<const expression_decl*>(s)->expression.get(), out); return;
		case node_type::statement_decl: collect_sites_stmt(static_cast<const statement_decl*>(s)->statement.get(), out); return;
		case node_type::destructuring_decl: collect_sites_expr(static_cast<const destructuring_decl*>(s)->initializer.get(), out); return;
		case node_type::expression_stmt: collect_sites_expr(static_cast<const expression_stmt*>(s)->expression.get(), out); return;
		case node_type::block_stmt: for (const auto& d : static_cast<const block_stmt*>(s)->declarations) collect_sites_stmt(d.get(), out); return;
		case node_type::if_stmt: { auto* i = static_cast<const if_stmt*>(s); collect_sites_expr(i->condition.get(), out); collect_sites_stmt(i->then_statement.get(), out); collect_sites_stmt(i->else_statement.get(), out); return; }
		case node_type::while_stmt: { auto* w = static_cast<const while_stmt*>(s); collect_sites_expr(w->condition.get(), out); collect_sites_stmt(w->body.get(), out); return; }
		case node_type::for_stmt: { auto* f = static_cast<const for_stmt*>(s); collect_sites_stmt(f->initializer.get(), out); collect_sites_expr(f->condition.get(), out); collect_sites_expr(f->update.get(), out); collect_sites_stmt(f->body.get(), out); return; }
		case node_type::range_for_stmt: { auto* r = static_cast<const range_for_stmt*>(s); collect_sites_expr(r->container.get(), out); collect_sites_stmt(r->body.get(), out); return; }
		case node_type::parallel_for_stmt: { auto* p = static_cast<const parallel_for_stmt*>(s); collect_sites_expr(p->container.get(), out); return; }   // the body is the worker's function: not lowered
		case node_type::return_stmt: collect_sites_expr(static_cast<const return_stmt*>(s)->value.get(), out); return;
		case node_type::try_stmt: { auto* t = static_cast<const try_stmt*>(s); collect_sites_stmt(t->try_block.get(), out); collect_sites_stmt(t->catch_block.get(), out); return; }
		case node_type::switch_stmt: {
			auto* sw = static_cast<const switch_stmt*>(s);
			collect_sites_expr(sw->condition.get(), out);
			for (const auto& c : sw->cases) { collect_sites_expr(c->value.get(), out); for (const auto& b : c->body) collect_sites_stmt(b.get(), out); }
			if (sw->default_case) for (const auto& b : sw->default_case->body) collect_sites_stmt(b.get(), out);
			return;
		}
		default: return;
	}
}
void collect_sites_expr(const expression* e, std::vector<site_input>& out) {
	if (!e) return;
	switch (e->get_type()) {
		case node_type::lambda_expr: {
			auto* l = const_cast<lambda_expr*>(static_cast<const lambda_expr*>(e));
			out.push_back({nullptr, l});
			for (const auto& c : l->captures) collect_sites_expr(c.initializer.get(), out);
			collect_sites_stmt(l->body.get(), out);
			return;
		}
		default: for_each_child_expression(e, [&](const expression* child) { collect_sites_expr(child, out); }); return;
	}
}

bool lower_site(engine& eng, const site_input& in, lowered_site& out, size_t& out_count) {
	const std::vector<parameter>& params = in.decl ? in.decl->parameters : in.lambda->parameters;
	const statement_ptr body = in.decl ? std::static_pointer_cast<statement>(in.decl->body) : in.lambda->body;
	const size_t locals = in.decl ? in.decl->local_count : in.lambda->local_count;
	if (!body || body->get_type() != node_type::block_stmt) return false;
	if (in.decl && in.decl->is_coroutine) return false;
	if (contains_yield(body.get())) return false;

	std::vector<candidate> cands;
	analysis an(cands);
	an.scopes.emplace_back();
	for (const auto& p : params) {
		int cand = -1;
		if (math_kind_type(p.type.get()) && !p.is_reference && !p.ref_escaping && p.slot_index != SIZE_MAX && p.symbol_id != UINT64_MAX) {
			candidate c;
			c.symbol = p.symbol_id; c.slot = p.slot_index; c.kind = p.type->base_type; c.is_param = true; c.name = p.name;
			cands.push_back(c);
			cand = static_cast<int>(cands.size() - 1);
		}
		an.declare(p.symbol_id, cand, p.slot_index, class_of_type(p.type.get()), vec_buffer_kind(p.type.get()), element_class_of_type(p.type.get()));
	}
	an.block_body(static_cast<const block_stmt*>(body.get())->declarations);
	// a parameter unpacks at every call: worth it when the body stores it or reads it in a loop or often
	for (auto& c : cands) {
		if (c.is_param && !c.escaped && !c.stored && c.loop_refs == 0 && c.refs <= math::dims(c.kind)) c.escaped = true;
	}
	size_t lowered = 0;
	for (const auto& c : cands) if (!c.escaped) ++lowered;
	if (lowered == 0 && an.vec_buffer.empty()) return false;   // a vector buffer's element statements lower without a lowered local

	string_symbolizer& sym = *engine_access::get_symbolizer(eng);
	size_t next = locals;
	for (auto& c : cands) {
		if (c.escaped) continue;
		for (size_t k = 0; k < math::dims(c.kind); ++k) {
			const std::string name = std::string(c.name) + "$" + math::component_name(c.kind, k);
			auto [id, view] = sym.intern_with_view(name);
			c.comp_slot[k] = next++;
			c.comp_symbol[k] = id;
			c.comp_name[k] = view;
		}
	}
	bool changed = false;
	auto run = [&](bool dry, size_t& used) -> std::shared_ptr<block_stmt> {
		rewriter rw(eng, cands, an, next, dry);
		auto* b = static_cast<const block_stmt*>(body.get());
		changed = false;
		std::vector<declaration_ptr> list;
		rw.param_prologue(list, b->location);
		auto rest = rw.block_list(b->declarations, &changed);
		for (auto& d : rest) list.push_back(std::move(d));
		used = rw.slots_used();
		auto nb = std::make_shared<block_stmt>(*b);
		nb->declarations = std::move(list);
		return nb;
	};
	size_t used = 0;
	run(true, used);   // settles which declarations keep their slot (needs_box)
	auto lowered_body = run(false, used);
	if (lowered == 0 && !changed) return false;   // a vector buffer none of whose statements lowered: the parsed body runs
	out.decl = in.decl;
	out.lambda = in.lambda;
	out.source_body = body;
	out.lowered_body = lowered_body;
	out.source_locals = locals;
	out.lowered_locals = used;
	out_count = lowered;
	return true;
}

} // namespace aggregate_lowering_impl

using namespace aggregate_lowering_impl;

void build_aggregate_lowering(engine& eng, const std::vector<declaration_ptr>& declarations, aggregate_lowering_state& state) {
	if (state.built) return;
	state.built = true;
	std::vector<site_input> inputs;
	for (const auto& d : declarations) collect_sites_stmt(d.get(), inputs);
	for (const auto& in : inputs) {
		lowered_site site;
		size_t count = 0;
		if (lower_site(eng, in, site, count)) { state.sites.push_back(std::move(site)); state.lowered_locals += count; }
	}
}

void set_aggregate_view(aggregate_lowering_state& state, bool lowered) {
	if (state.lowered_view == lowered) return;
	state.lowered_view = lowered;
	for (auto& s : state.sites) {
		const statement_ptr& body = lowered ? s.lowered_body : s.source_body;
		const size_t locals = lowered ? s.lowered_locals : s.source_locals;
		if (s.decl) { s.decl->body = std::static_pointer_cast<block_stmt>(body); s.decl->local_count = locals; }
		else { s.lambda->body = body; s.lambda->local_count = locals; }
	}
}

void fold_lowered_locals(std::vector<std::pair<std::string, script_value>>& locals, engine* eng) {
	// `name$c` is a component slot of `name` (c one of x y z w, or r g b a for a color); `$...` a temporary
	auto component_of_name = [](const std::string& n, std::string& base) -> char {
		const size_t d = n.rfind('$');
		if (d == std::string::npos || d == 0 || d + 2 != n.size()) return 0;
		const char c = n.back();
		if (std::string_view("xyzwrgba").find(c) == std::string_view::npos) return 0;
		base = n.substr(0, d);
		return c;
	};
	struct group { std::string name; std::vector<script_value> comps; bool color = false; };
	std::vector<group> groups;
	for (const auto& [name, value] : locals) {
		std::string base;
		const char c = component_of_name(name, base);
		if (!c) continue;
		auto g = std::find_if(groups.begin(), groups.end(), [&](const group& x) { return x.name == base; });
		if (g == groups.end()) { groups.push_back({base, {}, c == 'r'}); g = groups.end() - 1; }
		g->comps.push_back(value);
	}
	std::vector<std::pair<std::string, script_value>> out;
	for (const auto& [name, value] : locals) {
		if (!name.empty() && name[0] == '$') continue;
		std::string base;
		const char c = component_of_name(name, base);
		const std::string& key = c ? base : name;
		auto g = std::find_if(groups.begin(), groups.end(), [&](const group& x) { return x.name == key; });
		if (g == groups.end()) { out.emplace_back(name, value); continue; }
		if (std::find_if(out.begin(), out.end(), [&](const auto& x) { return x.first == key; }) != out.end()) continue;
		const size_t n = g->comps.size();
		const bool integral = n > 0 && g->comps[0].raw_storage_index() == script_value::TYPEID_INT;
		const vk kind = g->color ? vk::jai_color_type : math::family_kind(integral, n);
		float f[4] = {0, 0, 0, 0};
		int32_t i[4] = {0, 0, 0, 0};
		bool ok = kind != vk::jai_invalid_type && n <= 4;
		for (size_t k = 0; ok && k < n; ++k) {
			const script_value& v = g->comps[k];
			if (integral && v.raw_storage_index() == script_value::TYPEID_INT) i[k] = static_cast<int32_t>(v.unchecked_as_int());
			else if (!integral && v.raw_storage_index() == script_value::TYPEID_FLOAT) f[k] = static_cast<float>(v.unchecked_as_float());
			else ok = false;
		}
		out.emplace_back(key, ok ? math::make(kind, f, i, eng) : value);
	}
	locals = std::move(out);
}
} // namespace jai::detail
