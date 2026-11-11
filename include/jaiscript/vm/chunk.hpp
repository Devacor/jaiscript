#pragma once

#ifndef __JAISCRIPT_VM_CHUNK_HPP__
#define __JAISCRIPT_VM_CHUNK_HPP__

#include "opcode.hpp"
#include <jaiscript/core/value.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/host_member.hpp>
#include <deque>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace jai {
    class class_definition;
    class script_class_definition;
    struct script_method_dispatch;
}

namespace jai::vm {

    struct chunk;

    // One side of a fused binary/compound operand: identifier (slot + symbol index)
    // or literal (constant index). Mirrors what op_load/op_const would resolve.
    // One level of a fused subscript chain: an identifier index or an int literal.
    struct fused_subscript_index {
        static constexpr uint8_t index_ident = 1;
        static constexpr uint8_t index_literal = 2;
        uint8_t kind = index_ident;
        uint32_t slot = k_invalid_u32;    // ident: function-frame slot (k_invalid_u32 = env)
        uint32_t symbol = k_invalid_u32;  // ident: index into chunk::symbols
        int64_t literal = 0;              // literal: the int index
    };

    struct fused_operand {
        uint32_t slot = k_invalid_u32;        // function-frame slot (k_invalid_u32 = env)
        uint32_t symbol = k_invalid_u32;      // index into chunk::symbols; invalid = literal
        uint32_t const_index = k_invalid_u32; // index into chunk::constants; invalid = identifier
        uint32_t load_flags = 0;              // load_flag_* (type-ctor names)
        // Env-resolved plain identifier: chunk::nodes index of its identifier_expr, read live
        // for field proofs; invalid = slotted / literal / subscript
        uint32_t ident_node = k_invalid_u32;
        // a bare name the body's class resolves to a plain field (chunk::member_class): its slot + 1 there, 0 for none
        uint32_t this_slot = 0;
        // a bare name the body's class resolves past its members: read from the environment while the frame's layout holds (chunk::this_global_uses)
        bool this_global = false;

        // Subscript-read operand `base[i][j]...[k]` (every level transient-marked, so the
        // elision rule applies): slot/symbol above name the BASE container; the chain
        // holds one entry per level, OUTERMOST-application order (base[chain[0]][chain[1]]...).
        // Truly N-level: array levels resolve in place, any other level (map key, borrow,
        // OOB) replays that one level through the real op_index and the walk continues.
        // Only exec_binary_fused resolves these; fused_cmp_operand and the counted-for /
        // call-site consumers never see one (compiler emits them for binary operands only).
        std::vector<fused_subscript_index> subscript_chain;
        bool is_subscript() const noexcept { return !subscript_chain.empty(); }

        // INDEX-fusion containers only: chunk::nodes index of a member_expr — the operand
        // is `base.member` where slot/symbol name the base identifier. The exec resolves
        // the member through its own member_ic row (fill = exec_get_member's exact probe)
        // or the member_access_value ladder, base→member→index order preserved.
        uint32_t member_node = k_invalid_u32;

        // A plain frame-slot identifier (or a compiler temp): the typed heads address it
        // straight off the frame, no resolution ladder
        bool direct = false;
    };

    // op_push_fused: `recv.push(v);` with an identifier receiver and a slotted-identifier or
    // literal value; site is the call_site the unfused sequence would have used (the replay
    // path and its argument metadata)
    // `base.member = value;` in statement position (op_set_member_value): the member node, the base's frame slot and the value as an
    // operand (an identifier, a literal, or the temp a computed value lands in)
    struct member_store_proto {
        uint32_t node = k_invalid_u32;
        uint32_t base = k_invalid_u32;
        fused_operand value;
        bool release = false;   // the value is a compiler temp the store gives up (an instance never takes the scalar rows)
    };
    // `a[i].x` (op_velem_load) and `a[i].x op= v;` (op_velem_store) where the checker proved `a` a math vector buffer, `a` a frame
    // slot and `i` a frame slot or an int literal: the subscript as a one-level fused operand, the member node, the component each
    // vector kind names (-1 none, indexed by kind - v2) and, for a store, the compound kind and flags and the value
    // op_get_member_path: the chain's links, the outer member first; a subscript link reads its index from a frame slot or a constant
    // and replays as op_index under its flags, a member link names its member_expr through the chain
    struct member_path_proto {
        struct link {
            fused_operand index;
            uint32_t index_flags = 0;
            bool subscript = false;
        };
        std::vector<link> links;
        uint32_t root_slot = k_invalid_u32;   // the root identifier's frame slot (k_invalid_u32: the root's value was pushed)
    };
    struct velem_proto {
        fused_operand sub;
        uint32_t node = k_invalid_u32;
        uint32_t kind = 0;
        bool compound = false;
        int8_t comp[6] = {-1, -1, -1, -1, -1, -1};
        fused_operand value;
    };
    struct push_proto {
        fused_operand receiver;
        fused_operand value;              // ident / literal / one-level subscript (base ident, ident-or-literal index)
        uint32_t site = k_invalid_u32;
        // subscript value: the unfused sequence probes the member before the argument runs
        // (call_args_need_probe) and reads the element as an lvalue-shaped op_index; the
        // replay path reproduces both
        bool value_is_subscript = false;
        uint32_t value_index_flags = 0;
        bool value_temp = false;          // the value is a compiler temp only this push reads: it dies once the push took its copy
        bool converting_receiver = false; // the receiver is declared with an element type a push converts to (not a scalar, not var): a block runs the push as a boundary
    };

