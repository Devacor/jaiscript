#pragma once

#ifndef __JAISCRIPT_DETAIL_AST_HPP__
#define __JAISCRIPT_DETAIL_AST_HPP__

#include <jaiscript/core/types.hpp>
#include <jaiscript/core/type_info.hpp>
#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include "lexer.hpp"
#include <initializer_list>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

namespace jai {

    // Forward declarations
    class ast_node;
    class expression;
    class statement;
    class declaration;
    struct script_defined_function;   // environment.hpp; node mint caches hold it opaquely
    class class_definition;           // class_definition.hpp; node caches hold it by raw pointer
    namespace detail::regex { class compiled; }   // detail/regex.hpp; a constant pattern's compile, engine-independent

    using ast_node_ptr = std::shared_ptr<ast_node>;
    using expression_ptr = std::shared_ptr<expression>;
    using statement_ptr = std::shared_ptr<statement>;
    using declaration_ptr = std::shared_ptr<declaration>;
    
    // Forward declarations for switch statement components
    class switch_stmt;
    class case_stmt;
    class default_stmt;
    class fallthrough_stmt;
    class block_stmt;

    using switch_stmt_ptr = std::shared_ptr<switch_stmt>;
    using case_stmt_ptr = std::shared_ptr<case_stmt>;
    using default_stmt_ptr = std::shared_ptr<default_stmt>;
    using fallthrough_stmt_ptr = std::shared_ptr<fallthrough_stmt>;
    
    // Node type enum for switch-based dispatch (faster than virtual calls)
    enum class node_type : uint8_t {
        // Expressions (0-19)
        literal_expr = 0,
        identifier_expr,
        binary_expr,
        unary_expr,
        assignment_expr,
        call_expr,
        member_expr,
        lambda_expr,
        new_expr,
        ternary_expr,
        array_literal_expr,
        map_literal_expr,
        this_expr,
        super_expr,
        throw_expr,
        yield_expr,
        include_expr,   // include in expression position: evaluates to the file's result
        cast_expr,      // cast<T>(e) / cast<T&>(e) / cast<shared_ptr<T>>(e)
        import_expr,    // import(...): the call form, true when a module (re)ran

        // Statements (20-39)
        expression_stmt = 20,
        block_stmt,
        if_stmt,
        while_stmt,
        for_stmt,
        range_for_stmt,
        return_stmt,
        break_stmt,
        continue_stmt,
        try_stmt,
        switch_stmt,
        case_stmt,
        default_stmt,
        fallthrough_stmt,
        parallel_for_stmt,   // parallel_for (auto& x : arr) { ... } - in-place fork-join

        // Declarations (40-59)
        variable_decl = 40,
        function_decl,
        class_decl,
        namespace_decl,
        expression_decl,
        statement_decl,
        include_decl,
        import_decl,
        enum_decl,
        destructuring_decl,
        extern_decl,      // extern [type | function] name; - a name defined outside the program
    };

    // The checker's const verdict at a write or a binding, stamped by every walk: const_raises makes the node
    // raise `text` (an assignment after its right side runs, everything else when reached); const_runtime leaves the decision
    // to the run time (a path through a var: detail/const_rules.hpp)
    inline constexpr uint8_t const_none = 0, const_raises = 1, const_runtime = 2;
    struct const_site {
        uint8_t verdict = const_none;
        std::shared_ptr<const std::string> text;
        uint64_t text_id = 0;   // the text interned at stamp time
        explicit operator bool() const noexcept { return verdict != const_none; }
    };

    // parameter information for functions and lambdas
    struct parameter {
        type_info_ptr type;
        std::string name;
        bool is_reference = false;
        bool is_const = false;
        // Parser escape analysis: the parameter may be bound by reference downstream
        // (bare-identifier ref argument / ref-decl source), so value binding boxes its
        // storage into a cell (see reference_holder cell mode)
        bool ref_escaping = false;
        // Parser escape analysis for a `T& p` parameter: its reference reaches another binding (a call argument), so the call
        // marks it with T (detail::ref_declare) for the bindings that cannot see the name
        bool declared_ref_escapes = false;
        // A converted closure's captured name (never parsed): its slot binds the closure's cell when the frame enters
        bool binds_capture = false;
        mutable uint64_t symbol_id = UINT64_MAX;  // Cached symbol ID for optimization
        mutable size_t slot_index = SIZE_MAX;     // Slot index for fast local access (SIZE_MAX = not assigned)
        // A reference parameter the body never writes through takes a temporary
        // (detail/ref_param_writes.hpp): the verdict and the definition epoch it was derived at
        mutable uint64_t temporary_epoch = 0;
        mutable bool takes_temporary = false;
        expression_ptr default_value;

        parameter(type_info_ptr t, const std::string& n, bool ref = false, bool c = false)
            : type(t), name(n), is_reference(ref), is_const(c), symbol_id(UINT64_MAX), slot_index(SIZE_MAX) {}

        // `var` or untyped (an `auto` parameter has no type): a by-value binding takes the var tag, as a var declaration does
        bool declared_var() const noexcept { return type && type->base_type == script_value_type::jai_any_type; }
    };
    
    // Base AST node
    class ast_node {
    public:
        source_location location;
        node_type type_;  // For switch-based dispatch

        ast_node(const source_location& loc, node_type t) : location(loc), type_(t) {}
        virtual ~ast_node() = default;

        // Fast type check for switch dispatch
        [[nodiscard]] node_type get_type() const noexcept { return type_; }
    };
    
    namespace detail {
        // A chain the parser builds without recursing (a + b + c, a.b.c, f()(), a[0][0], a = b = c) is released without recursing:
        // a dying operator node hands its operands to one work list (parser.cpp)
        void release_operands(std::initializer_list<expression_ptr*> operands, std::vector<expression_ptr>* more = nullptr) noexcept;
    }

    // Base expression node
    class expression : public ast_node {
    public:
        expression(const source_location& loc, node_type t) : ast_node(loc, t) {}
    };
    
    // Literal expression
    class literal_expr : public expression {
    public:
        script_value value;

        literal_expr(const source_location& loc, const script_value& val)
            : expression(loc, node_type::literal_expr), value(val) {
#ifdef JAISCRIPT_DIAG_XTHREAD_RC
            value.diag_tag_single_thread();
#endif
        }    };
    
    // Parse-time type proofs. Stamped by the static checker's proof
    // pass, never serialized (jaibite re-derives them at load), read by the VM's compiler for lanes
    // that keep total runtime guards. Default = no proof.
    inline constexpr uint8_t proof_none = 0;
    inline constexpr uint8_t proof_same_type = 1;
    inline constexpr uint8_t operand_class_unknown = 0, operand_class_int_int = 1, operand_class_float_float = 2,
        operand_class_int_float = 3, operand_class_float_int = 4, operand_class_string_string = 5, operand_class_bool_bool = 6,
        operand_class_object_element = 7,   // a subscript whose container the checker proved an array of class instances
        operand_class_vec_element = 8;      // a subscript whose container the checker proved a math vector buffer (array<math::vec3>)

