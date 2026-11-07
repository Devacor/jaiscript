#pragma once

#ifndef __JAISCRIPT_DETAIL_TRANSIENT_READ_HPP__
#define __JAISCRIPT_DETAIL_TRANSIENT_READ_HPP__

#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/native_stack.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/detail/lvalue_shape.hpp>

namespace jai::detail {

    // ---- Transient-read pass (element-read overhead) ----
    // Stamps binary_expr::transient_element_read on subscript READS whose result is provably
    // consumed as a transient value, so both backends (vm_compiler emit / interpreter subscript
    // branch - parity by construction, ONE classifier) can push a shallow element copy instead
    // of minting a heap reference_holder. Default is MINT-WHEN-UNKNOWN: a position is only
    // marked when enumerated below; every lvalue-consuming context (store/compound target,
    // ref-param, reference-decl initializer, reference-return, mutating receiver, ++/--,
    // range-for subject, call argument, base of an outer lvalue chain) is simply never marked.
    //
    // CHECKLIST - any NEW syntax that can consume a subscript as an lvalue (bind a reference,
    // write through it, or expose its identity) must keep its operand positions UNMARKED here.
    //
    // Marked positions (the transient set):
    //   - operands of builtin binary value ops (arith/compare/bitwise/shift/spaceship);
    //     the LEFT operand only when the right subtree is callout_free (its copy is held
    //     while the right side evaluates - user code running there could observe use_count)
    //   - operands of && and || (each is truthiness-tested before anything else runs)
    //   - conditions of if / while / for / ternary (consumed immediately by is_truthy)
    //   - the index/key expression of ANY subscript (always dereferenced before use)
    //   - the base of a subscript chain that is itself marked (grid[y][x] as an operand),
    //     again only when the index subtree is callout_free
    //   - the VALUE of an assignment (simple or compound) when the target subtree is
    //     callout_free (both backends deref the RHS before storing - byte-parallel)
    //   - the object of a member read used as a value (anything but a bind, a write or a call through it): the read only
    //     looks through the object, so `grid[y][x].cost` and `a[i].pos.x` copy the final field and mint no element reference
    //   - both branches of a ternary propagate the ternary's own consumer context
    //   - the initializer of an int/float/bool/char declaration and the value of a by-value return from a function or lambda
    //     declaring one (the slot converts it; coroutines excluded)
    //
    // Runtime gate: the mark only takes effect while the engine has no GLOBAL
    // binary-operator or "[]" registration (has_custom_binary_ops, latched at the
    // register_overload_impl / add_global chokepoints) - globally overridden ops receive
    // operand copies whose transient +1 would be observable via use_count()/cpp_ref_count
    // inside the override, so the mint path is kept in that case.
    //
    // Class operator/"[]" METHODS deliberately do NOT latch the gate (an engine-wide
    // latch would kill the elision for any host binding operator types). Consequences,
    // both deliberate: (1) a subscript operand feeding a class operator method is READ
    // (copied) at operand-evaluation time - the interpreter's historical order, which the
    // old VM's late deref silently diverged from; (2) use_count observed inside a class
    // operator method may sit one higher than the pre-elision VM - calibration, not
    // semantics, per the transient-copy rule.
    //
    // Unary value ops are deliberately NOT in the set: -/~ do not deref references today
    // (a subscript operand errors on both backends), and marking would make that shape's
    // behavior depend on the runtime gate.
    struct transient_read_marker {
        native_stack_mark stack;

        static bool is_value_binary_op(token_type t) noexcept {
            switch (t) {
                case token_type::plus:
                case token_type::minus:
                case token_type::star:
                case token_type::slash:
                case token_type::percent:
                case token_type::less:
                case token_type::less_equal:
                case token_type::greater:
                case token_type::greater_equal:
                case token_type::equal_equal:
                case token_type::bang_equal:
                case token_type::spaceship:
                case token_type::ampersand:
                case token_type::pipe:
                case token_type::caret:
                case token_type::left_shift:
                case token_type::right_shift:
                    return true;
                default:
                    return false;
            }
        }

