#pragma once

#ifndef __JAISCRIPT_DETAIL_AGGREGATE_LOWERING_HPP__
#define __JAISCRIPT_DETAIL_AGGREGATE_LOWERING_HPP__

// The aggregate lowering: a typed math local of a function or lambda body becomes its
// component slots (`p` -> `p$x`, `p$y`, `p$z`, float slots for the binary32 kinds and int slots for the ivec kinds), and
// the vector operations over such locals become the kernel's component steps (detail/math_intrinsics.hpp f32$, i32$,
// cstore$, i32add$ .. i32neg$, vcomp$), so both backends run float and int slots and compute the boxed kernel's bits and texts. A
// vector that must exist as a value (a call argument, a container element, a return) is rebuilt from its components at
// that point. A local that escapes (a reference, a capture, a call argument, a store in expression position) stays
// boxed. The rewrite is a pure function of the parsed body: each lowered site keeps both bodies, and the engine shows
// the checker's diagnostics and the jaibite writer the parsed one (set_aggregate_view), everything else the lowered one.

#include <jaiscript/detail/ast.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jai { class engine; }

namespace jai::detail {

    struct lowered_site {
        function_decl* decl = nullptr;   // one of the two
        lambda_expr* lambda = nullptr;
        statement_ptr source_body, lowered_body;
        size_t source_locals = 0, lowered_locals = 0;
    };

    struct aggregate_lowering_state {
        bool built = false;
        bool lowered_view = false;
        size_t lowered_locals = 0;   // typed locals and parameters the sites replace (engine::aggregate_lowered_locals)
        std::vector<lowered_site> sites;
    };

    // Lowers every function and lambda body under `declarations` (once per state)
    void build_aggregate_lowering(engine& eng, const std::vector<declaration_ptr>& declarations, aggregate_lowering_state& state);

    // Shows every lowered site's lowered body (true) or its parsed body (false)
    void set_aggregate_view(aggregate_lowering_state& state, bool lowered);

    // The source view for a scope (diagnostics, the jaibite writer), the previous view restored at its end
    struct aggregate_source_view {
        aggregate_lowering_state* state;
        bool was;
        explicit aggregate_source_view(aggregate_lowering_state* s) : state(s), was(s && s->lowered_view) { if (state) { set_aggregate_view(*state, false); } }
        ~aggregate_source_view() { if (state) { set_aggregate_view(*state, was); } }
        aggregate_source_view(const aggregate_source_view&) = delete;
        aggregate_source_view& operator=(const aggregate_source_view&) = delete;
    };

    // A frame's named locals as the source spells them: the component slots of a lowered local fold back into its vector
    // (replacing the stale source slot), and the rewrite's temporaries are dropped
    void fold_lowered_locals(std::vector<std::pair<std::string, script_value>>& locals, engine* eng);

} // namespace jai::detail

#endif // __JAISCRIPT_DETAIL_AGGREGATE_LOWERING_HPP__