    // identifier expression
    class identifier_expr : public expression {
    public:
        std::string_view name;  // Points to symbolizer storage (permanent)
        uint64_t symbol_id;     // Interned symbol ID - must be set by parser
        size_t slot_index = SIZE_MAX; // Slot index for fast local access (SIZE_MAX = use environment)
        // C++ ::name spelling: resolution is GLOBAL-ENV-ONLY - locals, captures, and
        // the member scope are all skipped (the escape hatch for deliberately shadowed
        // globals). Parser never assigns a slot when set.
        bool global_qualified = false;
        // Parser resolution: false when this name statically resolves to a REFERENCE
        // declaration (ref param / auto& decl / auto& loop var). Simple stores consult it
        // when the storage holds a cell: value-decl names write the cell like the variable
        // itself (typed enforcement); ref-alias names store through unconstrained.
        bool names_value_decl = true;
        // Parser resolution: the name resolves to a by-value var declaration (a var local, a var or untyped parameter, a var
        // loop variable): a store into it treats a handle it holds as rebindable whatever the handle's own tag
        bool names_var_decl = false;
        // Parser resolution: true when this name statically resolves to a VALUE declaration the unit can see (a
        // local, a parameter, a program-level declaration of this unit); false for reference declarations and
        // for names the unit does not declare. A reference bound to such a name dereferences a cell, never a getter.
        bool names_known_value = false;
        // Parser resolution: the name is a slot-resident `T& name = e;` declaration or `T& p` parameter with a concrete T (the
        // declared type governs a store through it): the storage index a value of T's kind has, 0xFF when
        // every store runs T's table (detail::declared_ref_kind), and T in declared_ref
        uint8_t declared_ref_kind = 0;
        mutable bool this_field = false;   // a bare name in a method the checker resolved to a field of the class chain (declared, no user accessor): the method graft's `recv.name`
        mutable bool program_name = false;   // a bare name in a method the checker resolved past every member of the class chain: the method graft's program-scope read
        type_info* declared_ref = nullptr;

        identifier_expr(const source_location& loc, std::string_view n, uint64_t sym_id)
            : expression(loc, node_type::identifier_expr), name(n), symbol_id(sym_id), slot_index(SIZE_MAX) {}
    };
    
    // Binary expression
    class binary_expr : public expression {
    public:
        expression_ptr left;
        token op;
        expression_ptr right;
        // Subscript reads (op == left_bracket) whose result is provably consumed as a
        // transient value: both backends elide the element-reference mint and push a
        // shallow copy instead (runtime-gated on operator overrides). Stamped by the
        // parser's transient_read_marker pass (detail/transient_read.hpp).
        bool transient_element_read = false;
        // Static operand kinds (operand_class_*): annotation only in v1, no consumer.
        mutable uint8_t static_operand_class = operand_class_unknown;
        // a subscript's container the checker proved: 1 string, 2 array, 3 map (0 unproven)
        mutable uint8_t subscript_receiver = 0;

        binary_expr(const source_location& loc, expression_ptr l, const token& o, expression_ptr r)
            : expression(loc, node_type::binary_expr), left(l), op(o), right(r) {}
        ~binary_expr() override { detail::release_operands({&left, &right}); }
        // Returns true if this binary expression ALWAYS produces a boolean result
        // (comparison or logical operators)
        [[nodiscard]] inline bool returns_bool() const noexcept {
            switch (op.type) {
                // Comparison operators - always return bool
                case token_type::less:
                case token_type::greater:
                case token_type::less_equal:
                case token_type::greater_equal:
                case token_type::equal_equal:
                case token_type::bang_equal:
                // Logical operators - always return bool
                case token_type::ampersand_ampersand:
                case token_type::pipe_pipe:
                    return true;
                default:
                    return false;
            }
        }
    };
    
    // Unary expression
    class unary_expr : public expression {
    public:
        token op;
        expression_ptr operand;
        bool is_postfix;
        mutable const_site const_check;   // ++/-- of a const name

        unary_expr(const source_location& loc, const token& o, expression_ptr expr, bool postfix = false)
            : expression(loc, node_type::unary_expr), op(o), operand(expr), is_postfix(postfix) {}
        ~unary_expr() override { detail::release_operands({&operand}); }
        // Returns true if this unary expression ALWAYS produces a boolean result
        [[nodiscard]] inline bool returns_bool() const noexcept {
            return op.type == token_type::bang;  // Logical NOT always returns bool
        }
    };

    // Assignment expression
    class assignment_expr : public expression {
    public:
        expression_ptr target;
        token op;
        expression_ptr value;
        // proof_same_type: the RHS static type IS the target's declared type (identity conversion)
        mutable uint8_t static_store_proof = proof_none;
        // a compound op on a local: the class of the target's declared kind against the value's (operand_class_*), the
        // typed row the compiler picks for the fused compound (unknown: the int row speculated; mixed: no typed row)
        mutable uint8_t static_operand_class = operand_class_unknown;
        // Postfix ++/-- on a non-identifier target desugars to this node with `+= 1`;
        // the expression result is then the OLD value (the parser sets this)
        bool postfix_old_result = false;
        // Set on every ++/-- desugar (prefix and postfix): the target must be numeric, exactly
        // like the identifier path's gate (a string element must not concatenate "1")
        bool incdec_desugar = false;
        // In an assignment target (`(a = b) = c`, `++x = v`) the parser makes the inner assignment yield the reference it
        // stored through (its target is then an implicit cast<&>), the C++ lvalue an assignment is
        bool yields_reference = false;
        mutable const_site const_check;

        assignment_expr(const source_location& loc, expression_ptr t, const token& o, expression_ptr v)
            : expression(loc, node_type::assignment_expr), target(t), op(o), value(v) {}
        ~assignment_expr() override { detail::release_operands({&target, &value}); }    };

    // a program-scope name an argument proof read and what the proof took of it (call_expr::proof_globals): a graft re-checks it once a run
    // moved the definition epoch (the declaring unit may have run again with another type). kind: a scalar's base type, an array of scalars
    // as jai_array_type | element base type << 8, k_type for any other declared type (type, compared by identity); k_function marks a
    // function name whose declared return the proof took (a call's result)
    struct proof_global {
        static constexpr uint16_t k_function = 0x8000;
        static constexpr uint16_t k_type = 0x4000;
        uint64_t symbol = UINT64_MAX;
        uint16_t kind = 0;
        const type_info* type = nullptr;
        bool operator==(const proof_global& o) const noexcept { return symbol == o.symbol && kind == o.kind && type == o.type; }
    };

    // Function call expression
    class call_expr : public expression {
    public:
        expression_ptr callee;
        std::vector<expression_ptr> arguments;
        // Per-argument proof_same_type against the ONE statically resolved same-script callee
        // (proven_callee_decl); empty = unproven call
        mutable std::vector<uint8_t> static_arg_proofs;
        // the program-scope names the argument proofs read (a typed global another unit declares, or this unit's)
        mutable std::vector<proof_global> proof_globals;
        // a builtin method's receiver that is a certain program-scope typed global: a graft in its arguments re-checks it, since the graft
        // runs ahead of the receiver's read
        mutable proof_global receiver_global;
        // weak: the resolved declaration may belong to a program that dies before this site compiles (a dropped jaibite, an evicted parse-cache entry, a redefinition)
        mutable std::weak_ptr<const class function_decl> proven_callee_decl;
        mutable std::weak_ptr<const class function_decl> proven_method_decl;   // `recv.m(...)` on an object receiver in a frame slot: the one overload of the receiver's class the checker resolved (the inliner's method graft)
        mutable uint8_t builtin_receiver = 0;   // the checker proved the method receiver: 1 string, 2 array, 3 map (0 unproven)   // the one free script function a plain identifier callee resolves to (exact arity, a body, not a coroutine; reference parameters allowed): the inliner's identity
        mutable uint8_t builtin_receiver_open = 0;   // unproven, yet no class: the receiver may be a string, array or map (the vm picks the builtin by its kind)
        std::vector<uint8_t> arg_captures;   // per argument, the capture its value needs before a later argument runs (detail::arg_capture_kind); empty = none
        // a method that writes its receiver called on a const, or a const passed to a writing reference parameter
        mutable const_site const_check;
        // regex(pattern[, flags]) with literal texts while `regex` names the engine's constructor: the pattern compiled at load
        // (the checker's stamp, never serialized); both backends make the value from it while the name still binds the constructor
        mutable std::shared_ptr<const detail::regex::compiled> regex_constant;

