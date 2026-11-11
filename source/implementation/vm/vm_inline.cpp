// Inlined leaves ("Inlined leaves"): a plain identifier call the checker resolved to one small free
// script function compiles at its call site as the callee's body. The call becomes a temp: the operands the statement evaluates
// before the call go into typed temps first, in program order (the spill), then the graft declares the result temp (a guard on
// the callee's identity, the by-value parameters as declarations in slots above the caller's frame, a reference parameter renamed
// to the caller's variable, the body with every return a store into the result and a jump to the join; behind the join's jump the
// fallback, the real call declared into the same temp), and the statement runs once with the call and the spilled operands
// replaced by their temps. The caller's AST is never changed: the interpreter walks it, and the fallback compiles it.
#include <jaiscript/detail/const_rules.hpp>
#include <jaiscript/vm/vm_compiler.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/return_flow.hpp>
#include <jaiscript/detail/return_conv.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/detail/operator_table.hpp>
#include <jaiscript/detail/transient_read.hpp>
#include <jaiscript/detail/ref_param_writes.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/core/engine.hpp>

#include <algorithm>
#include <functional>

namespace jai::vm {

// what a body may call besides math:: intrinsics: the registered stdlib aliases, and (one level down) a leaf that grafts itself
struct land_ctx {
	const engine* eng = nullptr;
	std::function<bool(const call_expr*, const std::vector<const type_info*>*)> nested;
	const std::vector<const type_info*>* frame = nullptr;   // the declared types of the slots a body's call reads its arguments from (null: the caller's frame)
	const std::unordered_map<const expression*, const call_expr*>* origins = nullptr;   // a clone's call: the body's call it copies, whose arguments the verdict reads
};

namespace {

constexpr size_t k_inline_max_nodes = 512;   // the cloned body and the cloned statement together
constexpr size_t k_inline_max_hoists = 8;    // calls hoisted out of one statement
constexpr size_t k_inline_max_tree = 4096;   // the cloned bodies of one outermost graft and every graft compiled inside it, at any depth
constexpr size_t k_inline_call_max_nodes = 64;   // a body that keeps a call grafts only while its clone costs about what the frame it saves does

struct inline_context {
	size_t slot_offset = 0;                                        // the callee's slots move above the caller's frame
	std::vector<std::shared_ptr<identifier_expr>> renames;         // per callee slot: the caller's identifier a reference parameter takes, or null
	std::vector<uint8_t> ref_slots;                                // per callee slot: a reference parameter
	std::shared_ptr<identifier_expr> result;                       // the result temp (null: a void callee)
	type_info_ptr result_type;
	std::string_view callee_name;                                  // the return site's owner (an unproven return converts at the result's declaration)
	size_t nodes = 0;
	size_t budget = k_inline_max_nodes;                            // the body's clone is bounded; the statement's clone is compiled once whatever its size
	bool ok = true;
	const char* why = nullptr;                                     // the first rule that kept the call
	bool writes_global = false;
	bool writes_member = false;
	bool writes_ref = false;                                       // a store or an increment through a reference parameter: the caller's variable or element
	bool calls = false;                                            // a call the body keeps: made from the graft, its frame one level below the grafted one
	std::vector<const type_info*> slot_types;                      // per callee slot: its declared type (parameters and the body's declarations)
	std::vector<uint8_t> ref_stored;                               // per callee slot: a reference parameter stored as a whole
	std::vector<uint8_t> ref_store_unproven;                       // per callee slot: such a store the checker did not prove lands the parameter's type
	bool reads_env = false;                                        // the body names an environment value (a global, a stdlib alias)
	struct land_ctx lc;                                            // the stdlib aliases and the nested leaves a body may call
	std::vector<std::shared_ptr<ast_node>> pins;
	std::vector<uint64_t> declared_names;                          // the callee's locals: declared in the caller for the graft's compile
	const call_expr* replace = nullptr;                            // the statement's clone: this call becomes the result temp
	bool in_loop = false;                                          // the graft sits inside a loop: every landing must be a block instruction
	int loop_depth = 0;                                            // loops of the callee's own the clone is inside (a break or continue binds there)
	std::vector<std::pair<const expression*, std::shared_ptr<identifier_expr>>> spills;   // the statement's clone: a spilled operand becomes its temp
	std::vector<std::pair<const expression*, std::shared_ptr<identifier_expr>>> container_guards;   // the statement's clone: an element read whose container a guard took (its temp)
	std::vector<std::pair<const binary_expr*, std::shared_ptr<identifier_expr>>> guarded_reads;     // their clones
	std::shared_ptr<identifier_expr> receiver;                     // a method graft: `this` and the bare fields read through the caller's receiver
	bool receiver_this = false;                                    // a method graft on the caller's own `this`: the body's bare fields are the caller's
	bool caller_this = false;                                      // the statement's own clone: its `this` is the caller's
	std::vector<uint64_t> global_ids;                              // a method graft's bare program-scope names (the guard: no member of the receiver takes one)
	std::vector<uint64_t> sibling_ids;                             // a method graft's sibling methods called by bare name, called on the receiver (the guard: each still a method there)
	bool siblings = false;                                         // the graft compiles in a method body, where a sibling the clone calls grafts as in the method's own
	std::function<bool(const identifier_expr*)> caller_global;     // the caller's frame reads this bare program-scope name as the program's (its class takes no member of it)
	std::function<bool(const identifier_expr*)> caller_field;      // the caller's class lays this bare field out as a plain field of its own receiver
	bool caller_globals = false;                                   // a bare program-scope name read in the caller's frame
	bool method_graft() const { return receiver || receiver_this; }
	bool returns_proven = false;                                   // an untyped lambda whose returns the checker proved one scalar kind
	std::unordered_map<const expression*, const call_expr*> origins;   // per cloned call: the call it copies
	std::vector<script_value_type> site_kinds;                     // per callee slot: the kind the site's arguments give it (a stamp's input, never a declaration)
	script_value_type replace_kind = script_value_type::jai_null_type;   // the statement's clone: the kind the graft's result has
	bool stamps = false;                                           // an operation the checker left unclassified takes the class these kinds give (every consumer guards it)
	const class_definition* field_class = nullptr;
	std::vector<uint64_t> field_ids;
	std::unordered_map<const call_expr*, std::pair<uint32_t, uint32_t>>* alias_pins = nullptr;   // the statement's clone: a pinned alias call keeps its guard
	std::unordered_map<const call_expr*, std::pair<uint32_t, uint32_t>>* callee_probes = nullptr;   // and a probed call its probe
	std::unordered_map<const call_expr*, std::vector<uint32_t>>* staged_arguments = nullptr;
	template <class T, class... A> std::shared_ptr<T> make(A&&... a) {
		auto n = std::make_shared<T>(std::forward<A>(a)...);
		pins.push_back(n);
		if (++nodes > budget) { fail("a body too large"); }
		return n;
	}
	void fail(const char* w) { if (ok) { why = w; } ok = false; }
};

bool scalar_type(const type_info* t) {
	if (!t) return false;
	switch (t->base_type) {
	case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type: return true;
	default: return false;
	}
}
bool any_type(const type_info* t) { return !t || t->base_type == script_value_type::jai_any_type; }
// a by-value parameter of any registered type but a scalar or var (a string, an instance, a handle, a container, a math vector, a host type):
// its declaration binds the argument by the parameter's own lane (vm_backend::bind_graft_parameter)
bool value_param(const parameter& p) {
	const type_info* t = p.type.get();
	return !p.is_reference && t && !t->is_reference() && t->base_type != script_value_type::jai_any_type && t->base_type != script_value_type::jai_function_type &&
	       t->base_type != script_value_type::jai_null_type && t->type_name != "auto" &&
	       !(t->base_type == script_value_type::jai_int_type || t->base_type == script_value_type::jai_float_type || t->base_type == script_value_type::jai_bool_type);
}
bool void_type(const type_info* t) { return t && t->base_type == script_value_type::jai_null_type; }   // `void` is the null type
bool var_reference(const type_info* t) { return t && t->is_reference() && any_type(t->element_type().get()); }
bool math_call(const call_expr* ce) {
	return ce->callee && ce->callee->get_type() == node_type::member_expr && detail::math_intrinsic_for_call(static_cast<const member_expr*>(ce->callee.get())) != detail::math_fn::none;
}
// `sqrt(x)` naming a registered stdlib math alias from outside the frame: the math temp's guarded form (the real call on a miss)
bool nested_call(const call_expr* ce, const land_ctx* lc) {
	if (!lc || !lc->nested) return false;
	if (lc->origins) { if (const auto it = lc->origins->find(ce); it != lc->origins->end()) { ce = it->second; } }
	return lc->nested(ce, lc->frame);
}
bool alias_call(const call_expr* ce, const engine* eng) {
	if (!eng || !ce->callee || ce->callee->get_type() != node_type::identifier_expr || ce->arguments.size() != 1) return false;
	auto* id = static_cast<const identifier_expr*>(ce->callee.get());
	return !id->global_qualified && id->slot_index == SIZE_MAX && detail::engine_access::math_alias_fn(*eng, id->symbol_id) < detail::engine_access::k_math_alias_rows;
}
// a receiver a body names: a name, or a member chain over one (each step a member read the body may make anyway)
bool named_receiver(const expression* e) {
	while (e && e->get_type() == node_type::member_expr) {
		auto* m = static_cast<const member_expr*>(e);
		if (m->is_static || m->null_safe) return false;
		e = m->object.get();
	}
	return e && e->get_type() == node_type::identifier_expr;
}
// `name.size()` (or `a.b.size()`) on a receiver the checker proved an array: it reads the array and runs no user code (the builtin call
// guards the kind)
bool size_call(const call_expr* ce) {
	if (!ce->callee || ce->callee->get_type() != node_type::member_expr || !ce->arguments.empty() || ce->builtin_receiver != 2) return false;
	auto* m = static_cast<const member_expr*>(ce->callee.get());
	return !m->is_static && !m->null_safe && m->member == "size" && named_receiver(m->object.get());
}
// a loop anywhere in a body (a graft into a fiber frame runs it without a block)
bool body_has_loop(const statement* s) {
	if (!s) return false;
	switch (s->get_type()) {
	case node_type::while_stmt: case node_type::for_stmt: case node_type::range_for_stmt: return true;
	case node_type::block_stmt: for (const auto& d : static_cast<const block_stmt*>(s)->declarations) { if (body_has_loop(d.get())) return true; } return false;
	case node_type::statement_decl: return body_has_loop(static_cast<const statement_decl*>(s)->statement.get());
	case node_type::if_stmt: { auto* i = static_cast<const if_stmt*>(s); return body_has_loop(i->then_statement.get()) || body_has_loop(i->else_statement.get()); }
	case node_type::try_stmt: { auto* t = static_cast<const try_stmt*>(s); return body_has_loop(t->try_block.get()) || body_has_loop(t->catch_block.get()); }
	case node_type::switch_stmt: {
		auto* sw = static_cast<const switch_stmt*>(s);
		for (const auto& c : sw->cases) { for (const auto& b : c->body) { if (body_has_loop(b.get())) return true; } }
		if (sw->default_case) { for (const auto& b : sw->default_case->body) { if (body_has_loop(b.get())) return true; } }
		return false;
	}
	default: return false;
	}
}
bool logical_op(token_type t) { return t == token_type::ampersand_ampersand || t == token_type::pipe_pipe; }
// an operator whose right operand runs only on some evaluations (&&, ||, ??)
bool short_circuit_op(token_type t) { return logical_op(t) || t == token_type::question_question; }
bool incdec_op(token_type t) { return t == token_type::plus_plus || t == token_type::minus_minus; }

// an expression whose value lands in a slot by a block instruction (the typed declaration rows, the fused binary, the proven element
// read, the math row): an identifier (a slot, or a global through the move's environment operand; `::name` stays general) or a
// literal; arithmetic, a comparison, a shift or a unary of those; an element read of a proven kind over a named container; a math
// intrinsic of those; a ternary as a whole value or as an operand lifted into a temp. A cast, a store or any other call lands on the
// general form, which a block does not run.
bool landable(const expression* e, const land_ctx* eng);
// an operand of a block instruction: a landable value, or a global read (the fused binary's environment rows)
bool block_operand(const expression* e, const land_ctx* eng) { return e && (e->get_type() == node_type::identifier_expr || landable(e, eng)); }
// an operand inside an operation: a ternary lands only as a whole declaration or store (or an arm of one), never as an operand
bool inner_operand(const expression* e, const land_ctx* eng) { return e && e->get_type() != node_type::ternary_expr && block_operand(e, eng); }
bool landable(const expression* e, const land_ctx* eng) {
	if (!e) return false;
	switch (e->get_type()) {
	case node_type::literal_expr: return true;
	case node_type::identifier_expr: return !static_cast<const identifier_expr*>(e)->global_qualified;
	case node_type::member_expr: { auto* m = static_cast<const member_expr*>(e); return !m->is_static && m->object && m->object->get_type() == node_type::identifier_expr; }   // a member site over a named instance
	case node_type::binary_expr: {
		auto* b = static_cast<const binary_expr*>(e);
		if (b->op.type == token_type::left_bracket) {
			const uint8_t cls = b->static_operand_class;
			return (cls == operand_class_int_int || cls == operand_class_float_float) && b->left && b->left->get_type() == node_type::identifier_expr && inner_operand(b->right.get(), eng);
		}
		// a ternary operand lands in a temp ahead of the operation when its partner cannot observe the order: a literal, a name (an environment
		// name on its left reads into a temp first), a value binary or another ternary, as vm_compiler::compile_binary lifts it
		auto ternary_side = [&](const expression* x, const expression* partner) {
			if (!landable(x, eng)) return false;
			if (partner->get_type() == node_type::literal_expr || partner->get_type() == node_type::binary_expr || partner->get_type() == node_type::ternary_expr) return true;
			if (partner->get_type() != node_type::identifier_expr) return false;
			auto* id = static_cast<const identifier_expr*>(partner);
			return !id->global_qualified;
		};
		const bool lt = b->left && b->left->get_type() == node_type::ternary_expr, rt = b->right && b->right->get_type() == node_type::ternary_expr;
		return (lt ? ternary_side(b->left.get(), b->right.get()) : inner_operand(b->left.get(), eng)) &&
		       (rt ? ternary_side(b->right.get(), b->left.get()) : inner_operand(b->right.get(), eng));
	}
	case node_type::unary_expr: { auto* u = static_cast<const unary_expr*>(e); return !incdec_op(u->op.type) && inner_operand(u->operand.get(), eng); }
	case node_type::ternary_expr: {   // the condition's jumps and one landing per arm: a declaration or a slot store from it is a block's
		auto* x = static_cast<const ternary_expr*>(e);
		return inner_operand(x->condition.get(), eng) && block_operand(x->then_expression.get(), eng) && block_operand(x->else_expression.get(), eng);
	}
	case node_type::call_expr: {
		auto* ce = static_cast<const call_expr*>(e);
		if (!math_call(ce) && !alias_call(ce, eng ? eng->eng : nullptr) && !nested_call(ce, eng)) return false;
		for (const auto& a : ce->arguments) { if (!inner_operand(a.get(), eng)) return false; }
		return true;
	}
	default: return false;
	}
}
// a body statement a block runs: a store of a landable value into a slot, a renamed slot, a global, a field or an element; an increment
// of a slot identifier
bool landable_stmt(const expression* e, const land_ctx* eng) {
	if (!e) return false;
	switch (e->get_type()) {
	case node_type::assignment_expr: {
		auto* a = static_cast<const assignment_expr*>(e);
		const node_type tk = a->target->get_type();
		if (tk == node_type::binary_expr) { auto* b = static_cast<const binary_expr*>(a->target.get()); if (b->op.type != token_type::left_bracket || !landable(b->right.get(), eng)) return false; }
		else if (tk == node_type::member_expr) { auto* m = static_cast<const member_expr*>(a->target.get()); if (m->is_static || !m->object || m->object->get_type() != node_type::identifier_expr) return false; }
		else if (tk != node_type::identifier_expr) return false;
		return block_operand(a->value.get(), eng);
	}
	case node_type::unary_expr: { auto* u = static_cast<const unary_expr*>(e); return incdec_op(u->op.type) && u->operand && u->operand->get_type() == node_type::identifier_expr && static_cast<const identifier_expr*>(u->operand.get())->slot_index != SIZE_MAX; }
	default: return false;
	}
}

expression_ptr clone_expr(const expression_ptr& e, inline_context& c);

std::shared_ptr<identifier_expr> copy_identifier(const identifier_expr* r, const source_location& loc, inline_context& c) {
	auto n = c.make<identifier_expr>(loc, r->name, r->symbol_id);
	n->slot_index = r->slot_index; n->global_qualified = r->global_qualified; n->names_value_decl = r->names_value_decl; n->names_known_value = r->names_known_value;
	n->this_field = r->this_field; n->program_name = r->program_name;
	return n;
}

std::shared_ptr<identifier_expr> clone_identifier(const identifier_expr* id, inline_context& c) {
	if (id->slot_index != SIZE_MAX && id->slot_index < c.renames.size() && c.renames[id->slot_index]) { return copy_identifier(c.renames[id->slot_index].get(), id->location, c); }
	auto n = copy_identifier(id, id->location, c);
	n->slot_index = id->slot_index == SIZE_MAX ? SIZE_MAX : id->slot_index + c.slot_offset;
	return n;
}

// the kind plain arithmetic yields (no numeric operator registered); jai_null_type: not known
script_value_type arith_kind(token_type op, script_value_type l, script_value_type r) {
	using K = script_value_type;
	const bool li = l == K::jai_int_type, lf = l == K::jai_float_type, ri = r == K::jai_int_type, rf = r == K::jai_float_type;
	switch (op) {
	case token_type::plus: case token_type::minus: case token_type::star: case token_type::slash:
		if (li && ri) return K::jai_int_type;
		return (li || lf) && (ri || rf) ? K::jai_float_type : K::jai_null_type;
	case token_type::percent: case token_type::ampersand: case token_type::pipe: case token_type::caret: case token_type::left_shift: case token_type::right_shift:
		return li && ri ? K::jai_int_type : K::jai_null_type;
	case token_type::less: case token_type::greater: case token_type::less_equal: case token_type::greater_equal: case token_type::equal_equal: case token_type::bang_equal:
		return (li || lf) && (ri || rf) ? K::jai_bool_type : K::jai_null_type;
	default: return K::jai_null_type;
	}
}
// a reading builtin (detail::ref_write_reading_method: has, find, substr, keys, ...) of a string, an array or a map the checker proved, on a
// named receiver: it writes nothing and runs no user code while no argument can be a callable (a literal, a value of a scalar kind, or a
// name declared a type that is no function and no var)
script_value_type value_kind(const expression* e, const inline_context& c);
bool reading_builtin_call(const call_expr* ce, const inline_context& c) {
	if (!ce->callee || ce->callee->get_type() != node_type::member_expr || ce->builtin_receiver == 0) return false;
	auto* m = static_cast<const member_expr*>(ce->callee.get());
	if (m->is_static || m->null_safe || !named_receiver(m->object.get()) || !detail::ref_write_reading_method(m->member)) return false;
	for (const auto& a : ce->arguments) {
		if (a->get_type() == node_type::literal_expr || value_kind(a.get(), c) != script_value_type::jai_null_type) continue;
		if (a->get_type() != node_type::identifier_expr) return false;
		const size_t s = static_cast<const identifier_expr*>(a.get())->slot_index;
		const type_info* t = s < c.slot_types.size() ? c.slot_types[s] : nullptr;
		if (!t || t->is_reference() || t->base_type == script_value_type::jai_any_type || t->base_type == script_value_type::jai_function_type) return false;
	}
	return true;
}

// the kind a value of the callee's body has at run time, from its own declarations: a literal, a name declared a scalar (a reference
// parameter's declared type, which the rename requires of the caller's variable), plain arithmetic, a comparison, a negation, a cast to
// a scalar, a ternary whose arms agree; jai_null_type otherwise
script_value_type value_kind(const expression* e, const inline_context& c) {
	using K = script_value_type;
	if (!e) return K::jai_null_type;
	switch (e->get_type()) {
	case node_type::literal_expr: {
		const script_value& v = static_cast<const literal_expr*>(e)->value;
		return v.is_int() ? K::jai_int_type : v.is_float() ? K::jai_float_type : v.is_bool() ? K::jai_bool_type : K::jai_null_type;
	}
	case node_type::identifier_expr: {
		auto* id = static_cast<const identifier_expr*>(e);
		if (id->slot_index == SIZE_MAX || id->slot_index >= c.slot_types.size() || !scalar_type(c.slot_types[id->slot_index])) return K::jai_null_type;
		return c.slot_types[id->slot_index]->base_type;
	}
	case node_type::binary_expr: { auto* b = static_cast<const binary_expr*>(e); return arith_kind(b->op.type, value_kind(b->left.get(), c), value_kind(b->right.get(), c)); }
	case node_type::unary_expr: {
		auto* u = static_cast<const unary_expr*>(e);
		const K k = value_kind(u->operand.get(), c);
		if (u->op.type == token_type::minus) return k == K::jai_int_type || k == K::jai_float_type ? k : K::jai_null_type;
		if (u->op.type == token_type::tilde) return k == K::jai_int_type ? k : K::jai_null_type;
		return K::jai_null_type;
	}
	case node_type::ternary_expr: {
		auto* t = static_cast<const ternary_expr*>(e);
		const K a = value_kind(t->then_expression.get(), c);
		return a == value_kind(t->else_expression.get(), c) ? a : K::jai_null_type;
	}
	case node_type::cast_expr: { auto* k = static_cast<const cast_expr*>(e); return !k->by_reference && !k->instance_test && scalar_type(k->target.get()) ? k->target->base_type : K::jai_null_type; }
	default: return K::jai_null_type;
	}
}

// a store's target: a global, the caller's variable through a reference parameter (renamed or aliased), a field or an element
void note_store(const expression* target, inline_context& c, bool proven) {
	if (target->get_type() == node_type::identifier_expr) {
		auto* t = static_cast<const identifier_expr*>(target);
		const bool renamed = t->slot_index != SIZE_MAX && t->slot_index < c.renames.size() && c.renames[t->slot_index];
		if (t->slot_index == SIZE_MAX || renamed) { c.writes_global = true; }
		if (t->slot_index != SIZE_MAX && t->slot_index < c.ref_slots.size() && c.ref_slots[t->slot_index]) {
			c.writes_ref = true;
			if (t->slot_index < c.ref_stored.size()) { c.ref_stored[t->slot_index] = 1; if (!proven) { c.ref_store_unproven[t->slot_index] = 1; } }
		}
		return;
	}
	c.writes_member = true;
}

script_value_type spill_kind(const expression* e);
bool ever_stamped(const std::weak_ptr<const function_decl>& w);

// the kind of a value from the kinds of its names: a literal, plain arithmetic, a comparison, a logical operator, a negation, a cast to
// a scalar, a ternary whose arms agree, a subtree the checker classified, the call the statement's clone replaces; jai_null_type otherwise
template <class F>
script_value_type kind_of(const expression* e, const F& slot_kind, const call_expr* replace, script_value_type replace_kind) {
	using K = script_value_type;
	if (!e) return K::jai_null_type;
	switch (e->get_type()) {
	case node_type::literal_expr: {
		const script_value& v = static_cast<const literal_expr*>(e)->value;
		return v.is_int() ? K::jai_int_type : v.is_float() ? K::jai_float_type : v.is_bool() ? K::jai_bool_type : K::jai_null_type;
	}
	case node_type::identifier_expr: {
		auto* id = static_cast<const identifier_expr*>(e);
		return id->slot_index == SIZE_MAX || id->global_qualified ? K::jai_null_type : slot_kind(id->slot_index);
	}
	case node_type::binary_expr: {
		auto* b = static_cast<const binary_expr*>(e);
		if (logical_op(b->op.type)) return K::jai_bool_type;
		if (b->static_operand_class != operand_class_unknown) {
			const K k = spill_kind(b);
			if (k == K::jai_int_type || k == K::jai_float_type || k == K::jai_bool_type) return k;
		}
		if (b->op.type == token_type::left_bracket) return K::jai_null_type;
		return arith_kind(b->op.type, kind_of(b->left.get(), slot_kind, replace, replace_kind), kind_of(b->right.get(), slot_kind, replace, replace_kind));
	}
	case node_type::unary_expr: {
		auto* u = static_cast<const unary_expr*>(e);
		if (u->op.type == token_type::bang) return K::jai_bool_type;
		const K k = kind_of(u->operand.get(), slot_kind, replace, replace_kind);
		if (u->op.type == token_type::minus) return k == K::jai_int_type || k == K::jai_float_type ? k : K::jai_null_type;
		if (u->op.type == token_type::tilde) return k == K::jai_int_type ? k : K::jai_null_type;
		return K::jai_null_type;
	}
	case node_type::ternary_expr: {
		auto* t = static_cast<const ternary_expr*>(e);
		const K a = kind_of(t->then_expression.get(), slot_kind, replace, replace_kind);
		return a == kind_of(t->else_expression.get(), slot_kind, replace, replace_kind) ? a : K::jai_null_type;
	}
	case node_type::cast_expr: { auto* k = static_cast<const cast_expr*>(e); return !k->by_reference && !k->instance_test && scalar_type(k->target.get()) ? k->target->base_type : K::jai_null_type; }
	case node_type::call_expr: return e == replace ? replace_kind : K::jai_null_type;
	default: return K::jai_null_type;
	}
}

uint8_t kind_class(script_value_type l, script_value_type r) {
	using K = script_value_type;
	if (l == K::jai_int_type && r == K::jai_int_type) return operand_class_int_int;
	if (l == K::jai_float_type && r == K::jai_float_type) return operand_class_float_float;
	if (l == K::jai_int_type && r == K::jai_float_type) return operand_class_int_float;
	if (l == K::jai_float_type && r == K::jai_int_type) return operand_class_float_int;
	if (l == K::jai_bool_type && r == K::jai_bool_type) return operand_class_bool_bool;
	return operand_class_unknown;
}

// a stamp's kind: the site's kind of a callee slot, else its declared scalar type (the statement's clone: the caller's); a reference
// parameter is the caller's lvalue, whose kind is the caller's stamps' business
script_value_type stamp_kind(const expression* e, const inline_context& c) {
	return kind_of(e, [&c](size_t s) {
		if (s < c.ref_slots.size() && c.ref_slots[s]) return script_value_type::jai_null_type;
		if (s < c.site_kinds.size() && c.site_kinds[s] != script_value_type::jai_null_type) return c.site_kinds[s];
		return s < c.slot_types.size() && scalar_type(c.slot_types[s]) ? c.slot_types[s]->base_type : script_value_type::jai_null_type;
	}, c.replace, c.replace_kind);
}

// the definitions of a callee's untyped slots at one site (a `var` parameter's argument, a `var` local's initializer, each store, each
// increment) and its returns' values, for the kinds a fixpoint gives them
struct site_walk {
	std::vector<script_value_type> kind;   // per callee slot (jai_null_type: none)
	std::vector<uint8_t> open;             // an untyped slot the walk types
	struct def { size_t slot; token_type op; const expression* value; };   // equal: the value; plus_plus: an increment; else a compound store's base op
	std::vector<def> defs;
	std::vector<const expression*> returns;
	bool ok = true;
	const engine* eng = nullptr;           // the registered stdlib aliases
};

script_value_type walk_kind(const expression* e, const site_walk& w);

// every open slot keeps its kind only while each of its definitions yields that kind under the kinds so far, to a fixpoint
void settle(site_walk& w) {
	using K = script_value_type;
	for (bool changed = true; changed;) {
		changed = false;
		for (const site_walk::def& d : w.defs) {
			if (!w.open[d.slot] || w.kind[d.slot] == K::jai_null_type) continue;
			const K cur = w.kind[d.slot];
			const K k = d.op == token_type::equal ? walk_kind(d.value, w)
			          : d.op == token_type::plus_plus ? (cur == K::jai_int_type || cur == K::jai_float_type ? cur : K::jai_null_type)
			          : arith_kind(d.op, cur, walk_kind(d.value, w));
			if (k != cur) { w.kind[d.slot] = K::jai_null_type; changed = true; }
		}
	}
}

script_value_type walk_kind(const expression* e, const site_walk& w) {
	return kind_of(e, [&w](size_t s) { return s < w.kind.size() ? w.kind[s] : script_value_type::jai_null_type; }, nullptr, script_value_type::jai_null_type);
}

script_value_type scalar_kind(script_value_type k) {
	return k == script_value_type::jai_int_type || k == script_value_type::jai_float_type || k == script_value_type::jai_bool_type ? k : script_value_type::jai_null_type;
}

void walk_unknown(const expression* a, site_walk& w) {   // a slot a call may bind by reference takes no kind
	if (!a || a->get_type() != node_type::identifier_expr) return;
	const size_t s = static_cast<const identifier_expr*>(a)->slot_index;
	if (s < w.kind.size()) { w.open[s] = 0; w.kind[s] = script_value_type::jai_null_type; }
}

void walk_expr(const expression* e, site_walk& w) {
	if (!e || !w.ok) return;
	switch (e->get_type()) {
	case node_type::literal_expr: case node_type::identifier_expr: case node_type::this_expr: return;
	case node_type::binary_expr: { auto* b = static_cast<const binary_expr*>(e); walk_expr(b->left.get(), w); walk_expr(b->right.get(), w); return; }
	case node_type::unary_expr: {
		auto* u = static_cast<const unary_expr*>(e);
		if (incdec_op(u->op.type) && u->operand && u->operand->get_type() == node_type::identifier_expr) {
			const size_t s = static_cast<const identifier_expr*>(u->operand.get())->slot_index;
			if (s < w.kind.size()) { w.defs.push_back({s, token_type::plus_plus, nullptr}); }
		}
		walk_expr(u->operand.get(), w);
		return;
	}
	case node_type::assignment_expr: {
		auto* a = static_cast<const assignment_expr*>(e);
		if (a->target->get_type() == node_type::identifier_expr) {
			const size_t s = static_cast<const identifier_expr*>(a->target.get())->slot_index;
			token_type op = a->op.type;
			switch (op) {
			case token_type::plus_equal: op = token_type::plus; break;
			case token_type::minus_equal: op = token_type::minus; break;
			case token_type::star_equal: op = token_type::star; break;
			case token_type::slash_equal: op = token_type::slash; break;
			case token_type::percent_equal: op = token_type::percent; break;
			default: break;   // any other compound keeps its own token, which yields no kind
			}
			if (s < w.kind.size()) { w.defs.push_back({s, op, a->value.get()}); }
		} else {
			walk_expr(a->target.get(), w);
		}
		walk_expr(a->value.get(), w);
		return;
	}
	case node_type::member_expr: { auto* m = static_cast<const member_expr*>(e); if (!m->is_static) { walk_expr(m->object.get(), w); } return; }
	case node_type::ternary_expr: { auto* t = static_cast<const ternary_expr*>(e); walk_expr(t->condition.get(), w); walk_expr(t->then_expression.get(), w); walk_expr(t->else_expression.get(), w); return; }
	case node_type::cast_expr: walk_expr(static_cast<const cast_expr*>(e)->operand.get(), w); return;
	case node_type::call_expr: {
		auto* ce = static_cast<const call_expr*>(e);
		// a name passed to a call keeps its kind only where the call provably binds it by value: a math:: intrinsic, a stdlib alias (while
		// its name binds the builtin, its guard's condition), a by-value parameter of the callee the checker proved (its guard's condition);
		// any other call may bind it by reference
		const bool by_value = math_call(ce) || alias_call(ce, w.eng);
		const std::shared_ptr<const function_decl> callee = ever_stamped(ce->proven_callee_decl) ? ce->proven_callee_decl.lock() : nullptr;
		for (size_t i = 0; i < ce->arguments.size(); ++i) {
			const bool ref = callee ? (i >= callee->parameters.size() || callee->parameters[i].is_reference) : !by_value;
			if (ref) { walk_unknown(ce->arguments[i].get(), w); }
			walk_expr(ce->arguments[i].get(), w);
		}
		return;
	}
	default: w.ok = false; return;
	}
}

void walk_decls(const std::vector<declaration_ptr>& decls, site_walk& w);

void walk_stmt(const statement* s, site_walk& w) {
	if (!s || !w.ok) return;
	switch (s->get_type()) {
	case node_type::block_stmt: walk_decls(static_cast<const block_stmt*>(s)->declarations, w); return;
	case node_type::if_stmt: { auto* i = static_cast<const if_stmt*>(s); walk_expr(i->condition.get(), w); walk_stmt(i->then_statement.get(), w); walk_stmt(i->else_statement.get(), w); return; }
	case node_type::return_stmt: { auto* r = static_cast<const return_stmt*>(s); if (r->value) { w.returns.push_back(r->value.get()); walk_expr(r->value.get(), w); } return; }
	case node_type::expression_stmt: walk_expr(static_cast<const expression_stmt*>(s)->expression.get(), w); return;
	case node_type::for_stmt: {
		auto* f = static_cast<const for_stmt*>(s);
		if (f->initializer) { walk_decls({f->initializer}, w); }
		walk_expr(f->condition.get(), w); walk_expr(f->update.get(), w); walk_stmt(f->body.get(), w);
		return;
	}
	case node_type::while_stmt: { auto* ws = static_cast<const while_stmt*>(s); walk_expr(ws->condition.get(), w); walk_stmt(ws->body.get(), w); return; }
	case node_type::break_stmt: case node_type::continue_stmt: return;
	default: w.ok = false; return;
	}
}

void walk_decls(const std::vector<declaration_ptr>& decls, site_walk& w) {
	for (const declaration_ptr& d : decls) {
		if (!w.ok) return;
		switch (d->get_type()) {
		case node_type::variable_decl: {
			auto* v = static_cast<const variable_decl*>(d.get());
			walk_expr(v->initializer.get(), w);
			if (v->type && v->type->is_reference()) { walk_unknown(v->initializer.get(), w); }
			if (v->slot_index >= w.kind.size()) break;
			if (v->ref_escaping) { w.kind[v->slot_index] = script_value_type::jai_null_type; break; }   // a reference or a capture may write it
			const bool reference = v->type && v->type->is_reference();
			if (scalar_type(v->type.get()) && !reference) { w.kind[v->slot_index] = v->type->base_type; break; }
			if (!any_type(v->type.get()) || reference || v->is_static || !v->initializer) { w.kind[v->slot_index] = script_value_type::jai_null_type; break; }
			w.open[v->slot_index] = 1;
			w.kind[v->slot_index] = scalar_kind(walk_kind(v->initializer.get(), w));
			w.defs.push_back({v->slot_index, token_type::equal, v->initializer.get()});
			break;
		}
		case node_type::expression_decl: walk_expr(static_cast<const expression_decl*>(d.get())->expression.get(), w); break;
		case node_type::statement_decl: walk_stmt(static_cast<const statement_decl*>(d.get())->statement.get(), w); break;
		default: w.ok = false; return;
		}
	}
}

// the expression forms an inlined body and an inlined statement may hold: no call but a math intrinsic, no lambda, no `this`, no
// literal container, no yield, no throw, no include. The clone keeps every proof the checker stamped, so the graft compiles as
// typed as the original.
expression_ptr clone_expr(const expression_ptr& e, inline_context& c) {
	if (!c.ok || !e) return nullptr;
	for (const auto& s : c.spills) { if (s.first == e.get()) { return copy_identifier(s.second.get(), e->location, c); } }
	switch (e->get_type()) {
	case node_type::literal_expr: ++c.nodes; return e;   // immutable: shared as it is
	case node_type::identifier_expr: {
		auto* id = static_cast<const identifier_expr*>(e.get());
		if (c.method_graft() && id->slot_index == SIZE_MAX) {
			// a method's bare name: a field the checker proved (no user accessor, the method's own class) reads through the receiver, or is the
			// caller's own field on the caller's `this`; a program-scope name the checker resolved past every member is the caller's when the
			// caller's class takes no member of it (the guard: neither does the receiver's); anything else keeps the call
			if (id->global_qualified || !id->this_field) {
				if (id->global_qualified || !id->program_name || id->symbol_id == UINT64_MAX || !c.caller_global || !c.caller_global(id)) { c.fail("a bare name in a method the graft cannot resolve"); return nullptr; }
				if (std::find(c.global_ids.begin(), c.global_ids.end(), id->symbol_id) == c.global_ids.end()) { c.global_ids.push_back(id->symbol_id); }
				c.caller_globals = true;
				return copy_identifier(id, id->location, c);
			}
			if (std::find(c.field_ids.begin(), c.field_ids.end(), id->symbol_id) == c.field_ids.end()) { c.field_ids.push_back(id->symbol_id); }
			if (c.receiver_this) {
				if (!c.caller_field || !c.caller_field(id)) { c.fail("a bare name in a method the graft cannot resolve"); return nullptr; }
				return copy_identifier(id, id->location, c);
			}
			auto n = c.make<member_expr>(id->location, copy_identifier(c.receiver.get(), id->location, c), id->name, id->symbol_id, false, false);
			return n;
		}
		if (id->slot_index == SIZE_MAX) { c.reads_env = true; }
		return clone_identifier(id, c);
	}
	case node_type::this_expr:
		if (c.receiver) { return copy_identifier(c.receiver.get(), e->location, c); }
		if (c.receiver_this) { return c.make<this_expr>(e->location); }
		if (c.caller_this) { ++c.nodes; return e; }
		c.fail("an expression the graft does not take"); return nullptr;
	case node_type::binary_expr: {
		auto* b = static_cast<const binary_expr*>(e.get());
		auto l = clone_expr(b->left, c); auto r = clone_expr(b->right, c);
		if (!c.ok) return nullptr;
		auto n = c.make<binary_expr>(b->location, l, b->op, r);
		for (const auto& g : c.container_guards) { if (g.first == b) { c.guarded_reads.emplace_back(n.get(), g.second); } }
		n->transient_element_read = b->transient_element_read;
		n->static_operand_class = b->static_operand_class;
		if (c.stamps && n->static_operand_class == operand_class_unknown && b->op.type != token_type::left_bracket && !short_circuit_op(b->op.type)) { n->static_operand_class = kind_class(stamp_kind(b->left.get(), c), stamp_kind(b->right.get(), c)); }
		return n;
	}
	case node_type::unary_expr: {
		auto* u = static_cast<const unary_expr*>(e.get());
		if (incdec_op(u->op.type) && u->operand) { note_store(u->operand.get(), c, true); }   // an identifier's increment keeps its kind (any other target desugars to a compound store)
		auto o = clone_expr(u->operand, c);
		if (!c.ok) return nullptr;
		if (incdec_op(u->op.type) && o && o->get_type() != node_type::identifier_expr) {
			// a bare field's increment reads through the receiver: the member's `+= 1` / `-= 1`, as the parser writes `++r.f` (parser::desugar_incdec)
			const bool increment = u->op.type == token_type::plus_plus;
			token compound(increment ? token_type::plus_equal : token_type::minus_equal, increment ? "+=" : "-=", u->op.location);
			auto n = c.make<assignment_expr>(u->location, o, compound, c.make<literal_expr>(u->op.location, script_value(script_value::ast_literal_tag{}, static_cast<script_int>(1))));
			n->postfix_old_result = u->is_postfix;
			n->incdec_desugar = true;
			return n;
		}
		return c.make<unary_expr>(u->location, u->op, o, u->is_postfix);
	}
	case node_type::assignment_expr: {
		auto* a = static_cast<const assignment_expr*>(e.get());
		const node_type tk = a->target->get_type();
		if (tk != node_type::identifier_expr && tk != node_type::member_expr && tk != node_type::binary_expr) { c.fail("a store target the graft does not take"); return nullptr; }
		bool lands = false;   // a store through a reference parameter lands the parameter's declared kind
		if (a->target->get_type() == node_type::identifier_expr) {
			auto* t = static_cast<const identifier_expr*>(a->target.get());
			if (t->slot_index != SIZE_MAX && t->slot_index < c.slot_types.size() && scalar_type(c.slot_types[t->slot_index])) {
				const script_value_type want = c.slot_types[t->slot_index]->base_type;
				const token_type base = a->op.type == token_type::equal ? token_type::plus : jai::detail::compound_base(a->op.type);
				const script_value_type got = a->op.type == token_type::equal ? value_kind(a->value.get(), c) : arith_kind(base, want, value_kind(a->value.get(), c));
				// a store through the reference runs the variable's own table, so a number into a number slot
				// converts exactly as the renamed store does
				const auto numeric = [](script_value_type k) { return k == script_value_type::jai_int_type || k == script_value_type::jai_float_type; };
				lands = got == want || (numeric(got) && numeric(want));
			}
		}
		note_store(a->target.get(), c, lands);
		auto t = clone_expr(a->target, c); auto v = clone_expr(a->value, c);
		if (!c.ok) return nullptr;
		auto n = c.make<assignment_expr>(a->location, t, a->op, v);
		n->static_store_proof = a->static_store_proof; n->static_operand_class = a->static_operand_class;
		n->postfix_old_result = a->postfix_old_result; n->incdec_desugar = a->incdec_desugar;
		if (c.stamps && a->target->get_type() == node_type::identifier_expr) {
			const script_value_type tk = stamp_kind(a->target.get(), c), vk = stamp_kind(a->value.get(), c);
			if (a->op.type != token_type::equal && n->static_operand_class == operand_class_unknown) { n->static_operand_class = kind_class(tk, vk); }
			const size_t ts = static_cast<const identifier_expr*>(a->target.get())->slot_index;
			if (a->op.type == token_type::equal && tk != script_value_type::jai_null_type && tk == vk && n->static_store_proof == proof_none && ts < c.slot_types.size() && scalar_type(c.slot_types[ts])) {
				n->static_store_proof = proof_same_type;   // a declared scalar slot storing its own kind
			}
		}
		return n;
	}
	case node_type::member_expr: {
		auto* m = static_cast<const member_expr*>(e.get());
		auto o = clone_expr(m->object, c);
		if (!c.ok) return nullptr;
		auto n = c.make<member_expr>(m->location, o, m->member, m->member_id, m->is_arrow, m->is_static);
		n->on_caught = m->on_caught;
		n->null_safe = m->null_safe; n->getter_id = m->getter_id; n->enum_constant = m->enum_constant; n->call_result_discarded = m->call_result_discarded;
		return n;
	}
	case node_type::ternary_expr: {
		auto* t = static_cast<const ternary_expr*>(e.get());
		auto cond = clone_expr(t->condition, c); auto a = clone_expr(t->then_expression, c); auto b = clone_expr(t->else_expression, c);
		if (!c.ok) return nullptr;
		return c.make<ternary_expr>(t->location, cond, a, b);
	}
	case node_type::cast_expr: {
		auto* k = static_cast<const cast_expr*>(e.get());
		auto o = clone_expr(k->operand, c);
		if (!c.ok) return nullptr;
		auto made = c.make<cast_expr>(k->location, k->target, k->by_reference, o);
		made->instance_test = k->instance_test;
		return made;
	}
	case node_type::call_expr: {
		auto* ce = static_cast<const call_expr*>(e.get());
		if (c.replace && ce == c.replace) { ++c.nodes; return c.result ? copy_identifier(c.result.get(), e->location, c) : nullptr; }   // the statement: the call is its temp (null: a void call alone)
		if (!c.replace && !math_call(ce) && !size_call(ce) && !reading_builtin_call(ce, c)) {
			if (!alias_call(ce, c.lc.eng) && !nested_call(ce, &c.lc)) { c.calls = true; }   // any other call stays a call, made from the grafted body
			// the alias makes the real call when its name binds anything else, a nested leaf when its guard fails, a kept call always: that
			// callee may write anything, so nothing the statement reads before the graft stays in place behind it
			c.writes_global = c.writes_member = c.writes_ref = true;
		}   // the statement's clone keeps its calls (the plan ordered them)
		std::vector<expression_ptr> args;
		for (const auto& a : ce->arguments) { args.push_back(clone_expr(a, c)); }
		expression_ptr callee;
		if (c.method_graft() && c.siblings && ce->callee && ce->callee->get_type() == node_type::identifier_expr && ever_stamped(ce->proven_method_decl)) {
			// a sibling method called by bare name resolves on the method's own receiver: the clone calls it on the graft's receiver
			auto* id = static_cast<const identifier_expr*>(ce->callee.get());
			if (id->slot_index != SIZE_MAX || id->global_qualified || id->symbol_id == UINT64_MAX) { c.fail("a bare name in a method the graft cannot resolve"); return nullptr; }
			if (std::find(c.sibling_ids.begin(), c.sibling_ids.end(), id->symbol_id) == c.sibling_ids.end()) { c.sibling_ids.push_back(id->symbol_id); }
			c.calls = true;
			c.writes_global = c.writes_member = c.writes_ref = true;
			expression_ptr object = c.receiver ? expression_ptr(copy_identifier(c.receiver.get(), id->location, c)) : expression_ptr(c.make<this_expr>(id->location));
			callee = c.make<member_expr>(id->location, object, id->name, id->symbol_id, false, false);
		} else {
			callee = clone_expr(ce->callee, c);
		}
		if (!c.ok) return nullptr;
		auto n = c.make<call_expr>(ce->location, callee, std::move(args));
		n->proven_method_decl = ce->proven_method_decl;
		n->static_arg_proofs = ce->static_arg_proofs; n->proof_globals = ce->proof_globals; n->proven_callee_decl = ce->proven_callee_decl;
		n->builtin_receiver = ce->builtin_receiver; n->builtin_receiver_open = ce->builtin_receiver_open;
		c.origins[n.get()] = ce;
		for (auto* pins : { c.alias_pins, c.callee_probes }) {
			if (!pins) continue;
			if (auto pin = pins->find(ce); pin != pins->end()) { const auto v = pin->second; pins->erase(pin); (*pins)[n.get()] = v; }
		}
		if (c.staged_arguments) {
			if (auto staged = c.staged_arguments->find(ce); staged != c.staged_arguments->end()) {
				auto slots = std::move(staged->second);
				c.staged_arguments->erase(staged);
				(*c.staged_arguments)[n.get()] = std::move(slots);
			}
		}
		// a replaced argument is the hoisted leaf's result temp, a slot of its return type: the outer call's proof for it is the types' agreement
		if (auto callee_decl = c.replace && c.result && c.result_type ? ce->proven_callee_decl.lock() : nullptr) {
			for (size_t i = 0; i < ce->arguments.size() && i < callee_decl->parameters.size(); ++i) {
				if (ce->arguments[i].get() != c.replace) continue;
				const parameter& p = callee_decl->parameters[i];
				if (p.is_reference || !p.type || !scalar_type(p.type.get()) || p.type->base_type != c.result_type->base_type) continue;
				if (n->static_arg_proofs.size() < ce->arguments.size()) { n->static_arg_proofs.resize(ce->arguments.size(), proof_none); }
				n->static_arg_proofs[i] = proof_same_type;
			}
		}
		return n;
	}
	default:
		c.fail("an expression the graft does not take"); return nullptr;
	}
}

// control never falls out of the statement (every path ends in a return)
bool terminates(const statement* s) {
	if (!s) return false;
	switch (s->get_type()) {
	case node_type::return_stmt: return true;
	case node_type::block_stmt:
		for (const auto& d : static_cast<const block_stmt*>(s)->declarations) { if (d->get_type() == node_type::statement_decl && terminates(static_cast<const statement_decl*>(d.get())->statement.get())) return true; }
		return false;
	case node_type::if_stmt: { auto* i = static_cast<const if_stmt*>(s); return i->else_statement && terminates(i->then_statement.get()) && terminates(i->else_statement.get()); }
	default: return false;
	}
}

std::vector<declaration_ptr> clone_body(const std::vector<declaration_ptr>& decls, inline_context& c, bool final_position);

// the statements a body may hold: a block, an if, an expression statement and a return. A return declares the result temp from its
// value (the checker's proof, or the return site's conversion) and jumps to the graft's join, unless it is the body's final
// statement, where the join follows anyway
statement_ptr clone_stmt(const statement_ptr& s, inline_context& c, bool final_position) {
	if (!c.ok || !s) return nullptr;
	switch (s->get_type()) {
	case node_type::block_stmt: {
		auto* b = static_cast<const block_stmt*>(s.get());
		std::vector<declaration_ptr> ds = clone_body(b->declarations, c, final_position);
		if (!c.ok) return nullptr;
		return c.make<block_stmt>(b->location, std::move(ds));
	}
	case node_type::if_stmt: {
		auto* i = static_cast<const if_stmt*>(s.get());
		auto cond = clone_expr(i->condition, c);
		statement_ptr t = clone_stmt(i->then_statement, c, final_position);
		statement_ptr el = i->else_statement ? clone_stmt(i->else_statement, c, final_position) : nullptr;
		if (!c.ok) return nullptr;
		return c.make<if_stmt>(i->location, cond, t, el);
	}
	case node_type::return_stmt: {
		auto* r = static_cast<const return_stmt*>(s.get());
		if (c.loop_depth > 0) { c.fail("a return inside a loop"); return nullptr; }
		std::vector<declaration_ptr> ds;
		if (!c.result) {
			if (r->value) { c.fail("a value returned from a void callee"); return nullptr; }
		} else {
			if (!r->value || r->binds_reference) { c.fail("a return without a value or by reference"); return nullptr; }
			const bool proven_return = r->static_return_proof == proof_same_type || c.returns_proven;
			if (c.in_loop && !proven_return && !scalar_type(c.result_type.get()) && !any_type(c.result_type.get())) { c.fail("an unproven return in a loop"); return nullptr; }   // the result's declaration must be a block instruction: proven, a scalar the block lands by its declared type (converting, or missing onto the return's own text), or untyped (it holds what the return yields, as the call's result does)
			expression_ptr v = clone_expr(r->value, c);
			if (!c.ok) return nullptr;
			if (c.in_loop && !landable(v.get(), &c.lc)) { c.fail("a return the block cannot land"); return nullptr; }
			auto rd = c.make<variable_decl>(r->location, c.result_type, c.result->name, c.result->symbol_id, v);   // `T __h = e;`: this path's definition of the result
			rd->slot_index = c.result->slot_index;
			if (!proven_return) { rd->site_kind = variable_decl::site_return; rd->site_owner = c.callee_name; }   // the callee's return converted here, with the return's text
			ds.push_back(rd);
		}
		if (!final_position) {
			auto ex = c.make<return_stmt>(r->location, nullptr);
			ex->inline_exit = true;
			ds.push_back(c.make<statement_decl>(r->location, ex));
		}
		return c.make<block_stmt>(r->location, std::move(ds));
	}
	case node_type::expression_stmt: {
		auto* es = static_cast<const expression_stmt*>(s.get());
		auto e = clone_expr(es->expression, c);
		if (!c.ok) return nullptr;
		if (c.in_loop && !landable_stmt(e.get(), &c.lc)) { c.fail("a statement the block cannot land"); return nullptr; }
		std::vector<declaration_ptr> ds; ds.push_back(c.make<expression_decl>(es->location, e, false));
		return c.make<block_stmt>(es->location, std::move(ds));
	}
	case node_type::for_stmt: {
		auto* fs = static_cast<const for_stmt*>(s.get());
		declaration_ptr init;
		if (fs->initializer) {
			std::vector<declaration_ptr> one = clone_body({fs->initializer}, c, false);
			if (!c.ok) return nullptr;
			if (one.size() != 1) { c.fail("a loop the graft does not take"); return nullptr; }
			init = one[0];
		}
		if (c.in_loop && fs->condition && !landable(fs->condition.get(), &c.lc)) { c.fail("a loop the block cannot land"); return nullptr; }
		if (c.in_loop && fs->update && !landable_stmt(fs->update.get(), &c.lc)) { c.fail("a loop the block cannot land"); return nullptr; }
		auto cond = fs->condition ? clone_expr(fs->condition, c) : nullptr;
		auto upd = fs->update ? clone_expr(fs->update, c) : nullptr;
		++c.loop_depth;
		statement_ptr body = clone_stmt(fs->body, c, false);
		--c.loop_depth;
		if (!c.ok) return nullptr;
		auto loop = c.make<for_stmt>(fs->location, init, cond, upd, body);
		loop->fresh_binding = fs->fresh_binding;
		return loop;
	}
	case node_type::while_stmt: {
		auto* ws = static_cast<const while_stmt*>(s.get());
		if (c.in_loop && !landable(ws->condition.get(), &c.lc)) { c.fail("a loop the block cannot land"); return nullptr; }
		auto cond = clone_expr(ws->condition, c);
		++c.loop_depth;
		statement_ptr body = clone_stmt(ws->body, c, false);
		--c.loop_depth;
		if (!c.ok) return nullptr;
		return c.make<while_stmt>(ws->location, cond, body);
	}
	case node_type::break_stmt:
	case node_type::continue_stmt:
		if (c.loop_depth == 0) { c.fail("a statement the graft does not take"); return nullptr; }
		return s;   // no state: shared as it is
	default:
		c.fail("a statement the graft does not take"); return nullptr;
	}
}

std::vector<declaration_ptr> clone_body(const std::vector<declaration_ptr>& decls, inline_context& c, bool final_position) {
	std::vector<declaration_ptr> out;
	for (size_t k = 0; k < decls.size() && c.ok; ++k) {
		const declaration_ptr& d = decls[k];
		const bool last = final_position && k + 1 == decls.size();
		switch (d->get_type()) {
		case node_type::variable_decl: {
			auto* v = static_cast<const variable_decl*>(d.get());
			if (v->slot_index == SIZE_MAX || v->is_static || v->ref_escaping || v->function_spelling) { c.fail("a declaration the graft does not take"); return out; }
			if (v->type && v->type->is_reference()) { c.fail("a reference declaration"); return out; }
			if (c.in_loop && v->initializer && !landable(v->initializer.get(), &c.lc)) { c.fail("a declaration the block cannot land"); return out; }   // a declaration the block cannot land: the call stays
			expression_ptr init = v->initializer ? clone_expr(v->initializer, c) : nullptr;
			if (!c.ok) return out;
			if (v->slot_index < c.slot_types.size()) { c.slot_types[v->slot_index] = v->type.get(); }
			auto n = c.make<variable_decl>(v->location, v->type, v->name, v->name_id, init);
			n->slot_index = v->slot_index + c.slot_offset;
			n->decl_fast_flags = v->decl_fast_flags;
			n->alias_home = v->alias_home;
			n->alias_index = v->alias_index;
			c.declared_names.push_back(v->name_id);
			out.push_back(n);
			break;
		}
		case node_type::expression_decl: {
			auto* ed = static_cast<const expression_decl*>(d.get());
			if (ed->implicit_return) { c.fail("an implicit return"); return out; }
			if (c.in_loop && !landable_stmt(ed->expression.get(), &c.lc)) { c.fail("a statement the block cannot land"); return out; }
			auto e = clone_expr(ed->expression, c);
			if (!c.ok) return out;
			out.push_back(c.make<expression_decl>(ed->location, e, false));
			break;
		}
		case node_type::statement_decl: {
			const statement_ptr& s = static_cast<const statement_decl*>(d.get())->statement;
			if (!s) break;
			statement_ptr n = clone_stmt(s, c, last);
			if (!c.ok) return out;
			out.push_back(c.make<statement_decl>(s->location, n));
			break;
		}
		default:
			c.fail("a declaration the graft does not take"); return out;
		}
	}
	return out;
}

// the calls of an expression in the order they run: a callee's calls, then the arguments' in order, then the call itself; a math
// intrinsic is no call. A node the graft's clone cannot hold ends the scan.
void call_events(const expression_ptr& e, std::vector<std::shared_ptr<call_expr>>& out, bool& unsupported) {
	if (!e || unsupported) return;
	if (detail::const_site_of(e.get())) { unsupported = true; return; }   // a const guard runs where the node is reached
	switch (e->get_type()) {
	case node_type::literal_expr: case node_type::identifier_expr: case node_type::this_expr: return;
	case node_type::binary_expr: { auto* b = static_cast<const binary_expr*>(e.get()); call_events(b->left, out, unsupported); call_events(b->right, out, unsupported); return; }
	case node_type::unary_expr: call_events(static_cast<const unary_expr*>(e.get())->operand, out, unsupported); return;
	case node_type::member_expr: { auto* m = static_cast<const member_expr*>(e.get()); if (!m->is_static) { call_events(m->object, out, unsupported); } return; }
	case node_type::ternary_expr: { auto* t = static_cast<const ternary_expr*>(e.get()); call_events(t->condition, out, unsupported); call_events(t->then_expression, out, unsupported); call_events(t->else_expression, out, unsupported); return; }
	case node_type::cast_expr: call_events(static_cast<const cast_expr*>(e.get())->operand, out, unsupported); return;
	case node_type::assignment_expr: { auto* a = static_cast<const assignment_expr*>(e.get()); call_events(a->value, out, unsupported); call_events(a->target, out, unsupported); return; }
	case node_type::call_expr: {
		auto ce = std::static_pointer_cast<call_expr>(e);
		if (ce->callee && ce->callee->get_type() == node_type::member_expr) { auto* m = static_cast<const member_expr*>(ce->callee.get()); if (!m->is_static) { call_events(m->object, out, unsupported); } }
		else { call_events(ce->callee, out, unsupported); }
		for (const auto& a : ce->arguments) { call_events(a, out, unsupported); }
		if (!math_call(ce.get())) { out.push_back(ce); }
		return;
	}
	default: unsupported = true; return;
	}
}

// the ancestors of a node, the root first, the node last
bool find_path(const expression_ptr& root, const expression* target, std::vector<expression_ptr>& path) {
	if (!root) return false;
	path.push_back(root);
	if (root.get() == target) return true;
	auto down = [&](const expression_ptr& x) { return find_path(x, target, path); };
	bool found = false;
	switch (root->get_type()) {
	case node_type::binary_expr: { auto* b = static_cast<const binary_expr*>(root.get()); found = down(b->left) || down(b->right); break; }
	case node_type::unary_expr: found = down(static_cast<const unary_expr*>(root.get())->operand); break;
	case node_type::member_expr: found = down(static_cast<const member_expr*>(root.get())->object); break;
	case node_type::ternary_expr: { auto* t = static_cast<const ternary_expr*>(root.get()); found = down(t->condition) || down(t->then_expression) || down(t->else_expression); break; }
	case node_type::cast_expr: found = down(static_cast<const cast_expr*>(root.get())->operand); break;
	case node_type::assignment_expr: { auto* a = static_cast<const assignment_expr*>(root.get()); found = down(a->value) || down(a->target); break; }
	case node_type::call_expr: { auto* ce = static_cast<const call_expr*>(root.get()); found = down(ce->callee); for (size_t i = 0; !found && i < ce->arguments.size(); ++i) { found = down(ce->arguments[i]); } break; }
	default: break;
	}
	if (!found) path.pop_back();
	return found;
}

// a stamp the checker made, live or not (an empty weak_ptr shares no owner with the empty one)
bool ever_stamped(const std::weak_ptr<const function_decl>& w) {
	return w.owner_before(std::weak_ptr<const function_decl>()) || std::weak_ptr<const function_decl>().owner_before(w);
}

// the type a spilled operand's temp takes, from the checker's operand class: a comparison or a logical operator yields bool, an
// element read its proven kind, arithmetic the class's result; jai_null_type when the subtree is no spill
script_value_type spill_kind(const expression* e) {
	switch (e->get_type()) {
	case node_type::binary_expr: {
		auto* b = static_cast<const binary_expr*>(e);
		const uint8_t cls = b->static_operand_class;
		if (logical_op(b->op.type)) return script_value_type::jai_bool_type;
		if (cls == operand_class_unknown) return script_value_type::jai_null_type;
		if (b->returns_bool()) return script_value_type::jai_bool_type;
		if (b->op.type == token_type::left_bracket) {
			return cls == operand_class_int_int ? script_value_type::jai_int_type : cls == operand_class_float_float ? script_value_type::jai_float_type : script_value_type::jai_null_type;
		}
		switch (b->op.type) {
		case token_type::plus: case token_type::minus: case token_type::star: case token_type::slash: case token_type::percent:
			if (cls == operand_class_int_int) return script_value_type::jai_int_type;
			if (cls == operand_class_float_float || cls == operand_class_int_float || cls == operand_class_float_int) return script_value_type::jai_float_type;
			if (cls == operand_class_string_string && b->op.type == token_type::plus) return script_value_type::jai_string_type;
			return script_value_type::jai_null_type;
		case token_type::ampersand: case token_type::pipe: case token_type::caret: case token_type::left_shift: case token_type::right_shift:
			return cls == operand_class_int_int ? script_value_type::jai_int_type : script_value_type::jai_null_type;
		default: return script_value_type::jai_null_type;
		}
	}
	case node_type::unary_expr: return static_cast<const unary_expr*>(e)->op.type == token_type::bang ? script_value_type::jai_bool_type : script_value_type::jai_null_type;
	default: return script_value_type::jai_null_type;
	}
}

} // namespace

// the callee's shape and the site's arguments (the statement's position is the caller's business): one small body of the forms the
// clone takes, every by-value parameter a scalar the checker proved or a `var` taking a literal or a value of a scalar kind, every
// reference parameter a `var&` bound to a plain identifier or an element lvalue, an untyped result one scalar kind at the site. frame:
// the declared types of the slots the arguments are read from, when the call is a body's (null: the caller's frame)
const char* vm_compiler::inline_eligible(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame) const {
	if (!ir_passes_ || !typed_lowering_ || !callable_.active) return "the middle end is off";
	if (!fn || !fn->body) return "a callee without a body";
	if (call->const_check) return "a call a const rule guards";
	if (fn->is_coroutine || fn->is_static) return "a coroutine or a static callee";
	if (callable_.coroutine_body && body_has_loop(fn->body.get())) return "a loop the coroutine's frame would run without its block";
	if (fn->body.get() == callable_body_ || std::find(graft_bodies_.begin(), graft_bodies_.end(), fn->body.get()) != graft_bodies_.end()) return "a recursive call";
	if (call->arguments.size() != fn->parameters.size()) return "an arity the callee does not declare";
	const type_info* rt = fn->return_type.get();
	if (!(void_type(rt) || scalar_type(rt) || (!rt && fn->graft_return_kind != 0) || (any_type(rt) && untyped_result_kind(call, fn, frame) != script_value_type::jai_null_type))) return "a result the graft does not declare";   // a string result declares through the general DECL_VAR, which opens the enclosing block
	if (!void_type(rt) && !terminates(fn->body.get())) return "a body that falls out without a return";   // the result is defined on every path or the callee is not a leaf the graft takes
	if (detail::requires_return_value(rt) && !detail::has_complete_return(*fn)) return "a callee with an incomplete required return";
	std::vector<uint8_t> captures;   // the arguments a later argument may write (detail/arg_capture.hpp): a value argument or an element among them binds through the call
	detail::transient_read_marker::stamp_arg_captures(call->arguments, captures);
	for (size_t i = 0; i < fn->parameters.size(); ++i) {
		const parameter& p = fn->parameters[i];
		const expression* a = call->arguments[i].get();
		if (p.slot_index == SIZE_MAX || p.slot_index >= fn->local_count || p.default_value) return "a parameter with a default or without a slot";
		if (p.is_reference) {
			// an element is aliased where its bind cannot fail - the call binds after every argument and refuses a non-lvalue there (a
			// string's character, a missing map key), which the alias would refuse in the argument's place
			const bool typed_to_name = a->get_type() == node_type::identifier_expr;   // a typed one bound to a caller's local is that local: the declared type is the checker's (no bind or store checks it)
			if (!(!p.type || any_type(p.type.get()) || var_reference(p.type.get()) || typed_to_name) || p.ref_escaping) return "a reference parameter that is not a var&";   // `var& p` (the parser keeps the referent's type and the reference flag apart)
			if (a->get_type() == node_type::identifier_expr) {
				const auto* identifier = static_cast<const identifier_expr*>(a);
				if (identifier->global_qualified) return "a reference argument spelled ::name";
				if (identifier->slot_index == SIZE_MAX) return "a reference argument that is not a local";   // a global, a field or a method name binds through the caller's rules
				if (p.ref_store_unproven) {   // a float store a variable that is not a float may refuse: the refusal names the parameter
					const type_info* held = nullptr;
					if (frame && identifier->slot_index < frame->size()) { held = (*frame)[identifier->slot_index]; }
					else if (auto found = callable_.slot_types.find(static_cast<uint32_t>(identifier->slot_index)); found != callable_.slot_types.end()) { held = found->second; }
					if (held && held->is_reference()) { held = held->element_type().get(); }
					if (!held || held->base_type != script_value_type::jai_float_type) return "a reference parameter whose float store its variable may refuse";
				}
				if (p.type && !any_type(p.type.get()) && p.type->type_name != "auto") {
					// the alias names the caller's variable in the parameter's place, skipping the bind's admission and the parameter's
					// declared type at the stores: only a variable statically of that type (a local declared it, a
					// `T& alias`) grafts, any other binds through the call
					const type_info* actual = nullptr;
					if (frame && identifier->slot_index < frame->size()) { actual = (*frame)[identifier->slot_index]; }
					else if (auto found = callable_.slot_types.find(static_cast<uint32_t>(identifier->slot_index)); found != callable_.slot_types.end()) { actual = found->second; }
					if (actual && actual->is_reference()) { actual = actual->element_type().get(); }
					if (!actual && !identifier->names_value_decl) { actual = identifier->declared_ref; }
					if (!actual) return "a typed reference argument of an unknown declared type";
					if (!actual->equals(*p.type)) return "a typed reference argument of another declared type";
				}
				continue;
			}
			if (a->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(a)->op.type == token_type::left_bracket) {
				// an element of a private local declared an array: its typed slot holds an array at every point, so the element is an lvalue
				auto* base = static_cast<const binary_expr*>(a)->left.get();
				if (frame || base->get_type() != node_type::identifier_expr) return "a reference argument that may not be an lvalue";
				auto* id = static_cast<const identifier_expr*>(base);
				if (id->global_qualified || !id->names_value_decl || id->slot_index == SIZE_MAX || callable_.escaping.count(static_cast<uint32_t>(id->slot_index))) return "a reference argument that may not be an lvalue";
				const auto declared = callable_.slot_types.find(static_cast<uint32_t>(id->slot_index));
				if (declared == callable_.slot_types.end() || !declared->second || declared->second->is_reference() || declared->second->base_type != script_value_type::jai_array_type) return "a reference argument that may not be an lvalue";
				if (i < captures.size() && captures[i] != 0) return "an element a later argument may move";   // the call binds it where it is evaluated
				if (p.ref_store_unproven) return "a reference parameter whose float store its element may refuse";
				continue;
			}
			return "a reference argument that is not a name or an element";
		}
		if (p.ref_escaping) return "a parameter that escapes";
		const bool proven = i < call->static_arg_proofs.size() && call->static_arg_proofs[i] == proof_same_type;
		if (scalar_type(p.type.get())) { if (!proven) return "an unproven argument"; continue; }   // an unproven argument would bind out of the call's order (every argument before the first binding)
		if (any_type(p.type.get())) {
			if (a->get_type() == node_type::literal_expr) continue;
			if (arg_kind(a, frame) != script_value_type::jai_null_type) continue;   // a scalar value: binding it copies it, before or after the next argument alike
			return "a var parameter bound to more than a scalar value or a literal";
		}
		if (value_param(p)) {
			// its argument lands in a temp in the call's order and the declaration binds it after every argument by the parameter's own
			// table and text (a conversion, a copy, a nocopy refusal); a handle's sharing rules are the bind's, so a handle takes a proof
			const bool handle = p.type->base_type == script_value_type::jai_shared_ptr_type || p.type->base_type == script_value_type::jai_weak_ptr_type;
			if (handle && !proven) return "an unproven argument";
			if (i < captures.size() && captures[i] != 0) return "a value argument a later argument may write";
			continue;
		}
		return "a parameter type the graft does not take";
	}
	return nullptr;
}

// the first call the statement runs is a leaf the graft takes, in an unconditional position, with every operand evaluated before
// it a literal, a private local the body leaves alone, a name any write may reach taken into a temp, or a stamped subtree that
// spills into a typed temp. Any other first call ends the statement's hoisting: a call after it would run behind an effect.
// A callee named by a call whose arguments hold the first call resolves before that call runs (the callee before the arguments):
// the outermost such leaf grafts instead, its guard in the callee's place and the first call inside its parameters; a stdlib alias
// above it is resolved in its place by the alias's guard; a callee no guard pins keeps the call.
bool vm_compiler::plan_hoist(const expression_ptr& root, bool statement_position, hoist_plan& out) {
	land_ctx plain; plain.eng = engine_;
	std::vector<std::shared_ptr<call_expr>> events; bool unsupported = false;
	call_events(root, events, unsupported);
	if (events.empty()) return false;
	std::shared_ptr<call_expr> call = events.front();
	out.call = call;   // the call the plan is about, named with the rule that keeps it when the plan fails
	if (unsupported) { out.why = "a statement the graft does not take"; return false; }
	out.plain_numeric = false;
	out.receiver_globals.clear();
	std::vector<const call_expr*> alias_pins, probe_pins;
	bool args_call = false;   // the grafted call's arguments hold calls: they run inside its parameters, after its guard
	{
		std::vector<expression_ptr> cpath;
		if (!find_path(root, call.get(), cpath)) { out.why = "a position the graft does not take"; return false; }
		for (size_t k = 0; k + 1 < cpath.size(); ++k) {
			if (cpath[k]->get_type() != node_type::call_expr) continue;
			auto ce = std::static_pointer_cast<call_expr>(cpath[k]);
			if (!ce->callee || cpath[k + 1].get() == ce->callee.get() || ce->callee->get_type() != node_type::identifier_expr) continue;   // a method or a computed callee: the position rules below
			if (private_local(ce->callee.get()) || alias_pins_.count(ce.get()) || callee_probes_.count(ce.get())) continue;   // no user code rebinds it, or an earlier round resolved it
			if (alias_call(ce.get(), engine_) && math_temp_operand(ce.get()) != detail::math_fn::none) { alias_pins.push_back(ce.get()); continue; }
			std::shared_ptr<const function_decl> efn = ever_stamped(ce->proven_callee_decl) ? ce->proven_callee_decl.lock() : nullptr;
			if (efn && inline_eligible(ce.get(), efn.get()) == nullptr) { call = ce; out.call = call; args_call = true; break; }
			const std::string_view nm = static_cast<const identifier_expr*>(ce->callee.get())->name;
			if (nm == "weak_from_this" || nm == "shared_from_this" || nm == "__jai_parallel_advance" || nm == "__jai_parallel_element" || nm == "__jai_parallel_counted") { out.why = "a call in the arguments of a call no probe pins"; return false; }
			probe_pins.push_back(ce.get());   // any other callee: its probe runs in its place and the call takes the parked callee
		}
	}
	if (callee_probes_.count(call.get())) { out.why = "a call whose callee an earlier round probed"; return false; }
	std::shared_ptr<const function_decl> fn;
	out.receiver = nullptr;
	out.receiver_field = nullptr;
	out.receiver_this = false;
	if (call->callee && ever_stamped(call->proven_method_decl) &&
	    (call->callee->get_type() == node_type::member_expr || call->callee->get_type() == node_type::identifier_expr)) {
		// the receiver: a local; or, in a method body, the frame's `this` (`this.m()`, a sibling's bare `m()`) or a bare field holding a
		// handle, read where it lives
		const expression* object = call->callee->get_type() == node_type::member_expr ? static_cast<const member_expr*>(call->callee.get())->object.get() : nullptr;
		if (object && object->get_type() == node_type::identifier_expr && static_cast<const identifier_expr*>(object)->slot_index != SIZE_MAX) {
			const uint32_t rs = identifier_slot_operand(static_cast<identifier_expr*>(const_cast<expression*>(object)));
			if (rs == k_invalid_u32 || rs >= k_typed_slot_mask) { out.why = "a receiver that is not a local"; return false; }
			out.receiver = std::static_pointer_cast<identifier_expr>(static_cast<const member_expr*>(call->callee.get())->object);
		} else if (!method_frame()) {
			out.why = "a receiver of a frame that is not a method body"; return false;
		} else if (!object || object->get_type() == node_type::this_expr) {
			out.receiver_this = true;
		} else if (object->get_type() == node_type::identifier_expr && handle_field_of_member_class(static_cast<const identifier_expr*>(object))) {
			out.receiver_field = std::static_pointer_cast<identifier_expr>(static_cast<const member_expr*>(call->callee.get())->object);
			out.receiver = out.receiver_field;
		} else {
			out.why = "a receiver that is not a local, this or a handle field"; return false;
		}
		fn = call->proven_method_decl.lock();
		if (!fn) { out.why = "a callee whose declaration is gone"; return false; }
	} else {
		if (!call->callee || call->callee->get_type() != node_type::identifier_expr) { out.why = "a callee that is not a name"; return false; }
		if (!ever_stamped(call->proven_callee_decl)) { out.why = "an unresolved callee"; return false; }
		fn = call->proven_callee_decl.lock();
		if (!fn) { out.why = "a callee whose declaration is gone"; return false; }
	}
	if (const char* why = inline_eligible(call.get(), fn.get())) { out.why = why; return false; }
	if (void_type(fn->return_type.get()) && !(statement_position && root.get() == call.get())) { out.why = "a void result used as a value"; return false; }   // a void result is a value nowhere but alone in a statement
	// the body clones, and its writes decide what may stand before the call
	const bool in_loop = in_closable_loop();
	const bool outer_in_loop = graft_in_loop_;
	graft_in_loop_ = in_loop;
	struct restore_in_loop { bool& flag; bool value; ~restore_in_loop() { flag = value; } } restore{graft_in_loop_, outer_in_loop};
	if (in_loop) {   // inside a loop every parameter's argument lands by a block instruction, or the call stays (its landing converts what a declaration cannot)
		const land_ctx args = args_call ? graft_land_ctx(fn.get()) : plain;   // a leaf in an argument grafts inside the parameter's declaration
		for (size_t i = 0; i < fn->parameters.size(); ++i) { if (!fn->parameters[i].is_reference && !landable(call->arguments[i].get(), &args)) { out.why = "an argument the block cannot land"; return false; } }
	}
	inline_context probe; probe.lc = graft_land_ctx(fn.get()); probe.lc.frame = &probe.slot_types; probe.lc.origins = &probe.origins; probe.renames.assign(fn->local_count, nullptr); probe.result_type = graft_result_type(fn.get()); probe.callee_name = fn->name; probe.in_loop = in_loop;
	probe.returns_proven = fn->graft_return_kind != 0;
	probe.receiver = out.receiver;
	probe.receiver_this = out.receiver_this;
	probe.siblings = method_frame();
	probe.caller_global = [this](const identifier_expr* id) { return this_global_of(id, false); };
	probe.caller_field = [this](const identifier_expr* id) { return this_field_slot_of(id, false) != 0; };
	probe.ref_slots.assign(fn->local_count, 0); probe.ref_stored.assign(fn->local_count, 0); probe.ref_store_unproven.assign(fn->local_count, 0);
	graft_param_types(call.get(), fn.get(), nullptr, probe.slot_types);   // a reference parameter's declared type binds nothing: the variable it names is what it holds
	for (const parameter& p : fn->parameters) { if (p.is_reference && p.slot_index < fn->local_count) { probe.ref_slots[p.slot_index] = 1; } }
	if (!void_type(fn->return_type.get())) { auto r = std::make_shared<identifier_expr>(fn->body->location, std::string_view("\x01inl"), uint64_t(1)); probe.result = r; probe.pins.push_back(r); }
	(void)clone_body(fn->body->declarations, probe, true);
	if (!probe.ok) { out.why = probe.why ? probe.why : "a body the graft does not take"; return false; }
	if (graft_nesting_ > 0 && graft_tree_nodes_ + probe.nodes > k_inline_max_tree) { out.why = "a graft tree too large"; return false; }
	// a reference parameter renamed to the caller's variable turns a store through it into a store of the variable, which enforces the
	// variable's declared type where the reference's store does not: equal when the variable is a reference or untyped, or declared a
	// scalar type with every store proven to land that type
	for (size_t i = 0; i < fn->parameters.size(); ++i) {
		const parameter& p = fn->parameters[i];
		if (!p.is_reference || call->arguments[i]->get_type() != node_type::identifier_expr || p.slot_index >= probe.ref_stored.size() || !probe.ref_stored[p.slot_index]) continue;
		if (!static_cast<const identifier_expr*>(call->arguments[i].get())->names_value_decl) continue;
		const type_info* vt = probe.slot_types[p.slot_index];
		if (vt && vt->base_type == script_value_type::jai_any_type) continue;
		if (scalar_type(vt) && !probe.ref_store_unproven[p.slot_index]) { out.plain_numeric = true; continue; }
		out.why = "a reference parameter whose store the caller's variable would convert"; return false;
	}
	// the operands before the call: the left siblings along the path from the root
	std::vector<expression_ptr> path;
	if (!find_path(root, call.get(), path)) { out.why = "a position the graft does not take"; return false; }
	std::vector<expression_ptr> pre;
	std::vector<const expression*> copied_args;    // an outer call's argument its parameter does not take as a proven scalar
	bool free_identifier = false, in_place = false;
	for (size_t k = 0; k + 1 < path.size(); ++k) {
		const expression_ptr& a = path[k]; const expression* x = path[k + 1].get();
		switch (a->get_type()) {
		case node_type::binary_expr: {
			auto* b = static_cast<const binary_expr*>(a.get());
			if (x == b->right.get()) { if (short_circuit_op(b->op.type)) { out.why = logical_op(b->op.type) ? "an operand behind && or ||" : "an operand behind ??"; return false; } pre.push_back(b->left); }
			break;
		}
		case node_type::ternary_expr: { auto* t = static_cast<const ternary_expr*>(a.get()); if (x != t->condition.get()) { out.why = "a call in a ternary's arm"; return false; } break; }
		case node_type::assignment_expr: {
			auto* s = static_cast<const assignment_expr*>(a.get());
			if (x != s->value.get()) { out.why = "a call in a store's target"; return false; }
			break;   // a store runs its value ahead of every part of its target, compound or plain
		}
		case node_type::call_expr: {
			auto* ce = static_cast<const call_expr*>(a.get());
			if (x == ce->callee.get()) break;
			if (!ce->callee) { out.why = "a callee that is not a name"; return false; }
			// Preserve the ordinary call boundary for non-scalar/reference arguments: stage the callee and its complete
			// earlier argument prefix into owned temps now, using the normal argument-capture rules. The final call keeps the original AST
			// metadata for reference binding and only emits the remaining arguments. No aggregate is rejected or recopied.
			size_t prefix = 0;
			while (prefix < ce->arguments.size() && ce->arguments[prefix].get() != x) { ++prefix; }
			const auto outer_decl = ce->proven_callee_decl.lock();
			bool needs_boundary = false;
			for (size_t ai = 0; ai < prefix; ++ai) {
				const bool scalar_copy = outer_decl && ai < outer_decl->parameters.size() &&
					!outer_decl->parameters[ai].is_reference && scalar_type(outer_decl->parameters[ai].type.get());
				if (!scalar_copy && ce->arguments[ai]->get_type() != node_type::literal_expr) { needs_boundary = true; }
			}
			if (needs_boundary && ce->callee->get_type() == node_type::identifier_expr && !alias_call(ce, engine_)) {
				if (!callee_probes_.count(ce) && std::find(probe_pins.begin(), probe_pins.end(), ce) == probe_pins.end()) { probe_pins.push_back(ce); }
				pre.push_back(a);
				out.argument_prefixes[ce] = prefix;
				break;
			}
			if (ce->callee->get_type() == node_type::member_expr && !math_call(ce)) {
				// a method's receiver and lookup stand before its arguments: a builtin method of a container the checker proved, on a
				// receiver the graft cannot write (a private local, or a certain global when the body writes no global, re-checked by the
				// graft's guard), observes nothing the graft changes, and neither its receiver nor its lookup can fail
				auto* m = static_cast<const member_expr*>(ce->callee.get());
				auto* rid = m->object && m->object->get_type() == node_type::identifier_expr ? static_cast<const identifier_expr*>(m->object.get()) : nullptr;
				bool held = ce->builtin_receiver != 0 && !m->is_static && !m->null_safe && rid && !rid->global_qualified;
				if (held && rid->slot_index != SIZE_MAX) { held = private_local(rid) && !args_call; }
				else if (held) {
					held = ce->receiver_global.symbol == rid->symbol_id && !probe.writes_global;
					if (held) { out.receiver_globals.push_back(ce->receiver_global); }
				}
				if (!held) { out.why = "a method call's argument"; return false; }
			}
			else if (ce->callee->get_type() == node_type::identifier_expr) {   // an alias's guard or a probe resolves its callee here; any other name is a private local
				if (std::find(alias_pins.begin(), alias_pins.end(), ce) != alias_pins.end() || std::find(probe_pins.begin(), probe_pins.end(), ce) != probe_pins.end()) { pre.push_back(a); }
				else if (!alias_pins_.count(ce) && !callee_probes_.count(ce)) { pre.push_back(ce->callee); }
			}
			else { out.why = "an argument of a computed callee"; return false; }
			// a callee other than a math intrinsic or a stdlib alias may bind an argument by reference (whatever the name binds by then):
			// it binds the argument itself after every argument, so a spilled copy in an earlier argument's place would bind a temp (the
			// graft's own result stands as a value there, which a reference parameter refuses as the original's value would be refused)
			const bool by_value = math_call(ce) || std::find(alias_pins.begin(), alias_pins.end(), ce) != alias_pins.end() || alias_pins_.count(ce) || alias_call(ce, engine_) ||
			                      (ce->callee->get_type() == node_type::member_expr && ce->builtin_receiver != 0);   // a builtin reads its arguments as values
			const auto outer = math_call(ce) ? std::shared_ptr<const function_decl>() : ce->proven_callee_decl.lock();
			for (size_t ai = 0; ai < ce->arguments.size() && ce->arguments[ai].get() != x; ++ai) {
				const expression_ptr& arg = ce->arguments[ai];
				if (!by_value && arg->get_type() != node_type::literal_expr && !(private_local(arg.get()) && !args_call)) { out.why = "an argument before the call a reference parameter may bind"; return false; }   // a private local stays in place: bound or read as the call would, the graft cannot write it
				pre.push_back(arg);
				const bool scalar_param = outer && ai < outer->parameters.size() && !outer->parameters[ai].is_reference && scalar_type(outer->parameters[ai].type.get());
				if (!math_call(ce) && !scalar_param) { copied_args.push_back(arg.get()); }
			}
			break;
		}
		case node_type::unary_expr: case node_type::cast_expr: case node_type::member_expr: break;
		default: out.why = "a position the graft does not take"; return false;
		}
	}
	out.spills.clear();
	for (const expression_ptr& p : pre) {
		switch (p->get_type()) {
		case node_type::literal_expr: break;
		case node_type::call_expr: {   // an alias's guard, or a callee's probe; any other call (an intrinsic over a name the call may write) has no spill
			auto* pc = static_cast<const call_expr*>(p.get());
			const bool alias = std::find(alias_pins.begin(), alias_pins.end(), pc) != alias_pins.end();
			if (!alias && !out.argument_prefixes.count(pc) && std::find(probe_pins.begin(), probe_pins.end(), pc) == probe_pins.end()) { out.why = "an operand before the call the block cannot spill"; return false; }
			out.spills.emplace_back(p, alias ? script_value_type::jai_function_type : script_value_type::jai_object_type);
			break;
		}
		case node_type::identifier_expr: {
			free_identifier = true;
			const bool global = static_cast<const identifier_expr*>(p.get())->slot_index == SIZE_MAX;
			if (global || !private_local(p.get()) || args_call) {
				// a global, a closure's by-reference capture or a reference: the call (the graft, or the fallback's callee, whatever the name binds
				// by then) may write it, so its value is taken ahead of the call, as the real call's operand order has it; with calls in the
				// arguments, a private local too (a leaf there may bind it by reference). A temp holds its value only as a scalar: an outer call
				// that binds it by reference or copies it (an array, a map, an instance) binds the name itself
				if (std::find(copied_args.begin(), copied_args.end(), p.get()) != copied_args.end()) { out.why = "a name an outer call binds by reference or copies, ahead of the call"; return false; }
				out.spills.emplace_back(p, script_value_type::jai_any_type);
				break;
			}
			in_place = true;
			break;
		}
		default: {
			const script_value_type kind = spill_kind(p.get());
			if (kind != script_value_type::jai_null_type && landable(p.get(), &plain)) { out.spills.emplace_back(p, kind); break; }   // a spill is a block instruction of a known kind
			if (p->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(p.get())->op.type == token_type::left_bracket && fusable_index_expr(p.get())) {
				out.spills.emplace_back(p, script_value_type::jai_array_type);   // an element read of no proven kind: the fused read into a temp, the value as it stands
				break;
			}
			out.why = "an operand before the call the block cannot spill"; return false;
		}
		}
	}
	// an identifier before the call may be the referent of a reference parameter; without one, a nested leaf's fallback reaches only the callee's
	// own slots and the program scope, never a private local of the caller
	const bool ref_params = std::any_of(fn->parameters.begin(), fn->parameters.end(), [](const parameter& p) { return p.is_reference; });
	if (in_place && probe.writes_ref && ref_params) { out.why = "a name before the call the body may write through its reference"; return false; }
	if ((out.receiver || out.receiver_this) && free_identifier && probe.writes_member) { out.why = "a name before the call a method may write"; return false; }   // an identifier before the call may alias the receiver's instance
	// a field holding the receiver: a call, or a store into a field named like it (the caller's own field, when the receiver is the caller),
	// could move the receiver the clone reads
	if (out.receiver_field && probe.calls) { out.why = "a method of the frame's receiver whose body calls"; return false; }
	// a store through another object, or a bare store under the receiver field's own name, may be the caller's field
	if (out.receiver_field && (probe.writes_member || (probe.writes_global && std::find(probe.field_ids.begin(), probe.field_ids.end(), out.receiver_field->symbol_id) != probe.field_ids.end()))) {
		out.why = "a body that may store into the field holding its receiver"; return false;
	}
	out.fn = fn; out.in_loop = in_loop; out.why = nullptr;
	return true;
}

// what fn's body may call when fn grafts: the stdlib aliases, and a plain leaf the graft's compile grafts in turn, at any depth the tree's node
// budget allows: eligible by itself, never fn or an enclosing graft, its guard and fallback inside fn's graft. A call's verdict is kept for
// the chunk (its body's shape decides it; a recursion the memo misses is refused again where the graft compiles)
// a loop encloses the statement that a block may still stand for: one the first compile closed, or any loop when no first compile spoke
bool vm_compiler::in_closable_loop() const {
	for (const loop_context& lc : loops_) {
		if ((lc.kind == region_kind::loop || lc.kind == region_kind::range_for) && !open_loops_.count(lc.stmt)) return true;
	}
	return false;
}

land_ctx vm_compiler::graft_land_ctx(const function_decl* fn) {
	land_ctx lc;
	lc.eng = engine_;
	{
		lc.nested = [this, fn](const call_expr* ce, const std::vector<const type_info*>* frame) {
			if (!ce->callee || ce->callee->get_type() != node_type::identifier_expr || !ever_stamped(ce->proven_callee_decl)) return false;
			const std::shared_ptr<const function_decl> inner = ce->proven_callee_decl.lock();
			if (!inner || !inner->body || inner->body.get() == fn->body.get()) return false;
			// the verdict's inputs, never a clone's address: the callee, the grafts around it, and each argument's shape, proof and kind
			std::string key;
			auto put = [&key](uint64_t v) { key.append(reinterpret_cast<const char*>(&v), sizeof v); };
			put(reinterpret_cast<uintptr_t>(fn)); put(reinterpret_cast<uintptr_t>(inner.get())); put(graft_in_loop_ ? 1 : 0);
			for (const block_stmt* b : graft_bodies_) { put(reinterpret_cast<uintptr_t>(b)); }
			put(UINT64_MAX);
			for (size_t i = 0; i < ce->arguments.size(); ++i) {
				const expression* a = ce->arguments[i].get();
				uint64_t code = static_cast<uint64_t>(a->get_type());
				if (a->get_type() == node_type::identifier_expr) {
					auto* id = static_cast<const identifier_expr*>(a);
					code |= (id->slot_index == SIZE_MAX ? 1u : 0u) << 8 | (id->global_qualified ? 1u : 0u) << 9 | (id->names_value_decl ? 1u : 0u) << 10;
				} else if (a->get_type() == node_type::binary_expr) {
					code |= static_cast<uint64_t>(static_cast<const binary_expr*>(a)->op.type) << 16;
				}
				code |= static_cast<uint64_t>(arg_kind(a, frame)) << 32;
				code |= static_cast<uint64_t>(i < ce->static_arg_proofs.size() ? ce->static_arg_proofs[i] : 0) << 48;
				put(code);
			}
			if (const auto memo = nested_land_memo_.find(key); memo != nested_land_memo_.end()) return memo->second;
			graft_bodies_.push_back(fn->body.get());
			bool ok = inline_eligible(ce, inner.get(), frame) == nullptr;
			if (ok) {
				// the body the inner graft will clone, held to the block's landing rules: a leaf that stays a call inside the graft would only
				// widen the caller's frame
				inline_context probe; probe.lc = graft_land_ctx(inner.get()); probe.lc.frame = &probe.slot_types; probe.lc.origins = &probe.origins; probe.renames.assign(inner->local_count, nullptr); probe.result_type = graft_result_type(inner.get());
				probe.callee_name = inner->name; probe.in_loop = graft_in_loop_; probe.returns_proven = inner->graft_return_kind != 0;
				probe.ref_slots.assign(inner->local_count, 0);
				graft_param_types(ce, inner.get(), frame, probe.slot_types);
				for (const parameter& p : inner->parameters) { if (p.is_reference && p.slot_index < inner->local_count) { probe.ref_slots[p.slot_index] = 1; } }
				if (!void_type(inner->return_type.get())) { auto r = std::make_shared<identifier_expr>(inner->body->location, std::string_view("\x01inl"), uint64_t(1)); probe.result = r; probe.pins.push_back(r); }
				(void)clone_body(inner->body->declarations, probe, true);
				ok = probe.ok;
			}
			graft_bodies_.pop_back();
			nested_land_memo_[key] = ok;
			return ok;
		};
	}
	return lc;
}

// an argument's kind where the call reads it: a literal's, a local's declared scalar type in the frame (null: the caller's), or what plain
// arithmetic over those yields (a kind a stamp may assume: its consumers guard it)
script_value_type vm_compiler::arg_kind(const expression* a, const std::vector<const type_info*>* frame) const {
	return scalar_kind(kind_of(a, [this, frame](size_t s) {
		const type_info* t = nullptr;
		if (frame) { t = s < frame->size() ? (*frame)[s] : nullptr; }
		else if (const auto it = callable_.slot_types.find(static_cast<uint32_t>(s)); it != callable_.slot_types.end()) { t = it->second; }
		if (scalar_type(t)) return t->base_type;
		return !frame && s < callable_.slot_kinds.size() ? callable_.slot_kinds[s] : script_value_type::jai_null_type;
	}, nullptr, script_value_type::jai_null_type));
}

// the kinds a callable's untyped locals certainly hold: a `var` local every definition of which (its initializer, each store, each
// increment) yields one scalar kind from the kinds of the names it reads, to a fixpoint; a local a call may bind by reference, a reference
// or a capture reaches has none, and a body the walk does not open gives none. A stamp's input (every consumer guards it), never a declaration
void vm_compiler::infer_local_kinds(const std::vector<parameter>& parameters, const block_stmt* body, size_t local_count) {
	callable_.slot_kinds.clear();
	if (!body || !ir_passes_ || !typed_lowering_) return;
	site_walk w;
	w.eng = engine_;
	w.kind.assign(local_count, script_value_type::jai_null_type);
	w.open.assign(local_count, 0);
	for (const parameter& p : parameters) {
		if (p.slot_index < local_count && !p.is_reference && !p.ref_escaping && scalar_type(p.type.get())) { w.kind[p.slot_index] = p.type->base_type; }
	}
	walk_decls(body->declarations, w);
	if (!w.ok) return;
	settle(w);
	for (size_t s = 0; s < local_count; ++s) { if (!w.open[s]) { w.kind[s] = script_value_type::jai_null_type; } }   // a declared slot's kind is its declaration's
	callable_.slot_kinds = std::move(w.kind);
}

// the kinds a callee's slots take at the site: a scalar parameter or local its declared type, an untyped by-value parameter its argument's
// kind, an untyped local the kind every definition of it yields (its initializer, each store, each increment), to a fixpoint; a slot a
// call may bind by reference none. result: the kind every return yields (jai_null_type: none)
std::vector<script_value_type> vm_compiler::site_kinds(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame, script_value_type* result) const {
	using K = script_value_type;
	if (result) { *result = K::jai_null_type; }
	if (!fn || !fn->body) return {};
	site_walk w;
	w.eng = engine_;
	w.kind.assign(fn->local_count, K::jai_null_type);
	w.open.assign(fn->local_count, 0);
	for (size_t i = 0; i < fn->parameters.size() && i < call->arguments.size(); ++i) {
		const parameter& p = fn->parameters[i];
		if (p.slot_index >= fn->local_count || p.is_reference) continue;
		if (scalar_type(p.type.get())) { w.kind[p.slot_index] = p.type->base_type; continue; }
		if (!any_type(p.type.get())) continue;
		w.kind[p.slot_index] = arg_kind(call->arguments[i].get(), frame);
		w.open[p.slot_index] = 1;
	}
	walk_decls(fn->body->declarations, w);
	if (!w.ok) return {};
	settle(w);
	if (result && !w.returns.empty()) {
		K r = scalar_kind(walk_kind(w.returns.front(), w));
		for (const expression* v : w.returns) { if (scalar_kind(walk_kind(v, w)) != r) { r = K::jai_null_type; break; } }
		*result = r;
	}
	return w.kind;
}

// an untyped leaf's result at the site: the scalar kind every return yields under the site's kinds, else none (the call stays)
script_value_type vm_compiler::untyped_result_kind(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame) const {
	script_value_type r = script_value_type::jai_null_type;
	(void)site_kinds(call, fn, frame, &r);
	return r;
}

// the declared types of a graft's parameter slots as its clone reads them: a by-value parameter's own, a reference parameter bound to a
// variable that variable's (the rename makes it the variable), from the frame the arguments are read in (null: the caller's)
void vm_compiler::graft_param_types(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame, std::vector<const type_info*>& out) const {
	out.assign(fn->local_count, nullptr);
	for (size_t i = 0; i < fn->parameters.size() && i < call->arguments.size(); ++i) {
		const parameter& p = fn->parameters[i];
		if (p.slot_index >= fn->local_count) continue;
		if (!p.is_reference) { out[p.slot_index] = p.type.get(); continue; }
		if (call->arguments[i]->get_type() != node_type::identifier_expr) continue;
		auto* id = static_cast<const identifier_expr*>(call->arguments[i].get());
		if (!id->names_value_decl || id->slot_index == SIZE_MAX) continue;
		if (frame) { if (id->slot_index < frame->size()) { out[p.slot_index] = (*frame)[id->slot_index]; } }
		else if (const auto it = callable_.slot_types.find(static_cast<uint32_t>(id->slot_index)); it != callable_.slot_types.end()) { out[p.slot_index] = it->second; }
	}
}

// the graft's result type: the declared one, or for an untyped lambda the kind the checker proved its returns yield
type_info_ptr vm_compiler::graft_result_type(const function_decl* fn) {
	if (fn->return_type || fn->graft_return_kind == 0) return fn->return_type;
	return inline_type(fn->graft_return_kind == 1 ? script_value_type::jai_int_type : fn->graft_return_kind == 2 ? script_value_type::jai_float_type : script_value_type::jai_bool_type);
}

// `var&`: a reference parameter's alias declaration binds as the call binds the parameter (no check at the bind; a store through it
// follows the referent)
type_info_ptr vm_compiler::inline_ref_type() {
	for (auto& t : chunk_->owned_types) { if (t->base_type == script_value_type::jai_reference_type) return type_info_ptr(t.get()); }
	chunk_->owned_types.push_back(std::make_unique<type_info>(script_value_type::jai_any_type, std::string_view("var")));
	const type_info_ptr any(chunk_->owned_types.back().get());
	auto ref = std::make_unique<type_info>(script_value_type::jai_reference_type, std::string_view("var&"));
	ref->type_params.push_back(any);
	ref->id = symbolizer_->intern(ref->canonical_name());
	chunk_->owned_types.push_back(std::move(ref));
	return type_info_ptr(chunk_->owned_types.back().get());
}

type_info_ptr vm_compiler::inline_type(script_value_type kind) {
	for (auto& t : chunk_->owned_types) { if (t->base_type == kind) return type_info_ptr(t.get()); }
	const char* name = kind == script_value_type::jai_int_type ? "int" : kind == script_value_type::jai_float_type ? "float" : kind == script_value_type::jai_bool_type ? "bool" : "string";
	chunk_->owned_types.push_back(std::make_unique<type_info>(kind, std::string_view(name)));
	return type_info_ptr(chunk_->owned_types.back().get());
}

std::shared_ptr<identifier_expr> vm_compiler::inline_temp(const source_location& loc) {
	if (inline_symbol_ == 0) { inline_symbol_ = symbolizer_->intern("\x01inl"); }
	callable_.declared.insert(inline_symbol_);   // a private name no source spells: declared for the callable once
	auto t = std::make_shared<identifier_expr>(loc, std::string_view("\x01inl"), inline_symbol_);
	t->slot_index = temp_next_++; t->names_value_decl = true;
	callable_.slot_types.erase(static_cast<uint32_t>(t->slot_index));   // a slot an earlier graft declared: the temp holds anything
	if (temp_next_ > temp_high_) { temp_high_ = temp_next_; }
	chunk_->nodes.push_back(t);
	return t;
}

// the spills, the graft into the result temp, the statement rewritten over the temps
expression_ptr vm_compiler::compile_hoist(const hoist_plan& plan, const expression_ptr& root, const char*& refused) {
	const function_decl* fn = plan.fn.get(); const std::shared_ptr<call_expr>& call = plan.call;
	// the body's clone as the graft compiles it: the callee's slots above the frame, a reference parameter bound to a variable renamed to it
	auto setup = [&](inline_context& c, size_t base, const std::shared_ptr<identifier_expr>& result) {
		c.lc = graft_land_ctx(fn);
		c.lc.frame = &c.slot_types;
		c.lc.origins = &c.origins;
		c.slot_offset = base;
		c.renames.assign(fn->local_count, nullptr);
		c.ref_slots.assign(fn->local_count, 0);
		for (const parameter& p : fn->parameters) { if (p.is_reference && p.slot_index < fn->local_count) { c.ref_slots[p.slot_index] = 1; } }
		c.result = result; c.result_type = graft_result_type(fn); c.callee_name = fn->name; c.in_loop = plan.in_loop; graft_in_loop_ = plan.in_loop;
		c.returns_proven = fn->graft_return_kind != 0;
		c.receiver = plan.receiver;
		c.receiver_this = plan.receiver_this;
		c.siblings = method_frame();
		c.caller_global = [this](const identifier_expr* id) { return this_global_of(id, true); };
		c.caller_field = [this](const identifier_expr* id) { return this_field_slot_of(id, true) != 0; };
		graft_param_types(call.get(), fn, nullptr, c.slot_types);
	};
	auto rename = [&](inline_context& c, size_t i) {   // a reference parameter bound to a variable is renamed to it
		auto* id = static_cast<const identifier_expr*>(call->arguments[i].get());
		c.renames[fn->parameters[i].slot_index] = copy_identifier(id, id->location, c);
	};
	// a scalar value parameter the body never writes, its argument a private local of the parameter's declared type that no other argument
	// writes or binds by reference: the copy is unobservable, so the parameter is a renaming too (no move into the callee's slot)
	std::vector<chunk::inline_site::forward_callee> forward_callees;
	auto forwards = [&](size_t i) -> bool {
		const parameter& p = fn->parameters[i];
		const expression_ptr& a = call->arguments[i];
		if (p.is_reference || value_param(p) || !scalar_type(p.type.get()) || p.slot_index >= fn->local_count || !private_local(a.get())) return false;
		auto* id = static_cast<identifier_expr*>(a.get());
		const uint32_t slot = identifier_slot_operand(id);
		const auto t = callable_.slot_types.find(slot);
		if (t == callable_.slot_types.end() || !t->second || t->second->base_type != p.type->base_type) return false;
		for (size_t j = 0; j < call->arguments.size(); ++j) {
			if (j == i) continue;
			if (slot_argument_in(call->arguments[j].get(), slot)) return false;
			if (fn->parameters[j].is_reference && call->arguments[j]->get_type() == node_type::identifier_expr && static_cast<const identifier_expr*>(call->arguments[j].get())->slot_index == id->slot_index) return false;
		}
		std::vector<parameter> as_reference = fn->parameters;
		as_reference[i].is_reference = true;
		// a program-scope script function the body calls answers through its own parameters (a by-value one copies); the guard re-checks
		// each one read when a definition changes (forward_callees), any other callee is unseen and an argument use counts as a write
		std::vector<chunk::inline_site::forward_callee> read;
		detail::ref_write_analysis writes([&](const identifier_expr& id, std::vector<detail::ref_write_callee>& out) -> bool {
			if (!engine_ || id.global_qualified || id.slot_index != SIZE_MAX || id.symbol_id == UINT64_MAX) return false;
			const auto env = detail::engine_access::get_global_environment(*engine_);
			const script_value* v = env ? env->get_value_ptr(id.symbol_id) : nullptr;
			if (!v || !v->deref().is_function()) return false;
			const auto* thunk = v->deref().as_function().target<script_callable_thunk>();
			if (!thunk || thunk->payload.kind != script_callable::kind_type::function || !thunk->payload.fn || thunk->payload.fn->closure_env || thunk->payload.fn->converted() || !thunk->payload.fn->body) return false;
			out.push_back(detail::ref_write_callee{&thunk->payload.fn->parameters(), thunk->payload.fn->body.get(), thunk->payload.fn->name, false, false});
			read.push_back(chunk::inline_site::forward_callee{id.symbol_id, thunk->payload.fn->body.get(), thunk->payload.fn->body});
			return true;
		});
		if (writes.analyze(detail::ref_write_callee{&as_reference, fn->body.get(), {}, false, false}, i).writes) return false;
		for (const auto& r : read) { if (std::find(forward_callees.begin(), forward_callees.end(), r) == forward_callees.end()) { forward_callees.push_back(r); } }
		return true;
	};
	std::vector<uint8_t> forwarded(fn->parameters.size(), 0);
	for (size_t i = 0; i < fn->parameters.size() && i < call->arguments.size(); ++i) { forwarded[i] = forwards(i) ? 1 : 0; }
	// the clone's verdict before anything is emitted, with the slots it will have: a body the plan's probe took and this clone does not keeps the
	// call (the graft never compiles a body its clone refused)
	refused = nullptr;
	{
		size_t temps = 0;
		for (const auto& sp : plan.spills) {
			if (sp.second != script_value_type::jai_object_type) { ++temps; continue; }
			const auto* pc = static_cast<const call_expr*>(sp.first.get());
			if (auto prefix = plan.argument_prefixes.find(pc); prefix != plan.argument_prefixes.end()) {
				const auto old = callee_probes_.find(pc);
				const size_t have = old == callee_probes_.end() ? 0 : old->second.second;
				if (prefix->second > have) { temps += prefix->second - have; }
			}
		}
		std::shared_ptr<identifier_expr> r;
		if (!void_type(fn->return_type.get())) { r = std::make_shared<identifier_expr>(call->location, std::string_view("\x01inl"), uint64_t(1)); r->slot_index = temp_next_ + temps; r->names_value_decl = true; }
		size_t value_temps = 0;
		for (const parameter& p : fn->parameters) { if (value_param(p)) { ++value_temps; } }
		inline_context dry;
		setup(dry, temp_next_ + temps + (r ? 1 : 0) + value_temps, r);
		for (size_t i = 0; i < fn->parameters.size(); ++i) { if ((fn->parameters[i].is_reference && call->arguments[i]->get_type() == node_type::identifier_expr) || forwarded[i]) { rename(dry, i); } }
		if (r) { dry.pins.push_back(r); }
		(void)clone_body(fn->body->declarations, dry, true);
		if (!dry.ok) { refused = dry.why ? dry.why : "a body the graft does not take"; return root; }
		if (dry.calls && (dry.nodes > k_inline_call_max_nodes || body_has_loop(fn->body.get()))) { refused = "a body that calls, costing more than its frame"; return root; }   // a loop amortizes the frame
	}
	if (graft_nesting_++ == 0) { graft_tree_nodes_ = 0; }
	const bool was_off = inlining_off_;
	inlining_off_ = true;   // the spills, the graft's statements and the fallback compile plainly
	inline_context sc;      // the statement's clone: the spilled operands and the call become temps
	sc.budget = SIZE_MAX;
	sc.caller_this = true;
	sc.alias_pins = &alias_pins_;
	sc.callee_probes = &callee_probes_;
	sc.staged_arguments = &staged_arguments_;
	std::vector<const call_expr*> pinned;
	for (const auto& [sub, kind] : plan.spills) {
		if (kind == script_value_type::jai_function_type) {
			// a stdlib alias above the graft: its guard resolves the callee here, before the call runs, and the alias's op reads the guard's mark
			auto* ac = static_cast<const call_expr*>(sub.get());
			call_site site;
			site.call_node = add_node(sub);
			site.callee = make_fused_operand(ac->callee);
			site.arg_symbols.push_back(UINT64_MAX);
			site.arg_slots.push_back(k_invalid_u32);
			chunk_->call_sites.push_back(std::move(site));
			const uint32_t s = static_cast<uint32_t>(chunk_->call_sites.size() - 1);
			const uint32_t mark = static_cast<uint32_t>(temp_next_++);
			if (temp_next_ > temp_high_) { temp_high_ = temp_next_; }
			emit(opcode::op_math_guard, s, mark, static_cast<uint32_t>(math_temp_operand(ac)));
			alias_pins_[ac] = { s, mark };
			pinned.push_back(ac);
			continue;
		}
		if (kind == script_value_type::jai_object_type) {
			// a callee no graft takes, above the graft: its probe parks it here and the call compiled later takes the parked callee
			auto* pc = static_cast<const call_expr*>(sub.get());
			auto found = callee_probes_.find(pc);
			const uint32_t probe = found == callee_probes_.end()
				? static_cast<uint32_t>(emit(opcode::op_probe_callee, 0, static_cast<uint32_t>(pc->arguments.size())))
				: found->second.first;
			size_t staged = found == callee_probes_.end() ? 0 : found->second.second;
			if (auto prefix = plan.argument_prefixes.find(pc); prefix != plan.argument_prefixes.end()) {
				std::vector<uint8_t> captures;
				detail::transient_read_marker::stamp_arg_captures(pc->arguments, captures);
				for (; staged < prefix->second; ++staged) {
					compile_expression(pc->arguments[staged]);
					emit_arg_capture(captures, staged, pc->arguments[staged], probe, capture_form_probe);
					const uint32_t slot = static_cast<uint32_t>(temp_next_++);
					if (temp_next_ > temp_high_) { temp_high_ = temp_next_; }
					emit(opcode::op_call_temp, slot);
					note_landing(pc->arguments[staged].get());
					staged_arguments_[pc].push_back(slot);
				}
			}
			callee_probes_[pc] = {probe, static_cast<uint32_t>(staged)};
			pinned.push_back(pc);
			continue;
		}
		if (kind == script_value_type::jai_array_type) {
			// an element read of no proven kind, read where the statement reads it: the fused read's value into a temp (the general read
			// into one when the fused shape is refused)
			fused_operand landed;
			if (!compile_subscript_into_temp(sub, 0xFF, landed)) {
				compile_expression(sub);
				landed = fused_operand{};
				landed.slot = static_cast<uint32_t>(temp_next_++);
				if (temp_next_ > temp_high_) { temp_high_ = temp_next_; }
				emit(opcode::op_call_temp, landed.slot);
				note_landing(sub.get());
			}
			if (inline_symbol_ == 0) { inline_symbol_ = symbolizer_->intern("\x01inl"); }
			callable_.declared.insert(inline_symbol_);
			auto tid = std::make_shared<identifier_expr>(sub->location, std::string_view("\x01inl"), inline_symbol_);
			tid->slot_index = landed.slot; tid->names_value_decl = true;
			callable_.slot_types.erase(landed.slot);
			chunk_->nodes.push_back(tid);
			sc.spills.emplace_back(sub.get(), tid);
			continue;
		}
		auto tid = inline_temp(sub->location);
		if (kind == script_value_type::jai_any_type) {   // a name's value as it stands
			fused_binary_proto p;
			p.left = make_fused_operand(sub);
			chunk_->fused_binary_protos.push_back(std::move(p));
			// the container of an element read: the guard's always form takes its value here and the read keeps the name as its container,
			// reading the guard's temp whatever its index (a block's site resolves the name once at entry; a temp in its place would be a
			// container rewritten inside the body)
			const binary_expr* read = nullptr;
			std::vector<expression_ptr> path;
			if (sub->get_type() == node_type::identifier_expr && find_path(root, sub.get(), path) && path.size() >= 2 && path[path.size() - 2]->get_type() == node_type::binary_expr) {
				auto* b = static_cast<const binary_expr*>(path[path.size() - 2].get());
				// a value read: the statement itself, or an operand of arithmetic, a comparison, a non-mutating unary or a builtin method's argument
				bool read_position = path.size() < 3;
				if (!read_position) {
					const expression* up = path[path.size() - 3].get();
					switch (up->get_type()) {
					case node_type::binary_expr: { auto* ub = static_cast<const binary_expr*>(up); read_position = !(ub->op.type == token_type::left_bracket && ub->left.get() == b); break; }
					case node_type::unary_expr: read_position = !incdec_op(static_cast<const unary_expr*>(up)->op.type); break;
					case node_type::call_expr: { auto* uc = static_cast<const call_expr*>(up); read_position = uc->builtin_receiver != 0 && uc->callee.get() != b; break; }   // a builtin method's argument: a value
					default: break;
					}
				}
				if (b->op.type == token_type::left_bracket && b->left.get() == sub.get() && read_position) { read = b; }
			}
			emit(opcode::op_operand_temp, static_cast<uint32_t>(chunk_->fused_binary_protos.size() - 1), static_cast<uint32_t>(tid->slot_index), read ? 2u : 0u);
			note_landing(sub.get());
			if (read) { sc.container_guards.emplace_back(read, tid); }
			else { sc.spills.emplace_back(sub.get(), tid); }
			continue;
		}
		auto sd = std::make_shared<variable_decl>(sub->location, inline_type(kind), std::string_view("\x01inl"), inline_symbol_, sub);
		sd->slot_index = tid->slot_index;
		chunk_->nodes.push_back(sd);
		compile_variable_decl(sd);
		sc.spills.emplace_back(sub.get(), tid);
	}
	std::shared_ptr<identifier_expr> result;
	if (!void_type(fn->return_type.get())) { result = inline_temp(call->location); }
	std::vector<std::shared_ptr<identifier_expr>> value_temp(fn->parameters.size());   // a value parameter's argument, evaluated in the call's order
	for (size_t i = 0; i < fn->parameters.size(); ++i) { if (value_param(fn->parameters[i])) { value_temp[i] = inline_temp(call->location); } }

	chunk::inline_site site;
	if (plan.receiver || plan.receiver_this) {
		const uint64_t member_id = call->callee->get_type() == node_type::member_expr ? static_cast<const member_expr*>(call->callee.get())->member_id
		                                                                                : static_cast<const identifier_expr*>(call->callee.get())->symbol_id;
		site.method = true;
		site.receiver_slot = plan.receiver && !plan.receiver_field ? identifier_slot_operand(plan.receiver.get()) : k_invalid_u32;
		site.receiver_this_field = plan.receiver_field ? this_field_slot_of(plan.receiver_field.get()) : 0;
		site.member_id = member_id;
		site.name_symbol = add_symbol(member_id);
	} else {
		site.callee = make_fused_operand(call->callee);
		site.name_symbol = add_symbol(static_cast<identifier_expr*>(call->callee.get())->symbol_id);
	}
	site.body = fn->body.get();
	site.decl = plan.fn;
	site.proof_globals = call->proof_globals;
	for (const auto& g : plan.receiver_globals) { if (std::find(site.proof_globals.begin(), site.proof_globals.end(), g) == site.proof_globals.end()) { site.proof_globals.push_back(g); } }
	site.forward_callees = forward_callees;
	site.epoch_checked = !site.proof_globals.empty() || !site.forward_callees.empty();
	site.plain_numeric = plan.plain_numeric;
	site.call_stmt = current_stmt_;
	site.result_slot = result ? static_cast<uint32_t>(result->slot_index) : k_invalid_u32;
	const uint32_t site_index = static_cast<uint32_t>(chunk_->inline_sites.size());
	chunk_->inline_sites.push_back(site);
	const size_t guard_ip = emit(opcode::op_inline_guard, site_index, 0, 0);
	chunk_->inline_sites[site_index].guard_ip = static_cast<uint32_t>(guard_ip);
	chunk_->inline_sites[site_index].begin_ip = static_cast<uint32_t>(guard_ip + 1);

	const size_t base = temp_next_;
	temp_next_ += fn->local_count;   // the callee's slots
	if (temp_next_ > temp_high_) { temp_high_ = temp_next_; }
	inline_context ctx;
	setup(ctx, base, result);
	// the operations over the callee's slots the checker left unclassified take the classes the site's argument kinds give them: stamps
	// every consumer guards (an argument holding another kind at run time takes the general form), never a declaration
	script_value_type site_result = script_value_type::jai_null_type;
	ctx.site_kinds = site_kinds(call.get(), fn, nullptr, &site_result);
	ctx.stamps = !ctx.site_kinds.empty();
	// the parameters: a by-value parameter is a declaration of the callee's type from the argument (the assignment table
	// bind_parameters runs; the checker proved the type, so nothing converts); a reference parameter bound to an identifier is a
	// renaming, bound to an element lvalue an alias declaration (a rebind)
	// a value parameter: its argument into its temp in the call's order, its declaration binding it after every argument by the parameter's
	// own lane (exec_decl_var: a conversion, a copy, a nocopy refusal where the call raises them)
	std::vector<std::shared_ptr<variable_decl>> params;
	std::vector<size_t> param_arg;   // per entry of params: the argument index whose value lands in a temp first (SIZE_MAX: a declaration)
	std::vector<std::shared_ptr<variable_decl>> deferred;
	for (size_t i = 0; i < fn->parameters.size(); ++i) {
		const parameter& p = fn->parameters[i];
		const expression_ptr& a = call->arguments[i];
		if ((p.is_reference && a->get_type() == node_type::identifier_expr) || forwarded[i]) { rename(ctx, i); continue; }
		if (value_temp[i]) {
			params.push_back(nullptr); param_arg.push_back(i);
			auto d = std::make_shared<variable_decl>(call->location, p.type, p.name, p.symbol_id != UINT64_MAX ? p.symbol_id : symbolizer_->intern(p.name), copy_identifier(value_temp[i].get(), a->location, ctx));
			d->slot_index = base + p.slot_index;
			d->site_kind = variable_decl::site_parameter; d->site_owner = fn->name;
			d->graft_site = site_index; d->graft_param = static_cast<uint32_t>(i); d->graft_temp_slot = value_temp[i]->slot_index;
			d->graft_source_class = detail::classify_source(a.get(), [this](const call_expr& c) {
				return engine_ && detail::call_returns_class_value(c, const_cast<engine&>(*engine_));
			});
			d->decl_fast_flags = 0;   // the declaration binds by the parameter's lane (exec_decl_var), never a declaration's fast store
			ctx.pins.push_back(d);
			deferred.push_back(d);
			continue;
		}
		auto d = std::make_shared<variable_decl>(call->location, p.is_reference ? inline_ref_type() : p.type, p.name, p.symbol_id != UINT64_MAX ? p.symbol_id : symbolizer_->intern(p.name), a);   // an element: its alias, as the call binds it
		d->slot_index = base + p.slot_index;
		if (!p.is_reference) { d->site_kind = variable_decl::site_parameter; d->site_owner = fn->name; }   // a guard's miss converts with the call's text
		ctx.pins.push_back(d);
		params.push_back(d); param_arg.push_back(SIZE_MAX);
	}
	std::vector<declaration_ptr> body = clone_body(fn->body->declarations, ctx, true);
	graft_tree_nodes_ += ctx.nodes;
	chunk_->inline_sites[site_index].field_class = ctx.field_class;
	chunk_->inline_sites[site_index].field_ids = ctx.field_ids;
	chunk_->inline_sites[site_index].global_ids = ctx.global_ids;
	chunk_->inline_sites[site_index].sibling_ids = ctx.sibling_ids;
	chunk_->inline_sites[site_index].caller_globals = ctx.caller_globals;
	chunk_->inline_sites[site_index].env_reads = ctx.reads_env && !plan.receiver;
	// the callee's names count as declared while the graft compiles (a slot identifier compiles to its slot only under a declared
	// name); the names the caller did not declare leave before the fallback, which compiles the call as the caller's
	std::vector<uint64_t> added;
	auto declare = [&](uint64_t id) { if (id != UINT64_MAX && callable_.declared.insert(id).second) { added.push_back(id); } };
	for (const parameter& p : fn->parameters) { declare(p.symbol_id != UINT64_MAX ? p.symbol_id : symbolizer_->intern(p.name)); }
	for (const uint64_t id : ctx.declared_names) { declare(id); }
	push_region(region_kind::loop);
	loops_.back().kind = region_kind::graft;
	loops_.back().scope_depth = scope_depth_;
	kill_scopes_.emplace_back();   // the callee's frame: its parameters and its body's locals die where the call would return
	inlining_off_ = was_off;   // a leaf in an argument grafts inside its parameter's declaration, after this guard (the callee before the arguments)
	for (size_t k = 0; k < params.size(); ++k) {
		if (params[k]) { compile_variable_decl(params[k]); note_kill_decl(params[k].get()); continue; }
		const size_t i = param_arg[k];
		compile_expression(call->arguments[i]);
		emit(opcode::op_call_temp, static_cast<uint32_t>(value_temp[i]->slot_index));
	}
	for (const auto& d : deferred) {
		compile_expression(d->initializer);
		callable_.slot_types[static_cast<uint32_t>(d->slot_index)] = d->type.get();
		emit(opcode::op_decl_var, add_node(d), 1, 1);
		note_kill_decl(d.get());
		if (!d->type || d->type->may_own_instance()) { emit(opcode::op_kill_slot, static_cast<uint32_t>(d->graft_temp_slot)); }   // the argument's temp: the parameter holds it now
	}
	flush_kills();
	current_stmt_ = site.call_stmt;
	chunk_->inline_sites[site_index].begin_ip = static_cast<uint32_t>(chunk_->code.size());   // the arguments are the caller's (a raise there names no callee)
	graft_bodies_.push_back(fn->body.get());
	if (ctx.calls) {
		// a call the body makes runs one level below the grafted frame, as the callee's own frame would put it
		chunk_->inline_sites[site_index].calls = true;
		chunk_->graft_calls = true;
		emit(opcode::op_graft_depth, 1, 0, 0);
	}
	inlining_off_ = false;   // a leaf the body calls grafts here, a level down, within the tree's node budget
	for (const auto& d : body) {
		compile_declaration(d, false);
		note_kill_decl(d.get());
	}
	close_kill_scope();
	inlining_off_ = true;
	graft_bodies_.pop_back();
	current_stmt_ = site.call_stmt;   // the fallback and the rest of the statement are the caller's
	std::vector<size_t> exits = std::move(loops_.back().break_patches);
	loops_.pop_back();
	for (const uint64_t id : added) { callable_.declared.erase(id); }
	for (auto& n : ctx.pins) { chunk_->nodes.push_back(n); }
	if (ctx.calls) {
		const size_t pad = emit(opcode::op_graft_depth, 0, 0, 0);
		for (size_t at : exits) { patch_jump(at, pad); }
		exits.clear();
	}
	const size_t jump_ip = emit(opcode::op_jump, 0, 0, 0);
	chunk_->inline_sites[site_index].fallback_ip = static_cast<uint32_t>(chunk_->code.size());
	chunk_->code[guard_ip].b = static_cast<uint32_t>(chunk_->code.size());
	// the fallback: the real call, declared into the result temp as whatever it returns (the callee is not the one the guard knew)
	if (result) {
		auto fd = std::make_shared<variable_decl>(call->location, nullptr, std::string_view("\x01inl"), inline_symbol_, call);
		fd->slot_index = result->slot_index;
		chunk_->nodes.push_back(fd);
		compile_variable_decl(fd);
	} else if (!compile_no_result_expression(call)) {
		compile_expression(call, true);
		emit(opcode::op_pop);
	}
	const size_t join = chunk_->code.size();
	chunk_->inline_sites[site_index].join_ip = static_cast<uint32_t>(join);
	patch_jump(jump_ip, join);
	for (size_t at : exits) { patch_jump(at, join); }
	temp_next_ = base;   // the callee's slots go; the temps stay for the statement
	for (size_t s = base; s < base + fn->local_count; ++s) { callable_.primitive_slots.erase(static_cast<uint32_t>(s)); callable_.slot_types.erase(static_cast<uint32_t>(s)); }   // another graft may put anything in these slots
	if (result) { callable_.primitive_slots.erase(static_cast<uint32_t>(result->slot_index)); }   // the fallback declares it as anything
	inlining_off_ = was_off;
	sc.replace = call.get(); sc.result = result; sc.result_type = graft_result_type(fn);   // a replaced argument's proof reads the type
	// the statement's operations over the result the checker could not classify (an untyped callee's result) take the classes the kinds give
	sc.replace_kind = scalar_type(sc.result_type.get()) ? sc.result_type->base_type : site_result;
	if (sc.replace_kind != script_value_type::jai_null_type) {
		sc.stamps = true;
		size_t hi = 0;
		for (const auto& [s, t] : callable_.slot_types) { hi = std::max<size_t>(hi, static_cast<size_t>(s) + 1); }
		sc.slot_types.assign(hi, nullptr);
		for (const auto& [s, t] : callable_.slot_types) { sc.slot_types[s] = t; }
	}
	for (const auto& [read, slot] : subscript_guards_) { sc.container_guards.emplace_back(read, slot); }   // an earlier round's guarded read, cloned again
	expression_ptr replaced = clone_expr(root, sc);
	for (const auto& [read, slot] : sc.container_guards) { if (read->get_type() == node_type::binary_expr) { subscript_guards_.erase(static_cast<const binary_expr*>(read)); } }
	for (const auto& [read, slot] : sc.guarded_reads) { subscript_guards_[read] = slot; }
	for (const call_expr* p : pinned) { alias_pins_.erase(p); callee_probes_.erase(p); staged_arguments_.erase(p); }   // moved to the clone's node when the clone holds it
	for (auto& n : sc.pins) { chunk_->nodes.push_back(n); }
	--graft_nesting_;
	return sc.ok ? replaced : nullptr;
}

// every leaf the statement calls, first to last, into temps; the expression left for the statement to compile (the original when
// nothing was hoisted; null when the statement was one void call)
expression_ptr vm_compiler::hoist_leaf_calls(const expression_ptr& e, bool statement_position, bool& changed) {
	changed = false;
	if (inlining_off_ || !e || !ir_passes_ || !typed_lowering_ || !callable_.active) return e;
	expression_ptr cur = e;
	for (size_t round = 0; round < k_inline_max_hoists && cur; ++round) {
		hoist_plan plan;
		if (plan_hoist(cur, statement_position && round == 0, plan)) {
			const char* refused = nullptr;
			expression_ptr next = compile_hoist(plan, cur, refused);
			if (!refused) { cur = next; changed = true; continue; }
			plan.why = refused;
		}
		if (leaf_retry_ && plan.why && std::string_view(plan.why) == "an operand behind && or ||") break;   // the leaf's own hoist takes it
		if (plan.why && plan.call && plan.call->callee && plan.call->callee->get_type() == node_type::identifier_expr) {   // the kept call, named for the listing and the profile
			chunk_->inline_refusals.push_back({std::string(static_cast<const identifier_expr*>(plan.call->callee.get())->name), plan.why, static_cast<uint32_t>(plan.call->location.line)});
		} else if (plan.why && plan.call && ever_stamped(plan.call->proven_method_decl) && plan.call->callee->get_type() == node_type::member_expr) {
			chunk_->inline_refusals.push_back({"." + std::string(static_cast<const member_expr*>(plan.call->callee.get())->member), plan.why, static_cast<uint32_t>(plan.call->location.line)});
		}
		break;
	}
	return cur;
}

// a graft's return: the scopes between it and the graft's start pop, and the jump lands on the join
void vm_compiler::compile_graft_exit() {
	size_t target = SIZE_MAX;
	for (size_t idx = loops_.size(); idx-- > 0;) { if (loops_[idx].kind == region_kind::graft) { target = idx; break; } }
	if (target == SIZE_MAX) return;
	emit_region_exits(target, loops_[target].kill_depth);
	auto& region = loops_[target];
	if (scope_depth_ > region.scope_depth) { emit(opcode::op_scope_pop_n, static_cast<uint32_t>(scope_depth_ - region.scope_depth)); }
	region.break_patches.push_back(emit(opcode::op_jump, k_invalid_u32));
}

// an expression statement: `f(...);`, `v = f(...);`, `s = s + f(...);`, `a.push(f(...));`
JAI_NOINLINE bool vm_compiler::try_inline_expression(const expression_ptr& e) {
	if (inlining_off_ || !e) return false;
	const size_t mark = temp_next_;
	bool changed = false;
	expression_ptr h = hoist_leaf_calls(e, true, changed);
	if (!changed) return false;
	if (h && h->get_type() != node_type::identifier_expr && !compile_no_result_expression(h)) { compile_expression(h, true); emit(opcode::op_pop); }   // a bare temp: the result dropped
	free_temps(mark);
	return true;
}

// a declaration whose initializer calls a leaf: `T v = f(...);`, `T v = a + f(...);`
bool vm_compiler::try_inline_variable_decl(const std::shared_ptr<variable_decl>& decl) {
	if (inlining_off_ || !decl->initializer || decl->slot_index == SIZE_MAX) return false;
	if (decl->type && decl->type->is_reference()) return false;
	const size_t mark = temp_next_;
	bool changed = false;
	expression_ptr h = hoist_leaf_calls(decl->initializer, false, changed);
	if (!changed) return false;
	auto nd = std::make_shared<variable_decl>(decl->location, decl->type, decl->name, decl->name_id, h);
	nd->slot_index = decl->slot_index; nd->is_static = decl->is_static; nd->function_spelling = decl->function_spelling; nd->ref_escaping = decl->ref_escaping;
	nd->decl_fast_flags = decl->decl_fast_flags; nd->site_kind = decl->site_kind; nd->site_owner = decl->site_owner;
	nd->alias_home = decl->alias_home; nd->alias_index = decl->alias_index;
	chunk_->nodes.push_back(nd);
	compile_variable_decl(nd);
	free_temps(mark);
	return true;
}

// a return whose value calls a leaf: `return f(...);`, `return a + f(...);`
JAI_NOINLINE bool vm_compiler::try_inline_return(const statement_ptr& stmt) {
	if (inlining_off_) return false;
	auto* rs = static_cast<return_stmt*>(stmt.get());
	if (!rs->value || rs->binds_reference) return false;
	const size_t mark = temp_next_;
	bool changed = false;
	expression_ptr h = hoist_leaf_calls(rs->value, false, changed);
	if (!changed) return false;
	auto nr = std::make_shared<return_stmt>(rs->location, h);
	nr->static_return_proof = rs->static_return_proof; nr->return_operand_class = rs->return_operand_class;
	chunk_->nodes.push_back(nr);
	const size_t from = chunk_->stmt_nodes.size();
	compile_statement(nr);
	drop_debug_span(nr.get());   // the return's own statement records the stop
	for (size_t k = from; k < chunk_->stmt_nodes.size(); ++k) { if (chunk_->stmt_nodes[k] == nr.get()) { chunk_->stmt_nodes[k] = stmt.get(); } }   // one statement with its grafts: a block exit or bail at the return holds their results
	current_stmt_ = stmt.get();
	free_temps(mark);
	return true;
}

// a condition (an if, a while, a for): the grafts precede the condition's jumps; the caller frees the temps after them
// a whole condition that calls a bool function or method whose body is one proven return (`if (current.held())`, `while (r.more())`):
// the graft is the branches of the return's expression, and its fallback branches on the real call's value; no result temp, no landing
bool vm_compiler::try_condition_graft(const expression_ptr& cond, std::vector<size_t>& false_jumps) {
	if (inlining_off_ || !cond || cond->get_type() != node_type::call_expr || !ir_passes_ || !typed_lowering_ || !callable_.active) return false;
	hoist_plan plan;
	if (!plan_hoist(cond, false, plan) || plan.call.get() != cond.get() || !plan.spills.empty() || !plan.argument_prefixes.empty()) return false;
	const function_decl* fn = plan.fn.get();
	if (!fn || !fn->parameters.empty() || !fn->body || fn->body->declarations.size() != 1 || !fn->return_type || fn->return_type->base_type != script_value_type::jai_bool_type) return false;
	const declaration_ptr& only = fn->body->declarations[0];
	if (only->get_type() != node_type::statement_decl) return false;
	const statement* st = static_cast<const statement_decl*>(only.get())->statement.get();
	if (!st || st->get_type() != node_type::return_stmt) return false;
	const auto* ret = static_cast<const return_stmt*>(st);
	if (!ret->value || ret->binds_reference || ret->static_return_proof != proof_same_type) return false;   // a bool the condition reads as the return yields it
	const std::shared_ptr<call_expr>& call = plan.call;
	auto setup = [&](inline_context& c, bool note) {
		c.lc = graft_land_ctx(fn);
		c.lc.frame = &c.slot_types;
		c.lc.origins = &c.origins;
		c.slot_offset = temp_next_;
		c.result_type = graft_result_type(fn); c.callee_name = fn->name; c.in_loop = plan.in_loop;
		c.receiver = plan.receiver;
		c.receiver_this = plan.receiver_this;
		c.caller_global = [this, note](const identifier_expr* id) { return this_global_of(id, note); };
		c.caller_field = [this, note](const identifier_expr* id) { return this_field_slot_of(id, note) != 0; };
	};
	{
		inline_context dry;
		setup(dry, false);
		expression_ptr e = clone_expr(ret->value, dry);
		if (!dry.ok || dry.calls) return false;
		if (plan.in_loop && !landable(e.get(), &dry.lc)) { chunk_->inline_refusals.push_back({std::string(fn->name), "a condition the block cannot land", static_cast<uint32_t>(call->location.line)}); return false; }
	}
	chunk::inline_site site;
	if (plan.receiver || plan.receiver_this) {
		const uint64_t member_id = call->callee->get_type() == node_type::member_expr ? static_cast<const member_expr*>(call->callee.get())->member_id
		                                                                                : static_cast<const identifier_expr*>(call->callee.get())->symbol_id;
		site.method = true;
		site.receiver_slot = plan.receiver && !plan.receiver_field ? identifier_slot_operand(plan.receiver.get()) : k_invalid_u32;
		site.receiver_this_field = plan.receiver_field ? this_field_slot_of(plan.receiver_field.get()) : 0;
		site.member_id = member_id;
		site.name_symbol = add_symbol(member_id);
	} else {
		site.callee = make_fused_operand(call->callee);
		site.name_symbol = add_symbol(static_cast<identifier_expr*>(call->callee.get())->symbol_id);
	}
	site.body = fn->body.get();
	site.decl = plan.fn;
	site.proof_globals = call->proof_globals;
	for (const auto& g : plan.receiver_globals) { if (std::find(site.proof_globals.begin(), site.proof_globals.end(), g) == site.proof_globals.end()) { site.proof_globals.push_back(g); } }
	site.epoch_checked = !site.proof_globals.empty();
	site.plain_numeric = plan.plain_numeric;
	site.call_stmt = current_stmt_;
	const uint32_t site_index = static_cast<uint32_t>(chunk_->inline_sites.size());
	chunk_->inline_sites.push_back(site);
	const size_t guard_ip = emit(opcode::op_inline_guard, site_index, 0, 0);
	chunk_->inline_sites[site_index].guard_ip = static_cast<uint32_t>(guard_ip);
	chunk_->inline_sites[site_index].begin_ip = static_cast<uint32_t>(guard_ip + 1);
	inline_context ctx;
	setup(ctx, true);
	expression_ptr e = clone_expr(ret->value, ctx);
	chunk_->inline_sites[site_index].field_class = ctx.field_class;
	chunk_->inline_sites[site_index].field_ids = ctx.field_ids;
	chunk_->inline_sites[site_index].global_ids = ctx.global_ids;
	chunk_->inline_sites[site_index].sibling_ids = ctx.sibling_ids;
	chunk_->inline_sites[site_index].caller_globals = ctx.caller_globals;
	chunk_->inline_sites[site_index].env_reads = ctx.reads_env && !plan.receiver && !plan.receiver_this;
	const bool was_off = inlining_off_;
	inlining_off_ = true;   // the expression and the fallback compile plainly
	push_region(region_kind::loop);
	loops_.back().kind = region_kind::graft;
	loops_.back().scope_depth = scope_depth_;
	graft_bodies_.push_back(fn->body.get());
	current_stmt_ = st;   // the return's line, as the callee's frame would name it
	compile_branch_condition(e, false_jumps);
	graft_bodies_.pop_back();
	loops_.pop_back();
	for (auto& n : ctx.pins) { chunk_->nodes.push_back(n); }
	current_stmt_ = site.call_stmt;
	const size_t jump_ip = emit(opcode::op_jump, 0, 0, 0);
	chunk_->inline_sites[site_index].fallback_ip = static_cast<uint32_t>(chunk_->code.size());
	chunk_->code[guard_ip].b = static_cast<uint32_t>(chunk_->code.size());
	compile_branch_condition(cond, false_jumps);
	inlining_off_ = was_off;
	const size_t join = chunk_->code.size();
	chunk_->inline_sites[site_index].join_ip = static_cast<uint32_t>(join);
	patch_jump(jump_ip, join);
	return true;
}

expression_ptr vm_compiler::hoisted_condition(const expression_ptr& cond) {
	bool changed = false;
	expression_ptr h = hoist_leaf_calls(cond, false, changed);
	return h ? h : cond;
}

// a for's update: a statement position
void vm_compiler::compile_hoisted_update(const expression_ptr& update) {
	const size_t mark = temp_next_;
	bool changed = false;
	expression_ptr u = hoist_leaf_calls(update, true, changed);
	if (u && u->get_type() != node_type::identifier_expr && !compile_no_result_expression(u)) { compile_expression(u, true); emit(opcode::op_pop); }
	free_temps(mark);
}

} // namespace jai::vm