    // Per-call-site argument metadata: symbol id per argument (UINT64_MAX = not an identifier)
    struct call_site {
        std::vector<uint64_t> arg_symbols;
        std::vector<uint32_t> arg_slots;           // caller frame slot per argument (k_invalid_u32 = env-resolved)
        std::vector<uint8_t> arg_global;           // ::name argument (ref binding = GLOBAL env only)
        // chunk::nodes index per ref-bindable lvalue argument (k_invalid_u32 = none);
        // lazily materialized - empty when no argument qualifies
        std::vector<uint32_t> arg_lvalue_nodes;
        uint32_t member_node = k_invalid_u32;      // op_call_method: member_expr node index
        uint32_t call_node = k_invalid_u32;        // chunk::nodes index of the call_expr
        uint64_t receiver_symbol = UINT64_MAX;     // op_call_method: identifier receiver symbol id
        uint32_t receiver_slot = k_invalid_u32;    // op_call_method: caller frame slot of that receiver (k_invalid_u32 = env)
        // Compile-time builtin dispatch indices (name-sorted, engine-independent — see
        // builtin_method_registries): 0xFFFF = the member is not a builtin of that
        // registry. Valid only when the owning chunk's builtin_indexed is set.
        uint16_t bi_array = 0xFFFF;
        uint16_t bi_map = 0xFFFF;
        uint16_t bi_string = 0xFFFF;
        // op_call_method whose result is used under the name of an in-place builtin: the call raises when the receiver's kind
        // runs that builtin (detail/in_place_result.hpp); such a site never takes op_call_builtin
        bool in_place_check = false;
        // Monomorphic in-loop callee cache: keyed on the address of
        // the loaded function value's shared script_function payload; ic_pin keeps that
        // object alive so a hit can never be a recycled address (ABA) — a redefined name
        // mints a new script_function, so the site misses and revalidates (at most one
        // superseded function stays pinned per site until the refill). Never WRITTEN on
        // parallel workers (chunks are shared across worker function copies; same
        // cached_global_env_ gate as the env-lookup caches).
        // Identifier callee operand (op_probe_callee sites): resolved like op_load
        fused_operand callee;
        mutable const script_function* ic_identity = nullptr;
        mutable const script_defined_function* ic_fn = nullptr;
        mutable strong_ptr<script_function_node> ic_pin;
        mutable uint32_t ic_argc = 0;
        mutable bool ic_native = false;   // the cached callee is a host callable (ic_fn null): the call invokes it with the arguments as they are
        mutable bool ic_host_checked = false;   // the lane was looked for since the last refill (filled on a hit: a binding that holds)
        // Monomorphic method-dispatch cache: validated on
        // (class identity, method_epoch) per class_definition::bump_method_epoch's
        // contract (derived-propagating; definitions engine-lifetime pinned, no ABA).
        // Caches the resolve ladder's outcome for one receiver class: the pre-minted
        // dispatcher value, its dispatch payload, the resolved single overload.
        // Instances can grow fields at runtime and a field shadows the method, so a
        // hit re-probes has_field; typed-parameter methods stay arg-type-sensitive
        // (pick_best_overload can reject a single overload), so a hit re-runs
        // resolve_method_overload unless mic_static proved resolution
        // arg-independent (every arg-bound param untyped). Same cached_global_env_
        // gate as the caches above.
        mutable const class_definition* mic_cd = nullptr;
        mutable uint64_t mic_epoch = 0;
        mutable script_value mic_method{std::monostate{}, nullptr};
        mutable const script_method_dispatch* mic_dispatch = nullptr;
        // a bare sibling call of the receiver's C++ base method (mic_dispatch null, mic_method the host method): its direct lane
        mutable detail::host_method_hit mic_native;
        mutable std::shared_ptr<function_decl> mic_resolved;
        mutable bool mic_static = false;
        mutable bool mic_field_free = false;   // no field in the class chain is named like the method
        mutable uint8_t mic_lean = 0;   // the site's own conditions for the lean lane (vm_backend::lean_method_site): 1 hold, 2 not, 0 not asked
        // the classes a polymorphic method site met before the one above, each under the same (class identity, method_epoch) rule
        struct mic_entry {
            const class_definition* cd = nullptr;
            uint64_t epoch = 0;
            bool field_free = false;
            script_value method{std::monostate{}, nullptr};
            const script_method_dispatch* dispatch = nullptr;
            std::shared_ptr<function_decl> resolved;
        };
        static constexpr uint8_t k_mic_poly = 3;
        mutable mic_entry mic_poly[k_mic_poly];
        mutable uint8_t mic_poly_next = 0;
        // op_probe_member memo for sites the mic never arms (multi-overload methods):
        // "the lookup succeeds for this class at this method_epoch"; same gates as mic_*
        mutable const class_definition* probe_cd = nullptr;
        mutable uint64_t probe_epoch = 0;
        mutable bool probe_field_free = false;
        // Host native-lane memo (class_definition::native_entry) for one receiver class, null entry included: validated on
        // (class identity, native_stamp, field_shape_epoch); a hit still asks the instance for a field of the method's name
        mutable const class_definition* nat_cd = nullptr;
        mutable uint64_t nat_stamp = 0;   // a host class's native_stamp; a script layer's method_epoch
        mutable uint64_t nat_shape = 0;
        mutable detail::host_method_hit nat_hit;   // the entry (null: no lane) and the receiver layout it was measured under
        // The member ladder's answer for a host method no lane takes: the class's method value at (class identity, method
        // epoch), where no access check, user getter, field or builtin handle method of the name sits in front of it
        mutable const class_definition* ord_cd = nullptr;
        mutable uint64_t ord_epoch = 0;
        mutable script_value ord_method{std::monostate{}, nullptr};
        mutable bool nat_field_free = false;
        std::vector<uint32_t> staged_argument_slots;   // immutable owned temps released if this call is abandoned before invocation
        // op_new: the class name's symbol, interned when the site compiles
        uint64_t new_symbol = UINT64_MAX;
        // Constructor memo (op_new, class-name calls): the constructor value's thunk read once per value identity (a
        // redefinition mints a new value; the pin keeps a cached identity from being recycled) and its script class
        // cast once per class (definitions are engine-lifetime pinned). Same cached_global_env_ gate as the caches above.
        mutable const script_function* ctor_identity = nullptr;
        mutable strong_ptr<script_function_node> ctor_pin;
        mutable const void* ctor_thunk = nullptr;
        mutable const class_definition* ctor_cls_key = nullptr;
        mutable std::shared_ptr<script_class_definition> ctor_cls;
        // the cached host callable's one direct overload (engine::overload_dispatch::direct_entry): a call whose arguments are of its
        // parameters' kinds runs the binding through the invoker, the callee borrowed from the site; kept off the lines every call reads
        mutable const void* ic_host_target = nullptr;
        mutable checked_result<script_value> (*ic_host_invoke)(const void*, const script_value*, size_t) = nullptr;
        mutable uint32_t ic_host_argc = 0;
        mutable script_value_type ic_host_kinds[8]{};
        mutable strong_ptr<script_function_node> ic_host_owner;
        mutable const uint64_t* ic_host_gen = nullptr;   // the overload set's mutation count: a registration of the name since the fill declines the lane
        mutable uint64_t ic_host_at = 0;
        mutable const void* call_host_seen = nullptr;   // op_call: the callee identity the site last looked a direct entry up for
    };

    struct function_proto {
        std::shared_ptr<script_defined_function> fn;
        std::shared_ptr<function_decl> decl;
        bool is_coroutine = false;
        // Coroutines only: enclosing-frame slot locals the body references (derived
        // or restored, like closure_proto::outer_slot_plan) - snapshot at declaration
        std::vector<std::pair<uint64_t, size_t>> outer_slot_plan;
    };

    struct closure_proto {
        std::shared_ptr<lambda_expr> lambda;
        std::shared_ptr<block_stmt> body;                  // block-wrapped lambda body
        std::shared_ptr<chunk> body_chunk;
        // Capture plan derived by the compiler or copied from completed AST patches:
        // outer-frame locals referenced by the body, as (symbol id, outer slot).
        std::vector<std::pair<uint64_t, size_t>> outer_slot_plan;
        std::vector<uint64_t> used_variables;              // for [=]/[&] default capture
        struct explicit_capture {
            uint64_t symbol_id;
            bool by_reference;
            size_t outer_slot = SIZE_MAX;
            bool init = false;   // an init-capture: its value is on the stack under op_closure, in capture order
            bool weak_instance = false;   // [weak_this]
        };
        std::vector<explicit_capture> captures;
        uint8_t default_capture = 0;                       // lambda_expr::capture_default
        bool captures_this = false;
        bool uses_instance = false;                        // this/super in the body
        // The converted form, made at the node's first creation that can use it: the
        // captured names in the order the environment form defines them, their slots after the parameters, the chunk over them
        struct converted_form {
            std::shared_ptr<const std::vector<uint64_t>> ids;
            std::vector<size_t> outer_slots;               // each name's slot in the creating frame
            std::vector<uint8_t> read_only;                // the body only reads the name (no store, no call argument, no method receiver)
            std::shared_ptr<chunk> body_chunk;
        };
        mutable std::shared_ptr<const converted_form> converted;
        mutable uint8_t convert_state = 0;                 // 0 not judged, 1 converts, 2 never
    };

    struct destructure_proto {
        std::vector<std::pair<uint64_t, size_t>> names;    // (symbol id, slot index)
    };

    struct iter_proto {
        uint64_t var_symbol = UINT64_MAX;
        size_t slot = SIZE_MAX;                            // SIZE_MAX = define through the environment
        bool is_reference = false;
        type_info_ptr element_type;                        // a declared loop variable type the binding converts to (null: auto, var)
        std::string_view var_name;                         // the variable, for the binding's rejection text
        bool conversion_free = true;                       // the checker saw the elements hold the declared type
        bool binds_map_entry = false;                      // for (auto [k, v] : m): over a map the variable holds the entry's cursor
        uint64_t binding_text = UINT64_MAX;                // its "[k, v]" spelling, interned (a non-decomposing element's text)
    };

