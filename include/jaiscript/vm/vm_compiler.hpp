#pragma once

#ifndef __JAISCRIPT_VM_VM_COMPILER_HPP__
#define __JAISCRIPT_VM_VM_COMPILER_HPP__

#include "chunk.hpp"
#include <jaiscript/detail/native_stack.hpp>
#include <functional>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai {
    struct builtin_method_registries;
    class engine;
}

namespace jai::detail { enum class math_fn : uint8_t; }

namespace jai::vm {
    struct land_ctx;

    class vm_compiler {
    public:
        explicit vm_compiler(string_symbolizer* symbolizer,
                             const builtin_method_registries* builtins = nullptr)
            : symbolizer_(symbolizer), builtins_(builtins) {}

        // Typed scalar lowering follows the checker's proofs: off when type proofs are off,
        // so proofs-off runs are also typed-ops-off runs (the fuzz gate's second pass)
        void typed_lowering(bool on) { typed_lowering_ = on && !reference_; }
        // the listing tool and the tests: each closed region records its canonical operations (chunk::cell_loop::ir_text)
        void ir_listing(bool on) { ir_listing_ = on; }
        // the middle end's switch (engine::ir_passes): off, no proof-based elision and no pass; the regions run their checked forms
        void ir_passes(bool on) { ir_passes_ = on && !reference_; }
        // engine::vm_reference_mode: every chunk compiles to the general forms (no superinstruction, typed or dynamic row, region,
        // graft, counted loop, return fusion, tail-call hint or builtin dispatch index); set once, before the first compile
        void reference(bool on) { reference_ = on; inlining_off_ = on; if (on) { typed_lowering_ = false; ir_passes_ = false; } }
        // the stdlib math aliases a bare call may lower to (engine::register_math_alias)
        void math_aliases(const engine* e) { engine_ = e; }
        // the declaring class of the next method body compile_callable compiles (null: not a method body)
        void member_class(const class_definition* cls) { member_class_ = cls; }
        // A constructor's prologue (C++ [class.base.init]), compiled ahead of its body in the constructor's own frame: a delegation's
        // target, else each base in declaration order, then the level's own field initializers; its initializer-list arguments read
        // the constructor's parameters by name (name_slots), its field initializers never do (C++ [class.mem])
        struct constructor_prologue {
            const class_definition* level = nullptr;
            const constructor_initializer* delegation = nullptr;
            std::vector<chunk::ctor_step> steps;                        // one per base in declaration order (or the delegation's level)
            std::vector<const constructor_initializer*> initializers;   // each step's initializer (null: default-constructed)
            std::vector<std::pair<uint64_t, expression_ptr>> fields;    // the level's own field initializers, evaluated
            bool literal_fields = false;                                // the level's chain is literal: op_ctor_literals stamps them instead
            std::unordered_map<uint64_t, uint32_t> name_slots;
            std::shared_ptr<ast_node> origin;                           // the constructor: where a step without arguments stands
            bool evaluates = false;                                     // detail::prologue_evaluates
        };
        // the prologue the next compile_callable compiles ahead of its body (null: none)
        void constructor_prologue_for(const constructor_prologue* prologue) { prologue_ = prologue; }

        // program_frame: the chunk runs in a program frame with a window (vm_backend::run_program), so its block-scoped declarations take slots
        std::shared_ptr<chunk> compile_program(const std::vector<declaration_ptr>& declarations, bool program_frame = false);
        std::shared_ptr<chunk> compile_callable(std::string_view name,
                                                const std::vector<parameter>& parameters,
                                                const std::shared_ptr<block_stmt>& body,
                                                size_t local_count);
        // Env-only lookups; safe to invoke mid-run (class decls, field initializers)
        std::shared_ptr<chunk> compile_standalone_expression(const expression_ptr& expr);
        std::shared_ptr<chunk> compile_standalone_declaration(const declaration_ptr& decl);
    private:
        const std::unordered_map<uint64_t, uint32_t>* name_slots_ = nullptr;   // a constructor prologue's parameter names
    public:
        // A class level's field initializers outside a constructor (an aggregate's, a reload's): one body over the initializer
        // environment, each value stored into this's field
        std::shared_ptr<chunk> compile_level_initializers(std::string_view name, const std::vector<std::pair<uint64_t, expression_ptr>>& initializers);