        call_expr(const source_location& loc, expression_ptr c, std::vector<expression_ptr> args)
            : expression(loc, node_type::call_expr), callee(c), arguments(std::move(args)) {}
        ~call_expr() override { detail::release_operands({&callee}, &arguments); }

        // print(...) or format(...) by name whose first argument is a string literal, with values after it: the literal is its
        // format string (C++20's constant format string), so both backends run the builtin's literal entry while the name binds it
        bool literal_format_call() const {
            if (arguments.size() < 2 || callee->get_type() != node_type::identifier_expr || arguments[0]->get_type() != node_type::literal_expr) {
                return false;
            }
            const std::string_view name = static_cast<const identifier_expr*>(callee.get())->name;
            return (name == "print" || name == "format") && static_cast<const literal_expr*>(arguments[0].get())->value.is_string();
        }
    };

    // member access expression
    class member_expr : public expression {
    public:
        expression_ptr object;
        std::string_view member;  // Points to symbolizer storage (permanent)
        uint64_t member_id = UINT64_MAX;  // Cached interned ID for the member name
        mutable uint64_t getter_id = UINT64_MAX;  // Cached interned ID for "_get_" + member (computed on first use)
        bool is_arrow;  // true for ->, false for .
        bool is_static; // true for ::, false for . or ->
        bool null_safe = false;  // true for ?. null-safe member access

        // math:: intrinsic resolution cache (static access `math::<fn>`): 255 = not yet
        // resolved, 254 = resolved-not-an-intrinsic, else detail::math_fn. Name-based,
        // engine-independent - safe to share across engines like the other AST caches.
        mutable uint8_t math_intrinsic_cache = 255;
        // A member a reference binds over an evaluated base (`f(a[i+1].x)`, `return m[k + s].hp`): when the base evaluates to a
        // reference the read hands back a reference, a math component's COMPONENT reference, and with place_read_field a field's
        // (transient_read_marker stamps it; detail/math_component_store.hpp, detail/ref_lvalue.hpp evaluated_member_reference)
        static constexpr uint8_t place_read_component = 1;
        static constexpr uint8_t place_read_field = 2;
        mutable uint8_t place_read = 0;
        // the checker proved an enum's value read by name (the name holds a map<string,int>, whose member read runs no user code): a
        // capture ahead of it is never needed (transient_read_marker::callout_free)
        mutable bool enum_constant = false;
        mutable uint64_t setter_id = UINT64_MAX;  // "_set_" + member, cached like getter_id
        // A structured range-for binding's read of its entry (detail::stamp_map_entry_bindings): 1 the key, 2 the value
        uint8_t entry_half = 0;
        // a reference bind's verdict for one script class (detail::ref_plain_script_field): the member is a plain script field of it
        // (no C++ property, no user accessor, public chain) while both epochs hold; ref_field_declared is its declared type, ref_field_slot
        // the slot the class assigned it
        mutable const class_definition* ref_field_class = nullptr;
        mutable uint64_t ref_field_shape = 0;
        mutable uint64_t ref_field_methods = 0;
        mutable type_info* ref_field_declared = nullptr;
        mutable uint32_t ref_field_slot = UINT32_MAX;
        // the call this member is the callee of stands where its result is discarded (a statement, a for's update; mark_result_discarded):
        // an in-place builtin's nothing may go unused there and nowhere else (detail/in_place_result.hpp)
        bool call_result_discarded = false;

        member_expr(const source_location& loc, expression_ptr obj, std::string_view mem, bool arrow, bool static_access = false)
            : expression(loc, node_type::member_expr), object(obj), member(mem), is_arrow(arrow), is_static(static_access) {}
        ~member_expr() override { detail::release_operands({&object}); }

        member_expr(const source_location& loc, expression_ptr obj, std::string_view mem, uint64_t mem_id, bool arrow, bool static_access = false)
            : expression(loc, node_type::member_expr), object(obj), member(mem), member_id(mem_id), is_arrow(arrow), is_static(static_access) {}    };
    
    // Lambda expression
    class lambda_expr : public expression {
    public:
        enum class capture_default {
            none,           // No default capture (explicit captures only)
            by_value,       // [=] - capture all by value
            by_reference    // [&] - capture all by reference
        };
        
        struct capture {
            std::string_view name;      // Variable name - points to symbolizer storage (permanent)
            bool by_reference;          // True if this specific variable is captured by reference
            mutable uint64_t symbol_id = UINT64_MAX;  // Cached symbol ID for optimization
            size_t outer_slot = SIZE_MAX;  // Enclosing-frame slot the parser resolved the name to
            expression_ptr initializer;    // an init-capture `[x = e]`: e, evaluated at creation into the lambda's own x

            // Constructor for explicit variable captures
            capture(std::string_view n, bool by_ref)
                : name(n), by_reference(by_ref), symbol_id(UINT64_MAX) {}
        };
        
        capture_default default_capture = capture_default::none;

        std::vector<capture> captures;
        std::vector<parameter> parameters;
        type_info_ptr return_type;
        statement_ptr body;
        size_t local_count = 0;  // Total slots needed (params + locals) for stack allocation

        // Outer-frame locals this lambda captures, as (symbol_id, outer slot) pairs.
        // Built on the FIRST closure creation, which also patches the body identifiers'
        // slot_index to SIZE_MAX — so later creations from this same AST must replay
        // this plan rather than re-scan (a re-scan would find nothing).
        mutable std::vector<std::pair<uint64_t, size_t>> outer_slot_plan;
        mutable bool outer_slot_plan_built = false;

        // the interpreter's free names of the body (this_id for this/super), the vm proto's used_variables
        mutable std::vector<uint64_t> free_names;
        mutable bool free_names_built = false;
        mutable bool uses_instance = false;
        // the capture list was checked against the creating scope once (both backends); later creations skip it
        mutable bool capture_list_verified = false;

        // the lambda as a declaration for a call of the local it initializes (the checker's proofs, the inliner's leaf graft)
        mutable std::shared_ptr<class function_decl> graft_decl;
        mutable std::vector<const class function_decl*> graft_overloads;
        mutable uint8_t inferred_return_kind = 0;   // no declared return type, every return the same certain kind: 1 int, 2 float, 3 bool (the checker's)