    // op_binary_fused: evaluates `left op right` without materializing operand loads
    // on the value stack (the vm's analog of the interpreter's AST-shape fast paths)
    struct fused_binary_proto {
        uint8_t op = 0;                        // token_type
        // The checker's proof-grade operand class (binary_expr::static_operand_class) when
        // both operands are direct slots or literals: 1 = int/int, 2 = float/float, 0 = none.
        // Selects the typed head; the head's tag guards keep it sound and the general
        // kernel below it runs on any miss (proofs off = 0 everywhere, byte-identical).
        uint8_t typed_class = 0;
        // an operand is a compiler temp holding an element read: an index fold over it may take the typed
        // lane without a proof (the load's guards decide, the original instruction runs on a miss)
        bool elem_read_operand = false;
        // the compiler temps this consumer gives up once it has read them (1 left, 2 right): a value that may own an instance never takes a
        // numeric row, so the rows that read one release it
        uint8_t release = 0;
        fused_operand left;
        fused_operand right;
    };

    // Counting-loop pattern `for (i = lit; i cmp end; i +=/-= step)` — the vm analog of
    // the interpreter's native-loop fast path. When the runtime types don't cooperate
    // the loop falls back to the generic cond/update bytecode (targets below).
    struct counted_for_proto {
        fused_operand var;                     // loop variable (identifier)
        fused_operand end;                     // identifier or int literal
        fused_operand step;                    // identifier or int literal
        uint8_t cmp = 0;                       // token_type of the comparison
        bool step_subtract = false;            // -= / -- update
        bool step_incdec = false;              // ++/-- update (overflow error names the real op)
        // Jump targets patched at compile time
        uint32_t body_ip = k_invalid_u32;
        uint32_t exit_ip = k_invalid_u32;      // past the op_cfor_pop (fast exits pop in-op)
        uint32_t generic_cond_ip = k_invalid_u32;
        uint32_t generic_update_ip = k_invalid_u32;
    };

    // Per-instruction env-lookup inline cache, two heads. FAST head {fast_env,
    // fast_serial, fast_ptr}: valid while the CURRENT env identity and the engine-wide
    // env serial (string_symbolizer::env_epoch - every env construct/reset/clear/define/
    // parent change draws from it) are both unchanged, which proves the whole chain
    // unmodified - two compares, no transparency walk, arbitrary (even shadowing) chains.
    // Armed only with env-LOCAL cells (vm_storage_lookup cacheable=true), never
    // this/static-field fallbacks: rebind_method_this deliberately doesn't advance the
    // serial. SLOW head {env, epoch, ptr}: valid while the resolve env's OWN epoch
    // (environment::local_epoch) is unchanged - survives the unrelated env churn that
    // advances the engine serial (method scopes resetting per call), so call-heavy code
    // keeps hitting when the fast head can't. Stored pointers target stable deque
    // storage in the entry's env or an ancestor (a parent cannot be pool-recycled while
    // its child chain lives; the global env is engine-lifetime with in-place
    // redefinition).
    struct env_lookup_cache_entry {
        const environment* env = nullptr;
        uint64_t epoch = 0;
        script_value* ptr = nullptr;
        const environment* fast_env = nullptr;
        uint64_t fast_serial = 0;
        script_value* fast_ptr = nullptr;
        // This-field IC (bare method-field reads): the engine-wide env serial recorded
        // when this site's FULL WALK missed and the this-fallback resolved a field.
        // While the serial holds (no define/reparent anywhere), the walk provably still
        // misses, so resolution may jump straight to the receiver's field cell. 0 = unarmed.
        // class+slot skip the per-access field_slot hash find: the id->slot mapping is
        // append-only for a class object's lifetime (ensure_field_slots only emplaces),
        // so a matching class pointer makes the slot valid forever; a different class
        // (polymorphic site) or no slot (overflow field) takes the generic find.
        uint64_t this_field_serial = 0;
        const void* this_field_class = nullptr;
        uint64_t this_field_sym = 0;       // the symbol that armed the proof: cache roles are per ip, not per symbol
        uint32_t this_field_slot = 0xFFFFFFFFu;
        // Store twin (bare method-field WRITES): the same serial + class proof names the
        // cell; the arm banks what the store landing needs on top - no C++ property
        // setter for the id, the declared type (null = auto: infer from the cell's tag) -
        // under the class's method_epoch (declared types are replaced wholesale on reload)
        uint8_t this_store_state = 0;   // 0 unarmed, 1 armed
        uint64_t this_store_epoch = 0;
        type_info* this_store_type = nullptr;
        // Member-miss proof (a bare name in a method that no member answers, a global read): the receiver's script class and the
        // declaring class, each under its method_epoch (every field, method and static change bumps it, derived classes too),
        // when the full member probe missed for miss_sym. While both hold and the instance grew no runtime field, the probe
        // provably misses again and resolution goes straight to the environment.
        const void* miss_dynamic = nullptr;
        const void* miss_static = nullptr;
        uint64_t miss_sym = 0;
        uint64_t miss_dynamic_epoch = 0;
        uint64_t miss_static_epoch = 0;
    };

    // op_compound_fused: `target op= <rhs>` in one dispatch. Binary mode: rhs_proto indexes
    // fused_binary_protos (identifier operands in any mix — rhs cache roles 0/1 plus target
    // role 2 stay within the per-ip budget). Bare mode (rhs_proto == k_invalid_u32): rhs is
    // a single identifier or constant operand (`sum += i` / `sum += 1`).
    struct compound_fused_proto {
        uint32_t rhs_proto = 0;         // fused_binary_protos index; k_invalid_u32 = bare rhs
        fused_operand rhs;              // bare-mode operand (identifier or constant)
        uint32_t symbol = 0;            // chunk::symbols index (store target)
        uint32_t slot = k_invalid_u32;  // target frame slot (k_invalid_u32 = env)
        uint32_t kind_flags = 0;        // compound kind | compound_flag_*
        uint8_t typed_class = 0;        // the checker's class of target and value (assignment_expr::static_operand_class): the typed row
    };

    // op_concat_operands: a string chain's run of plain operands (constants, identifiers) joined onto the accumulator on the stack, each
    // read at its own step as op_concat_append would read and join it; `hint` sizes the accumulator on the chain's first append (0 = none)
    struct concat_proto {
        std::vector<fused_operand> pieces;
        uint32_t hint = 0;
        bool joins_text = false;   // a template string's run: a scalar piece becomes text (vm_backend::join_text)
    };

    // op_text_append: the target (op_store's or op_compound_store's symbol and frame slot) and the chain's pieces in order, each a constant,
    // a name read at its step or a compiler temp an earlier instruction of the statement landed (reads_ip: the first such instruction, the
    // statement's own ip when none). assign: `s = s + ...`, whose snapshot is the target itself, else `s += ...`, whose first piece heads
    struct text_append_proto {
        std::vector<fused_operand> pieces;
        uint32_t symbol = k_invalid_u32;
        uint32_t slot = k_invalid_u32;
        uint32_t store_flags = 0;   // assign: op_store's flags for the general tail
        uint32_t reads_ip = k_invalid_u32;
        uint32_t release = 0;   // bit k: piece k is a temp the op gives up once it is done (vm_compiler::consumer_releases)
        bool assign = false;
    };

    // op_binary_fused_decl / op_binary_fused_store: a fused binary whose result lands in
    // a compile-time-known sink with no push and no second dispatch (dest-addressed).
    // decl mode uses node_index (the variable_decl); store mode uses
    // symbol/slot/store flags exactly like op_store's instruction fields.
    struct fused_binary_dst_proto {
        uint32_t binary_proto = 0;      // fused_binary_protos index (the computation)
        uint32_t node_index = k_invalid_u32;  // decl mode: chunk::nodes index of the variable_decl
        uint32_t symbol = 0;            // store mode: chunk::symbols index
        uint32_t slot = k_invalid_u32;  // store mode: target frame slot (k_invalid_u32 = env)
        uint32_t flags = 0;             // store mode: store_flag_*; decl mode: b/c of op_decl_var
    };