    private:
        enum class region_kind : uint8_t {
            loop,        // while / C-style for: break + continue target
            range_for,   // break + continue target; iteration state popped at the exit label
            switch_ctx,  // consumes break; continue passes through (the stack form pops its [value, ft] pair)
            try_block,   // pass-through: op_try_pop before jumps that exit it
            catch_block, // pass-through: op_catch_end before jumps that exit it
            graft,       // an inlined body: its returns jump to the graft's join (vm_inline.cpp)
        };

        struct loop_context {
            region_kind kind = region_kind::loop;
            std::vector<size_t> break_patches;
            std::vector<size_t> continue_patches;
            size_t continue_target = k_invalid_u32;   // known target (while/range-for); patches used otherwise
            bool continue_is_loop_back = false;       // while: continue re-checks budget via loop_back
            size_t scope_depth = 0;
            size_t kill_depth = 0;                     // kill scopes open outside the region: a jump out of it destroys the ones above
            const ast_node* stmt = nullptr;            // a loop's statement: its block verdict decides how a graft inside it lands (open_loops_)
            bool switch_stack_pair = false;            // a switch on the stack form: its [value, ft] pair sits on the value stack
            uint32_t switch_subject = k_invalid_u32;   // a switch's subject temp that may own an instance: a jump out of the switch kills it
        };

        struct callable_context {
            bool coroutine_body = false;   // the body yields: it runs on a fiber frame, which never runs a block, so a graft of a callee with a loop keeps the call (the callee's own frame closes it)
            // Symbols this callable itself declares (params + locals): identifiers outside
            // this set compile to env lookups, never to baked (possibly outer-frame) slots.
            std::unordered_set<uint64_t> declared;
            // reference declarations bound to a value the unit declares (a value slot, a program-level value
            // declaration, an alias already plain), slot -> symbol: their dereference is a cell, never a getter
            std::unordered_map<uint32_t, uint64_t> plain_refs;
            // slots a reference or a capture may reach (declarations and parameters the parser marked escaping, reference
            // parameters): user code run by a hoisted read may rebind them, so they never stay in place ahead of such a read
            std::unordered_set<uint32_t> escaping;
            // value slots declared int, float or bool: they hold that kind at every point, so a unary or an intrinsic over
            // one runs no user operator or conversion
            std::unordered_set<uint32_t> primitive_slots;
            // the declared type of each value slot as its declaration compiled (null: untyped): what a store of the variable enforces
            std::unordered_map<uint32_t, const type_info*> slot_types;
            std::unordered_set<uint32_t> parameter_slots;   // the callable's by-value parameters: a return of one copies when a user copy constructor would run
            // per frame slot: the scalar kind an untyped local certainly holds (vm_compiler::infer_local_kinds), a stamp's input
            std::vector<script_value_type> slot_kinds;
            bool active = false;
        };