        // Chain captures resolved by the parser for NESTED lambdas: enclosing-frame
        // locals that only a nested lambda uses (this lambda captures the slot), and
        // names a nested lambda needs this lambda's default capture to carry
        std::vector<std::pair<uint64_t, size_t>> chained_outer_slots;
        std::vector<uint64_t> chained_outer_names;

        // Shared immutable parameter storage, minted once per node (see function_decl):
        // every closure created from this lambda shares it instead of deep-copying.
        // Capture-free lambdas (closure_env = nullptr) additionally reuse the ONE
        // script_defined_function across creations under one unit (a file two units include
        // mints one per includer).
        mutable std::shared_ptr<const std::vector<parameter>> shared_params_cache;
        mutable std::shared_ptr<script_defined_function> mint_cache;

        lambda_expr(const source_location& loc) : expression(loc, node_type::lambda_expr), local_count(0) {}
    };

    // Node-cached shared parameter storage (function_decl / lambda_expr): built on the
    // first mint, shared by every later function value from the same node. A parallel region
    // never fills it: its admission pre-mints every body a worker can call, a class's implicit
    // default constructor mints with the class, and parallel bodies reject nested declarations.
    template<typename NodeT>
    inline const std::shared_ptr<const std::vector<parameter>>& shared_parameters_for(const NodeT* node) {
        if (!node->shared_params_cache) {
            node->shared_params_cache = std::make_shared<const std::vector<parameter>>(node->parameters);
        }
        return node->shared_params_cache;
    }

    // New expression
    class new_expr : public expression {
    public:
        type_info_ptr type;
        std::vector<expression_ptr> arguments;
        std::vector<uint8_t> arg_captures;

        new_expr(const source_location& loc, type_info_ptr t, std::vector<expression_ptr> args)
            : expression(loc, node_type::new_expr), type(t), arguments(std::move(args)) {}    };

    // include in expression position — always runs the file, evaluates to its result
    // (exactly what engine::execute returns for that source). Statement-position include
    // stays include_decl (value discarded).
    class include_expr : public expression {
    public:
        std::string path;          // literal syntax: include "file.jai" / include <file>
        expression_ptr path_expr;  // computed syntax: include(expr)

        include_expr(const source_location& loc, const std::string& p)
            : expression(loc, node_type::include_expr), path(p), path_expr(nullptr) {}

        include_expr(const source_location& loc, expression_ptr expr)
            : expression(loc, node_type::include_expr), path(), path_expr(expr) {}    };

    // An import site's memory for the engine-wide check pass (engine::import_skips): the sites that loaded their modules
    // under the engine's import generation skip while no pass is open; a computed site remembers the paths it loaded
    struct import_site {
        uint64_t seen_generation = 0;
        uint64_t seen_paths_generation = 0;
        std::vector<uint64_t> seen_paths;
        uint64_t pass_id = 0;               // the check pass this site last reached in
        std::vector<uint64_t> pass_paths;   // the paths it checked in that pass: one reached again means the loop wrapped
    };

    // import(...): the call form. Each argument is a path or an array of paths (every argument a string literal: `paths`
    // holds them, else `path_exprs` the arguments), an optional trailing jai::force. One throttle decision for the whole
    // list, every changed module parsed and checked before any runs (engine::import_modules); true when a module (re)ran.
    class import_expr : public expression {
    public:
        std::vector<std::string> paths;
        std::vector<expression_ptr> path_exprs;
        bool force = false;
        mutable import_site site;

        explicit import_expr(const source_location& loc) : expression(loc, node_type::import_expr) {}
    };

    // Ternary expression
    class ternary_expr : public expression {
    public:
        expression_ptr condition;
        expression_ptr then_expression;
        expression_ptr else_expression;

        ternary_expr(const source_location& loc, expression_ptr c, expression_ptr t, expression_ptr e)
            : expression(loc, node_type::ternary_expr), condition(c), then_expression(t), else_expression(e) {}    };

    // Array literal expression
    class array_literal_expr : public expression {
    public:
        std::vector<expression_ptr> elements;

        array_literal_expr(const source_location& loc, std::vector<expression_ptr> elems)
            : expression(loc, node_type::array_literal_expr), elements(std::move(elems)) {}    };

    // Map literal expression
    class map_literal_expr : public expression {
    public:
        std::vector<std::pair<expression_ptr, expression_ptr>> entries;

        map_literal_expr(const source_location& loc, std::vector<std::pair<expression_ptr, expression_ptr>> e)
            : expression(loc, node_type::map_literal_expr), entries(std::move(e)) {}    };

    // This expression
    class this_expr : public expression {
    public:
        this_expr(const source_location& loc) : expression(loc, node_type::this_expr) {}    };

    // Super expression
    class super_expr : public expression {
    public:
        super_expr(const source_location& loc) : expression(loc, node_type::super_expr) {}    };

    // Throw expression
    class throw_expr : public expression {
    public:
        expression_ptr value;  // Optional - null for re-throw

        throw_expr(const source_location& loc, expression_ptr val = nullptr)
            : expression(loc, node_type::throw_expr), value(val) {}    };

    class yield_expr : public expression {
    public:
        expression_ptr value;  // Optional - null for void yield

        yield_expr(const source_location& loc, expression_ptr val = nullptr)
            : expression(loc, node_type::yield_expr), value(std::move(val)) {}    };

    // cast<T>(e) (value copy tagged T), cast<T&>(e) (reference, by_reference), and
    // cast<shared_ptr<T>>(e) (target IS the shared_ptr<T> spelling) - detail/cast_kernel.hpp
    class cast_expr : public expression {
    public:
        type_info_ptr target;
        bool by_reference;
        // A piece of an assignment target the parser made read as a reference (a conditional's arm, an inner assignment's
        // target): no type verdict, and a value that is no reference is no lvalue
        bool implicit_ref = false;
        // An implicit arm where a reference may bind (an argument, a T& declaration, default or return): the arm is read as a
        // value, and its storage, when it names one, replaces the value
        bool binding_ref = false;
        // a parallel body's rewritten `return e;` (parallel_transform.cpp): e converts as that function's return does, with its text
        std::string_view return_of;
        expression_ptr operand;
        mutable const_site const_check;   // cast<T&>(c) / cast<shared_ptr<T>>(c) of a const

        cast_expr(const source_location& loc, type_info_ptr t, bool by_ref, expression_ptr e)
            : expression(loc, node_type::cast_expr), target(std::move(t)), by_reference(by_ref), operand(std::move(e)) {}    };

    // Base statement node
    // [[warn_mode]] / [[strict_mode]] led the statement (docs/site/embedding/06-static-checking.html "Escape hatches").
    // Checker-only: on a block's first statement it covers the block, elsewhere the statement's
    // own subtree; both backends ignore it.
    inline constexpr uint8_t check_mode_attr_none = 0, check_mode_attr_warn = 1, check_mode_attr_strict = 2;

    class statement : public ast_node {
    public:
        uint8_t check_mode_attr = check_mode_attr_none;

        statement(const source_location& loc, node_type t) : ast_node(loc, t) {}
    };
    
    // expression statement
    // An expression whose value its place discards: a call's member callee learns so, through a conditional's arms and the body of
    // an immediately called arrow function (the rewrite of a?.b()), so a builtin that returns nothing may stand there
    inline void mark_result_discarded(expression* e);

    class expression_stmt : public statement {
    public:
        expression_ptr expression;