    // op_index_fused / op_index_store_fused: subscript read/store whose container and
    // index resolve as fused operands (identifier slots/env or literals) instead of two
    // LOAD dispatches + pushes. The in-bounds array+int shape runs inline; every other
    // shape (map/OOB/borrow/custom-[]) pushes the resolved operands and replays the
    // verbatim unfused op, so semantics and error text stay byte-identical.
    struct fused_index_proto {
        fused_operand container;
        fused_operand index;
        // Computed flat-binary index (plain-ident containers only): fused_binary_protos
        // entry evaluated in-op through binary_fused_compute (the ONE fused-binary body).
        // Cache roles at these sites: binary left/right own ip*3/ip*3+1, container parks
        // at ip*3+2 (entries are provenance-checked by {env, epoch} but not symbol).
        uint32_t index_binary = k_invalid_u32;
        // Left-spine arithmetic chain continuing index_binary: ((step0) op1 r1) op2 r2 …
        // fused_binary_protos entries whose LEFT is EMPTY (the accumulator stands in;
        // no resolver may touch it) — only op + right are read. Arithmetic ops only,
        // rights ident/literal. All-int chains fold to a raw index in-op; any other
        // shape replays the verbatim unfused stack sequence from the accumulator.
        std::vector<uint32_t> index_chain_ext;
        // Store-value operand (op_index_store_fused, STATEMENT position only — the
        // instruction's c=1 elides the result push): ident/literal RHS resolves in-op
        // instead of LOAD+push. Absent (all-invalid) = the value rides the stack.
        fused_operand store_value;
        // op_index_fused_decl lane facts, proven at the peephole (decl_fast_slot_store,
        // not ref-escaping, slot-resident): the read lands DIRECTLY in decl_slot when
        // the runtime value's raw index matches decl_expect (0xFF = any scalar) —
        // no per-exec decl checks, no landing round-trip. k_invalid_u32 = general
        // landing. decl_type = the decl's type tag to stamp (raw ptr; the chunk pins
        // the decl node which pins its type), null = no stamp.
        uint32_t decl_slot = k_invalid_u32;
        uint8_t decl_expect = 0xFF;
        type_info* decl_type = nullptr;
        // decl_slot is a compiler temp (no declaration node): the read lands there for the consuming
        // op (a binary operand, a store value, an outer index); a non-scalar element lands boxed
        bool decl_temp = false;
        // Typed element lanes: the compiler proved the shape -
        // a plain identifier container, an index that is a direct slot, a literal or an
        // int/int-stamped fold whose chain steps are int/int with direct or literal rights.
        // A decl proto with the lane facts above opens exec_index_fused_decl with
        // typed_decl_read; a store proto opens exec_index_store_fused with typed_index_store;
        // any runtime miss runs the general kernel unchanged.
        bool typed_lane = false;
        bool speculative = false;   // the lane came from elem_read_operand, not a proof (census: misses per site)
        // a reference-slot container guarded ahead of the index (op_operand_temp c = 1): the temp that holds its
        // materialized value when the slot's reference is not a plain cell; the readers take the slot otherwise
        uint32_t container_guard = k_invalid_u32;
        uint8_t release = 0;   // the compiler temps the read or store gives up once it is done: 1 the guard, 2 the index, 4 the stored value
        // the guard's always form (op_operand_temp c = 2): the container's value is taken ahead of an index that may run user
        // code (a custom numeric operator, a getter, an operator[]), whatever the slot holds; the readers take the temp every time
        bool guard_always = false;
        uint8_t receiver = 0;   // the subscript's container the checker proved (binary_expr::subscript_receiver): 1 string, 2 array, 3 map, 0 unproven
        // op_index_store_fused: the store target's chunk::string_target_sites entry (k_invalid_u32 = none)
        uint32_t string_target = k_invalid_u32;
        bool compound = false;        // op_index_compound_operand: a read-modify-write of the element by compound_kind (opcode.hpp compound_*; compound_plus is 0)
        uint32_t compound_kind = 0;
    };

    // A subscript store target (op_index_store c, op_index_compound_fused c, op_index_store_fused's
    // proto): a string base mints a STRING-ELEMENT reference pinned through the base
    // (detail/string_element.hpp) - an identifier base through its slot and symbol (the
    // string receiver ladder), a member base through the ref-lvalue kernel on the node
    struct string_target_site {
        uint32_t node = k_invalid_u32;          // chunk::nodes index of the subscript binary_expr
        uint32_t base_slot = k_invalid_u32;     // identifier base: frame slot (k_invalid_u32 = env)
        uint32_t base_symbol = k_invalid_u32;   // identifier base: chunk::symbols index
    };

    struct chunk {
        std::vector<vm_instruction> code;
        // Lazily sized to 3*code.size(): role slots per ip (0=left/load, 1=fused right,
        // 2=store target) so fused superinstructions never collide
        mutable std::vector<env_lookup_cache_entry> env_lookup_cache;
        // Per instruction: the statement node in effect (raw pointer into the pinned tree)
        std::vector<const ast_node*> stmt_nodes;
        // The statements the debugger stops at, each with its instruction range [start, end): a statement's range holds its
        // children's. The compiler records them; the first armed dispatch sorts them, links parents and fills debug_innermost.
        struct debug_span { uint32_t start; uint32_t end; const ast_node* node; uint32_t parent; };
        mutable std::vector<debug_span> debug_spans;
        mutable std::vector<uint32_t> debug_innermost;   // per instruction: 1 + the innermost span holding it, 0 for none
        // The debugger's locals, outermost declaration first: the name `symbol` lives in `slot` while the frame's ip is in [first_ip, end_ip)
        struct debug_local { uint32_t slot; uint32_t first_ip; uint32_t end_ip; uint64_t symbol; };
        std::vector<debug_local> debug_locals;