        string_symbolizer* symbolizer_;
        const builtin_method_registries* builtins_ = nullptr;   // compile-time bi_* fill
        const engine* engine_ = nullptr;
        const class_definition* member_class_ = nullptr;
        const constructor_prologue* prologue_ = nullptr;
        void compile_constructor_prologue(const constructor_prologue& prologue);
        uint32_t this_field_slot_of(const identifier_expr* ident, bool note = true);   // note: list the name for the frame's layout verdict
        bool this_global_of(const identifier_expr* ident, bool note = true);
        bool handle_field_of_member_class(const identifier_expr* ident) const;   // a bare field the method's class declares a shared_ptr
        bool method_frame() const { return member_class_ && chunk_ && chunk_->member_class == member_class_ && !reference_; }   // the callable compiling is a method body of member_class_
        void note_this_field(size_t ip, const identifier_expr* ident);
        // Scope exits destroy the frame-slot locals that may own an instance (C++ lifetimes): one scope per block,
        // case body, for initializer and range-for iteration, its slots killed in reverse at its close and by the jumps that
        // leave it; a catch entry kills what its try block declared
        std::vector<std::vector<uint32_t>> kill_scopes_;
        std::vector<std::vector<uint32_t>> try_kills_;
        bool slot_kill_wanted(const type_info_ptr& t, size_t slot, uint64_t symbol) const;
        void note_kill_slot(uint32_t slot);
        void note_kill_decl(const statement* decl);
        void close_kill_scope();
        void emit_kills_to(size_t depth, size_t from = SIZE_MAX);
        void push_region(region_kind kind);
        bool typed_lowering_ = true;
        bool ir_listing_ = false;
        bool ir_passes_ = true;
        bool reference_ = false;
        static constexpr size_t k_region_min_work = 16;   // a straight-line region's least number of operations it runs in place of dispatches (its entry and flush cost about ten)
        // raw typed frames: each parameter's scalar kind when every parameter of the callable
        // being compiled is a by-value int, float or bool in its own slot, without a default (frame_eligible_ false: the body gets no frame program)
        std::vector<uint8_t> frame_param_kinds_;
        bool frame_eligible_ = false;
        static uint8_t raw_scalar_kind(const type_info* t);       // the cell kind of a declared int, float or bool; 0 otherwise
        static bool raw_signature(const function_decl* fn);       // a free function a raw call may enter: scalar by-value parameters, a scalar result
        bool raw_call_operand(const expression* e) const;         // a hoistable call whose checked callee has the raw signature and a frame program
        static bool raw_call_reaches_program(const function_decl* callee, const block_stmt* self_body, const detail::native_stack_mark& stack);   // the callee is the function itself, or forms a frame program
        static bool raw_call_reaches_program(const function_decl* callee, const block_stmt* self_body, std::vector<const function_decl*>& visiting,
                                             const detail::native_stack_mark& stack);
        chunk::typed_site& typed_site_at(size_t ip);
        void lower_typed_ops(chunk& c);
        void lower_dynamic_ops(chunk& c);
        bool try_lower_decl_move(const std::shared_ptr<variable_decl>& decl, const expression* init);
        bool fusable_index_expr(const expression* e);
        bool index_read_partner_shape(const binary_expr* rb);
        bool try_emit_fused_index(binary_expr* expr, uint32_t flags);
        bool try_emit_fused_index_shape(binary_expr* expr, uint32_t flags);
        bool compile_subscript_into_temp(const expression_ptr& e, uint8_t expect, fused_operand& out);
        const expression* member_path(const expression* e, member_path_proto& out);
        const expression* member_path_root(const expression* e) const;
        uint32_t member_base_slot(const expression* e) const;   // a member read or store whose base is a frame-slot identifier: the base's slot, else invalid
        uint32_t member_this_base(const expression* e) const;   // a member read or store whose base is a member-class body's bare field: its slot + 1, else 0
        void fold_member_temps(chunk& c);
        void fuse_kill_jumps(chunk& c);
        void note_component_base(uint32_t node, const member_expr* m);   // chunk::component_base_slots for a math component store
        bool member_temp_operand(const expression* e) const;
        void compile_member_into_temp(const expression_ptr& e, fused_operand& out);
        bool call_temp_operand(const expression* e) const;   // a call an expression consumes as an operand: a plain-name callee, its result into a compiler temp
        bool value_call_operand(const expression* e) const;
        bool call_partner_ok(const expression* e, const expression* call);           // a partner a hoisted call may run ahead of: a literal or a private local
        void compile_call_into_temp(const expression_ptr& e, fused_operand& out);
        bool quiet_partner(const expression* e);
        bool private_local(const expression* e);       // a frame slot no user code can rebind: a value declaration, not escaping
        bool slot_argument_in(const expression* e, uint32_t slot);   // a call in the subtree takes the slot as a direct argument or receiver (a reference parameter reaches it)
        bool read_ahead_of(const expression* operand, const expression* later);   // an identifier operand read in its op must be taken before `later` runs
        bool graft_value(const expression* e) const;   // a graft's result or spilled operand in a statement's clone (an rvalue to a reference parameter)
        bool primitive_local(const expression* e);     // a private local declared int, float or bool
        bool quiet_read(const expression* e);          // a hoisted read that cannot run user code (a private container of proven element class over a quiet index; a unary or an intrinsic over a quiet operand)
        bool quiet_index(const expression* e);         // a literal, a private local or a quiet read (never a binary: a custom numeric operator is user code)
        bool observing_hoist_in(const expression* e);  // a subtree holding a hoisted read that may run user code
        bool hoist_in(const expression* e);            // a subtree holding a hoisted read, intrinsic or unary, quiet or not
        // a call to a one-argument math:: intrinsic, or a scalar pair row, whose result a compiler temp can carry (none = not one)
        detail::math_fn math_temp_operand(const expression* e) const;
        detail::math_fn math_pair_operand(const call_expr* call);   // math::min(a, b) and the other scalar pair rows over operand shapes
        bool math_pair_argument(const expression* e);
        void compile_math_argument(const expression_ptr& arg, fused_operand& out);
        detail::math_fn math_alias_operand(const call_expr* call) const;   // a bare call to a stdlib math alias (none = not one)
        static bool math_alias_call(const expression* e);                  // a math temp spelled as a bare call: its callee resolves at run time
        void compile_math_into_temp(const expression_ptr& e, fused_operand& out);
        bool unary_temp_operand(const expression* e);
        void compile_unary_into_temp(const expression_ptr& e, fused_operand& out);
        void deref_ahead_of(const expression* pushed, const expression* later);
        bool copy_ahead_of(const expression* pushed, const expression* later);
        bool compile_storage_reference(const expression_ptr& value);
        uint32_t literal_taken_operands(const std::vector<bool>& taken);
        size_t emit_arg_capture(const std::vector<uint8_t>& captures, size_t k, const expression_ptr& arg, uint32_t where, uint32_t form);
        bool ternary_temp_operand(const expression* e);
        void compile_ternary_into_temp(const expression_ptr& e, fused_operand& out);
        void land_ternary_in_temp(const ternary_expr* t, uint32_t dst);
        void land_arm_in_temp(const expression_ptr& arm, uint32_t dst);
        bool env_identifier(const expression* e);
        bool value_temp_operand(const expression* e);
        bool compile_value_into_temp(const expression_ptr& e, uint8_t expect, fused_operand& out);
        // the register-file lift's classification (compile_binary), shared with the fused index emitter
        bool lift_operand_shape(const expression* e);
        bool lift_temp_only(const expression* e);
        bool lift_partners_ok(const binary_expr* b);
        static constexpr int k_lift_temp_limit = 16;   // the most temps one lifted tree holds at once: the window a call pays for
        int lift_temp_peak(const expression* e, int a_limit);
        bool lift_takes(const binary_expr* b);
        bool binary_lands_fused(binary_expr* b);
        bool scalar_value_expr(const expression* e);
        bool numeric_value_binary(expression* e);
        bool reference_slot_operand(const expression* e);
        void materialize_operand_temp(const expression_ptr& e, fused_operand& out);
        void guard_container_temp(const expression_ptr& e, fused_operand& container, uint32_t& guard, bool always);
        static uint8_t operand_expect(const binary_expr* node);
        bool try_lower_store_move(identifier_expr* ident, const expression_ptr& value, uint32_t flags);
        chunk* chunk_ = nullptr;
        std::vector<loop_context> loops_;
        callable_context callable_;
        size_t scope_depth_ = 0;
        const ast_node* current_stmt_ = nullptr;
        // Register-file temps: expression temporaries are frame slots above the parser's
        // local_count (LIFO within one expression tree); compile_callable raises the
        // chunk's local_count to the watermark so the window covers them. Only the main
        // function-body chunk allocates (default-arg/standalone chunks share or lack the
        // callee window — chunk_->is_function_body is the gate).
        size_t temp_next_ = 0;
        size_t temp_high_ = 0;
        bool temps_active_ = false;
        bool program_region_ = false;   // compiling a program statement whose block-scoped declarations hold slots (compile_program)
        // A compiler temp gives up what it holds once its consumer has read it, unless its value is proven to own no instance
        struct temp_release {
            size_t stmt_ip = 0;                       // the statement compiling begins here: its temps' landings are found from here
            std::vector<uint32_t> pending;            // temps whose consumer ran: killed ahead of the next instruction (a peephole may still fold the consumer)
            std::unordered_set<size_t> owning_nothing;    // landings that need no kill: the value owns no instance, or its consumer gives it up
        };
        temp_release release_;
        void note_landing(const expression* e);
        const type_info* declared_type_of(const expression* e);   // a frame slot's or a bare field's declared type (null: unknown)
        bool value_owns_nothing(const expression* e);
        const type_info* slot_declared_type(const expression* e);   // a frame slot's declared type (a reference's referent)
        uint32_t temp_landed(size_t ip, bool& owns);
        std::vector<uint32_t> take_owning_temps(size_t from_ip, size_t floor);
        bool consumer_releases(uint32_t t);
        void free_temps(size_t mark);
        void drop_relanded_kill();
        void flush_kills();
        void release_temps(size_t from_ip, size_t floor);
        void kill_slots(const std::vector<uint32_t>& slots);
        void kill_loop_guards(size_t start);
        void kill_on_both_edges(const std::vector<uint32_t>& owned, std::vector<size_t>& jumps, size_t first);
        void compile_condition_leaf(const expression_ptr& cond, std::vector<size_t>& jumps, bool true_sense);
        uint32_t temp_symbol_index_ = k_invalid_u32;   // shared "\x01tmp" symbols entry
        uint32_t temp_symbol_index();
        // Tail-position hint for `return f(args)` with clean region exits: consumed by
        // compile_call so nested arg calls never inherit it; stamped as c=1 on
        // op_call_from_scratch (a pure hint — every correctness gate lives at runtime)
        bool pending_tail_call_ = false;
        // inlined leaves (vm_inline.cpp): the callable's own body (no recursion), the graft's own compile (no inlining inside a graft or its
        // fallback) and the result slot's name
        const block_stmt* callable_body_ = nullptr;
        std::vector<const block_stmt*> graft_bodies_;   // the grafts being compiled, outermost first: a leaf inside one grafts too (one level), never one of them again
        bool inlining_off_ = false;
        size_t graft_nesting_ = 0;       // compile_hoist's own nesting: grafts compiled inside a graft (its body, its parameters)
        size_t graft_tree_nodes_ = 0;    // the nodes cloned for the outermost graft being compiled and every graft inside it
        // a loop the first compile left open (refused, no block of its own): a graft whose every enclosing loop is open lands by the rules
        // outside a loop, since no block stands to keep (compile_callable compiles again with them); the loops compiled so far and where
        // each began, which name them to the first compile's verdicts
        std::unordered_set<const ast_node*> open_loops_;
        std::vector<std::pair<const ast_node*, uint32_t>> loop_marks_;
        bool graft_in_loop_ = true;   // the graft being planned or compiled lands by the block rules: a leaf its body calls is held to them too
        // an if's condition: each leaf between the short-circuit jumps hoists its own leaf calls, so a graft behind && or || runs only on
        // the path that reaches its operand
        bool leaf_grafts_ = false;
        uint32_t entry_proto_ = k_invalid_u32;   // the structured range-for whose body compiles: its iter proto
        bool leaf_retry_ = false;   // the if's whole condition hoists first: a call behind && or || is retried at its leaf, so the listing names it there
        void mark_loop(const ast_node* stmt) { loops_.back().stmt = stmt; loop_marks_.emplace_back(stmt, static_cast<uint32_t>(chunk_->code.size())); }
        bool in_closable_loop() const;
        std::shared_ptr<chunk> compile_callable_once(std::string_view name, const std::vector<parameter>& parameters, const std::shared_ptr<block_stmt>& body, size_t local_count);
        std::shared_ptr<chunk> compile_program_code(const std::vector<declaration_ptr>& declarations, bool program_frame);
        std::shared_ptr<chunk> compile_callable_code(std::string_view name, const std::vector<parameter>& parameters, const std::shared_ptr<block_stmt>& body, size_t local_count);
        std::shared_ptr<chunk> compile_standalone_expression_code(const expression_ptr& expr);
        std::shared_ptr<chunk> compile_standalone_declaration_code(const declaration_ptr& decl);
        std::shared_ptr<chunk> compile_level_initializers_code(std::string_view name, const std::vector<std::pair<uint64_t, expression_ptr>>& initializers);
        template<class Compile>
        std::shared_ptr<chunk> within_native_stack(std::string_view name, size_t local_count, bool function_body, Compile&& compile);
        detail::native_stack_mark stack_;
        int compile_entries_ = 0;   // compile entries open: the outermost arms the mark and answers a nesting failure
        std::unordered_map<std::string, bool> nested_land_memo_;   // a call a graft's body makes, by what decides it (the callee, the grafts around it, its arguments' shapes and kinds): whether it grafts as the body's block instructions
        uint64_t inline_symbol_ = 0;
        std::unordered_map<const call_expr*, std::pair<uint32_t, uint32_t>> alias_pins_;   // a stdlib alias whose callee a guard resolved at its place (call site, mark temp): compile_math_into_temp takes them
        std::unordered_map<const call_expr*, std::pair<uint32_t, uint32_t>> callee_probes_;   // a call staged ahead of a graft (probe ip, argument prefix already evaluated/captured in owned temps): compile_call completes it
        std::unordered_map<const call_expr*, std::vector<uint32_t>> staged_arguments_;   // owned argument temps, moved into their call in source order
        std::unordered_map<const binary_expr*, std::shared_ptr<identifier_expr>> subscript_guards_;   // a graft statement's element read whose container a guard took ahead of the graft: the guard's temp
        std::shared_ptr<identifier_expr> take_subscript_guard(const binary_expr* read);
        std::unordered_map<size_t, std::vector<size_t>> capture_sites_;   // capture instruction patches owned by an emitted callee probe
        struct hoist_plan { std::shared_ptr<call_expr> call; std::shared_ptr<const function_decl> fn; std::shared_ptr<identifier_expr> receiver; std::shared_ptr<identifier_expr> receiver_field; bool receiver_this = false; std::vector<std::pair<expression_ptr, script_value_type>> spills; std::unordered_map<const call_expr*, size_t> argument_prefixes; std::vector<proof_global> receiver_globals; bool in_loop = false; bool plain_numeric = false; const char* why = nullptr; };   // why: the rule that kept the call when the plan failed
        const char* inline_eligible(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame = nullptr) const;   // nullptr: the graft takes the call; else the rule that keeps it
        script_value_type arg_kind(const expression* a, const std::vector<const type_info*>* frame) const;
        std::vector<script_value_type> site_kinds(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame, script_value_type* result) const;
        script_value_type untyped_result_kind(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame) const;
        void infer_local_kinds(const std::vector<parameter>& parameters, const block_stmt* body, size_t local_count);
        void graft_param_types(const call_expr* call, const function_decl* fn, const std::vector<const type_info*>* frame, std::vector<const type_info*>& out) const;
        bool names_slot(const expression* e, uint32_t slot);
        bool push_value_temp(const std::shared_ptr<call_expr>& expr, identifier_expr* recv, const expression_ptr& arg, fused_operand& out);
        bool plan_hoist(const expression_ptr& root, bool statement_position, hoist_plan& out);
        type_info_ptr inline_type(script_value_type kind);
        type_info_ptr inline_ref_type();
        type_info_ptr graft_result_type(const function_decl* fn);
        struct land_ctx graft_land_ctx(const function_decl* fn);
        std::shared_ptr<identifier_expr> inline_temp(const source_location& loc);
        expression_ptr compile_hoist(const hoist_plan& plan, const expression_ptr& root, const char*& refused);
        expression_ptr hoist_leaf_calls(const expression_ptr& e, bool statement_position, bool& changed);
        void compile_graft_exit();
        bool try_inline_expression(const expression_ptr& e);
        bool try_inline_variable_decl(const std::shared_ptr<variable_decl>& decl);
        bool try_inline_return(const statement_ptr& stmt);
        expression_ptr hoisted_condition(const expression_ptr& cond);
        bool try_condition_graft(const expression_ptr& cond, std::vector<size_t>& false_jumps);
        void compile_hoisted_update(const expression_ptr& update);