        expression_stmt(const source_location& loc, expression_ptr expr)
            : statement(loc, node_type::expression_stmt), expression(expr) { mark_result_discarded(expression.get()); }    };

    // Block statement
    class block_stmt : public statement {
    public:
        std::vector<declaration_ptr> declarations;
        // A function body whose locals a lambda captures by reference: its frame exit clears the cell cycles they close
        // (script_value::release_cell_cycles); set by the escape-mark pass
        bool shares_cells = false;

        block_stmt(const source_location& loc, std::vector<declaration_ptr> decls)
            : statement(loc, node_type::block_stmt), declarations(std::move(decls)) {}    };

    // If statement
    class if_stmt : public statement {
    public:
        expression_ptr condition;
        statement_ptr then_statement;
        statement_ptr else_statement;  // Can be null

        if_stmt(const source_location& loc, expression_ptr c, statement_ptr t, statement_ptr e = nullptr)
            : statement(loc, node_type::if_stmt), condition(c), then_statement(t), else_statement(e) {}    };

    // While statement
    class while_stmt : public statement {
    public:
        expression_ptr condition;
        statement_ptr body;

        while_stmt(const source_location& loc, expression_ptr c, statement_ptr b)
            : statement(loc, node_type::while_stmt), condition(c), body(b) {}    };

    // For statement
    class for_stmt : public statement {
    public:
        declaration_ptr initializer;     // Can be null
        expression_ptr condition;  // Can be null
        expression_ptr update;     // Can be null
        statement_ptr body;

        for_stmt(const source_location& loc, declaration_ptr i, expression_ptr c, expression_ptr u, statement_ptr b)
            : statement(loc, node_type::for_stmt), initializer(i), condition(c), update(u), body(b) { mark_result_discarded(update.get()); }    };

    // Range-based for statement (C++11 style)
    class range_for_stmt : public statement {
    public:
        type_info_ptr element_type;      // Type of loop variable (auto, int, etc.)
        std::string_view variable_name;  // Name of loop variable - points to symbolizer storage (permanent)
        uint64_t variable_name_id = UINT64_MAX;  // Interned ID for fast lookup (UINT64_MAX = not interned)
        size_t variable_slot_index = SIZE_MAX;   // Slot index for fast local access (SIZE_MAX = use environment)
        bool is_reference;             // true for auto&, false for auto
        bool is_const;                 // true for const auto&
        // Parser escape analysis (value-mode loop vars): per-iteration values are stored
        // through a cell so the variable can be bound by reference
        bool var_ref_escaping = false;
        // Declared auto: the checker's first proof pass makes the variable a var when the container's elements have no static
        // type; the spelling stays auto for re-checks and .jaibite
        bool auto_spelled = false;
        bool auto_typed = false;
        // The checker saw the container's elements already hold the declared type: the binding converts nothing (a cell head
        // may land the element as it is)
        bool element_conversion_free = false;
        // for (auto [k, v] : m): the body's first two declarations bind the entry's halves (detail::stamp_map_entry_bindings)
        bool binds_map_entry = false;
        uint64_t binding_text_id = UINT64_MAX;   // the binding as written, "[k, v]", interned for its run-time text
        mutable const_site const_check;   // T& / var& over the elements of a const
        expression_ptr container;      // The container to iterate over
        statement_ptr body;            // Loop body

        range_for_stmt(const source_location& loc, type_info_ptr type, std::string_view varName,
                     bool ref, bool constRef, expression_ptr cont, statement_ptr b)
            : statement(loc, node_type::range_for_stmt), element_type(type), variable_name(varName),
              is_reference(ref), is_const(constRef), container(cont), body(b) {}

        range_for_stmt(const source_location& loc, type_info_ptr type, std::string_view varName, uint64_t varNameId,
                     bool ref, bool constRef, expression_ptr cont, statement_ptr b)
            : statement(loc, node_type::range_for_stmt), element_type(type), variable_name(varName), variable_name_id(varNameId),
              is_reference(ref), is_const(constRef), container(cont), body(b) {}    };

    // parallel_for (auto& x : arr) { ... } — the in-place fork-join loop. The body is parsed as a
    // ONE-PARAMETER FUNCTION BODY (own slot numbering: the loop variable is slot 0,
    // own local_count) so both backends execute it through the parallel region
    // machinery: workers bind the loop variable by reference to each owned element of
    // the evaluated container and mutate it in place. No output array, no merge.
    class parallel_for_stmt : public statement {
    public:
        parameter loop_param;            // declarator: name/type; is_reference must be true (admission errors otherwise)
        expression_ptr container;
        // The zip form `parallel_for (auto& x, auto& o : xs, outs)`: further loop variables (body
        // slots 1..m-1, declared in this order) walking further arrays of the SAME length in
        // lockstep - the emitter pattern's pre-allocated output written in place. Empty = plain.
        std::vector<parameter> extra_params;
        std::vector<expression_ptr> extra_containers;
        size_t arity() const noexcept { return 1 + extra_params.size(); }
        statement_ptr body;              // block_stmt, parsed inside the body's own function scope
        size_t local_count = 0;          // body frame size (loop variable = slot 0)
        // Locals of the frame the statement runs in that the body names (symbol, slot): the
        // barrier provisions them from that frame's slots, which no environment chain can see
        std::vector<std::pair<uint64_t, size_t>> frame_captures;
        // The lazily-synthesized body function both backends share. Its body pointer
        // keys admission + the region slot pools; hot reload reparses the file and
        // mints a fresh node, so the cache can never go stale.
        mutable std::shared_ptr<script_defined_function> body_fn_cache;
        // The worker's chunk loop for this statement (parallel_transform.cpp build_chunk_wrapper):
        // the body grafted into a counter loop (mode 1), or a call per element when the body
        // returns from inside a nested loop (mode 2). 0 = not built yet.
        mutable std::shared_ptr<class function_decl> chunk_decl_cache;
        mutable uint8_t chunk_mode = 0;

        parallel_for_stmt(const source_location& loc, parameter p, expression_ptr cont, statement_ptr b)
            : statement(loc, node_type::parallel_for_stmt), loop_param(std::move(p)),
              container(std::move(cont)), body(std::move(b)) {}

        // The counted form `parallel_for (int i = a; i < b; i++)`: the parser's range call (parser::counted_parallel_range)
        static constexpr std::string_view k_counted_range = "__jai_parallel_range";
        bool counted() const noexcept {
            if (!container || container->get_type() != node_type::call_expr) { return false; }
            const auto& callee = static_cast<const call_expr*>(container.get())->callee;
            return callee && callee->get_type() == node_type::identifier_expr && static_cast<const identifier_expr*>(callee.get())->name == k_counted_range;
        }
    };

    // Return statement
    class return_stmt : public statement {
    public:
        expression_ptr value;  // Can be null
        // Enclosing function declares a reference return (int& f() / -> auto&): the
        // value binds as a reference instead of evaluating to a copy. Stamped by the
        // parser's ref_escape_marker pass (innermost function, coroutines excluded).
        bool binds_reference = false;
        // the VM's graft: a return that jumps to the inlined body's join (synthesized by the compiler, never parsed or serialized)
        bool inline_exit = false;
        mutable uint8_t static_return_proof = proof_none;
        mutable const_site const_check;   // a const returned by reference
        // Interpreter memo of detail::classify_return_operand (by-value return
        // materialization); derived at first visit, never serialized. The vm compiler stamps
        // return_operand_parameter, which its return reads only for a value a copy constructor builds
        mutable uint8_t return_operand_class = 0;