        std::vector<script_value> constants;               // engine-less AST-literal templates
        // regex(pattern[, flags]) sites with literal texts (op_regex_constant): the pattern the checker compiled, and how many
        // texts the call passes should `regex` bind something else
        struct regex_constant { std::shared_ptr<const detail::regex::compiled> re; uint32_t argc = 1; };
        std::vector<regex_constant> regex_constants;
        // Instruction ranges [begin, end) evaluating call arguments; consulted only on the
        // C++-exception path (interpreter parity: visit_call converts arg-eval throws into
        // script unwinding at the frame making the call, so its own try/catch can fire)
        std::vector<std::pair<uint32_t, uint32_t>> call_arg_zones;
        std::vector<uint32_t> reference_slots;   // slots declared as references (a by-reference parameter, a local bound to something other than a value this unit declares): their load dereferences, possibly through a getter
        std::vector<uint64_t> symbols;
        std::vector<std::string_view> messages;            // kept by the symbolizer: a checked_result raised from them outlives the chunk
        std::vector<std::shared_ptr<ast_node>> nodes;      // pins referenced AST nodes alive
        std::vector<std::pair<uint64_t, const expression*>> field_inits;   // a field-init store's field and initializer (pinned in nodes)
        // A constructor's prologue steps (vm_compiler::constructor_prologue): what each op_ctor_* constructs. The chunk is rebuilt
        // when a class is (re)defined, so the classes and declarations it names hold while it runs
        struct ctor_step {
            const class_definition* level = nullptr;    // the constructor's class
            const class_definition* target = nullptr;   // the base (op_ctor_base), the level (op_ctor_delegate, op_ctor_literals)
            uint32_t index = 0;                         // the base's place among the level's bases
            bool named = false;                         // an initializer names it
            bool super_without_base = false;            // super(...) in a class with no base
            std::vector<std::shared_ptr<function_decl>> targets;   // op_ctor_delegate: the level's other constructors
            std::shared_ptr<class_definition> level_ref;            // op_ctor_delegate: the level, for its frame
        };
        std::vector<ctor_step> ctor_steps;
        bool level_initializers = false;   // vm_compiler::compile_level_initializers' body: a throwing field store is rethrown natively
        std::vector<std::unique_ptr<type_info>> owned_types;   // the inliner's spill temps' primitive types (the compiler has no engine to intern one)
        // Per ip (lazily sized, never longer than code): nodes index of the env-resolved
        // identifier_expr behind an op_load / op_store / op_binary_fused_store /
        // op_return_ident, read live for field proofs; k_invalid_u32 = none
        std::vector<uint32_t> ident_nodes;
        // A method body compiled against its declaring class: the bare names that class resolves to plain fields (no local, no
        // method, getter or user accessor of the name, not a C++ property) carry the field's slot + 1 per instruction (loads and
        // stores) and per operand (fused_operand::this_slot). A frame whose receiver's class lays those fields out at the same
        // slots (member_class itself, or a class whose verdict row says so) reads them straight from its receiver
        const class_definition* member_class = nullptr;
        std::vector<uint32_t> this_field_slots;                  // per instruction: slot + 1, 0 = none
        std::vector<std::pair<uint64_t, uint32_t>> this_field_uses;   // (field id, slot) the body addresses
        std::vector<uint32_t> this_global_ips;                   // per instruction: the LOAD's symbols index + 1 for a bare name the body's class resolves past its members (an environment read once the frame's layout holds)
        std::vector<uint64_t> this_global_uses;                  // those names: the receiver's class takes none of them as a member
        struct member_layout_row { const class_definition* cd = nullptr; uint64_t gen = 0; bool same = false; };   // gen: engine::layout_generation
        static constexpr uint8_t k_member_layout_rows = 4;
        mutable member_layout_row member_layout_rows[k_member_layout_rows];
        mutable uint8_t member_layout_next = 0;
        // per frame slot of a slot-resident `T& name = e;` declaration with a concrete T: T and its kind byte
        // (detail::declared_ref_kind); a store through the name decides by them, nothing is stamped at the binding
        std::vector<type_info*> slot_declared_refs;
        std::vector<uint8_t> slot_declared_kinds;
        // a switch's dispatch (op_switch_table): the subject, each case's key (an int constant or a program-scope const name) and body,
        // the no-match target (the default body or the end); the dense table is built at run time from the keys and rebuilt when a
        // program-scope const or the names' environment changes
        struct switch_table {
            fused_operand subject;
            std::vector<fused_operand> keys;
            std::vector<uint32_t> targets;
            uint32_t no_match = 0;
            bool constant_keys = false;   // every key an int constant: the table, once built, never goes stale
            mutable bool built = false, usable = false;
            mutable const void* env = nullptr;
            mutable uint64_t env_serial = 0, const_epoch = 0, definition_epoch = 0;
            mutable int64_t low = 0;
            mutable std::vector<uint32_t> dense;   // subject - low -> the case index, k_invalid_u32 = no case
        };
        std::vector<switch_table> switch_tables;
        std::vector<call_site> call_sites;
        // op_array / op_map with c != 0: the bits from word c - 1 mark the operands copied when evaluated (copy_ahead_of),
        // which the op takes as they are
        std::vector<uint64_t> literal_taken;
        std::vector<function_proto> function_protos;
        std::vector<closure_proto> closure_protos;
        std::vector<destructure_proto> destructure_protos;
        std::vector<iter_proto> iter_protos;
        // Closed typed loops as cell blocks: the loop's scalar slots as raw cells. The copy at [copy_head,
        // copy_back] is the cell form; the original instructions at [head, back] are the general form
        // a miss returns to (ip mapped by offset). Each cell records its slot, its expected kind, whether
        // the loop reads it before writing it (live-in: unboxed at entry), whether the body defines it
        // (flushed on exit or miss) and the landing that defines it (a decl carries its stamp).
        // a set of cells, up to 256: the dirty cells of a running block, the compiler values, the environment values,
        // the values live at a copy instruction
        struct cell_mask {
            static constexpr uint32_t words = 4;
            uint64_t w[words] = {0, 0, 0, 0};
            void set(uint32_t k) noexcept { w[k >> 6] |= uint64_t(1) << (k & 63); }
            void clear(uint32_t k) noexcept { w[k >> 6] &= ~(uint64_t(1) << (k & 63)); }
            bool test(uint32_t k) const noexcept { return ((w[k >> 6] >> (k & 63)) & 1) != 0; }
            bool any() const noexcept { return (w[0] | w[1] | w[2] | w[3]) != 0; }
            void reset() noexcept { for (uint32_t i = 0; i < words; ++i) { w[i] = 0; } }
            cell_mask operator~() const noexcept { cell_mask r; for (uint32_t i = 0; i < words; ++i) { r.w[i] = ~w[i]; } return r; }
            cell_mask operator&(const cell_mask& o) const noexcept { cell_mask r; for (uint32_t i = 0; i < words; ++i) { r.w[i] = w[i] & o.w[i]; } return r; }
            cell_mask operator|(const cell_mask& o) const noexcept { cell_mask r; for (uint32_t i = 0; i < words; ++i) { r.w[i] = w[i] | o.w[i]; } return r; }
            cell_mask& operator|=(const cell_mask& o) noexcept { for (uint32_t i = 0; i < words; ++i) { w[i] |= o.w[i]; } return *this; }
            cell_mask& operator&=(const cell_mask& o) noexcept { for (uint32_t i = 0; i < words; ++i) { w[i] &= o.w[i]; } return *this; }
            static cell_mask all() noexcept { cell_mask r; for (uint32_t i = 0; i < words; ++i) { r.w[i] = ~uint64_t(0); } return r; }
        };
        static constexpr uint32_t k_cell_limit = 64 * cell_mask::words;
        struct cell_loop {
            // env_operand: the environment scalar this cell holds (index into env_operands; k_invalid_u32 = a frame
            // slot); such a cell has a synthetic slot above the frame's, is resolved once at entry and never written
            // temp: a compiler temp (written before read inside its statement, no declared type): no entry guard, no unbox,
            // any kind lands
            // first_decl: the loop's first write to the cell is its declaration (a local the body declares: it enters empty and its
            // flush stamps with decl_stamp, the declaration's type, whatever later stores land)
            // string_borrow: a temp that may hold a borrowed string node, because its only consumer replays from the producer on a miss
            // (the append pair); any other temp refuses a string element, since a miss at its consumer reads the temp's slot
            struct cell { uint32_t slot; uint8_t kind; uint8_t landing; bool live_in; bool defined; type_info* stamp; uint32_t env_operand = k_invalid_u32; bool temp = false; bool first_decl = false; type_info* decl_stamp = nullptr; bool string_borrow = false; bool iter_index = false; bool open_def = false; };   // iter_index: a range-for block's index, the iteration state's (never a slot's); open_def: a compiler value with a definition of no declared kind (a kind a typed read fixed converts none of its landings)
            uint32_t head, back, copy_head, copy_back, exit_ip;
            // a straight-line region (no back edge): entered at head by op_cells_enter, left at back by the copy's last exit; the trampoline
            // holds the instruction the entry replaced and a jump to head + 1, where a refusal or a miss at head resumes the general form (a
            // compare-headed loop's head is such an entry too, beside its back edge)
            bool straight = false;
            uint32_t trampoline = k_invalid_u32;
            // counted: the counted_for_protos index of a counted loop (its back edge is the entry and its cell back edge
            // steps and compares; the copy's head slot is a placeholder); k_invalid_u32 = a compare-headed loop
            uint32_t counted = k_invalid_u32;
            // an element alias declared in the body (`var& strip = STRIPS[sidx]`): the copy instruction that rebinds it, its cell (a compiler value of
            // kind reference: the element index is its payload, the source table its own site), the site index of its source table and the site
            // indices of the loads and stores through the alias, whose containers the rebind sets and the entry never resolves
            struct rebind { uint32_t copy_ip = 0, cell = 0, source_site = k_invalid_u32; std::vector<uint32_t> dependents; bool iter = false; uint32_t ref_decl = k_invalid_u32; };   // iter: a range-for's by-reference loop variable (the reference carries the range-for's element constraint); ref_decl: a class reference's declaration (its admission runs at each new binding, its constraint rides the flushed reference)
            std::vector<rebind> rebinds;
            // a C_TEXT_APPEND: the target's cell, its pieces' cells or k_typed_const_bit | constant, the general proto (the charge rule)
            struct text_append { uint32_t target = 0; uint32_t proto = 0; std::vector<uint32_t> pieces; };
            std::vector<text_append> text_appends;
            std::vector<uint8_t> site_alias;   // per site: 1 = a site through an alias (its container the rebind's element; skipped by the entry and the hold check)
            std::vector<uint32_t> switch_targets;   // per C_SWITCH: its cases' copy ips, then its no-match copy ip
            std::vector<uint32_t> land_sites;   // the sites a landing after a call checks again (no alias site, no range head), k_land_member_site marking a member site
            static constexpr uint32_t k_land_member_site = 0x80000000u;
            std::vector<uint32_t> inline_sites;   // the inlined leaves inside the region (chunk::inline_sites indexes): their guards hold at the entry and after every effect point
            struct math_alias_site { uint32_t site; uint8_t fn; uint32_t cache_slot; };   // a stdlib alias the block evaluates as its kernel: the name binds the builtin at the entry and after every effect point
            std::vector<math_alias_site> math_alias_sites;
            // a range-for head (`for (auto& p : pool)`): the iter_protos index (the loop variable's slot and binding), the index cell (the
            // iteration state's index at entry, stepped by the back edge, synced on a miss and at the exit) and the head's site (the state's container)
            uint32_t iter_proto = k_invalid_u32, iter_index_cell = k_invalid_u32, iter_site = k_invalid_u32;
            std::vector<uint32_t> inner_counted;   // the counted_for_protos indices of the inner counted loops the block took, outermost first: a miss inside one pushes the general form's state (cells_miss)
            std::vector<cell> cells;
            std::vector<uint32_t> env_cells;           // the cells with an environment operand: a landing after a call re-reads them
            std::vector<uint32_t> entry_cells;         // the cells the entry reads (environment values, the range index, live slots); temps and body declarations enter empty
            std::vector<uint32_t> site_env_cache;      // per site: the lookup-cache slot of a load whose container is a plain environment name (k_invalid_u32: any other site)
            std::vector<uint32_t> string_candidates;   // the slot cells whose kind may be a string at run time: a landing re-borrows them
            // indexed by frame slot (and the env cells' synthetic slots): the slot's FIRST cell, k_invalid_u32 = not a cell. A
            // compiler temp slot may carry several cells, one per definition of another kind; the emission advances the
            // binding as it passes each definition and the copy names cells directly, so this table is emission-time only
            std::vector<uint32_t> slot_to_cell;
            std::vector<uint32_t> origin;         // per copy instruction (copy_head + k): the original instruction it stands for (the miss path's map)
            // per copy instruction: the temp cells whose definition is live there (defined earlier, read at or after it),
            // the only temps a miss at that instruction flushes; temp_mask = every temp cell
            std::vector<cell_mask> live_temps;
            cell_mask temp_mask;
            cell_mask env_mask;       // every environment cell: the ones a body writes are flushed ahead of a call and at every exit
            std::vector<uint32_t> sites;   // the load and store sites' copy ips: the entry resolves their containers, a landing after a call checks them
            // member_node: an enum's value read by name (`Ai.summon`): the environment value is the enum's map, the cell its entry for the member
            // an enum member (member_node): the enum's const map last seen and its entry, pinned (vm_backend::env_operand_value)
            struct env_operand_ref { fused_operand operand; uint32_t cache_slot; uint32_t member_node = 0xFFFFFFFFu; mutable strong_ptr<script_map> enum_map; mutable const script_value* enum_entry = nullptr; };
            std::vector<env_operand_ref> env_operands;
            // a call inside the block: the probe, the pushes, the call and the landing are four copy instructions over one
            // descriptor. An argument is a cell (boxed at the call; a slot's cell is flushed first so a by-reference parameter
            // binds the current value) or a push the block runs verbatim (a constant, an environment name, a slot the loop
            // holds no cell for). The consumer lands the result into dst by the declared-type rule (1 = a scalar declaration,
            // 2 = a no-result store, 3 = a compiler temp: op_call_temp, the expression around the call reads it as a slot) or drops it (0 = a statement).
            struct call_arg { uint8_t kind; uint32_t v; uint32_t ip; };   // kind 0 = a slot (a cell or verbatim, decided at emission), 1 = cell v, 2 = the push at ip verbatim
            // raw: the checker resolved the callee to a free script function with scalar by-value parameters and a scalar result, and every
            // argument is a cell or a constant; at run time the arguments instruction enters the callee's frame program raw when the parked
            // callee has one whose parameter kinds are the arguments' kinds
            // raw: the arguments instruction may enter the callee's frame program raw; raw_kinds: the callee's parameter kinds as the checked
            // declaration names them (four bits per parameter, parameter i at bits 4i); raw_proven: every argument is a pinned cell or a constant
            // of exactly that kind, so a raw entry checks no argument
            struct cell_call {
                uint32_t probe_ip, call_ip, consumer_ip; uint8_t consumer = 0; uint32_t dst = k_invalid_u32; std::vector<call_arg> args; uint8_t raw = 0;
                uint8_t raw_proven = 0; uint32_t raw_kinds = 0;
                // what the landing re-checks (narrowed): the land_sites entries and the inlined leaves the block can reach from it before
                // another landing, which checks the rest; not narrowed: every one
                bool narrowed = false;
                std::vector<uint32_t> land_sites, inline_sites;
            };
            std::vector<cell_call> calls;
            // a frame program: the region covers the whole function body from its first instruction to its final HALT, its returns are ret
            // operations (C_RET), and raw_enterable says a raw call may enter it at copy_head with the parameters written into param_cells
            // (one per parameter, by slot): its entry reads nothing but the parameters (no environment value, no site, no guard of a graft)
            bool frame_program = false;
            bool raw_enterable = false;
            bool writes_stamps = false;   // a frame program that can stamp a cell (an auto declaration from a name, a boundary's re-read): a raw entry clears the stamps
            bool raw_sites = false;   // a call of the block may be raw (plans is sized): only such a block advances the raw epoch at its entry and its doors
            // a boundary operation: a statement the translation does not take runs on the
            // general form while the block stays parked; start is its first general ip, end the general ip it ends at (the resume point: a
            // CELLS_RESUME there, or the loop's back edge), resume the copy ip the block continues from, define the cell of the local the
            // statement declares (read back from its slot at the resume, whatever the block held before)
            struct boundary { uint32_t start = 0, end = 0, resume = 0, define = 0xFFFFFFFFu; };
            std::vector<boundary> boundaries;
            std::vector<uint32_t> param_cells;
            std::string ir_text;   // the region's canonical operations (index:kind, ->target, w slot, r slots), filled only when the compiler lists the IR
            uint32_t param_kinds = 0;   // the parameter cells' kinds, four bits per parameter (the call site's raw_kinds form)
            // a raw site's plan, the run time's (one per call when any call is raw): the epoch in which the probe last validated the callee
            // (vm_backend::raw_epoch_), its body's chunk, that body's frame program, the function the probe found and how a raw return lands
            // (vm_backend::call_record::raw_land)
            struct call_plan { uint64_t epoch = 0; chunk* body = nullptr; const cell_loop* program = nullptr; const script_defined_function* fn = nullptr; uint8_t land = 0; };
            mutable std::vector<call_plan> plans;
        };
        std::vector<cell_loop> cell_loops;
        uint32_t max_cells = 0;   // the most cells any loop of this chunk holds: a frame's dirty bytes past it are never read
        // a loop the translation refused: its span, the first offending instruction and the refusal class (the coverage
        // report and the disassembly); a `translation` refusal is a supported core operation in an encoding the
        // translation does not normalize yet, a coverage gap rather than a limit of the model
        struct cell_refusal { uint32_t head, back, ip; uint8_t reason; opcode op; };
        static constexpr uint8_t cell_refusal_none = 0, cell_refusal_head = 1, cell_refusal_representation = 2, cell_refusal_effect = 3,
                                 cell_refusal_translation = 4, cell_refusal_malformed = 5, cell_refusal_no_cells = 6, cell_refusal_too_many_cells = 7,
                                 cell_refusal_bail_dominates = 8;
        static const char* cell_refusal_name(uint8_t r) {
            switch (r) {
            case cell_refusal_head: return "head";
            case cell_refusal_representation: return "representation";
            case cell_refusal_effect: return "effect";
            case cell_refusal_translation: return "translation";
            case cell_refusal_malformed: return "malformed";
            case cell_refusal_no_cells: return "no cells";
            case cell_refusal_too_many_cells: return "too many cells";
            case cell_refusal_bail_dominates: return "bail dominates the back edge";
            default: return "none";
            }
        }
        // the class of an instruction the translation has no operation for: a supported core operation in another
        // encoding is a translation gap; a call, an exit, a scope or a control effect is an effect; members, objects and
        // allocations are representations the model does not take
        static uint8_t cell_refusal_of(opcode op) {
            if (op >= opcode::op_t_add_ii && op <= opcode::op_t_load) return cell_refusal_translation;   // the typed scalar set (opcode.hpp)
            switch (op) {
            case opcode::op_const: case opcode::op_null: case opcode::op_true: case opcode::op_false: case opcode::op_pop: case opcode::op_dup:            case opcode::op_to_bool: case opcode::op_load: case opcode::op_store: case opcode::op_compound_store: case opcode::op_incdec:
            case opcode::op_decl_var: case opcode::op_binary: case opcode::op_binary_fused: case opcode::op_index:
            case opcode::op_unary: case opcode::op_jump_if_false: case opcode::op_jump_if_true:
            case opcode::op_compound_fused: case opcode::op_math: case opcode::op_index_store: case opcode::op_index_compound_fused:
            case opcode::op_index_fused: case opcode::op_operand_temp: case opcode::op_call_temp: case opcode::op_member_temp: case opcode::op_set_member_fused: case opcode::op_math_temp: case opcode::op_push_fused:
            case opcode::op_cfor_prep: case opcode::op_cfor_back: case opcode::op_cfor_pop: case opcode::op_loop_back: case opcode::op_cells_back:
            case opcode::op_cells_cfor_back: case opcode::op_fused_cmp_jump: case opcode::op_cast: case opcode::op_index_store_fused: case opcode::op_index_compound_operand:
                return cell_refusal_translation;
            case opcode::op_get_member: case opcode::op_get_member_path: case opcode::op_set_member: case opcode::op_member_compound: case opcode::op_get_static: case opcode::op_set_static:
            case opcode::op_this: case opcode::op_super: case opcode::op_from_this: case opcode::op_null_guard: case opcode::op_probe_member:
            case opcode::op_array: case opcode::op_map: case opcode::op_new: case opcode::op_decl_ref_ident: case opcode::op_decl_ref_value: case opcode::op_destructure:
                return cell_refusal_representation;
            default:
                return cell_refusal_effect;
            }
        }
        std::vector<cell_refusal> cell_refusals;
        std::vector<std::pair<uint32_t, uint32_t>> logical_values;   // a && or || whose value is used: its first ip and the ip of the instruction consuming the value
        std::vector<fused_binary_proto> fused_binary_protos;
        std::vector<compound_fused_proto> compound_fused_protos;
        std::vector<concat_proto> concat_protos;
        std::vector<text_append_proto> text_append_protos;
        std::vector<fused_binary_dst_proto> fused_binary_dst_protos;
        std::vector<fused_index_proto> fused_index_protos;
        std::vector<string_target_site> string_target_sites;
        std::vector<push_proto> push_protos;
        std::vector<member_store_proto> member_store_protos;
        std::vector<velem_proto> velem_protos;
        std::vector<member_path_proto> member_path_protos;
        // an inlined leaf (Inlined leaves): the guard at guard_ip admits [begin_ip, fallback_ip) when the callee
        // name binds the function whose body is `body`; otherwise the fallback [fallback_ip, join_ip) runs the original statement
        struct inline_site {
            fused_operand callee;                 // the callee identifier, resolved as the probe resolves it
            uint32_t name_symbol = k_invalid_u32;
            const block_stmt* body = nullptr;
            std::shared_ptr<const function_decl> decl;   // the callee, pinned: the trace names it, and body stays a live identity for the guard (no address reuse)
            const ast_node* call_stmt = nullptr;   // the caller's statement: the trace's line for the caller's frame
            uint32_t guard_ip = 0, begin_ip = 0, fallback_ip = 0, join_ip = 0;
            uint32_t result_slot = k_invalid_u32;  // the result temp (every return defines it, the join reads it); k_invalid_u32: a void callee
            bool method = false;                  // a method graft: the receiver slot's instance resolves member_id to the body
            uint32_t receiver_slot = k_invalid_u32;   // k_invalid_u32 on a method graft: the receiver is the frame's `this`, or its this-field
            uint64_t member_id = 0;
            const class_definition* field_class = nullptr;   // the class of the bare fields the body names (the receiver's exact class); null: none
            std::vector<uint64_t> field_ids;
            bool env_reads = false;                // a free function's body reads an environment name: the guard holds only where the caller has no member scope (a field would shadow it)
            bool plain_numeric = false;            // a renamed reference parameter's stores were proven by the kinds of plain arithmetic: the guard holds only while no numeric operator is registered
            // the guard's verdict for the receiver classes it met, each at its method epoch (every fact the verdict reads moves that epoch)
            struct holds_row { const class_definition* cd = nullptr; uint64_t epoch = 0; bool holds = false; };
            static constexpr uint8_t k_holds_rows = 4;
            mutable holds_row holds_rows[k_holds_rows];
            mutable uint8_t holds_next = 0;
            mutable strong_ptr<script_function_node> holds_fn;   // the free function the guard last validated: the same object holds again
            // the program-scope names the call's argument proofs read (call_expr::proof_globals: symbol id, kind): the guard holds only
            // while each still holds its kind, re-checked when a run moved engine::definition_epoch past globals_epoch
            std::vector<proof_global> proof_globals;
            // the program-scope functions the parameter forwarding read (symbol id, body): a by-value parameter renamed to the caller's
            // variable because each callee it is passed to takes it by value; re-checked with the proof globals
            struct forward_callee {
                uint64_t symbol = 0;
                const statement* body = nullptr;
                std::weak_ptr<const block_stmt> owner;   // body names that function's body only while this holds
                bool operator==(const forward_callee& o) const noexcept { return symbol == o.symbol && body == o.body; }
            };
            std::vector<forward_callee> forward_callees;
            bool epoch_checked = false;   // proof_globals or forward_callees non-empty: the guard asks inline_globals_hold
            mutable uint64_t globals_epoch = 0;
            bool calls = false;           // the body makes calls: op_graft_depth counts the grafted frame while they run
            uint32_t receiver_this_field = 0;   // the frame's this-field holding a method graft's receiver: its slot + 1 (0: none)
            bool caller_globals = false;        // the body's program-scope names read in the caller's frame: its names resolve as the program's
            std::vector<uint64_t> global_ids;   // a method graft's program-scope names: the receiver's class takes no member of any
            std::vector<uint64_t> sibling_ids;  // a method graft's sibling methods the clone calls on the receiver: each a method of the receiver's class
        };
        std::vector<inline_site> inline_sites;
        bool graft_calls = false;   // an inline site's body makes calls (the trace names its frame above them)
        struct inline_refusal { std::string callee; const char* reason = nullptr; uint32_t line = 0; };   // a call the inliner kept and the rule that kept it
        std::vector<inline_refusal> inline_refusals;   // the listing and the coverage report name them; no instruction reads the table
        // Typed scalar ops (opcode.hpp op_t_*): the instruction each one was lowered from
        // (two for a move: the LOAD or CONST and the DECL_VAR or STORE it replaced) and the
        // decl's type stamp. Indexed by ip; rows for other instructions are empty.
        static constexpr uint32_t k_iter_container = 0xFFFFFFFEu;   // a site row's cont_slot: the container is the iteration state's (a range-for block's head and its landing)
        struct typed_site {
            vm_instruction orig{opcode::op_halt, 0, 0, 0};
            vm_instruction orig2{opcode::op_halt, 0, 0, 0};
            type_info* stamp = nullptr;
            // op_t_load: the fused index proto flattened into one row. cont_slot = the
            // container's frame slot, k_invalid_u32 = env-resident (resolved through the
            // original proto's operand); index = operands[0] when fold_ops == 0, else
            // operands[0] op[0] operands[1], then op[k] operands[k + 1] for each chain step;
            // an operand is a slot or a constant index behind k_typed_const_bit
            uint32_t cont_slot = k_invalid_u32;
            static constexpr size_t k_max_chain_ext = 4;   // fused_index_proto::index_chain_ext steps a row holds
            uint32_t operands[k_max_chain_ext + 2] = {};
            uint8_t fold_ops[k_max_chain_ext + 1] = {};
            uint8_t fold_count = 0;
            uint8_t decl_expect = 0xFF;
            // an op_operand_temp origin (materialize_typed_reads): the read resolves through this synthetic
            // proto and lands in a compiler temp with the canonical stamp (in place when the temp holds it)
            uint32_t index_proto = k_invalid_u32;
            bool temp = false;
            bool speculative = false;
            uint32_t guard_temp = k_invalid_u32;   // fused_index_proto::container_guard, flattened
            bool guard_always = false;             // fused_index_proto::guard_always, flattened
            bool release = false;                  // op_t_decl_mov / op_t_store_mov: the source is a compiler temp the move gives up
            // a block's copy of a read: the general instruction's own ip, whose environment cache slots the entry resolves the container
            // through (the copy's miss ip may be another instruction's: an argument's read misses onto the first argument instruction)
            uint32_t orig_ip = k_invalid_u32;
        };
        std::vector<typed_site> typed_sites;
        // the environment operands of typed ops (k_typed_env_bit): resolved as the replaced instruction resolved them, at its cache slots
        std::vector<fused_operand> typed_env_operands;
        std::vector<counted_for_proto> counted_for_protos;