        // No user code can run while evaluating this subtree (builtin subscripts included:
        // an object "[]" override flips the engine's runtime gate, closing that hole).
        // intrinsics: a math:: intrinsic call counts too (both backends run it as the kernel).
        template<bool intrinsics = false>
        static bool callout_free(const expression* e) noexcept {
            if (!e) {
                return true;
            }
            switch (e->get_type()) {
                case node_type::literal_expr:
                case node_type::identifier_expr:
                case node_type::this_expr:
                case node_type::super_expr:
                    return true;
                case node_type::binary_expr: {
                    auto* b = static_cast<const binary_expr*>(e);
                    if (b->op.type == token_type::left_bracket || is_value_binary_op(b->op.type) ||
                        b->op.type == token_type::ampersand_ampersand || b->op.type == token_type::pipe_pipe) {
                        return callout_free<intrinsics>(b->left.get()) && callout_free<intrinsics>(b->right.get());
                    }
                    return false;
                }
                case node_type::unary_expr: {
                    auto* u = static_cast<const unary_expr*>(e);
                    if (u->op.type == token_type::bang || u->op.type == token_type::minus ||
                        u->op.type == token_type::plus || u->op.type == token_type::tilde) {
                        return callout_free<intrinsics>(u->operand.get());
                    }
                    return false;   // ++/-- mutate; anything else is unknown
                }
                case node_type::ternary_expr: {
                    auto* t = static_cast<const ternary_expr*>(e);
                    return callout_free<intrinsics>(t->condition.get()) && callout_free<intrinsics>(t->then_expression.get()) &&
                           callout_free<intrinsics>(t->else_expression.get());
                }
                case node_type::call_expr: {
                    if constexpr (intrinsics) {
                        auto* c = static_cast<const call_expr*>(e);
                        if (!c->callee || c->callee->get_type() != node_type::member_expr || c->arguments.size() > 8 ||
                            math_intrinsic_for_call(static_cast<const member_expr*>(c->callee.get())) == math_fn::none) {
                            return false;
                        }
                        for (const auto& a : c->arguments) {
                            if (!callout_free<intrinsics>(a.get())) return false;
                        }
                        return true;
                    }
                    return false;
                }
                case node_type::member_expr:
                    return static_cast<const member_expr*>(e)->enum_constant;   // an enum's value; any other member may be a getter
                default:
                    return false;   // member (getters), call, assignment, new, lambda, literals, ...
            }
        }

        // A call argument's value is taken when it is evaluated: a read of storage (a reference, a heavy handle, a host-bound
        // value) that a later argument may write is captured before that argument runs. ref: only a reference result needs it
        // (a by-value call or operator result is fresh); full: any read of storage.
        static constexpr uint8_t arg_capture_ref = 1;
        static constexpr uint8_t arg_capture_full = 2;
        static constexpr uint8_t arg_capture_kind_mask = 3;
        // beside the kind: an lvalue whose place evaluation decides (a receiver, an index), so a reference parameter's binding
        // mints its reference when the argument is evaluated
        static constexpr uint8_t arg_capture_lvalue = 4;

        static bool place_lvalue_argument(const expression* e) noexcept {
            if (!e || e->get_type() == node_type::identifier_expr) return false;
            if (e->get_type() == node_type::member_expr) {
                const auto* m = static_cast<const member_expr*>(e);
                if (m->is_static) return false;
                if (math::any_component_name(m->member)) return true;
            }
            return is_ref_bindable_lvalue(e);
        }

        static uint8_t arg_capture_kind(const expression* e) noexcept {
            switch (e->get_type()) {
                case node_type::literal_expr:
                case node_type::lambda_expr:
                case node_type::new_expr:
                    return 0;
                case node_type::call_expr:
                case node_type::unary_expr:
                    return arg_capture_ref;
                case node_type::binary_expr:
                    return static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket ? arg_capture_full : arg_capture_ref;
                default:
                    return arg_capture_full;
            }
        }

        // every argument ahead of the last one that may run user code; empty when none needs a capture
        static void stamp_arg_captures(const std::vector<expression_ptr>& args, std::vector<uint8_t>& out) {
            out.clear();
            size_t last = args.size();
            while (last > 0 && callout_free<true>(args[last - 1].get())) { --last; }
            bool any = false;
            for (size_t i = 0; i + 1 < last; ++i) {
                const uint8_t kind = arg_capture_kind(args[i].get());
                if (kind == 0) { continue; }
                if (!any) { out.assign(last - 1, 0); any = true; }
                out[i] = kind | (place_lvalue_argument(args[i].get()) ? arg_capture_lvalue : 0);
            }
        }