        return_stmt(const source_location& loc, expression_ptr v = nullptr)
            : statement(loc, node_type::return_stmt), value(v) {}    };

    // Break statement
    class break_stmt : public statement {
    public:
        break_stmt(const source_location& loc) : statement(loc, node_type::break_stmt) {}    };

    // Continue statement
    class continue_stmt : public statement {
    public:
        continue_stmt(const source_location& loc) : statement(loc, node_type::continue_stmt) {}    };

    // Try-catch statement
    class try_stmt : public statement {
    public:
        statement_ptr try_block;
        std::string catch_var;  // Optional - variable name for exception message
        uint64_t catch_var_id = UINT64_MAX;    // Interned catch variable (UINT64_MAX = unnamed catch)
        size_t catch_slot_index = SIZE_MAX;    // Frame slot of the catch variable (SIZE_MAX = scope env)
        statement_ptr catch_block;

        try_stmt(const source_location& loc, statement_ptr try_blk, statement_ptr catch_blk, const std::string& var = "")
            : statement(loc, node_type::try_stmt), try_block(try_blk), catch_var(var), catch_block(catch_blk) {}    };

    // Switch statement
    class switch_stmt : public statement {
    public:
        expression_ptr condition;
        std::vector<case_stmt_ptr> cases;
        default_stmt_ptr default_case;  // Can be null

        switch_stmt(const source_location& loc, expression_ptr cond)
            : statement(loc, node_type::switch_stmt), condition(cond), default_case(nullptr) {}    };

    // Case statement (part of switch)
    class case_stmt : public statement {
    public:
        expression_ptr value;  // The case value to match
        std::vector<statement_ptr> body;
        bool has_fallthrough = false;  // Set if last statement is fallthrough
        // the checker's operand class of `condition == value` (a proof like binary_expr's, never serialized): the VM's case test
        mutable uint8_t static_operand_class = operand_class_unknown;

        case_stmt(const source_location& loc, expression_ptr val)
            : statement(loc, node_type::case_stmt), value(val) {}    };

    // Default statement (part of switch)
    class default_stmt : public statement {
    public:
        std::vector<statement_ptr> body;

        default_stmt(const source_location& loc)
            : statement(loc, node_type::default_stmt) {}    };

    // Fallthrough statement (only valid inside switch cases)
    class fallthrough_stmt : public statement {
    public:
        fallthrough_stmt(const source_location& loc)
            : statement(loc, node_type::fallthrough_stmt) {}    };
    
    // Base declaration node
    class declaration : public statement {
    public:
        declaration(const source_location& loc, node_type t) : statement(loc, t) {}
    };
    
    // The function a read-only by-value local declaration belongs to, as the local-copy proof needs it
    // (detail::prove_local_aliases): stamped on every candidate declaration of a plain function's body when the function
    // is minted; the proof re-runs when engine::definition_epoch() moved
    class block_stmt;
    struct local_alias_home {
        std::shared_ptr<const std::vector<parameter>> params;
        type_info_ptr return_type;
        std::weak_ptr<block_stmt> body;
        mutable uint64_t epoch = 0;
        mutable std::vector<uint8_t> verdicts;   // by variable_decl::alias_index; a graft's copy of a declaration reads its original's
    };

    // Variable declaration
    class variable_decl : public declaration {
    public:
        type_info_ptr type;
        std::string_view name;  // Points to symbolizer storage (permanent)
        uint64_t name_id = UINT64_MAX;  // Interned name for fast lookup (UINT64_MAX = not interned)
        expression_ptr initializer;  // Can be null
        bool is_static = false;      // For static class members
        // Declared with the `function` keyword (`function f = expr;`). The slot itself stays
        // auto (parse_type maps `function` to auto, the same as a function-typed parameter);
        // the checker reads this to hold the slot to callables and null
        bool function_spelling = false;
        // Declared without an initializer (`T x;`, `var x;`): the parser supplies the type's default, and a same-unit re-run
        // of a program-scope declaration keeps the existing value (detail::redeclaration_keeps_value)
        bool default_initialized = false;
        // `inline T x;` (program scope or a namespace): one variable every declaring unit shares - an existing one is
        // adopted and its initializer never runs, else the declaration creates it (environment::share; a plain declaration
        // of the name then raises, detail::inline_redefinition)
        bool is_inline = false;
        // `const T x = e;`: set once by its initializer, then only read; in a class body a class-level constant
        bool is_const = false;
        // A const T& holds the temporary it bound (C++ lifetime extension), so its death may destroy an instance
        bool owning_reference() const noexcept { return is_const && type && type->base_type == script_value_type::jai_reference_type; }
        mutable const_site const_check;   // T& / var& / shared_ptr<T> bound to a const
        // Declared `auto`: the unit's first proof pass types the slot by its initializer's
        // STATIC type, once - a known type keeps auto (the runtime locks to the value, which has that type), a var
        // or unknown static type makes `type` the var marker; the spelling stays auto for re-checks and .jaibite
        bool auto_spelled = false;
        bool auto_typed = false;
        // Parser escape analysis: the variable may be bound by reference, so its
        // declaration boxes the value into a cell (storage IS the cell from birth)
        bool ref_escaping = false;
        // Parser resolution: `T& name = x;` over a value declared T already (int& r = n with int n): the storage applies T's
        // table itself, so the declared type adds nothing at the stores or the binding
        bool declared_ref_storage_enforced = false;
        // Checker proof (`T& name = e;`): e's static type is certain and a T (detail::reference_binding_admits cannot refuse it),
        // so the binding skips the run-time admission
        mutable bool reference_admission_proven = false;
        size_t slot_index = SIZE_MAX; // Slot index for fast local access (SIZE_MAX = global/class member)
        // Read-only by-value local elision (runtime caches, never serialized): the declaration's function and its slot in
        // the home's verdicts (true = the declaration may bind the source's object instead of a clone)
        mutable std::shared_ptr<const local_alias_home> alias_home;
        mutable uint32_t alias_index = 0;

        // Construction-time classification for both backends' decl fast path
        // (exec_decl_var / visit_variable_decl): a decl carrying BOTH bits stores a
        // scalar or string payload whose storage index already matches the declared type
        // straight into its slot - conversion/clone/homogeneity are provably identity there
        // (a string copies as its shared node, exactly what clone_for_assignment does).
        // 0 = always the full path (weak_ptr/shared_ptr/reference declared types).
        static constexpr uint8_t decl_fast_plain = 1 << 0;  // no weak_ptr/shared_ptr/reference init rules apply
        static constexpr uint8_t decl_fast_scalar = 1 << 1; // auto/var/int/float/bool/char/string: matching payloads need no conversion
        static constexpr uint8_t decl_fast_slot_store = decl_fast_plain | decl_fast_scalar;
        uint8_t decl_fast_flags = 0;
        // the VM's synthesized declarations: the typed-slot rejection spells the site the declaration stands for (a graft's
        // result converts with the callee's return text); the parser leaves the declaration site
        static constexpr uint8_t site_declaration = 0, site_parameter = 1, site_return = 2;
        uint8_t site_kind = site_declaration;
        uint8_t graft_source_class = 0;   // a graft's parameter: its argument's C++ category (detail::classify_source; 0 none), which decides the elision
        std::string_view site_owner;
        // a graft's by-value parameter (site_parameter): its inline site, its parameter index and the private temp its argument
        // was evaluated into, so the bind takes the call's read-only alias and prvalue elision (vm bind_graft_parameter)
        uint32_t graft_site = UINT32_MAX;
        uint32_t graft_param = UINT32_MAX;
        size_t graft_temp_slot = SIZE_MAX;