        // Default-argument expressions compiled per parameter (function-body chunks only)
        std::vector<std::shared_ptr<chunk>> param_default_chunks;

        // a function body's chunk keeps its body alive: a holder's body-keyed check (script_method_dispatch::body_cache_key) never meets a
        // recycled address
        std::shared_ptr<const block_stmt> owner_body;
        std::string function_name;                         // empty = top level
        size_t local_count = 0;
        size_t first_temp_slot = 0;   // the parser's locals end here; every slot from it on is the compiler's (temps, a graft's slots)
        bool is_function_body = false;
        // Compile-time lazy-env gate: false = every op proven not to need the per-call
        // scope environment, so plain in-loop callees skip creating it (fail-closed default)
        bool needs_frame_env = true;
        // A lambda captures one of the body's locals by reference (block_stmt::shares_cells): the frame's exit checks
        // the cells it shared for cycles. Implies needs_frame_env, so the check sits on the env frame's exit only
        bool shares_cells = false;
        // Compiler had the builtin registries: call_site bi_* indices are authoritative
        // (invalid = not a builtin). False (raw-compiler tests) keeps the find fallback.
        bool builtin_indexed = false;
        // GET_MEMBER field-read site cache (flat classes tier-1 stage B): per-ip
        // {receiver class, method_epoch, field slot}; slot UINT32_MAX = negative
        // (same_as / nonpublic chain / sp-builtin name / getter shadow / no slot —
        // the resolve ladder speaks). Lazily sized to code.size(); main-engine only
        // (same cached_global_env_ gate as the caches above).
        struct member_ic_entry {
            const class_definition* cd = nullptr;
            uint64_t epoch = 0;
            uint32_t slot = UINT32_MAX;
            type_info* scalar_type = nullptr;   // a store site's int, float or bool field: its declared type and storage kind
            uint8_t scalar_kind = 0;
            detail::host_member_hit host;       // slot UINT32_MAX on a bound C++ class: the member's in-place lane, when it has one
        };
        mutable std::vector<member_ic_entry> member_ic;
        // the field IC's second class per env cache slot (a base method running on several subclasses), main engine only; sized on the
        // first receiver of another class
        struct this_field_row { const class_definition* cd = nullptr; uint32_t slot = 0; };
        mutable std::vector<this_field_row> this_field_alt;
        // the member-miss proof's second receiver class per env cache slot (the name is no member of that class either, at its method
        // epoch), main engine only; sized when a site first meets a second class
        struct member_miss_row { const class_definition* dyn = nullptr; uint64_t epoch = 0; };
        mutable std::vector<member_miss_row> member_miss_alt;
        // A bare name in a method that reads a C++ member of the receiver, by env cache slot (ip * 3 + operand): the receiver
        // class, its method epoch and the member's lane. Sized on the first such read.
        struct bare_host_entry {
            const class_definition* cd = nullptr;
            uint64_t epoch = 0;
            detail::host_member_hit hit;
        };
        mutable std::vector<bare_host_entry> bare_host_ic;
        // Block-entry rows (main engine; a parallel worker keeps its own in its record of the chunk), one per cell loop: what the loop's last full entry resolved - each site's path (frame
        // slot or environment binding) and container, each environment cell's binding - under that environment and epoch, the
        // lookup cache's own rule. Any call's entry restores the row and the re-entry check proves it against the frame it runs in
        struct entry_memo_row {
            struct site { uint32_t slot; const script_value* env; const script_value* value; const script_array* node; const script_array* node2; const script_value* member; };
            struct env_cell { uint32_t cell; const script_value* binding; };
            const environment* env_of = nullptr;
            uint64_t env_serial = 0;
            std::vector<site> sites;
            std::vector<env_cell> env_cells;
        };
        mutable std::vector<entry_memo_row> entry_memos;
        // Compile-time sticky-method-scope gate: true = no op parks per-call state in the
        // scope env or captures its identity beyond the call (closures/refs/try/scopes/env
        // decls), so a method body may run in ONE persistent per-dispatch method env
        // (vm_backend::push_method_frame). Superset of the lazy list: member-access ops are
        // allowed here (they read this/access-context, both re-stamped per call).
        bool method_env_reusable = false;