        // A reference binds this expression (an argument, a reference declaration's initializer, a cast<T&> operand, a
        // reference return): a member over a base only evaluation names (a computed subscript, a string key) reads as a
        // reference. A math component does wherever a reference may bind; where one surely binds (place_read_field: a
        // declaration, a cast<T&>, a return) every member of the chain does, so a field reads as its FIELD reference. A structural
        // chain binds at the bind point instead (detail/ref_lvalue.hpp resolve_ref_lvalue).
        static void stamp_place_read(expression* e, uint8_t mode = member_expr::place_read_component) {
            while (e && e->get_type() == node_type::member_expr) {
                auto* m = static_cast<member_expr*>(e);
                if (m->is_static || m->null_safe || m->is_arrow || !m->object) return;
                const node_type t = m->object->get_type();
                if (t == node_type::identifier_expr || t == node_type::this_expr || is_ref_bindable_lvalue(e)) return;
                if (mode != member_expr::place_read_field) {
                    if (math::any_component_name(m->member) && !m->place_read) { m->place_read = mode; }
                    return;
                }
                m->place_read = mode;
                e = m->object.get();
            }
        }

        // int/float/bool/char consume a value in every outcome: a matching scalar copies, anything else converts to a fresh
        // value or raises, so nothing retains an un-cloned payload
        static bool hard_scalar(const type_info_ptr& t) noexcept {
            return t && (t->base_type == script_value_type::jai_int_type || t->base_type == script_value_type::jai_float_type ||
                         t->base_type == script_value_type::jai_bool_type || t->base_type == script_value_type::jai_char_type);
        }

        // the innermost function returns a hard scalar by value
        bool scalar_return = false;

        // transient_ctx: this expression's RESULT is consumed as a transient value by its parent. place: the parent may bind,
        // write or call through the result (an lvalue use); a value use (place false) of a member only looks through its object.
        void walk_expr(expression* e, bool transient_ctx, bool place = true) {
            if (!e) {
                return;
            }
            stack.guard(e->location);
            switch (e->get_type()) {
                case node_type::binary_expr: {
                    auto* b = static_cast<binary_expr*>(e);
                    if (b->op.type == token_type::left_bracket) {
                        if (transient_ctx) {
                            b->transient_element_read = true;
                        }
                        // Chain base: transient only when this read is, and the index can't
                        // run user code while the base's copy is held. Index: always deref'd
                        // to a scalar/key immediately, in read AND store-target position.
                        walk_expr(b->left.get(), transient_ctx && callout_free(b->right.get()));
                        walk_expr(b->right.get(), true);
                        return;
                    }
                    if (b->op.type == token_type::ampersand_ampersand || b->op.type == token_type::pipe_pipe) {
                        walk_expr(b->left.get(), true);    // truthiness-tested before right runs
                        walk_expr(b->right.get(), true);
                        return;
                    }
                    if (is_value_binary_op(b->op.type)) {
                        walk_expr(b->left.get(), callout_free(b->right.get()), false);
                        walk_expr(b->right.get(), true);   // consumed by the op immediately
                        return;
                    }
                    walk_expr(b->left.get(), false);
                    walk_expr(b->right.get(), false);
                    return;
                }
                case node_type::unary_expr:
                    walk_expr(static_cast<unary_expr*>(e)->operand.get(), false);
                    return;
                case node_type::assignment_expr: {
                    auto* a = static_cast<assignment_expr*>(e);
                    walk_expr(a->target.get(), false);     // store/compound target: always mint
                    walk_expr(a->value.get(), callout_free(a->target.get()), false);
                    return;
                }
                case node_type::call_expr: {
                    auto* c = static_cast<call_expr*>(e);
                    walk_expr(c->callee.get(), false);
                    for (const auto& arg : c->arguments) {
                        walk_expr(arg.get(), false);       // ref-params: always mint
                        stamp_place_read(arg.get());
                    }
                    stamp_arg_captures(c->arguments, c->arg_captures);
                    return;
                }
                case node_type::member_expr:
                    walk_expr(static_cast<member_expr*>(e)->object.get(), transient_ctx || !place);
                    return;
                case node_type::lambda_expr: {
                    auto* l = static_cast<lambda_expr*>(e);
                    for (auto& p : l->parameters) {
                        walk_expr(p.default_value.get(), false);
                    }
                    for (auto& cap : l->captures) { walk_expr(cap.initializer.get(), false); }
                    const bool outer_scalar_return = scalar_return;
                    scalar_return = hard_scalar(l->return_type);
                    walk_stmt(l->body.get());
                    scalar_return = outer_scalar_return;
                    return;
                }
                case node_type::new_expr: {
                    auto* n = static_cast<new_expr*>(e);
                    for (const auto& arg : n->arguments) {
                        walk_expr(arg.get(), false);
                        stamp_place_read(arg.get());
                    }
                    stamp_arg_captures(n->arguments, n->arg_captures);
                    return;
                }
                case node_type::include_expr:
                    walk_expr(static_cast<include_expr*>(e)->path_expr.get(), false);
                    return;
                case node_type::import_expr:
                    for (const auto& a : static_cast<import_expr*>(e)->path_exprs) walk_expr(a.get(), false);
                    return;
                case node_type::ternary_expr: {
                    auto* t = static_cast<ternary_expr*>(e);
                    walk_expr(t->condition.get(), true);   // consumed before either branch runs
                    walk_expr(t->then_expression.get(), transient_ctx, place);
                    walk_expr(t->else_expression.get(), transient_ctx, place);
                    return;
                }
                case node_type::array_literal_expr:
                    for (const auto& el : static_cast<array_literal_expr*>(e)->elements) {
                        walk_expr(el.get(), false, false);
                    }
                    return;
                case node_type::map_literal_expr:
                    for (const auto& [k, v] : static_cast<map_literal_expr*>(e)->entries) {
                        walk_expr(k.get(), false, false);
                        walk_expr(v.get(), false, false);
                    }
                    return;
                case node_type::throw_expr:
                    walk_expr(static_cast<throw_expr*>(e)->value.get(), false, false);
                    return;
                case node_type::yield_expr:
                    walk_expr(static_cast<yield_expr*>(e)->value.get(), false);
                    return;
                case node_type::cast_expr:
                    walk_expr(static_cast<cast_expr*>(e)->operand.get(), false);   // cast<T&> needs the mint
                    if (static_cast<cast_expr*>(e)->by_reference) { stamp_place_read(static_cast<cast_expr*>(e)->operand.get(), member_expr::place_read_field); }
                    return;
                default:
                    return;   // literal/identifier/this/super: leaves
            }
        }

