#include "../../include/jaiscript/detail/parser.hpp"
#include "../../include/jaiscript/core/engine.hpp"
#include "../../include/jaiscript/core/script_namespace.hpp"
#include "../../include/jaiscript/detail/integer_ops.hpp"  // checked constant folding
#include "../../include/jaiscript/detail/scalar_ops.hpp"  // an enumerator initializer's arithmetic
#include "../../include/jaiscript/detail/transient_read.hpp"
#include "../../include/jaiscript/detail/lexer.hpp"  // a spliced include is lexed here
#include "../../include/jaiscript/detail/map_entry_binding.hpp"
#include "../../include/jaiscript/detail/expr_children.hpp"
#include "../../include/jaiscript/detail/program_slots.hpp"
#include "../../include/jaiscript/detail/template_library.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <optional>

namespace jai {

namespace {

    // Escape-mark pass (cell reference model). A variable "ref-escapes" when it may be bound by reference at runtime: it appears
    // as a bare-identifier call argument (callee ref-ness is unknowable statically), as
    // the identifier source of a reference declaration (auto& x = y), or as an explicit
    // by-reference lambda capture. Marked declarations box their storage into a cell
    // (reference_holder cell mode), so reference binding is a handle share and an escaped
    // reference keeps its target alive. Conservative by design: function-level granularity
    // (all same-named decls in a function mark together) and unresolved symbols bubble
    // outward through enclosing functions to the program scope; symbols unresolved even
    // there (cross-execute globals, C++ defines) are handled at bind time instead. Class
    // FIELDS are never registered: field refs use the instance-pinned identity mode.
    struct ref_escape_marker {
        struct scope {
            std::unordered_multimap<uint64_t, variable_decl*> vars;
            std::unordered_multimap<uint64_t, parameter*> params;
            std::unordered_multimap<uint64_t, range_for_stmt*> range_vars;
            std::unordered_multimap<uint64_t, std::pair<destructuring_decl*, size_t>> destructures;
            block_stmt* body = nullptr;   // the function body this scope's frame runs
        };
        std::vector<scope> scopes;
        // the for loops being walked and the names declared inside them that hide a loop's variable (loop null), innermost last
        struct loop_variable { for_stmt* loop; uint64_t symbol; };
        std::vector<loop_variable> loop_variables;
        std::vector<uint64_t> catch_variables;   // the handlers being walked: their variables, and a typed chain's names for one
        detail::native_stack_mark stack;
        // class bodies being walked: inside one, an unqualified name resolves to a member (a script field or a
        // native property) before a same-named program global, and fields are never registered here
        size_t class_depth = 0;
        string_symbolizer* symbolizer = nullptr;
        bool names_execute_elapsed = false;   // the engine stamps host entries once a loaded program can read them

        void escape(uint64_t symbol, bool lambda_capture = false) {
            if (symbol == UINT64_MAX) {
                return;
            }
            if (lambda_capture) {
                for (auto it = loop_variables.rbegin(); it != loop_variables.rend(); ++it) {
                    if (it->symbol != symbol) { continue; }
                    if (it->loop) { it->loop->fresh_binding = true; }
                    break;
                }
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                bool found = false;
                auto vr = it->vars.equal_range(symbol);
                for (auto v = vr.first; v != vr.second; ++v) { v->second->ref_escaping = true; found = true; }
                auto pr = it->params.equal_range(symbol);
                for (auto p = pr.first; p != pr.second; ++p) { p->second->ref_escaping = true; found = true; }
                auto rr = it->range_vars.equal_range(symbol);
                for (auto r = rr.first; r != rr.second; ++r) { r->second->var_ref_escaping = true; found = true; }
                auto dr = it->destructures.equal_range(symbol);
                for (auto d = dr.first; d != dr.second; ++d) {
                    destructuring_decl* decl = d->second.first;
                    if (decl->ref_escaping.size() < decl->names.size()) {
                        decl->ref_escaping.resize(decl->names.size(), false);
                    }
                    decl->ref_escaping[d->second.second] = true;
                    found = true;
                }
                if (found) {
                    if (lambda_capture && it->body) { it->body->shares_cells = true; }
                    return;
                }
            }
        }

        // Ref-return producers only: walk_args never calls this for call arguments, since both backends' ref-param binds
        // box the caller's storage in place on demand (share_env_ref / share_boxed_env_storage).
        void escape_arg(const expression* arg) {
            if (arg && arg->get_type() == node_type::identifier_expr) {
                escape(static_cast<const identifier_expr*>(arg)->symbol_id);
            }
        }

        // Default-[&] lambda bodies: every identifier resolving OUTSIDE the lambda's own
        // scope is captured by reference, so its declaration must cell-box (C++ lambda
        // semantics are the contract). Stack of scope-boundary
        // indexes: while inside a [&] body, an identifier declared at a scope BELOW the
        // innermost boundary escapes.
        std::vector<size_t> by_ref_capture_boundaries;

        void escape_if_outer_read(uint64_t symbol) {
            if (symbol == UINT64_MAX || by_ref_capture_boundaries.empty()) {
                return;
            }
            const size_t boundary = by_ref_capture_boundaries.back();
            // Find the innermost scope declaring the symbol; only scopes BELOW the
            // lambda boundary mark (lambda-own declarations are ordinary locals)
            for (size_t i = scopes.size(); i-- > 0;) {
                auto& sc = scopes[i];
                const bool declared_here =
                    sc.vars.count(symbol) || sc.params.count(symbol) ||
                    sc.range_vars.count(symbol) || sc.destructures.count(symbol);
                if (declared_here) {
                    if (i < boundary) {
                        escape(symbol, true);
                    }
                    return;
                }
            }
        }

        // The innermost scope that declares `symbol`: whether a declaration there is a reference (ref param / auto& decl /
        // auto& loop var; destructured names are always value decls) and whether that scope is the program's
        struct declaration_hit {
            bool found = false;
            bool is_ref = false;
            bool program_scope = false;
        };
        declaration_hit innermost_declaration(uint64_t symbol) const {
            declaration_hit result;
            if (symbol == UINT64_MAX) {
                return result;
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                auto vr = it->vars.equal_range(symbol);
                for (auto v = vr.first; v != vr.second; ++v) {
                    result.found = true;
                    if (v->second->type && v->second->type->base_type == script_value_type::jai_reference_type) {
                        result.is_ref = true;
                    }
                }
                auto pr = it->params.equal_range(symbol);
                for (auto q = pr.first; q != pr.second; ++q) {
                    result.found = true;
                    if (q->second->is_reference) {
                        result.is_ref = true;
                    }
                }
                auto rr = it->range_vars.equal_range(symbol);
                for (auto r = rr.first; r != rr.second; ++r) {
                    result.found = true;
                    if (r->second->is_reference) {
                        result.is_ref = true;
                    }
                }
                if (it->destructures.count(symbol)) {
                    result.found = true;
                }
                if (result.found) {
                    result.program_scope = std::next(it) == scopes.rend();
                    return result;
                }
            }
            return result;
        }

        // True when the innermost scope that declares `symbol` declares it as a reference (unresolved symbols answer
        // false: value-decl semantics is the safe default for stores)
        bool names_reference_decl(uint64_t symbol) const {
            return innermost_declaration(symbol).is_ref;
        }

        // True when the innermost scope that declares `symbol` declares it a by-value var (a var local, a var or untyped
        // parameter, a var loop variable): a store to it rebinds whatever handle it holds (detail::slot_value_assign)
        bool names_var_decl(uint64_t symbol) const {
            if (symbol == UINT64_MAX) {
                return false;
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                auto vr = it->vars.equal_range(symbol);
                auto pr = it->params.equal_range(symbol);
                auto rr = it->range_vars.equal_range(symbol);
                if (vr.first == vr.second && pr.first == pr.second && rr.first == rr.second) {
                    if (it->destructures.count(symbol)) { return false; }
                    continue;
                }
                bool result = true;
                for (auto v = vr.first; v != vr.second; ++v) {
                    if (!v->second->type || v->second->type->base_type != script_value_type::jai_any_type) { result = false; }
                }
                for (auto q = pr.first; q != pr.second; ++q) {
                    if (q->second->is_reference || !q->second->declared_var()) { result = false; }
                }
                for (auto r = rr.first; r != rr.second; ++r) {
                    if (r->second->is_reference || !r->second->element_type || r->second->element_type->base_type != script_value_type::jai_any_type) { result = false; }
                }
                return result;
            }
            return false;
        }

        // The T of the `T& name` declaration or `T& p` parameter a name resolves to, slot-resident with a concrete T (the store
        // decides by T)
        type_info* names_declared_reference(uint64_t symbol) const {
            if (symbol == UINT64_MAX) {
                return nullptr;
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                auto vr = it->vars.equal_range(symbol);
                const bool declared_here = vr.first != vr.second || it->params.count(symbol) || it->range_vars.count(symbol) ||
                                           it->destructures.count(symbol);
                if (!declared_here) {
                    continue;
                }
                if (it->range_vars.count(symbol) || it->destructures.count(symbol)) {
                    return nullptr;
                }
                type_info* found = nullptr;
                auto pr = it->params.equal_range(symbol);
                for (auto q = pr.first; q != pr.second; ++q) {
                    const type_info* t = q->second->slot_index != SIZE_MAX ? detail::parameter_declared_ref(*q->second) : nullptr;
                    if (!t || (found && found != t) || vr.first != vr.second) { return nullptr; }
                    found = const_cast<type_info*>(t);
                }
                for (auto v = vr.first; v != vr.second; ++v) {
                    const variable_decl* d = v->second;
                    if (!d->type || d->type->base_type != script_value_type::jai_reference_type || d->slot_index == SIZE_MAX ||
                        d->declared_ref_storage_enforced || !detail::declared_ref_kind(d->type->element_type().get())) {
                        return nullptr;
                    }
                    if (found && found != d->type->element_type().get()) { return nullptr; }
                    found = d->type->element_type().get();
                }
                return found;
            }
            return nullptr;
        }

        // The declared type of the value declaration a name resolves to (a typed local, a by-value parameter), else null
        const type_info* resolved_value_type(uint64_t symbol) const {
            if (symbol == UINT64_MAX) {
                return nullptr;
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                auto vr = it->vars.equal_range(symbol);
                auto pr = it->params.equal_range(symbol);
                if (vr.first == vr.second && pr.first == pr.second && !it->range_vars.count(symbol) && !it->destructures.count(symbol)) {
                    continue;
                }
                const type_info* found = nullptr;
                int count = 0;
                for (auto v = vr.first; v != vr.second; ++v, ++count) {
                    if (!v->second->type || v->second->type->base_type == script_value_type::jai_reference_type) { return nullptr; }
                    found = v->second->type.get();
                }
                for (auto q = pr.first; q != pr.second; ++q, ++count) {
                    if (q->second->is_reference || !q->second->type) { return nullptr; }
                    found = q->second->type.get();
                }
                return count == 1 ? found : nullptr;
            }
            return nullptr;
        }

        // A reference declaration whose reference value reaches another binding (a call argument may bind a reference
        // parameter): it carries its declared type with it (the runtime stamp, detail::ref_declare)
        void escape_reference_decl(uint64_t symbol) {
            if (symbol == UINT64_MAX) {
                return;
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                auto vr = it->vars.equal_range(symbol);
                bool found = it->range_vars.count(symbol) || it->destructures.count(symbol);
                for (auto v = vr.first; v != vr.second; ++v) {
                    found = true;
                    if (v->second->type && v->second->type->base_type == script_value_type::jai_reference_type) { v->second->ref_escaping = true; }
                }
                auto pr = it->params.equal_range(symbol);
                for (auto q = pr.first; q != pr.second; ++q) {
                    found = true;
                    if (q->second->is_reference) { q->second->declared_ref_escapes = true; }
                }
                if (found) {
                    return;
                }
            }
        }

        // Whether the name resolves to a VALUE declaration at all (false for a reference declaration and for a name the
        // unit does not declare: another unit's global, a host name); a program-scope hit inside a class body may be
        // shadowed by a member at runtime
        bool names_known_value_decl(uint64_t symbol) const {
            const declaration_hit hit = innermost_declaration(symbol);
            return hit.found && !hit.is_ref && !(hit.program_scope && class_depth > 0);
        }

        void walk_args(const std::vector<expression_ptr>& args) {
            for (const auto& a : args) {
                if (a && a->get_type() == node_type::identifier_expr) { escape_reference_decl(static_cast<const identifier_expr*>(a.get())->symbol_id); }
                walk_expr(a.get());
            }
        }

        // Innermost function's ref-return-ness: return statements inside a reference-
        // returning function bind their operand as a reference (return_stmt stamping)
        std::vector<char> ref_return_stack;

        void walk_function(std::vector<parameter>& params, statement* body,
                           const std::vector<constructor_initializer>* inits = nullptr,
                           bool returns_reference = false) {
            scopes.emplace_back();
            if (body && body->get_type() == node_type::block_stmt) { scopes.back().body = static_cast<block_stmt*>(body); }
            ref_return_stack.push_back(returns_reference ? 1 : 0);
            for (auto& p : params) { p.declared_ref_escapes = false; }
            for (auto& p : params) {
                if (p.symbol_id == UINT64_MAX && symbolizer) {
                    p.symbol_id = symbolizer->intern(p.name);
                }
                scopes.back().params.emplace(p.symbol_id, &p);
                if (p.default_value) {
                    walk_expr(p.default_value.get());
                }
            }
            if (inits) {
                for (const auto& ci : *inits) {
                    walk_args(ci.arguments);
                }
            }
            walk_stmt(body);
            ref_return_stack.pop_back();
            scopes.pop_back();
        }

        void walk_expr(expression* e) {
            if (!e) {
                return;
            }
            stack.guard(e->location);
            switch (e->get_type()) {
                case node_type::identifier_expr: {
                    auto* id = static_cast<identifier_expr*>(e);
                    id->names_value_decl = !names_reference_decl(id->symbol_id);
                    id->names_var_decl = !id->global_qualified && names_var_decl(id->symbol_id);
                    id->names_known_value = names_known_value_decl(id->symbol_id);
                    id->declared_ref = id->global_qualified ? nullptr : names_declared_reference(id->symbol_id);
                    id->declared_ref_kind = detail::declared_ref_kind(id->declared_ref);
                    escape_if_outer_read(id->symbol_id);   // default-[&] body: outer reads escape
                    break;
                }
                case node_type::binary_expr: {
                    auto* b = static_cast<binary_expr*>(e);
                    walk_expr(b->left.get());
                    walk_expr(b->right.get());
                    break;
                }
                case node_type::unary_expr:
                    walk_expr(static_cast<unary_expr*>(e)->operand.get());
                    break;
                case node_type::assignment_expr: {
                    auto* a = static_cast<assignment_expr*>(e);
                    walk_expr(a->target.get());
                    walk_expr(a->value.get());
                    break;
                }
                case node_type::call_expr: {
                    auto* c = static_cast<call_expr*>(e);
                    walk_expr(c->callee.get());
                    walk_args(c->arguments);
                    break;
                }
                case node_type::member_expr: {
                    auto* m = static_cast<member_expr*>(e);
                    if (m->is_static && m->member == "execute_elapsed" && m->object && m->object->get_type() == node_type::identifier_expr &&
                        static_cast<identifier_expr*>(m->object.get())->name == "time") {
                        names_execute_elapsed = true;
                    }
                    m->on_caught = m->object && m->object->get_type() == node_type::identifier_expr &&
                                   std::find(catch_variables.begin(), catch_variables.end(), static_cast<identifier_expr*>(m->object.get())->symbol_id) != catch_variables.end();
                    walk_expr(m->object.get());
                    break;
                }
                case node_type::lambda_expr: {
                    auto* l = static_cast<lambda_expr*>(e);
                    for (const auto& cap : l->captures) { walk_expr(cap.initializer.get()); }   // init-captures evaluate in this scope
                    // Explicit by-ref captures escape the captured variable
                    for (const auto& cap : l->captures) {
                        if (cap.by_reference) {
                            if (cap.symbol_id == UINT64_MAX && symbolizer) {
                                cap.symbol_id = symbolizer->intern(std::string(cap.name));
                            }
                            escape(cap.symbol_id, true);
                        }
                    }
                    // Default-[&]: the boundary is the lambda's own scope (pushed by
                    // walk_function = scopes.size() at entry); outer reads in the body
                    // escape via escape_if_outer_read in the identifier case
                    const bool default_by_ref = l->default_capture == lambda_expr::capture_default::by_reference;
                    if (default_by_ref) {
                        by_ref_capture_boundaries.push_back(scopes.size());
                    }
                    walk_function(l->parameters, l->body.get(), nullptr,
                                  l->return_type && l->return_type->base_type == script_value_type::jai_reference_type);
                    if (default_by_ref) {
                        by_ref_capture_boundaries.pop_back();
                    }
                    break;
                }
                case node_type::new_expr:
                    walk_args(static_cast<new_expr*>(e)->arguments);
                    break;
                case node_type::include_expr:
                    walk_expr(static_cast<include_expr*>(e)->path_expr.get());
                    break;
                case node_type::import_expr:
                    walk_args(static_cast<import_expr*>(e)->path_exprs);
                    break;
                case node_type::ternary_expr: {
                    auto* t = static_cast<ternary_expr*>(e);
                    walk_expr(t->condition.get());
                    walk_expr(t->then_expression.get());
                    walk_expr(t->else_expression.get());
                    break;
                }
                case node_type::array_literal_expr:
                    for (const auto& el : static_cast<array_literal_expr*>(e)->elements) {
                        walk_expr(el.get());
                    }
                    break;
                case node_type::map_literal_expr:
                    for (const auto& [k, v] : static_cast<map_literal_expr*>(e)->entries) {
                        walk_expr(k.get());
                        walk_expr(v.get());
                    }
                    break;
                case node_type::throw_expr:
                    walk_expr(static_cast<throw_expr*>(e)->value.get());
                    break;
                case node_type::yield_expr:
                    walk_expr(static_cast<yield_expr*>(e)->value.get());
                    break;
                case node_type::cast_expr: {
                    auto* c = static_cast<cast_expr*>(e);
                    // cast<T&>(x) shares x's cell exactly like auto& r = x
                    if (c->by_reference && c->operand->get_type() == node_type::identifier_expr) {
                        escape(static_cast<identifier_expr*>(c->operand.get())->symbol_id);
                    }
                    walk_expr(c->operand.get());
                    break;
                }
                default:
                    break;   // literal/identifier/this/super: leaves
            }
        }

        void walk_stmt(statement* s) {
            if (!s) {
                return;
            }
            stack.guard(s->location);
            switch (s->get_type()) {
                case node_type::expression_stmt:
                    walk_expr(static_cast<expression_stmt*>(s)->expression.get());
                    break;
                case node_type::block_stmt: {
                    const size_t hidden = loop_variables.size();
                    const size_t handlers = catch_variables.size();
                    for (const auto& d : static_cast<block_stmt*>(s)->declarations) {
                        walk_stmt(d.get());
                    }
                    loop_variables.resize(hidden);
                    catch_variables.resize(handlers);
                    break;
                }
                case node_type::if_stmt: {
                    auto* i = static_cast<if_stmt*>(s);
                    walk_expr(i->condition.get());
                    walk_stmt(i->then_statement.get());
                    walk_stmt(i->else_statement.get());
                    break;
                }
                case node_type::while_stmt: {
                    auto* w = static_cast<while_stmt*>(s);
                    walk_expr(w->condition.get());
                    walk_stmt(w->body.get());
                    break;
                }
                case node_type::for_stmt: {
                    auto* f = static_cast<for_stmt*>(s);
                    const size_t outer = loop_variables.size();
                    f->fresh_binding = false;
                    walk_stmt(f->initializer.get());
                    if (f->initializer && f->initializer->get_type() == node_type::variable_decl) {
                        auto* d = static_cast<variable_decl*>(f->initializer.get());
                        const bool reference = d->type && d->type->base_type == script_value_type::jai_reference_type;
                        if (d->name_id != UINT64_MAX) { loop_variables.back() = {reference ? nullptr : f, d->name_id}; }
                    }
                    walk_expr(f->condition.get());
                    walk_expr(f->update.get());
                    walk_stmt(f->body.get());
                    loop_variables.resize(outer);
                    break;
                }
                case node_type::range_for_stmt: {
                    auto* r = static_cast<range_for_stmt*>(s);
                    if (!scopes.empty() && r->variable_name_id != UINT64_MAX) {
                        scopes.back().range_vars.emplace(r->variable_name_id, r);
                    }
                    walk_expr(r->container.get());
                    walk_stmt(r->body.get());
                    break;
                }
                case node_type::parallel_for_stmt: {
                    // The body is a synthesized worker function whose one (by-ref)
                    // parameter is the loop variable — walk it as a function body so
                    // its own ref-decl/escape analysis applies; nothing in it can
                    // escape-mark ENCLOSING locals (worker capture is env-resident by
                    // symbol, never by slot).
                    auto* pf = static_cast<parallel_for_stmt*>(s);
                    walk_expr(pf->container.get());
                    scopes.emplace_back();
                    ref_return_stack.push_back(0);
                    if (pf->loop_param.symbol_id == UINT64_MAX && symbolizer) {
                        pf->loop_param.symbol_id = symbolizer->intern(pf->loop_param.name);
                    }
                    scopes.back().params.emplace(pf->loop_param.symbol_id, &pf->loop_param);
                    walk_stmt(pf->body.get());
                    ref_return_stack.pop_back();
                    scopes.pop_back();
                    break;
                }
                case node_type::return_stmt: {
                    auto* r = static_cast<return_stmt*>(s);
                    if (!ref_return_stack.empty() && ref_return_stack.back()) {
                        // Ref-return producer: the operand binds as a reference; a
                        // bare-identifier operand escapes (its decl boxes from the start)
                        r->binds_reference = true;
                        escape_arg(r->value.get());
                    }
                    walk_expr(r->value.get());
                    break;
                }
                case node_type::try_stmt: {
                    auto* t = static_cast<try_stmt*>(s);
                    walk_stmt(t->try_block.get());
                    const size_t handlers = catch_variables.size();
                    if (t->catch_var_id != UINT64_MAX) { catch_variables.push_back(t->catch_var_id); }
                    walk_stmt(t->catch_block.get());
                    catch_variables.resize(handlers);
                    break;
                }
                case node_type::switch_stmt: {
                    auto* sw = static_cast<switch_stmt*>(s);
                    walk_expr(sw->condition.get());
                    for (const auto& c : sw->cases) {
                        walk_stmt(c.get());
                    }
                    walk_stmt(sw->default_case.get());
                    break;
                }
                case node_type::case_stmt: {
                    auto* c = static_cast<case_stmt*>(s);
                    walk_expr(c->value.get());
                    for (const auto& b : c->body) {
                        walk_stmt(b.get());
                    }
                    break;
                }
                case node_type::default_stmt:
                    for (const auto& b : static_cast<default_stmt*>(s)->body) {
                        walk_stmt(b.get());
                    }
                    break;
                case node_type::statement_decl:
                    walk_stmt(static_cast<statement_decl*>(s)->statement.get());
                    break;
                case node_type::variable_decl: {
                    auto* d = static_cast<variable_decl*>(s);
                    // Ref decl: the identifier source escapes (auto& x = y shares y's cell)
                    if (d->type && d->type->base_type == script_value_type::jai_reference_type &&
                        d->initializer && d->initializer->get_type() == node_type::identifier_expr) {
                        escape(static_cast<identifier_expr*>(d->initializer.get())->symbol_id);
                    }
                    walk_expr(d->initializer.get());
                    d->declared_ref_storage_enforced = false;
                    if (d->type && d->type->base_type == script_value_type::jai_reference_type && d->initializer &&
                        d->initializer->get_type() == node_type::identifier_expr && !static_cast<identifier_expr*>(d->initializer.get())->global_qualified) {
                        const type_info* referent = d->type->element_type().get();
                        const type_info* source = resolved_value_type(static_cast<identifier_expr*>(d->initializer.get())->symbol_id);
                        const uint8_t kind = detail::declared_ref_kind(referent);
                        d->declared_ref_storage_enforced = source && kind && kind != 0xFF && detail::declared_ref_kind(source) == kind;
                    }
                    if (!scopes.empty() && d->name_id != UINT64_MAX) {
                        scopes.back().vars.emplace(d->name_id, d);
                    }
                    if (d->name_id != UINT64_MAX) { loop_variables.push_back({nullptr, d->name_id}); }
                    // a typed chain's `var& name = <caught>`
                    if (d->type && d->type->base_type == script_value_type::jai_reference_type && d->initializer &&
                        d->initializer->get_type() == node_type::identifier_expr && d->name_id != UINT64_MAX &&
                        std::find(catch_variables.begin(), catch_variables.end(), static_cast<identifier_expr*>(d->initializer.get())->symbol_id) != catch_variables.end()) {
                        catch_variables.push_back(d->name_id);
                    }
                    break;
                }
                case node_type::function_decl: {
                    auto* fd = static_cast<function_decl*>(s);
                    walk_function(fd->parameters, fd->body.get(), &fd->initializers,
                                  fd->return_type && fd->return_type->base_type == script_value_type::jai_reference_type &&
                                  !fd->is_coroutine);
                    break;
                }
                case node_type::class_decl: {
                    auto* cd = static_cast<class_decl*>(s);
                    struct class_body_scope { size_t& depth; class_body_scope(size_t& d) : depth(d) { ++depth; } ~class_body_scope() { --depth; } } body_scope{class_depth};
                    for (const auto& m : cd->members) {
                        if (!m.declaration) {
                            continue;
                        }
                        if (m.declaration->get_type() == node_type::function_decl) {
                            walk_stmt(m.declaration.get());
                        } else if (m.declaration->get_type() == node_type::variable_decl) {
                            // Fields never become cells (identity-mode refs); only their
                            // initializer expressions can produce escapes
                            walk_expr(static_cast<variable_decl*>(m.declaration.get())->initializer.get());
                        }
                    }
                    break;
                }
                case node_type::namespace_decl:
                    for (const auto& d : static_cast<namespace_decl*>(s)->declarations) {
                        walk_stmt(d.get());
                    }
                    break;
                case node_type::expression_decl:
                    walk_expr(static_cast<expression_decl*>(s)->expression.get());
                    break;
                case node_type::destructuring_decl: {
                    auto* d = static_cast<destructuring_decl*>(s);
                    walk_expr(d->initializer.get());
                    if (!scopes.empty()) {
                        for (size_t i = 0; i < d->names.size(); ++i) {
                            if (d->names[i].second != UINT64_MAX) {
                                scopes.back().destructures.emplace(d->names[i].second, std::make_pair(d, i));
                            }
                        }
                    }
                    break;
                }
                case node_type::include_decl:
                    walk_expr(static_cast<include_decl*>(s)->path_expr.get());
                    break;
                case node_type::import_decl:
                    walk_expr(static_cast<import_decl*>(s)->path_expr.get());
                    break;
                default:
                    break;   // break/continue/fallthrough/enum: leaves
            }
        }

        void run(const std::vector<declaration_ptr>& program) {
            scopes.emplace_back();   // program scope: top-level decls = globals
            for (const auto& d : program) {
                walk_stmt(d.get());
            }
            scopes.pop_back();
        }
    };

} // namespace

void detail::release_operands(std::initializer_list<expression_ptr*> operands, std::vector<expression_ptr>* more) noexcept {
    std::vector<expression_ptr> pending;
    auto take = [&pending](expression_ptr& e) {
        if (!e || e.use_count() != 1) { return; }
        switch (e->get_type()) {
            case node_type::binary_expr: case node_type::unary_expr: case node_type::assignment_expr: case node_type::call_expr: case node_type::member_expr:
                pending.push_back(std::move(e));
                return;
            default:
                return;
        }
    };
    try {
        for (expression_ptr* operand : operands) { take(*operand); }
        if (more) { for (expression_ptr& argument : *more) { take(argument); } }
        while (!pending.empty()) {
            expression_ptr node = std::move(pending.back());
            pending.pop_back();
            switch (node->get_type()) {
                case node_type::binary_expr: { auto* b = static_cast<binary_expr*>(node.get()); take(b->left); take(b->right); break; }
                case node_type::unary_expr: take(static_cast<unary_expr*>(node.get())->operand); break;
                case node_type::assignment_expr: { auto* a = static_cast<assignment_expr*>(node.get()); take(a->target); take(a->value); break; }
                case node_type::call_expr: {
                    auto* c = static_cast<call_expr*>(node.get());
                    take(c->callee);
                    for (expression_ptr& argument : c->arguments) { take(argument); }
                    break;
                }
                case node_type::member_expr: take(static_cast<member_expr*>(node.get())->object); break;
                default: break;
            }
        }
    } catch (...) {}   // out of memory: what is left dies recursively
}

bool detail::run_ast_marking_passes(const std::vector<declaration_ptr>& declarations,
                                    string_symbolizer* symbolizer) {
	bool names_execute_elapsed = false;
	detail::program_slot_pass().run(declarations);   // ahead of the escape marking, which reads the slots
	{
		ref_escape_marker marker;
		marker.symbolizer = symbolizer;
		marker.run(declarations);
		names_execute_elapsed = marker.names_execute_elapsed;
	}
	detail::mark_transient_reads(declarations);
	return names_execute_elapsed;
}

parser::parser(const std::vector<token>& tokens, string_symbolizer* symbolizer, engine* eng, const std::unordered_set<std::string>& registeredTemplateTypes, const std::string& filename)
    : stream_(&tokens), filename_(filename), current_(0), registered_template_types_(registeredTemplateTypes), symbolizer_(symbolizer), engine_(eng) {
    if (!symbolizer_) {
        throw std::invalid_argument("parser requires a valid string_symbolizer");
    }
    if (!engine_) {
        throw std::invalid_argument("parser requires a valid engine");
    }
}

type_info_ptr parser::store_type_info(type_info&& info) {
    return detail::engine_access::get_type_info(*engine_, info);
}

bool parser::apply_using_declarations(const std::vector<token>& in, std::vector<token>& out) {
    bool any = false;
    for (const token& t : in) { if (t.type == token_type::identifier && t.lexeme == "using") { any = true; break; } }
    if (!any) return false;
    struct alias { std::string_view name; std::vector<token> target; };
    std::vector<alias> aliases;
    out.clear();
    out.reserve(in.size());
    const size_t n = in.size();
    auto statement_start = [&]() {
        if (out.empty()) return true;
        const token_type k = out.back().type;
        return k == token_type::semicolon || k == token_type::left_brace || k == token_type::right_brace;
    };
    for (size_t i = 0; i < n; ++i) {
        const token& t = in[i];
        if (t.type == token_type::identifier && t.lexeme == "using" && statement_start() && i + 1 < n && in[i + 1].type == token_type::identifier) {
            std::vector<alias> items;
            size_t j = i + 1;
            bool ok = true;
            while (true) {
                if (j >= n || in[j].type != token_type::identifier) { report_error("expected a qualified name after 'using' (using ns::name;)", j < n ? in[j] : t); ok = false; break; }
                const size_t start = j++;
                while (j + 1 < n && in[j].type == token_type::colon_colon && in[j + 1].type == token_type::identifier) { j += 2; }
                std::vector<token> target(in.begin() + static_cast<std::ptrdiff_t>(start), in.begin() + static_cast<std::ptrdiff_t>(j));
                const std::string_view last = target.back().lexeme;
                const size_t cut = last.rfind("::");
                if (target.size() == 1 && cut == std::string_view::npos) {
                    report_error("'using" + std::string(" ") + std::string(last) + "' names no namespace: a using declaration takes a qualified name (using ns::name;)", in[start]);
                    ok = false;
                    break;
                }
                items.push_back({cut == std::string_view::npos ? last : last.substr(cut + 2), std::move(target)});
                if (j < n && in[j].type == token_type::comma) { ++j; continue; }
                break;
            }
            if (ok && (j >= n || in[j].type != token_type::semicolon)) { report_error("expected ';' after the using declaration", j < n ? in[j] : t); ok = false; }
            if (ok) {
                for (auto& a : items) {
                    const bool taken = std::any_of(aliases.begin(), aliases.end(), [&](const alias& x) { return x.name == a.name; });
                    if (taken) { report_error("'" + std::string(a.name) + "' is already named by a using declaration", a.target.back()); continue; }
                    aliases.push_back(std::move(a));
                }
            }
            while (j < n && in[j].type != token_type::semicolon && in[j].type != token_type::eof) { ++j; }
            i = j < n && in[j].type == token_type::semicolon ? j : j - 1;
            continue;
        }
        if (t.type == token_type::identifier && !aliases.empty()) {
            const token_type before = out.empty() ? token_type::eof : out.back().type;
            // after '.', '?.' or '::' a name is a member; after '->' it is a member unless a body or a reference follows (a return type)
            const token_type after = i + 1 < n ? in[i + 1].type : token_type::eof;
            const bool return_type = before == token_type::arrow && (after == token_type::left_brace || after == token_type::ampersand || after == token_type::semicolon);
            if (before != token_type::dot && (before != token_type::arrow || return_type) && before != token_type::colon_colon && before != token_type::question_dot) {
                auto a = std::find_if(aliases.begin(), aliases.end(), [&](const alias& x) { return x.name == t.lexeme; });
                if (a != aliases.end()) {
                    for (size_t k = 0; k < a->target.size(); ++k) {
                        token copy = a->target[k];
                        copy.location = t.location;
                        if (k == 0) { copy.using_name = a->name; }
                        out.push_back(std::move(copy));
                    }
                    continue;
                }
            }
        }
        out.push_back(t);
    }
    return true;
}

namespace {
    // The block a token sits in: a namespace's (its own path and the index of the namespace around it), a class body's (its bare
    // name) or any other. A qualified name is built only when a lookup needs it, so nesting costs no more than its depth.
    struct name_scope { enum kind_t { other, space, klass } kind; std::string name; size_t outer = SIZE_MAX; };

    struct name_scope_walk {
        std::vector<name_scope> scopes;
        name_scope pending{name_scope::other, {}};