        static uint8_t classify_decl_fast(const type_info_ptr& t) {
            if (!t) {
                return decl_fast_slot_store;   // auto: infer-then-enforce, matching payloads pass untouched
            }
            switch (t->base_type) {
                case script_value_type::jai_weak_ptr_type:
                case script_value_type::jai_shared_ptr_type:
                case script_value_type::jai_reference_type:
                    return 0;
                case script_value_type::jai_any_type:
                case script_value_type::jai_int_type:
                case script_value_type::jai_float_type:
                case script_value_type::jai_bool_type:
                case script_value_type::jai_char_type:
                case script_value_type::jai_string_type:
                    return decl_fast_slot_store;
                default:
                    return decl_fast_plain;   // container/object: payload needs full enforcement
            }
        }

        // The fast path's admission, shared by every decl landing on both backends: a raw
        // int/float/bool/char/string payload (never a bound or reference storage index) lands
        // as-is when the declared type is var/auto or names that payload's type
        static bool identity_payload(size_t storage_index, const type_info* declared) noexcept {
            const script_value_type bt = declared ? declared->base_type : script_value_type::jai_any_type;
            switch (storage_index) {
                case script_value::TYPEID_INT: return bt == script_value_type::jai_any_type || bt == script_value_type::jai_int_type;
                case script_value::TYPEID_FLOAT: return bt == script_value_type::jai_any_type || bt == script_value_type::jai_float_type;
                case script_value::TYPEID_BOOL: return bt == script_value_type::jai_any_type || bt == script_value_type::jai_bool_type;
                case script_value::TYPEID_CHAR: return bt == script_value_type::jai_any_type || bt == script_value_type::jai_char_type;
                case script_value::TYPEID_STRING: return bt == script_value_type::jai_any_type || bt == script_value_type::jai_string_type;
                default: return false;
            }
        }
        // The one implicit scalar conversion a declaration performs with no user code and no
        // failure mode: a raw int into a float slot widens (the conversion table's int -> float
        // row); both fast paths land it as the widened value stamped with the declared type
        static bool widening_payload(size_t storage_index, const type_info* declared) noexcept {
            return storage_index == script_value::TYPEID_INT && declared && declared->base_type == script_value_type::jai_float_type;
        }

        variable_decl(const source_location& loc, type_info_ptr t, std::string_view n, expression_ptr init = nullptr)
            : declaration(loc, node_type::variable_decl), type(t), name(n), initializer(init), is_static(false), slot_index(SIZE_MAX), decl_fast_flags(classify_decl_fast(t)) {}

        variable_decl(const source_location& loc, type_info_ptr t, std::string_view n, uint64_t nid, expression_ptr init = nullptr)
            : declaration(loc, node_type::variable_decl), type(t), name(n), name_id(nid), initializer(init), is_static(false), slot_index(SIZE_MAX), decl_fast_flags(classify_decl_fast(t)) {}
    };
    
    // Constructor initialization entry (for : super(args), : this(args))
    struct constructor_initializer {
        std::string target; // "super", "this" or the name of the base it constructs
        std::vector<expression_ptr> arguments;
        std::vector<uint8_t> arg_captures;

        constructor_initializer(const std::string& t, std::vector<expression_ptr> args)
            : target(t), arguments(std::move(args)) {}
    };

    // Function declaration
    class function_decl : public declaration, public std::enable_shared_from_this<function_decl> {
    public:
        std::string_view name;  // Points to symbolizer storage (permanent)
        uint64_t name_id = UINT64_MAX;  // Interned ID for the function name
        std::vector<parameter> parameters;
        type_info_ptr return_type;
        std::shared_ptr<block_stmt> body;
        std::vector<constructor_initializer> initializers; // For constructor initialization lists
        bool is_override = false; // For override keyword in derived classes
        bool is_static = false;   // For static methods
        bool is_coroutine = false;
        bool is_nodiscard = false;   // [[nodiscard]]: a call whose result an expression statement drops fails the load
        size_t local_count = 0;   // Total slots needed (params + locals) for stack allocation

        // Enclosing-frame slot locals a nested COROUTINE body references, as
        // (symbol_id, outer slot). Interpreter-owned: built on the first declaration
        // execution, which also patches those body identifiers' slot_index to SIZE_MAX
        // (mirrors lambda_expr::outer_slot_plan); the vm copies a completed plan or
        // derives its own at compile time when the interpreter has not built one.
        mutable std::vector<std::pair<uint64_t, size_t>> outer_slot_plan;
        mutable bool outer_slot_plan_built = false;

        // Shared immutable parameter storage, minted once per node: every function value
        // created from this declaration shares it instead of deep-copying the vector
        // (built lazily on the first mint - single-threaded; parallel bodies reject
        // nested declarations). Coroutine declarations additionally cache their owning
        // function_decl copy (the factory needs shared ownership of an AST the visitor
        // only sees by raw pointer).
        mutable std::shared_ptr<const std::vector<parameter>> shared_params_cache;
        mutable std::shared_ptr<function_decl> coroutine_mint_cache;
        // Non-coroutine declarations always mint with closure_env = nullptr, so the ONE
        // script_defined_function can be reused for every execution of this node under one
        // unit (the cached mint's unit id is the reuse condition; the coroutine copy's too)
        mutable std::shared_ptr<script_defined_function> mint_cache;
        // The unit whose code ran this declaration (methods, constructors, coroutines and namespace
        // functions run under it)
        mutable unit_stamp unit{};
        mutable uint64_t context = 0;   // a namespace function's declaring host entry (engine::host_entry_generation)
        // Return-epilogue classification memo (vm method frames; classify_return_conv
        // derives it from return_type alone, engine-independent). 0 = uncomputed. Same
        // benign-idempotent write pattern as script_defined_function::backend_return_conv.
        mutable uint8_t backend_return_conv = 0;
        // the vm backend that keeps this declaration alive until its retire point (vm_backend::retain_method_decl): its frames name it raw
        mutable const void* vm_retainer = nullptr;
        mutable uint8_t vm_lean_bind = 0;   // vm: 1 = every parameter a by-value slot in order an in-place argument may already be, 2 = not
        mutable uint64_t vm_lean_kinds = 0;   // vm: the lean bind as storage kinds (vm_backend.cpp lean_kinds_of; 0 = not yet computed)
        // a lambda's graft declaration without a declared return type: the kind its returns certainly yield (1 int, 2 float, 3 bool)
        mutable uint8_t graft_return_kind = 0;

        function_decl(const source_location& loc, std::string_view n, uint64_t id = UINT64_MAX)
            : declaration(loc, node_type::function_decl), name(n), name_id(id), is_override(false), is_static(false), is_coroutine(false), local_count(0) {}
    };