        void walk_stmt(statement* s) {
            if (!s) {
                return;
            }
            stack.guard(s->location);
            switch (s->get_type()) {
                case node_type::expression_stmt:
                    walk_expr(static_cast<expression_stmt*>(s)->expression.get(), false);
                    return;
                case node_type::block_stmt:
                    for (const auto& d : static_cast<block_stmt*>(s)->declarations) {
                        walk_stmt(d.get());
                    }
                    return;
                case node_type::if_stmt: {
                    auto* i = static_cast<if_stmt*>(s);
                    walk_expr(i->condition.get(), true);
                    walk_stmt(i->then_statement.get());
                    walk_stmt(i->else_statement.get());
                    return;
                }
                case node_type::while_stmt: {
                    auto* w = static_cast<while_stmt*>(s);
                    walk_expr(w->condition.get(), true);
                    walk_stmt(w->body.get());
                    return;
                }
                case node_type::for_stmt: {
                    auto* f = static_cast<for_stmt*>(s);
                    walk_stmt(f->initializer.get());
                    walk_expr(f->condition.get(), true);
                    walk_expr(f->update.get(), false);
                    walk_stmt(f->body.get());
                    return;
                }
                case node_type::range_for_stmt: {
                    auto* r = static_cast<range_for_stmt*>(s);
                    walk_expr(r->container.get(), false);  // subject may be iterated by reference
                    walk_stmt(r->body.get());
                    return;
                }
                case node_type::parallel_for_stmt: {
                    auto* pf = static_cast<parallel_for_stmt*>(s);
                    walk_expr(pf->container.get(), false);  // container is bound live for the region
                    walk_stmt(pf->body.get());
                    return;
                }
                case node_type::return_stmt:
                    walk_expr(static_cast<return_stmt*>(s)->value.get(), scalar_return && !static_cast<return_stmt*>(s)->binds_reference,
                              static_cast<return_stmt*>(s)->binds_reference);
                    if (static_cast<return_stmt*>(s)->binds_reference) { stamp_place_read(static_cast<return_stmt*>(s)->value.get(), member_expr::place_read_field); }
                    return;
                case node_type::try_stmt: {
                    auto* t = static_cast<try_stmt*>(s);
                    walk_stmt(t->try_block.get());
                    walk_stmt(t->catch_block.get());
                    return;
                }
                case node_type::switch_stmt: {
                    auto* sw = static_cast<switch_stmt*>(s);
                    walk_expr(sw->condition.get(), false); // subject is held across case expressions
                    for (const auto& c : sw->cases) {
                        walk_stmt(c.get());
                    }
                    walk_stmt(sw->default_case.get());
                    return;
                }
                case node_type::case_stmt: {
                    auto* c = static_cast<case_stmt*>(s);
                    walk_expr(c->value.get(), false);
                    for (const auto& b : c->body) {
                        walk_stmt(b.get());
                    }
                    return;
                }
                case node_type::default_stmt:
                    for (const auto& b : static_cast<default_stmt*>(s)->body) {
                        walk_stmt(b.get());
                    }
                    return;
                case node_type::statement_decl:
                    walk_stmt(static_cast<statement_decl*>(s)->statement.get());
                    return;
                case node_type::variable_decl: {
                    // HARD-SCALAR declared types (int/float/bool/char - not auto/var)
                    // consume the initializer as a VALUE in every outcome: a matching
                    // scalar copies (copy IS clone), anything else converts to a fresh
                    // value or errors before storing - no path can retain the un-cloned
                    // payload, so the read is transient (the profiled dominant GLOOM
                    // shape: `float d = ch[ci]` minted a holder the decl then dropped).
                    // auto/var stay unmarked (they store heavy payloads as-is - an
                    // un-cloned transient copy would alias the element); reference-typed
                    // decls bind their initializer and compile via the decl_ref ops.
                    auto* d = static_cast<variable_decl*>(s);
                    walk_expr(d->initializer.get(), hard_scalar(d->type), d->type && d->type->base_type == script_value_type::jai_reference_type);
                    if (d->type && d->type->base_type == script_value_type::jai_reference_type) { stamp_place_read(d->initializer.get(), member_expr::place_read_field); }
                    return;
                }
                case node_type::function_decl: {
                    auto* fd = static_cast<function_decl*>(s);
                    for (auto& p : fd->parameters) {
                        walk_expr(p.default_value.get(), false);
                    }
                    for (auto& ci : fd->initializers) {
                        for (const auto& arg : ci.arguments) {
                            walk_expr(arg.get(), false);
                        }
                        stamp_arg_captures(ci.arguments, ci.arg_captures);
                    }
                    const bool outer_scalar_return = scalar_return;
                    scalar_return = !fd->is_coroutine && hard_scalar(fd->return_type);
                    walk_stmt(fd->body.get());
                    scalar_return = outer_scalar_return;
                    return;
                }
                case node_type::class_decl:
                    for (const auto& m : static_cast<class_decl*>(s)->members) {
                        walk_stmt(m.declaration.get());
                    }
                    return;
                case node_type::namespace_decl:
                    for (const auto& d : static_cast<namespace_decl*>(s)->declarations) {
                        walk_stmt(d.get());
                    }
                    return;
                case node_type::expression_decl:
                    walk_expr(static_cast<expression_decl*>(s)->expression.get(), false);
                    return;
                case node_type::destructuring_decl:
                    walk_expr(static_cast<destructuring_decl*>(s)->initializer.get(), false);
                    return;
                case node_type::include_decl:
                    walk_expr(static_cast<include_decl*>(s)->path_expr.get(), false);
                    return;
                case node_type::import_decl:
                    walk_expr(static_cast<import_decl*>(s)->path_expr.get(), false);
                    return;
                default:
                    return;   // break/continue/fallthrough/enum: leaves
            }
        }
    };