        size_t innermost_namespace() const {
            for (size_t k = scopes.size(); k-- > 0;) { if (scopes[k].kind == name_scope::space) { return k; } }
            return SIZE_MAX;
        }
        std::string qualified(size_t k) const {
            std::vector<const std::string*> parts;
            for (; k != SIZE_MAX; k = scopes[k].outer) { parts.push_back(&scopes[k].name); }
            std::string result;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                if (!result.empty()) { result += "::"; }
                result += **it;
            }
            return result;
        }
        // the parser's namespace-name rule: any token but these
        static bool namespace_name_token(token_type k) {
            return k != token_type::eof && k != token_type::left_brace && k != token_type::right_brace && k != token_type::left_paren &&
                   k != token_type::right_paren && k != token_type::semicolon && k != token_type::colon_colon;
        }
        void step(const std::vector<token>& in, size_t i) {
            const token& t = in[i];
            switch (t.type) {
                case token_type::namespace_keyword: {
                    std::string path;
                    size_t j = i + 1;
                    while (j < in.size() && namespace_name_token(in[j].type)) {
                        path += in[j].lexeme;
                        if (j + 1 < in.size() && in[j + 1].type == token_type::colon_colon) { path += "::"; j += 2; } else { ++j; break; }
                    }
                    if (j < in.size() && in[j].type == token_type::left_brace && !path.empty()) {
                        pending = {name_scope::space, std::move(path), innermost_namespace()};
                    }
                    break;
                }
                case token_type::class_keyword:
                    if (i + 1 < in.size() && in[i + 1].type == token_type::identifier) { pending = {name_scope::klass, std::string(in[i + 1].lexeme)}; }
                    break;
                case token_type::semicolon:
                    pending = {name_scope::other, {}};
                    break;
                case token_type::left_brace:
                    scopes.push_back(std::move(pending));
                    pending = {name_scope::other, {}};
                    break;
                case token_type::right_brace:
                    if (!scopes.empty()) { scopes.pop_back(); }
                    break;
                default:
                    break;
            }
        }
    };
}

bool parser::qualify_namespace_classes(const std::vector<token>& in, std::vector<token>& out) {
    if (std::none_of(in.begin(), in.end(), [](const token& t) { return t.type == token_type::namespace_keyword; })) return false;
    std::unordered_map<std::string, std::unordered_set<std::string_view>> declared;   // namespace -> the classes this stream declares in it
    std::unordered_set<std::string_view> declared_anywhere;
    {
        name_scope_walk walk;
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i].type == token_type::class_keyword && i + 1 < in.size() && in[i + 1].type == token_type::identifier &&
                !walk.scopes.empty() && walk.scopes.back().kind == name_scope::space) {
                declared[walk.qualified(walk.scopes.size() - 1)].insert(in[i + 1].lexeme);
                declared_anywhere.insert(in[i + 1].lexeme);
            }
            walk.step(in, i);
        }
    }
    auto& spaces = detail::engine_access::script_namespaces(*engine_);
    auto names_class = [&](const std::string& space, std::string_view name) {
        if (auto d = declared.find(space); d != declared.end() && d->second.count(name)) { return true; }
        namespace_queries_.insert(space + "::" + std::string(name));
        const auto space_id = symbolizer_->find(space);
        const auto name_id = space_id ? symbolizer_->find(name) : std::nullopt;
        if (!name_id) { return false; }
        auto ns = spaces.find(*space_id);
        return ns != spaces.end() && ns->second && ns->second->classes.count(*name_id) != 0;
    };
    // a name no namespace declares as a class needs no qualified name built
    auto some_namespace_class = [&](std::string_view name) {
        if (declared_anywhere.count(name)) { return true; }
        namespace_queries_.emplace(name);
        const auto name_id = symbolizer_->find(name);
        if (!name_id) { return false; }
        return std::any_of(spaces.begin(), spaces.end(), [&](const auto& ns) { return ns.second && ns.second->classes.count(*name_id) != 0; });
    };
    // the innermost namespace that declares the name, its enclosing ones after it (C++ lookup); empty when none does
    auto qualifier_for = [&](const name_scope_walk& walk, std::string_view name) -> std::string {
        const size_t innermost = walk.innermost_namespace();
        if (innermost == SIZE_MAX || !some_namespace_class(name)) { return {}; }
        for (std::string space = walk.qualified(innermost);;) {   // every enclosing namespace's name is a prefix of the innermost's
            if (names_class(space, name)) { return space; }
            const size_t cut = space.rfind("::");
            if (cut == std::string::npos) { break; }
            space.resize(cut);
        }
        return {};
    };
    out.clear();
    out.reserve(in.size());
    name_scope_walk walk;
    bool any = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const token& t = in[i];
        if (t.type == token_type::identifier && !walk.scopes.empty()) {
            const token_type before = out.empty() ? token_type::eof : out.back().type;
            const token_type after = i + 1 < in.size() ? in[i + 1].type : token_type::eof;
            const bool return_type = before == token_type::arrow && (after == token_type::left_brace || after == token_type::ampersand || after == token_type::semicolon);
            const bool member_name = before == token_type::dot || before == token_type::question_dot || before == token_type::colon_colon ||
                                     (before == token_type::arrow && !return_type);
            const bool declared_name = before == token_type::class_keyword || before == token_type::tilde || before == token_type::namespace_keyword;
            // ClassName( at a member's start in its own body is the constructor, named by the class's own spelling
            const bool after_visibility = before == token_type::colon && out.size() >= 2 &&
                (out[out.size() - 2].type == token_type::public_keyword || out[out.size() - 2].type == token_type::private_keyword ||
                 out[out.size() - 2].type == token_type::protected_keyword);
            const bool constructor = walk.scopes.back().kind == name_scope::klass && walk.scopes.back().name == t.lexeme &&
                after == token_type::left_paren && (before == token_type::left_brace || before == token_type::semicolon ||
                                                    before == token_type::right_brace || after_visibility);
            if (!member_name && !declared_name && !constructor) {
                if (const std::string space = qualifier_for(walk, t.lexeme); !space.empty()) {
                    for (size_t start = 0;;) {
                        const size_t cut = space.find("::", start);
                        const std::string_view segment = std::string_view(space).substr(start, cut == std::string::npos ? std::string::npos : cut - start);
                        const auto [segment_id, segment_view] = symbolizer_->intern_with_view(segment);
                        out.emplace_back(token_type::identifier, segment_view, segment_id, t.location);
                        out.emplace_back(token_type::colon_colon, "::", t.location);
                        if (cut == std::string::npos) { break; }
                        start = cut + 2;
                    }
                    out.push_back(t);
                    any = true;
                    continue;
                }
            }
        }
        out.push_back(t);
        walk.step(in, i);
    }
    return any;
}

// A declaration's name: never a using name (the name stands for its qualified spelling) nor a qualified spelling
bool parser::reject_using_name(const token& t, const char* what) {
    if (t.type != token_type::identifier) return false;
    if (!t.using_name.empty()) {
        // the using name expanded to its qualified spelling's tokens in place: t is the first, the rest follow it
        std::string qualified(t.lexeme);
        if (&t == &peek()) {
            for (size_t k = current_ + 1; k + 1 < stream_->size() && (*stream_)[k].type == token_type::colon_colon &&
                                          (*stream_)[k + 1].type == token_type::identifier; k += 2) {
                qualified += "::" + std::string((*stream_)[k + 1].lexeme);
            }
        }
        report_error("'" + std::string(t.using_name) + "' names " + qualified +
                     " by a using declaration; a " + std::string(what) + " cannot be declared with that name", t);
        return true;
    }
    if (t.lexeme.find("::") != std::string_view::npos) {
        report_error("'" + std::string(t.lexeme) + "' is a qualified name; a " + std::string(what) + " cannot be declared with it", t);
        return true;
    }
    return false;
}

void parser::check_reload_hook(const jai::declaration& d, bool& seen) {
    static const char* const k_signature = "void hot_reload_migrate(map<string, var> previous)";
    if (d.get_type() == node_type::function_decl) {
        const auto& f = static_cast<const function_decl&>(d);
        if (f.name != "hot_reload_migrate") return;
        const type_info* previous = detail::engine_access::get_type_info_map(*engine_, detail::engine_access::get_type_info_string(*engine_), detail::engine_access::get_type_info_any(*engine_));
        const bool shaped = !f.is_coroutine && f.parameters.size() == 1 && f.parameters[0].type && f.parameters[0].type->id == previous->id &&
                            !f.parameters[0].is_reference && !f.parameters[0].default_value &&
                            (!f.return_type || f.return_type->id == detail::engine_access::get_type_info_void(*engine_)->id);
        if (!shaped) {
            report_error_at(std::string("hot_reload_migrate at a unit's top level is its reload hook, declared ") + k_signature, f.location);
        } else if (seen) {
            report_error_at("a unit declares its reload hook hot_reload_migrate once", f.location);
        }
        seen = true;
    } else if (d.get_type() == node_type::variable_decl && static_cast<const variable_decl&>(d).name == "hot_reload_migrate") {
        report_error_at(std::string("'hot_reload_migrate' at a unit's top level names its reload hook, declared ") + k_signature, d.location);
    }
}

checked_result<std::vector<declaration_ptr>> parser::parse() {
    stack_.arm();
    paren_close_.clear();
    std::vector<declaration_ptr> declarations;
    bool reload_hook_seen = false;
    if (qualify_namespace_classes(*stream_, namespace_stream_)) { stream_ = &namespace_stream_; }
    if (apply_using_declarations(*stream_, using_stream_)) { stream_ = &using_stream_; }

    try {
        while (!is_at_end()) {
            const size_t errors_before = errors_.size();
            auto decl_result = declaration();

            if (!decl_result) {
                // an error that bypassed report_error still gets its diagnostic
                if (errors_.size() == errors_before && decl_result.error() != std::error_code()) {
                    report_error(format_error(decl_result, *symbolizer_), peek());
                }
                synchronize();
            } else {
                auto decl = std::move(decl_result.value());
                if (decl) {
                    check_reload_hook(*decl, reload_hook_seen);
                    declarations.push_back(std::move(decl));
                }
            }
        }
    } catch (const detail::nesting_exhausted& e) {
        report_error_at(std::string(detail::k_nesting_too_deep_listing), e.location());
    }

    if (!errors_.empty()) {
        recovered_ = std::move(declarations);
        return failed_parse();
    }

    // Escape-mark + transient-read passes (shared with jaibite loads, which must
    // re-derive the parser-pass flags the serializer does not carry)
    try {
        if (detail::run_ast_marking_passes(declarations, symbolizer_)) { detail::engine_access::note_execute_elapsed_use(*engine_); }
    } catch (const detail::nesting_exhausted& e) {
        report_error_at(std::string(detail::k_nesting_too_deep_listing), e.location());
        return failed_parse();
    }

    // Mark the last expression declaration as an implicit return
    if (!declarations.empty()) {
        auto& last_decl = declarations.back();
        if (last_decl->get_type() == node_type::expression_decl) {
            static_cast<expression_decl*>(last_decl.get())->implicit_return = true;
        } else if (last_decl->get_type() == node_type::statement_decl) {
            auto* stmt_decl = static_cast<statement_decl*>(last_decl.get());
            // If the last declaration is a statement_decl containing a block,
            // mark the last expression in that block as implicit return
            if (stmt_decl->statement->get_type() == node_type::block_stmt) {
                auto* block = static_cast<block_stmt*>(stmt_decl->statement.get());
                if (!block->declarations.empty()) {
                    if (block->declarations.back()->get_type() == node_type::expression_decl) {
                        static_cast<expression_decl*>(block->declarations.back().get())->implicit_return = true;
                    }
                }
            }
            // If the last declaration is a statement_decl containing an expression_stmt,
            // mark it as implicit return by setting a flag on the statement_decl
            else if (stmt_decl->statement->get_type() == node_type::expression_stmt) {
                auto* expr_stmt = static_cast<expression_stmt*>(stmt_decl->statement.get());
                // We need to mark this expression statement as an implicit return
                // Since expression_stmt doesn't have implicit_return flag, we need to convert
                // this statement_decl to an expression_decl
                auto expr_decl = std::make_shared<expression_decl>(
                    expr_stmt->expression->location,
                    expr_stmt->expression
                );
                expr_decl->implicit_return = true;
                declarations.back() = expr_decl;
            }
        }
    }

    return declarations;
}

// Error handling - logs error to errors_ vector for later reporting
void parser::report_error(const std::string& message, const token& token) {
    report_error_at(message, token.location);
}

void parser::report_error_at(const std::string& message, const source_location& at) {
    errors_.push_back({at, message});
}

std::vector<std::string> parser::get_errors() const {
    std::vector<std::string> result;
    result.reserve(errors_.size());
    for (const auto& e : errors_) { result.push_back(e.at.to_string() + ": " + e.message); }
    return result;
}

// The static listing's shape (check_report::format): each error at its file, line and column, then, for the parsed text's own
// file, the line with a caret under the column
checked_result<std::vector<declaration_ptr>> parser::failed_parse() {
    const std::string& own_file = stream_->empty() ? filename_ : stream_->front().location.filename;
    std::string listing = "parse failed:";
    for (const auto& e : errors_) {
        check_report one;
        one.diagnostics.push_back({check_diagnostic::level::error, e.at.line, e.at.column, e.message});
        listing += '\n';
        listing += one.format(e.at.filename == own_file ? source_ : std::string_view{}, e.at.filename);
        if (listing.back() == '\n') { listing.pop_back(); }
    }
    return checked_result<std::vector<declaration_ptr>>(make_error_code(parse_error_code::syntax_error), "{0}", symbolizer_->intern(listing));
}

void parser::synchronize() {
    advance();
    
    while (!is_at_end()) {
        if (previous().type == token_type::semicolon) return;
        
        switch (peek().type) {
            case token_type::class_keyword:
            case token_type::extern_keyword:
            case token_type::inline_keyword:
            case token_type::auto_keyword:
            case token_type::var_keyword:
            case token_type::int_keyword:
            case token_type::float_keyword:
            case token_type::string_keyword:
            case token_type::bool_keyword:
            case token_type::char_keyword:
            case token_type::void_keyword:
            case token_type::if_keyword:
            case token_type::while_keyword:
            case token_type::for_keyword:
            case token_type::return_keyword:
            case token_type::try_keyword:
                return;
            default:
                advance();
        }
    }
}

namespace {
    bool is_type_keyword_token(token_type type) {
        switch (type) {
            case token_type::auto_keyword:
            case token_type::var_keyword:
            case token_type::int_keyword:
            case token_type::float_keyword:
            case token_type::string_keyword:
            case token_type::bool_keyword:
            case token_type::char_keyword:
            case token_type::void_keyword:
            case token_type::function_keyword:
            case token_type::array_keyword:
            case token_type::map_keyword:
            case token_type::weak_ptr_keyword:
            case token_type::shared_ptr_keyword:
                return true;
            default:
                return false;
        }
    }

    // A reserved word is a name where a qualifier or a member declaration makes it one: after '.', '?.', '->' or '::', and as
    // the declared name of a class member, a namespace member or an enum value; bare, it stays the keyword
    bool is_keyword_word(token_type type) {
        return (type >= token_type::bool_keyword && type <= token_type::extern_keyword) || type == token_type::inline_keyword;
    }

    // this and super name the receiver and its base, so neither is declared as a member
    bool is_declarable_keyword_name(token_type type) {
        return is_keyword_word(type) && type != token_type::this_keyword && type != token_type::super_keyword;
    }

    // a keyword as a member declaration's name: followed by '(' (a method), '=' or ';' (a field)
    bool keyword_member_name_at(const std::vector<token>& stream, size_t index) {
        if (index + 1 >= stream.size() || !is_declarable_keyword_name(stream[index].type)) { return false; }
        const token_type next = stream[index + 1].type;
        return next == token_type::left_paren || next == token_type::equal || next == token_type::semicolon;
    }

    std::string type_keyword_name_message(std::string_view lexeme, const char* what) {
        std::string name(lexeme);
        return "'" + name + "' is a type keyword and cannot be used as a " + std::string(what) +
               " name - rename it (e.g. '" + name + "name')";
    }

    // JSON-style map keys: one single token immediately followed by ':'. Literal keys
    // beyond identifier/string are legal (int/float/char/bool - the runtime map model is
    // key-type-agnostic); the set must stay single-token so block-vs-map lookahead never
    // has to parse an expression. Used by BOTH detection sites (looks_like_map_literal
    // and parse_map_literal's probe) so {1: "a"} and {"s": 0, 1: "a"} agree.
    bool is_json_map_key_token(token_type type) {
        switch (type) {
            case token_type::identifier:
            case token_type::string_literal:
            case token_type::integer_literal:
            case token_type::float_literal:
            case token_type::char_literal:
            case token_type::true_keyword:
            case token_type::false_keyword:
                return true;
            default:
                return false;
        }
    }
}

void parser::report_expected_name(const std::string& expected_message, const char* what) {
    if (std::string_view(what) == "member" && (check(token_type::this_keyword) || check(token_type::super_keyword))) {
        report_error("'" + std::string(check(token_type::this_keyword) ? "this" : "super") + "' is " +
                     (check(token_type::this_keyword) ? "the receiver" : "the base class") + " and cannot be used as a " + what + " name", peek());
    } else if (is_type_keyword_token(peek().type)) {
        report_error(type_keyword_name_message(peek().lexeme, what), peek());
    } else {
        report_error(expected_message, peek());
    }
}

checked_result<token> parser::consume_name(const std::string& expected_message, const char* what) {
    if (check(token_type::identifier) && reject_using_name(peek(), what)) {
        return make_error_code(parse_error_code::syntax_error);
    }
    if (!check(token_type::identifier) && is_type_keyword_token(peek().type)) {
        return consume(token_type::identifier, type_keyword_name_message(peek().lexeme, what));
    }
    return consume(token_type::identifier, expected_message);
}

void parser::synchronize_class_member(size_t class_open_index) {
    // Brace-aware recovery for the class-member loop: plain synchronize() stops at the
    // first ';' INSIDE an aborted method body, so the member loop resumed mid-body,
    // mistook the method's '}' for the class's, and leaked every later member to top
    // level (spurious errors miles downstream). Re-derive the brace depth from the token
    // array (immune to lookahead/pushback), then skip to the next member boundary at
    // class-body level - or stop AT the class's own '}' so the loop closes the class
    // where it really ends.
    int depth = 1;
    for (size_t i = class_open_index + 1; i < current_ && i < stream_->size(); ++i) {
        if ((*stream_)[i].type == token_type::left_brace) { ++depth; }
        else if ((*stream_)[i].type == token_type::right_brace) { --depth; }
    }
    const size_t entry = current_;
    while (!is_at_end()) {
        if (depth <= 0) { return; }
        if (depth == 1) {
            if (check(token_type::right_brace)) { return; }
            if (current_ > entry &&
                (previous().type == token_type::semicolon || previous().type == token_type::right_brace)) {
                return;
            }
        }
        const token& consumed = advance();
        if (consumed.type == token_type::left_brace) { ++depth; }
        else if (consumed.type == token_type::right_brace) { --depth; }
    }
}

// token access returns references: a token copy copies its strings
const token& parser::peek() const {
    if (pushed_back_token_.has_value()) {
        return pushed_back_token_.value();
    }
    return (*stream_)[current_];
}

const token& parser::previous() const {
    return (*stream_)[current_ - 1];
}

const token& parser::advance() {
    if (pushed_back_token_.has_value()) {
        // Move pushed_back token to last_advanced_ so we can return a reference
        last_advanced_ = std::move(pushed_back_token_.value());
        pushed_back_token_.reset();
        return last_advanced_;
    }
    if (!is_at_end()) current_++;
    return previous();
}

bool parser::is_at_end() const {
    return peek().type == token_type::eof;
}

bool parser::check(token_type type) const {
    if (is_at_end()) return false;
    return peek().type == type;
}