        // Pins the whole program tree for top-level chunks
        std::vector<declaration_ptr> pinned_decls;

        uint32_t frame_program = k_invalid_u32;   // the cell_loops index of the body's frame program (k_invalid_u32: none)

        // a member store or compound whose member is a vector component name (x, y, z, w, width, height, depth) on an identifier
        // base: (member node, the identifier's frame slot operand; k_invalid_u32 = not a frame slot), so a component store resolves
        // the variable it names (detail/math_component_store.hpp). Last, so the hot members keep their offsets
        std::vector<std::pair<uint32_t, uint32_t>> component_base_slots;
        uint32_t component_base_slot(uint32_t node) const noexcept {
            for (const auto& [n, slot] : component_base_slots) { if (n == node) { return slot; } }
            return 0xFFFFFFFFu;
        }
#ifdef JAISCRIPT_VM_COVERAGE
        // the straight-line spans [head, end) the region builder dropped (vm/coverage.hpp): reason none = too little work (work counts
        // the span's operations, bounded those up to its last statement boundary, which the builder judges), else the translation's
        // refusal class at ip
        struct straight_note { uint32_t head = 0, end = 0, ip = 0; uint8_t reason = 0; uint32_t work = 0, bounded = 0; };
        std::vector<straight_note> straight_notes;
#endif
        // member temps and the consumer of their temps run as one instruction (op_mfold): each field read where it lives through its member
        // temp's member_ic row into its temp, then the consumer's own fast rows; anything else continues with the instructions as compiled
        struct member_fold {
            enum : uint8_t { typed_arith = 1, typed_cmp_jump, dyn_arith, dyn_cmp_jump, typed_decl_move, member_store };
            vm_instruction consumer;   // as compiled: its operands name the temps
            vm_instruction first;      // the first member temp, which a miss runs (a second one stays in the code after the fold)
            uint8_t kind = 0;
            uint8_t count = 0;         // the member temps (1 or 2)
        };
        std::vector<member_fold> member_folds;
        std::unordered_map<uint32_t, uint32_t> member_this_symbols;   // a this-field base's member node: its symbols index
    };

} // namespace jai::vm

#endif // __JAISCRIPT_VM_CHUNK_HPP__