    inline void mark_transient_reads(const std::vector<declaration_ptr>& program) {
        transient_read_marker marker;
        for (const auto& d : program) {
            marker.walk_stmt(d.get());
        }
    }

    // ---- Read chains ("Temporaries") ----
    // A link is a member read or a subscript. A chain whose outer link is a member read and which has two links or more holds
    // the value it starts from until the outer read completes when that value is a temporary (anything but a name): C++ keeps a
    // temporary until its full-expression ends, so `make().mid.peek.v` runs peek while make()'s object is alive, and the object
    // dies after peek's own result (both backends hold it; KEEP BYTE-PARALLEL)
    inline bool read_chain_link(const expression* e) {
        if (e->get_type() == node_type::member_expr) { return !static_cast<const member_expr*>(e)->is_static; }
        return e->get_type() == node_type::binary_expr && static_cast<const binary_expr*>(e)->op.type == token_type::left_bracket;
    }

    inline bool chain_root_is_temporary(const expression* root) {
        switch (root->get_type()) {
            case node_type::identifier_expr:
            case node_type::this_expr:
            case node_type::super_expr:
            case node_type::member_expr:   // a static read (C::x, ns::v): a link would have continued the walk
                return false;
            default:
                return true;
        }
    }

    // The temporary a member read's chain holds, else null
    inline const expression* held_chain_root(const member_expr* outer) {
        if (outer->is_static || !outer->object || !read_chain_link(outer->object.get())) { return nullptr; }
        const expression* e = outer->object.get();
        while (read_chain_link(e)) {
            e = e->get_type() == node_type::member_expr ? static_cast<const member_expr*>(e)->object.get() : static_cast<const binary_expr*>(e)->left.get();
            if (!e) { return nullptr; }
        }
        return chain_root_is_temporary(e) ? e : nullptr;
    }

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_TRANSIENT_READ_HPP__