        size_t emit(opcode op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
        // Fuses a trailing fused-comparison condition with its jump_if_false
        size_t emit_cond_jump_false(const expression* cond_expr);
        size_t emit_cond_jump_true(const expression* cond_expr);
        bool fuse_cond_compare(const expression* cond_expr, uint32_t false_target, uint32_t proved);
        // Fuses a whole-RHS trailing fused-binary/load/const with its identifier compound store
        void emit_compound_store(uint32_t symbol_index, uint32_t slot, uint32_t kind_flags, const expression* rhs, uint8_t typed_class = operand_class_unknown);
        void patch_jump(size_t at, size_t target);
        uint32_t add_constant(const script_value& v);
        uint32_t add_symbol(uint64_t id);
        uint32_t add_message(std::string_view msg);
        uint32_t add_node(std::shared_ptr<ast_node> node);
        void emit_const_guard(const std::shared_ptr<ast_node>& node);
        [[noreturn]] void unreachable_construct(const char* construct);

        void compile_declaration(const declaration_ptr& decl, bool top_level);
        void compile_statement(const statement_ptr& stmt);
        void compile_declaration_code(const declaration_ptr& decl, bool top_level);
        void compile_statement_code(const statement_ptr& stmt);
        void note_debug_span(const ast_node* node, size_t start);
        void note_debug_local(size_t slot, std::string_view name);
        void note_debug_locals(const statement* decl);
        void close_debug_locals(size_t mark);
        void drop_debug_span(const ast_node* node);
        void compile_block(block_stmt* block);
        void compile_expression(const expression_ptr& expr, bool as_statement = false);
        // each construct of compile_expression and compile_statement_code compiles in its own frame: a level of nesting holds only the
        // frames of the constructs it nests through
        void compile_ternary(const ternary_expr* t);
        void compile_array_literal(const expression_ptr& expr);
        void compile_map_literal(const expression_ptr& expr);
        void compile_include_expression(const include_expr* inc);
        void compile_import_expression(const expression_ptr& expr);
        void compile_throw(const expression_ptr& expr);
        void compile_yield(const yield_expr* ye);
        void compile_return(const statement_ptr& stmt);
        void compile_destructuring(const destructuring_decl* dd);
        void compile_include_declaration(const include_decl* inc);
        void compile_import_declaration(const declaration_ptr& decl);
        closure_proto closure_proto_of(const std::shared_ptr<lambda_expr>& expr);
        std::shared_ptr<chunk> open_callable(std::string_view name, const std::vector<parameter>& parameters, const std::shared_ptr<block_stmt>& body, size_t local_count);
        void close_callable(chunk& result, const block_stmt& body);
        std::shared_ptr<chunk> recompile_open_loops(std::shared_ptr<chunk> first, std::string_view name, const std::vector<parameter>& parameters, const std::shared_ptr<block_stmt>& body, size_t local_count);
        std::shared_ptr<chunk> nesting_exhausted_chunk(std::string_view name, size_t local_count, bool function_body, const source_location& at);
        // A call site whose arguments are still compiling lives here, not in a frame of the walk (a call_site is several hundred bytes)
        std::vector<call_site> pending_sites_;
        struct site_lease {
            std::vector<call_site>& sites;
            size_t at;
            explicit site_lease(std::vector<call_site>& a_sites) : sites(a_sites), at(a_sites.size()) { sites.emplace_back(); }
            ~site_lease() { sites.resize(at); }
            site_lease(const site_lease&) = delete;
            site_lease& operator=(const site_lease&) = delete;
            call_site& operator*() const { return sites[at]; }
            call_site* operator->() const { return &sites[at]; }
        };
        // Statement-position stores/incdecs whose result is discarded compile to a
        // no-result form (no push, no op_pop). Returns false if the shape doesn't apply.
        bool compile_no_result_expression(const expression_ptr& expr);
        bool try_emit_text_builder_store(identifier_expr* ident, assignment_expr* assign);
        bool try_emit_text_append(identifier_expr* ident, assignment_expr* assign);
        bool try_emit_text_builder_member(const std::shared_ptr<member_expr>& member, assignment_expr* assign);
        bool try_emit_text_builder_index(binary_expr* sub, assignment_expr* assign, uint32_t string_site);
        const expression* text_builder_chain(assignment_expr* assign, std::vector<const binary_expr*>& chain);
        void emit_text_builder_chain(const std::vector<const binary_expr*>& chain);
        bool same_receiver(const expression* a, const expression* b);
        // Fused subscript store shared by the expression and statement (no_result) forms;
        // false = the shape doesn't fuse and the caller compiles the unfused sequence
        bool try_emit_fused_index_store(binary_expr* sub, const expression_ptr& value, bool no_result, uint32_t string_site, uint32_t compound = k_invalid_u32);
        // The chunk::string_target_sites entry of a subscript store target (k_invalid_u32 when
        // its base can only be a temporary or arrives as its own reference)
        uint32_t string_target_site_for(const expression_ptr& target);
        bool try_emit_fused_index_store_shape(binary_expr* sub, const expression_ptr& value, bool no_result, uint32_t string_site, uint32_t compound = k_invalid_u32);
        bool try_emit_fused_push(const std::shared_ptr<call_expr>& call);
        // Branch-position condition: &&/|| thread straight to conditional jumps (no bool
        // materialization; each comparison pairs into fused_cmp_jump). Falsy control lands
        // on the collected jumps — the caller patches them to the else/exit target.
        void compile_branch_condition(const expression_ptr& cond, std::vector<size_t>& false_jumps);
        void compile_branch_condition_true(const expression_ptr& cond, std::vector<size_t>& true_jumps);

        void compile_identifier_load(const expression_ptr& expr);
        void compile_binary(const std::shared_ptr<binary_expr>& expr);
        bool compile_binary_shaped(const std::shared_ptr<binary_expr>& expr);   // false: the stack form, nothing emitted
        const binary_expr* not_append_chain_ = nullptr;   // compile_binary's spine: a `+` known to be no append chain
        void compile_unary(const std::shared_ptr<unary_expr>& expr);
        void compile_assignment(const std::shared_ptr<assignment_expr>& expr, bool as_statement);
        void record_argument_source(call_site& site, const expression_ptr& arg, size_t argc);
        uint32_t place_argument_base(const expression* arg);
        void compile_call(const std::shared_ptr<call_expr>& expr);
        void compile_member(const std::shared_ptr<member_expr>& expr);
        void compile_member_read(const std::shared_ptr<member_expr>& expr);
        const expression* held_root_ = nullptr;   // the temporary a read chain holds, until compile_expression reaches it (detail::held_chain_root)
        bool held_root_fired_ = false;
        void member_tail_into_temp(const expression* e);
        bool size_temp_operand(const expression* e) const;
        bool enum_temp_operand(const expression* e) const;
        bool elem_member_operand(const expression* e) const;
        bool velem_operand(const expression* e) const;
        void velem_proto_for(const expression_ptr& member, velem_proto& p);
        uint32_t elem_alias_temp();
        void compile_new(const std::shared_ptr<new_expr>& expr);
        void compile_cast(const std::shared_ptr<cast_expr>& expr);
        void compile_lambda(const std::shared_ptr<lambda_expr>& expr);
        static bool compound_rhs_inert(const expression* e);
        uint32_t declared_ref_at_bind(const variable_decl* decl) const;
        void note_slot_declared_ref(size_t slot, const type_info* referent);
        void compile_variable_decl(const std::shared_ptr<variable_decl>& decl);
        void compile_variable_decl_value(const std::shared_ptr<variable_decl>& decl);
        void compile_decl_value(const std::shared_ptr<variable_decl>& decl, const expression_ptr& init);
        void compile_ternary_store(identifier_expr* ident, assignment_expr* assign, const std::shared_ptr<ternary_expr>& t);
        void compile_store_arm(identifier_expr* ident, assignment_expr* assign, const expression_ptr& value);
        void compile_function_decl(const std::shared_ptr<function_decl>& decl);
        void compile_if(if_stmt* stmt);
        void compile_while(while_stmt* stmt);
        void compile_for(for_stmt* stmt);
        // Emits the op_cfor_prep/back counted-loop form when the for matches the
        // interpreter's counting-loop pattern; false = use the generic codegen
        bool compile_counted_for(for_stmt* stmt);
        void compile_range_for(range_for_stmt* stmt);
        bool compile_entry_half(const expression_ptr& init);
        void compile_try(try_stmt* stmt);
        void compile_switch(switch_stmt* stmt);
        bool compile_switch_branches(switch_stmt* stmt);
        void compile_case_body(const std::vector<statement_ptr>& body);
        void compile_break();
        void compile_continue();
        // Emits op_try_pop/op_catch_end/switch stack pops for every region between the current point and target_region (SIZE_MAX = all
        // regions, for return), then kills the scopes down to kill_depth; a handler's own locals die ahead of the thrown object it holds
        size_t emit_region_exits(size_t target_region, size_t kill_depth = SIZE_MAX);   // returns the scope depth the exits left
        void collect_declared_symbols(const statement_ptr& stmt, std::unordered_set<uint64_t>& out) const;
        void collect_outer_slot_candidates(const statement_ptr& stmt,
                                           const std::unordered_set<uint64_t>& declared,
                                           std::vector<std::pair<uint64_t, size_t>>& out,
                                           std::unordered_set<uint64_t>& seen) const;

        uint32_t identifier_slot_operand(identifier_expr* ident) const;
        bool symbol_is_env_only(uint64_t symbol_id) const;
        bool call_args_need_probe(const call_expr& call) const;
        fused_operand make_fused_operand(const expression* e);
        // Node-owning spelling: an env-resolved plain identifier also pins its node
        // (fused_operand::ident_node) so field proofs stay live across re-stamps
        fused_operand make_fused_operand(const expression_ptr& e);
        // chunk::ident_nodes[ip] = the identifier's node when it resolves through the env
        // (fields are never slotted); slotted identifiers leave no entry
        void note_ident_node(size_t ip, const expression_ptr& ident);

        static uint32_t binary_shape(binary_expr* expr);
    };

} // namespace jai::vm

#endif // __JAISCRIPT_VM_VM_COMPILER_HPP__