    // Class declaration
    class class_decl : public declaration {
    public:
        enum member_visibility {
            Public,
            Private,
            Protected
        };

        struct member {
            member_visibility visibility;
            declaration_ptr declaration;
        };

        std::string_view name;  // Points to symbolizer storage (permanent)
        uint64_t name_id = UINT64_MAX;  // Interned name for fast comparison (UINT64_MAX = not interned)
        std::vector<std::string_view> base_classes;  // Points to symbolizer storage (permanent)
        std::vector<member> members;
        bool nocopy = false;  // 'nocopy class': by-value copies of instances are errors (a deleted C++ copy constructor)

        class_decl(const source_location& loc, std::string_view n, uint64_t nid = UINT64_MAX)
            : declaration(loc, node_type::class_decl), name(n), name_id(nid) {}    };

    // Namespace declaration
    class namespace_decl : public declaration {
    public:
        std::string_view name;  // Points to symbolizer storage (permanent)
        uint64_t name_id = UINT64_MAX;  // Interned name for fast comparison (UINT64_MAX = not interned)
        std::vector<declaration_ptr> declarations;  // Can hold functions, classes, variables

        namespace_decl(const source_location& loc, std::string_view n, uint64_t nid = UINT64_MAX)
            : declaration(loc, node_type::namespace_decl), name(n), name_id(nid) {}    };

    // expression as declaration (for top-level expressions)
    class expression_decl : public declaration {
    public:
        expression_ptr expression;
        bool implicit_return;  // True if this expression should implicitly return its value

        expression_decl(const source_location& loc, expression_ptr expr, bool implicit_ret = false)
            : declaration(loc, node_type::expression_decl), expression(expr), implicit_return(implicit_ret) { mark_result_discarded(expression.get()); }    };

    // statement as declaration (for top-level statements in scripting context)
    class statement_decl : public declaration {
    public:
        statement_ptr statement;

        statement_decl(const source_location& loc, statement_ptr stmt)
            : declaration(loc, node_type::statement_decl), statement(stmt) {}    };

    // include declaration - always parses the file
    class include_decl : public declaration {
    public:
        std::string path;  // The file path to include (for literal syntax)
        expression_ptr path_expr;  // The expression that evaluates to path (for function syntax)

        // Constructor for literal syntax: include "file.jai"
        include_decl(const source_location& loc, const std::string& p)
            : declaration(loc, node_type::include_decl), path(p), path_expr(nullptr) {}

        // Constructor for expression syntax: include(expr)
        include_decl(const source_location& loc, expression_ptr expr)
            : declaration(loc, node_type::include_decl), path(), path_expr(expr) {}    };

    // import declaration - intelligently manages file parsing
    class import_decl : public declaration {
    public:
        std::string path;  // The file path to import (for literal syntax)
        expression_ptr path_expr;  // The expression that evaluates to path (for function syntax)

        // Constructor for literal syntax: import "file.jai"
        import_decl(const source_location& loc, const std::string& p)
            : declaration(loc, node_type::import_decl), path(p), path_expr(nullptr) {}

        // Constructor for expression syntax: import(expr)
        import_decl(const source_location& loc, expression_ptr expr)
            : declaration(loc, node_type::import_decl), path(), path_expr(expr) {}

        mutable import_site site;
    };

    // Enum declaration
    class enum_decl : public declaration {
    public:
        std::string_view name;
        uint64_t name_id;
        std::vector<std::pair<std::string_view, uint64_t>> values;  // name, symbol_id pairs
        std::vector<int64_t> numbers;   // each value's integer: its initializer, else the one before it plus one (the first 0)

        enum_decl(const source_location& loc, std::string_view n, uint64_t nid)
            : declaration(loc, node_type::enum_decl), name(n), name_id(nid) {}
    };

    // Destructuring declaration: auto [x, y, z] = [1, 2, 3];
    class destructuring_decl : public declaration {
    public:
        std::vector<std::pair<std::string_view, uint64_t>> names;
        std::vector<size_t> slot_indices;
        // Parser escape analysis, per name (empty = no name escapes)
        std::vector<bool> ref_escaping;
        expression_ptr initializer;
        bool is_const = false;   // const auto [a, b] = e;

        destructuring_decl(const source_location& loc, expression_ptr init)
            : declaration(loc, node_type::destructuring_decl), initializer(std::move(init)) {}
    };

    // extern declaration: a name defined outside the program (host-registered, another unit, a later dynamic
    // definition). The checker treats it as a declared global; both backends only record the name, so a use that
    // finds nothing defined says the extern was never defined (environment::undefined_text).
    class extern_decl : public declaration {
    public:
        type_info_ptr type;   // null for `extern name;` (var) and the function form
        bool is_function;     // extern function name; - a callable of unknown signature
        bool is_const = false;   // extern const T name;: this unit never writes it
        std::string_view name;   // qualified by the enclosing namespaces: `namespace game { extern a::b; }` names "game::a::b"
        uint64_t name_id;
        uint32_t written_at = 0;   // where the spelling the declaration wrote begins in name ("a::b" above)

        std::string_view written() const { return name.substr(written_at); }
        bool qualified() const { return name.find("::") != std::string_view::npos; }

        extern_decl(const source_location& loc, type_info_ptr t, bool fn, std::string_view n, uint64_t nid)
            : declaration(loc, node_type::extern_decl), type(std::move(t)), is_function(fn), name(n), name_id(nid) {}
    };

    inline void mark_result_discarded(expression* e) {
        while (e) {
            if (e->get_type() == node_type::ternary_expr) {
                auto* t = static_cast<ternary_expr*>(e);
                mark_result_discarded(t->then_expression.get());
                e = t->else_expression.get();
                continue;
            }
            if (e->get_type() != node_type::call_expr) { return; }
            expression* callee = static_cast<call_expr*>(e)->callee.get();
            if (callee && callee->get_type() == node_type::member_expr) {
                static_cast<member_expr*>(callee)->call_result_discarded = true;
                return;
            }
            if (!callee || callee->get_type() != node_type::lambda_expr) { return; }
            const auto* body = static_cast<const lambda_expr*>(callee)->body.get();
            if (!body || body->get_type() != node_type::block_stmt) { return; }
            const auto& decls = static_cast<const block_stmt*>(body)->declarations;
            if (decls.size() != 1 || decls[0]->get_type() != node_type::statement_decl) { return; }
            const statement* only = static_cast<const statement_decl*>(decls[0].get())->statement.get();
            if (!only || only->get_type() != node_type::return_stmt) { return; }
            e = static_cast<const return_stmt*>(only)->value.get();
        }
    }

    // Helper function to check if an expression is GUARANTEED to return a boolean
    // This allows skipping is_truthy() type dispatch and using unchecked_as_bool() directly
    [[nodiscard]] inline bool expression_returns_bool(const expression* expr) noexcept {
        switch (expr->get_type()) {
            case node_type::binary_expr:
                return static_cast<const binary_expr*>(expr)->returns_bool();
            case node_type::unary_expr:
                return static_cast<const unary_expr*>(expr)->returns_bool();
            case node_type::literal_expr:
                return static_cast<const literal_expr*>(expr)->value.is_bool();
            default:
                return false;
        }
    }

} // namespace jai

#endif // __JAISCRIPT_DETAIL_AST_HPP__