bool parser::match(token_type type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool parser::match(std::initializer_list<token_type> types) {
    for (token_type type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

checked_result<token> parser::consume(token_type type, std::string_view message) {
    if (check(type)) return advance();
    return expected_token(message);
}

checked_result<void> parser::expect(token_type type, std::string_view message) {
    if (!check(type)) [[unlikely]] { return expected_token(message); }
    advance();
    return {};
}

JAI_NOINLINE error_propagator parser::expected_token(std::string_view message) {
    report_error(std::string(message), peek());
    return {make_error_code(parse_error_code::syntax_error), "expected token: {0}", symbolizer_->intern(message)};
}

// Primary expressions: the productions that nest first, each in its own frame, so a level of nesting holds none of the others' locals
checked_result<expression_ptr> parser::primary() {
    switch (peek().type) {
        case token_type::left_brace:
            if (looks_like_map_literal()) { return parse_map_literal(); }
            break;
        case token_type::left_paren:
            if (arrow_function_ahead()) { return arrow_function_expression(); }
            advance();
            return parenthesized();
        case token_type::left_bracket:
            advance();
            return bracket_expression();
        case token_type::cast_keyword:
            advance();
            return cast_expression();
        case token_type::identifier:
            if (arrow_function_ahead()) { return arrow_function_expression(); }
            break;
        case token_type::function_keyword:
            if (current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::left_paren) {
                advance();
                return anonymous_function_expression();
            }
            break;
        default:
            break;
    }
    return primary_rest();
}

JAI_NOINLINE checked_result<expression_ptr> parser::primary_rest() {
    // Lexer errors travel as error tokens carrying the message in the lexeme
    // (e.g. template-string format specs rejected at lex time). Surface that
    // message instead of a generic "expected expression".
    if (check(token_type::error)) {
        report_error(std::string(peek().lexeme), peek());
        advance();
        return make_error_code(parse_error_code::syntax_error);
    }

    if (check(token_type::left_brace) && current_ + 2 < stream_->size() && (*stream_)[current_ + 1].type == token_type::string_literal) {
        const token& after_key = (*stream_)[current_ + 2];
        if (after_key.type != token_type::comma && after_key.type != token_type::right_brace) {
            report_error("expected ':' after the map key", after_key);
            return make_error_code(parse_error_code::syntax_error);
        }
    }

    if (arrow_function_ahead()) {
        return arrow_function_expression();
    }
    if (check(token_type::identifier) && peek().lexeme == "const_cast" && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::less) {
        report_foreign(detail::foreign_word::const_cast_call, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    if (check(token_type::const_keyword)) {
        report_foreign(detail::foreign_word::const_misplaced, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    if (check(token_type::function_keyword) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::star) {
        report_foreign(detail::foreign_word::generator_function, peek());
        return make_error_code(parse_error_code::syntax_error);
    }

    // Anonymous function expression: `function (params) {...}` (a [&] lambda, the JavaScript closure).
    // `function name...` never reaches here (declaration() routes it first).
    if (check(token_type::function_keyword) &&
        current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::left_paren) {
        advance(); // consume 'function'
        return anonymous_function_expression();
    }

    // Literals
    if (match(token_type::true_keyword)) {
        script_value val(script_value::ast_literal_tag{}, true);
        return std::make_shared<literal_expr>(previous().location, val);
    }

    if (match(token_type::false_keyword)) {
        script_value val(script_value::ast_literal_tag{}, false);
        return std::make_shared<literal_expr>(previous().location, val);
    }

    if (match(token_type::null_keyword)) {
        script_value val(script_value::ast_literal_tag{}, std::monostate{});
        return std::make_shared<literal_expr>(previous().location, val);
    }

    if (match(token_type::integer_literal)) {
        token token = previous();
        script_value val(script_value::ast_literal_tag{}, token.int_value);
        return std::make_shared<literal_expr>(token.location, val);
    }

    if (match(token_type::float_literal)) {
        token token = previous();
        script_value val(script_value::ast_literal_tag{}, token.float_value);
        return std::make_shared<literal_expr>(token.location, val);
    }

    if (match(token_type::string_literal)) {
        token token = previous();
        script_value val(script_value::ast_literal_tag{}, token.string_value);
        return std::make_shared<literal_expr>(token.location, val);
    }

    if (match(token_type::char_literal)) {
        token token = previous();
        script_value val(script_value::ast_literal_tag{}, token.char_value);
        return std::make_shared<literal_expr>(token.location, val);
    }

    // Keywords
    if (!template_barriers_.empty() && (check(token_type::this_keyword) || check(token_type::super_keyword))) {
        report_error("a template sees only the names passed to it and the program's names; '" + std::string(check(token_type::this_keyword) ? "this" : "super") +
                     "' is not one of them (pass a local instead)", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    if (match(token_type::this_keyword)) {
		check_instance_capture(previous());
        return std::make_shared<this_expr>(previous().location);
    }

    if (match(token_type::super_keyword)) {
		check_instance_capture(previous());
        // super:: must be followed by a method name
        token superToken = previous();
        if (superToken.lexeme != "super::") {
            report_error(check(token_type::left_paren)
                ? "super(...) constructs the base only in a constructor's initializer list (: super(args)); call a base method with super::name(args)"
                : "a base class method is called with super::name(args), not super.name", superToken);
            return make_error_code(parse_error_code::syntax_error);
        }
        JAISCRIPT_TRY_ASSIGN(token methodName, is_keyword_word(peek().type) ? checked_result<token>(advance())
                                                                            : consume(token_type::identifier, "expected method name after 'super::'"));

        // Create a member access on super
        auto superExpr = std::make_shared<super_expr>(superToken.location);
        uint64_t member_id = get_symbol_id(methodName);
        return std::make_shared<member_expr>(methodName.location, superExpr, methodName.lexeme, member_id, false);
    }

    // include in expression position: evaluates to the included file's result value
    // (var cfg = include "config.jai"). Statement-position include is claimed earlier
    // by declaration(), so statement semantics are unchanged. Same three path forms as
    // the directive: "path", <path>, (expr).
    if (match(token_type::include_keyword)) {
        token includeToken = previous();
        if (match(token_type::left_paren)) {
            JAISCRIPT_TRY_ASSIGN(expression_ptr path_expr, expression());
            JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after include expression"));
            return std::make_shared<include_expr>(includeToken.location, path_expr);
        }
        JAISCRIPT_TRY_ASSIGN(std::string path, include_literal_path());
        return std::make_shared<include_expr>(includeToken.location, path);
    }

    if (template_call_ahead()) {
        if (peek().lexeme == "template") { return template_call(); }
        return scoped_template_call(peek().lexeme == "scoped_template_text");
    }

    // import(...) in expression position: true when a module (re)ran (`if (import(MODULES)) { ... }`)
    if (check(token_type::import_keyword) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::left_paren) {
        advance();
        const source_location at = previous().location;
        advance();
        return import_call(at);
    }

    // new T(args) / new T{args}: pure sugar for shared_ptr<T>(args) construction —
    // the same new_expr the shared_ptr<T>(...) spelling produces (idempotent when T
    // is already shared_ptr<...>)
    // make_shared<T>(args) is the same expression as new T(args); bare make_shared stays an ordinary name
    const bool make_shared_spelling = check(token_type::identifier) && peek().lexeme == "make_shared" &&
        current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::less;
    if (make_shared_spelling || match(token_type::new_keyword)) { return new_expression(make_shared_spelling); }

    // A math type (one fused token, lexer.cpp) in expression position is the static member math::K the math::
    // intrinsics resolve, so math::vec3(1, 2, 3) calls the constructor intrinsic
    if (check(token_type::identifier) && math_type_from_name(peek().lexeme) != script_value_type::jai_null_type) {
        token typeToken = advance();
        auto [math_id, math_view] = symbolizer_->intern_with_view("math");
        auto math_ident = std::make_shared<identifier_expr>(typeToken.location, math_view, math_id);
        math_ident->slot_index = lookup_slot(math_id);
        auto [kind_id, kind_view] = symbolizer_->intern_with_view(typeToken.lexeme.substr(6));
        return std::make_shared<member_expr>(typeToken.location, math_ident, kind_view, kind_id, false, true);
    }

    // Global-qualified identifier (C++ ::name): reach a global deliberately shadowed by
    // a class member. The identifier resolves GLOBAL-ENV-ONLY; the ordinary postfix loop
    // then applies, so ::f(1), ::hp = 5, and ::arr[i] compose naturally. Only the
    // prefix form is consumed here - infix :: (Class::member, ns::fn) parses postfix.
    if (check(token_type::colon_colon) && current_ + 1 < stream_->size() &&
        (*stream_)[current_ + 1].type == token_type::identifier) {
        advance();   // '::'
        token identToken = advance();
        auto id_expr = std::make_shared<identifier_expr>(identToken.location, identToken.lexeme, get_symbol_id(identToken));
        id_expr->global_qualified = true;
        return id_expr;
    }

    // Check if a keyword is being used as a namespace identifier (e.g., string::length)
    // This must come before identifier handling
    if (peek().type != token_type::identifier && peek().type != token_type::eof) {
        // Check if this token is followed by ::
        size_t lookAhead = current_ + 1;
        if (lookAhead < stream_->size() && (*stream_)[lookAhead].type == token_type::colon_colon) {
            // This is a keyword being used as a namespace name
            token nameToken = advance();
            auto id_expr = std::make_shared<identifier_expr>(nameToken.location, nameToken.lexeme, get_symbol_id(nameToken));
            resolve_name(*id_expr);
            return id_expr;
        }
    }

    // Identifiers (including potential template constructors)
    if (match(token_type::identifier)) { return identifier_expression(); }

    // Check for type constructors (array<int>(), map<string, int>(), etc.)
    if (check(token_type::array_keyword) || check(token_type::map_keyword) ||
        check(token_type::weak_ptr_keyword) || check(token_type::shared_ptr_keyword) ||
        check(token_type::int_keyword) || check(token_type::float_keyword) ||
        check(token_type::string_keyword) || check(token_type::bool_keyword) || check(token_type::char_keyword)) {
        if (auto constructed = type_constructor_expression()) { return std::move(*constructed); }
    }
    
    // Super keyword
    if (match(token_type::super_keyword)) {
        return std::make_shared<super_expr>(previous().location);
    }

    // Regular identifier (variable name)
    if (match(token_type::identifier)) {
        token name = previous();
        auto id_expr = std::make_shared<identifier_expr>(name.location, name.lexeme, get_symbol_id(name));
        resolve_name(*id_expr);
        return id_expr;
    }

    report_error("expected expression", peek());
    return make_error_code(parse_error_code::syntax_error);
}

bool parser::template_call_ahead() const {
    if (!check(token_type::identifier) || current_ + 1 >= stream_->size() || (*stream_)[current_ + 1].type != token_type::left_paren) { return false; }
    const std::string_view word = peek().lexeme;
    return word == "template" || word == "scoped_template" || word == "scoped_template_text";
}

// A name the innermost barriers do not admit: neither declared in the template's own scopes nor passed through every barrier
// between the use and its declaration. A scoped barrier admits its own names and ends the walk.
bool parser::template_hides(uint64_t symbol_id) const {
    size_t top = function_scope_stack_.size();
    for (size_t b = template_barriers_.size(); b-- > 0;) {
        const template_barrier& barrier = template_barriers_[b];
        for (size_t i = barrier.scope_count; i < top; ++i) {
            if (function_scope_stack_[i].symbol_to_slot.count(symbol_id)) { return false; }
        }
        if (std::find(barrier.names.begin(), barrier.names.end(), symbol_id) == barrier.names.end()) { return true; }
        if (barrier.scoped) { return false; }
        top = barrier.scope_count;
    }
    return false;
}

// A use of a name: its slot, or, in a template body that does not see the enclosing name, the program's name (::name). A
// trusted template's raw is its builtin.
void parser::resolve_name(identifier_expr& id) {
    if (!template_barriers_.empty() && template_hides(id.symbol_id)) {
        if (id.name == "raw" && !template_barriers_.back().scoped) {
            auto [raw_id, raw_view] = symbolizer_->intern_with_view(detail::template_library::k_raw);
            id.name = raw_view;
            id.symbol_id = raw_id;
        }
        id.global_qualified = true;
        id.slot_index = SIZE_MAX;
        return;
    }
    id.slot_index = lookup_slot(id.symbol_id);
}

expression_ptr parser::template_builtin(std::string_view name, const source_location& at, std::vector<expression_ptr> arguments) {
    auto [id, view] = symbolizer_->intern_with_view(name);
    auto callee = std::make_shared<identifier_expr>(at, view, id);
    callee->global_qualified = true;
    return std::make_shared<call_expr>(at, std::move(callee), std::move(arguments));
}

// A template's text parsed here, under `barrier`: its tokens replace the stream until the body ends
checked_result<expression_ptr> parser::template_text_body(const std::string& content, const std::string& file, template_barrier barrier) {
    lexer lex(content, symbolizer_, file);
    const std::vector<token> tokens = lex.tokenize_template(symbolizer_->intern(detail::template_library::k_joiner), symbolizer_->intern(detail::template_library::k_splice));
    const std::vector<token>* saved_stream = stream_;
    const size_t saved_current = current_;
    const std::optional<token> saved_pushed_back = pushed_back_token_;
    stream_ = &tokens; current_ = 0; pushed_back_token_.reset();
    template_barriers_.push_back(std::move(barrier));
    const size_t errors_before = errors_.size();
    auto body = expression();
    if (body && !is_at_end()) {
        report_error("unexpected text after the template body", peek());
    }
    template_barriers_.pop_back();
    stream_ = saved_stream; current_ = saved_current; pushed_back_token_ = saved_pushed_back;
    paren_close_.erase(&tokens);
    if (!body) {
        if (errors_.size() == errors_before) { report_error_at(format_error(body, *symbolizer_), source_location{file, 1, 1}); }
        return body.error_value();
    }
    if (errors_.size() != errors_before) { return make_error_code(parse_error_code::syntax_error); }
    return body;
}

// template(path, a, b, ...): a literal path renders here, in the caller, seeing a, b, ... by name and the program; any other
// path, or a file that is already being expanded (a template rendering itself), renders through template#dynamic
JAI_NOINLINE checked_result<expression_ptr> parser::template_call() {
    const token name = advance();
    advance();   // '('
    const source_location at = name.location;
    if (check(token_type::right_paren)) {
        report_error("template() takes a path, then the names the template sees", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    JAISCRIPT_TRY_ASSIGN(expression_ptr path, expression());
    std::vector<token> names;
    size_t position = 1;
    bool named = true;
    while (match(token_type::comma)) {
        ++position;
        const bool plain = check(token_type::identifier) && current_ + 1 < stream_->size() &&
            ((*stream_)[current_ + 1].type == token_type::comma || (*stream_)[current_ + 1].type == token_type::right_paren);
        if (plain) {
            const token& n = advance();
            for (const token& earlier : names) {
                if (earlier.lexeme == n.lexeme) { report_error("template() names '" + std::string(n.lexeme) + "' twice", n); named = false; }
            }
            names.push_back(n);
            continue;
        }
        const token bad = peek();
        JAISCRIPT_TRY(expression());
        report_error("template() argument " + std::to_string(position) +
                     " is not a plain variable name: the template sees each argument by its name, so name the value first (auto entry = rows[0];) and pass the name", bad);
        named = false;
    }
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after template arguments"));
    auto reported = [&]() -> checked_result<expression_ptr> {   // the error is listed; the parse goes on past the call
        return expression_ptr(std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, std::monostate{})));
    };
    if (!named) { return reported(); }

    std::vector<expression_ptr> passed;
    std::string spelled;
    template_barrier barrier;
    barrier.scope_count = function_scope_stack_.size();
    for (const token& n : names) {
        auto id = std::make_shared<identifier_expr>(n.location, n.lexeme, get_symbol_id(n));
        resolve_name(*id);
        barrier.names.push_back(id->symbol_id);
        if (!spelled.empty()) { spelled += ','; }
        spelled += n.lexeme;
        passed.push_back(std::move(id));
    }
    auto literal_text = [&](const std::string& text) {
        return std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, text));
    };
    auto dynamic = [&](expression_ptr path_value) -> expression_ptr {
        std::vector<expression_ptr> load;
        load.push_back(std::move(path_value));
        load.push_back(literal_text(spelled));
        load.push_back(literal_text(at.filename));
        return std::make_shared<call_expr>(at, template_builtin(detail::template_library::k_dynamic, at, std::move(load)), std::move(passed));
    };
    const bool literal = path->get_type() == node_type::literal_expr &&
        static_cast<literal_expr*>(path.get())->value.raw_storage_index() == script_value::TYPEID_STRING;
    if (!literal) { return dynamic(std::move(path)); }
    const std::string spelled_path(static_cast<literal_expr*>(path.get())->value.unchecked_as_string());
    if (!engine_->is_enabled(privilege::filesystem)) { return dynamic(std::move(path)); }   // the render raises the privilege error, as include does
    auto resolved = resolve_include_path(spelled_path, engine_, at.filename);
    if (!resolved) {
        report_error("template file '" + spelled_path + "' not found", name);
        return reported();
    }
    if (std::find(template_chain_.begin(), template_chain_.end(), resolved.value()) != template_chain_.end() || template_chain_.size() >= 32) {
        return dynamic(literal_text(resolved.value()));
    }
    std::ifstream file(resolved.value(), std::ios::binary);
    if (!file) {
        report_error("template file '" + spelled_path + "' cannot be read", name);
        return reported();
    }
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    spliced_files_.push_back(resolved.value());
    template_chain_.push_back(resolved.value());
    auto body = template_text_body(content, resolved.value(), std::move(barrier));
    template_chain_.pop_back();
    if (!body) { return reported(); }
    return body;
}

// scoped_template(path, map) / scoped_template_text(text, map): a literal source is checked here against the scoped rules, and
// the call renders its text (kept in the call, so the program runs the version it loaded) through template#scoped
JAI_NOINLINE checked_result<expression_ptr> parser::scoped_template_call(bool text_form) {
    const token name = advance();
    advance();   // '('
    const source_location at = name.location;
    const std::string spelling = text_form ? "scoped_template_text(text, map)" : "scoped_template(path, map)";
    std::vector<expression_ptr> arguments;
    if (!check(token_type::right_paren)) {
        do {
            JAISCRIPT_TRY_ASSIGN(expression_ptr arg, expression());
            arguments.push_back(std::move(arg));
        } while (match(token_type::comma));
    }
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after " + spelling.substr(0, spelling.find('(')) + " arguments"));
    auto reported = [&]() -> checked_result<expression_ptr> {   // the error is listed; the parse goes on past the call
        return expression_ptr(std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, std::monostate{})));
    };
    if (arguments.size() != 2) {
        report_error(spelling + " takes 2 arguments, got " + std::to_string(arguments.size()), name);
        return reported();
    }
    auto literal_text = [&](const std::string& text) {
        return std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, text));
    };
    const bool literal = arguments[0]->get_type() == node_type::literal_expr &&
        static_cast<literal_expr*>(arguments[0].get())->value.raw_storage_index() == script_value::TYPEID_STRING;
    std::vector<expression_ptr> call;
    if (!literal) {
        call.push_back(std::move(arguments[0]));
        call.push_back(literal_text(text_form ? std::string(detail::template_library::k_text_label) : at.filename));
        call.push_back(std::move(arguments[1]));
        return template_builtin(text_form ? detail::template_library::k_scoped : detail::template_library::k_scoped_file, at, std::move(call));
    }
    const std::string source(static_cast<literal_expr*>(arguments[0].get())->value.unchecked_as_string());
    std::string content = source;
    std::string label(detail::template_library::k_text_label);
    if (!text_form) {
        if (!engine_->is_enabled(privilege::scoped_template)) {   // the render raises the privilege error, as include does
            call.push_back(std::move(arguments[0]));
            call.push_back(literal_text(at.filename));
            call.push_back(std::move(arguments[1]));
            return template_builtin(detail::template_library::k_scoped_file, at, std::move(call));
        }
        const auto found = detail::template_library::resolve_scoped_file(*engine_, source, at.filename);
        if (!found.error.empty()) {
            report_error(found.error, name);
            return reported();
        }
        const std::string& resolved_path = found.resolved;
        std::ifstream file(resolved_path, std::ios::binary);
        if (!file) {
            report_error("template file '" + source + "' cannot be read", name);
            return reported();
        }
        content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        spliced_files_.push_back(resolved_path);
        label = resolved_path;
    }
    template_barrier barrier;
    barrier.scope_count = function_scope_stack_.size();
    barrier.names.push_back(symbolizer_->intern("args"));
    barrier.scoped = true;
    auto body = template_text_body(content, label, std::move(barrier));
    if (!body) { return reported(); }
    std::vector<detail::template_library::violation> violations;
    detail::template_library::scoped_violations(*body.value(), *symbolizer_, violations);
    for (const auto& v : violations) { report_error_at(v.message, v.at); }
    if (!violations.empty()) { return reported(); }
    call.push_back(literal_text(content));
    call.push_back(literal_text(label));
    call.push_back(std::move(arguments[1]));
    return template_builtin(detail::template_library::k_scoped, at, std::move(call));
}

checked_result<expression_ptr> parser::parse_scoped_template_body() {
    template_barrier barrier;
    barrier.names.push_back(symbolizer_->intern("args"));
    barrier.scoped = true;
    template_barriers_.push_back(std::move(barrier));
    auto body = expression();
    if (body && !is_at_end()) { report_error("unexpected text after the template body", peek()); }
    template_barriers_.pop_back();
    if (body && errors_.empty()) {
        std::vector<detail::template_library::violation> violations;
        detail::template_library::scoped_violations(*body.value(), *symbolizer_, violations);
        for (const auto& v : violations) { report_error_at(v.message, v.at); }
    }
    if (!body && errors_.empty()) { report_error(format_error(body, *symbolizer_), peek()); }
    if (!errors_.empty()) {
        auto failed = failed_parse();
        return failed.error_value();
    }
    return body;
}

JAI_NOINLINE checked_result<expression_ptr> parser::new_expression(bool make_shared_spelling) {
    const token& newToken = make_shared_spelling ? advance() : previous();
    if (make_shared_spelling) { advance(); }
    JAISCRIPT_TRY_ASSIGN(type_info_ptr pointee_type, parse_type());
    if (make_shared_spelling) {
        consume_greater_in_generic("expected '>' after make_shared type");
        if (!check(token_type::left_paren)) {
            report_error("expected '(' after make_shared<T>", peek());
            return make_error_code(parse_error_code::syntax_error);
        }
    }

    type_info_ptr type = pointee_type;
    if (!pointee_type || pointee_type->base_type != script_value_type::jai_shared_ptr_type) { type = shared_ptr_type_of(pointee_type); }

    if (!check(token_type::left_paren) && !check(token_type::left_brace)) {
        report_foreign(check(token_type::left_bracket) ? detail::foreign_word::new_array : detail::foreign_word::new_without_arguments, newToken);
        return make_error_code(parse_error_code::syntax_error);
    }
    const bool is_brace = check(token_type::left_brace);
    advance(); // consume '(' or '{'
    JAISCRIPT_TRY_ASSIGN(auto arguments, construction_arguments(is_brace));
    return std::make_shared<new_expr>(newToken.location, type, std::move(arguments));
}

JAI_NOINLINE checked_result<expression_ptr> parser::identifier_expression() {
    token identToken = previous();
    
    // A host template's name followed by '<' may be a construction (Point<int>(1, 2)); a failed type parse is a comparison
    if (check(token_type::less) && is_registered_template_type(std::string(identToken.lexeme))) {
        size_t savedPos = current_ - 1; // Save position including the identifier
        current_--; // Back up to re-parse as type
        const size_t errors_before = errors_.size();
        const std::optional<token> pushed_before = pushed_back_token_;

        auto type_result = parse_type();
        if (type_result && errors_.size() == errors_before) {
            type_info_ptr type = std::move(type_result.value());
            if (type && check(token_type::left_brace)) {
                // This is a brace-initialized constructor like array<int>{}
                advance(); // consume '{'
                JAISCRIPT_TRY_ASSIGN(auto arguments, construction_arguments(true));
                return construction(type, std::move(arguments), true, identToken.location);
            } else if (type && check(token_type::left_paren)) {
                // Parentheses constructor for user-defined template types
                advance(); // consume '('
                JAISCRIPT_TRY_ASSIGN(auto arguments, construction_arguments(false));
                return construction(type, std::move(arguments), false, identToken.location);
            }
        }
        current_ = savedPos + 1;
        errors_.resize(errors_before);
        pushed_back_token_ = pushed_before;
    }

    auto id_expr = std::make_shared<identifier_expr>(identToken.location, identToken.lexeme, get_symbol_id(identToken));
    resolve_name(*id_expr);
    return id_expr;
}

JAI_NOINLINE std::optional<checked_result<expression_ptr>> parser::type_constructor_expression() {
    size_t savedPos = current_;
    auto type_result = parse_type();
    if (type_result) {
        type_info_ptr type = std::move(type_result.value());
        if (type) {
            // This is a type constructor - convert it to an identifier expression
            // that will be handled by postfix() for () or {} initialization
            current_ = savedPos;
            token typeToken = advance();
            std::string type_name(typeToken.lexeme);

            // For template types, we need to parse the full type
            if (check(token_type::less)) {
                // This is a template type, parse the full type
                current_ = savedPos;
                JAISCRIPT_TRY_ASSIGN(type, parse_type());

                // Check if this is followed by {} or () for constructor syntax
                if (check(token_type::left_brace) || check(token_type::left_paren)) {
                    const bool is_brace = check(token_type::left_brace);
                    advance(); // consume '{' or '('
                    JAISCRIPT_TRY_ASSIGN(auto arguments, construction_arguments(is_brace));
                    return construction(type, std::move(arguments), is_brace, typeToken.location);
                }

                // Otherwise return identifier with type name
                type_name = type->type_name;
            }

            auto [sym_id, stable_view] = symbolizer_->intern_with_view(type_name);
            return std::make_shared<identifier_expr>(typeToken.location, stable_view, sym_id);
        }
    }
    current_ = savedPos;
    return std::nullopt;
}


JAI_NOINLINE checked_result<expression_ptr> parser::parenthesized() {
    JAISCRIPT_TRY_ASSIGN(expression_ptr expr, expression());
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after expression"));
    return expr;
}

// Array literal with [] or Lambda expression
JAI_NOINLINE checked_result<expression_ptr> parser::bracket_expression() {
    const source_location& startLoc = previous().location;

    // A bracket group followed by '(' is a capture list (an array literal is never called): the lambda, read as one
    if (!pushed_back_token_) {
        size_t depth = 0;
        for (size_t i = current_ - 1; i < stream_->size(); ++i) {
            const token_type t = (*stream_)[i].type;
            if (t == token_type::left_bracket || t == token_type::left_paren || t == token_type::left_brace) {
                ++depth;
            } else if (t == token_type::right_bracket || t == token_type::right_paren || t == token_type::right_brace) {
                if (--depth == 0) {
                    if (t == token_type::right_bracket && i + 1 < stream_->size() && (*stream_)[i + 1].type == token_type::left_paren) {
                        current_ -= 1;
                        return lambda_expression();
                    }
                    break;
                }
            } else if (t == token_type::eof || (t == token_type::semicolon && depth == 1)) {
                break;
            }
        }
    }

    // Check for empty array OR empty capture list for lambda
    if (check(token_type::right_bracket)) {
        JAISCRIPT_TRY(expect(token_type::right_bracket, "expected ']'"));

        // Check if this is followed by '(' which would make it a lambda
        if (check(token_type::left_paren)) {
            // Put back the brackets so lambda_expression can parse them
            current_ -= 2; // Back up to before '['
            return lambda_expression();
        }

        return std::make_shared<array_literal_expr>(startLoc, std::vector<expression_ptr>());
    }

    // Save position for potential backtrack to lambda
    size_t savedPos = current_;

    // Try to parse as array literal first
    std::vector<expression_ptr> elements;

    // Check if this might be a lambda with default capture [= or [&
    // or explicit capture [this, [x, [&x
    if (check(token_type::equal) || check(token_type::ampersand)) {
        // This is definitely a lambda, not an array
        current_ = savedPos - 1;
        return lambda_expression();
    }

    // Check for [this]() or [identifier]() pattern which is definitely a lambda
    // We need to look ahead further to distinguish from single-element arrays like [x]
    if (check(token_type::this_keyword) || check(token_type::identifier)) {
        // Look ahead to see if this is followed by ] and then (
        size_t lookAhead = current_ + 1;
        if (lookAhead < stream_->size() && (*stream_)[lookAhead].type == token_type::right_bracket) {
            // Check if there's a ( after the ]
            size_t lookAhead2 = lookAhead + 1;
            if (lookAhead2 < stream_->size() && (*stream_)[lookAhead2].type == token_type::left_paren) {
                // Pattern is [this]() or [x]() - definitely a lambda
                current_ = savedPos - 1;
                return lambda_expression();
            }
        }
        // NOTE: We intentionally do NOT check for [identifier, here anymore
        // because [x, y, z] could be either an array literal OR a lambda capture list.
        // We'll distinguish them later by checking if there's a ( after the closing ].
    }

    // the elements; a comma makes it definitely an array
    bool listed = false;
    while (true) {
        JAISCRIPT_TRY_ASSIGN(auto elem, expression());
        elements.push_back(std::move(elem));
        if (!match(token_type::comma)) { break; }
        listed = true;
    }

    if (listed) {
        JAISCRIPT_TRY(expect(token_type::right_bracket, "expected ']' after array elements"));
        // Check if this is followed by '(' which would make it a lambda
        if (check(token_type::left_paren)) {
            // Restore position and parse as lambda
            current_ = savedPos - 1;
            return lambda_expression();
        }
        return std::make_shared<array_literal_expr>(startLoc, std::move(elements));
    }
    // If we see ], it might be a single-element array or lambda capture
    else if (match(token_type::right_bracket)) {
        // Check if this is followed by '(' which would make it a lambda
        if (check(token_type::left_paren)) {
            // Restore position and parse as lambda
            current_ = savedPos - 1;
            return lambda_expression();
        }
        return std::make_shared<array_literal_expr>(startLoc, std::move(elements));
    }
    // Otherwise, it might be a lambda capture list
    else {
        // Restore position and parse as lambda
        current_ = savedPos - 1;
        return lambda_expression();
    }
}

// cast<T>(e) / cast<T&>(e) / cast<shared_ptr<T>>(e): the explicit typed-boundary
// spelling (detail/cast_kernel.hpp); the type is anything a declaration accepts
JAI_NOINLINE checked_result<expression_ptr> parser::cast_expression() {
    const token& castToken = previous();
    JAISCRIPT_TRY(expect(token_type::less, "expected '<' after 'cast'"));
    if (check(token_type::auto_keyword) || check(token_type::var_keyword) ||
        check(token_type::void_keyword) || check(token_type::function_keyword)) {
        report_error("cast needs a concrete type: cast<T>(e), cast<T&>(e) or cast<shared_ptr<T>>(e)", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    JAISCRIPT_TRY_ASSIGN(type_info_ptr target, parse_type());
    if (!target || target->base_type == script_value_type::jai_any_type ||
        (target->base_type == script_value_type::jai_shared_ptr_type && !target->element_type())) {
        report_error("cast needs a concrete type: cast<T>(e), cast<T&>(e) or cast<shared_ptr<T>>(e)", castToken);
        return make_error_code(parse_error_code::syntax_error);
    }
    const bool by_reference = match(token_type::ampersand);
    consume_greater_in_generic("expected '>' after cast type");
    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after cast type"));
    JAISCRIPT_TRY_ASSIGN(auto operand, expression());
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after cast operand"));
    if (by_reference) { operand = bound_conditional(std::move(operand)); }
    return std::make_shared<cast_expr>(castToken.location, target, by_reference, std::move(operand));
}

// Type parsing
checked_result<type_info_ptr> parser::parse_type() {
    stack_.guard(peek().location);
    switch (peek().type) {
        case token_type::array_keyword:
            advance();
            return array_type();
        case token_type::map_keyword:
            advance();
            return map_type();
        case token_type::weak_ptr_keyword:
            advance();
            return weak_ptr_type();
        case token_type::shared_ptr_keyword:
            advance();
            return shared_ptr_type();
        default:
            return named_type();
    }
}

JAI_NOINLINE checked_result<type_info_ptr> parser::named_type() {
    if (check(token_type::const_keyword)) {   // array<const int>, cast<const T&>: const qualifies a declaration
        report_foreign(detail::foreign_word::const_type_argument, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    // Handle auto - type inference (nullptr = infer from initializer, or uninitialized)
    if (match(token_type::auto_keyword)) {
        return type_info_ptr(nullptr);
    }

    // Handle var - dynamic typing (any type allowed, never locks)
    if (match(token_type::var_keyword)) {
        return store_type_info(type_info::make_any(*symbolizer_));
    }

    // `function`: a callable-or-null slot (a std::function's rule) at every boundary
    if (match(token_type::function_keyword)) {
        return store_type_info(type_info::make_callable(*symbolizer_));
    }

    // Primitive types
    if (match(token_type::int_keyword)) {
        return store_type_info(type_info::make_int(*symbolizer_));
    }
    if (match(token_type::float_keyword)) {
        return store_type_info(type_info::make_float(*symbolizer_));
    }
    if (match(token_type::string_keyword)) {
        return store_type_info(type_info::make_string(*symbolizer_));
    }
    if (match(token_type::bool_keyword)) {
        return store_type_info(type_info::make_bool(*symbolizer_));
    }
    if (match(token_type::char_keyword)) {
        return store_type_info(type_info::make_char(*symbolizer_));
    }
    if (match(token_type::void_keyword)) {
        return store_type_info(type_info::make_void(*symbolizer_));
    }

    if (check(token_type::identifier) && peek().lexeme == "utf8_string") {
        advance();
        return store_type_info(type_info::make_utf8_string(*symbolizer_));
    }

    if (check(token_type::identifier) && (peek().lexeme == "uint8" || peek().lexeme == "int8")) {
        report_error(std::string("'") + std::string(peek().lexeme) + "' is an array element type only (array<" + std::string(peek().lexeme) +
                     ">, one byte per element); a single byte is an int", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    if (check(token_type::identifier)) {
        const script_value_type math_kind = math_type_from_name(peek().lexeme);
        if (math_kind != script_value_type::jai_null_type) {
            advance();
            return store_type_info(type_info::make_math(*symbolizer_, math_kind));
        }
    }

    // Handle "double" as an alias for float (since we only have float_keyword)
    if (check(token_type::identifier) && peek().lexeme == "double") {
        advance();  // consume "double"
        return store_type_info(type_info::make_float(*symbolizer_));
    }

    // User-defined type (potentially templated)
    if (match(token_type::identifier)) {
        std::string type_name(previous().lexeme);
        // ns::Type names a class by its qualified name: a host's (time::date) or one a script namespace declares
        while (check(token_type::colon_colon) &&
               current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::identifier) {
            advance();
            type_name += "::";
            type_name += advance().lexeme;
        }

        // A host template's base name followed by '<' is a template (Point<int>); the bare name stays an ordinary identifier
        if (check(token_type::less) && is_registered_template_type(type_name)) {
            advance();
            std::vector<type_info_ptr> templateParams;
            std::vector<std::string> spellings;
            do {
                type_info_ptr param;
                if (check(token_type::identifier) && (peek().lexeme == "uint8" || peek().lexeme == "int8")) {
                    param = store_type_info(advance().lexeme == "uint8" ? type_info::make_uint8(*symbolizer_) : type_info::make_int8(*symbolizer_));
                } else {
                    JAISCRIPT_TRY_ASSIGN(param, parse_type());
                }
                if (!param) {
                    report_error("a template argument needs a concrete type", previous());
                    return make_error_code(parse_error_code::syntax_error);
                }
                spellings.push_back(param->canonical_name());
                templateParams.push_back(param);
            } while (match(token_type::comma));

            consume_greater_in_generic("expected '>' after template parameters");

            type_info info = type_info::make_object(*symbolizer_, template_instance_name(type_name, spellings));
            info.type_params = std::move(templateParams);
            return store_type_info(std::move(info));
        }

        return store_type_info(type_info::make_object(*symbolizer_, type_name));
    }

    report_error("expected type", peek());
    return make_error_code(parse_error_code::syntax_error);
}

JAI_NOINLINE checked_result<type_info_ptr> parser::array_type() {
    JAISCRIPT_TRY(expect(token_type::less,
        "expected '<' after 'array' - 'array' is the built-in array type keyword (array<T>); "
        "if this was meant as a variable, rename it"));
    type_info_ptr element_type;
    if (check(token_type::identifier) && (peek().lexeme == "uint8" || peek().lexeme == "int8")) {
        element_type = byte_element_type();
    } else {
        JAISCRIPT_TRY_ASSIGN(element_type, parse_type());
    }
    consume_greater_in_generic("expected '>' after array element type");
    return array_type_of(element_type);
}

JAI_NOINLINE checked_result<type_info_ptr> parser::map_type() {
    JAISCRIPT_TRY(expect(token_type::less,
        "expected '<' after 'map' - 'map' is the built-in map type keyword (map<K, V>); "
        "if this was meant as a variable, rename it"));
    JAISCRIPT_TRY_ASSIGN(type_info_ptr keyType, parse_type());
    JAISCRIPT_TRY(expect(token_type::comma, "expected ',' after map key type"));
    JAISCRIPT_TRY_ASSIGN(type_info_ptr valueType, parse_type());
    consume_greater_in_generic("expected '>' after map value type");
    return map_type_of(keyType, valueType);
}

JAI_NOINLINE checked_result<type_info_ptr> parser::weak_ptr_type() {
    JAISCRIPT_TRY(expect(token_type::less, "expected '<' after 'weak_ptr'"));
    if (check(token_type::const_keyword)) {
        report_foreign(detail::foreign_word::pointer_to_const, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    JAISCRIPT_TRY_ASSIGN(type_info_ptr pointee_type, parse_type());
    consume_greater_in_generic("expected '>' after weak_ptr type");
    return weak_ptr_type_of(pointee_type);
}

JAI_NOINLINE checked_result<type_info_ptr> parser::shared_ptr_type() {
    JAISCRIPT_TRY(expect(token_type::less, "expected '<' after 'shared_ptr'"));
    if (check(token_type::const_keyword)) {
        report_foreign(detail::foreign_word::pointer_to_const, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    // A dynamic pointee adds nothing over plain var (which
    // already holds and rebinds any shared_ptr) - reject with the teaching pair
    if (check(token_type::var_keyword)) {
        report_error("shared_ptr<var> is not supported: use var (holds and rebinds any shared_ptr) or shared_ptr<auto> (infers then enforces the pointee)", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    JAISCRIPT_TRY_ASSIGN(type_info_ptr pointee_type, parse_type());
    consume_greater_in_generic("expected '>' after shared_ptr type");
    return shared_ptr_type_of(pointee_type);
}

JAI_NOINLINE type_info_ptr parser::byte_element_type() {
    return store_type_info(advance().lexeme == "uint8" ? type_info::make_uint8(*symbolizer_) : type_info::make_int8(*symbolizer_));
}

JAI_NOINLINE type_info_ptr parser::array_type_of(const type_info_ptr& element) {
    return store_type_info(type_info::make_array(*symbolizer_, element));
}

JAI_NOINLINE type_info_ptr parser::map_type_of(const type_info_ptr& key, const type_info_ptr& value) {
    return store_type_info(type_info::make_map(*symbolizer_, key, value));
}

JAI_NOINLINE type_info_ptr parser::weak_ptr_type_of(const type_info_ptr& pointee) {
    return store_type_info(type_info::make_weak_ptr(*symbolizer_, pointee));
}

// shared_ptr<T> has reference semantics (no clone on assign); its type_name is T's
JAI_NOINLINE type_info_ptr parser::shared_ptr_type_of(const type_info_ptr& pointee_type) {
    type_info info(script_value_type::jai_shared_ptr_type);
    info.type_params.push_back(pointee_type);
    if (pointee_type) {
        info.type_name = pointee_type->type_name;
    } else {
        info.type_name = "shared_ptr";
    }
    info.id = symbolizer_->intern(info.canonical_name());
    return store_type_info(std::move(info));
}

namespace {
    constexpr int k_no_operator = -1;
    constexpr int k_assignment_precedence = 0;   // right-associative: its value is a whole expression
    constexpr int k_ternary_precedence = 1;      // right-associative: the else arm is another conditional
    constexpr int k_coalesce_precedence = 2;     // right-associative, below || and above the conditional, as C#'s
    constexpr int k_equality_precedence = 8;
    constexpr int k_relational_precedence = 9;
    constexpr int k_shift_precedence = 10;
    constexpr int k_additive_precedence = 11;    // + - * / % fold their literal operands

    // The binding strength of the operator a token spells after an operand; || (3) through * / % (12) are left-associative
    int operator_precedence(token_type t) {
        switch (t) {
            case token_type::question_question_equal: case token_type::equal: case token_type::plus_equal: case token_type::minus_equal:
            case token_type::star_equal: case token_type::slash_equal: case token_type::percent_equal: case token_type::ampersand_equal:
            case token_type::pipe_equal: case token_type::caret_equal: case token_type::left_shift_equal: case token_type::right_shift_equal:
                return k_assignment_precedence;
            case token_type::question: return k_ternary_precedence;
            case token_type::question_question: return k_coalesce_precedence;
            case token_type::pipe_pipe: return 3;
            case token_type::ampersand_ampersand: return 4;
            case token_type::pipe: return 5;
            case token_type::caret: return 6;
            case token_type::ampersand: return 7;
            case token_type::equal_equal: case token_type::bang_equal: return k_equality_precedence;
            case token_type::less: case token_type::less_equal: case token_type::greater: case token_type::greater_equal:
            case token_type::spaceship: return k_relational_precedence;
            case token_type::left_shift: case token_type::right_shift: return k_shift_precedence;
            case token_type::plus: case token_type::minus: return k_additive_precedence;
            case token_type::star: case token_type::slash: case token_type::percent: return 12;
            default: return k_no_operator;
        }
    }

    JAI_NOINLINE expression_ptr subscript_node(expression_ptr object, expression_ptr index, const source_location& at) {
        token op(token_type::left_bracket, "[", at);
        return std::make_shared<binary_expr>(op.location, std::move(object), op, std::move(index));
    }

    // A target `??=` reads and then may store: evaluating it twice must do what evaluating it once does, so it names storage
    // through names, members and elements whose indices run no code
    bool coalesce_target_pure(const expression* e) {
        if (!e) return false;
        switch (e->get_type()) {
            case node_type::identifier_expr:
            case node_type::this_expr:
            case node_type::literal_expr:
                return true;
            case node_type::member_expr: {
                auto* m = static_cast<const member_expr*>(e);
                return !m->null_safe && coalesce_target_pure(m->object.get());
            }
            case node_type::binary_expr: {
                auto* b = static_cast<const binary_expr*>(e);
                switch (b->op.type) {
                    case token_type::left_bracket: case token_type::plus: case token_type::minus: case token_type::star:
                    case token_type::slash: case token_type::percent:
                        return coalesce_target_pure(b->left.get()) && coalesce_target_pure(b->right.get());
                    default:
                        return false;
                }
            }
            case node_type::unary_expr: {
                auto* u = static_cast<const unary_expr*>(e);
                return (u->op.type == token_type::minus || u->op.type == token_type::plus) && coalesce_target_pure(u->operand.get());
            }
            default:
                return false;
        }
    }
}

checked_result<expression_ptr> parser::expression() {
    return operator_expression(k_assignment_precedence);
}

// One frame per operand on the way down: what follows the operand climbs in operator_rest
// The operator after an operand: a token's, or the word instanceof, an operator only there (a relational one, as in JavaScript)
int parser::infix_precedence() const {
    const token& next = peek();
    if (next.type == token_type::identifier && next.lexeme == "instanceof") { return k_relational_precedence; }
    return operator_precedence(next.type);
}

checked_result<expression_ptr> parser::operator_expression(int min_precedence) {
    stack_.guard(peek().location);
    const size_t operand_start = current_;
    auto left = unary();
    if (!left || infix_precedence() < min_precedence) { return left; }
    return operator_rest(std::move(left.value()), min_precedence, operand_start);
}

// e instanceof T: T a class by name (ns::T too); the node is a cast that answers instead of converting
JAI_NOINLINE checked_result<expression_ptr> parser::instance_test_rest(expression_ptr left) {
    const token word = advance();
    if (!check(token_type::identifier)) {
        report_error(is_type_keyword_token(peek().type)
            ? "'instanceof' tests a class, and '" + std::string(peek().lexeme) + "' is a builtin type: compare type_of(x) == \"" + std::string(peek().lexeme) + "\""
            : std::string("expected a class name after 'instanceof'"), peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    JAISCRIPT_TRY_ASSIGN(type_info_ptr target, parse_type());
    if (!target || target->base_type != script_value_type::jai_object_type || target->type_name.empty()) {
        report_error("expected a class name after 'instanceof'", word);
        return make_error_code(parse_error_code::syntax_error);
    }
    auto test = std::make_shared<cast_expr>(word.location, target, false, std::move(left));
    test->instance_test = true;
    return expression_ptr(test);
}

JAI_NOINLINE checked_result<expression_ptr> parser::operator_rest(expression_ptr left, int min_precedence, size_t operand_start) {
    for (int precedence = infix_precedence(); precedence >= min_precedence; precedence = infix_precedence()) {
        if (precedence == k_relational_precedence && peek().type == token_type::identifier) {
            JAISCRIPT_TRY_ASSIGN(left, instance_test_rest(std::move(left)));
            continue;
        }
        if (precedence == k_assignment_precedence) { return assignment_rest(std::move(left), operand_start); }
        if (precedence == k_ternary_precedence) {
            JAISCRIPT_TRY_ASSIGN(left, ternary_rest(std::move(left)));
            continue;
        }
        if (precedence == k_coalesce_precedence) {
            JAISCRIPT_TRY_ASSIGN(left, coalesce_rest(std::move(left)));
            continue;
        }
        advance();
        const token& op = previous();
        if (precedence == k_equality_precedence && check(token_type::equal)) {
            report_foreign(op.type == token_type::equal_equal ? detail::foreign_word::strict_equal : detail::foreign_word::strict_not_equal, op);
            return make_error_code(parse_error_code::syntax_error);
        }
        JAISCRIPT_TRY_ASSIGN(expression_ptr right, operator_expression(precedence + 1));
        if (precedence >= k_additive_precedence) {
            left = try_constant_fold(std::move(left), op, std::move(right));
        } else {
            left = std::make_shared<binary_expr>(op.location, std::move(left), op, std::move(right));
        }
    }
    return left;
}

JAI_NOINLINE checked_result<expression_ptr> parser::assignment_rest(expression_ptr expr, size_t target_start) {
    if (check(token_type::question_question_equal)) { return coalesce_assignment(std::move(expr), target_start); }
    advance();
    const token& op = previous();
    JAISCRIPT_TRY_ASSIGN(expression_ptr right, expression());
    return std::make_shared<assignment_expr>(op.location, lvalue_target(expr), op, right);
}

// a ??= b is a ?? (a = b): the target read once to test it, stored only when it held null
JAI_NOINLINE checked_result<expression_ptr> parser::coalesce_assignment(expression_ptr expr, size_t target_start) {
    advance();
    token op = previous();
    const node_type shape = expr->get_type();
    const bool storage = shape == node_type::identifier_expr || shape == node_type::member_expr ||
                         (shape == node_type::binary_expr && static_cast<const binary_expr*>(expr.get())->op.type == token_type::left_bracket);
    if (!storage || !coalesce_target_pure(expr.get())) {
        report_error("'??=' needs a variable, a member or an element whose index runs no code; store anything else in a local first", op);
        return make_error_code(parse_error_code::syntax_error);
    }
    const size_t resume = current_;
    const std::optional<token> pushed = pushed_back_token_;
    current_ = target_start;
    pushed_back_token_.reset();
    auto store_target = operator_expression(k_ternary_precedence);
    current_ = resume;
    pushed_back_token_ = pushed;
    if (!store_target) { return store_target.error_value(); }
    JAISCRIPT_TRY_ASSIGN(expression_ptr right, expression());
    token assign(token_type::equal, "=", op.location);
    auto store = std::make_shared<assignment_expr>(op.location, lvalue_target(store_target.value()), assign, right);
    token coalesce_op(token_type::question_question, "??", op.location);
    return std::make_shared<binary_expr>(op.location, expr, coalesce_op, store);
}

// `{ key:` from the token `offset` past the current one: a map literal's head, which no block begins with
bool parser::json_map_ahead(size_t offset) const {
    const size_t at = current_ + offset;
    return at + 1 < stream_->size() && is_json_map_key_token((*stream_)[at].type) && (*stream_)[at + 1].type == token_type::colon;
}

// Helper function to check if { starts a map literal (as opposed to a block statement)
// This avoids using exceptions for control flow
// At a key token: the key (one token, or a name qualified with ::) is followed by ':'. Leaves the position past the key.
bool parser::json_map_key_follows() {
    const bool name = check(token_type::identifier);
    advance();
    while (name && check(token_type::colon_colon)) {
        advance();
        if (!check(token_type::identifier)) { return false; }
        advance();
    }
    return check(token_type::colon);
}

bool parser::looks_like_map_literal() {
    // Save position for lookahead
    size_t savedPos = current_;

    // Consume the {
    advance();

    // Empty braces {} is an empty map
    if (check(token_type::right_brace)) {
        current_ = savedPos;
        return true;
    }

    // Check for keywords that indicate a block statement, not a map
    if (check(token_type::auto_keyword) || check(token_type::var_keyword) ||
        check(token_type::if_keyword) || check(token_type::while_keyword) ||
        check(token_type::for_keyword) || check(token_type::return_keyword) ||
        check(token_type::class_keyword) || check(token_type::function_keyword)) {
        current_ = savedPos;
        return false;
    }

    // Check for function calls: identifier followed by (
    // This indicates a block with statements, not a map
    if (check(token_type::identifier)) {
        advance(); // consume identifier
        if (check(token_type::left_paren)) {
            current_ = savedPos;
            return false;
        }
        current_ = savedPos + 1; // back to after {
    }

    // Check for JSON-style map: one key token (identifier/string/int/float/char/bool), or a qualified name (E::v),
    // followed by ':'
    if (is_json_map_key_token(peek().type)) {
        const bool isMap = json_map_key_follows();
        current_ = savedPos;
        return isMap;
    }

    // Check for C++ style map: {{ ...
    if (check(token_type::left_brace)) {
        current_ = savedPos;
        return true;
    }

    // Default: not a map (could be block or other construct)
    current_ = savedPos;
    return false;
}

checked_result<expression_ptr> parser::parse_map_literal() {
    stack_.guard(peek().location);   // {{...}} recurses here without passing through expression()
    JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{'"));
    const source_location& startLoc = previous().location;

    // Empty map
    if (check(token_type::right_brace)) {
        JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}'"));
        return std::make_shared<map_literal_expr>(startLoc, std::vector<std::pair<expression_ptr, expression_ptr>>());
    }

    // Save position to check for JSON style
    size_t savedPos = current_;

    // Check if it's JSON style by looking for "key": value pattern
    bool isJsonStyle = false;

    // First, check if the first token could be a key (identifier/string/int/float/
    // char/bool - keep in lockstep with looks_like_map_literal via is_json_map_key_token)
    if (is_json_map_key_token(peek().type)) {
        isJsonStyle = json_map_key_follows();
        current_ = savedPos;
    }

    std::vector<std::pair<expression_ptr, expression_ptr>> entries;
    entries.reserve(8);

    if (isJsonStyle) {
        // Parse JSON style: {key: value, key2: value2} or {"key": value}
        do {
            expression_ptr key;

            // a bare identifier key is its name as a string: {x: 10} is {"x": 10}; a qualified name is its value
            if (check(token_type::identifier) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type != token_type::colon_colon) {
                key = identifier_map_key();
            } else {
                // Parse as expression (for quoted strings or computed keys)
                JAISCRIPT_TRY_ASSIGN(key, expression());
            }

            JAISCRIPT_TRY(expect(token_type::colon, "expected ':' after key in JSON-style map"));
            JAISCRIPT_TRY_ASSIGN(expression_ptr value, expression());
            entries.emplace_back(std::move(key), std::move(value));
        } while (match(token_type::comma));
    } else {
        // Parse C++ style: {{"key", value}, {"key2", value2}}
        do {
            JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' for map entry"));
            JAISCRIPT_TRY_ASSIGN(expression_ptr key, expression());
            JAISCRIPT_TRY(expect(token_type::comma, "expected ',' between key and value in map entry"));
            JAISCRIPT_TRY_ASSIGN(expression_ptr value, expression());
            JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after map entry"));

            entries.emplace_back(std::move(key), std::move(value));
        } while (match(token_type::comma));
    }

    JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after map entries"));
    return std::make_shared<map_literal_expr>(startLoc, std::move(entries));
}

JAI_NOINLINE expression_ptr parser::identifier_map_key() {
    token keyToken = advance();
    script_string keyStr(keyToken.lexeme);
    script_value keyValue(script_value::ast_literal_tag{}, keyStr);
    return std::make_shared<literal_expr>(keyToken.location, keyValue);
}

JAI_NOINLINE checked_result<expression_ptr> parser::ternary_rest(expression_ptr condition) {
    advance();
    const token& question = previous();
    JAISCRIPT_TRY_ASSIGN(expression_ptr then_expression, expression());
    JAISCRIPT_TRY(expect(token_type::colon, "expected ':' after then expression in ternary"));
    JAISCRIPT_TRY_ASSIGN(expression_ptr else_expression, operator_expression(k_ternary_precedence));
    return std::make_shared<ternary_expr>(question.location, condition, then_expression, else_expression);
}

// a ?? b: a when it is not null, else b (b runs only then)
JAI_NOINLINE checked_result<expression_ptr> parser::coalesce_rest(expression_ptr left) {
    advance();
    const token& op = previous();
    JAISCRIPT_TRY_ASSIGN(expression_ptr right, operator_expression(k_coalesce_precedence));
    return std::make_shared<binary_expr>(op.location, left, op, right);
}

// An enumerator's initializer, evaluated at parse time as C++ evaluates it: an integer or char literal, an earlier enumerator
// (bare or through the enum's name), and the integer operators over them, under the run time's overflow and shift rules.
// The failing case comes back as the error text's tail.
static bool enum_initializer_value(const expression* e, const enum_decl& d, std::vector<script_int>& known, script_int& out, std::string& why) {
    switch (e->get_type()) {
        case node_type::literal_expr: {
            const script_value& v = static_cast<const literal_expr*>(e)->value;
            if (v.is_int()) { out = v.unchecked_as_int(); return true; }
            if (v.raw_storage_index() == script_value::TYPEID_CHAR) { out = static_cast<script_int>(v.unchecked_as_char()); return true; }
            why = "needs a constant integer";
            return false;
        }
        case node_type::identifier_expr:
        case node_type::member_expr: {
            std::string_view name;
            if (e->get_type() == node_type::identifier_expr) {
                name = static_cast<const identifier_expr*>(e)->name;
            } else {
                const auto* m = static_cast<const member_expr*>(e);
                if (m->null_safe || m->is_arrow || !m->object || m->object->get_type() != node_type::identifier_expr ||
                    static_cast<const identifier_expr*>(m->object.get())->name != d.name) {
                    why = "needs a constant integer";
                    return false;
                }
                name = m->member;
            }
            for (size_t i = 0; i < known.size(); ++i) {
                if (d.values[i].first == name) { out = known[i]; return true; }
            }
            why = "names '" + std::string(name) + "', which is not an earlier value of '" + std::string(d.name) + "'";
            return false;
        }
        case node_type::unary_expr: {
            const auto* u = static_cast<const unary_expr*>(e);
            script_int v = 0;
            if (u->is_postfix || !enum_initializer_value(u->operand.get(), d, known, v, why)) { if (why.empty()) { why = "needs a constant integer"; } return false; }
            switch (u->op.type) {
                case token_type::plus: out = v; return true;
                case token_type::tilde: out = ~v; return true;
                case token_type::minus:
                    if (!ints::try_neg(v, out)) { why = "overflows the int range"; return false; }
                    return true;
                default: why = "needs a constant integer"; return false;
            }
        }
        case node_type::binary_expr: {
            const auto* b = static_cast<const binary_expr*>(e);
            script_int l = 0, r = 0;
            if (!enum_initializer_value(b->left.get(), d, known, l, why) || !enum_initializer_value(b->right.get(), d, known, r, why)) { return false; }
            switch (b->op.type) {
                case token_type::plus: case token_type::minus: case token_type::star: case token_type::slash: case token_type::percent:
                    switch (detail::int_arith(b->op.type, l, r, out)) {
                        case detail::scalar_fault::none: return true;
                        case detail::scalar_fault::overflow: why = "overflows the int range"; return false;
                        default: why = "divides by zero"; return false;
                    }
                case token_type::ampersand: out = l & r; return true;
                case token_type::pipe: out = l | r; return true;
                case token_type::caret: out = l ^ r; return true;
                case token_type::left_shift: case token_type::right_shift:
                    if (r < 0 || r > 63) { why = "shifts by " + std::to_string(r) + ": the shift amount must be between 0 and 63"; return false; }
                    out = b->op.type == token_type::left_shift ? (l << r) : (l >> r);
                    return true;
                default: why = "needs a constant integer"; return false;
            }
        }
        default:
            why = "needs a constant integer";
            return false;
    }
}

// Constant folding optimization: evaluates literal operations at parse time
// If both operands are literals, compute the result now instead of at runtime
JAI_NOINLINE expression_ptr parser::try_constant_fold(expression_ptr left, const token& op, expression_ptr right) {
    if (left->get_type() != node_type::literal_expr || right->get_type() != node_type::literal_expr) {
        // Not both literals, return binary_expr
        return std::make_shared<binary_expr>(op.location, left, op, right);
    }

    auto* leftLit = static_cast<literal_expr*>(left.get());
    auto* rightLit = static_cast<literal_expr*>(right.get());

    const script_value& leftVal = leftLit->value;
    const script_value& rightVal = rightLit->value;

    // Integer folds go through the overflow policy: on overflow nothing folds, so the run raises with the operator's name
    // (a wrapping build folds, jai::ints wrapping without UB)
    if (leftVal.is_int() && rightVal.is_int()) {
        script_int leftInt = leftVal.as_int();
        script_int rightInt = rightVal.as_int();
        script_int result = 0;

        switch (op.type) {
            case token_type::plus:
                if (!ints::try_add(leftInt, rightInt, result)) {
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                break;
            case token_type::minus:
                if (!ints::try_sub(leftInt, rightInt, result)) {
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                break;
            case token_type::star:
                if (!ints::try_mul(leftInt, rightInt, result)) {
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                break;
            case token_type::slash:
                // Zero divisor and INT64_MIN/-1: let the runtime raise
                if (rightInt == 0 || !ints::try_div(leftInt, rightInt, result)) {
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                break;
            case token_type::percent:
                if (rightInt == 0) {
                    // Don't fold modulo by zero - let runtime handle it
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                result = ints::mod(leftInt, rightInt);   // -1 divisor: 0, no hardware trap
                break;
            default:
                // Unsupported operation for constant folding
                return std::make_shared<binary_expr>(op.location, left, op, right);
        }

        // Create a folded literal with the computed value
        return std::make_shared<literal_expr>(op.location, script_value(script_value::ast_literal_tag{}, result));
    }

    // Fold float arithmetic
    if ((leftVal.is_float() || leftVal.is_int()) && (rightVal.is_float() || rightVal.is_int())) {
        script_float leftFloat = leftVal.is_int() ? static_cast<script_float>(leftVal.as_int()) : leftVal.as_float();
        script_float rightFloat = rightVal.is_int() ? static_cast<script_float>(rightVal.as_int()) : rightVal.as_float();
        script_float result = 0.0;

        switch (op.type) {
            case token_type::plus:
                result = leftFloat + rightFloat;
                break;
            case token_type::minus:
                result = leftFloat - rightFloat;
                break;
            case token_type::star:
                result = leftFloat * rightFloat;
                break;
            case token_type::slash:
                if (rightFloat == 0.0) {
                    // Don't fold division by zero - let runtime handle it
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                result = leftFloat / rightFloat;
                break;
            case token_type::percent:
                if (rightFloat == 0.0) {
                    // Don't fold modulo by zero - let runtime handle it
                    return std::make_shared<binary_expr>(op.location, left, op, right);
                }
                result = std::fmod(leftFloat, rightFloat);
                break;
            default:
                // Unsupported operation for constant folding
                return std::make_shared<binary_expr>(op.location, left, op, right);
        }

        // Create a folded literal with the computed value
        return std::make_shared<literal_expr>(op.location, script_value(script_value::ast_literal_tag{}, result));
    }

    // Fold string concatenation
    if (op.type == token_type::plus && leftVal.is_string() && rightVal.is_string()) {
        script_string result = leftVal.as_string() + rightVal.as_string();
        return std::make_shared<literal_expr>(op.location, script_value(script_value::ast_literal_tag{}, result));
    }

    // Can't fold this operation
    return std::make_shared<binary_expr>(op.location, left, op, right);
}

checked_result<expression_ptr> parser::unary() {
    stack_.guard(peek().location);   // !!!x recurses here without passing through expression()
    switch (peek().type) {
        case token_type::ampersand: case token_type::dot: case token_type::bang: case token_type::minus: case token_type::plus:
        case token_type::plus_plus: case token_type::minus_minus: case token_type::tilde: case token_type::throw_keyword:
        case token_type::yield_keyword:
            return unary_operator();
        default:
            return postfix();
    }
}

JAI_NOINLINE checked_result<expression_ptr> parser::unary_operator() {
    if (check(token_type::ampersand)) {
        report_foreign(detail::foreign_word::address_of, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    if (check(token_type::dot) && current_ + 2 < stream_->size() && (*stream_)[current_ + 1].type == token_type::dot &&
        (*stream_)[current_ + 2].type == token_type::dot) {
        report_foreign(detail::foreign_word::spread, peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    if (match({token_type::bang, token_type::minus, token_type::plus, token_type::plus_plus,
               token_type::minus_minus, token_type::tilde})) {
        const token& op = previous();
        JAISCRIPT_TRY_ASSIGN(expression_ptr right, unary());
        if ((op.type == token_type::plus_plus || op.type == token_type::minus_minus) &&
            right->get_type() != node_type::identifier_expr) {
            return desugar_incdec(op, right, false);
        }
        // a negated numeric literal is a literal: the sign is part of the number as the language reads it, so -1 and
        // -1.5 are constants to every consumer (the fused compare, the move rows, the counted head); a folded INT64_MIN
        // stays a run-time negation (which raises under checked overflow), and -0.0 keeps IEEE's sign as the runtime negation would
        // +x on a number is the number (C++'s unary plus); a class's operator+() is the unary_expr's
        if (op.type == token_type::plus && right->get_type() == node_type::literal_expr) {
            const size_t k = static_cast<literal_expr*>(right.get())->value.raw_storage_index();
            if (k == script_value::TYPEID_INT || k == script_value::TYPEID_FLOAT) { return right; }
        }
        if (op.type == token_type::minus && right->get_type() == node_type::literal_expr) {
            const script_value& lv = static_cast<literal_expr*>(right.get())->value;
            script_int negated = 0;
            if (lv.raw_storage_index() == script_value::TYPEID_INT && ints::try_neg(lv.unchecked_as_int(), negated)) {
                return std::make_shared<literal_expr>(op.location, script_value(script_value::ast_literal_tag{}, negated));
            }
            if (lv.raw_storage_index() == script_value::TYPEID_FLOAT) {
                return std::make_shared<literal_expr>(op.location, script_value(script_value::ast_literal_tag{}, -lv.unchecked_as_float()));
            }
        }
        return std::make_shared<unary_expr>(op.location, op, right);
    }

    if (match(token_type::throw_keyword)) {
        const token& throw_token = previous();
        expression_ptr value = nullptr;

        // Check if there's an expression after throw (not a semicolon or end of statement)
        if (!check(token_type::semicolon) && !is_at_end()) {
            JAISCRIPT_TRY_ASSIGN(value, expression());
        }

        return std::make_shared<throw_expr>(throw_token.location, value);
    }

    if (match(token_type::yield_keyword)) {
        const token& yield_token = previous();
        if (!in_coroutine_) {
            report_error("yield can only be used inside a coroutine function", yield_token);
            return checked_result<expression_ptr>(
                make_error_code(parse_error_code::syntax_error),
                "yield can only be used inside a coroutine function");
        }
        expression_ptr value = nullptr;
        if (!check(token_type::semicolon) && !check(token_type::right_paren) &&
            !check(token_type::right_brace) && !check(token_type::comma) &&
            !is_at_end()) {
            JAISCRIPT_TRY_ASSIGN(value, expression());
        }
        return std::make_shared<yield_expr>(yield_token.location, std::move(value));
    }

    return postfix();
}

checked_result<expression_ptr> parser::postfix() {
    const size_t chain_start = current_;
    const bool chain_starts_clean = !pushed_back_token_.has_value();
    auto operand = primary();
    if (!operand || !postfix_operator_ahead()) { return operand; }
    return postfix_rest(std::move(operand.value()), chain_start, chain_starts_clean);
}

bool parser::postfix_operator_ahead() const {
    switch (peek().type) {
        case token_type::left_paren: case token_type::left_brace: case token_type::dot: case token_type::question_dot:
        case token_type::arrow: case token_type::colon_colon: case token_type::left_bracket: case token_type::plus_plus:
        case token_type::minus_minus:
            return true;
        default:
            return false;
    }
}

JAI_NOINLINE checked_result<expression_ptr> parser::postfix_rest(expression_ptr expr, size_t chain_start, bool chain_starts_clean) {
    std::vector<size_t> null_links;   // the ?. tokens of this chain
    while (postfix_operator_ahead()) {
        JAISCRIPT_TRY_ASSIGN(expr, postfix_operator(std::move(expr), null_links));
    }
    if (!null_links.empty() && chain_starts_clean && !pushed_back_token_) { return null_chain_rest(chain_start, null_links, expr); }
    return expr;
}

JAI_NOINLINE checked_result<expression_ptr> parser::postfix_operator(expression_ptr expr, std::vector<size_t>& null_links) {
    if (match(token_type::left_paren)) { return finish_call(std::move(expr)); }
    if (match(token_type::left_brace)) { return brace_construction(std::move(expr)); }
    if (match(token_type::dot) || match(token_type::question_dot)) {
        bool is_null_safe = (previous().type == token_type::question_dot);
        if (is_null_safe && !pushed_back_token_) { null_links.push_back(current_ - 1); }
        JAISCRIPT_TRY_ASSIGN(expr, finish_member_access(std::move(expr), false));
        if (is_null_safe) {
            static_cast<member_expr*>(expr.get())->null_safe = true;
        }
        return expr;
    }
    if (match(token_type::arrow)) { return finish_member_access(std::move(expr), true); }
    if (match(token_type::colon_colon)) { return static_member_access(std::move(expr)); }
    if (match(token_type::left_bracket)) { return subscript(std::move(expr)); }
    advance();
    const token& op = previous();   // postfix increment/decrement
    if (expr->get_type() != node_type::identifier_expr) {
        return desugar_incdec(op, expr, true);
    }
    return std::make_shared<unary_expr>(op.location, op, expr, true);  // true = postfix
}

// Brace initialization: Type{args...}, only after a type name
JAI_NOINLINE checked_result<expression_ptr> parser::brace_construction(expression_ptr expr) {
    if (expr->get_type() == node_type::identifier_expr) {
        auto* identExpr = static_cast<identifier_expr*>(expr.get());
        JAISCRIPT_TRY_ASSIGN(auto arguments, construction_arguments(true));
        auto type_info = store_type_info(type_info::make_object(*symbolizer_, identExpr->name));
        return std::make_shared<new_expr>(identExpr->location, type_info, std::move(arguments));
    }
    report_error("expected ';' after expression", previous());   // `i++ {`: only a type name takes a brace initializer
    return make_error_code(parse_error_code::syntax_error);
}

// Static member access or namespace member access; keywords are member names too (namespace members)
JAI_NOINLINE checked_result<expression_ptr> parser::static_member_access(expression_ptr expr) {
    token name = peek();
    if (name.type == token_type::identifier ||
        (name.type != token_type::eof && name.type != token_type::left_paren &&
         name.type != token_type::right_paren && name.type != token_type::semicolon)) {
        advance();
        uint64_t member_id = get_symbol_id(name);
        return std::make_shared<member_expr>(name.location, expr, name.lexeme, member_id, false, true);
    }
    report_error("expected member name after '::'", name);
    return make_error_code(parse_error_code::syntax_error);
}

JAI_NOINLINE checked_result<expression_ptr> parser::subscript(expression_ptr expr) {
    JAISCRIPT_TRY_ASSIGN(expression_ptr index, expression());
    JAISCRIPT_TRY(expect(token_type::right_bracket, "expected ']' after array index"));
    return subscript_node(std::move(expr), std::move(index), previous().location);
}

// a?.b.c: a null before the ?. skips the rest of the chain, which then yields null (JavaScript's and C#'s optional chaining). The
// last ?. a plain link follows (one more than its member and that member's call) is rewritten over the tokens: a base that runs no
// code reads twice, `(null != base) ? base.rest : null`; any other base is evaluated once into an arrow function's parameter,
// `((p) => (null != p) ? p.rest : null)(base)`. Each part parses again, so an earlier ?. in it is rewritten the same way.
checked_result<expression_ptr> parser::null_chain_rest(size_t chain_start, const std::vector<size_t>& links, expression_ptr expr) {
    const std::vector<token>& tokens = *stream_;
    const size_t chain_end = current_;
    auto group_end = [&](size_t at) {   // past the balanced group opened at `at`
        size_t depth = 0;
        for (size_t i = at; i < chain_end; ++i) {
            if (tokens[i].type == token_type::left_paren || tokens[i].type == token_type::left_bracket || tokens[i].type == token_type::left_brace) { ++depth; }
            else if (tokens[i].type == token_type::right_paren || tokens[i].type == token_type::right_bracket || tokens[i].type == token_type::right_brace) {
                if (--depth == 0) return i + 1;
            }
        }
        return chain_end;
    };
    size_t link = SIZE_MAX;
    for (size_t k = links.size(); k-- > 0;) {
        size_t at = links[k] + 2;   // past ?. and the member's name
        if (at < chain_end && tokens[at].type == token_type::left_paren) { at = group_end(at); }
        if (at < chain_end && tokens[at].type != token_type::question_dot) { link = links[k]; break; }
    }
    if (link == SIZE_MAX) { return expr; }

    bool pure = true;
    for (size_t i = chain_start; i < link && pure; ++i) {
        switch (tokens[i].type) {
            case token_type::left_paren: case token_type::left_brace: case token_type::plus_plus: case token_type::minus_minus:
            case token_type::equal: case token_type::plus_equal: case token_type::minus_equal: case token_type::star_equal:
            case token_type::slash_equal: case token_type::percent_equal: case token_type::ampersand_equal: case token_type::pipe_equal:
            case token_type::caret_equal: case token_type::left_shift_equal: case token_type::right_shift_equal:
            case token_type::question_question_equal: case token_type::new_keyword: case token_type::function_keyword:
            case token_type::fat_arrow: case token_type::yield_keyword: case token_type::throw_keyword: case token_type::cast_keyword:
                pure = false;
                break;
            default:
                break;
        }
    }

    const source_location at = tokens[link].location;
    auto synth = [&](token_type t, std::string_view lexeme) { return token(t, lexeme, at); };
    std::vector<token> rewritten;
    rewritten.reserve((chain_end - chain_start) * 2 + 16);
    rewritten.push_back(synth(token_type::left_paren, "("));
    if (pure) {
        rewritten.push_back(synth(token_type::null_keyword, "null"));
        rewritten.push_back(synth(token_type::bang_equal, "!="));
        rewritten.push_back(synth(token_type::left_paren, "("));
        rewritten.insert(rewritten.end(), tokens.begin() + chain_start, tokens.begin() + link);
        rewritten.push_back(synth(token_type::right_paren, ")"));
        rewritten.push_back(synth(token_type::question, "?"));
        rewritten.insert(rewritten.end(), tokens.begin() + chain_start, tokens.begin() + link);
    } else {
        const auto [held_id, held_name] = hidden_name("chain");
        const token held(token_type::identifier, held_name, held_id, at);
        rewritten.push_back(synth(token_type::left_paren, "("));
        rewritten.push_back(held);
        rewritten.push_back(synth(token_type::right_paren, ")"));
        rewritten.push_back(synth(token_type::fat_arrow, "=>"));
        rewritten.push_back(synth(token_type::null_keyword, "null"));
        rewritten.push_back(synth(token_type::bang_equal, "!="));
        rewritten.push_back(held);
        rewritten.push_back(synth(token_type::question, "?"));
        rewritten.push_back(held);
    }
    rewritten.push_back(synth(token_type::dot, "."));
    rewritten.insert(rewritten.end(), tokens.begin() + link + 1, tokens.begin() + chain_end);
    rewritten.push_back(synth(token_type::colon, ":"));
    rewritten.push_back(synth(token_type::null_keyword, "null"));
    rewritten.push_back(synth(token_type::right_paren, ")"));
    if (!pure) {
        rewritten.push_back(synth(token_type::left_paren, "("));
        rewritten.insert(rewritten.end(), tokens.begin() + chain_start, tokens.begin() + link);
        rewritten.push_back(synth(token_type::right_paren, ")"));
    }
    rewritten.push_back(synth(token_type::eof, ""));

    const std::vector<token>* saved_stream = stream_;
    const size_t saved_current = current_;
    stream_ = &rewritten;
    current_ = 0;
    auto parsed = expression();
    const bool whole = parsed && is_at_end();
    stream_ = saved_stream;
    current_ = saved_current;
    pushed_back_token_.reset();
    paren_close_.erase(&rewritten);
    if (!parsed) { return parsed.error_value(); }
    if (!whole) {
        report_error("unexpected tokens in an optional chain", tokens[link]);
        return make_error_code(parse_error_code::syntax_error);
    }
    return parsed.value();
}

// ++x / x++ on a non-identifier target (element, entry, member, nested) IS `x += 1` /
// `x -= 1` through the compound-assignment machinery both backends already share for
// every target kind, typed lane and error text; identifiers keep their dedicated unary
// path. Postfix marks the node so the expression result is the old value.
expression_ptr parser::desugar_incdec(const token& op, const expression_ptr& target, bool postfix) {
    const bool increment = op.type == token_type::plus_plus;
    token compound(increment ? token_type::plus_equal : token_type::minus_equal, increment ? "+=" : "-=", op.location);
    script_value one(script_value::ast_literal_tag{}, static_cast<script_int>(1));
    auto node = std::make_shared<assignment_expr>(op.location, lvalue_target(target), compound,
                                                  std::make_shared<literal_expr>(op.location, one));
    node->postfix_old_result = postfix;
    node->incdec_desugar = true;
    return node;
}

// The C++ lvalues past names, members, elements and cast<T&> as an assignment target: a conditional of lvalues reads its
// arms as references, an assignment yields the reference it stored through, and a prefix ++/-- on a name is that
// assignment (`x += 1` yielding x); anything else stays as written and raises as a non-lvalue
expression_ptr parser::lvalue_target(const expression_ptr& target) {
    if (!detail::lvalue_capable(target.get())) { return target; }
    switch (target->get_type()) {
        case node_type::ternary_expr: {
            auto* c = static_cast<ternary_expr*>(target.get());
            c->then_expression = bound_reference(c->then_expression);
            c->else_expression = bound_reference(c->else_expression);
            return target;
        }
        case node_type::assignment_expr: {
            auto* a = static_cast<assignment_expr*>(target.get());
            a->target = bound_reference(a->target);
            a->yields_reference = true;
            return target;
        }
        case node_type::unary_expr: {
            auto* u = static_cast<unary_expr*>(target.get());
            auto node = std::static_pointer_cast<assignment_expr>(desugar_incdec(u->op, u->operand, false));
            node->target = bound_reference(u->operand);
            node->yields_reference = true;
            return node;
        }
        case node_type::binary_expr:
        case node_type::member_expr: {
            // an element or member of a conditional of lvalues names storage in the arm it picks: (c ? a : b)[i] = v
            jai::expression* container = target.get();
            while (true) {
                if (container->get_type() == node_type::binary_expr && static_cast<binary_expr*>(container)->op.type == token_type::left_bracket) {
                    container = static_cast<binary_expr*>(container)->left.get();
                } else if (container->get_type() == node_type::member_expr && !static_cast<member_expr*>(container)->is_static) {
                    container = static_cast<member_expr*>(container)->object.get();
                } else {
                    break;
                }
            }
            if (container->get_type() == node_type::ternary_expr && detail::lvalue_capable(container)) {
                auto* c = static_cast<ternary_expr*>(container);
                c->then_expression = bound_reference(c->then_expression);
                c->else_expression = bound_reference(c->else_expression);
            }
            return target;
        }
        default:
            return target;
    }
}

expression_ptr parser::bound_reference(const expression_ptr& piece) {
    if (piece->get_type() == node_type::cast_expr && static_cast<cast_expr*>(piece.get())->by_reference) { return piece; }
    auto cast = std::make_shared<cast_expr>(piece->location, store_type_info(type_info::make_any(*symbolizer_)), true, lvalue_target(piece));
    cast->implicit_ref = true;
    return cast;
}

// A conditional whose arms both name storage, where a reference may bind it (an argument, a T& declaration, default or return,
// a cast<T&> operand), reads its arms as references, so the binding names the variable the condition picks (C++)
expression_ptr parser::bound_conditional(expression_ptr value) {
    if (!detail::conditional_names_storage(value.get())) { return value; }
    auto* c = static_cast<ternary_expr*>(value.get());
    for (expression_ptr* arm : {&c->then_expression, &c->else_expression}) {
        if ((*arm)->get_type() == node_type::ternary_expr) {
            *arm = bound_conditional(*arm);
        } else if ((*arm)->get_type() != node_type::cast_expr || !static_cast<cast_expr*>(arm->get())->by_reference) {
            // a member may name no storage (an enumerator, a computed property): the arm keeps its value then
            auto cast = std::make_shared<cast_expr>((*arm)->location, store_type_info(type_info::make_any(*symbolizer_)), true, *arm);
            cast->implicit_ref = true;
            cast->binding_ref = true;
            *arm = cast;
        }
    }
    return value;
}

checked_result<declaration_ptr> parser::declaration() {
    if (!check_mode_attribute_ahead()) { return declaration_body(); }
    return attributed_declaration();
}

JAI_NOINLINE checked_result<declaration_ptr> parser::attributed_declaration() {
    if (nodiscard_attribute_ahead()) {
        const source_location at = peek().location;
        current_ += 5;
        JAISCRIPT_TRY_ASSIGN(declaration_ptr decl, declaration());
        if (!mark_nodiscard(decl, at)) { return make_error_code(parse_error_code::syntax_error); }
        return decl;
    }
    JAISCRIPT_TRY_ASSIGN(uint8_t mode, check_mode_attribute());
    if (is_at_end() || check(token_type::right_brace)) {
        // nothing follows: the attribute covers an empty block
        auto empty = std::make_shared<block_stmt>(previous().location, std::vector<declaration_ptr>{});
        auto wrapped = std::make_shared<statement_decl>(empty->location, empty);
        wrapped->check_mode_attr = mode;
        return wrapped;
    }
    JAISCRIPT_TRY_ASSIGN(declaration_ptr decl, nodiscard_attribute_ahead() ? declaration() : declaration_body());
    if (decl) { decl->check_mode_attr = mode; }
    return decl;
}

checked_result<declaration_ptr> parser::declaration_body() {
    stack_.guard(peek().location);
    switch (peek().type) {   // the statements, which nest, and which no other declaration form below begins with
        case token_type::left_brace:
            if (nesting_depth_ != 0 || !json_map_ahead()) { return statement_declaration(); }
            break;
        case token_type::if_keyword: case token_type::while_keyword: case token_type::for_keyword: case token_type::parallel_for_keyword:
        case token_type::return_keyword: case token_type::break_keyword: case token_type::continue_keyword: case token_type::try_keyword:
        case token_type::switch_keyword: case token_type::fallthrough_keyword:
            return statement_declaration();
        default:
            break;
    }
    return declaration_rest();
}

JAI_NOINLINE checked_result<declaration_ptr> parser::statement_declaration() {
    JAISCRIPT_TRY_ASSIGN(auto stmt, statement());
    return std::make_shared<statement_decl>(stmt->location, stmt);
}

JAI_NOINLINE checked_result<declaration_ptr> parser::declaration_rest() {
    if (do_while_ahead()) {
        JAISCRIPT_TRY_ASSIGN(auto stmt, statement());
        return std::make_shared<statement_decl>(stmt->location, stmt);
    }
    if (foreign_word_at_statement()) { return make_error_code(parse_error_code::syntax_error); }
    if (check(token_type::const_keyword)) { return const_declaration(); }

    if (match(token_type::nocopy_keyword)) {
        const token at = previous();
        JAISCRIPT_TRY(expect(token_type::class_keyword, "expected 'class' after 'nocopy'"));
        reject_class_in_function_body(at);
        JAISCRIPT_TRY_ASSIGN(auto result, class_declaration(true));
        match(token_type::semicolon);
        return result;
    }
    if (match(token_type::class_keyword)) {
        reject_class_in_function_body(previous());
        JAISCRIPT_TRY_ASSIGN(auto result, class_declaration());
        match(token_type::semicolon);  // Consume optional semicolon after class
        return result;
    }
    if (match(token_type::namespace_keyword)) {
        return namespace_declaration();
    }

    if (match(token_type::extern_keyword)) {
        return extern_declaration();
    }
    if (match(token_type::inline_keyword)) {
        return inline_variable_declaration();
    }

    // Check for include/import directives
    if (match(token_type::include_keyword)) {
        JAISCRIPT_TRY_ASSIGN(auto result, include_declaration());
        match(token_type::semicolon);  // Consume optional trailing semicolon
        return result;
    }
    if (match(token_type::import_keyword)) {
        JAISCRIPT_TRY_ASSIGN(auto result, import_declaration());
        match(token_type::semicolon);  // Consume optional trailing semicolon
        return result;
    }

    // Check for enum keyword
    if (match(token_type::enum_keyword)) {
        if (check(token_type::identifier) && reject_using_name(peek(), "enum")) { return make_error_code(parse_error_code::syntax_error); }
        JAISCRIPT_TRY_ASSIGN(token name_tok, consume(token_type::identifier, "expected enum name"));
        auto [name_id, name_view] = symbolizer_->intern_with_view(name_tok.lexeme);
        JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' after enum name"));

        auto decl = std::make_shared<enum_decl>(name_tok.location, name_view, name_id);

        if (!check(token_type::right_brace)) {
            do {
                const bool keyword_value = is_declarable_keyword_name(peek().type) && current_ + 1 < stream_->size() &&
                                           ((*stream_)[current_ + 1].type == token_type::comma || (*stream_)[current_ + 1].type == token_type::right_brace ||
                                            (*stream_)[current_ + 1].type == token_type::equal);
                JAISCRIPT_TRY_ASSIGN(token val_tok, keyword_value ? checked_result<token>(advance()) : consume(token_type::identifier, "expected enum value name"));
                auto [val_id, val_view] = symbolizer_->intern_with_view(val_tok.lexeme);
                decl->values.push_back({val_view, val_id});
                script_int number = 0;
                if (match(token_type::equal)) {
                    const token& at = peek();
                    JAISCRIPT_TRY_ASSIGN(expression_ptr init, expression());
                    std::string why;
                    if (!enum_initializer_value(init.get(), *decl, decl->numbers, number, why)) {
                        report_error("enum value '" + std::string(val_tok.lexeme) + "' " + why, at);
                        return make_error_code(parse_error_code::syntax_error);
                    }
                } else if (!decl->numbers.empty() && !ints::try_add(decl->numbers.back(), 1, number)) {
                    report_error("enum value '" + std::string(val_tok.lexeme) + "' overflows the int range: the value before it is the largest int", val_tok);
                    return make_error_code(parse_error_code::syntax_error);
                }
                decl->numbers.push_back(number);
            } while (match(token_type::comma));
        }

        JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after enum values"));
        return decl;
    }

    if (match(token_type::coroutine_keyword)) {
        const bool was_in_coroutine = in_coroutine_;
        in_coroutine_ = true;
        coroutine_body_pending_ = true;
        auto parsed = bare_coroutine_head() ? bare_coroutine_declaration() : function_declaration();
        in_coroutine_ = was_in_coroutine;
        coroutine_body_pending_ = false;
        JAISCRIPT_TRY_ASSIGN(auto result, std::move(parsed));
        auto* func = static_cast<function_decl*>(result.get());
        func->is_coroutine = true;
        // A suspended fiber's return channel is a value channel: ref returns are
        // function-only (the epilogue would flatten anyway - make it explicit)
        if (func->return_type && func->return_type->base_type == script_value_type::jai_reference_type) {
            report_error("coroutines cannot return references", previous());
            return make_error_code(parse_error_code::syntax_error);
        }
        if (reject_coroutine_reference_parameter(*func)) { return make_error_code(parse_error_code::syntax_error); }
        match(token_type::semicolon);
        return result;
    }

    if (check(token_type::function_keyword) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::star) {
        report_foreign(detail::foreign_word::generator_function, peek());
        return make_error_code(parse_error_code::syntax_error);
    }

    // Check specifically for function keyword to handle function declarations
    // (`function (` falls through to expression parsing — anonymous function expression)
    if (check(token_type::function_keyword) &&
        !(current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::left_paren)) {
        // `function name = expr;` / `function name;` declares a callable-or-null VARIABLE
        // (the parameter's and the namespace path's type); `function name(` stays a function declaration.
        if (current_ + 2 < stream_->size() &&
            (*stream_)[current_ + 1].type == token_type::identifier &&
            ((*stream_)[current_ + 2].type == token_type::equal ||
             (*stream_)[current_ + 2].type == token_type::semicolon)) {
            return variable_declaration();
        }
        JAISCRIPT_TRY_ASSIGN(auto result, function_declaration());
        // After parsing a function, consume optional semicolon but don't require it
        match(token_type::semicolon);
        return result;
    }

    // Check for explicit type keywords that start declarations
    if (match({token_type::auto_keyword, token_type::var_keyword, token_type::int_keyword, token_type::float_keyword,
               token_type::string_keyword, token_type::bool_keyword, token_type::char_keyword, token_type::void_keyword,
               token_type::array_keyword, token_type::map_keyword, token_type::weak_ptr_keyword, token_type::shared_ptr_keyword})) {

        // Check for destructuring: auto [x, y, z] = expr;
        if ((previous().type == token_type::auto_keyword || previous().type == token_type::var_keyword)
            && check(token_type::left_bracket)) {
            return destructuring_declaration();
        }

        // Check if this keyword is followed by :: - if so, it's a namespace access, not a type
        if (check(token_type::colon_colon)) {
            // This is a namespace access expression like string::length(), not a variable declaration
            // Back up and let it fall through to expression parsing
            current_--;
            // Fall through to expression parsing below
        } else {
            // We already consumed the type keyword, so we need to backtrack
            current_--;

            // Look ahead to determine if this is a function or variable declaration
            size_t savedPos = current_;
            JAISCRIPT_TRY(parse_type()); // consume the type

        // Skip optional & for reference types
        if (check(token_type::ampersand)) {
            advance();
        }

        // Check if this is followed by an identifier (variable/function name)
        if (check(token_type::identifier)) {
            advance(); // consume identifier
            if (check(token_type::left_paren) && !paren_group_ends_statement(current_)) {
                // This is a function declaration
                current_ = savedPos;
                return function_declaration();
            }
            // Otherwise it's a variable declaration
            current_ = savedPos;
            return variable_declaration();
        } else if (check(token_type::left_paren) || check(token_type::left_brace)) {
            // This is a constructor expression like array<int>() or array<int>{}
            current_ = savedPos;
            // Fall through to parse as expression
            } else {
                // No identifier after type, so it's a variable declaration
                current_ = savedPos;
                return variable_declaration();
            }
        }
    }

    // Check if this might be a type name followed by an identifier (for custom types)
    // We need to look ahead to see if this is a declaration
    if (check(token_type::identifier)) {
        // Save current position
        size_t savedPos = current_;
        std::string firstIdentifier(peek().lexeme);

        // Look ahead to see if this is "identifier identifier" pattern
        advance(); // consume first identifier

        // ns::Type name ... / ns::Type& name ...: a declaration with a qualified class type, by the same rules as below
        size_t after = current_;
        while (after + 1 < stream_->size() && (*stream_)[after].type == token_type::colon_colon &&
               (*stream_)[after + 1].type == token_type::identifier) {
            after += 2;
        }
        if (after != current_ && after + 1 < stream_->size()) {
            const token_type next = (*stream_)[after].type;
            if (next == token_type::identifier) {
                const bool is_function = (*stream_)[after + 1].type == token_type::left_paren && !paren_group_ends_statement(after + 1);
                current_ = savedPos;
                return is_function ? function_declaration() : variable_declaration();
            }
            if (next == token_type::ampersand && after + 2 < stream_->size() && (*stream_)[after + 1].type == token_type::identifier) {
                const token_type then = (*stream_)[after + 2].type;
                if (then == token_type::left_paren || then == token_type::equal || then == token_type::semicolon) {
                    current_ = savedPos;
                    return then == token_type::left_paren ? function_declaration() : variable_declaration();
                }
            }
        }

        if (check(token_type::less) && is_registered_template_type(firstIdentifier)) {
            // Point<int> p; / Point<int>& r = q; / Point<int> make() {...}: the same declaration rules as a plain class name;
            // a failed type parse is an expression (Point < x)
            current_ = savedPos;
            const size_t errors_before = errors_.size();
            const std::optional<token> pushed_before = pushed_back_token_;
            auto type_result = parse_type();
            if (type_result && type_result.value() && errors_.size() == errors_before && !pushed_back_token_) {
                const bool reference = check(token_type::ampersand);
                const size_t name_at = current_ + (reference ? 1 : 0);
                if (name_at + 1 < stream_->size() && (*stream_)[name_at].type == token_type::identifier) {
                    const token_type then = (*stream_)[name_at + 1].type;
                    const bool is_function = then == token_type::left_paren && !paren_group_ends_statement(name_at + 1);
                    if (!reference || then == token_type::left_paren || then == token_type::equal || then == token_type::semicolon) {
                        current_ = savedPos;
                        return is_function ? function_declaration() : variable_declaration();
                    }
                }
            }
            current_ = savedPos;
            errors_.resize(errors_before);
            pushed_back_token_ = pushed_before;
        } else if (check(token_type::identifier) && !(peek().lexeme == "instanceof" && current_ + 1 < stream_->size() &&
                                                      (*stream_)[current_ + 1].type == token_type::identifier)) {   // x instanceof T ... is an expression
            // Pattern: identifier identifier - a declaration. A '(' after the second
            // identifier makes it a class-typed free function (C++ return-type-first,
            // same as the namespace path); otherwise a variable: Point p; / Point mk() {...}
            advance(); // consume second identifier
            const bool is_function = check(token_type::left_paren) && !paren_group_ends_statement(current_);
            current_ = savedPos;
            if (is_function) {
                return function_declaration();
            }
            return variable_declaration();
        } else if (check(token_type::ampersand) &&
                   current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::identifier &&
                   current_ + 2 < stream_->size() &&
                   ((*stream_)[current_ + 2].type == token_type::left_paren ||
                    (*stream_)[current_ + 2].type == token_type::equal ||
                    (*stream_)[current_ + 2].type == token_type::semicolon)) {
            // Pattern: ClassName& name( - a class-typed reference-return free function
            // (Box& get() { ... }); ClassName& name = / ClassName& name; - a class-typed
            // reference variable (Base& b = derived;). Declaration wins like the
            // identifier-identifier form above (C++: whatever can be a declaration is one);
            // a bitwise-and expression spells it (Box) & get().
            const bool is_function = (*stream_)[current_ + 2].type == token_type::left_paren;
            current_ = savedPos;
            if (is_function) {
                return function_declaration();
            }
            return variable_declaration();
        } else {
            // Not a simple declaration pattern, restore and parse as expression
            current_ = savedPos;
            // Fall through to parse as expression
        }
    }

    // a '{' that starts a statement opens a block, as in C++, except a map literal spelled with keys at a unit's top level: the
    // unit's value (a data file include reads as its value, execute's result)
    if (check(token_type::left_brace)) {
        if (nesting_depth_ == 0 && json_map_ahead()) {
            JAISCRIPT_TRY_ASSIGN(auto expr, expression());
            if (!is_at_end()) {
                JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after expression"));
            }
            return std::make_shared<expression_decl>(expr->location, expr);
        }
        JAISCRIPT_TRY_ASSIGN(auto stmt, statement());
        return std::make_shared<statement_decl>(stmt->location, stmt);
    }

    // Check for other statements that can appear at top level
    if (check(token_type::if_keyword) || check(token_type::while_keyword) || check(token_type::for_keyword) ||
        check(token_type::parallel_for_keyword) ||
        check(token_type::return_keyword) || check(token_type::break_keyword) || check(token_type::continue_keyword) ||
        check(token_type::try_keyword) || check(token_type::switch_keyword) || check(token_type::fallthrough_keyword)) {
        // We need to wrap the statement in a declaration since parse() returns declarations
        JAISCRIPT_TRY_ASSIGN(auto stmt, statement());
        // Create a statement_decl to wrap statements at the top level
        return std::make_shared<statement_decl>(stmt->location, stmt);
    }

    // Otherwise it's a top-level expression statement
    JAISCRIPT_TRY_ASSIGN(auto expr, expression());

    // Allow semicolon to be optional at end of file for single expressions
    if (!is_at_end()) {
        JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after expression"));
    }

    // Create an expression_decl for top-level expressions
    return std::make_shared<expression_decl>(expr->location, expr);
}

// Helper method implementations
JAI_NOINLINE checked_result<expression_ptr> parser::finish_call(expression_ptr callee) {
    std::vector<expression_ptr> arguments;

    if (!check(token_type::right_paren)) {
        // Reserve capacity for common case of 2-4 arguments
        arguments.reserve(4);
        do {
            JAISCRIPT_TRY_ASSIGN(auto arg, expression());
            arguments.push_back(bound_conditional(std::move(arg)));
        } while (match(token_type::comma));
    }

    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after arguments"));
    return std::make_shared<call_expr>(previous().location, callee, std::move(arguments));
}

JAI_NOINLINE checked_result<expression_ptr> parser::finish_member_access(expression_ptr object, bool is_arrow) {
    if (is_keyword_word(peek().type)) {
        token name = advance();
        return std::make_shared<member_expr>(name.location, object, name.lexeme, get_symbol_id(name), is_arrow);
    }
    JAISCRIPT_TRY_ASSIGN(token name, consume_name("expected member name", "member"));
    uint64_t member_id = get_symbol_id(name);
    return std::make_shared<member_expr>(name.location, object, name.lexeme, member_id, is_arrow);
}

checked_result<std::vector<parameter>> parser::parse_parameter_list() {
    std::vector<parameter> params;

    if (!check(token_type::right_paren)) {
        do {
            const bool is_const = match(token_type::const_keyword);
            if (is_const && check(token_type::const_keyword)) {
                report_error("'const' appears once in a declaration", peek());
                return make_error_code(parse_error_code::syntax_error);
            }

            type_info_ptr type = nullptr;
            std::string name;
            bool is_reference = false;

            // Check for :name syntax (shorthand for auto: name)
            if (match(token_type::colon)) {
                // :name means auto type
                type = nullptr; // nullptr means auto
                JAISCRIPT_TRY_ASSIGN(token name_tok, consume_name("expected parameter name after ':'", "parameter"));
                name = name_tok.lexeme;
            }
            // Check for type: name syntax
            else if (check(token_type::identifier) || check(token_type::auto_keyword) || check(token_type::var_keyword) ||
                     check(token_type::function_keyword) || check(token_type::int_keyword) || check(token_type::float_keyword) ||
                     check(token_type::string_keyword) || check(token_type::bool_keyword) || check(token_type::char_keyword) ||
                     check(token_type::void_keyword) || check(token_type::array_keyword) || check(token_type::map_keyword) ||
                     check(token_type::weak_ptr_keyword) || check(token_type::shared_ptr_keyword)) {

                JAISCRIPT_TRY_ASSIGN(type, parse_type());

                if (match(token_type::colon)) {
                    // type: name syntax
                    JAISCRIPT_TRY_ASSIGN(token name_tok, consume_name("expected parameter name after ':'", "parameter"));
                    name = name_tok.lexeme;
                } else if (check(token_type::ampersand) || check(token_type::identifier)) {
                    // Traditional type name syntax
                    // Check for reference
                    is_reference = match(token_type::ampersand);
                    JAISCRIPT_TRY_ASSIGN(token name_tok, consume_name("expected parameter name", "parameter"));
                    name = name_tok.lexeme;
                } else if (check(token_type::comma) || check(token_type::right_paren) || check(token_type::equal)) {
                    // No identifier after type: the type was the name of an untyped parameter, a var (foo(x), foo(x = 3); the '='
                    // is consumed by the shared default parse below)
                    if (type && type->base_type == script_value_type::jai_object_type && !type->type_name.empty() &&
                        type->type_name.find("::") == std::string::npos) {
                        name = type->type_name;
                        type = store_type_info(type_info::make_any(*symbolizer_));
                    } else {
                        report_expected_name("expected parameter name", "parameter");
    return make_error_code(parse_error_code::syntax_error);
                    }
                } else {
                    report_expected_name("expected parameter name", "parameter");
    return make_error_code(parse_error_code::syntax_error);
                }
            }
            // No type specified, error
            else {
                report_error("expected parameter type or ':' for auto parameter", peek());
    return make_error_code(parse_error_code::syntax_error);
            }

            parameter param(type, name, is_reference, is_const);
            param.symbol_id = symbolizer_->intern(name);

            if (match(token_type::equal)) {
                JAISCRIPT_TRY_ASSIGN(param.default_value, expression());
                if (is_reference) { param.default_value = bound_conditional(std::move(param.default_value)); }
            }

            params.push_back(std::move(param));
        } while (match(token_type::comma));
    }

    bool seen_default = false;
    for (const auto& p : params) {
        if (p.default_value) {
            seen_default = true;
        } else if (seen_default) {
            report_error("parameter without default value cannot follow parameter with default value", peek());
            return make_error_code(parse_error_code::syntax_error);
        }
    }

    return params;
}

// Lambda expression parsing
checked_result<expression_ptr> parser::lambda_expression() {
    auto lambda = std::make_shared<lambda_expr>(peek().location);
    JAISCRIPT_TRY(lambda_captures(*lambda));
    return finish_lambda_after_captures(std::move(lambda));
}

JAI_NOINLINE checked_result<void> parser::lambda_captures(lambda_expr& lambda) {
    JAISCRIPT_TRY(expect(token_type::left_bracket, "expected '[' for lambda"));
    JAISCRIPT_TRY_ASSIGN(auto capture_result, parse_capture_list());
    lambda.captures = std::move(capture_result.first);
    lambda.default_capture = capture_result.second;
    for (auto& cap : lambda.captures) {
        if (cap.symbol_id == UINT64_MAX && symbolizer_) cap.symbol_id = symbolizer_->intern(cap.name);
        if (cap.symbol_id != UINT64_MAX && !cap.initializer && !template_barriers_.empty() && template_hides(cap.symbol_id)) {
            report_error("a lambda in a template captures '" + std::string(cap.name) + "', which the template does not see", previous());
        }
        if (cap.symbol_id != UINT64_MAX && !cap.initializer) cap.outer_slot = lookup_capture_slot(cap.symbol_id);   // an init-capture names no outer variable
    }
    return expect(token_type::right_bracket, "expected ']' after capture list");
}

// Anonymous function expression: `function (params) {...}` closes over its scope as JavaScript does, by reference -
// a desugar to a [&] lambda
checked_result<expression_ptr> parser::anonymous_function_expression() {
    auto lambda = std::make_shared<lambda_expr>(previous().location);
    lambda->default_capture = lambda_expr::capture_default::by_reference;
    return finish_lambda_after_captures(std::move(lambda));
}

// Shared tail for [] lambdas and anonymous function expressions: parameters,
// optional trailing return, scope/slot bookkeeping, body
checked_result<expression_ptr> parser::finish_lambda_after_captures(std::shared_ptr<lambda_expr> lambda) {
    JAISCRIPT_TRY(lambda_signature(*lambda));
    return finish_lambda_body(std::move(lambda), false);
}

// The parameters and the optional trailing return type
JAI_NOINLINE checked_result<void> parser::lambda_signature(lambda_expr& lambda) {
    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' for lambda parameters"));
    JAISCRIPT_TRY_ASSIGN(lambda.parameters, parse_parameter_list());
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after parameters"));
    if (const_method_suffix()) { return make_error_code(parse_error_code::syntax_error); }

    if (match(token_type::arrow)) {
        // -> { means auto return type (nullptr)
        if (check(token_type::left_brace)) {
            lambda.return_type = nullptr;
        } else {
            JAISCRIPT_TRY_ASSIGN(lambda.return_type, parse_type());
            lambda.return_type = wrap_reference_return_type(lambda.return_type);
        }
    }
    return {};
}

namespace {
    constexpr uint32_t k_paren_unknown = 0xFFFFFFFFu;
    constexpr uint32_t k_paren_unclosed = 0xFFFFFFFEu;
}

bool parser::arrow_function_ahead() const {
    const std::vector<token>& tokens = *stream_;
    if (pushed_back_token_) return false;
    if (check(token_type::identifier)) {
        return current_ + 1 < tokens.size() && tokens[current_ + 1].type == token_type::fat_arrow;
    }
    if (!check(token_type::left_paren)) return false;
    const uint32_t close = paren_close(current_);
    return close != k_paren_unclosed && close + 1 < tokens.size() && tokens[close + 1].type == token_type::fat_arrow;
}

// A ';', '{', '}' or the end before a group closes leaves it and every group open around it unclosed; a group already closed is
// stepped over whole, so a stream's parentheses are walked once
uint32_t parser::paren_close(size_t open) const {
    const std::vector<token>& tokens = *stream_;
    std::vector<uint32_t>& close = paren_close_[stream_];
    if (close.size() != tokens.size()) { close.assign(tokens.size(), k_paren_unknown); }
    if (close[open] != k_paren_unknown) { return close[open]; }
    std::vector<size_t> opened{open};
    for (size_t i = open + 1; i < tokens.size() && !opened.empty(); ++i) {
        const token_type t = tokens[i].type;
        if (t == token_type::left_paren) {
            if (close[i] == k_paren_unknown) {
                opened.push_back(i);
            } else if (close[i] == k_paren_unclosed) {
                break;
            } else {
                i = close[i];
            }
        } else if (t == token_type::right_paren) {
            close[opened.back()] = static_cast<uint32_t>(i);
            opened.pop_back();
        } else if (t == token_type::semicolon || t == token_type::left_brace || t == token_type::right_brace || t == token_type::eof) {
            break;
        }
    }
    for (size_t at : opened) { close[at] = k_paren_unclosed; }
    return close[open];
}

// (params) => expr / (params) => { body } / name => expr: the JavaScript and C# arrow function, a function expression that
// closes over its scope by reference like function(...) does
checked_result<expression_ptr> parser::arrow_function_expression() {
    auto lambda = std::make_shared<lambda_expr>(peek().location);
    lambda->default_capture = lambda_expr::capture_default::by_reference;
    if (check(token_type::identifier)) {
        token name = advance();
        parameter param(store_type_info(type_info::make_any(*symbolizer_)), std::string(name.lexeme));
        param.symbol_id = get_symbol_id(name);
        lambda->parameters.push_back(std::move(param));
    } else {
        JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' for arrow function parameters"));
        JAISCRIPT_TRY_ASSIGN(lambda->parameters, parse_parameter_list());
        JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after arrow function parameters"));
    }
    JAISCRIPT_TRY(expect(token_type::fat_arrow, "expected '=>' after arrow function parameters"));
    return finish_lambda_body(std::move(lambda), true);
}

checked_result<expression_ptr> parser::finish_lambda_body(std::shared_ptr<lambda_expr> lambda, bool expression_body) {
    jump_scope jumps_of_body(jumps_, {});
    // Enter function scope for slot-based local variable tracking
    enter_function_scope(lambda.get());

    // Named captures live in the closure environment, not this function's frame.
    // Installing their names also makes an init-capture shadow its enclosing name.
    for (const auto& capture : lambda->captures) {
        if (capture.symbol_id != UINT64_MAX) {
            function_scope_stack_.back().symbol_to_slot[capture.symbol_id] = SIZE_MAX;
        }
    }

    // Assign slots to parameters (they get slots 0, 1, 2, ...)
    for (auto& param : lambda->parameters) {
        if (param.symbol_id != UINT64_MAX) {
            param.slot_index = allocate_slot(param.symbol_id);
        }
    }

    // Parse body (never a coroutine body: `yield` inside a lambda is the ordinary parse error)
    struct coroutine_scope { bool& flag; bool saved; ~coroutine_scope() { flag = saved; } } body_scope{in_coroutine_, in_coroutine_};
    in_coroutine_ = false;
    coroutine_scope ctor_scope{in_constructor_, in_constructor_};
    in_constructor_ = false;
    coroutine_scope ref_return_scope{returns_reference_, returns_reference_};
    returns_reference_ = lambda->return_type && lambda->return_type->base_type == script_value_type::jai_reference_type;
    if (expression_body && !check(token_type::left_brace)) { return lambda_expression_body(std::move(lambda)); }
    if (check(token_type::identifier) && peek().lexeme == "mutable") {
        report_foreign(detail::foreign_word::mutable_lambda, peek());
        advance();   // the body still parses, so the one error is this one
    }
    if (!match(token_type::left_brace)) {
        exit_function_scope();  // Clean up on error
        report_error("expected '{' for lambda body", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    auto body_stmt = block_statement();
    if (!body_stmt) {
        exit_function_scope();   // a failed body leaves no scope naming the lambda that dies with this return
        return body_stmt.error_value();
    }
    lambda->body = std::dynamic_pointer_cast<block_stmt>(body_stmt.value());

    // Exit function scope and record total slot count
    lambda->local_count = exit_function_scope();

    return lambda;
}

// An arrow function's one expression, its value returned; finish_lambda_body has entered the function scope
JAI_NOINLINE checked_result<expression_ptr> parser::lambda_expression_body(std::shared_ptr<lambda_expr> lambda) {
    const source_location at = peek().location;
    auto value = expression();
    if (!value) {
        exit_function_scope();
        return value.error_value();
    }
    std::vector<declaration_ptr> body;
    auto ret = std::make_shared<return_stmt>(at, returns_reference_ ? bound_conditional(value.value()) : value.value());
    body.push_back(std::make_shared<statement_decl>(at, ret));
    lambda->body = std::make_shared<block_stmt>(at, std::move(body));
    lambda->local_count = exit_function_scope();
    return lambda;
}

checked_result<std::pair<std::vector<lambda_expr::capture>, lambda_expr::capture_default>> parser::parse_capture_list() {
    std::vector<lambda_expr::capture> captures;
    lambda_expr::capture_default default_mode = lambda_expr::capture_default::none;

    // Helper lambda to parse a capture variable name (identifier or 'this')
    // Returns interned string_view to ensure permanent storage
    auto parse_capture_name = [this]() -> checked_result<std::string_view> {
        if (check(token_type::this_keyword)) {
            advance();  // consume 'this'
			check_instance_capture(previous());
            // "this" is pre-interned in symbolizer
            return symbolizer_->get_string(symbolizer_->get_this_id());
        } else {
            JAISCRIPT_TRY_ASSIGN(token name_tok, consume(token_type::identifier, "expected capture variable name or 'this'"));
            // Token lexeme already points to symbolizer storage for identifiers
            return name_tok.lexeme;
        }
    };

    // One named entry: `name`, `&name`, or the init-capture `name = expression` (evaluated at creation in the
    // enclosing scope into the lambda's own name; a reference capture takes no initializer)
    auto push_capture = [&](std::string_view name, bool byRef) -> checked_result<void> {
        lambda_expr::capture cap(name, byRef);
        if (check(token_type::equal)) {
            if (byRef || name == symbolizer_->get_string(symbolizer_->get_this_id())) {
                report_error(byRef ? "a reference capture takes no initializer" : "'this' takes no initializer", peek());
                return make_error_code(parse_error_code::syntax_error);
            }
            advance();   // consume '='
            JAISCRIPT_TRY_ASSIGN(cap.initializer, expression());
        }
        captures.push_back(std::move(cap));
        return {};
    };

    if (!check(token_type::right_bracket)) {
        // Check for default capture first
        if (check(token_type::equal)) {
            // [=...] - capture all by value (with possible exceptions)
            advance(); // consume '='
            default_mode = lambda_expr::capture_default::by_value;

            // Check for exceptions after comma
            if (match(token_type::comma)) {
                do {
                    bool byRef = match(token_type::ampersand);
                    JAISCRIPT_TRY_ASSIGN(std::string_view name, parse_capture_name());
                    JAISCRIPT_TRY(push_capture(name, byRef));
                } while (match(token_type::comma));
            }
        } else if (check(token_type::ampersand)) {
            // Could be [&...] or [&variable]
            advance(); // consume '&'

            if (check(token_type::comma) || check(token_type::right_bracket)) {
                // [&...] - capture all by reference (with possible exceptions)
                default_mode = lambda_expr::capture_default::by_reference;

                // Check for exceptions after comma
                if (match(token_type::comma)) {
                    do {
                        bool byRef = false; // Explicit variables after [&,] are by value unless prefixed with &
                        if (match(token_type::ampersand)) {
                            byRef = true;
                        }
                        JAISCRIPT_TRY_ASSIGN(std::string_view name, parse_capture_name());
                        JAISCRIPT_TRY(push_capture(name, byRef));
                    } while (match(token_type::comma));
                }
            } else {
                // [&variable] - specific variable by reference
                JAISCRIPT_TRY_ASSIGN(std::string_view name, parse_capture_name());
                JAISCRIPT_TRY(push_capture(name, true));

                // Continue parsing other captures
                while (match(token_type::comma)) {
                    bool byRef = match(token_type::ampersand);
                    JAISCRIPT_TRY_ASSIGN(std::string_view varName, parse_capture_name());
                    JAISCRIPT_TRY(push_capture(varName, byRef));
                }
            }
        } else {
            // Regular explicit captures: [x, y, &z, this, w = e]
            do {
                bool byRef = match(token_type::ampersand);
                JAISCRIPT_TRY_ASSIGN(std::string_view name, parse_capture_name());
                JAISCRIPT_TRY(push_capture(name, byRef));
            } while (match(token_type::comma));
        }
    }

    return std::make_pair(captures, default_mode);
}

// statement parsing implementations
checked_result<statement_ptr> parser::statement() {
    if (!check_mode_attribute_ahead()) { return statement_body(); }
    return attributed_statement();
}

JAI_NOINLINE checked_result<statement_ptr> parser::attributed_statement() {
    JAISCRIPT_TRY_ASSIGN(uint8_t mode, check_mode_attribute());
    if (is_at_end() || check(token_type::right_brace)) {
        auto empty = std::make_shared<block_stmt>(previous().location, std::vector<declaration_ptr>{});
        empty->check_mode_attr = mode;
        return empty;
    }
    JAISCRIPT_TRY_ASSIGN(statement_ptr stmt, statement_body());
    if (stmt) { stmt->check_mode_attr = mode; }
    return stmt;
}

checked_result<statement_ptr> parser::statement_body() {
    stack_.guard(peek().location);   // if (...) if (...) recurses here without passing through declaration()
    switch (peek().type) {
        case token_type::left_brace:
            advance();
            if (json_map_ahead(0)) { return block_map_error(); }
            return block_statement();
        case token_type::identifier:
            if (do_while_ahead()) {
                advance();
                return do_while_statement();
            }
            if (foreign_word_at_statement()) { return make_error_code(parse_error_code::syntax_error); }
            break;
        case token_type::if_keyword: advance(); return if_statement();
        case token_type::while_keyword: advance(); return while_statement();
        case token_type::for_keyword: advance(); return for_statement();
        case token_type::parallel_for_keyword: advance(); return parallel_for_statement();
        case token_type::return_keyword: advance(); return return_statement();
        case token_type::break_keyword: advance(); return break_statement();
        case token_type::continue_keyword: advance(); return continue_statement();
        case token_type::try_keyword: advance(); return try_statement();
        case token_type::switch_keyword: advance(); return switch_statement();
        case token_type::import_keyword: advance(); return import_statement();
        case token_type::fallthrough_keyword: advance(); return fallthrough_statement();
        default:
            break;
    }
    return expression_statement();
}

JAI_NOINLINE checked_result<statement_ptr> parser::block_map_error() {
    report_error("a statement that starts with '{' is a block: write a map literal there in parentheses, ({...})", previous());
    return make_error_code(parse_error_code::syntax_error);
}

// `if (changed) import "m.jai";`: the declaration as a one-statement block
JAI_NOINLINE checked_result<statement_ptr> parser::import_statement() {
    const source_location at = previous().location;
    JAISCRIPT_TRY_ASSIGN(declaration_ptr imported, import_declaration());
    match(token_type::semicolon);
    return statement_ptr(std::make_shared<block_stmt>(at, std::vector<declaration_ptr>{std::move(imported)}));
}

JAI_NOINLINE checked_result<statement_ptr> parser::fallthrough_statement() {
    if (!in_switch_case_) {
        report_error("'fallthrough' can only be used inside a switch case", previous());
        return make_error_code(parse_error_code::syntax_error);
    }
    auto fallthrough = std::make_shared<fallthrough_stmt>(previous().location);
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after 'fallthrough'"));
    return fallthrough;
}

// A case body holds statements, as in C++: a declaration there names the braces it needs, once, and the switch parses on
JAI_NOINLINE checked_result<statement_ptr> parser::case_body_statement() {
    const size_t start = current_;
    const size_t errors_before = errors_.size();
    const std::optional<token> pushed_before = pushed_back_token_;
    const token at = peek();
    auto stmt = statement();
    if (stmt) { return stmt; }
    current_ = start;
    errors_.resize(errors_before);
    pushed_back_token_ = pushed_before;
    auto decl = declaration();
    if (decl && decl.value() && decl.value()->get_type() != node_type::statement_decl && errors_.size() == errors_before) {
        report_error("a declaration in a case body needs the body in braces: case value: { ... }", at);
        return statement_ptr(std::make_shared<block_stmt>(at.location, std::vector<declaration_ptr>{}));
    }
    current_ = start;
    errors_.resize(errors_before);
    pushed_back_token_ = pushed_before;
    return statement();
}

checked_result<statement_ptr> parser::block_statement() {
    stack_.guard(peek().location);
    const token& leftBrace = previous();
    std::vector<declaration_ptr> declarations;

    // Locals declared in this block stop resolving by name once it closes
    block_scope_guard block_scope(*this);
    nesting_guard nesting(nesting_depth_);

    while (!check(token_type::right_brace) && !is_at_end()) {
        JAISCRIPT_TRY_ASSIGN(declaration_ptr decl, declaration());
        if (!splice_include(decl, declarations)) { declarations.push_back(std::move(decl)); }
    }

    JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after block"));

    return std::make_shared<block_stmt>(leftBrace.location, std::move(declarations));
}

// A class is a program-scope type: a function or lambda body cannot declare one, written there or pasted in by an include
void parser::reject_class_in_function_body(const token& at) {
    if (function_scope_stack_.empty()) { return; }
    if (!splice_spelled_.empty()) {
        report_error("an include inside a function body cannot declare class '" + std::string(peek().lexeme) +
                     "' (a class is a program-scope type and the included text is the function's own); import '" + splice_spelled_.back() + "' instead", at);
        return;
    }
    report_error("a class cannot be declared inside a function body (a class is a program-scope type); declare it at program scope or import its file", at);
}

// A literal include inside a function body is textual at parse time, as C++'s is: the included file's declarations are
// parsed here, in this scope, so the function's parameters and locals are theirs and theirs are the function's, and the
// checker and both backends see one function. A computed path stays a runtime include (it runs in the environment and
// never sees the frame), an unresolved or unreadable path is left to the runtime include's own error, and a function
// without an include pays nothing.
bool parser::splice_include(const declaration_ptr& decl, std::vector<declaration_ptr>& out) {
    if (!decl || decl->get_type() != node_type::include_decl || function_scope_stack_.empty() || !engine_) return false;
    auto* inc = static_cast<include_decl*>(decl.get());
    std::string path = inc->path;
    if (inc->path_expr) {
        if (inc->path_expr->get_type() != node_type::literal_expr) return false;
        const auto* lit = static_cast<const literal_expr*>(inc->path_expr.get());
        if (lit->value.raw_storage_index() != script_value::TYPEID_STRING) return false;
        path = std::string(lit->value.unchecked_as_string());
    }
    if (!engine_->is_enabled(privilege::filesystem)) return false;
    auto resolved = resolve_include_path(path, engine_, decl->location.filename);
    if (!resolved) return false;
    if (splice_depth_ >= 32 || std::find(splice_active_.begin(), splice_active_.end(), resolved.value()) != splice_active_.end()) {
        report_error_at("include '" + path + "' includes itself or nests too deep", decl->location);
        return true;
    }
    std::ifstream file(resolved.value(), std::ios::binary);
    if (!file) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    lexer lex(content, symbolizer_, resolved.value());
    const std::vector<token> lexed = lex.tokenize();
    std::vector<token> qualified;
    const std::vector<token>& own = qualify_namespace_classes(lexed, qualified) ? qualified : lexed;
    std::vector<token> used;   // the included file's own using declarations apply to its own text only
    const std::vector<token>& tokens = apply_using_declarations(own, used) ? used : own;
    const std::vector<token>* saved_stream = stream_;
    const size_t saved_current = current_;
    const std::optional<token> saved_pushed_back = pushed_back_token_;
    stream_ = &tokens; current_ = 0; pushed_back_token_.reset();
    ++splice_depth_; splice_active_.push_back(resolved.value()); splice_spelled_.push_back(path);
    while (!is_at_end()) {
        const size_t errors_before = errors_.size();
        auto d = declaration();
        if (!d) {
            if (errors_.size() == errors_before && d.error() != std::error_code()) { report_error(format_error(d, *symbolizer_), peek()); }
            synchronize();
            continue;
        }
        if (d.value() && !splice_include(d.value(), out)) { out.push_back(std::move(d.value())); }
    }
    splice_spelled_.pop_back();
    splice_active_.pop_back(); --splice_depth_;
    stream_ = saved_stream; current_ = saved_current; pushed_back_token_ = saved_pushed_back;
    paren_close_.erase(&tokens);
    spliced_files_.push_back(resolved.value());
    return true;
}

checked_result<statement_ptr> parser::expression_statement() {
    JAISCRIPT_TRY_ASSIGN(expression_ptr expr, expression());
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after expression"));
    return std::make_shared<expression_stmt>(expr->location, expr);
}

// `if (a = b)`: the assignment is the condition itself; `if ((a = b))` has parentheses of its own and is left unmarked
void parser::mark_bare_condition(const expression_ptr& condition, size_t first_token) {
    if (!condition || condition->get_type() != node_type::assignment_expr) return;
    auto* assign = static_cast<assignment_expr*>(condition.get());
    if (assign->op.type != token_type::equal || assign->incdec_desugar) return;
    if ((*stream_)[first_token].type == token_type::left_paren) {
        size_t depth = 0;
        size_t at = first_token;
        for (; at < current_; ++at) {
            const token_type k = (*stream_)[at].type;
            if (k == token_type::left_paren) { ++depth; }
            else if (k == token_type::right_paren && --depth == 0) { break; }
        }
        if (at + 1 == current_) return;
    }
    assign->bare_condition = true;
}

checked_result<statement_ptr> parser::if_statement() {
    const token& ifToken = previous();

    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'if'"));
    const size_t condition_start = current_;
    JAISCRIPT_TRY_ASSIGN(expression_ptr condition, expression());
    mark_bare_condition(condition, condition_start);
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after if condition"));

    JAISCRIPT_TRY_ASSIGN(statement_ptr then_statement, statement());
    statement_ptr else_statement = nullptr;

    if (match(token_type::else_keyword)) {
        JAISCRIPT_TRY_ASSIGN(else_statement, statement());
    }

    return std::make_shared<if_stmt>(ifToken.location, condition, then_statement, else_statement);
}

checked_result<statement_ptr> parser::while_statement() {
    const token& whileToken = previous();
    jump_scope loop(jumps_, {jumps_.breakable + 1, jumps_.continuable + 1, jumps_.parallel_body});

    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'while'"));
    const size_t condition_start = current_;
    JAISCRIPT_TRY_ASSIGN(expression_ptr condition, expression());
    mark_bare_condition(condition, condition_start);
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after while condition"));

    JAISCRIPT_TRY_ASSIGN(statement_ptr body, statement());

    return std::make_shared<while_stmt>(whileToken.location, condition, body);
}

// parallel_for (auto& x : arr) { body } — range-for SYNTAX, but the body is parsed as a
// ONE-PARAMETER FUNCTION BODY (fresh function scope: loop variable = slot 0, own
// local_count): workers execute it as a synthesized function, so an enclosing frame's
// slots must never be stamped on body identifiers (slot_lookup_boundary_). Enclosing
// names in the body resolve by symbol through the worker environment at runtime: the region
// barrier provisions captured reads there, globals from the environment chain and the
// enclosing frame's locals from the slots recorded in frame_captures.
checked_result<statement_ptr> parser::parallel_for_statement() {
    jump_scope loop(jumps_, {0, 1, true});
    token forToken = previous();

    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'parallel_for'"));

    bool is_const = match(token_type::const_keyword);
    JAISCRIPT_TRY_ASSIGN(type_info_ptr element_type, parse_type());
    const bool is_reference = match(token_type::ampersand);
    if (!check(token_type::identifier)) {
        report_error("expected loop variable name in parallel_for", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    token varName = advance();
    // auto& mutates the owned element in place (the loop's output channel);
    // plain auto gets a worker-local COPY - zero side effects, still legal
    // Zip: `, decl` repeats declare further loop variables over further arrays (same length)
    std::vector<parameter> extra_params;
    std::vector<expression_ptr> extra_containers;
    expression_ptr container;
    if (match(token_type::equal)) {
        JAISCRIPT_TRY_ASSIGN(container, counted_parallel_range(element_type, is_reference, is_const, varName));
    } else {
        while (match(token_type::comma)) {
            bool extra_const = match(token_type::const_keyword);
            JAISCRIPT_TRY_ASSIGN(type_info_ptr extra_type, parse_type());
            const bool extra_ref = match(token_type::ampersand);
            if (!check(token_type::identifier)) {
                report_error("expected loop variable name in parallel_for", peek());
                return make_error_code(parse_error_code::syntax_error);
            }
            token extraName = advance();
            parameter extra(extra_type, std::string(extraName.lexeme), extra_ref, extra_const);
            extra.symbol_id = get_symbol_id(extraName);
            extra_params.push_back(std::move(extra));
        }
        JAISCRIPT_TRY(expect(token_type::colon, "expected ':' after parallel_for loop variable"));

        // The containers evaluate in the ENCLOSING scope on the calling thread
        JAISCRIPT_TRY_ASSIGN(container, expression());
        while (match(token_type::comma)) {
            JAISCRIPT_TRY_ASSIGN(expression_ptr extra_container, expression());
            extra_containers.push_back(std::move(extra_container));
        }
        if (extra_containers.size() != extra_params.size()) {
            report_error("parallel_for zip: one array per loop variable (" + std::to_string(1 + extra_params.size()) +
                         " variables, " + std::to_string(1 + extra_containers.size()) + " arrays)", peek());
            return make_error_code(parse_error_code::syntax_error);
        }
    }
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after parallel_for range"));

    parameter loop_param(element_type, std::string(varName.lexeme), is_reference, is_const);
    loop_param.symbol_id = get_symbol_id(varName);

    enter_function_scope();
    const size_t saved_boundary = slot_lookup_boundary_;
    slot_lookup_boundary_ = function_scope_stack_.size() - 1;
    parallel_frame_captures_.emplace_back();
    if (loop_param.symbol_id != UINT64_MAX) {
        loop_param.slot_index = allocate_slot(loop_param.symbol_id);
    }
    for (auto& extra : extra_params) {   // slots 1..m-1, before the body's locals
        if (extra.symbol_id != UINT64_MAX) { extra.slot_index = allocate_slot(extra.symbol_id); }
    }
    std::vector<std::pair<uint64_t, size_t>> frame_captures;
    auto restore_scope = [&]() {
        slot_lookup_boundary_ = saved_boundary;
        frame_captures = std::move(parallel_frame_captures_.back());
        parallel_frame_captures_.pop_back();
        return exit_function_scope();
    };
    if (!match(token_type::left_brace)) {
        restore_scope();
        report_error("expected '{' for parallel_for body", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    auto body_result = block_statement();
    if (!body_result) {
        restore_scope();
        return body_result;
    }
    auto node = std::make_shared<parallel_for_stmt>(forToken.location, std::move(loop_param),
                                                    std::move(container), body_result.value());
    node->extra_params = std::move(extra_params);
    node->extra_containers = std::move(extra_containers);
    node->local_count = restore_scope();
    node->frame_captures = std::move(frame_captures);
    return node;
}

// A step the serial loop would evaluate to the same value every time: names, literals and operators, no call or store
static bool loop_invariant_step(const jai::expression* e) {
    if (!e) { return true; }
    switch (e->get_type()) {
        case node_type::call_expr: case node_type::assignment_expr: case node_type::lambda_expr: case node_type::new_expr:
        case node_type::yield_expr: case node_type::throw_expr: case node_type::import_expr: case node_type::include_expr:
            return false;
        case node_type::unary_expr: {
            const token_type op = static_cast<const unary_expr*>(e)->op.type;
            if (op == token_type::plus_plus || op == token_type::minus_minus) { return false; }
            break;
        }
        default:
            break;
    }
    bool result = true;
    detail::for_each_child_expression(e, [&](const jai::expression* child) { result = result && loop_invariant_step(child); });
    return result;
}

// parallel_for (int i = a; i < b; i++): one int variable stepped by a fixed amount toward a bound, so it visits each index of
// one arithmetic sequence once. The range is that sequence: __jai_parallel_range(a, b, k, comparison, step kind) evaluates
// a, b and k once, in order, before the region (detail::parallel_counted_range); the region iterates it as an index array
checked_result<expression_ptr> parser::counted_parallel_range(const type_info_ptr& element_type, bool is_reference, bool is_const, const token& variable) {
    auto refuse = [&](const char* text) -> checked_result<expression_ptr> {
        report_error(text, peek());
        return make_error_code(parse_error_code::syntax_error);
    };
    const bool int_variable = !is_reference && !is_const &&   // a null type is auto
                              (!element_type || (element_type->base_type == script_value_type::jai_int_type && !element_type->is_byte_element()));
    if (!int_variable) { return refuse("parallel_for: a counted loop declares one int variable (int i = a or auto i = a)"); }
    const source_location at = variable.location;
    JAISCRIPT_TRY_ASSIGN(expression_ptr start, expression());
    if (!match(token_type::semicolon)) { return refuse("parallel_for: a counted loop declares one int variable (int i = a or auto i = a)"); }
    auto names_variable = [&]() { return check(token_type::identifier) && peek().lexeme == variable.lexeme; };
    const char* compares = "parallel_for: a counted loop compares its variable with a bound (i < b, i <= b, i > b or i >= b)";
    if (!names_variable()) { return refuse(compares); }
    advance();
    script_int comparison = 0;
    if (match(token_type::less)) { comparison = 0; }
    else if (match(token_type::less_equal)) { comparison = 1; }
    else if (match(token_type::greater)) { comparison = 2; }
    else if (match(token_type::greater_equal)) { comparison = 3; }
    else { return refuse(compares); }
    JAISCRIPT_TRY_ASSIGN(expression_ptr bound, operator_expression(k_shift_precedence));
    if (!match(token_type::semicolon)) { return refuse(compares); }
    const char* steps = "parallel_for: a counted loop must step its variable by a fixed amount toward a bound (write i++, i--, i += k or i -= k)";
    script_int step_kind = 0;   // 1 ++, -1 --, 2 +=, -2 -=
    expression_ptr step;
    if (match(token_type::plus_plus) || match(token_type::minus_minus)) {
        step_kind = previous().type == token_type::plus_plus ? 1 : -1;
        if (!names_variable()) { return refuse(steps); }
        advance();
    } else if (names_variable()) {
        advance();
        if (match(token_type::plus_plus) || match(token_type::minus_minus)) {
            step_kind = previous().type == token_type::plus_plus ? 1 : -1;
        } else if (match(token_type::plus_equal) || match(token_type::minus_equal)) {
            step_kind = previous().type == token_type::plus_equal ? 2 : -2;
            JAISCRIPT_TRY_ASSIGN(step, expression());
            if (!loop_invariant_step(step.get())) { return refuse(steps); }
        } else {
            return refuse(steps);
        }
    } else {
        return refuse(steps);
    }
    if (!check(token_type::right_paren)) { return refuse(steps); }
    // a constant step's direction is known here: zero, or away from the bound, never ends the serial loop
    std::optional<script_int> constant = 1;
    if (step) {
        constant.reset();
        const jai::expression* e = step.get();
        bool negated = false;
        if (e->get_type() == node_type::unary_expr && static_cast<const unary_expr*>(e)->op.type == token_type::minus) {
            negated = true;
            e = static_cast<const unary_expr*>(e)->operand.get();
        }
        if (e && e->get_type() == node_type::literal_expr && static_cast<const literal_expr*>(e)->value.is_int()) {
            const script_int v = static_cast<const literal_expr*>(e)->value.unchecked_as_int();
            constant = negated ? (v == 0 ? 0 : (v > 0 ? -1 : 1)) : (v == 0 ? 0 : (v > 0 ? 1 : -1));
        }
    }
    if (constant) {
        if (*constant == 0) { return refuse("parallel_for: a counted loop's step must not be zero"); }
        if ((comparison <= 1) != (*constant * step_kind > 0)) {
            return refuse("parallel_for: a counted loop must step toward its bound (i < b and i <= b count up, i > b and i >= b count down)");
        }
    }
    if (!step) {
        step = std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, script_int(1)));
    }
    auto [range_id, range_view] = symbolizer_->intern_with_view(parallel_for_stmt::k_counted_range);
    auto callee = std::make_shared<identifier_expr>(at, range_view, range_id);
    callee->slot_index = lookup_slot(range_id);
    std::vector<expression_ptr> arguments;
    arguments.push_back(std::move(start));
    arguments.push_back(std::move(bound));
    arguments.push_back(std::move(step));
    arguments.push_back(std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, comparison)));
    arguments.push_back(std::make_shared<literal_expr>(at, script_value(script_value::ast_literal_tag{}, step_kind)));
    return expression_ptr(std::make_shared<call_expr>(at, callee, std::move(arguments)));
}

checked_result<statement_ptr> parser::for_statement() {
    jump_scope loop(jumps_, {jumps_.breakable + 1, jumps_.continuable + 1, jumps_.parallel_body});
    token forToken = previous();

    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'for'"));

    // Save position for potential backtracking
    size_t savedPosition = current_;

    // Try to parse as range-based for loop first
    // This will be: [const] type [&] identifier : expression
    bool is_const = match(token_type::const_keyword);
    bool is_reference = false;
    type_info_ptr element_type = nullptr;
    // for (const x : xs) and for (const [k, v] : m): const alone is const auto, as in a declaration (const x = e;)
    const bool bare_const = is_const && (check(token_type::left_bracket) ||
        (check(token_type::identifier) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::colon));
    if (is_const && (check(token_type::colon) || check(token_type::ampersand))) {
        report_error("a range-for's const variable needs a name: for (const x : xs)", peek());
        return make_error_code(parse_error_code::syntax_error);
    }

    // Check if we have a type followed by optional & and identifier : pattern (every type spelling: array<int>,
    // map<string, int>, shared_ptr<C>, weak_ptr<C> start with keywords)
    if (bare_const || is_type_keyword_token(peek().type) || check(token_type::identifier)) {

        if (!bare_const) {
            // Parse the type
            auto element_type_result = parse_type();
            if (!element_type_result) {
                // Failed to parse type, restore position and try traditional for
                current_ = savedPosition;
                goto traditional_for;
            }
            element_type = std::move(element_type_result.value());

            // Check for reference after type
            if (match(token_type::ampersand)) {
                is_reference = true;
            }
        }

        // for (auto [k, v] : m): the entry's key and value by name, C++17's structured binding. The loop walks a hidden entry
        // variable and the names are declarations reading its halves: by value both are the iteration's copies, with auto& the
        // value is a reference to the map's entry (detail::stamp_map_entry_bindings marks the shape for both backends)
        const token_type spelled = (*stream_)[savedPosition + (is_const ? 1 : 0)].type;
        if (check(token_type::left_bracket) && (bare_const || spelled == token_type::auto_keyword || spelled == token_type::var_keyword)) {
            const token bracket = advance();
            const bool auto_spelled = !element_type;
            std::vector<token> names;
            do {
                JAISCRIPT_TRY_ASSIGN(token name, consume_name("expected a name in the structured binding", "variable"));
                names.push_back(name);
            } while (match(token_type::comma));
            JAISCRIPT_TRY(expect(token_type::right_bracket, "expected ']' after the structured binding's names"));
            if (names.size() != 2) {
                report_error("a map entry binds two names, the key and the value: for (auto [k, v] : m)", bracket);
                return make_error_code(parse_error_code::syntax_error);
            }
            JAISCRIPT_TRY(expect(token_type::colon, "expected ':' after the structured binding"));
            JAISCRIPT_TRY_ASSIGN(expression_ptr container, expression());
            JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after range expression"));

            block_scope_guard loop_var_scope(*this);
            const auto [pair_id, pair_name] = hidden_name("entry");
            const size_t pair_slot = in_function_scope() ? allocate_slot(pair_id) : SIZE_MAX;
            std::vector<declaration_ptr> bindings;
            const char* halves[2] = {"first", "second"};
            for (size_t i = 0; i < 2; ++i) {
                // the key is a copy (a map key is read-only, as std::map's const Key is); by reference the value is the entry's
                type_info_ptr binding_type = element_type;
                if (i == 1 && is_reference) {
                    type_info ref_type(script_value_type::jai_reference_type);
                    ref_type.type_name = auto_spelled ? "auto&" : element_type->type_name + "&";
                    ref_type.type_params.push_back(element_type);
                    ref_type.id = symbolizer_->intern(ref_type.canonical_name());
                    binding_type = store_type_info(std::move(ref_type));
                }
                auto [half_id, half_view] = symbolizer_->intern_with_view(halves[i]);
                auto half = std::make_shared<member_expr>(names[i].location, hidden_identifier(pair_id, pair_name, names[i].location), half_view, half_id, false);
                auto binding = std::make_shared<variable_decl>(names[i].location, binding_type, names[i].lexeme,
                                                               get_symbol_id(names[i]), half);
                binding->is_const = is_const;
                if (in_function_scope()) { binding->slot_index = allocate_slot(binding->name_id); }
                bindings.push_back(std::move(binding));
            }
            JAISCRIPT_TRY_ASSIGN(statement_ptr body, statement());
            bindings.push_back(std::make_shared<statement_decl>(body->location, body));
            auto block = std::make_shared<block_stmt>(body->location, std::move(bindings));
            auto result = std::make_shared<range_for_stmt>(forToken.location, element_type, pair_name, pair_id, is_reference, is_const, container, block);
            result->variable_slot_index = pair_slot;
            detail::stamp_map_entry_bindings(*result, symbolizer_);
            return result;
        }

        // Must have an identifier
        if (check(token_type::identifier)) {
            token varName = advance();

            // Check for colon - this indicates range-based for
            if (match(token_type::colon)) {
                // This is a range-based for loop!
                // Parse the container in the OUTER scope (it must not see the
                // loop variable's slot).
                JAISCRIPT_TRY_ASSIGN(expression_ptr container, expression());
                JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after range expression"));

                // Allocate the loop variable's slot BEFORE parsing the body so
                // that references to it inside the body resolve to this slot.
                // The loop variable's name stops resolving after the loop closes.
                block_scope_guard loop_var_scope(*this);
                uint64_t var_id = get_symbol_id(varName);
                size_t var_slot = SIZE_MAX;
                if (in_function_scope() && var_id != UINT64_MAX) {
                    var_slot = allocate_slot(var_id);
                }

                JAISCRIPT_TRY_ASSIGN(statement_ptr body, statement());

                auto result = std::make_shared<range_for_stmt>(
                    forToken.location, element_type, varName.lexeme, var_id,
                    is_reference, is_const, container, body
                );
                result->variable_slot_index = var_slot;

                return result;
            }
        }
    }

    // Not a range-based for loop, restore position and parse as traditional for
    current_ = savedPosition;

traditional_for:
    // Init — the loop variable's slot stops resolving after the loop closes
    block_scope_guard loop_var_scope(*this);
    declaration_ptr init = nullptr;
    const token init_const_at = peek();
    const bool init_const = match(token_type::const_keyword);
    // An identifier starts a declaration only when a type spelling is followed by a name (C++: whatever can be a
    // declaration is one); `i = 0`, `o[0] = 1`, `j += 1` are expression initializers
    bool identifier_declares = false;
    if (check(token_type::identifier)) {
        const size_t type_start = current_;
        const size_t errors_before = errors_.size();
        const std::optional<token> pushed_before = pushed_back_token_;
        auto type_probe = parse_type();
        if (type_probe && errors_.size() == errors_before) {
            match(token_type::ampersand);
            identifier_declares = check(token_type::identifier);
        }
        current_ = type_start;
        errors_.resize(errors_before);
        pushed_back_token_ = pushed_before;
    }
    // for (const i = 0; ...): const alone is const auto
    const bool bare_const_init = init_const && check(token_type::identifier) && current_ + 1 < stream_->size() &&
                                 (*stream_)[current_ + 1].type == token_type::equal;
    if (match(token_type::semicolon)) {
        // No init
    } else if (check(token_type::auto_keyword) || check(token_type::var_keyword) ||
               check(token_type::int_keyword) || check(token_type::float_keyword) ||
               check(token_type::string_keyword) || check(token_type::bool_keyword) ||
               check(token_type::char_keyword) || check(token_type::array_keyword) ||
               check(token_type::map_keyword) || identifier_declares || bare_const_init) {
        // Parse variable declaration without consuming the semicolon
        // (the for loop will handle it)
        type_info_ptr type;
        if (!bare_const_init) {
            JAISCRIPT_TRY_ASSIGN(type_info_ptr spelled_type, parse_type());
            type = std::move(spelled_type);
        }

        // Check for reference after type
        if (match(token_type::ampersand)) {
            type_info refType(script_value_type::jai_reference_type);
            refType.type_name = type ? (type->type_name + "&") : "auto&";
            refType.type_params.push_back(type);
            refType.id = symbolizer_->intern(refType.canonical_name());
            type = store_type_info(std::move(refType));
        }

        JAISCRIPT_TRY_ASSIGN(token name, consume_name("expected variable name", "variable"));

        expression_ptr initializer = nullptr;
        const bool spelled_initializer = match(token_type::equal);
        if (spelled_initializer) {
            JAISCRIPT_TRY_ASSIGN(initializer, expression());
        } else {
            initializer = default_initializer(type, name.location);
        }

        auto var_init = std::make_shared<variable_decl>(name.location, type, name.lexeme, get_symbol_id(name), initializer);
        var_init->default_initialized = init_const && !spelled_initializer;
        if (in_function_scope() && var_init->name_id != UINT64_MAX) {
            var_init->slot_index = allocate_slot(var_init->name_id);
        }
        if (init_const && !finish_const(var_init, init_const_at, false)) { return make_error_code(parse_error_code::syntax_error); }
        init = var_init;
        // Note: NOT consuming semicolon here - the for loop will handle it
    } else {
        if (init_const) {
            report_foreign(detail::foreign_word::const_misplaced, init_const_at);
            return make_error_code(parse_error_code::syntax_error);
        }
        JAISCRIPT_TRY_ASSIGN(expression_ptr expr, expression());
        init = std::make_shared<expression_decl>(expr->location, std::move(expr));
    }

    // Consume semicolon after init
    if (init) {
        JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after for loop initializer"));
    }

    // Condition
    expression_ptr condition = nullptr;
    if (!check(token_type::semicolon)) {
        const size_t condition_start = current_;
        JAISCRIPT_TRY_ASSIGN(condition, expression());
        mark_bare_condition(condition, condition_start);
    }
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after for loop condition"));

    // Update
    expression_ptr update = nullptr;
    if (!check(token_type::right_paren)) {
        JAISCRIPT_TRY_ASSIGN(update, expression());
    }
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after for loop clauses"));

    JAISCRIPT_TRY_ASSIGN(statement_ptr body, statement());

    return std::make_shared<for_stmt>(forToken.location, init, condition, update, body);
}

checked_result<statement_ptr> parser::return_statement() {
    const token& returnToken = previous();

    expression_ptr value = nullptr;
    if (!check(token_type::semicolon)) {
        if (in_constructor_) {
            report_error("a constructor cannot return a value (a bare 'return;' ends it early and the call yields the instance)", returnToken);
            return make_error_code(parse_error_code::syntax_error);
        }
        JAISCRIPT_TRY_ASSIGN(value, expression());
        if (returns_reference_ && !in_coroutine_) { value = bound_conditional(std::move(value)); }
    }

    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after return value"));

    return std::make_shared<return_stmt>(returnToken.location, value);
}

checked_result<statement_ptr> parser::break_statement() {
    token breakToken = previous();
    if (jumps_.breakable == 0) {
        report_error(jumps_.parallel_body ? "'break' is not allowed in a parallel_for body: iterations are independent; use 'continue' for the next element"
                                          : "'break' outside a loop or switch", breakToken);
    }
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after 'break'"));
    return std::make_shared<break_stmt>(breakToken.location);
}

checked_result<statement_ptr> parser::continue_statement() {
    token continueToken = previous();
    if (jumps_.continuable == 0) {
        report_error("'continue' outside a loop", continueToken);
    }
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after 'continue'"));
    return std::make_shared<continue_stmt>(continueToken.location);
}

checked_result<statement_ptr> parser::try_statement() {
    token tryToken = previous();

    // Parse try block
    JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' after 'try'"));
    JAISCRIPT_TRY_ASSIGN(statement_ptr try_block, block_statement());

    // Must have catch block
    JAISCRIPT_TRY(expect(token_type::catch_keyword, "expected 'catch' after try block"));

    // Optional catch variable: a block-scoped local of the catch body (slot in a
    // function frame, scope env at top level), exactly the range-for variable's shape
    std::string catch_var;
    uint64_t catch_var_id = UINT64_MAX;
    size_t catch_slot = SIZE_MAX;
    block_scope_guard catch_var_scope(*this);
    // `(Type name)`: a typed clause; `(name)`, `(var name)`, `(auto name)` and no parentheses catch everything
    const auto typed_clause = [this]() {
        if (!check(token_type::left_paren) || current_ + 2 >= stream_->size()) { return false; }
        const token& first = (*stream_)[current_ + 1];
        const token& second = (*stream_)[current_ + 2];
        if (first.type == token_type::var_keyword || first.type == token_type::auto_keyword) { return false; }
        return !(first.type == token_type::identifier && second.type == token_type::right_paren);
    };
    if (!typed_clause()) {
        if (match(token_type::left_paren)) {
            if (!match(token_type::var_keyword)) { (void)match(token_type::auto_keyword); }
            JAISCRIPT_TRY_ASSIGN(token var_name, consume(token_type::identifier, "expected variable name in catch"));
            catch_var = var_name.lexeme;
            JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after catch variable"));
            catch_var_id = get_symbol_id(var_name);
            if (in_function_scope() && catch_var_id != UINT64_MAX) {
                catch_slot = allocate_slot(catch_var_id);
            }
        }

        // Parse catch block
        JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' after 'catch'"));
        JAISCRIPT_TRY_ASSIGN(statement_ptr catch_block, block_statement());
        if (check(token_type::catch_keyword)) {
            report_error("a catch with no type catches everything: it is the last handler of its try", peek());
            return make_error_code(parse_error_code::syntax_error);
        }

        auto result = std::make_shared<try_stmt>(tryToken.location, try_block, catch_block, catch_var);
        result->catch_var_id = catch_var_id;
        result->catch_slot_index = catch_slot;
        return result;
    }

    // Typed clauses are one handler over a hidden variable: each clause is a test of what was caught, tried in order, whose
    // body declares the clause's variable as that value; with no clause for it the error is thrown on (`throw;`)
    const token catch_token = previous();
    // plain locals: the lambdas below name them (a structured binding is not capturable everywhere)
    const auto caught = hidden_name("catch");
    const uint64_t caught_id = caught.first;
    const std::string_view caught_name = caught.second;
    if (in_function_scope()) { catch_slot = allocate_slot(caught_id); }
    const auto takes = hidden_name("catch_takes");
    const uint64_t takes_id = takes.first;
    const std::string_view takes_name = takes.second;
    const auto value = hidden_name("catch_value");
    const uint64_t value_id = value.first;
    const std::string_view value_name = value.second;
    struct clause {
        source_location at;
        expression_ptr test;      // null: the handler that catches everything
        statement_ptr body;
    };
    std::vector<clause> clauses;
    bool first = true;
    while (first || match(token_type::catch_keyword)) {
        first = false;
        const token clause_token = previous();
        if (!clauses.empty() && !clauses.back().test) {
            report_error("a catch with no type catches everything: it is the last handler of its try", clause_token);
            return make_error_code(parse_error_code::syntax_error);
        }
        clause parsed;
        parsed.at = clause_token.location;
        block_scope_guard clause_scope(*this);
        declaration_ptr variable;
        const auto value_of_caught = [&](const source_location& at) {
            std::vector<expression_ptr> arguments{hidden_identifier(caught_id, caught_name, at)};
            return std::make_shared<call_expr>(at, std::make_shared<identifier_expr>(at, value_name, value_id), std::move(arguments));
        };
        // `var& name = <caught>`: the handler's name for the caught value where the clause keeps it dynamic
        const auto name_of_caught = [&](const token& name) {
            type_info reference(script_value_type::jai_reference_type);
            type_info_ptr dynamic = store_type_info(type_info::make_any(*symbolizer_));
            reference.type_name = dynamic->type_name + "&";
            reference.type_params.push_back(dynamic);
            reference.id = symbolizer_->intern(reference.canonical_name());
            auto declared = std::make_shared<variable_decl>(name.location, store_type_info(std::move(reference)), name.lexeme, get_symbol_id(name),
                                                            hidden_identifier(caught_id, caught_name, name.location));
            if (in_function_scope() && declared->name_id != UINT64_MAX) { declared->slot_index = allocate_slot(declared->name_id); }
            return declared;
        };
        if (typed_clause()) {
            advance();
            const bool is_const = match(token_type::const_keyword);
            const token const_at = previous();
            type_info_ptr type;
            std::string type_name;
            if (check(token_type::identifier) && peek().lexeme == "error" && current_ + 1 < stream_->size() &&
                ((*stream_)[current_ + 1].type == token_type::identifier || (*stream_)[current_ + 1].type == token_type::ampersand)) {
                advance();
                type_name = "error";
            } else {
                JAISCRIPT_TRY_ASSIGN(type, parse_type());
                if (!type) {
                    report_error("expected a type in the catch clause: catch (Type name)", peek());
                    return make_error_code(parse_error_code::syntax_error);
                }
                const type_info* named = type->base_type == script_value_type::jai_shared_ptr_type && !type->type_params.empty() ? type->type_params[0].get() : type.get();
                type_name = named->type_name;
            }
            (void)match(token_type::ampersand);   // the variable is the thrown value either way
            JAISCRIPT_TRY_ASSIGN(token var_name, consume(token_type::identifier, "expected variable name in catch"));
            JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after catch variable"));
            std::vector<expression_ptr> arguments{hidden_identifier(caught_id, caught_name, var_name.location),
                                                  std::make_shared<literal_expr>(var_name.location, script_value(script_value::ast_literal_tag{}, type_name))};
            parsed.test = std::make_shared<call_expr>(var_name.location, std::make_shared<identifier_expr>(var_name.location, takes_name, takes_id), std::move(arguments));
            std::shared_ptr<variable_decl> declared;
            if (type) {
                declared = std::make_shared<variable_decl>(var_name.location, type, var_name.lexeme, get_symbol_id(var_name), value_of_caught(var_name.location));
                if (in_function_scope() && declared->name_id != UINT64_MAX) { declared->slot_index = allocate_slot(declared->name_id); }
            } else {
                declared = name_of_caught(var_name);
            }
            if (is_const && !finish_const(declared, const_at, false)) { return make_error_code(parse_error_code::syntax_error); }
            variable = declared;
        } else if (match(token_type::left_paren)) {
            if (!match(token_type::var_keyword)) { (void)match(token_type::auto_keyword); }
            JAISCRIPT_TRY_ASSIGN(token var_name, consume(token_type::identifier, "expected variable name in catch"));
            JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after catch variable"));
            variable = name_of_caught(var_name);
        }
        JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' after 'catch'"));
        const source_location body_at = previous().location;
        JAISCRIPT_TRY_ASSIGN(statement_ptr handler, block_statement());
        if (variable) {
            std::vector<declaration_ptr> scoped{variable, std::make_shared<statement_decl>(body_at, handler)};
            parsed.body = std::make_shared<block_stmt>(body_at, std::move(scoped));
        } else {
            parsed.body = handler;
        }
        clauses.push_back(std::move(parsed));
    }

    statement_ptr chain;
    std::vector<std::string> clause_types;
    if (clauses.back().test) {
        for (const clause& typed : clauses) {
            const auto* test = static_cast<const call_expr*>(typed.test.get());
            clause_types.push_back(static_cast<const literal_expr*>(test->arguments[1].get())->value.to_string());
        }
        std::vector<declaration_ptr> rethrow{std::make_shared<statement_decl>(catch_token.location,
            std::make_shared<expression_stmt>(catch_token.location, std::make_shared<throw_expr>(catch_token.location, nullptr)))};
        chain = std::make_shared<block_stmt>(catch_token.location, std::move(rethrow));
    } else {
        chain = clauses.back().body;
        clauses.pop_back();
    }
    for (size_t i = clauses.size(); i-- > 0;) {
        chain = std::make_shared<if_stmt>(clauses[i].at, clauses[i].test, clauses[i].body, chain);
    }
    std::vector<declaration_ptr> handler_body{std::make_shared<statement_decl>(catch_token.location, chain)};
    auto result = std::make_shared<try_stmt>(tryToken.location, try_block, std::make_shared<block_stmt>(catch_token.location, std::move(handler_body)),
                                             std::string(caught_name));
    result->catch_var_id = caught_id;
    result->catch_slot_index = catch_slot;
    result->clause_types = std::move(clause_types);
    return result;
}

// declaration parsing implementations
checked_result<declaration_ptr> parser::class_declaration(bool nocopy, std::string_view space) {
    stack_.guard(peek().location);
    if (check(token_type::identifier) && reject_using_name(peek(), "class")) { return make_error_code(parse_error_code::syntax_error); }
    JAISCRIPT_TRY_ASSIGN(token className, consume(token_type::identifier, "expected class name"));
    if (check(token_type::identifier) && peek().lexeme == "extends") {
        report_foreign(detail::foreign_word::extends_base, peek());
        return make_error_code(parse_error_code::syntax_error);
    }

    std::vector<std::string_view> base_classes;
    if (match(token_type::colon)) {
        do {
            JAISCRIPT_TRY_ASSIGN(token base_class_tok, consume(token_type::identifier, "expected base class name"));
            if (!check(token_type::colon_colon)) {
                // Token lexeme already points to symbolizer storage (identifiers are interned during lexing)
                base_classes.emplace_back(base_class_tok.lexeme);
                continue;
            }
            // a qualified base (ns::Base, binary::writer): the class registered under that spelling
            std::string qualified(base_class_tok.lexeme);
            while (check(token_type::colon_colon) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::identifier) {
                advance();
                qualified += "::" + std::string(advance().lexeme);
            }
            base_classes.emplace_back(symbolizer_->intern_with_view(qualified).second);
        } while (match(token_type::comma));
    }

    JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' before class body"));
    const size_t class_open_index = current_ - 1;

    // a class a namespace declares is named ns::Name (C++); its constructors and destructor carry that name
    auto [class_name_id, class_name_view] = space.empty() ? symbolizer_->intern_with_view(className.lexeme)
                                                          : symbolizer_->intern_with_view(std::string(space) + "::" + std::string(className.lexeme));
    auto classDecl = std::make_shared<class_decl>(className.location, class_name_view, class_name_id);
    classDecl->base_classes = std::move(base_classes);
    classDecl->nocopy = nocopy;

    // Parse class members
    class_decl::member_visibility visibility = class_decl::Public;
    const bool class_entry_in_coroutine = in_coroutine_;
    struct constructor_scope { bool& flag; bool saved; ~constructor_scope() { flag = saved; } } class_ctor_scope{in_constructor_, in_constructor_};
    in_constructor_ = false;   // a class declared inside a constructor body: its members are their own bodies
    nesting_guard nesting(nesting_depth_);

    while (!check(token_type::right_brace) && !is_at_end()) {
        if (check(token_type::import_keyword)) {   // an import is a statement that takes no scope: never a member
            report_error("import is a statement, not a member: it is not allowed directly in a class or a namespace body", peek());
            return make_error_code(parse_error_code::syntax_error);
        }
        // Error-recovery `continue`s below can leave in_coroutine_ armed; every member
        // starts from the class-entry state
        in_coroutine_ = class_entry_in_coroutine;
        coroutine_body_pending_ = false;

        // Check for visibility specifiers
        if (match(token_type::public_keyword)) {
            auto colon_result = consume(token_type::colon, "expected ':' after 'public'");
            if (!colon_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            visibility = class_decl::Public;
            continue;
        }
        if (match(token_type::private_keyword)) {
            auto colon_result = consume(token_type::colon, "expected ':' after 'private'");
            if (!colon_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            visibility = class_decl::Private;
            continue;
        }
        if (match(token_type::protected_keyword)) {
            auto colon_result = consume(token_type::colon, "expected ':' after 'protected'");
            if (!colon_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            visibility = class_decl::Protected;
            continue;
        }

        if (foreign_word_at_statement()) {
            synchronize_class_member(class_open_index);
            continue;
        }

        source_location nodiscard_at{};
        const bool member_nodiscard = nodiscard_attribute_ahead();
        if (member_nodiscard) {
            nodiscard_at = peek().location;
            current_ += 5;
        }

        // const T NAME = e; (also static const / const static): a class-level constant
        bool member_const = false;
        bool const_static = false;
        const token const_at = peek();
        if (check(token_type::static_keyword) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::const_keyword) {
            advance();
            const_static = true;
        }
        if (match(token_type::const_keyword)) {
            member_const = true;
            if (match(token_type::static_keyword)) { const_static = true; }
            if (untyped_member_declaration_ahead()) {   // const NAME = e;: const auto
                token name = advance();
                expression_ptr init;
                if (match(token_type::equal)) {
                    auto init_result = expression();
                    if (!init_result) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }
                    init = std::move(init_result.value());
                }
                if (!consume(token_type::semicolon, "expected ';' after field declaration")) {
                    synchronize_class_member(class_open_index);
                    continue;
                }
                auto var_decl = std::make_shared<variable_decl>(name.location, type_info_ptr{}, name.lexeme, get_symbol_id(name), init);
                var_decl->default_initialized = !init;
                var_decl->is_static = true;
                if (!finish_const(var_decl, const_at, true) || (member_nodiscard && !mark_nodiscard(var_decl, nodiscard_at, class_name_view))) {
                    synchronize_class_member(class_open_index);
                    continue;
                }
                classDecl->members.push_back({visibility, var_decl});
                continue;
            }
        } else if (const_static) {
            current_--;
            const_static = false;
        }

        // Coroutine methods: `coroutine` prefixes every method spelling, in any order with
        // static/override (`static coroutine` reads as `coroutine static`); the whole run
        // is consumed here so each branch below sees the plain head. Ctors/dtors/fields
        // reject at the tail.
        bool member_is_coroutine = false;
        bool leading_static = const_static;
        bool leading_override = false;
        if (coroutine_modifier_run()) {
            in_coroutine_ = true;
            coroutine_body_pending_ = true;
            for (;;) {
                if (match(token_type::coroutine_keyword)) { member_is_coroutine = true; }
                else if (match(token_type::static_keyword)) { leading_static = true; }
                else if (match(token_type::override_keyword)) { leading_override = true; }
                else { break; }
            }
        }

        // Parse member declaration
        declaration_ptr member;

        // ClassName( is a constructor; ClassName name is a member of the class's type (the general member path)
        if (check(token_type::identifier) && peek().lexeme == className.lexeme &&
            current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::left_paren) {
            advance(); // consume class name
            auto body_result = parse_function_body(class_name_view, class_name_id, nullptr, /*allow_ctor_initializers=*/true);
            if (!body_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            member = std::move(body_result.value());
        } else if (match(token_type::tilde)) {
            auto destructor_name_result = consume(token_type::identifier, "expected class name after '~'");
            if (!destructor_name_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            std::string dtor_name_str = "~" + std::string(class_name_view);
            auto [dtor_name_id, dtor_name_view] = symbolizer_->intern_with_view(dtor_name_str);
            auto body_result = parse_function_body(dtor_name_view, dtor_name_id, nullptr);
            if (!body_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            member = std::move(body_result.value());
        } else if (check(token_type::function_keyword) &&
                   current_ + 2 < stream_->size() &&
                   ((*stream_)[current_ + 1].type == token_type::identifier || is_declarable_keyword_name((*stream_)[current_ + 1].type)) &&
                   (*stream_)[current_ + 1].lexeme != "operator" &&
                   ((*stream_)[current_ + 2].type == token_type::equal ||
                    (*stream_)[current_ + 2].type == token_type::semicolon)) {
            // `function name = expr;` / `function name;` is a callable-or-null FIELD (the top level's type);
            // `function operator=` stays an operator method.
            advance(); // consume 'function'
            token name = advance();
            expression_ptr init = nullptr;
            if (match(token_type::equal)) {
                auto init_result = expression();
                if (!init_result) {
                    synchronize_class_member(class_open_index);
                    continue;
                }
                init = std::move(init_result.value());
            }
            auto semicolon_result = consume(token_type::semicolon, "expected ';' after field declaration");
            if (!semicolon_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            auto var_decl = std::make_shared<variable_decl>(name.location, store_type_info(type_info::make_callable(*symbolizer_)), name.lexeme, get_symbol_id(name), init);
            var_decl->function_spelling = true;
            member = var_decl;
        } else if (match(token_type::function_keyword) || (member_is_coroutine && bare_coroutine_head())) {
            // Method declaration with 'function' keyword: function name(...) or function operator=(...);
            // the bare coroutine head (coroutine name(...) / coroutine operator+(...)) lands here too
            bool is_static = leading_static;
            bool is_override = leading_override;

            // Check for static/override modifiers after function keyword
            while (check(token_type::static_keyword) || check(token_type::override_keyword)) {
                if (match(token_type::static_keyword)) is_static = true;
                if (match(token_type::override_keyword)) is_override = true;
            }

            std::string method_name;
            source_location method_location = peek().location;

            // Check for operator overload: function operator= or function operator+
            if (check(token_type::identifier) && peek().lexeme == "operator") {
                advance(); // consume 'operator'
                auto op_name = parse_operator_method_symbol();
                if (!op_name) {
                    synchronize_class_member(class_open_index);
                    continue;
                }
                method_name = std::move(*op_name);
            } else if (check(token_type::identifier) || keyword_member_name_at(*stream_, current_)) {
                // Regular method name
                token name_tok = advance();
                method_name = std::string(name_tok.lexeme);
            } else {
                report_error("expected method name or 'operator' after 'function'", peek());
                synchronize_class_member(class_open_index);
                continue;
            }

            // Intern method name and parse function body
            auto [method_name_id, method_name_view] = symbolizer_->intern_with_view(method_name);
            auto body_result = parse_function_body(method_name_view, method_name_id, nullptr);
            if (!body_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            auto func = std::dynamic_pointer_cast<function_decl>(body_result.value());
            if (func) {
                func->is_static = is_static;
                func->is_override = is_override;
            }
            member = std::move(body_result.value());
        } else {
            // Regular member (variable or function)
            bool is_static = leading_static || match(token_type::static_keyword);
            bool is_override = leading_override || match(token_type::override_keyword);
            const bool function_spelling = check(token_type::function_keyword);

            auto type_result = parse_type();
            if (!type_result) {
                synchronize_class_member(class_open_index);
                continue;
            }
            type_info_ptr type = std::move(type_result.value());
            if (reference_method_head()) { type = wrap_reference_return_type(std::move(type)); }   // float& px() { ... }
            const type_info_ptr method_return_type = function_spelling ? nullptr : type;

            if (check(token_type::identifier) && reject_using_name(peek(), "member")) {
                synchronize_class_member(class_open_index);
                continue;
            }
            if (check(token_type::identifier) || keyword_member_name_at(*stream_, current_)) {
                token name = advance();
                std::string method_name(name.lexeme);

                // Check for operator overload: function operator=(Type arg) or function operator+(...)
                if (method_name == "operator") {
                    auto op_name = parse_operator_method_symbol();
                    if (!op_name) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }
                    method_name = std::move(*op_name);
                }

                if (match(token_type::left_paren)) {
                    // Function - we need to parse parameters and check for override before body
                    current_--; // Back up to before '('

                    // Intern method name FIRST to get permanent storage for string_view
                    auto [method_name_id, method_name_view] = symbolizer_->intern_with_view(method_name);

                    // Create function declaration with interned name
                    auto func = std::make_shared<function_decl>(name.location, method_name_view);
                    func->name_id = method_name_id;
                    func->is_static = is_static;
                    func->is_override = is_override; // Set from earlier check

                    // Parse parameters
                    auto open_paren_result = consume(token_type::left_paren, "expected '(' after function name");
                    if (!open_paren_result) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }

                    auto params_result = parse_parameter_list();
                    if (!params_result) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }
                    func->parameters = std::move(params_result.value());

                    auto close_paren_result = consume(token_type::right_paren, "expected ')' after parameters");
                    if (!close_paren_result) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }
                    if (const_method_suffix()) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }

                    // Handle trailing return type
                    if (match(token_type::arrow)) {
                        if (check(token_type::left_brace)) {
                            // -> { adds no information: keep the leading type
                            func->return_type = method_return_type;
                        } else {
                            auto return_type_result = parse_type();
                            if (!return_type_result) {
                                synchronize_class_member(class_open_index);
                                continue;
                            }
                            func->return_type = wrap_reference_return_type(std::move(return_type_result.value()));
                            if (method_return_type && func->return_type && func->return_type->id != method_return_type->id) {
                                report_error("conflicting return types '" + method_return_type->type_name + "' and '" +
                                             func->return_type->type_name + "' (leading and trailing '->' types must match)", previous());
                                synchronize_class_member(class_open_index);
                                continue;
                            }
                        }
                    } else {
                        func->return_type = method_return_type;
                    }

                    // Check for override keyword after parameters (alternative position)
                    if (match(token_type::override_keyword)) {
                        func->is_override = true;
                    }

                    // Validate: static methods cannot use override keyword
                    if (func->is_static && func->is_override) {
                        report_error("static methods cannot use 'override' keyword - they are not virtual", name);
                        synchronize_class_member(class_open_index);
                        continue;
                    }

                    // Parse constructor initialization list if present
                    if (check(token_type::colon)) {
                        // Initializer lists are constructor-only; on methods
                        // silently parsing and ignoring them would be a trap
                        report_error("constructor initializer lists (': super(...)' / ': this(...)') are only allowed on constructors", peek());
                        synchronize_class_member(class_open_index);
                        continue;
                    }

                    // Now parse the body
                    if (!match(token_type::left_brace)) {
                        report_error("expected '{' before function body", peek());
                        synchronize_class_member(class_open_index);
                        continue;
                    }

                    // Enter function scope for slot-based local variable tracking
                    enter_function_scope();

                    // Assign slots to parameters
                    for (auto& param : func->parameters) {
                        if (param.symbol_id == UINT64_MAX) {
                            param.symbol_id = symbolizer_->intern(param.name);
                        }
                        param.slot_index = allocate_slot(param.symbol_id);
                    }

                    auto body_result = block_statement();
                    if (!body_result) {
                        exit_function_scope();  // Clean up on error
                        synchronize_class_member(class_open_index);
                        continue;
                    }
                    func->body = std::dynamic_pointer_cast<block_stmt>(body_result.value());
                    func->local_count = exit_function_scope();

                    member = func;
                } else {
                    // Variable
                    expression_ptr init = nullptr;
                    if (match(token_type::equal)) {
                        auto init_result = expression();
                        if (!init_result) {
                            synchronize_class_member(class_open_index);
                            continue;
                        }
                        init = std::move(init_result.value());
                    }

                    auto semicolon_result = consume(token_type::semicolon, "expected ';' after field declaration");
                    if (!semicolon_result) {
                        synchronize_class_member(class_open_index);
                        continue;
                    }

                    const bool spelled_init = init != nullptr;
                    if (!init) { init = default_initializer(type, name.location); }
                    auto var_decl = std::make_shared<variable_decl>(name.location, type, name.lexeme, get_symbol_id(name), init);
                    var_decl->is_static = is_static;
                    var_decl->default_initialized = member_const && !spelled_init;   // a class constant needs its initializer (finish_const)
                    var_decl->function_spelling = function_spelling;
                    member = var_decl;
                }
            } else {
                report_expected_name("expected member name", "member");
                synchronize_class_member(class_open_index);
                continue;
            }
        }

        in_coroutine_ = class_entry_in_coroutine;
        if (member_is_coroutine) {
            auto* func_member = member && member->get_type() == node_type::function_decl
                ? static_cast<function_decl*>(member.get()) : nullptr;
            if (!func_member) {
                report_error("'coroutine' in a class body must be followed by a method", peek());
                synchronize_class_member(class_open_index);
                continue;
            }
            if (func_member->name == class_name_view ||
                (func_member->name.size() > 1 && func_member->name[0] == '~')) {
                report_error("constructors and destructors cannot be coroutines", peek());
                synchronize_class_member(class_open_index);
                continue;
            }
            if (func_member->return_type && func_member->return_type->base_type == script_value_type::jai_reference_type) {
                report_error("coroutines cannot return references", peek());
                synchronize_class_member(class_open_index);
                continue;
            }
            if (reject_coroutine_reference_parameter(*func_member)) {
                synchronize_class_member(class_open_index);
                continue;
            }
            func_member->is_coroutine = true;
        }
        if (member_const) {
            if (!finish_const(member, const_at, true)) {
                synchronize_class_member(class_open_index);
                continue;
            }
            static_cast<variable_decl*>(member.get())->is_static = true;
        }
        if (member_nodiscard && !mark_nodiscard(member, nodiscard_at, class_name_view)) {
            synchronize_class_member(class_open_index);
            continue;
        }
        classDecl->members.push_back({visibility, member});
    }

    in_coroutine_ = class_entry_in_coroutine;
    JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after class body"));

    return classDecl;
}

checked_result<declaration_ptr> parser::namespace_declaration() {
    stack_.guard(peek().location);
    // Accept identifier or any keyword as namespace name
    // Supports: namespace string {}, namespace mylib {}, namespace my::nested::nspace {}
    std::vector<std::string> namespace_path;
    source_location start_loc = peek().location;

    // Parse first namespace name
    token first_name = advance();
    if (first_name.type == token_type::eof ||
        first_name.type == token_type::left_brace ||
        first_name.type == token_type::right_brace ||
        first_name.type == token_type::left_paren ||
        first_name.type == token_type::right_paren ||
        first_name.type == token_type::semicolon ||
        first_name.type == token_type::colon_colon) {
        report_error("expected namespace name", first_name);
    return make_error_code(parse_error_code::syntax_error);
    }
    namespace_path.emplace_back(first_name.lexeme);

    // Check for :: (nested namespace syntax like C++17)
    while (match(token_type::colon_colon)) {
        token next_name = advance();
        if (next_name.type == token_type::eof ||
            next_name.type == token_type::left_brace ||
            next_name.type == token_type::right_brace ||
            next_name.type == token_type::left_paren ||
            next_name.type == token_type::right_paren ||
            next_name.type == token_type::semicolon ||
            next_name.type == token_type::colon_colon) {
            report_error("expected namespace name after '::'", next_name);
    return make_error_code(parse_error_code::syntax_error);
        }
        namespace_path.emplace_back(next_name.lexeme);
    }

    JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' before namespace body"));

    // Build the full namespace name by prepending the current namespace context
    // For nested namespaces: namespace outer { namespace inner {} }
    // When parsing "inner", current_namespace_path_ = ["outer"], so we create "outer::inner"
    std::string full_namespace_name;

    // Prepend current namespace context
    if (!current_namespace_path_.empty()) {
        full_namespace_name = current_namespace_path_[0];
        for (size_t i = 1; i < current_namespace_path_.size(); ++i) {
            full_namespace_name += "::" + current_namespace_path_[i];
        }
        full_namespace_name += "::";
    }

    // Append the new namespace path
    full_namespace_name += namespace_path[0];
    for (size_t i = 1; i < namespace_path.size(); ++i) {
        full_namespace_name += "::" + namespace_path[i];
    }

    // Intern the namespace name at parse time for fast comparisons later
    auto [ns_id, ns_view] = symbolizer_->intern_with_view(full_namespace_name);
    auto namespace_decl_node = std::make_shared<namespace_decl>(start_loc, ns_view, ns_id);

    // Push this namespace onto the context stack for nested namespaces
    for (const auto& part : namespace_path) {
        current_namespace_path_.push_back(part);
    }

    // Parse namespace members (functions, classes, variables)
    nesting_guard nesting(nesting_depth_);
    while (!check(token_type::right_brace) && !is_at_end()) {
        if (check(token_type::import_keyword)) {   // an import is a statement that takes no scope: never a member
            report_error("import is a statement, not a member: it is not allowed directly in a class or a namespace body", peek());
            return make_error_code(parse_error_code::syntax_error);
        }
        source_location nodiscard_at{};
        const bool nodiscard_member = nodiscard_attribute_ahead();
        if (nodiscard_member) {
            nodiscard_at = peek().location;
            current_ += 5;
        }
        // Parse any declaration (class, function, variable)
        declaration_ptr member_decl = nullptr;

        const token member_start = peek();
        const bool inline_member = match(token_type::inline_keyword);
        const bool const_member = match(token_type::const_keyword);
        bool nested_nocopy = !const_member && match(token_type::nocopy_keyword);
        if (nested_nocopy || match(token_type::class_keyword)) {
            if (nested_nocopy) { JAISCRIPT_TRY(expect(token_type::class_keyword, "expected 'class' after 'nocopy'")); }
            auto class_result = class_declaration(nested_nocopy, full_namespace_name);
            if (!class_result) {
                synchronize();
                continue;
            }
            member_decl = std::move(class_result.value());
            match(token_type::semicolon);  // Consume optional semicolon after class
        } else if (!inline_member && !const_member && match(token_type::coroutine_keyword)) {
            // a coroutine member: the program-scope spellings (a namespace body is declaration*)
            const bool was_in_coroutine = in_coroutine_;
            in_coroutine_ = true;
            coroutine_body_pending_ = true;
            auto parsed = bare_coroutine_head() ? bare_coroutine_declaration() : function_declaration();
            in_coroutine_ = was_in_coroutine;
            coroutine_body_pending_ = false;
            if (!parsed) {
                synchronize();
                continue;
            }
            auto* func = static_cast<function_decl*>(parsed.value().get());
            func->is_coroutine = true;
            if (func->return_type && func->return_type->base_type == script_value_type::jai_reference_type) {
                report_error("coroutines cannot return references", previous());
                continue;
            }
            if (reject_coroutine_reference_parameter(*func)) { continue; }
            member_decl = std::move(parsed.value());
            match(token_type::semicolon);
        } else if (match(token_type::namespace_keyword)) {
            // Nested namespaces
            auto namespace_result = namespace_declaration();
            if (!namespace_result) {
                synchronize();
                continue;
            }
            member_decl = std::move(namespace_result.value());
        } else if (!inline_member && !const_member && match(token_type::extern_keyword)) {
            auto extern_result = extern_declaration();
            if (!extern_result) {
                synchronize();
                continue;
            }
            member_decl = std::move(extern_result.value());
        } else {
            // Function or variable declaration
            const bool function_spelling = check(token_type::function_keyword);
            type_info_ptr type;
            if (!((inline_member || const_member) && untyped_member_declaration_ahead())) {
                auto type_result = parse_type();
                if (!type_result) {
                    synchronize();
                    continue;
                }
                // `var& r = G;` / `Box& get()`: a reference variable or a reference-returning function, as at program scope
                type = wrap_reference_return_type(std::move(type_result.value()));
            }
            const bool reference_member = type && type->base_type == script_value_type::jai_reference_type;

            if (!check(token_type::identifier) && !keyword_member_name_at(*stream_, current_)) {
                if (check(token_type::this_keyword) || check(token_type::super_keyword)) {
                    report_expected_name("expected member name in namespace", "member");
                } else {
                    report_error("expected member name in namespace", peek());
                }
                synchronize();
                continue;
            }

            token member_name = advance();

            if (match(token_type::left_paren)) {
                // Function declaration
                auto func = std::make_shared<function_decl>(member_name.location, member_name.lexeme);
                func->name_id = get_symbol_id(member_name);
                func->return_type = function_spelling ? nullptr : type;

                // Parse parameters
                auto params_result = parse_parameter_list();
                if (!params_result) {
                    synchronize();
                    continue;
                }
                func->parameters = std::move(params_result.value());

                auto close_paren_result = consume(token_type::right_paren, "expected ')' after parameters");
                if (!close_paren_result) {
                    synchronize();
                    continue;
                }
                if (const_method_suffix()) {
                    synchronize();
                    continue;
                }

                // Check for override keyword after parameters
                bool is_override = match(token_type::override_keyword);
                func->is_override = is_override;

                // Handle trailing return type
                if (match(token_type::arrow)) {
                    if (!check(token_type::left_brace)) {
                        auto return_type_result = parse_type();
                        if (!return_type_result) {
                            synchronize();
                            continue;
                        }
                        func->return_type = wrap_reference_return_type(std::move(return_type_result.value()));
                        if (!function_spelling && type && func->return_type && func->return_type->id != type->id) {
                            report_error("conflicting return types '" + type->type_name + "' and '" +
                                         func->return_type->type_name + "' (leading and trailing '->' types must match)", previous());
                            synchronize();
                            continue;
                        }
                    }
                }

                // Parse function body
                auto open_brace_result = consume(token_type::left_brace, "expected '{' before function body");
                if (!open_brace_result) {
                    synchronize();
                    continue;
                }

                // Enter function scope for slot-based local variable tracking
                enter_function_scope();

                // Assign slots to parameters
                for (auto& param : func->parameters) {
                    if (param.symbol_id == UINT64_MAX) {
                        param.symbol_id = symbolizer_->intern(param.name);
                    }
                    param.slot_index = allocate_slot(param.symbol_id);
                }

                auto body_result = block_statement();
                if (!body_result) {
                    exit_function_scope();  // Clean up on error
                    synchronize();
                    continue;
                }
                func->body = std::dynamic_pointer_cast<block_stmt>(body_result.value());
                func->local_count = exit_function_scope();

                member_decl = func;
            } else {
                // Variable declaration
                expression_ptr init = nullptr;
                if (match(token_type::equal)) {
                    auto init_result = expression();
                    if (!init_result) {
                        synchronize();
                        continue;
                    }
                    init = std::move(init_result.value());
                }

                auto semicolon_result = consume(token_type::semicolon, "expected ';' after variable declaration");
                if (!semicolon_result) {
                    synchronize();
                    continue;
                }

                const bool had_init = init != nullptr;
                if (!init && reference_member) { report_error("reference variable '" + std::string(member_name.lexeme) + "' must be initialized: a reference names a variable from its declaration on", member_name); }
                if (!init && !reference_member) { init = default_initializer(type, member_name.location); }
                auto var_decl = std::make_shared<variable_decl>(member_name.location, type, member_name.lexeme, get_symbol_id(member_name), init);
                var_decl->function_spelling = function_spelling;
                var_decl->default_initialized = const_member && !had_init;
                member_decl = var_decl;
            }
        }

        if (inline_member && member_decl) {
            auto* var_decl = member_decl->get_type() == node_type::variable_decl ? static_cast<variable_decl*>(member_decl.get()) : nullptr;
            if (!var_decl || (var_decl->type && var_decl->type->base_type == script_value_type::jai_reference_type)) {
                report_error(var_decl ? "an inline variable cannot be a reference" : "'inline' declares a variable", member_start);
                continue;
            }
            var_decl->is_inline = true;
        }
        if (const_member && member_decl && !finish_const(member_decl, member_start, false)) {
            continue;
        }
        if (nodiscard_member && member_decl && !mark_nodiscard(member_decl, nodiscard_at)) {
            continue;
        }

        if (member_decl) {
            namespace_decl_node->declarations.push_back(member_decl);
        }
    }

    JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after namespace body"));

    // Pop the namespace context (restore to parent namespace or global scope)
    for (size_t i = 0; i < namespace_path.size(); ++i) {
        current_namespace_path_.pop_back();
    }

    return namespace_decl_node;
}

// Consumes the symbol after an 'operator' method name (= + - * / [] < > <= >= == !=).
// Errors are reported here; nullopt tells the caller to synchronize. ONE consumption
// shared by every class-member operator site so their accepted sets can't drift.
std::optional<std::string> parser::parse_operator_method_symbol() {
    if (check(token_type::equal)) { advance(); return std::string("="); }
    if (check(token_type::plus)) { advance(); return std::string("+"); }
    if (check(token_type::minus)) { advance(); return std::string("-"); }
    if (check(token_type::star)) { advance(); return std::string("*"); }
    if (check(token_type::slash)) { advance(); return std::string("/"); }
    if (check(token_type::left_bracket)) {
        advance();
        if (!match(token_type::right_bracket)) {
            report_error("expected ']' after '[' in operator[]", peek());
            return std::nullopt;
        }
        return std::string("[]");
    }
    if (check(token_type::less)) { advance(); return std::string("<"); }
    if (check(token_type::greater)) { advance(); return std::string(">"); }
    if (check(token_type::less_equal)) { advance(); return std::string("<="); }
    if (check(token_type::greater_equal)) { advance(); return std::string(">="); }
    if (check(token_type::equal_equal)) { advance(); return std::string("=="); }
    if (check(token_type::bang_equal)) { advance(); return std::string("!="); }
    // The rest of C++'s overloadable set (&&, ||, the comma, ->, new and delete stay out); unary or
    // binary by the parameter count, like a C++ member operator. ! goes through the one contextual conversion, to_bool()
    if (check(token_type::bang)) {
        report_error("operator! cannot be declared: !v is !to_bool(v); declare bool to_bool()", peek());
        return std::nullopt;
    }
    if (check(token_type::percent)) { advance(); return std::string("%"); }
    if (check(token_type::ampersand)) { advance(); return std::string("&"); }
    if (check(token_type::pipe)) { advance(); return std::string("|"); }
    if (check(token_type::caret)) { advance(); return std::string("^"); }
    if (check(token_type::left_shift)) { advance(); return std::string("<<"); }
    if (check(token_type::right_shift)) { advance(); return std::string(">>"); }
    if (check(token_type::spaceship)) { advance(); return std::string("<=>"); }
    if (check(token_type::tilde)) { advance(); return std::string("~"); }
    if (check(token_type::left_paren) && current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::right_paren) {
        advance(); advance();
        return std::string("()");
    }
    report_error("expected operator symbol after 'operator' keyword", peek());
    return std::nullopt;
}

checked_result<declaration_ptr> parser::include_declaration() {
    // include "path" or include <path> or include(expr)

    // Check for function-style syntax: include(expr)
    if (match(token_type::left_paren)) {
        // Parse the expression inside parentheses
        JAISCRIPT_TRY_ASSIGN(expression_ptr path_expr, expression());
        JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after include expression"));
        return std::make_shared<include_decl>(previous().location, path_expr);
    }

    JAISCRIPT_TRY_ASSIGN(std::string path, include_literal_path());
    return std::make_shared<include_decl>(previous().location, path);
}

// include's literal path: "path" or <path>
checked_result<std::string> parser::include_literal_path() {
    if (match(token_type::string_literal)) {
        return previous().string_value;
    }
    if (match(token_type::less)) {
        std::string result;
        while (!check(token_type::greater) && !is_at_end()) {
            result += peek().lexeme;
            advance();
        }
        JAISCRIPT_TRY(expect(token_type::greater, "expected '>' after include path"));
        return result;
    }
    report_error("expected string literal, '<', or '(' after include", peek());
    return make_error_code(parse_error_code::syntax_error);
}

// A construction's arguments after its '(' or '{', through the closer
checked_result<std::vector<expression_ptr>> parser::construction_arguments(bool a_brace, const char* a_closeError) {
    const token_type close = a_brace ? token_type::right_brace : token_type::right_paren;
    std::vector<expression_ptr> result;
    if (!check(close)) {
        result.reserve(4);
        do {
            JAISCRIPT_TRY_ASSIGN(auto arg, expression());
            result.push_back(bound_conditional(std::move(arg)));
        } while (match(token_type::comma));
    }
    JAISCRIPT_TRY(expect(close, a_closeError ? a_closeError : a_brace ? "expected '}' after constructor arguments" : "expected ')' after constructor arguments"));
    return result;
}

// The call form's arguments after `import(`: paths or arrays of paths, then an optional jai::force (a `jai::` name
// in the last position is a directive, so a misspelled one fails the load naming it)
checked_result<expression_ptr> parser::import_call(const source_location& at) {
    auto call = std::make_shared<import_expr>(at);
    std::vector<expression_ptr> args;
    if (!check(token_type::right_paren)) {
        do {
            if (check(token_type::identifier) && peek().lexeme == "jai" && current_ + 2 < stream_->size() &&
                (*stream_)[current_ + 1].type == token_type::colon_colon && (*stream_)[current_ + 2].type == token_type::identifier &&
                current_ + 3 < stream_->size() && (*stream_)[current_ + 3].type == token_type::right_paren) {
                const token first = peek();
                const std::string directive = "jai::" + std::string((*stream_)[current_ + 2].lexeme);
                if (directive != "jai::force") {
                    report_error("unknown import directive '" + directive + "' (the directive is jai::force)", first);
                    return make_error_code(parse_error_code::syntax_error);
                }
                advance();
                advance();
                advance();
                call->force = true;
                break;
            }
            JAISCRIPT_TRY_ASSIGN(expression_ptr arg, expression());
            args.push_back(std::move(arg));
        } while (match(token_type::comma));
    }
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after import paths"));
    if (args.empty()) {
        report_error("import(...) needs at least one path", previous());
        return make_error_code(parse_error_code::syntax_error);
    }
    bool literal = true;
    for (const auto& a : args) {
        literal = literal && a->get_type() == node_type::literal_expr &&
                  static_cast<literal_expr*>(a.get())->value.raw_storage_index() == script_value::TYPEID_STRING;
    }
    if (literal) {
        for (const auto& a : args) { call->paths.push_back(static_cast<literal_expr*>(a.get())->value.unchecked_as_string()); }
    } else {
        call->path_exprs = std::move(args);
    }
    return expression_ptr(call);
}

checked_result<declaration_ptr> parser::import_declaration() {
    // import "path" or import <path> or import(paths...)

    // The call form is an expression whose value a statement discards
    if (match(token_type::left_paren)) {
        const source_location at = previous().location;
        JAISCRIPT_TRY_ASSIGN(expression_ptr call, import_call(at));
        return declaration_ptr(std::make_shared<expression_decl>(at, std::move(call)));
    }

    // Original literal syntax
    std::string path;

    if (match(token_type::string_literal)) {
        // import "path"
        path = previous().string_value;
    } else if (match(token_type::less)) {
        // import <path>
        // Read until we find >
        std::string path_buffer;
        while (!check(token_type::greater) && !is_at_end()) {
            path_buffer += peek().lexeme;
            advance();
        }
        JAISCRIPT_TRY(expect(token_type::greater, "expected '>' after import path"));
        path = path_buffer;
    } else {
        report_error("expected string literal, '<', or '(' after import", peek());
    return make_error_code(parse_error_code::syntax_error);
    }

    return std::make_shared<import_decl>(previous().location, path);
}

checked_result<declaration_ptr> parser::extern_declaration() {
    // extern function name; | extern name; | extern <type> name; where name may be qualified (ns::name, a::b::c)
    source_location loc = previous().location;
    type_info_ptr type;
    bool is_function = false;
    const bool is_const = match(token_type::const_keyword);
    const auto untyped_name_ahead = [this]() {
        size_t at = current_;
        while (at + 1 < stream_->size() && (*stream_)[at].type == token_type::identifier) {
            if ((*stream_)[at + 1].type == token_type::semicolon) { return true; }
            if ((*stream_)[at + 1].type != token_type::colon_colon) { return false; }
            at += 2;
        }
        return false;
    };
    if (!is_const && match(token_type::function_keyword)) {
        is_function = true;
    } else if (!untyped_name_ahead()) {
        JAISCRIPT_TRY_ASSIGN(type, parse_type());
    }
    std::string qualified;
    for (const auto& part : current_namespace_path_) { qualified += part + "::"; }
    const auto written_at = static_cast<uint32_t>(qualified.size());
    JAISCRIPT_TRY_ASSIGN(token name, consume(token_type::identifier, "expected name after 'extern'"));
    qualified += name.lexeme;
    while (match(token_type::colon_colon)) {
        JAISCRIPT_TRY_ASSIGN(token part, consume(token_type::identifier, "expected name after '::' in extern declaration"));
        qualified += "::";
        qualified += part.lexeme;
    }
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after extern declaration"));
    auto [name_id, name_view] = symbolizer_->intern_with_view(qualified);
    auto decl = std::make_shared<extern_decl>(loc, type, is_function, name_view, name_id);
    decl->is_const = is_const;
    decl->written_at = written_at;
    return decl;
}

bool parser::untyped_declaration_ahead() const {
    if (!check(token_type::identifier) || current_ + 1 >= stream_->size()) { return false; }
    const token_type next = (*stream_)[current_ + 1].type;
    return next == token_type::semicolon || next == token_type::equal;
}

// a type keyword there reads as the declaration's type (const int = 1; stays an error)
bool parser::untyped_member_declaration_ahead() const {
    return untyped_declaration_ahead() ||
           (!is_type_keyword_token(peek().type) && keyword_member_name_at(*stream_, current_) && (*stream_)[current_ + 1].type != token_type::left_paren);
}

// inline [type] name [= init];: the C++17 inline variable - one variable shared by every unit that declares it
checked_result<declaration_ptr> parser::inline_variable_declaration() {
    const token at = previous();
    if (nesting_depth_ != 0) {
        report_error("an inline variable is declared at program scope or in a namespace", at);
        return make_error_code(parse_error_code::syntax_error);
    }
    std::shared_ptr<variable_decl> decl;
    const bool is_const = match(token_type::const_keyword);
    if (untyped_declaration_ahead()) {
        token name = advance();
        expression_ptr init;
        if (match(token_type::equal)) { JAISCRIPT_TRY_ASSIGN(init, expression()); }
        JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after variable declaration"));
        decl = std::make_shared<variable_decl>(name.location, type_info_ptr{}, name.lexeme, get_symbol_id(name), init);
        decl->default_initialized = !init;
    } else {
        JAISCRIPT_TRY_ASSIGN(declaration_ptr parsed, variable_declaration());
        decl = std::static_pointer_cast<variable_decl>(parsed);
    }
    if (decl->type && decl->type->base_type == script_value_type::jai_reference_type) {
        report_error("an inline variable cannot be a reference", at);
        return make_error_code(parse_error_code::syntax_error);
    }
    decl->is_inline = true;
    if (is_const && !finish_const(decl, at, false)) { return make_error_code(parse_error_code::syntax_error); }
    return declaration_ptr(decl);
}

// [x, y, z] = expr; after auto, var or a bare const: the '[' is next
checked_result<declaration_ptr> parser::destructuring_declaration() {
    advance();
    auto loc = previous().location;
    auto decl = std::make_shared<destructuring_decl>(loc, nullptr);
    do {
        JAISCRIPT_TRY_ASSIGN(token name_tok, consume_name("expected variable name in destructuring", "variable"));
        auto [sym_id, sym_view] = symbolizer_->intern_with_view(name_tok.lexeme);
        decl->names.push_back({sym_view, sym_id});
        decl->slot_indices.push_back(in_function_scope() ? allocate_slot(sym_id) : SIZE_MAX);
    } while (match(token_type::comma));
    JAISCRIPT_TRY(expect(token_type::right_bracket, "expected ']' after destructuring names"));
    JAISCRIPT_TRY(expect(token_type::equal, "expected '=' after destructuring pattern"));
    JAISCRIPT_TRY_ASSIGN(decl->initializer, expression());
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after destructuring declaration"));
    return declaration_ptr(decl);
}

checked_result<declaration_ptr> parser::const_declaration() {
    const token at = advance();
    if (check(token_type::const_keyword)) {
        report_error("'const' appears once in a declaration", peek());
        return make_error_code(parse_error_code::syntax_error);
    }
    declaration_ptr decl;
    if (check(token_type::left_bracket)) {
        // const [a, b] = e;: const auto [a, b] = e
        JAISCRIPT_TRY_ASSIGN(decl, destructuring_declaration());
    } else if (untyped_declaration_ahead()) {
        // const x = e;: the JavaScript spelling of const auto x = e
        token name = advance();
        expression_ptr init;
        if (match(token_type::equal)) { JAISCRIPT_TRY_ASSIGN(init, expression()); }
        JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after variable declaration"));
        auto var = std::make_shared<variable_decl>(name.location, type_info_ptr{}, name.lexeme, get_symbol_id(name), init);
        var->default_initialized = !init;
        if (in_function_scope() && var->name_id != UINT64_MAX) { var->slot_index = allocate_slot(var->name_id); }
        decl = var;
    } else if (check(token_type::inline_keyword) || check(token_type::extern_keyword) || check(token_type::static_keyword)) {
        report_error("write '" + std::string(peek().lexeme) + " const', not 'const " + std::string(peek().lexeme) + "'", peek());
        return make_error_code(parse_error_code::syntax_error);
    } else {
        JAISCRIPT_TRY_ASSIGN(decl, declaration_body());
    }
    if (!finish_const(decl, at, false)) { return make_error_code(parse_error_code::syntax_error); }
    return decl;
}

// Marks a const declaration and holds it to its initializer; false (an error reported) for anything that is not a variable
bool parser::finish_const(const declaration_ptr& decl, const token& at, bool class_constant) {
    if (!decl) { return false; }
    if (decl->get_type() == node_type::function_decl) {
        report_foreign(detail::foreign_word::const_return, at);
        return false;
    }
    if (decl->get_type() == node_type::destructuring_decl) {
        static_cast<destructuring_decl*>(decl.get())->is_const = true;
        return true;
    }
    if (decl->get_type() != node_type::variable_decl) {
        report_foreign(detail::foreign_word::const_misplaced, at);
        return false;
    }
    auto* var = static_cast<variable_decl*>(decl.get());
    const bool reference = var->type && var->type->base_type == script_value_type::jai_reference_type;
    const bool class_default = var->type && var->type->base_type == script_value_type::jai_object_type && !var->type->type_name.empty();
    const bool missing = reference ? !var->initializer : (var->default_initialized || !var->initializer);
    if (missing && (class_constant || reference || !class_default)) {
        const std::string spelled = var->type ? var->type->canonical_name() : std::string("auto");
        report_error("const '" + std::string(var->name) + "' needs an initializer: const " + spelled + " " + std::string(var->name) + " = value;", at);
        return false;
    }
    var->is_const = true;
    return true;
}

bool parser::const_method_suffix() {
    if (!check(token_type::const_keyword)) { return false; }
    report_foreign(detail::foreign_word::const_method, peek());
    return true;
}

// [[warn_mode]] / [[strict_mode]] at a statement position, optional trailing ';'. none when
// the position holds no attribute (a '[[' there opens a nested array literal as before).
bool parser::check_mode_attribute_ahead() const {
    return current_ + 4 < stream_->size() && check(token_type::left_bracket) &&
           (*stream_)[current_ + 1].type == token_type::left_bracket &&
           (*stream_)[current_ + 2].type == token_type::identifier &&
           (*stream_)[current_ + 3].type == token_type::right_bracket &&
           (*stream_)[current_ + 4].type == token_type::right_bracket;
}

checked_result<uint8_t> parser::check_mode_attribute() {
    if (!check_mode_attribute_ahead()) {
        return check_mode_attr_none;
    }
    const token name = (*stream_)[current_ + 2];
    const uint8_t mode = name.lexeme == "warn_mode" ? check_mode_attr_warn
                       : name.lexeme == "strict_mode" ? check_mode_attr_strict : check_mode_attr_none;
    if (mode == check_mode_attr_none) {
        report_error("unknown attribute '" + std::string(name.lexeme) + "'", name);
        return make_error_code(parse_error_code::syntax_error);
    }
    current_ += 5;
    match(token_type::semicolon);
    return mode;
}

bool parser::nodiscard_attribute_ahead() const {
    return check_mode_attribute_ahead() && (*stream_)[current_ + 2].lexeme == "nodiscard";
}

// [[nodiscard]] marks a function or a method that returns a value; false (an error reported) for anything else
bool parser::mark_nodiscard(const declaration_ptr& decl, const source_location& at, std::string_view class_name) {
    auto* fn = decl && decl->get_type() == node_type::function_decl ? static_cast<function_decl*>(decl.get()) : nullptr;
    if (!fn || (!class_name.empty() && (fn->name == class_name || (fn->name.size() > 1 && fn->name[0] == '~')))) {
        report_error_at("[[nodiscard]] applies to a function or a method", at);
        return false;
    }
    if (fn->return_type && fn->return_type->base_type == script_value_type::jai_null_type && fn->return_type->type_name == "void") {
        report_error_at("[[nodiscard]] function '" + std::string(fn->name) + "' returns void: there is no result to keep", at);
        return false;
    }
    fn->is_nodiscard = true;
    return true;
}

// A coroutine runs on after its call returns, so a reference parameter would outlive the storage it names
bool parser::reject_coroutine_reference_parameter(const function_decl& coroutine) {
    for (const auto& p : coroutine.parameters) {
        if (!p.is_reference) { continue; }
        const std::string spelled = p.type ? p.type->canonical_name() : std::string("auto");
        report_error_at("coroutine '" + std::string(coroutine.name) + "' cannot take '" + p.name + "' by reference: it runs on after the call "
                        "returns, past the argument's lifetime; take it by value (" + spelled + " " + p.name +
                        "), or pass a shared_ptr to share an object", coroutine.location);
        return true;
    }
    return false;
}

// `& name(` / `& operator..` after a member's type: a reference-returning method (a class has no reference fields)
bool parser::reference_method_head() const {
    if (!check(token_type::ampersand) || current_ + 2 >= stream_->size() || (*stream_)[current_ + 1].type != token_type::identifier) {
        return false;
    }
    return (*stream_)[current_ + 1].lexeme == "operator" || (*stream_)[current_ + 2].type == token_type::left_paren;
}

type_info_ptr parser::wrap_reference_return_type(type_info_ptr return_type) {
    if (!match(token_type::ampersand)) {
        return return_type;
    }
    type_info refType(script_value_type::jai_reference_type);
    refType.type_name = return_type ? (return_type->type_name + "&") : "auto&";
    refType.type_params.push_back(return_type);
    refType.id = symbolizer_->intern(refType.canonical_name());
    return store_type_info(std::move(refType));
}

// `coroutine name(` / `coroutine operator+(`: no leading type and no `function` keyword.
// `coroutine IDENT (` never parsed (parse_type ate the name), so claiming it changes no
// valid program; `coroutine T name(` and `coroutine function name(` keep their paths.
bool parser::bare_coroutine_head() const {
    if (!check(token_type::identifier)) return false;
    return peek().lexeme == "operator" ||
           (current_ + 1 < stream_->size() && (*stream_)[current_ + 1].type == token_type::left_paren);
}

bool parser::coroutine_modifier_run() const {
    for (size_t i = current_; i < stream_->size(); ++i) {
        if ((*stream_)[i].type == token_type::coroutine_keyword) return true;
        if ((*stream_)[i].type != token_type::static_keyword && (*stream_)[i].type != token_type::override_keyword) return false;
    }
    return false;
}

checked_result<declaration_ptr> parser::bare_coroutine_declaration() {
    const token& name = advance();
    auto [name_id, name_view] = symbolizer_->intern_with_view(name.lexeme);
    return parse_function_body(name_view, name_id, nullptr);
}

checked_result<declaration_ptr> parser::function_declaration() {
    type_info_ptr return_type;
    if (!match(token_type::function_keyword)) { JAISCRIPT_TRY_ASSIGN(return_type, parse_type()); }   // `function f(...)` declares no return type
    return_type = wrap_reference_return_type(return_type);

    if (check(token_type::identifier) && reject_using_name(peek(), "function")) { return make_error_code(parse_error_code::syntax_error); }
    JAISCRIPT_TRY_ASSIGN(token name, consume(token_type::identifier, "expected function name"));
    auto [name_id, name_view] = symbolizer_->intern_with_view(name.lexeme);
    return parse_function_body(name_view, name_id, return_type);
}

checked_result<declaration_ptr> parser::parse_function_body(std::string_view name, uint64_t name_id, type_info_ptr return_type, bool allow_ctor_initializers) {
    jump_scope jumps_of_body(jumps_, {});
    auto func = std::make_shared<function_decl>(previous().location, name, name_id);
    // Only the body a coroutine head armed may yield: a nested function inherits nothing
    struct coroutine_scope { bool& flag; bool saved; ~coroutine_scope() { flag = saved; } } body_scope{in_coroutine_, in_coroutine_};
    in_coroutine_ = coroutine_body_pending_;
    coroutine_body_pending_ = false;
    coroutine_scope ctor_scope{in_constructor_, in_constructor_};
    in_constructor_ = allow_ctor_initializers;

    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after function name"));
    JAISCRIPT_TRY_ASSIGN(func->parameters, parse_parameter_list());
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after parameters"));
    if (const_method_suffix()) { return make_error_code(parse_error_code::syntax_error); }

    // Handle trailing return type
    if (match(token_type::arrow)) {
        // Check if return type is specified or if we go directly to {
        if (check(token_type::left_brace)) {
            // -> { adds no information: keep the leading type when one was given
            func->return_type = return_type;
        } else {
            JAISCRIPT_TRY_ASSIGN(func->return_type, parse_type());
            func->return_type = wrap_reference_return_type(func->return_type);
            // A leading return type and a contradictory trailing one is an error;
            // a matching pair is redundant-legal.
            if (return_type && func->return_type && func->return_type->id != return_type->id) {
                report_error("conflicting return types '" + return_type->type_name + "' and '" +
                             func->return_type->type_name + "' (leading and trailing '->' types must match)", previous());   // the body still parses
            }
        }
    } else {
        // No arrow - if we have a return type from before 'function', use it
        // Otherwise default to auto (nullptr) for inference
        func->return_type = return_type;  // nullptr means auto inference
    }

    // Parse constructor initialization list (: super(args), : this(args))
    if (check(token_type::colon) && !allow_ctor_initializers) {
        // Initializer lists are constructor-only; on free functions, destructors, and
        // function-keyword methods silently ignoring them would be a trap
        report_error("constructor initializer lists (': super(...)' / ': this(...)') are only allowed on constructors", peek());   // the list and the body still parse
    }
    if (match(token_type::colon)) {
        do {
            // Parse initializer target (super or this)
            if (match(token_type::super_keyword)) {
                // Parse super(args)
                JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'super'"));
                std::vector<expression_ptr> args;
                if (!check(token_type::right_paren)) {
                    do {
                        JAISCRIPT_TRY_ASSIGN(auto arg, expression());
                        args.push_back(bound_conditional(std::move(arg)));
                    } while (match(token_type::comma));
                }
                JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after super arguments"));
                func->initializers.emplace_back("super", std::move(args));
            } else if (match(token_type::this_keyword)) {
                // Parse this(args)
                JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'this'"));
                std::vector<expression_ptr> args;
                if (!check(token_type::right_paren)) {
                    do {
                        JAISCRIPT_TRY_ASSIGN(auto arg, expression());
                        args.push_back(bound_conditional(std::move(arg)));
                    } while (match(token_type::comma));
                }
                JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after this arguments"));
                func->initializers.emplace_back("this", std::move(args));
            } else if (check(token_type::identifier)) {
                // Parse Base(args): the base it names constructs with them (C++ [class.base.init])
                std::string base_name(advance().lexeme);
                JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after base class name"));
                std::vector<expression_ptr> args;
                if (!check(token_type::right_paren)) {
                    do {
                        JAISCRIPT_TRY_ASSIGN(auto arg, expression());
                        args.push_back(bound_conditional(std::move(arg)));
                    } while (match(token_type::comma));
                }
                JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after base constructor arguments"));
                func->initializers.emplace_back(std::move(base_name), std::move(args));
            } else {
                report_error("expected 'super', 'this' or a base class name in constructor initializer list", peek());
    return make_error_code(parse_error_code::syntax_error);
            }
        } while (match(token_type::comma));
    }

    coroutine_scope ref_return_scope{returns_reference_, returns_reference_};
    returns_reference_ = func->return_type && func->return_type->base_type == script_value_type::jai_reference_type;

    // Enter function scope for slot-based local variable tracking
    const bool nested_function = in_function_scope();
    enter_function_scope();
    if (nested_function) { function_scope_stack_.back().nested_function = name_id; }

    // Assign slots to parameters (they get slots 0, 1, 2, ...)
    for (auto& param : func->parameters) {
        if (param.symbol_id != UINT64_MAX) {
            param.slot_index = allocate_slot(param.symbol_id);
        }
    }

    if (!match(token_type::left_brace)) {
        exit_function_scope();  // Clean up on error
        report_error("expected '{' before function body", peek());
    return make_error_code(parse_error_code::syntax_error);
    }
    auto body = block_statement();
    if (!body) {
        exit_function_scope();
        return body.error_value();
    }
    func->body = std::dynamic_pointer_cast<block_stmt>(body.value());

    // Exit function scope and record total slot count
    func->local_count = exit_function_scope();

    return func;
}

expression_ptr parser::default_initializer(const type_info_ptr& type, const source_location& loc) {
    if (!type) return nullptr;
    switch (type->base_type) {
    case script_value_type::jai_int_type: return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_int(0)));
    case script_value_type::jai_float_type: return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_float(0.0)));
    case script_value_type::jai_bool_type: return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_bool(false)));
    case script_value_type::jai_char_type: return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_char(0)));
    case script_value_type::jai_string_type:
    case script_value_type::jai_utf8_string_type: return std::make_shared<literal_expr>(loc, script_value(script_value::ast_literal_tag{}, script_string()));
    case script_value_type::jai_array_type: return std::make_shared<array_literal_expr>(loc, std::vector<expression_ptr>());
    case script_value_type::jai_map_type: return std::make_shared<map_literal_expr>(loc, std::vector<std::pair<expression_ptr, expression_ptr>>());
    case script_value_type::jai_object_type:
        if (!type->type_name.empty()) return std::make_shared<new_expr>(loc, type, std::vector<expression_ptr>());
        return nullptr;
    case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type: case script_value_type::jai_color_type:
    case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type:
        return math_constructor_call(type->base_type, {}, loc);
    default: return nullptr;
    }
}

// math::K(args): the constructor intrinsic a math type's declaration initializes through
expression_ptr parser::math_constructor_call(script_value_type kind, std::vector<expression_ptr> arguments, const source_location& loc) {
    auto [math_id, math_view] = symbolizer_->intern_with_view("math");
    auto math_ident = std::make_shared<identifier_expr>(loc, math_view, math_id);
    math_ident->slot_index = lookup_slot(math_id);
    auto [kind_id, kind_view] = symbolizer_->intern_with_view(std::string_view(math_type_name(kind)).substr(6));
    auto callee = std::make_shared<member_expr>(loc, math_ident, kind_view, kind_id, false, true);
    return std::make_shared<call_expr>(loc, callee, std::move(arguments));
}

// array<T>(size) and array<T>(size, value) are std::vector's sized constructors, array::resize over an empty array<T>; array<T>{a, b}
// is its list, array::concat over an empty array<T>. Every other typed construction is a new_expr
checked_result<expression_ptr> parser::construction(const type_info_ptr& type, std::vector<expression_ptr> arguments, bool brace, const source_location& loc) {
    if (!type || type->base_type != script_value_type::jai_array_type || arguments.empty()) {
        return expression_ptr(std::make_shared<new_expr>(loc, type, std::move(arguments)));
    }
    if (!brace && arguments.size() > 2) {
        report_error("array<T>(...) takes a size and an optional value, not " + std::to_string(arguments.size()) + " arguments", previous());
        return make_error_code(parse_error_code::syntax_error);
    }
    auto [array_id, array_view] = symbolizer_->intern_with_view("array");
    auto array_ident = std::make_shared<identifier_expr>(loc, array_view, array_id);
    array_ident->slot_index = lookup_slot(array_id);
    auto [function_id, function_view] = symbolizer_->intern_with_view(brace ? "concat" : "resize");
    auto callee = std::make_shared<member_expr>(loc, array_ident, function_view, function_id, false, true);
    std::vector<expression_ptr> call_arguments;
    call_arguments.push_back(std::make_shared<new_expr>(loc, type, std::vector<expression_ptr>()));
    if (brace) {
        call_arguments.push_back(std::make_shared<array_literal_expr>(loc, std::move(arguments)));
    } else {
        for (auto& argument : arguments) { call_arguments.push_back(std::move(argument)); }
    }
    return expression_ptr(std::make_shared<call_expr>(loc, callee, std::move(call_arguments)));
}

checked_result<expression_ptr> parser::initializer_from_arguments(const type_info_ptr& type, std::vector<expression_ptr> arguments, bool brace, const source_location& loc) {
    switch (type ? type->base_type : script_value_type::jai_any_type) {
    case script_value_type::jai_vec2_type: case script_value_type::jai_vec3_type: case script_value_type::jai_vec4_type: case script_value_type::jai_color_type:
    case script_value_type::jai_ivec2_type: case script_value_type::jai_ivec3_type:
        return math_constructor_call(type->base_type, std::move(arguments), loc);
    case script_value_type::jai_int_type: case script_value_type::jai_float_type: case script_value_type::jai_bool_type:
    case script_value_type::jai_char_type: case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
        // a primitive: () and {} are its zero, one argument is its value
        if (arguments.empty()) return default_initializer(type, loc);
        if (arguments.size() == 1) return std::move(arguments[0]);
        report_error("a '" + type->canonical_name() + "' is initialized by one value, not " + std::to_string(arguments.size()), previous());
        return make_error_code(parse_error_code::syntax_error);
    default:
        return construction(type, std::move(arguments), brace, loc);
    }
}

bool parser::paren_group_ends_statement(size_t lparen) const {
    size_t depth = 0;
    for (size_t i = lparen; i < stream_->size(); ++i) {
        const token_type k = (*stream_)[i].type;
        if (k == token_type::left_paren) { ++depth; }
        else if (k == token_type::right_paren) {
            if (--depth == 0) { return i + 1 < stream_->size() && (*stream_)[i + 1].type == token_type::semicolon; }
        }
        else if (k == token_type::eof) { return false; }
    }
    return false;
}

checked_result<declaration_ptr> parser::variable_declaration() {
    const bool function_spelling = check(token_type::function_keyword);
    JAISCRIPT_TRY_ASSIGN(type_info_ptr type, parse_type());

    // Check for reference after type (e.g., int& x or auto& x)
    bool is_reference = false;
    if (match(token_type::ampersand)) {
        is_reference = true;
        // Create a reference type
        type_info refType(script_value_type::jai_reference_type);
        refType.type_name = type ? (type->type_name + "&") : "auto&";
        refType.type_params.push_back(type);
        refType.id = symbolizer_->intern(refType.canonical_name());
        type = store_type_info(std::move(refType));
    }

    JAISCRIPT_TRY_ASSIGN(token name, consume_name("expected variable name", "variable"));

    expression_ptr initializer = nullptr;
    if (match(token_type::equal)) {
        JAISCRIPT_TRY_ASSIGN(initializer, expression());
        if (is_reference) { initializer = bound_conditional(std::move(initializer)); }
    } else if ((check(token_type::left_brace) || check(token_type::left_paren)) && type && !is_reference) {
        // Type name{args...} / Type name(args...): construction with the declared type (a primitive takes its one value)
        const bool brace = check(token_type::left_brace);
        advance();
        JAISCRIPT_TRY_ASSIGN(auto arguments, construction_arguments(brace, "expected closing bracket after constructor arguments"));
        JAISCRIPT_TRY_ASSIGN(initializer, initializer_from_arguments(type, std::move(arguments), brace, name.location));
    } else if (check(token_type::left_brace)) {
        // Auto type with brace initializer - parse as expression
        JAISCRIPT_TRY_ASSIGN(initializer, expression());
    }
    if (!initializer && is_reference) {
        report_error("reference variable '" + std::string(name.lexeme) + "' must be initialized: a reference names a variable from its declaration on", name);
    }
    const bool default_initialized = !initializer && !is_reference;
    if (default_initialized) {
        initializer = default_initializer(type, name.location);   // T x;: the type's default (a class default-constructs)
    }

    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after variable declaration"));

    auto decl = std::make_shared<variable_decl>(name.location, type, name.lexeme, get_symbol_id(name), initializer);
    decl->function_spelling = function_spelling;
    decl->default_initialized = default_initialized;

    // If we're inside a function, assign a slot for this local variable
    if (in_function_scope() && decl->name_id != UINT64_MAX) {
        decl->slot_index = allocate_slot(decl->name_id);
    }

    return decl;
}

void parser::consume_greater_in_generic(std::string_view message) {
    // Handle the case where >> is tokenized as RightShift when we need >
    if (check(token_type::right_shift)) {
        // We have >>, but we only want to consume one >
        token rightShift = advance();
        
        // Push back a synthetic > token for the second >
        // This allows the parser to continue as if there were two separate > tokens
        token syntheticGreater(token_type::greater, ">", rightShift.location);
        pushed_back_token_ = syntheticGreater;
        
        // We've consumed one > (implicitly), the other is pushed back
        return;
    }

    // Normal case: just consume a > token (intentionally ignoring result for error recovery)
    (void)consume(token_type::greater, message);
}

bool parser::is_registered_template_type(const std::string& type_name) const {
    return registered_template_types_.find(type_name) != registered_template_types_.end();
}

checked_result<statement_ptr> parser::switch_statement() {
    jump_scope cases(jumps_, {jumps_.breakable + 1, jumps_.continuable, jumps_.parallel_body});
    token switchToken = previous();

    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'switch'"));
    JAISCRIPT_TRY_ASSIGN(expression_ptr condition, expression());
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after switch condition"));

    JAISCRIPT_TRY(expect(token_type::left_brace, "expected '{' after switch condition"));

    auto switchStmt = std::make_shared<switch_stmt>(switchToken.location, condition);
    bool empty_case_above = false;

    // Parse cases and default
    while (!check(token_type::right_brace) && !is_at_end()) {
        if (match(token_type::case_keyword)) {
            // Parse case
            JAISCRIPT_TRY_ASSIGN(expression_ptr caseValue, expression());
            JAISCRIPT_TRY(expect(token_type::colon, "expected ':' after case value"));

            auto caseStmt = std::make_shared<case_stmt>(previous().location, caseValue);

            // Set context for parsing case body
            bool oldInSwitchCase = in_switch_case_;
            in_switch_case_ = true;
            bool ended_by_break = false;

            // Parse case body until we hit another case, default, or end of switch
            while (!check(token_type::case_keyword) && !check(token_type::default_keyword) &&
                   !check(token_type::right_brace) && !is_at_end()) {

                // Check for fallthrough as the last statement
                if (check(token_type::fallthrough_keyword)) {
                    auto fallthroughStmt = std::make_shared<fallthrough_stmt>(peek().location);
                    advance(); // consume 'fallthrough'
                    auto consume_result = consume(token_type::semicolon, "expected ';' after 'fallthrough'");
                    if (!consume_result) {
                        in_switch_case_ = oldInSwitchCase;
                        return consume_result.error();
                    }
                    caseStmt->body.push_back(fallthroughStmt);
                    caseStmt->has_fallthrough = true;
                    break; // fallthrough must be the last statement in a case
                }

                // Check for break statement (it's allowed but redundant)
                if (check(token_type::break_keyword)) {
                    advance(); // consume 'break'
                    auto consume_result = consume(token_type::semicolon, "expected ';' after 'break'");
                    if (!consume_result) {
                        in_switch_case_ = oldInSwitchCase;
                        return consume_result.error();
                    }
                    // Don't add break to the body since it's implicit
                    ended_by_break = true;
                    break;
                }

                auto stmt_result = case_body_statement();
                if (!stmt_result) {
                    in_switch_case_ = oldInSwitchCase;
                    return stmt_result.error();
                }
                caseStmt->body.push_back(stmt_result.value());
            }

            // Restore context
            in_switch_case_ = oldInSwitchCase;

            // a case with no statements shares the body of the label below it
            empty_case_above = caseStmt->body.empty() && !ended_by_break && (check(token_type::case_keyword) || check(token_type::default_keyword));
            if (empty_case_above) {
                caseStmt->body.push_back(std::make_shared<fallthrough_stmt>(caseStmt->location));
                caseStmt->has_fallthrough = true;
            }

            switchStmt->cases.push_back(caseStmt);

        } else if (match(token_type::default_keyword)) {
            // Parse default
            if (switchStmt->default_case) {
                report_error("multiple default labels in switch statement", previous());
    return make_error_code(parse_error_code::syntax_error);
            }

            JAISCRIPT_TRY(expect(token_type::colon, "expected ':' after 'default'"));

            auto defaultStmt = std::make_shared<default_stmt>(previous().location);

            // Set context for parsing default body
            bool oldInSwitchCase = in_switch_case_;
            in_switch_case_ = true;

            bool default_ended_by_break = false;
            while (!check(token_type::case_keyword) && !check(token_type::default_keyword) &&
                   !check(token_type::right_brace) && !is_at_end()) {

                // Check for break statement (allowed but redundant)
                if (check(token_type::break_keyword)) {
                    advance(); // consume 'break'
                    auto consume_result = consume(token_type::semicolon, "expected ';' after 'break'");
                    if (!consume_result) {
                        in_switch_case_ = oldInSwitchCase;
                        return consume_result.error();
                    }
                    default_ended_by_break = true;
                    break;
                }

                auto stmt_result = case_body_statement();
                if (!stmt_result) {
                    in_switch_case_ = oldInSwitchCase;
                    return stmt_result.error();
                }
                defaultStmt->body.push_back(stmt_result.value());
            }

            // Restore context
            in_switch_case_ = oldInSwitchCase;

            // default's body runs after every case's, so it shares one only as the last label
            if (((defaultStmt->body.empty() && !default_ended_by_break) || empty_case_above) && check(token_type::case_keyword)) {
                report_error("a label that shares a body with 'default' needs 'default' as the last label of the switch", peek());
                return make_error_code(parse_error_code::syntax_error);
            }
            empty_case_above = false;
            switchStmt->default_case = defaultStmt;

        } else {
            report_error("expected 'case' or 'default' in switch statement", peek());
    return make_error_code(parse_error_code::syntax_error);
        }
    }

    JAISCRIPT_TRY(expect(token_type::right_brace, "expected '}' after switch body"));

    return switchStmt;
}

std::pair<uint64_t, std::string_view> parser::hidden_name(std::string_view stem) {
    std::string name(stem);
    name += '#';
    return symbolizer_->intern_with_view(name);
}

expression_ptr parser::hidden_identifier(uint64_t id, std::string_view name, const source_location& loc) {
    auto ident = std::make_shared<identifier_expr>(loc, name, id);
    ident->slot_index = lookup_slot(id);
    return ident;
}

bool parser::do_while_ahead() const {
    return check(token_type::identifier) && peek().lexeme == "do" && current_ + 1 < stream_->size() &&
           (*stream_)[current_ + 1].type == token_type::left_brace;
}

checked_result<statement_ptr> parser::do_while_statement() {
    const token do_token = previous();
    block_scope_guard loop_scope(*this);
    const auto [flag_id, flag_name] = hidden_name("do");
    script_value first_pass(script_value::ast_literal_tag{}, true);
    auto flag = std::make_shared<variable_decl>(do_token.location, store_type_info(type_info::make_bool(*symbolizer_)), flag_name, flag_id,
                                                std::make_shared<literal_expr>(do_token.location, first_pass));
    if (in_function_scope()) { flag->slot_index = allocate_slot(flag_id); }

    jump_scope loop(jumps_, {jumps_.breakable + 1, jumps_.continuable + 1, jumps_.parallel_body});
    JAISCRIPT_TRY_ASSIGN(statement_ptr body, statement());
    JAISCRIPT_TRY_ASSIGN(token while_token, consume(token_type::while_keyword, "expected 'while' after the body of 'do'"));
    JAISCRIPT_TRY(expect(token_type::left_paren, "expected '(' after 'while'"));
    const size_t condition_start = current_;
    JAISCRIPT_TRY_ASSIGN(expression_ptr condition, expression());
    mark_bare_condition(condition, condition_start);
    JAISCRIPT_TRY(expect(token_type::right_paren, "expected ')' after while condition"));
    JAISCRIPT_TRY(expect(token_type::semicolon, "expected ';' after do-while condition"));

    token or_op(token_type::pipe_pipe, "||", while_token.location);
    auto test = std::make_shared<binary_expr>(while_token.location, hidden_identifier(flag_id, flag_name, while_token.location), or_op, condition);
    token assign(token_type::equal, "=", while_token.location);
    script_value later_pass(script_value::ast_literal_tag{}, false);
    auto update = std::make_shared<assignment_expr>(while_token.location, hidden_identifier(flag_id, flag_name, while_token.location), assign,
                                                    std::make_shared<literal_expr>(while_token.location, later_pass));
    return std::make_shared<for_stmt>(do_token.location, flag, test, update, body);
}

void parser::report_foreign(detail::foreign_word w, const token& at) {
    report_error(std::string(detail::foreign_word_text(w)), at);
}

bool parser::foreign_word_at_statement() {
    const token& t = peek();
    if (t.type != token_type::identifier || current_ + 1 >= stream_->size()) return false;
    const token& next = (*stream_)[current_ + 1];
    const bool declaration_follows = next.type == token_type::identifier || is_type_keyword_token(next.type) || next.type == token_type::const_keyword ||
                                     next.type == token_type::static_keyword;
    if (t.lexeme == "constexpr" && declaration_follows) {
        report_foreign(detail::foreign_word::constexpr_declaration, t);
        return true;
    }
    if (t.lexeme == "mutable" && declaration_follows) {
        report_foreign(detail::foreign_word::mutable_field, t);
        return true;
    }
    if (t.lexeme == "delete" && (next.type == token_type::identifier || next.type == token_type::this_keyword || next.type == token_type::left_bracket)) {
        report_foreign(detail::foreign_word::delete_statement, t);
        return true;
    }
    if (t.lexeme == "goto" && next.type == token_type::identifier && current_ + 2 < stream_->size() &&
        (*stream_)[current_ + 2].type == token_type::semicolon) {
        report_foreign(detail::foreign_word::goto_statement, t);
        return true;
    }
    if (t.lexeme == "virtual" && (next.type == token_type::identifier || is_type_keyword_token(next.type) || next.type == token_type::static_keyword)) {
        report_foreign(detail::foreign_word::virtual_method, t);
        return true;
    }
    return false;
}

uint64_t parser::get_symbol_id(const token& tok) const {
    // If lexer already interned this token (identifiers), use that
    if (tok.symbol_id != 0) {
        return tok.symbol_id;
    }
    // Otherwise intern now (for keywords used as identifiers, etc.)
    return symbolizer_->intern(tok.lexeme);
}

} // namespace jai
