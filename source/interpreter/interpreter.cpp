#include <jaiscript/jaiscript.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/interpreter.hpp>
#include <jaiscript/detail/null_member_diagnostic.hpp>
#include <jaiscript/detail/uniquely_held.hpp>
#include <jaiscript/detail/release_machine.hpp>
#include <jaiscript/detail/enum_value.hpp>
#include <jaiscript/detail/inline_variable.hpp>
#include <jaiscript/stdlib/containers.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/core/class_registry.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/integer_ops.hpp>   // kCheckedOverflow + jai::ints overflow policy
#include <jaiscript/detail/type_proofs.hpp>
#include <jaiscript/detail/body_walker.hpp>   // nested-coroutine capture analysis
#include <jaiscript/detail/math_intrinsics.hpp>   // math:: language intrinsics (shared kernel)
#include <jaiscript/detail/aggregate_lowering.hpp>
#include <jaiscript/detail/redeclaration.hpp>
#include <jaiscript/detail/math_component_store.hpp>   // math-value component stores (vm parity)
#include <jaiscript/detail/ref_lvalue.hpp>    // shared ref-param lvalue binding (vm parity)
#include <jaiscript/detail/method_value.hpp>
#include <jaiscript/detail/in_place_result.hpp>
#include <jaiscript/detail/string_mutator_receiver.hpp>
#include <jaiscript/detail/string_element.hpp>
#include <jaiscript/detail/import_paths.hpp>
#include <jaiscript/detail/capture_ladder.hpp> // shared capture-source ladder (vm parity)
#include <jaiscript/detail/cast_kernel.hpp>   // cast<T>/cast<T&>/cast<shared_ptr<T>> kernel (vm parity)
#include <jaiscript/detail/const_rules.hpp>   // const guards (vm parity)
#include <jaiscript/detail/aggregate_init.hpp>
#include <jaiscript/detail/static_store.hpp>  // class-qualified static stores (vm parity)
#include <jaiscript/detail/namespace_scope.hpp>  // a namespace's variable cells and the ns::x store target (shared with the vm)
#include <jaiscript/detail/namespace_function.hpp>   // a namespace function's cell and body environment (shared with the vm)
#include <jaiscript/detail/transient_read.hpp>
#include <jaiscript/detail/binary_handlers.hpp>
#include <jaiscript/detail/arg_capture.hpp>   // call arguments take their value when evaluated (vm parity)
#include <jaiscript/detail/construction_kernel.hpp>
#include <jaiscript/detail/function_overloads.hpp>
#include <jaiscript/detail/char_promotion.hpp>   // char operands promote to int64 0..255
#include <jaiscript/detail/utf8_operands.hpp>
#include <jaiscript/detail/utf8_text.hpp>
#include <jaiscript/detail/container_enforce.hpp> // typed-container boundary kernel (vm parity)
#include <jaiscript/detail/parallel_transform.hpp>   // parallel_borrow_subscript_read (captured reads)
#include <jaiscript/detail/ast_serializer.hpp>   // structural_node_key (hot-reload identity)
#include <jaiscript/detail/typed_slot.hpp>   // the one typed-slot conversion table (vm parity)
#include <jaiscript/detail/range_for_diagnostic.hpp>   // range-for texts (vm parity)
#include <jaiscript/detail/map_entry_binding.hpp>
#include <jaiscript/detail/float_text.hpp>
#include <jaiscript/detail/regex_library.hpp>   // regex(literal) compiled at load (vm parity)
#include <jaiscript/debug/controller.hpp>     // step-debugger statement hook
#include <stdexcept>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <filesystem>
#include <jaiscript/detail/string_methods_more.hpp>
#include <jaiscript/detail/utf8_methods.hpp>
#include <jaiscript/detail/binary_data.hpp>

namespace jai {

// a dot read of a plain map's key (only an enum's value table reads its keys with a dot): KEEP BYTE-PARALLEL with the vm
static JAI_NOINLINE checked_result<void> map_dot_key_error(uint64_t member_id) {
    return checked_result<void>(make_error_code(runtime_error_code::member_not_found), "map has no member '{0}'; read the key as [\"{1}\"]", member_id, member_id);
}

namespace {
    using detail::capture_call_argument;

    // Local helper: error result for an overflowing integer op in the inlined
    // fast paths below (mirrors interpreter_dispatch.cpp's handlers).
    inline checked_result<void> int_overflow_v(const char* msg) {
        return checked_result<void>(make_error_code(runtime_error_code::invalid_numeric_operand), msg);
    }

    // Decl-ref (auto&/T& x = y) aliasing of a value that is already a reference: SHARE
    // the holder, constraint included. Element/field/map-entry refs
    // keep their bind-time type constraint through a ref decl — auto& over an
    // array<int> element enforces int on store exactly like direct element assignment
    // (reverses the e43f8a6f constraint-dropping pin). Sharing also keeps container
    // re-resolution (realloc/shrink safe). KEEP BYTE-PARALLEL with vm_decl_ref_alias.
    checked_result<script_value> decl_ref_alias(const script_value& source, engine*) {
        return source;
    }

    // Box-in-place + share: non-ref storage moves into a cell and the storage becomes
    // the handle (all read/store paths handle the boxed form); ref storage just shares
    // its holder. The one way env-variable storage is ever bound by reference — decl
    // refs, [&] captures, ref returns. KEEP BYTE-PARALLEL with vm_backend::share_env_ref
    // (the vm twin also demotes active counted-for fast states).
    script_value share_boxed_env_storage(script_value& storage, engine* eng) {
        if (!storage.is_reference()) {
            script_value inner = std::move(storage);
            storage = script_value::make_cell_reference(std::move(inner), eng);
        }
        return script_value(storage);
    }

    // A handle's backend_state is always interpreter-owned here: an engine's backend
    // is fixed before its first execute, so no other backend can have populated it.
    interpreter_coroutine_state& coroutine_state(coroutine_handle& handle, engine* a_engine) {
        auto* state = static_cast<interpreter_coroutine_state*>(handle.backend_state());
        if (!state) {
            auto owned = std::make_unique<interpreter_coroutine_state>();
            owned->eng = a_engine;
            state = owned.get();
            handle.set_backend_state(std::move(owned));
        }
        return *state;
    }

    // Replay parking for the generic call lane: the callee ahead of the arguments
    std::vector<script_value> parked_with_head(const script_value& head, std::vector<script_value> rest) {
        rest.insert(rest.begin(), head);
        return rest;
    }
}

// An operand site inside an expression visitor: on the yield signal, park what this node
// had produced (evaluation order) and the suspended child's ordinal, then keep unwinding
#define JAISCRIPT_TRY_OPERAND(expr, node, phase, ordinal, partial) \
    do { \
        auto __result = (expr); \
        if (!__result) [[unlikely]] { \
            if (is_yield_signal(__result)) park_operands(node, phase, ordinal, partial); \
            return __result.error_value(); \
        } \
    } while(0)

// A statement's one expression: the signal makes the statement the replay root
#define JAISCRIPT_TRY_ROOT(stmt, root) \
    do { \
        auto __result = eval_root<Replaying>(root); \
        if (!__result) [[unlikely]] { \
            if (is_yield_signal(__result)) { note_statement_yield(stmt, root); return {}; } \
            return __result.error_value(); \
        } \
    } while(0)

namespace detail {
// Hands a read chain's hold (detail::held_chain_root) to the operand about to be evaluated, and takes it back however the evaluation ends
struct chain_root_handoff {
    script_value*& hold;
    chain_root_handoff(script_value*& a_hold, script_value* a_to) : hold(a_hold) { hold = a_to; }
    ~chain_root_handoff() { hold = nullptr; }
};
} // namespace detail

void interpreter::init_builtin_methods() {
    builtin_method_registries registries;
    init_builtin_method_registries(string_symbolizer_, registries);
    array_methods_ = std::move(registries.array_methods);
    map_methods_ = std::move(registries.map_methods);
    string_methods_ = std::move(registries.string_methods);
    utf8_methods_ = std::move(registries.utf8_methods);
    string_mutator_ids_ = std::move(registries.string_mutator_ids);
    in_place_ = std::move(registries.in_place);
    weak_ptr_methods_ = std::move(registries.weak_ptr_methods);
    shared_ptr_methods_ = std::move(registries.shared_ptr_methods);
}

// Helper to create a bound method - binds 'this' as the first argument
script_value interpreter::create_bound_method(const script_value& this_obj, const script_value& method) {
    return make_bound_method(this_obj, method);
}

bool interpreter::is_lvalue_expression(expression* e) const {
    return is_lvalue_shaped_expr(e);
}

script_value interpreter::member_target::method(uint64_t id) const {
    if (class_def) {
        return class_def->get_method(id, false);
    }
    return script_value::make_invalid(nullptr);
}

script_value interpreter::member_target::user_accessor(uint64_t id) const {
    if (class_def) {
        return class_def->find_user_accessor(id);
    }
    return script_value::make_invalid(nullptr);
}

bool interpreter::member_target::has_field(uint64_t id) const {
    return instance && instance->has_field(id);
}

const script_value& interpreter::member_target::get_field(uint64_t id) const {
    return instance->get_field(id);
}

std::string_view interpreter::member_target::class_name() const {
    if (class_def) {
        return class_def->get_name();
    }
    if (instance) {
        return instance->get_class_name();
    }
    static const std::string no_name;
    return no_name;
}

interpreter::member_target interpreter::resolve_member_target(const script_value& objectValue) const {
    member_target target;
    auto holder = objectValue.get_object_holder();
    if (!holder) {
        return target;
    }
    if (holder->is_instance()) {
        target.instance = holder->instance_handle();
        if (target.instance) {
            target.class_def = target.instance->get_class_definition();
            // A plain wrapper of an extended object IS the object's layer: the access reads the layer through the object's slot (a handle is the wrapper itself, so it
            // cannot be re-pointed); getters, setters, bound methods and the vm's caches see the layer the same way.
            if (target.class_def && target.class_def->wrapped_object_has_layer(*target.instance)) {
                if (auto hooks = target.class_def->slot_hooks()) {
                    auto layer = hooks->read(target.instance->cpp_object_pointer());
                    if (layer && layer != target.instance && layer->get_engine() == engine_) {
                        target.instance = std::move(layer);
                        target.class_def = target.instance->get_class_definition();
                    }
                }
            }
        }
    } else if (engine_) {
        target.engine_def = holder->type_id() != UINT64_MAX
            ? engine_->get_class_definition(holder->type_id())
            : engine_->get_class_definition(std::string(holder->type_name()));
        target.class_def = target.engine_def.get();
    }
    return target;
}

// Helper for is_truthy() to check for to_bool() method on objects
bool interpreter::object_to_bool_via_method(const script_value& value) {
    // KEEP BYTE-PARALLEL with vm_backend::object_to_bool_via_method (class to_bool memo) and
    // the typed-slot table's bool row (detail::convert_typed_slot_script_rows_impl)
    if (const auto* h = value.get_storage().get_if<script_value::TYPEID_OBJECT>(); h && *h) {
        auto* oh = h->get();
        if (oh->is_instance()) {
            if (auto* cd = oh->unchecked_instance()->get_class_definition();
                cd && !cd->may_define_to_bool() && !cd->wrapped_object_has_layer(*oh->unchecked_instance())) {
                return true;
            }
        }
    }
    auto target = resolve_member_target(value);
    if (!target) {
        return true;  // Not a class object - treat as truthy
    }

    auto method_id = to_bool_id_;
    if (parallel_worker_ && !worker_operators_pinned(value, { method_id })) [[unlikely]] {
        if (!is_unwinding_ && !limits_->terminal_error) {
            const std::string message(worker_operator_refusal(value, { method_id }));
            active_exception_value_ = make_value(message);
            current_exception_ = script_exception(message);
            is_unwinding_ = true;
        }
        return false;
    }
    auto method_val = operator_value(target, method_id);
    if (method_val.is_null() || method_val.is_invalid() || !method_val.is_function()) {
        return true;  // No to_bool() method - objects are truthy by default
    }

    auto result = operator_call(value, method_val, method_id, {});
    if (!result) {
        // truthiness has no error channel: the runtime error travels as the script exception its text would throw (a
        // terminal error keeps its latch) (KEEP BYTE-PARALLEL with vm_backend::object_to_bool_via_method)
        if (!is_unwinding_ && !limits_->terminal_error) {
            const std::string message = format_error(result, *string_symbolizer_);
            active_exception_value_ = make_value(message);
            current_exception_ = script_exception(message);
            is_unwinding_ = true;
        }
        return false;
    }
    if (result.value().is_bool()) {
        return result.value().unchecked_as_bool();
    }
    return true;  // Method didn't return a valid bool - treat as truthy
}

// Helper for handle_equal() to check for operator== or equals() method on objects
// Returns: nullopt if no custom equality method, true/false if method found and returned valid result
bool interpreter::object_defines_custom_equality(const script_value& v) const {
    const class_definition* cd = detail::operand_instance_class(v);
    return cd && (cd->defines_method(eq_method_id_) || cd->defines_method(ops_.spaceship));
}

// The language's <: honor the global override before builtin/class ordering.
checked_result<script_value> interpreter::array_transform(uint64_t a_member, script_value& a_self, const std::vector<script_value>& a_args) {
	auto context = builtin_ctx();
	context.copy_array_result = true;
	auto found = array_methods_.find(a_member);
	if (found == array_methods_.end()) { return checked_result<script_value>(make_error_code(runtime_error_code::member_not_found), "Unknown array transform"); }
	const auto name = string_symbolizer_->get_string(a_member);
	if (name == "sort" || name == "reverse") {
		script_value result = a_self.clone();
		context.copy_array_result = false;
		auto answer = found->second(context, result, a_args);
		if (!answer) { return answer.error_value(); }
		return result;
	}
	return found->second(context, a_self, a_args);
}

checked_result<script_value> interpreter::map_transform(uint64_t a_member, script_value& a_self, const std::vector<script_value>& a_args) {
	auto context = builtin_ctx();
	context.copy_array_result = true;
	auto found = map_methods_.find(a_member);
	if (found == map_methods_.end()) { return checked_result<script_value>(make_error_code(runtime_error_code::member_not_found), "Unknown map transform"); }
	return found->second(context, a_self, a_args);
}

checked_result<bool> interpreter::language_less(const script_value& a_left, const script_value& a_right) {
	const auto& left = a_left.deref();
	const auto& right = a_right.deref();
	if (operator_table_) {
		if (const script_value* function = operator_table_->entry(detail::op_slot::less)) {
			auto result = function->as_function()({left, right});
			if (!result) { return result.error_value(); }
			return is_truthy(result.value());
		}
	}
	auto result = binary_operator(token_type::less, left, right);
	if (!result) { return result.error_value(); }
	return is_truthy(result.value());
}

// The language's ==: a global operator== first, then the value's own (a class operator==, memberwise, numeric across kinds)
checked_result<bool> interpreter::language_equal(const script_value& left, const script_value& right) {
    const script_value& l = left.deref();
    const script_value& r = right.deref();
    if (operator_table_ && operator_table_->any()) {
        if (const script_value* opFunc = operator_table_->entry(detail::binary_op_slot(token_type::equal_equal))) {
            auto result = opFunc->as_function()({l, r});
            if (!result) { return result.error_value(); }
            return is_truthy(result.value());
        }
    }
    auto equal = binary_operator(token_type::equal_equal, l, r);
    if (!equal) { return equal.error_value(); }
    return is_truthy(equal.value());
}

interpreter::interpreter()
    : ownedSymbolizer_(std::make_unique<string_symbolizer>()),
      string_symbolizer_(ownedSymbolizer_.get()),
      environment_(std::make_shared<environment>(string_symbolizer_)),
      hasReturnValue_(false),
      current_method_this_(std::monostate{}, nullptr) {
    initialize();
    call_stack_.reserve(128);    // Avoid reallocation during deep recursion (e.g., Fibonacci)
}

interpreter::interpreter(string_symbolizer* external_symbolizer)
    : ownedSymbolizer_(nullptr),
      string_symbolizer_(external_symbolizer),
      environment_(std::make_shared<environment>(string_symbolizer_)),
      hasReturnValue_(false),
      current_method_this_(std::monostate{}, nullptr) {
    initialize();
}

interpreter::interpreter(string_symbolizer* external_symbolizer, std::shared_ptr<environment> global_env)
    : ownedSymbolizer_(nullptr),
      string_symbolizer_(external_symbolizer),
      environment_(global_env),
      hasReturnValue_(false),
      current_method_this_(std::monostate{}, nullptr) {
    initialize();
}

// Every constructor's shared setup: the cached symbol ids and the builtin registries
void interpreter::initialize() {
    env_symbolizer_ = string_symbolizer_;

    // Initialize cached type IDs for fast object type comparison
    class_definition_type_id_ = string_symbolizer_->intern("class_definition");
    weak_ptr_holder_type_id_ = string_symbolizer_->intern("weak_ptr_holder");
    shared_ptr_holder_type_id_ = string_symbolizer_->intern("shared_ptr_holder");
    weak_from_this_id_ = string_symbolizer_->intern("weak_from_this");
    shared_from_this_id_ = string_symbolizer_->intern("shared_from_this");
    coroutine_handle_type_id_ = string_symbolizer_->intern("coroutine_handle");
    next_id_ = string_symbolizer_->intern("next");
    handle_reset_id_ = string_symbolizer_->intern("reset");
    done_id_ = string_symbolizer_->intern("done");
    advance_id_ = string_symbolizer_->intern("advance");
    result_id_ = string_symbolizer_->intern("result");
    same_as_id_ = string_symbolizer_->intern("same_as");
    eq_method_id_ = string_symbolizer_->intern("==");
    to_bool_id_ = string_symbolizer_->intern("to_bool");
    to_string_id_ = string_symbolizer_->intern("to_string");
    base_type_ids_.intern(*string_symbolizer_);
    arity_count_ids_.intern(*string_symbolizer_);

    ops_.intern(*string_symbolizer_);
    subscript_op_id_ = string_symbolizer_->intern("[]");
    assign_operator_id_ = string_symbolizer_->intern("=");

    // Initialize built-in method registries with interned method names
    init_builtin_methods();

    // Initialize binary operator dispatch table
}

void interpreter::add_global(const std::string& name, const script_value& value) {
    environment_->define(name, value);
}

std::shared_ptr<environment> interpreter::get_global_environment() const {
    // Get the global environment directly from the engine
    // This avoids issues with closures/methods capturing stale environment references
    // from different execute() calls that don't chain to the same root
    if (auto eng = engine_) {
        return eng->get_global_environment();
    }
    // Fallback: walk up the parent chain (shouldn't happen if engine is alive)
    auto global_env = environment_;
    while (global_env && global_env->get_parent()) {
        global_env = global_env->get_parent();
    }
    return global_env ? global_env : environment_;
}

// Debug sync point: execute entry, every 1024 budget ticks, a sleep and an armed session's
// pending change, always on the script thread. Mirrors the transport-writable
// controller state into the plain per-statement gate (debug_hook_) and the controller's hot
// cache; a session that just ended gets a fresh budget deadline (the armed one is long past
// after any pause).
void interpreter::sync_debug_hook() {
    auto* dbg = debugger_.load(std::memory_order_acquire);
    debug::controller* next = (dbg && dbg->sync_hot_state()) ? dbg : nullptr;
    if (debug_hook_ && !next) { arm_execution_deadline(); }
    debug_hook_ = next;
}

void interpreter::begin_host_call() {
    if (engine_) { limits_ = &engine_->execution_limits(); }
    arm_execution_deadline();
    limits_->reset_for_execute();
}

void interpreter::prepare_for_execution() {
    sync_debug_hook();
    arm_execution_deadline();

    // Share the engine's limit state: reentrant executes see the outer run's terminal
    // latch, so a terminal error crosses the reentrant boundary uncaught
    if (auto eng = engine_) {
        limits_ = &eng->execution_limits();
    }

    // Clear exception state even when re-entered: the engine's catch path relies on
    // this scrub to strip a nested run's uncaught throw before the suspended outer
    // run resumes (vm backend ordering — clears before its reentrancy early-return).
    current_exception_.reset();
    is_unwinding_ = false; trace_captured_ = false;
    active_exception_value_.reset();  // No need to create a value here either
    current_catch_var_id_ = 0;

    // a reentrant execute (an include or import, a host callback) keeps the outer run's value stack, return value and
    // environment: execute isolates the nested program itself (the vm's frames_ guard)
    if (is_executing()) {
        return;
    }

    // Terminal latch (and memory allowance) reset only at a NON-reentrant entry: a
    // nested execute must not grant the outer run's unwinding error a second life
    limits_->reset_for_execute();

    // Clear execution state
    valueStack_.clear();
    returnValue_.reset();  // No need to create a value - value_or() will create it if needed
    hasReturnValue_ = false;

    // Clear coroutine state — unless re-entered from inside a RUNNING coroutine
    // (include/import, or a host callback calling execute() mid-resume): clearing
    // then would sever the live coroutine's yield machinery and its next `yield`
    // would fail while the caller received the previous value as a bogus success.
    if (!active_coroutine_ || active_coroutine_->get_status() != coroutine_handle::status::running) {
        hasYieldRequest_ = false;
        active_coroutine_ = nullptr;
    }

    // a run starts in the engine's global environment (an unwound run can leave environment_ elsewhere)
    if (execute_depth_ == 0) {
        instance_scope_saved_.clear();
        pending_instance_env_.reset();
    }
    auto eng_global = get_global_environment();
    if (eng_global) {
        environment_ = eng_global;
    } else {
        // Fallback: walk up the parent chain (shouldn't happen if engine is alive)
        while (environment_->get_parent()) {
            environment_ = environment_->get_parent();
        }
    }
    // Note: We don't clear the global environment, so variables persist between executions
}

void interpreter::push_scope() {
    environment_ = block_environment(environment_);
}

void interpreter::pop_scope() {
    if (environment_->get_parent()) {
        // Clear local values to trigger destructors before popping scope
        // This is crucial for script class destructors to run at scope exit
        auto current_env = environment_;
        environment_ = environment_->get_parent();
        end_scope(current_env);
    }
}

void interpreter::define_variable(const std::string& name, const script_value& value) {
    environment_->define(name, value);
}

void interpreter::push_instance_scope() {
    instance_scope_saved_.push_back(environment_);
    environment_ = std::make_shared<environment>(get_global_environment(), env_symbolizer_);
    pending_instance_env_ = environment_;
}

void interpreter::pop_instance_scope() {
    pending_instance_env_.reset();
    if (instance_scope_saved_.empty()) { pop_scope(); return; }
    auto instance_env = environment_;
    environment_ = std::move(instance_scope_saved_.back());
    instance_scope_saved_.pop_back();
    if (instance_env != environment_) { end_scope(instance_env); }
}

script_value interpreter::execute(const std::vector<declaration_ptr>& declarations, bool textual) {
    arm_native_stack();
    std::optional<detail::call_depth_guard> reentry_depth;
    checked_result<void> entry_result;
    if (is_executing()) {
        if (current_call_depth_ >= limits_->interpreter_call_depth_limit) {
            entry_result = detail::raise_call_depth();
        } else if (detail::native_stack_low()) {
            entry_result = detail::raise_native_stack();
        } else {
            reentry_depth.emplace(current_call_depth_);
        }
    }

    // A reentrant execute (a host callback firing mid-script, a hot reload) runs the nested program at top level - the global
    // environment, fresh call and value stacks - and restores the in-flight run's state afterwards
    struct reentry_isolation {
        interpreter* self;
        bool reentrant;
        bool textual;
        std::shared_ptr<environment> saved_env;
        std::vector<call_frame> saved_call_stack;
        script_valueStack saved_values;
        std::optional<script_value> saved_return;
        bool saved_has_return = false;
        detail::member_scope_ctx saved_member_scope{};
        unit_stamp saved_run_unit{};
        const ast_node* saved_top_level_node = nullptr;

        reentry_isolation(interpreter* interp, bool textual_run) : self(interp), reentrant(interp->is_executing()), textual(textual_run) {
            saved_run_unit = self->run_unit_;
            self->run_unit_ = self->string_symbolizer_->current_unit();   // the run's stamp: the derived unit of its top-level code
            if (reentrant) {
                saved_top_level_node = self->top_level_node_;
                saved_env = self->environment_;
                saved_call_stack = std::move(self->call_stack_);
                self->call_stack_.clear();
                saved_values = std::move(self->valueStack_);
                self->valueStack_.clear();
                saved_return = std::move(self->returnValue_);
                saved_has_return = self->hasReturnValue_;
                if (!self->pending_instance_env_ && !textual) {   // an include runs where it is written
                    self->environment_ = self->get_global_environment();
                }
                saved_member_scope = self->member_scope_;
                if (!textual) { self->member_scope_ = {}; }
            }
            // Counted, not a flag: a nested execute's exit decrements
            // symmetrically and can never clear the outer run's in-flight state
            self->pending_instance_env_.reset();
            ++self->execute_depth_;
        }
        ~reentry_isolation() {
            self->run_unit_ = saved_run_unit;
            if (reentrant) {
                self->top_level_node_ = saved_top_level_node;
                self->environment_ = std::move(saved_env);
                self->call_stack_ = std::move(saved_call_stack);
                self->valueStack_ = std::move(saved_values);
                self->returnValue_ = std::move(saved_return);
                self->hasReturnValue_ = saved_has_return;
                self->member_scope_ = saved_member_scope;
            }
            --self->execute_depth_;
        }
    } isolation(this, textual);

    script_value last_script_value = make_value();
    hasReturnValue_ = false;  // Reset return value state
    captured_trace_.clear();
    trace_captured_ = false;
    top_level_node_ = nullptr;

    // A denied entry runs only the existing error boundary, even for an empty
    // program; no declaration is dispatched and its exception text stays identical.
    const size_t execution_count = entry_result ? declarations.size() : 1;
    for (size_t i = 0; i < execution_count; i++) {
        auto* decl = entry_result ? declarations[i].get() : nullptr;
        // std::cerr << "  Declaration " << i << " type: " << typeid(*decl).name() << "\n";

        // Execute declaration with exception handling
        run_error_.reset();
        try {
            // std::cerr << "  About to visit declaration " << i << "\n";
            auto result = entry_result ? dispatch_decl(decl) : entry_result;
            if (!result) [[unlikely]] {
                run_error_ = result.error_value();
                // Convert error code to exception at boundary
                // Include the custom error message if available, formatted with symbol resolution
                if (!result.message().empty()) {
                    auto formatted = format_error_message(result.message(),
                        string_symbolizer_->get_string(result.symbol_id()),
                        string_symbolizer_->get_string(result.symbol_id2()));
                    throw std::system_error(result.error(), formatted);
                } else {
                    throw std::system_error(result.error());
                }
            }
            // std::cerr << "  Finished visiting declaration " << i << "\n";
        } catch (const script_exception& e) {
            // Convert to interpreter exception state
            run_error_.reset();
            active_exception_value_ = make_value(std::string(e.what()));
            current_exception_ = e;
            is_unwinding_ = true;
        } catch (const std::exception& e) {
            // a native exception a statement raises (a host error, an allocation failure) is the run's script exception, as on the vm
            active_exception_value_ = make_value(std::string(detail::native_exception_text(e)));
            current_exception_ = script_exception(detail::native_exception_text(e));
            is_unwinding_ = true;
        }

        // Check if we're unwinding due to an uncaught exception
        if (is_unwinding_) {
            if (!trace_captured_) capture_stack_trace();
            // Stop executing further declarations
            break;
        }
        
        // Check if this is an implicit return expression
        if (decl->get_type() == node_type::expression_decl) {
            auto* expr_decl = static_cast<expression_decl*>(decl);
            if (expr_decl->implicit_return && !valueStack_.empty()) {
                last_script_value = pop_value();
                // Dereference in case it's a reference (for expressions like m["key"] that return references)
                last_script_value = last_script_value.deref();
            }
        }
        
        // Clear any remaining values on the stack (from non-implicit expressions)
        while (!valueStack_.empty()) {
            pop_value();
        }
        
        // If we hit a return statement, break out of execution
        if (hasReturnValue_) {
            return returnValue_.value();
        }
    }
    return last_script_value;
}

script_value interpreter::evaluate(expression_ptr expr) {
    auto result = dispatch_expr(expr.get());
    if (!result) {
        // Return null on error
        return make_value();
    }
    return pop_value();
}

// Variable access methods
script_value interpreter::get_variable(const std::string& name) const {
    auto result = environment_->get(name);
    if (!result) {
        if (!result.message().empty()) {
            auto formatted = format_error_message(result.message(),
                string_symbolizer_->get_string(result.symbol_id()),
                string_symbolizer_->get_string(result.symbol_id2()));
            throw runtime_error(formatted);
        }
        throw runtime_error(result.error().message());
    }
    return result.value().deref();
}

bool interpreter::has_variable(const std::string& name) const {
    return environment_->contains(name);
}

std::unordered_map<std::string_view, script_value> interpreter::get_all_variables() const {
    // Since we should be at root scope after execution, just return local variables
    return environment_->get_local_variables();
}


// expression visitors
checked_result<void> interpreter::visit_literal_expr(literal_expr* expr) {
    // Literals are created at parse time without engine references and type_info set to nullptr
    // Extract raw values from storage and recreate with proper type_info
    // We can't use as_int()/as_string() etc because they check type() which returns jai_null_type for AST literals

    // Access the raw storage variant directly
    const auto& storage = expr->value.get_storage();

    // Determine type from variant index and extract + recreate value
    switch (storage.index()) {
        case 1:  // script_int
            push_value(make_value(storage.get<script_int>()));
            break;
        case 2:  // script_float
            push_value(make_value(storage.get<script_float>()));
            break;
        case 3:  // script_string (wrapped in strong_ptr)
            push_value(literal_string_value(storage.get<strong_ptr<script_string>>()));
            break;
        case 4:  // script_char
            push_value(make_value(storage.get<script_char>()));
            break;
        case 5:  // script_bool
            push_value(make_value(storage.get<script_bool>()));
            break;
        case 0:  // std::monostate (null)
            push_value(make_value());
            break;
        default:
            // For other types, try to set engine ref (though this shouldn't happen with literals)
            push_value(expr->value);
            break;
    }
    return checked_result<void>();
}

checked_result<void> interpreter::visit_identifier_expr(identifier_expr* expr) {
    // ::name - GLOBAL env only (locals, captures, member scope all skipped).
    // KEEP BYTE-PARALLEL with vm exec_load's global-only door.
    if (expr->global_qualified) {
        if (script_value* g = get_global_environment()->get_value_ptr(expr->symbol_id)) {
            push_value(g->deref());
            return checked_result<void>();
        }
        return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
            environment_->undefined_text(expr->symbol_id), expr->symbol_id);
    }
    // a local or a parameter reads its frame slot
    if (expr->slot_index != SIZE_MAX && !call_stack_.empty()) {
        script_value* local = call_stack_.back().get_local(expr->slot_index);
        if (local) {
            push_value(local->deref());  // Automatically handles references
            return checked_result<void>();
        }
    }

    // Special handling for type constructors like weak_ptr<T>, shared_ptr<T>.
    // This is reached on every non-local (global/function/member) identifier access,
    // so gate the two prefix scans on the first character: any name not starting
    // with 'w'/'s' (the overwhelming majority) can't match and skips both find()s.
    if (!expr->name.empty() && (expr->name.front() == 'w' || expr->name.front() == 's') &&
        (expr->name.find("weak_ptr<") == 0 || expr->name.find("shared_ptr<") == 0)) {
        // This is a type constructor being used as a function
        // Extract the base type name (weak_ptr or shared_ptr)
        size_t pos = expr->name.find('<');
        std::string base_type(expr->name.substr(0, pos));
        
        // Look up the constructor function for this type (nullable-pointer probe:
        // same ladder as get(), null miss - KEEP BYTE-PARALLEL
        // with vm_backend::exec_load)
        if (script_value* ctor = environment_->get_value_ptr(string_symbolizer_->intern(base_type));
            ctor && ctor->is_function()) {
            push_value(*ctor);
            return checked_result<void>();
        }
        // Fall through to normal error handling if not found
    }
    
    // Member scope (detail/member_scope.hpp): C++ unqualified-lookup order - the
    // function's own scopes, then the class's members, then the definition/global
    // chain. Field/static pushes copy WITHOUT deref (the old fallback's exact shape);
    // method hits mint the same bound method the old post-walk fallback minted.
    if (member_scope_.active()) {
        if (script_value* own = detail::own_scope_lookup(environment_.get(), member_scope_.floor, expr->symbol_id)) {
            push_value(own->deref());
            return checked_result<void>();
        }
        auto member = detail::probe_member_scope(member_scope_.state, member_scope_.this_storage,
                                                 member_scope_.statics, expr->symbol_id, /*want_methods*/ true);
        if (member) {
            JAISCRIPT_TRY(detail::bare_member_access(member, expr->symbol_id, environment_->find_access_context()));
            if (member.what == detail::member_probe_hit::kind::method) {
                if (member.instance && member_scope_.this_storage) {
                    if (!identifier_callee_lookup_) { return bare_method_value_error(expr, *member_scope_.this_storage); }
                    push_value(create_bound_method(*member_scope_.this_storage, member.method_value));
                } else {
                    push_value(std::move(member.method_value));   // static method: unbound callable
                }
            } else if (member.what == detail::member_probe_hit::kind::cpp_property) {
                auto read = detail::read_bare_cpp_property(*member_scope_.this_storage, member.instance,
                                                           member.method_value, member.getter_id, expr->symbol_id);
                if (!read) {
                    return read.error_value();
                }
                push_value(std::move(read.value()));
            } else {
                push_value(*member.value_ptr);
            }
            return checked_result<void>();
        }
        if (script_value* beyond = member_scope_.floor ? member_scope_.floor->get_value_ptr(expr->symbol_id)
                                                       : environment_->get_value_ptr(expr->symbol_id)) {
            push_value(beyond->deref());
            return checked_result<void>();
        }
        if (identifier_callee_lookup_ && member_scope_.this_storage) {   // KEEP BYTE-PARALLEL with vm_backend::fused_ident_value
            if (auto refused = detail::refuse_cdtor_call(member_scope_.this_storage->get_class_instance().get(), expr->symbol_id)) { return *refused; }
        }
        uint64_t scoped_name_id = string_symbolizer_->intern(expr->name);
        return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
            "Undefined variable '{0}'", scoped_name_id);
    }

    // Use parser's pre-computed symbol ID (always set by parser)
    // With lazy caching, environment_ will cache lookups automatically.
    // get_value_ptr IS get_ref's exact resolution ladder minus the error wrapper.
    if (script_value* env_val = environment_->get_value_ptr(expr->symbol_id)) {
        push_value(env_val->deref());  // Automatically handles references
    } else {

        // Variable not found - check if it's a member of 'this' (pointer probe: no
        // per-probe copy of the live this value)
        if (script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id())) {
            script_value& this_val = *this_ptr;
            if (this_val.is_object()) {
                // Try to access as a member of 'this'
                strong_ptr<class_instance> instance = this_val.get_class_instance();

                if (instance) {
                    // Intern the name to ID
                    uint64_t name_id = string_symbolizer_->intern(expr->name);

                    // A hybrid's C++ property reads through its getter (never a placeholder cell)
                    if (uint64_t getter_id = detail::cpp_property_getter_id(instance.get(), name_id)) {
                        auto read = detail::read_bare_cpp_property(this_val, instance.get(),
                                                                   instance->get_method(getter_id, false), getter_id, name_id);
                        if (!read) {
                            return read.error_value();
                        }
                        push_value(std::move(read.value()));
                        return checked_result<void>();
                    }
                    // Check instance fields first
                    if (instance->has_field(name_id)) {
                        push_value(instance->get_field(name_id));
                        return checked_result<void>();
                    }

                    // Check for methods (returns bound method)
                    script_value method = instance->get_method(name_id, false);
                    if (!method.is_invalid()) {
                        if (!identifier_callee_lookup_) { return bare_method_value_error(expr, this_val); }
                        script_value bound_method = create_bound_method(this_val, method);
                        push_value(std::move(bound_method));
                        return checked_result<void>();
                    }

                    // Check for static fields of the class
                    if (class_definition* owner = detail::bare_static_owner(bare_declaring_class(), instance->get_class_definition(),
                                                                            expr->symbol_id)) {
                        push_value(owner->get_static_field(expr->symbol_id));
                        return checked_result<void>();
                    }
                    if (identifier_callee_lookup_) {
                        if (auto refused = detail::refuse_cdtor_call(instance.get(), expr->symbol_id)) { return *refused; }
                    }
                }
            }
        }

        // Use error code instead of exception state - include variable name for debugging
        uint64_t name_id = string_symbolizer_->intern(expr->name);
        return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
            environment_->undefined_text(name_id), name_id);
    }
    return checked_result<void>();
}

template<bool Replaying>
checked_result<void> interpreter::visit_binary_expr_t(binary_expr* expr, const replay_point* rs) {
    script_value* const chain_root = std::exchange(chain_root_hold_, nullptr);
    std::optional<script_value> pre_fetched_left;
    // Replay: an operand evaluated before the yield is parked (left as partial[0])
    if constexpr (Replaying) {
        if (rs && rs->index == 1) { pre_fetched_left = rs->partial[0]; }
    }

    // Handle logical operators specially for short-circuit evaluation
    // Return proper boolean values (not JavaScript-style operand values)
    if (expr->op.type == token_type::ampersand_ampersand || expr->op.type == token_type::pipe_pipe) {
        if (!parked<Replaying>(rs, 0)) {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->left.get(), rs, 0), expr, rp_operand, 0, {});
            if (is_unwinding_) { return {}; }   // the left operand raised: no short-circuit decision, no right operand
        }
        script_value left = parked<Replaying>(rs, 0) ? *pre_fetched_left : pop_value();

        bool leftTruthy = is_truthy(left);

        if (expr->op.type == token_type::ampersand_ampersand) {
            if (!leftTruthy) {
                push_value(make_value(false));  // Short-circuit: left is falsy, return false
                return {};
            }
            // Left is truthy, evaluate right and return its boolean value
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->right.get(), rs, 1), expr, rp_operand, 1, (std::vector<script_value>{left}));
            if (is_unwinding_) { return {}; }
            script_value right = pop_value();
            push_value(make_value(is_truthy(right)));
            return {};
        } else { // pipe_pipe
            if (leftTruthy) {
                push_value(make_value(true));  // Short-circuit: left is truthy, return true
                return {};
            }
            // Left is falsy, evaluate right and return its boolean value
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->right.get(), rs, 1), expr, rp_operand, 1, (std::vector<script_value>{left}));
            if (is_unwinding_) { return {}; }
            script_value right = pop_value();
            push_value(make_value(is_truthy(right)));
            return {};
        }
    }

    // a ?? b: the left operand's value unless it is null; the right operand runs only then
    if (expr->op.type == token_type::question_question) {
        if (!parked<Replaying>(rs, 0)) {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->left.get(), rs, 0), expr, rp_operand, 0, {});
            if (is_unwinding_) { return {}; }
        }
        script_value left = parked<Replaying>(rs, 0) ? *pre_fetched_left : pop_value();
        if (!left.is_null()) {
            push_value(std::move(left));
            return {};
        }
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->right.get(), rs, 1), expr, rp_operand, 1, (std::vector<script_value>{left}));
        return {};
    }

    // Capture whether THIS binary expression is the outermost assignment-target
    // subscript, then clear the flag so any nested subscripts evaluated while
    // computing the operands below are treated as reads (no map auto-insert).
    const bool want_lvalue_write = lvalue_target_context_;
    const bool want_string_target = want_lvalue_write || string_target_context_;
    lvalue_target_context_ = false;
    string_target_context_ = false;

    // Evaluate operands once and use them throughout (or use pre-fetched values)
    std::optional<script_value> left_raw_opt, left_opt, right_opt;
    const bool subscript = expr->op.type == token_type::left_bracket;
    const bool left_links = subscript && detail::read_chain_link(expr->left.get());

    if (parked<Replaying>(rs, 0)) {
        left_raw_opt = *pre_fetched_left;   // evaluated before the yield, never re-run
    } else {
        {
            detail::chain_root_handoff handoff{chain_root_hold_, left_links ? chain_root : nullptr};
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->left.get(), rs, 0), expr, rp_operand, 0, {});
        }
        if (is_unwinding_) { return {}; }   // the left operand raised: the right one never runs (the vm's order)
        left_raw_opt = pop_value();  // Keep raw value for subscript handling
    }
    if (chain_root && subscript && !left_links) { *chain_root = *left_raw_opt; }
    left_opt = left_raw_opt->deref();  // Dereferenced version for most operations

    JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->right.get(), rs, 1), expr, rp_operand, 1, (std::vector<script_value>{*left_raw_opt}));
    // Check if we're unwinding due to an exception in the right expression
    if (is_unwinding_) {
        // Don't try to pop a value that wasn't pushed due to the exception
        return {};
    }
    right_opt = pop_value().deref();  // Handle references safely

    // Extract values from optional (guaranteed to have values at this point)
    script_value& left_raw = *left_raw_opt;
    script_value& left = *left_opt;
    script_value& right = *right_opt;
    
    // Check for custom operator functions first using cached symbol IDs (eliminates string construction)
    uint64_t op_symbol_id = 0;
    switch (expr->op.type) {
        case token_type::plus: op_symbol_id = ops_.plus; break;
        case token_type::minus: op_symbol_id = ops_.minus; break;
        case token_type::star: op_symbol_id = ops_.star; break;
        case token_type::slash: op_symbol_id = ops_.slash; break;
        case token_type::percent: op_symbol_id = ops_.percent; break;
        case token_type::less: op_symbol_id = ops_.less; break;
        case token_type::less_equal: op_symbol_id = ops_.less_equal; break;
        case token_type::greater: op_symbol_id = ops_.greater; break;
        case token_type::greater_equal: op_symbol_id = ops_.greater_equal; break;
        case token_type::equal_equal: op_symbol_id = ops_.equal_equal; break;
        case token_type::bang_equal: op_symbol_id = ops_.bang_equal; break;
        case token_type::spaceship: op_symbol_id = ops_.spaceship; break;
        case token_type::ampersand: op_symbol_id = ops_.ampersand; break;
        case token_type::pipe: op_symbol_id = ops_.pipe; break;
        case token_type::caret: op_symbol_id = ops_.caret; break;
        case token_type::left_shift: op_symbol_id = ops_.left_shift; break;
        case token_type::right_shift: op_symbol_id = ops_.right_shift; break;
        default: break;
    }

    // Check for custom operator function (excluding subscript) through the engine's
    // flat operator table (detail/operator_table.hpp): one mask test + array read,
    // no environment probe on any operator path.
    if (op_symbol_id != 0 && operator_table_ && operator_table_->any()) {
        if (const script_value* opFunc = operator_table_->entry(detail::binary_op_slot(expr->op.type))) {
            const script_function& func = opFunc->as_function();
            std::vector<script_value> args = {left, right};
            auto result = call_value(func, args);
            if (!result) {
                // Function returned error - propagate it up
                return result.error_value();
            }
            push_value(std::move(result.value()));
            return {};
        }
    }

    // Handle subscript operation specially
    if (expr->op.type == token_type::left_bracket) {
        if (left.raw_storage_index() == script_value::TYPEID_PARALLEL_BORROW) {
            // Parallel captured-read borrow: element reads go through the shared kernel
            // (detail/parallel_transform.hpp - parity with the vm twin by construction);
            // writes hit the runtime write wall inside it. No element reference is ever
            // minted into a borrowed container.
            auto elem = detail::parallel_borrow_subscript_read(left, right, engine_, want_lvalue_write);
            if (!elem) { return checked_result<void>(elem.error(), elem.message()); }
            push_value(std::move(elem).value());
            return {};
        }
        if (left.is_array()) {
            if (!right.is_int()) {
                return checked_result<void>(make_error_code(runtime_error_code::invalid_index_type), "Array index must be an integer");
            }
            script_int index = right.unchecked_as_int();
            const script_array* node = left.unchecked_array_node();

            if (index < 0 || index >= static_cast<script_int>(node->size())) {
                // Numbers intern as symbols: the {0}/{1} machinery resolves symbol ids,
                // so raw counts printed garbage names ("Array index bool out of bounds
                // for array of size class_definition")
                return detail::index_out_of_bounds_error(string_symbolizer_, detail::array_index_out_of_bounds_text, detail::array_index_out_of_bounds_slotless, index, node->size());
            }

            // An lvalue-shaped base allows modification, even if use_count == 1 (KEEP BYTE-PARALLEL with vm_backend::exec_index)
            const bool is_lvalue = detail::subscript_base_is_lvalue(is_lvalue_shaped_expr(expr->left.get()), left_raw);

            if (is_lvalue) {
                // Reallocation-safe reference to the element (container+index, not a raw
                // pointer into the vector buffer): holding `arr[i]` as an lvalue across a
                // push that reallocates would otherwise dangle.
                auto array_type_info = left.get_type_info();
                type_info_ptr element_type = array_type_info ? array_type_info->element_type() : nullptr;
                script_value ref_value = script_value::make_element_reference(
                    left.get_array_storage(), static_cast<size_t>(index), engine_, element_type);
                push_value(std::move(ref_value));
            } else if (node->is_typed()) {
                push_value(node->get(static_cast<size_t>(index), engine_));   // raw buffer read
            } else {
                // True temporary (e.g., function return), read-only access
                push_value(node->values()[index]);
            }
        } else if (left.is_map()) {
            // For maps, we need to handle both assignment and read access
            // Try to get a mutable reference if possible
            auto& map = const_cast<script_map&>(left.as_map());

            const bool is_lvalue = detail::subscript_base_is_lvalue(is_lvalue_shaped_expr(expr->left.get()), left_raw);

            if (is_lvalue && want_lvalue_write) {
                // Assignment target (m[k] = v): auto-insert a slot so the
                // assignment has somewhere to write through. The key is COPIED into
                // the map; an engine-less key (a raw AST literal reaching here via the
                // binary fast path) would poison the whole map for clone().
                script_value key = right;
                // Declared key type: keys never coerce, so the
                // check runs BEFORE the auto-insert (KEEP BYTE-PARALLEL with the vm)
                if (auto map_tag = left.get_type_info()) {
                    if (type_info_ptr key_type = map_tag->key_type();
                        key_type && key_type->base_type != script_value_type::jai_any_type &&
                        key.deref().type() != key_type->base_type) {
                        if (!detail::map_key_kind_admits(key.deref().type(), key_type->base_type)) {
                            return detail::element_type_error(string_symbolizer_, runtime_error_code::map_key_type_mismatch, "Cannot use '{0}' as a key of type '{1}'", key.deref(), key_type);
                        }
                        auto converted_key = detail::map_key_in_kind(key.deref(), key_type->base_type, engine_);
                        if (!converted_key) { return converted_key.error_value(); }
                        key = std::move(converted_key.value());
                    }
                }
                const size_t prior_map_size = map.size();
                script_value& value_ref = map[key];
                // engine::memory_cap: count NEW-entry growth (raised at the next back-edge)
                if (map.size() != prior_map_size) {
                    limits_->memory_charge_deferred(2 * sizeof(script_value) + (key.is_string() ? key.unchecked_as_string().size() : 0));
                }

                // Get value type constraint from the map's type_info for validation on assignment
                auto map_type_info = left.get_type_info();
                type_info_ptr value_type = map_type_info ? map_type_info->value_type() : nullptr;
                // Map-entry mode: pins the map + re-resolves by key (temporaries/erase safe)
                script_value ref_value = script_value::make_map_entry_reference(left.get_map_storage(), key, &value_ref, engine_, value_type);
                push_value(std::move(ref_value));
            } else if (is_lvalue) {
                // lvalue-shaped but a READ (x = m[k], a compound's target, the inner m[k] of m[k][i] / m[k].field): a reference to
                // the EXISTING entry so nested in-place mutation still works; a missing key raises and never grows the map
                auto it = map.find(right);
                if (it != map.end()) {
                    script_value& value_ref = const_cast<script_value&>(it->second);
                    auto map_type_info = left.get_type_info();
                    type_info_ptr value_type = map_type_info ? map_type_info->value_type() : nullptr;
                    push_value(script_value::make_map_entry_reference(left.get_map_storage(), right, &value_ref, engine_, value_type));
                } else {
                    return detail::map_key_missing_error(string_symbolizer_, left.type_info_raw(), right);
                }
            } else {
                // This is a true temporary (e.g., function return), read-only access
                auto it = map.find(right);
                if (it != map.end()) {
                    // Ensure the value has an engine ref before pushing
                    script_value val = it->second;
                    push_value(std::move(val));
                } else {
                    return detail::map_key_missing_error(string_symbolizer_, left.type_info_raw(), right);
                }
            }
        } else if (left.is_string()) {
            // s[i]: byte i as a char; a store target mints a STRING-ELEMENT reference
            // (detail/string_element.hpp; KEEP BYTE-PARALLEL with vm_backend::exec_index)
            if (!right.is_int()) {
                return checked_result<void>(make_error_code(runtime_error_code::invalid_index_type), "String index must be an integer");
            }
            const auto& str = left.unchecked_as_string();
            script_int index = right.unchecked_as_int();
            if (index < 0 || index >= static_cast<script_int>(str.size())) {
                return detail::index_out_of_bounds_error(string_symbolizer_, detail::string_index_out_of_bounds_text, detail::string_index_out_of_bounds_slotless, index, str.size());
            }
            const script_char byte = static_cast<script_char>(str[static_cast<size_t>(index)]);
            if (want_string_target) {
                script_value pin = string_element_pin(left_raw, expr->left.get());
                if (pin.is_reference()) {
                    push_value(script_value::make_string_element_reference(pin, static_cast<size_t>(index), engine_));
                    return {};
                }
            }
            push_value(script_value(byte, engine_));
        } else if (left.is_utf8_string()) {
            // u[i]: character i as a utf8_string, a store target like s[i] (KEEP BYTE-PARALLEL with vm_backend::exec_index)
            if (!right.is_int()) {
                return checked_result<void>(make_error_code(runtime_error_code::invalid_index_type), "String index must be an integer");
            }
            const script_utf8& text = left.unchecked_as_utf8();
            const bool cache = !engine_->parallel_region_active();
            const size_t len = detail::utf8::length(text, cache);
            script_int index = right.unchecked_as_int();
            if (index < 0 || index >= static_cast<script_int>(len)) {
                return detail::index_out_of_bounds_error(string_symbolizer_, detail::string_index_out_of_bounds_text, detail::string_index_out_of_bounds_slotless, index, len);
            }
            if (want_string_target) {
                script_value pin = string_element_pin(left_raw, expr->left.get());
                if (pin.is_reference()) {
                    push_value(script_value::make_string_element_reference(pin, static_cast<size_t>(index), engine_));
                    return {};
                }
            }
            push_value(detail::utf8::char_value(text, static_cast<size_t>(index), engine_, cache));
        } else {
            if (left.is_object()) {
                // a worker's operator[] runs through its region pin; anything else keeps the verdict (KEEP BYTE-PARALLEL with the vm)
                if (parallel_worker_) [[unlikely]] {
                    const detail::parallel_method_pin* pin = worker_operator_pin(left, subscript_op_id_);
                    if (!pin) {
                        return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation), worker_operator_refusal(left, { subscript_op_id_ }));
                    }
                    auto result = detail::call_parallel_operator_pin(*this, *pin, subscript_op_id_, left, { right }, engine_);
                    if (!result) {
                        return result.error_value();
                    }
                    if (want_string_target && !result.value().is_reference() && !is_unwinding_) {
                        return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target),
                            detail::by_value_subscript_store_text);
                    }
                    push_value(std::move(result.value()));
                    return {};
                }
                // First, try to find operator[] as a method on the object (for class instances)
                auto instance_result = left.checked_as<strong_ptr<class_instance>>();
                if (instance_result) {
                    auto instance = instance_result.value();
                    // Use cached subscript operator ID
                    script_value method = instance->get_method(subscript_op_id_, false);
                    if (method.is_function()) {
                        const script_function& func = method.as_function();
                        std::vector<script_value> args = {left, right};
                        auto result = call_value(func, args);
                        if (!result) {
                            return result.error_value();
                        }
                        if (want_string_target && !result.value().is_reference() && !is_unwinding_) {   // KEEP BYTE-PARALLEL with the vm
                            return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target),
                                detail::by_value_subscript_store_text);
                        }
                        push_value(std::move(result.value()));
                        return {};
                    }
                }

                // Fall back to the global [] operator (flat table - no env probe)
                if (operator_table_) {
                    if (const script_value* getMethod = operator_table_->entry(detail::op_slot::subscript)) {
                        const script_function& func = getMethod->as_function();
                        std::vector<script_value> args = {left, right};
                        auto result = call_value(func, args);
                        if (!result) {
                            // Function returned error - propagate it up
                            return result.error_value();
                        }
                        if (want_string_target && !result.value().is_reference() && !is_unwinding_) {
                            return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target),
                                detail::by_value_subscript_store_text);
                        }
                        push_value(std::move(result.value()));
                        return {};
                    }
                }
            }
            return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
                "Subscript can only be used on arrays, maps, or types with [] operator");
        }
        return {};
    }

    auto result = expr->op.joins_text ? join_text(left, right) : binary_operator(expr->op.type, left, right);
    if (!result) [[unlikely]] {
        return result.error_value();
    }
    push_value(std::move(result.value()));
    return {};
}
template<bool Replaying>
checked_result<void> interpreter::visit_unary_expr_t(unary_expr* expr, const replay_point* rs) {
    if (expr->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(expr, expr->const_check)); }
    // Generic path - evaluate operand and use existing logic
    JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->operand.get(), rs, 0), expr, rp_operand, 0, {});
    // Operand raised a script exception (e.g. ++obj.missing member read): propagate it
    // instead of clobbering it with invalid_assignment_target below (vm parity)
    if (is_unwinding_) {
        return {};
    }
    script_value operand = pop_value();
    // an element read arrives as a reference wrapper (the rhs-lvalue shape): the unary consumes the VALUE like every
    // other consumer (KEEP BYTE-PARALLEL with vm_backend::exec_unary); copy out through a temp, the holder owns the target
    if (operand.is_reference()) {
        script_value derefed = operand.deref();
        operand = std::move(derefed);
    }

    // a class instance's unary operator answers where the builtin rows miss (KEEP BYTE-PARALLEL with vm_backend::exec_unary)
    auto user_unary = [&](token_type uop) -> std::optional<checked_result<void>> {
        auto user = object_unary_via_method(operand, uop);
        if (!user) { return std::nullopt; }
        if (!*user) { return checked_result<void>(user->error_value()); }
        push_value(std::move(user->value()));
        return checked_result<void>{};
    };

    // `!` reads truthiness before a bound operand decodes: an opaque bound is truthy while its pointer is live, and decoding it
    // reads null (KEEP BYTE-PARALLEL with vm_backend::exec_unary)
    if (expr->op.type == token_type::bang) {
        push_value(make_value(!is_truthy(operand)));
        return {};
    }

    size_t oi = operand.raw_storage_index();
    // a bound operand decodes to a detached temporary (KEEP BYTE-PARALLEL with vm_backend::exec_unary)
    if (oi == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
        operand = operand.bound_decoded_temp();
        oi = operand.raw_storage_index();
    }
    // a char promotes to its int under - and ~ as under + (KEEP BYTE-PARALLEL with vm_backend::exec_unary)
    if (oi == script_value::TYPEID_CHAR && (expr->op.type == token_type::minus || expr->op.type == token_type::tilde)) {
        operand = detail::char_promoted(operand, engine_);
        oi = script_value::TYPEID_INT;
    }
    switch (expr->op.type) {
        case token_type::minus: {
            if (oi == script_value::TYPEID_INT) {
                // -INT64_MIN is undefined behavior; ints::try_neg applies the overflow
                // policy (raise when checked, wrap to INT64_MIN otherwise).
                script_int neg;
                if (!ints::try_neg(operand.unchecked_as_int(), neg)) {
                    return int_overflow_v("Integer overflow in unary '-'");
                }
                push_value(make_value(neg));
            } else if (oi == script_value::TYPEID_FLOAT) {
                push_value(make_value(-operand.unchecked_as_float()));
            } else if (detail::math::is_kind(oi)) {   // KEEP BYTE-PARALLEL with vm_backend::exec_unary
                auto negated = detail::math::negate(operand, engine_);
                if (!negated) {
                    return negated.error_value();
                }
                push_value(std::move(negated.value()));
            } else {
                if (auto landed = user_unary(token_type::minus)) { return std::move(*landed); }
                return checked_result<void>(make_error_code(runtime_error_code::invalid_numeric_operand), "Unary minus requires numeric operand");
            }
            break;
        }

        case token_type::bang:
            push_value(make_value(!is_truthy(operand)));
            break;

        case token_type::plus:
            // C++'s unary plus: a number or a math value is itself, a char promotes to its int
            if (oi == script_value::TYPEID_INT || oi == script_value::TYPEID_FLOAT || detail::math::is_kind(oi)) { push_value(std::move(operand)); break; }
            if (oi == script_value::TYPEID_CHAR) { push_value(detail::char_promoted(operand, engine_)); break; }
            if (auto landed = user_unary(token_type::plus)) { return std::move(*landed); }
            return checked_result<void>(make_error_code(runtime_error_code::invalid_numeric_operand), "Unary plus requires numeric operand");

        case token_type::tilde:
            // Bitwise NOT
            if (oi != script_value::TYPEID_INT) {
                if (auto landed = user_unary(token_type::tilde)) { return std::move(*landed); }
                return checked_result<void>(make_error_code(runtime_error_code::invalid_numeric_operand), "Bitwise NOT requires integer operand");
            }
            push_value(make_value(~operand.unchecked_as_int()));
            break;
            
        case token_type::plus_plus:
        case token_type::minus_minus:
            return increment_variable(expr);

        default:
            return checked_result<void>(make_error_code(runtime_error_code::unknown_operator));  // [ErrorText] Unknown operator
    }
    return {};
}

// ++x, x++, --x and x-- on a named variable (a member, an element or a reference target is a desugared compound): x += 1 on its
// storage (detail::incdec_step), the value stored back through the variable's table; a field of this takes the field store (KEEP
// BYTE-PARALLEL with vm_backend::exec_incdec)
checked_result<void> interpreter::increment_variable(unary_expr* expr) {
    if (expr->operand->get_type() != node_type::identifier_expr) {
        return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target));
    }
    auto* identifier = static_cast<identifier_expr*>(expr->operand.get());
    const bool increment = expr->op.type == token_type::plus_plus;
    const detail::incdec_form form = expr->is_postfix ? detail::incdec_form::postfix : detail::incdec_form::prefix;
    if (script_value* varPtr = variable_cell(identifier)) {
        if (auto in_place = object_operator_in_place(varPtr->deref(), increment ? ops_.plus : ops_.minus, make_value(), form)) {
            if (!*in_place) { return in_place->error_value(); }
            push_value(std::move(in_place->value()));
            return {};
        }
        if (const uint8_t kind = identifier->declared_ref_kind && varPtr->is_reference() ? identifier->declared_ref_kind : detail::ref_declared_kind(*varPtr);
            kind && kind != detail::ref_declared_current(*varPtr).raw_storage_index()) [[unlikely]] {   // `int& r` over another kind: the declared type's table
            auto stepped = detail::ref_target_assign(*varPtr, make_value(static_cast<script_int>(1)), increment ? token_type::plus : token_type::minus,
                true, expr->is_postfix, true, operator_table_, engine_, string_symbolizer_,
                [this](const script_value& l, token_type o, const script_value& r) { return evaluate_arithmetic(l, o, r, detail::compound_index(o)); },
                identifier->declared_ref);
            if (!stepped) { return stepped.error_value(); }
            push_value(std::move(stepped.value()));
            return {};
        }
        // a scratch-backed reference (a typed element, a C++ property) commits what the step wrote into its scratch
        auto* scratch_ref = varPtr->scratch_ref_holder();
        script_value& target = varPtr->deref();
        JAISCRIPT_TRY_ASSIGN(script_value stepped, detail::incdec_step(target, increment, engine_));
        script_value old = expr->is_postfix ? detail::incdec_old_value(target, engine_) : make_value();
        if (target.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
            target.assign_through(stepped);
        } else if (target.raw_storage_index() == script_value::TYPEID_INT && stepped.raw_storage_index() == script_value::TYPEID_INT) {
            target.unchecked_as_int_ref() = stepped.unchecked_as_int();
        } else if (target.raw_storage_index() == script_value::TYPEID_FLOAT) {
            target.unchecked_as_float_ref() = stepped.unchecked_as_float();
        } else {
            JAISCRIPT_TRY(compound_typed_store_back(target, stepped, identifier->name));
        }
        if (scratch_ref) [[unlikely]] { scratch_ref->commit_scratch(); }
        push_value(expr->is_postfix ? std::move(old) : std::move(stepped));
        return {};
    }
    if (identifier->global_qualified) {
        return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
            environment_->undefined_text(identifier->symbol_id), identifier->symbol_id);
    }
    script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id());
    strong_ptr<class_instance> instance = this_ptr && this_ptr->is_object() ? this_ptr->get_class_instance() : strong_ptr<class_instance>{};
    if (!instance || !instance->has_field(identifier->symbol_id)) {
        return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
            environment_->undefined_text(identifier->symbol_id), identifier->symbol_id);
    }
    if (class_definition* field_class = instance->get_class_definition(); field_class && field_class->chain_has_nonpublic()) [[unlikely]] {
        JAISCRIPT_TRY(detail::enforce_member_access(field_class, identifier->symbol_id, environment_->find_access_context()));
    }
    // a hybrid's C++ property reads through its getter and stores through its setter
    script_value& this_val = *this_ptr;
    const uint64_t prop_getter = detail::cpp_property_getter_id(instance.get(), identifier->symbol_id);
    const uint64_t prop_setter = prop_getter ? detail::cpp_property_setter_id(instance.get(), identifier->symbol_id) : 0;
    script_value current = make_value();
    if (prop_getter) {
        JAISCRIPT_TRY_ASSIGN(current, detail::read_bare_cpp_property(this_val, instance.get(), instance->get_method(prop_getter, false), prop_getter, identifier->symbol_id));
    } else {
        current = instance->get_field(identifier->symbol_id);
    }
    if (auto in_place = object_operator_in_place(current, increment ? ops_.plus : ops_.minus, make_value(), form)) {
        if (!*in_place) { return in_place->error_value(); }
        push_value(std::move(in_place->value()));
        return {};
    }
    JAISCRIPT_TRY_ASSIGN(script_value stepped, detail::incdec_step(current, increment, engine_));
    script_value old = expr->is_postfix ? detail::incdec_old_value(current, engine_) : make_value();
    if (prop_setter) {
        JAISCRIPT_TRY(detail::write_cpp_property(this_val, instance->get_class_definition(), prop_setter, stepped));
    } else {
        auto enforced = instance->field_store(identifier->symbol_id, stepped, [&] { return clone_for_assignment(stepped); });
        if (!enforced) {
            return enforced.error_value();
        }
        instance->set_field_unchecked(identifier->symbol_id, enforced.value());
    }
    push_value(expr->is_postfix ? std::move(old) : std::move(stepped));
    return {};
}

checked_result<script_value> interpreter::assign_math_component(script_value& base, member_expr* memberExpr, const script_value& value) {
    return detail::math::assign_component(base, memberExpr, value,
        [this](const identifier_expr* id) { return resolve_local_or_env(id->slot_index, id->symbol_id, id->global_qualified); },
        [this](const member_expr* m) {
            return detail::math::static_component_cell(m, *engine_, *environment_, *string_symbolizer_, environment_->find_access_context());
        },
        detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()}, environment_.get(), engine_, string_symbolizer_);
}

checked_result<void> interpreter::assign_member_value(const script_value& objectValue, member_expr* memberExpr, const script_value& value,
                                                      script_value* consumable) {
    // A worker stores a field directly: the setter probe below copies a shared method value (a cross-worker count race), and a
    // backing field's enforce and set is what the auto-setter does on the worker's own instance; any other member store is refused
    if (parallel_worker_ && objectValue.raw_storage_index() == script_value::TYPEID_OBJECT) [[unlikely]] {
        auto& holder = const_cast<script_value&>(objectValue).unchecked_get_object_storage();
        if (holder && holder->is_instance()) {
            auto* inst = holder->unchecked_instance();
            if (memberExpr->member_id != UINT64_MAX && detail::parallel_script_field(*inst, memberExpr->member_id)) {
                auto enforced = inst->field_store(memberExpr->member_id, value, [&] { return clone_for_assignment(value); });
                if (!enforced) {
                    return enforced.error_value();
                }
                inst->set_field_unchecked(memberExpr->member_id, std::move(enforced).value());
                return {};
            }
            if (const detail::parallel_method_pin* setter = memberExpr->member_id != UINT64_MAX && parallel_method_pins_
                    ? parallel_method_pins_->find(inst->get_class_definition(), string_symbolizer_->get_setter_id_with_view(memberExpr->member_id).first) : nullptr) {
                auto stored = detail::call_parallel_host_pin(*setter, objectValue, memberExpr->member_id, { value }, engine_, true);
                if (!stored) { return stored.error_value(); }
                return {};
            }
            return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
                "only direct field stores on class instances are allowed in a parallel body");
        }
    }
    auto target = resolve_member_target(objectValue);
    if (!target) {
        if (objectValue.deref().parallel_object_borrow()) [[unlikely]] { return detail::parallel_shared_store_refusal(memberExpr->member_id).error_value(); }
        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
            "Cannot assign property to non-object value");
    }

    uint64_t member_id = memberExpr->member_id != UINT64_MAX
        ? memberExpr->member_id
        : string_symbolizer_->intern(memberExpr->member);

    if (target.class_def && target.class_def->chain_has_nonpublic()) [[unlikely]] {
        JAISCRIPT_TRY(detail::enforce_member_access(target.class_def, member_id,
                                                    environment_->find_access_context()));
    }

    auto [setter_id, _] = string_symbolizer_->get_setter_id_with_view(member_id);
    script_value setter = target.user_accessor(setter_id);   // a synthesized setter takes the field rung
    if (!setter.is_null() && !setter.is_invalid() && setter.is_function()) {
        const script_function& func = setter.as_function();
        std::vector<script_value> args = {objectValue, clone_for_assignment(value)};
        auto result = call_value(func, args);
        if (!result) {
            return result.error_value();
        }
    } else if (target.has_field(member_id)) {
        // Direct field assignment (deep copy for value types, share for shared_ptr);
        // typed fields enforce like locals
        auto enforced = target.instance->field_store(member_id, value, [&] { return clone_for_assignment(value); }, consumable);
        if (!enforced) {
            return enforced.error_value();
        }
        target.instance->set_field_unchecked(member_id, enforced.value());
    } else {
        raise_script_text(no_such_member_store_text(memberExpr->member, target.class_name()), memberExpr->location);
        return {};
    }
    return {};
}

// a node the checker stamped with a const verdict: KEEP BYTE-PARALLEL with the vm's op_error / op_const_guard
checked_result<void> interpreter::const_guard_at(const ast_node* node, const const_site& site) {
    return detail::const_guard(node, site, detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
                               environment_.get(), engine_, string_symbolizer_);
}

checked_result<void> interpreter::non_lvalue_target(const expression* target) {
    return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target), "{0}",
                                string_symbolizer_->intern(detail::non_lvalue_target_text(target)));
}

// a op= b, ++ and -- on a named target: a local, a capture, a global or a field of this
template<bool Replaying>
checked_result<void> interpreter::assign_compound_identifier(assignment_expr* expr, const replay_point* rs) {
    auto* identifier = static_cast<identifier_expr*>(expr->target.get());

    script_value* varPtr = variable_cell(identifier);
    if (varPtr) {
        // Constrained element/field ref: route through the shared
        // helper so the compound result honors the constraint like subscript
        // compounds do (vm parity). A C++ PROPERTY ref takes the same route: its
        // deref is a scratch, the write must reach the setter.
        if (varPtr->is_reference()) {
            const auto* refHolder = varPtr->get_reference_holder();
            if (refHolder && (refHolder->container_element_type || refHolder->property_ref() || refHolder->component_element())) {
                JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_rhs, 0, {});
                if (is_unwinding_) {
                    push_value(make_value());
                    return {};
                }
                script_value rightValue = pop_value();
                if (auto in_place = compound_in_place(varPtr->deref(), expr, rightValue)) {
                    if (!*in_place) { return in_place->error_value(); }
                    push_value(std::move(in_place->value()));
                    return {};
                }
                const token_type op = detail::compound_base(expr->op.type);
                const char* opName = detail::binary_op_name(op);
                auto result = detail::ref_compound_store_constrained(*varPtr, rightValue, op, opName,
                    operator_table_, engine_, string_symbolizer_,
                    [this](const script_value& l, token_type o, const script_value& r) {
                        return evaluate_arithmetic(l, o, r, detail::compound_index(o));
                    }, identifier->declared_ref);
                if (!result) {
                    return result.error_value();
                }
                push_value(std::move(result.value()));
                return {};
            }
        }
        // an element, map-entry or property reference is dereferenced only after the right-hand side has run (a
        // reference to a removed element raises when the store resolves, not before the right side's calls; KEEP
        // BYTE-PARALLEL with the VM's compound store, whose right value is already on the stack)
        const auto* target_holder = varPtr->is_reference() ? varPtr->get_reference_holder() : nullptr;
        const uint8_t declared_kind = !target_holder ? uint8_t{0} : identifier->declared_ref_kind ? identifier->declared_ref_kind : detail::ref_declared_kind(*varPtr);
        const bool deferred_target = target_holder != nullptr && (!target_holder->plain_cell() ||
                                     (declared_kind && declared_kind != target_holder->cell()->raw_storage_index()));
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_rhs, 0, {});
        if (is_unwinding_) {
            // The RHS raised: surface the ORIGINAL exception instead of running the
            // compound arithmetic on the placeholder, whose follow-on type error
            // clobbered the catch value (KEEP BYTE-PARALLEL with the constrained-ref
            // branch above and the vm's compound store)
            push_value(make_value());
            return {};
        }
        script_value rightValue = pop_value();
        // a bound rhs decodes to a detached temporary
        if (rightValue.deref().raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
            script_value decoded = rightValue.deref().bound_decoded_temp();
            rightValue = std::move(decoded);
        }
        auto& derefRight = rightValue.deref();

        // Get type for RHS
        auto rightType = derefRight.type();

        // The right side ran user code: a by-reference argument may have boxed the variable into a cell in
        // place, so the slot's representation changed under the reference taken above (the wrapper read as a
        // Type mismatch). The rest of the path runs against the binding read again HERE, where the language's
        // order puts the target's read (the right operand before the left, as for every compound assignment;
        // KEEP BYTE-PARALLEL with the VM's compound store, which reads the slot after the right side)
        auto apply = [&](script_value& target, script_value_type leftType) -> checked_result<void> {
        if (expr->incdec_desugar) {   // a desugared ++/-- steps like the named form (increment_variable)
            JAISCRIPT_TRY_ASSIGN(script_value stepped, detail::incdec_step(target, expr->op.type == token_type::plus_equal, engine_));
            if (target.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
                target.assign_through(stepped);
            } else {
                JAISCRIPT_TRY(compound_typed_store_back(target, std::move(stepped), identifier->name));
            }
            push_value(expression_result_needed_ ? target.clone() : target);
            return {};
        }
        // Check for custom operators first (rare path; flat table, no env probe)
        if (has_custom_numeric_ops_) [[unlikely]] {
            detail::op_slot op_slot = detail::op_slot::none;
            switch (expr->op.type) {
                case token_type::plus_equal: op_slot = detail::op_slot::plus; break;
                case token_type::minus_equal: op_slot = detail::op_slot::minus; break;
                case token_type::star_equal: op_slot = detail::op_slot::star; break;
                default: if (detail::compound_is_bitwise(expr->op.type)) { op_slot = detail::binary_op_slot(detail::compound_base(expr->op.type)); } break;
            }
            if (operator_table_) {
                if (const script_value* opFunc = operator_table_->entry(op_slot)) {
                    const script_function& func = opFunc->as_function();
                    std::vector<script_value> args = {target.clone(), rightValue};
                    auto result = call_value(func, args);
                    if (!result) {
                        return result.error_value();
                    }
                    target = std::move(result.value());
                    push_value(expression_result_needed_ ? target.clone() : target);
                    return {};
                }
            }
        }

        // Check for custom arithmetic operators on class instances
        if (leftType == script_value_type::jai_object_type) {
            uint64_t op_symbol_id = 0;
            op_symbol_id = compound_op_symbol(expr->op.type);
            if (op_symbol_id != 0) {
                auto custom_outcome = object_compound_through_binary(target, rightValue, op_symbol_id);
                if (custom_outcome.has_value() && !*custom_outcome) { return custom_outcome->error_value(); }
                std::optional<script_value> custom_result;
                if (custom_outcome.has_value()) { custom_result = std::move(custom_outcome->value()); }
                if (custom_result.has_value()) {
                    JAISCRIPT_TRY(detail::compound_slot_store(target, std::move(custom_result.value()), engine_));
                    push_value(expression_result_needed_ ? target.clone() : target);
                    return {};
                }
            }
        }

        // &= |= ^= <<= >>=: the binary operator's kernel, stored back through the slot's own table
        // (KEEP BYTE-PARALLEL with the VM exec_compound_store)
        if (detail::compound_is_bitwise(expr->op.type)) [[unlikely]] {
            const token_type baseOp = detail::compound_base(expr->op.type);
            JAISCRIPT_TRY_ASSIGN(script_value promoted, evaluate_arithmetic(target, baseOp, derefRight, detail::compound_index(baseOp)));
            if (target.raw_storage_index() == script_value::TYPEID_CPP_BOUND) { target.assign_through(std::move(promoted)); }
            else { JAISCRIPT_TRY(compound_typed_store_back(target, std::move(promoted), identifier->name)); }
            push_value(expression_result_needed_ ? target.clone() : target);
            return {};
        }

        // + - * / % on numbers and += on text, in place where the kind holds; a bound variable stores back through
        // assign_through. A zero divisor raises before the operands' kinds are read, an overflow names the compound, and
        // a script value's char, utf8 or math operand and its %= take the general arithmetic (KEEP BYTE-PARALLEL with the
        // VM exec_compound_store)
        const bool bound = target.raw_storage_index() == script_value::TYPEID_CPP_BOUND;
        const token_type op = expr->op.type;
        const token_type baseOp = detail::compound_base(op);
        const size_t rightIdx = derefRight.raw_storage_index();
        const bool zero_divisor = (rightIdx == script_value::TYPEID_INT && derefRight.unchecked_as_int() == 0) ||
                                  (rightIdx == script_value::TYPEID_FLOAT && derefRight.unchecked_as_float() == 0.0);
        if (bound && op == token_type::percent_equal) { return checked_result<void>(make_error_code(runtime_error_code::unknown_operator)); }
        if (!bound) {
            if (op == token_type::plus_equal && detail::compound_plus_text(target, derefRight) == detail::compound_text::mismatch) {
                return detail::compound_mismatch(0, target, derefRight, engine_);   // one '+=' table for every destination
            }
            const size_t leftIdx = target.raw_storage_index();
            const bool general = detail::char_operands_promote(leftIdx, rightIdx) || detail::utf8_operands(leftIdx, rightIdx) || detail::math::owns_arithmetic(leftIdx, rightIdx);
            if (!general && op == token_type::percent_equal && rightIdx == script_value::TYPEID_INT && derefRight.unchecked_as_int() == 0) {
                return checked_result<void>(make_error_code(runtime_error_code::modulo_by_zero), "Modulo by zero");
            }
            if (general || op == token_type::percent_equal) [[unlikely]] {
                JAISCRIPT_TRY_ASSIGN(script_value promoted, evaluate_arithmetic(target, baseOp, derefRight, detail::compound_index(baseOp)));
                JAISCRIPT_TRY(compound_typed_store_back(target, std::move(promoted), identifier->name));
                push_value(expression_result_needed_ ? target.clone() : target);
                return {};
            }
        }
        if (op == token_type::slash_equal && zero_divisor) { return checked_result<void>(make_error_code(runtime_error_code::division_by_zero)); }
        // a bound target is read by its kind (the storage is the binding); a script value by its storage (a typed null reads none)
        const size_t leftIdx = !bound ? target.raw_storage_index() : target.is_int() ? script_value::TYPEID_INT : target.is_float() ? script_value::TYPEID_FLOAT
                             : target.is_string() ? script_value::TYPEID_STRING : script_value::TYPEID_NULL;
        const size_t index = detail::compound_index(baseOp);
        if (op == token_type::plus_equal && leftIdx == script_value::TYPEID_STRING && rightIdx == script_value::TYPEID_STRING) {
            // engine::memory_cap chokepoint: deny the append before it exists
            if (!limits_->memory_charge(derefRight.unchecked_as_string().size())) [[unlikely]] { return detail::raise_memory_cap(*limits_); }
            target.unchecked_as_string_ref(engine_) += derefRight.unchecked_as_string();
        } else if (!bound && op == token_type::plus_equal && leftIdx == script_value::TYPEID_STRING && rightIdx == script_value::TYPEID_CHAR) {
            // string += char appends the char as text (a char is a text unit; string += int stays an error)
            if (!limits_->memory_charge(sizeof(script_value) + target.unchecked_as_string().size() + 1)) [[unlikely]] { return detail::raise_memory_cap(*limits_); }
            target.unchecked_as_string_ref(engine_).push_back(derefRight.unchecked_as_char());
        } else if (!detail::numeric_storage(leftIdx) || !detail::numeric_storage(rightIdx)) {
            return detail::compound_mismatch(index, target, derefRight, engine_);
        } else if (leftIdx == script_value::TYPEID_INT && rightIdx == script_value::TYPEID_INT) {
            constexpr const char* overflow_text[] = { "Integer overflow in '+='", "Integer overflow in '-='", "Integer overflow in '*='", "Integer overflow in '/='" };
            script_int out;
            if (detail::int_arith(baseOp, target.unchecked_as_int(), derefRight.unchecked_as_int(), out) != detail::scalar_fault::none) { return int_overflow_v(overflow_text[index]); }
            if (bound) { target.assign_through(make_value(out)); }
            else { target.unchecked_as_int_ref() = out; }
        } else {
            const script_float a = leftIdx == script_value::TYPEID_INT ? static_cast<script_float>(target.unchecked_as_int()) : target.unchecked_as_float();
            const script_float b = rightIdx == script_value::TYPEID_INT ? static_cast<script_float>(derefRight.unchecked_as_int()) : derefRight.unchecked_as_float();
            script_float out;
            (void)detail::float_arith(baseOp, a, b, out);   // its one fault, a zero divisor, raised above
            if (bound) { target.assign_through(make_value(out)); }
            else if (leftIdx == script_value::TYPEID_FLOAT) { target.unchecked_as_float_ref() = out; }
            else { JAISCRIPT_TRY(compound_typed_store_back(target, make_value(out), identifier->name)); }
        }
        push_value(expression_result_needed_ ? target.clone() : target);
        return {};
        };
        varPtr = variable_cell(identifier);
        if (!varPtr) [[unlikely]] {
            return checked_result<void>(make_error_code(runtime_error_code::undefined_variable), environment_->undefined_text(identifier->symbol_id), identifier->symbol_id);
        }
        if (deferred_target) {
            // the target's reference is read here, after the right side: a removed element or map entry is the
            // language's error at this point (the checked twin of deref, so the error takes the ordinary flow)
            auto alive = detail::ref_checked_deref(*varPtr);
            if (!alive) [[unlikely]] { return alive.error_value(); }
        }
        if (auto in_place = compound_in_place(varPtr->deref(), expr, rightValue)) {
            if (!*in_place) { return in_place->error_value(); }
            push_value(std::move(in_place->value()));
            return {};
        }
        // `int& r` over storage of another kind, or a result leaving T's kind: the declared type's table (KEEP
        // BYTE-PARALLEL with the vm's compound store)
        if (const uint8_t kind = !varPtr->is_reference() ? uint8_t{0} : identifier->declared_ref_kind ? identifier->declared_ref_kind : detail::ref_declared_kind(*varPtr);
            kind && !detail::ref_declared_compound_keeps(kind, detail::ref_declared_current(*varPtr), derefRight, detail::compound_base(expr->op.type))) [[unlikely]] {
            const token_type op = detail::compound_base(expr->op.type);
            auto result = detail::ref_compound_store_constrained(*varPtr, rightValue, op, detail::binary_op_name(op),
                operator_table_, engine_, string_symbolizer_,
                [this](const script_value& l, token_type o, const script_value& r) {
                    return evaluate_arithmetic(l, o, r, detail::compound_index(o));
                }, identifier->declared_ref);
            if (!result) {
                return result.error_value();
            }
            push_value(std::move(result.value()));
            return {};
        }
        return apply(varPtr->deref(), varPtr->deref().type());
    } else {
        // VM parity (C++17 sequencing): the rhs of a compound assignment is
        // evaluated BEFORE the target resolves, so rhs errors and side effects
        // (e.g. an undefined rhs identifier) surface first
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_rhs, 0, {});
        if (is_unwinding_) {
            push_value(make_value());
            return {};
        }
        script_value rightValue = pop_value();

        // ::name targets never reach the implicit this.member fallback
        if (identifier->global_qualified) {
            return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                environment_->undefined_text(identifier->symbol_id), identifier->symbol_id);
        }
        // Fallback: identifier is an implicit this.member access
        // Check if 'this' exists and has this field
        script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id());
        if (!this_ptr || !this_ptr->is_object()) {
            return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                environment_->undefined_text(identifier->symbol_id, "Undefined variable '{0}' (no 'this' in scope)"), identifier->symbol_id);
        }

        script_value& this_val = *this_ptr;
        strong_ptr<class_instance> instance = this_val.get_class_instance();

        if (!instance || !instance->has_field(identifier->symbol_id)) {
            return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                environment_->undefined_text(identifier->symbol_id, "Undefined variable '{0}' (not a field of 'this')"), identifier->symbol_id);
        }
        if (class_definition* field_class = instance->get_class_definition(); field_class && field_class->chain_has_nonpublic()) [[unlikely]] {
            JAISCRIPT_TRY(detail::enforce_member_access(field_class, identifier->symbol_id, environment_->find_access_context()));
        }

        // Get current field value. A hybrid's C++ property reads through its getter and
        // stores through its setter (KEEP BYTE-PARALLEL with the VM exec_compound_store this-tail)
        const uint64_t prop_getter = detail::cpp_property_getter_id(instance.get(), identifier->symbol_id);
        const uint64_t prop_setter = prop_getter ? detail::cpp_property_setter_id(instance.get(), identifier->symbol_id) : 0;
        script_value currentValue = make_value();
        if (prop_getter) {
            JAISCRIPT_TRY_ASSIGN(currentValue, detail::read_bare_cpp_property(this_val, instance.get(),
                instance->get_method(prop_getter, false), prop_getter, identifier->symbol_id));
        } else {
            currentValue = instance->get_field(identifier->symbol_id);
        }
        if (auto in_place = compound_in_place(currentValue, expr, rightValue)) {
            if (!*in_place) { return in_place->error_value(); }
            push_value(std::move(in_place->value()));
            return {};
        }

        // a op= b through the binary operator on class instances
        if (!expr->incdec_desugar && currentValue.is_object()) {
            const uint64_t op_symbol_id = compound_op_symbol(expr->op.type);
            if (op_symbol_id != 0) {
                auto custom_outcome = object_compound_through_binary(currentValue, rightValue, op_symbol_id);
                if (custom_outcome.has_value() && !*custom_outcome) { return custom_outcome->error_value(); }
                std::optional<script_value> custom_result;
                if (custom_outcome.has_value()) { custom_result = std::move(custom_outcome->value()); }
                if (custom_result.has_value()) {
                    if (prop_setter) {
                        JAISCRIPT_TRY(detail::write_cpp_property(this_val, instance->get_class_definition(), prop_setter, custom_result.value()));
                    } else {
                        auto enforced = instance->field_store(identifier->symbol_id, custom_result.value(), [&] { return clone_for_assignment(custom_result.value()); });
                        if (!enforced) {
                            return enforced.error_value();
                        }
                        instance->set_field_unchecked(identifier->symbol_id, enforced.value());
                    }
                    push_value(std::move(custom_result.value()));
                    return {};
                }
            }
        }

        JAISCRIPT_TRY_ASSIGN(script_value resultValue, compound_value(detail::compound_base(expr->op.type), currentValue, rightValue, expr->incdec_desugar));

        // Store the enforced (declared-type converted) value; a consumed expression's
        // value is the stored one, read back through a property getter
        if (prop_setter) {
            JAISCRIPT_TRY(detail::write_cpp_property(this_val, instance->get_class_definition(), prop_setter, resultValue));
            if (expression_result_needed_) {
                JAISCRIPT_TRY_ASSIGN(resultValue, detail::read_bare_cpp_property(this_val, instance.get(),
                    instance->get_method(prop_getter, false), prop_getter, identifier->symbol_id));
            }
        } else {
            auto enforced = instance->field_store(identifier->symbol_id, resultValue, [&] { return clone_for_assignment(resultValue); });
            if (!enforced) {
                return enforced.error_value();
            }
            if (expression_result_needed_) { resultValue = clone_for_assignment(enforced.value()); }
            instance->set_field_unchecked(identifier->symbol_id, enforced.value());
        }
        push_value(std::move(resultValue));
    }
    return {};
}

// a op= b on a member or a static, the receiver evaluated once
template<bool Replaying>
checked_result<void> interpreter::assign_compound_member(assignment_expr* expr, const replay_point* rs) {
    auto* memberExpr = static_cast<member_expr*>(expr->target.get());
    // C::s op= v writes back through the shared static-store kernel (vm parity)
    auto store_static_target = [&](const script_value& result) -> checked_result<script_value> {
        if (auto ns_target = detail::resolve_namespace_store(*engine_, memberExpr, *string_symbolizer_)) {
            return store_namespace_cell(*ns_target, memberExpr, clone_for_assignment(result));
        }
        auto cls = detail::resolve_static_store_class(memberExpr, *environment_, *string_symbolizer_,
                                                      environment_->find_access_context());
        if (!cls) {
            return cls.error_value();
        }
        auto stored = detail::store_static_member(*cls.value(), memberExpr->member_id, result, [&] { return clone_for_assignment(result); });
        if (!stored) {
            return stored.error_value();
        }
        return std::move(stored.value());
    };
    // Handle compound assignment to member expression (e.g., obj.value += 10)
    // C++17: the right-hand side is sequenced before the target, then the
    // receiver is evaluated ONCE and held raw (a reference receiver re-resolves by mode at the store)
    // across the read and the store-back; a :: target has no receiver (replay: rhs 0, receiver 1,
    // partial {rhs}). KEEP BYTE-PARALLEL with the vm's value-first member compound.
    script_value rightValue;
    script_value objectValue;
    script_value currentValue;
    script_value postfix_old;
    if (parked<Replaying>(rs, 0)) {
        rightValue = rs->partial[0];
    } else {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_member_compound, 0, {});
        if (is_unwinding_) {
            // RHS raised: surface the original exception, never touch the target
            push_value(make_value());
            return {};
        }
        rightValue = pop_value();
        if (rightValue.is_reference()) {   // an element or map read: its value, taken now (KEEP BYTE-PARALLEL with the vm's member compound)
            script_value derefed = rightValue.deref();
            rightValue = std::move(derefed);
        }
    }
    if (memberExpr->is_static) {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(memberExpr, rs, 1), expr, rp_asg_member_compound, 1, (std::vector<script_value>{rightValue}));
    } else {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(memberExpr->object.get(), rs, 1), expr, rp_asg_member_compound, 1, (std::vector<script_value>{rightValue}));
        objectValue = pop_value();
        if (is_unwinding_) {
            push_value(make_value());
            return {};
        }
        JAISCRIPT_TRY(member_read_on(memberExpr, objectValue, false));
    }
    currentValue = pop_value().deref();
    if (is_unwinding_) {
        // Target read raised: surface it, not the ++/-- gate's error on the placeholder
        push_value(make_value());
        return {};
    }
    postfix_old = expr->postfix_old_result ? currentValue : make_value();
    if (auto in_place = compound_in_place(currentValue, expr, rightValue)) {
        if (!*in_place) { return in_place->error_value(); }
        push_value(std::move(in_place->value()));
        return {};
    }

    // a op= b through the binary operator on class instances
    if (!expr->incdec_desugar && currentValue.is_object()) {
        const uint64_t op_symbol_id = compound_op_symbol(expr->op.type);
        if (op_symbol_id != 0) {
            auto custom_outcome = object_compound_through_binary(currentValue, rightValue, op_symbol_id);
            if (custom_outcome.has_value() && !*custom_outcome) { return custom_outcome->error_value(); }
            std::optional<script_value> custom_result;
            if (custom_outcome.has_value()) { custom_result = std::move(custom_outcome->value()); }
            if (custom_result.has_value()) {
                if (memberExpr->is_static) {
                    script_value stored_static;
                    JAISCRIPT_TRY_ASSIGN(stored_static, store_static_target(custom_result.value()));
                    push_value(expression_result_needed_ ? clone_for_assignment(stored_static) : std::move(custom_result.value()));
                    return {};
                }
                // Assign the result back to the property
                script_value target_obj = objectValue.deref();
                if (target_obj.is_object()) {
                    JAISCRIPT_TRY(assign_member_value(target_obj, memberExpr, custom_result.value()));
                    if (is_unwinding_) {
                        push_value(make_value());
                        return {};
                    }
                    if (expression_result_needed_ &&
                        detail::member_reads_back(target_obj, memberExpr->member_id, string_symbolizer_->get_getter_id_with_view(memberExpr->member_id).first, engine_)) {
                        JAISCRIPT_TRY(member_read_on(memberExpr, target_obj, false));
                        if (!is_unwinding_) { script_value got = pop_value(); push_value(clone_for_assignment(got.deref())); }
                        return {};
                    }
                    push_value(std::move(custom_result.value()));
                    return {};
                }
            }
        }
    }

    JAISCRIPT_TRY_ASSIGN(script_value resultValue, compound_value(detail::compound_base(expr->op.type), currentValue, rightValue, expr->incdec_desugar));

    if (memberExpr->is_static) {
        script_value stored_static;
        JAISCRIPT_TRY_ASSIGN(stored_static, store_static_target(resultValue));
        push_value(expr->postfix_old_result ? std::move(postfix_old)
                   : expression_result_needed_ ? clone_for_assignment(stored_static) : std::move(resultValue));
        return {};
    }

    // Now assign the result back to the property (the receiver evaluated once above)
    script_value target_obj = objectValue.deref();

    // A math value's component (KEEP BYTE-PARALLEL with vm_backend::exec_member_compound)
    if (detail::math::is_kind(target_obj.raw_storage_index())) {
        auto stored = assign_math_component(objectValue, memberExpr, resultValue);
        if (!stored) {
            return stored.error_value();
        }
        push_value(expr->postfix_old_result ? std::move(postfix_old)
                   : expression_result_needed_ ? std::move(stored.value()) : std::move(resultValue));
        return {};
    }

    // Check if it's an object
    if (!target_obj.is_object()) {
        if (target_obj.parallel_object_borrow()) [[unlikely]] { return detail::parallel_shared_store_refusal(memberExpr->member_id).error_value(); }
        // Set exception state instead of throwing
        raise_script_text(non_object_member_store_text({}, {}), memberExpr->location);
        push_value(make_value());
        return {};
    }
    if (detail::class_temporary_member_store(memberExpr, objectValue, 2)) [[unlikely]] { return detail::class_temporary_member_error(); }   // objectValue and its dereferenced copy

    JAISCRIPT_TRY(assign_member_value(target_obj, memberExpr, resultValue));
    if (is_unwinding_) {
        push_value(make_value());
        return {};
    }
    if (expression_result_needed_ && !expr->postfix_old_result &&
        detail::member_reads_back(target_obj, memberExpr->member_id, string_symbolizer_->get_getter_id_with_view(memberExpr->member_id).first, engine_)) {
        JAISCRIPT_TRY(member_read_on(memberExpr, target_obj, false));   // the stored value, through a user _get_
        if (!is_unwinding_) { script_value got = pop_value(); push_value(clone_for_assignment(got.deref())); }
        return {};
    }
    push_value(expr->postfix_old_result ? std::move(postfix_old) : std::move(resultValue));
    return {};
}

// a op= b on any other lvalue (a subscript, an element of a call's result): through the reference its read minted
template<bool Replaying>
checked_result<void> interpreter::assign_compound_element(assignment_expr* expr, const replay_point* rs) {
    // General compound assignment for any expression
    // This handles subscripts, function calls that return references, etc.

    // C++17: the right-hand side is sequenced before the target: the value first, then the
    // subscript's container and index and the element's read (replay: rhs 0, target 1, partial {rhs}). KEEP
    // BYTE-PARALLEL with the vm's value-first index compound.
    script_value rightValue;
    if (parked<Replaying>(rs, 0)) {
        rightValue = rs->partial[0];
    } else {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_general, 0, {});
        if (is_unwinding_) {
            // RHS raised: surface the original exception, never touch the target
            push_value(make_value());
            return {};
        }
        rightValue = pop_value();
    }
    string_target_context_ = expr->target->get_type() == node_type::binary_expr &&
                             static_cast<binary_expr*>(expr->target.get())->op.type == token_type::left_bracket;
    auto target_read = eval_child<Replaying>(expr->target.get(), rs, 1);
    string_target_context_ = false;
    JAISCRIPT_TRY_OPERAND(std::move(target_read), expr, rp_asg_general, 1, (std::vector<script_value>{rightValue}));
    script_value currentValue = pop_value();
    if (is_unwinding_) {
        push_value(make_value());
        return {};
    }
    if (auto in_place = compound_in_place(currentValue, expr, rightValue)) {
        if (!*in_place) { return in_place->error_value(); }
        push_value(std::move(in_place->value()));
        return {};
    }
    script_value postfix_old = expr->postfix_old_result ? script_value(currentValue.deref()) : make_value();
    script_value resultValue = make_value();
    // a global operator answers first (the flat table; KEEP BYTE-PARALLEL with vm_backend::exec_index_compound); ++ and --
    // step numbers only
    const script_value* compound_op_fn = operator_table_ && !expr->incdec_desugar
        ? operator_table_->entry(detail::binary_op_slot(detail::compound_base(expr->op.type))) : nullptr;
    if (compound_op_fn) {
        auto result = call_value(compound_op_fn->as_function(), {currentValue, rightValue});
        if (!result) {
            return result.error_value();
        }
        resultValue = std::move(result.value());
    } else {
        // a bound rhs decodes before the zero divisor's check, which comes ahead of the operands' kinds
        if (rightValue.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
            rightValue = rightValue.bound_decoded_temp();
        }
        const size_t ri = rightValue.raw_storage_index();
        if (expr->op.type == token_type::slash_equal && ((ri == script_value::TYPEID_INT && rightValue.unchecked_as_int() == 0) ||
                                                        (ri == script_value::TYPEID_FLOAT && rightValue.unchecked_as_float() == 0.0))) {
            return checked_result<void>(make_error_code(runtime_error_code::division_by_zero), "Division by zero");
        }
        if (expr->op.type == token_type::percent_equal && ri == script_value::TYPEID_INT && rightValue.unchecked_as_int() == 0) {
            return checked_result<void>(make_error_code(runtime_error_code::modulo_by_zero), "Modulo by zero");
        }
        const bool text_slot = currentValue.is_reference() && currentValue.get_reference_holder() && currentValue.get_reference_holder()->string_element();
        JAISCRIPT_TRY_ASSIGN(resultValue, compound_value(detail::compound_base(expr->op.type), currentValue, rightValue, expr->incdec_desugar, text_slot));
    }

    if (currentValue.is_reference()) {
        // The target read minted the element reference: store through it, never
        // re-evaluate the subscript (its side effects, and any yield, ran once)
        JAISCRIPT_TRY(store_through_subscript_reference(currentValue, std::move(resultValue)));
        if (expr->postfix_old_result) {
            valueStack_.discard();
            push_value(std::move(postfix_old));
        }
    } else {
        // No lvalue came back (rvalue base, string subscript, missing map key): the
        // vm's exec_index_compound position and text (KEEP BYTE-PARALLEL)
        return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target), "Cannot assign to rvalue expression");
    }
    return {};
}

// x = v on a named target: its frame slot, an environment cell, a member of this or a static
checked_result<void> interpreter::assign_identifier(assignment_expr* expr, script_value value) {
    auto* identifier = static_cast<identifier_expr*>(expr->target.get());

    // a local or a parameter stores into its frame slot
    if (identifier->slot_index != SIZE_MAX && !call_stack_.empty()) {
        script_value* frameLocal = call_stack_.back().get_local(identifier->slot_index);
        if (frameLocal) {
            // A cell in the slot IS the variable's storage when the name is the
            // value decl itself (escape-boxed local): write the cell with the same
            // typed enforcement a plain slot gets. Ref-alias names (ref params /
            // auto& decls) and non-cell references keep store-through semantics.
            script_value* storage = frameLocal;
            bool store_through = false;
            if (frameLocal->is_reference()) {
                auto* holder = frameLocal->get_reference_holder();
                if (holder && holder->has_cell && identifier->names_value_decl) {
                    storage = holder->cell();
                } else {
                    store_through = true;
                }
            }
            if (store_through) {
                JAISCRIPT_TRY(detail::ref_store_through(*frameLocal, value, engine_, string_symbolizer_, identifier->declared_ref_kind ? identifier->declared_ref : nullptr, identifier->declared_ref_kind, identifier->name));
            } else {
                // Slot locals enforce their locked type like the environment path does
                // (same-type fast guard keeps the hot store path call-free)
                type_info_ptr slot_type = storage->get_type_info();
                if (detail::slot_value_assign_candidate(*storage) && (!slot_type || slot_type->base_type != script_value_type::jai_object_type)) {
                    const type_info* held_tag = storage->type_info_raw();
                    const bool declared_var = held_tag && held_tag->base_type == script_value_type::jai_shared_ptr_type && !held_tag->dynamic_pointee &&
                                              identifier->names_var_decl;
                    if (auto stored = detail::slot_value_assign(*storage, value, engine_, declared_var)) {   // the held object decides (KEEP BYTE-PARALLEL with vm_backend::held_object_store)
                        if (!*stored) { return stored->error_value(); }
                        if (stored->value() == detail::value_store::assigned) {
                            push_value(expression_result_needed_ ? clone_for_assignment(*storage) : std::move(value));
                        } else {
                            script_value rebound = clone_for_assignment(value);
                            detail::var_stamp(rebound, nullptr, engine_);
                            *storage = std::move(rebound);
                            push_value(std::move(value));
                        }
                        return {};
                    }
                }
                // A user operator= writes the held instance (KEEP BYTE-PARALLEL with vm_backend::store_popped_value)
                if (slot_type && slot_type->base_type == script_value_type::jai_object_type) {   // a proven same-class store copy-assigns too
                    if (auto assigned = detail::user_assign_operator(*storage, value, engine_, nullptr, nullptr, &value)) {
                        if (!*assigned) { return assigned->error_value(); }
                        push_value(expression_result_needed_ ? clone_for_assignment(*storage) : std::move(value));   // the expression's value is the destination's; a discarded one drops the source
                        return {};
                    }
                }
                JAISCRIPT_TRY_ASSIGN(value, converted_for_variable(slot_type, std::move(value), identifier->name));
                if (detail::uniquely_held(value)) {   // a prvalue is the stored object (KEEP BYTE-PARALLEL with vm_backend::store_popped_value)
                    *storage = std::move(value);
                    push_value(expression_result_needed_ ? clone_for_assignment(*storage) : script_value(*storage));
                    return {};
                }
                // Direct assignment to call frame local (or its cell); the
                // kernel deep-copies values and shares shared_ptr handles
                *storage = clone_for_assignment(value);
            }
            push_value(std::move(value));
            return {};
        }
    }

    // Member scope: a member (field, static, or method - shadowing is by NAME)
    // outranks everything past the function's own scopes, so an env hit that
    // could only be a global must yield to the member-store kernel below.
    bool member_wins = false;
    if (!identifier->global_qualified && member_scope_.active() &&
        !detail::own_scope_lookup(environment_.get(), member_scope_.floor, identifier->symbol_id)) {
        const auto store_member = detail::probe_member_scope(member_scope_.state, member_scope_.this_storage,
                                                             member_scope_.statics, identifier->symbol_id, /*want_methods*/ false);
        if (store_member) {
            JAISCRIPT_TRY(detail::bare_member_access(store_member, identifier->symbol_id, environment_->find_access_context()));
        }
        member_wins = store_member.what != detail::member_probe_hit::kind::miss ||
                      detail::member_scope_has_method(member_scope_.state, member_scope_.this_storage,
                                                      member_scope_.statics, identifier->symbol_id);
    }

    // ::name resolves and stores against the GLOBAL env only
    environment* ident_env = identifier->global_qualified
        ? get_global_environment().get() : environment_.get();

    // Get the current value to check if it's a reference (environment path)
    if (!member_wins && ident_env->contains(identifier->symbol_id)) {
        script_value* currentVal = ident_env->get_value_ptr(identifier->symbol_id);
        bool boxed_cell = false;
        bool store_through = false;
        if (currentVal && currentVal->is_reference()) {
            // Escape-boxed variable (cell) named as itself: redirect to the cell
            // inner and run the full assignment semantics on it; anything else
            // stores through
            auto* holder = currentVal->get_reference_holder();
            if (holder && holder->has_cell && identifier->names_value_decl) {
                currentVal = holder->cell();
                boxed_cell = true;
            } else {
                store_through = true;
            }
        }
        // Boxed storage writes the cell inner directly (an env assign would
        // replace the handle and detach every alias)
        auto store_back = [&](script_value&& v) -> checked_result<void> {
            if (boxed_cell) {
                *currentVal = std::move(v);
                return {};
            }
            return ident_env->assign(identifier->symbol_id, std::move(v));
        };
        // The value of a store through an alias, a bound variable or a handle: a consumed one is the destination's
        // stored value; the statement root hands the host its right-hand side
        const bool handle_rows = store_through || (currentVal && (currentVal->is_cpp_bound() || currentVal->is_weak_ptr() ||
            (currentVal->get_type_info() && currentVal->get_type_info()->base_type == script_value_type::jai_shared_ptr_type)));
        std::optional<script_value> host_result;
        if (handle_rows && !expression_result_needed_) { host_result = value; }
        script_value* const stored_cell = boxed_cell ? currentVal : nullptr;
        auto push_stored = [&]() {
            if (host_result) { push_value(std::move(*host_result)); return; }
            const script_value* stored = stored_cell ? stored_cell : ident_env->get_value_ptr(identifier->symbol_id);
            push_value(stored ? clone_for_assignment(stored->deref()) : make_value());
        };
        if (store_through) {
            // This is a reference - assign through it (deep copy the value)
            JAISCRIPT_TRY(detail::ref_store_through(*currentVal, value, engine_, string_symbolizer_, identifier->declared_ref_kind ? identifier->declared_ref : nullptr, identifier->declared_ref_kind, identifier->name));
        } else if (currentVal && currentVal->is_cpp_bound()) {
            // This is a C++ bound value - use assign_through
            currentVal->assign_through(value);
        } else {
            // the declared type converts the value (a var's or a handle's held object may take it first, a class value's
            // operator= runs); a handle's expression value is push_stored's
            type_info_ptr target_type = currentVal->get_type_info();
            auto push_result = [&](script_value result) {
                if (handle_rows) { push_stored(); } else { push_value(std::move(result)); }
            };
            if (!detail::handle_rebinds(*currentVal, value) && detail::slot_value_assign_candidate(*currentVal) &&
                (!target_type || target_type->base_type != script_value_type::jai_object_type)) {
                if (auto stored = detail::slot_value_assign(*currentVal, value, engine_)) {   // the held object decides
                    if (!*stored) { return stored->error_value(); }
                    if (stored->value() == detail::value_store::rebind) {
                        script_value rebound = clone_for_assignment(value);
                        detail::var_stamp(rebound, nullptr, engine_);
                        JAISCRIPT_TRY(store_back(std::move(rebound)));
                        push_result(std::move(value));
                    } else {
                        push_result(expression_result_needed_ ? clone_for_assignment(*currentVal) : std::move(value));
                    }
                    return {};
                }
            }
            // A user operator= writes the held instance (KEEP BYTE-PARALLEL with vm_backend::store_popped_value)
            if (target_type && target_type->base_type == script_value_type::jai_object_type) {
                if (auto assigned = detail::user_assign_operator(*currentVal, value, engine_, nullptr, nullptr, &value)) {
                    if (!*assigned) { return assigned->error_value(); }
                    push_result(expression_result_needed_ ? clone_for_assignment(*currentVal) : std::move(value));   // the expression's value is the destination's; a discarded one drops the source
                    return {};
                }
            }
            JAISCRIPT_TRY_ASSIGN(value, converted_for_variable(target_type, std::move(value), identifier->name));
            // an lvalue right-hand side is copied (the kernel deep-copies values and shares handles), a temporary moves in
            if (is_lvalue_expression(expr->value.get())) {
                JAISCRIPT_TRY(store_back(clone_for_assignment(value)));
                push_result(std::move(value));
            } else if (boxed_cell) {
                JAISCRIPT_TRY(store_back(std::move(value)));
                push_result(*currentVal);   // the cell inner IS the stored value
            } else {
                JAISCRIPT_TRY(ident_env->assign(identifier->symbol_id, std::move(value)));
                script_value* stored = ident_env->get_value_ptr(identifier->symbol_id);
                push_result(stored ? *stored : make_value());   // shallow: the stored value, as C++ yields the lvalue
            }
            return {};
        }
        if (handle_rows) {
            if (is_unwinding_) { push_value(make_value()); } else { push_stored(); }
        }
    } else {
        // ::name targets never reach members - a global miss is an error
        if (identifier->global_qualified) {
            return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                environment_->undefined_text(identifier->symbol_id), identifier->symbol_id);
        }
        // Variable doesn't exist in environment
        // Try static_method_environment's assign (which handles static fields)
        // or instance method's 'this' field assignment
        bool assigned_to_member = false;

        // Determine if RHS is lvalue (needs clone) or temporary (can move)
        const bool is_lvalue_read = is_lvalue_expression(expr->value.get());

        script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id());
        if (this_ptr) {
            script_value& this_val = *this_ptr;
            if (this_val.is_object()) {
                auto obj_holder = this_val.get_object_holder();
                if (obj_holder->is_instance()) {
                    auto instance = obj_holder->instance_handle();

                    // First try instance fields
                    if (instance->has_field(identifier->symbol_id)) {
                        // A hybrid's C++ property stores through its setter, exactly like
                        // this.name = v (a same-named script field shadows it)
                        if (uint64_t setter_id = detail::cpp_property_setter_id(instance.get(), identifier->symbol_id)) {
                            JAISCRIPT_TRY(detail::write_cpp_property(this_val, instance->get_class_definition(), setter_id, value));
                            assigned_to_member = true;
                        }
                        // Not a C++ property or no setter - use regular field assignment
                        // (typed fields enforce like locals)
                        if (!assigned_to_member) {
                            // All-detach rule: the implicit-this
                            // MOVE path stores a detached snapshot like the
                            // explicit o.f = spelling
                            if (value.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
                                value = value.detached_for_store();
                            }
                            if (is_lvalue_read) {
                                auto enforced = instance->field_store(identifier->symbol_id, value, [&] { return clone_for_assignment(value); });
                                if (!enforced) {
                                    return enforced.error_value();
                                }
                                instance->set_field_unchecked(identifier->symbol_id, enforced.value());
                            } else {
                                auto enforced = instance->field_store(identifier->symbol_id, value, [&] { return std::move(value); }, &value);
                                if (!enforced) {
                                    return enforced.error_value();
                                }
                                instance->set_field_unchecked(identifier->symbol_id, enforced.value());
                                value = instance->get_field(identifier->symbol_id);  // Shallow copy for return
                            }
                            assigned_to_member = true;
                        }
                    }
                    // Then try static fields (typed statics enforce like fields)
                    else {
                        class_definition* class_def = detail::bare_static_owner(bare_declaring_class(), instance->get_class_definition(),
                                                                                identifier->symbol_id);
                        if (class_def) {
                            // an implicit static store stores a detached snapshot too
                            if (value.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
                                value = value.detached_for_store();
                            }
                            if (is_lvalue_read) {
                                auto stored = detail::store_static_member(*class_def, identifier->symbol_id, value, [&] { return clone_for_assignment(value); });
                                if (!stored) {
                                    return stored.error_value();
                                }
                                if (expression_result_needed_) { value = clone_for_assignment(stored.value()); }   // the stored value is the expression's
                            } else {
                                auto stored = detail::store_static_member(*class_def, identifier->symbol_id, value, [&] { return std::move(value); });
                                if (!stored) {
                                    return stored.error_value();
                                }
                                value = std::move(stored.value());
                            }
                            assigned_to_member = true;
                        }
                    }
                }
            }
        }

        // Static-method scope: no 'this' to route through, and a nested block's env
        // assign would raw-write the static cell it finds through the scope env - land
        // the store on the scope's class (KEEP BYTE-PARALLEL with vm_backend
        // store_popped_value's statics tail)
        class_definition* static_owner = !assigned_to_member && member_scope_.state == detail::member_scope_state::statics
            ? detail::bare_static_owner(member_scope_.statics, nullptr, identifier->symbol_id) : nullptr;
        if (static_owner) {
            if (value.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
                value = value.detached_for_store();
            }
            if (is_lvalue_read) {
                auto stored = detail::store_static_member(*static_owner, identifier->symbol_id, value, [&] { return clone_for_assignment(value); });
                if (!stored) {
                    return stored.error_value();
                }
                if (expression_result_needed_) { value = clone_for_assignment(stored.value()); }   // the stored value is the expression's
            } else {
                auto stored = detail::store_static_member(*static_owner, identifier->symbol_id, value, [&] { return std::move(value); });
                if (!stored) {
                    return stored.error_value();
                }
                value = std::move(stored.value());
            }
            assigned_to_member = true;
        }

        if (!assigned_to_member) {
            // Use environment->assign() which will:
            // - For static_method_environment: check static fields, then return error if not found
            // - For method_environment: check 'this' fields, then define locally if not found
            // - For regular environment: return error if variable doesn't exist
            if (is_lvalue_read) {
                JAISCRIPT_TRY(environment_->assign(identifier->symbol_id, clone_for_assignment(value)));
            } else {
                JAISCRIPT_TRY(environment_->assign(identifier->symbol_id, std::move(value)));
                // Get back the stored value for the return (shallow copy)
                script_value* stored = environment_->get_value_ptr(identifier->symbol_id);
                value = stored ? *stored : make_value();  // Shallow copy, not clone
            }
        }
        push_value(std::move(value));
    }
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_assignment_expr_t(assignment_expr* expr, const replay_point* rs) {
    if (expr->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(expr, expr->const_check)); }
    // Non-lvalue targets fail before ANY evaluation (the vm rejects them at compile time)
    const node_type target_kind = expr->target->get_type();
    const bool through_reference = target_kind != node_type::identifier_expr && target_kind != node_type::member_expr &&
                                   target_kind != node_type::binary_expr && detail::reference_target(expr->target.get());
    if (target_kind != node_type::identifier_expr && target_kind != node_type::member_expr &&
        !(target_kind == node_type::binary_expr &&
          static_cast<binary_expr*>(expr->target.get())->op.type == token_type::left_bracket) && !through_reference) {
        return non_lvalue_target(expr->target.get());
    }
    // Only the statement's own root discards its value; a nested assignment's stored value is consumed
    struct result_needed_scope { bool& flag; bool saved; ~result_needed_scope() { flag = saved; } } result_needed{ expression_result_needed_, expression_result_needed_ };
    expression_result_needed_ = expr != unconsumed_root_;

    if (through_reference) [[unlikely]] {
        // cast<T&>(e), f() returning T&, a conditional of lvalues, an assignment, a prefix ++/--: the right-hand side first
        // (C++17), then the reference, then the store (detail::ref_target_assign; KEEP BYTE-PARALLEL with
        // vm_backend::exec_ref_target_store)
        script_value rightValue;
        if (parked<Replaying>(rs, 0)) {
            rightValue = rs->partial[0];
        } else {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_general, 0, {});
            if (is_unwinding_) {
                return {};
            }
            rightValue = pop_value();
            if (rightValue.is_reference()) {
                script_value derefed = rightValue.deref();
                rightValue = std::move(derefed);
            }
        }
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->target.get(), rs, 1), expr, rp_asg_general, 1, (std::vector<script_value>{rightValue}));
        script_value target_ref = pop_value();
        if (is_unwinding_) {
            push_value(make_value());
            return {};
        }
        if (!target_ref.is_reference()) {
            return non_lvalue_target(expr->target.get());
        }
        const token_type op = expr->op.type == token_type::equal ? token_type::equal : detail::compound_base(expr->op.type);
        if (op != token_type::equal) {
            if (auto current = detail::ref_checked_deref(target_ref); current) {
                if (auto in_place = compound_in_place(current.value(), expr, rightValue)) {
                    if (!*in_place) { return in_place->error_value(); }
                    push_value(expr->yields_reference ? std::move(target_ref) : std::move(in_place->value()));
                    return {};
                }
            }
        }
        auto stored = detail::ref_target_assign(target_ref, rightValue, op, expr->incdec_desugar, expr->postfix_old_result,
            expression_result_needed_ && !expr->yields_reference, operator_table_, engine_, string_symbolizer_,
            [this](const script_value& l, token_type o, const script_value& r) {
                return evaluate_arithmetic(l, o, r, detail::compound_index(o));
            });
        if (!stored) {
            return stored.error_value();
        }
        push_value(expr->yields_reference ? std::move(target_ref) : std::move(stored.value()));
        return {};
    }

    if (expr->op.type != token_type::equal) {
        if (expr->target->get_type() == node_type::identifier_expr) {
            return assign_compound_identifier<Replaying>(expr, rs);
        }
        if (expr->target->get_type() == node_type::member_expr) {
            return assign_compound_member<Replaying>(expr, rs);
        }
        return assign_compound_element<Replaying>(expr, rs);
    } else {

        script_value value;
        if (parked<Replaying>(rs, 0)) {
            value = rs->partial[0];   // evaluated before the yield: the rhs runs once
        } else {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_asg_regular, 0, {});
            // Check if we're unwinding due to an exception in the value expression
            if (is_unwinding_) {
                // Don't try to pop a value that wasn't pushed due to the exception
                return {};
            }
            value = pop_value();
        }

        // Element/subscript reads arrive as reference wrappers (rhs-lvalue read shape);
        // assignment consumes the VALUE - normalize like every other consumer, or the
        // typed-store check below sees 'reference' and rejects `int x; x = a[0]`
        // (KEEP BYTE-PARALLEL with vm_backend::exec_store). Copy out through a temp:
        // assigning value.deref() straight into value destroys the holder that OWNS
        // the deref target while the copy is still reading it.
        if (value.is_reference()) {
            script_value derefed = value.deref();
            value = std::move(derefed);
        }

        if (expr->target->get_type() == node_type::identifier_expr) {
            return assign_identifier(expr, std::move(value));
        }
        if (expr->target->get_type() == node_type::member_expr) {
            auto* memberExpr = static_cast<member_expr*>(expr->target.get());
            // Static member assignment: the shared kernel resolves the class and runs the
            // field-store table over the static's declared type (vm parity)
            if (memberExpr->is_static) {
                if (auto ns_target = detail::resolve_namespace_store(*engine_, memberExpr, *string_symbolizer_)) {
                    auto stored = store_namespace_cell(*ns_target, memberExpr, clone_for_assignment(value));
                    if (!stored) {
                        return stored.error_value();
                    }
                    push_value(expression_result_needed_ ? clone_for_assignment(stored.value()) : std::move(stored.value()));
                    return {};
                }
                auto cls = detail::resolve_static_store_class(memberExpr, *environment_, *string_symbolizer_,
                                                              environment_->find_access_context());
                if (!cls) {
                    return cls.error_value();
                }
                // The rhs is the one evaluated at the top of this branch: re-dispatching it
                // here ran every side effect twice (and fired a yield twice)
                auto stored = detail::store_static_member(*cls.value(), memberExpr->member_id, value, [&] { return clone_for_assignment(value); });
                if (!stored) {
                    return stored.error_value();
                }
                push_value(expression_result_needed_ ? clone_for_assignment(stored.value()) : std::move(stored.value()));   // a consumed static store is a copy of the stored value
                return {};
            }

            // Regular member assignment - evaluate the object (replay ordinal 1: the rhs is parked as partial[0])
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(memberExpr->object.get(), rs, 1), expr, rp_asg_regular, 1, (std::vector<script_value>{value}));
            script_value objectValue = pop_value();
            if (is_unwinding_) {
                push_value(make_value());
                return {};
            }

            // Dereference if it's a reference (e.g., from array[index])
            script_value dereferenced = objectValue.deref();

            // A math value's component lands in the storage the base names (detail/math_component_store.hpp;
            // KEEP BYTE-PARALLEL with vm_backend::store_member)
            if (detail::math::is_kind(dereferenced.raw_storage_index())) {
                auto stored = assign_math_component(objectValue, memberExpr, value);
                if (!stored) {
                    return stored.error_value();
                }
                push_value(expression_result_needed_ ? std::move(stored.value()) : std::move(value));
                return {};
            }

            // Unwrap shared_ptr if needed
            // After refactor: shared_ptr<T> uses same storage, no unwrapping needed

            // Check if it's an object
            if (!dereferenced.is_object()) {
                if (dereferenced.parallel_object_borrow()) [[unlikely]] { return detail::parallel_shared_store_refusal(memberExpr->member_id).error_value(); }
                // Set exception state instead of throwing
                raise_script_text(dereferenced.is_null() ? null_member_store_text(memberExpr->object.get(), memberExpr->member) : non_object_member_store_text(class_named_by_dot_receiver(memberExpr->object.get(), dereferenced, engine_),
                                                                       memberExpr->member), memberExpr->location);
                push_value(make_value());
                return {};
            }
            if (detail::class_temporary_member_store(memberExpr, objectValue, 2)) [[unlikely]] { return detail::class_temporary_member_error(); }   // objectValue and its dereferenced copy

            JAISCRIPT_TRY(assign_member_value(dereferenced, memberExpr, value, &value));   // the store's own operand (KEEP BYTE-PARALLEL with the vm's store_member)
            if (is_unwinding_) {
                push_value(make_value());
                return {};
            }
            if (expression_result_needed_ &&
                detail::member_reads_back(dereferenced, memberExpr->member_id, string_symbolizer_->get_getter_id_with_view(memberExpr->member_id).first, engine_)) {
                JAISCRIPT_TRY(member_read_on(memberExpr, dereferenced, false));   // the stored value, through a user _get_
                if (!is_unwinding_) { script_value got = pop_value(); push_value(clone_for_assignment(got.deref())); }
                return {};
            }
            push_value(std::move(value));  // discarded: nothing observes it
        }
        // Check if target is a subscript expression (array[index] or map[key])
        else if (expr->target->get_type() == node_type::binary_expr) {
            auto* binaryExpr = static_cast<binary_expr*>(expr->target.get());
            if (binaryExpr->op.type == token_type::left_bracket) {
                // Evaluate the entire target expression (e.g., nested["nums"][1])
                // This should return a reference if it's a valid lvalue.
                // Mark this as an assignment-target subscript so a map key is
                // auto-inserted (visit_binary clears the flag for nested reads).
                lvalue_target_context_ = true;
                auto target_eval = eval_child<Replaying>(expr->target.get(), rs, 1);
                lvalue_target_context_ = false;
                if (!target_eval) [[unlikely]] {
                    if (is_yield_signal(target_eval)) park_operands(expr, rp_asg_regular, 1, {value});
                    return target_eval.error_value();
                }
                script_value target_ref = pop_value();
                if (is_unwinding_) {
                    push_value(make_value());
                    return {};
                }
                
                // Check if we got a reference
                if (target_ref.is_reference()) {
                    JAISCRIPT_TRY(store_through_subscript_reference(target_ref, std::move(value)));
                } else {
                    // Not a reference - this means the subscript expression didn't
                    // return an lvalue (e.g., trying to assign to a function call result)
                    return checked_result<void>(make_error_code(runtime_error_code::invalid_assignment_target), "Cannot assign to rvalue expression");
                }
            } else {
                return non_lvalue_target(expr->target.get());
            }
        } else {
            return non_lvalue_target(expr->target.get());
        }
    }
    return {};
}

// Store through the reference a subscript target read minted. Mode-based re-resolution
// (never a cached address): realloc/erase between mint and write can't corrupt the heap.
// TYPED elements: scratch target + buffer commit (KEEP BYTE-PARALLEL with
// vm_backend::exec_index_assign). Leaves the assigned value on the stack.
checked_result<void> interpreter::store_through_subscript_reference(script_value& target_ref, script_value value) {
    auto refHolder = target_ref.get_reference_holder();
    if (refHolder && refHolder->string_element()) {
        JAISCRIPT_TRY(detail::store_string_element(*refHolder, value, engine_));
        push_value(expression_result_needed_ ? clone_for_assignment(value.deref()) : std::move(value));
        return {};
    }
    const bool typed_element = refHolder && refHolder->typed_element();
    script_value* target_ptr;
    if (typed_element) {
        if (refHolder->container_index >= refHolder->container->size()) {
            return checked_result<void>(make_error_code(runtime_error_code::invalid_reference), "Invalid reference in assignment");
        }
        target_ptr = const_cast<script_value*>(&refHolder->materialize_typed_element(engine_));
    } else {
        target_ptr = refHolder->resolve_target();
        if (!target_ptr) {
            return checked_result<void>(make_error_code(runtime_error_code::invalid_reference), "Invalid reference in assignment");
        }
    }

    // Validate element type if this is a container subscript reference
    type_info_ptr element_type = refHolder->container_element_type;
    // a class element keeps its object: the copy assignment (KEEP BYTE-PARALLEL with vm_backend::exec_index_assign)
    const auto in_place = detail::element_copy_assign(*target_ptr, value, element_type, engine_);
    if (in_place && !*in_place) { return *in_place; }
    if (in_place) {
    } else if (element_type) {
        if (!detail::container_element_compatible(value, element_type, engine_)) {
            return detail::element_type_error(string_symbolizer_, runtime_error_code::array_element_type_mismatch, "Cannot assign '{0}' to element of type '{1}'", value, element_type);
        }
        // Convert element if needed (e.g., int -> float for array<float>)
        auto converted = detail::convert_container_element_taking(engine_, value, element_type);
        if (!converted) { return converted.error_value(); }
        *target_ptr = std::move(converted.value());
    } else {
        // No element type constraint - values deep-copy, shared_ptr handles share
        *target_ptr = detail::take_for_assignment(value);
    }
    if (typed_element) {
        refHolder->container->set(refHolder->container_index, *target_ptr);
    }
    push_value(expression_result_needed_ ? clone_for_assignment(*target_ptr) : std::move(value));   // a copy of the stored element is the expression's value  // Assignment expressions return the assigned value
    return {};
}

// The pin a string subscript store target writes through (detail/string_element.hpp): the
// base's own reference, its field / property reference, or its variable cell boxed like a
// reference binding boxes it; null for a temporary base. KEEP BYTE-PARALLEL with
// vm_backend::string_element_pin.
script_value interpreter::string_element_pin(script_value& base_raw, const expression* base_expr) {
    if (base_raw.is_reference()) {
        return base_raw;
    }
    if (base_expr->get_type() == node_type::identifier_expr) {
        if (script_value* cell = string_receiver_cell(static_cast<const identifier_expr*>(base_expr))) {
            return share_boxed_slot(*cell);
        }
        return make_value();
    }
    if (detail::is_member_final_ref_lvalue(base_expr)) {
        auto resolved = detail::resolve_ref_lvalue(base_expr,
            detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
            environment_.get(), engine_, string_symbolizer_);
        if (resolved && resolved.value().is_reference()) {
            return std::move(resolved.value());
        }
    }
    return make_value();
}

// statement visitors
template<bool Replaying>
checked_result<void> interpreter::visit_expression_stmt_t(expression_stmt* stmt) {
    const expression* prev = std::exchange(unconsumed_root_, stmt->expression.get());
    auto result = eval_root<Replaying>(stmt->expression.get());
    unconsumed_root_ = prev;
    if (!result) [[unlikely]] {
        if (is_yield_signal(result)) {
            note_statement_yield(stmt, stmt->expression.get());
            return {};
        }
        return result.error_value();
    }

    // Early exit if exception is propagating
    if (is_unwinding_) return {};

    // Pop the result - expression statements don't produce values
    pop_value();
    return {};
}

checked_result<void> interpreter::visit_block_stmt(block_stmt* stmt) {
    auto previous = environment_;

    // Check if we're resuming into this block from a coroutine yield
    size_t start_index = 0;
    bool resuming = false;
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        auto* cont = coro_state.peek_continuation(stmt);
        if (cont) {
            start_index = cont->index;
            // Restore the environment that was saved when this block yielded.
            // This includes any inner scopes (for-loop, nested blocks) that
            // were active at the time of yield.
            if (cont->saved_env) {
                environment_ = cont->saved_env;
            }
            coro_state.pop_continuation();
            resuming = true;
            // DON'T create new scope - the saved environment already has it
        }
    }

    if (!resuming) {
        // Create a new child scope for the block
        // With lazy caching, this is O(1) - no flat_lookup_ copy needed
        environment_ = block_environment(environment_);
    }

    try {
        // The resumed statement consumes its own frames (expression replay); the rest run cold
        bool resume_entry = resuming;
        for (size_t i = start_index; i < stmt->declarations.size(); ++i) {
            auto result = resume_entry ? dispatch_decl_resuming(stmt->declarations[i].get())
                                       : dispatch_decl(stmt->declarations[i].get());
            resume_entry = false;

            // IMPORTANT: Clear value stack after each declaration to prevent accumulation
            // This ensures objects are destroyed at statement boundaries, not just at block exit
            // Variable declarations pop their values, but some other declarations (like expression_decl)
            // may leave values on the stack
            if (valueStack_.size() > 0) {
                valueStack_.clear();
            }

            if (!result) {
                kill_scope_slots(stmt->declarations);
                // Restore scope before returning
                if (!resuming || i > start_index) {
                    end_scope(environment_);
                }
                environment_ = previous;
                return result;
            }

            // Check for control flow: break, continue, return, or exceptions
            if (is_unwinding_ || hasBreakRequest_ || hasContinueRequest_ || hasReturnValue_) {
                break;
            }

            // On yield: record continuation and return without releasing environment
            if (hasYieldRequest_) {
                if (active_coroutine_) {
                    auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                    // If inner constructs (if/while/for/block) pushed continuations,
                    // we need to re-enter this statement on resume (save i).
                    // If no inner continuations exist, the yield was a direct child
                    // statement, so skip it on resume (save i + 1).
                    size_t resume_index = coro_state.has_continuations() ? i : i + 1;
                    // Save the current environment (may be deeper than this block's scope
                    // due to inner constructs like for-loops that didn't restore).
                    // On resume, we'll restore this to get back to the right depth.
                    coro_state.push_continuation(stmt, resume_index, environment_);
                }
                // Restore environment_ to previous so parent constructs see correct scope.
                environment_ = previous;
                return {};
            }
        }
    } catch (...) {
        // Restore scope even if an error occurs
        if (!resuming) {
            end_scope(environment_);
        }
        environment_ = previous;
        throw;
    }

    // Clear the value stack before releasing scope
    // This ensures any lingering object references on the stack are released
    // Block statements don't produce a value, so the stack should be cleared
    valueStack_.clear();
    kill_scope_slots(stmt->declarations);

    // Restore the previous scope - only release if we created it
    if (!resuming) {
        end_scope(environment_);
    }
    environment_ = previous;
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_variable_decl_t(variable_decl* decl) {
    if (decl->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(decl, decl->const_check)); }
    // a same-unit re-run of a program-scope declaration written without an initializer keeps the variable, an inline
    // declaration adopts the variable it finds (the vm's op_decl_keep)
    if ((decl->default_initialized || decl->is_inline) && decl->slot_index == SIZE_MAX) [[unlikely]] {
        if (decl->is_inline) {
            uint32_t owner = 0;
            const auto verdict = environment_->inline_declare(decl->name_id, owner);
            if (verdict == environment::inline_verdict::conflict) {
                return detail::inline_plain_conflict(*string_symbolizer_, decl->name_id, engine_->unit_label(owner));
            }
            if (verdict == environment::inline_verdict::adopt) { return {}; }
        } else if (!environment_->get_parent()) {
            if (const script_value* cell = environment_->same_unit_cell(decl->name_id); cell && detail::redeclaration_keeps_value(*decl, *cell)) { return {}; }
        }
    }
    // Helper to store value in slot (local) or environment (global)
    auto define_variable = [&](script_value value) -> checked_result<void> {
        // Escape-marked declarations box into a cell (reference_holder cell mode):
        // reference binding then shares the handle. Reference values pass through
        // untouched (ref decls alias; a cell never wraps a reference).
        if (decl->ref_escaping && !value.is_reference()) {
            value = script_value::make_cell_reference(std::move(value), engine_);
        }
        if (value.is_reference() && detail::reference_decl_checks_admission(*decl)) [[unlikely]] {
            if (auto rejected = detail::reference_decl_rejection(*decl, value, engine_)) {
                // a const T& over another type's lvalue binds a T made from it (KEEP BYTE-PARALLEL with the vm's define_ref_decl_value)
                if (!decl->is_const) { return *rejected; }
                auto held = detail::const_reference_temporary(*decl, script_value(value.deref()), engine_, [&](script_value v, const type_info_ptr& t) {
                    return enforce_type_compatibility(std::move(v), t, decl->name, detail::slot_kind::declaration);
                });
                if (!held) { return *rejected; }
                value = std::move(held.value());
            }
        }
        // `int& r = e`: a reference whose value reaches another binding (or that no slot holds) carries its declared type; a
        // slot-resident one is decided at its stores from the name (identifier_expr::declared_ref)
        if (decl->type && decl->type->is_reference() && value.is_reference() && !decl->declared_ref_storage_enforced &&
            (decl->ref_escaping || decl->slot_index == SIZE_MAX || call_stack_.empty())) {
            detail::ref_declare(value, decl->type->element_type().get(), engine_);
        }
        if (decl->slot_index != SIZE_MAX && !call_stack_.empty()) {
            if (decl->ref_escaping && !parallel_worker_) [[unlikely]] {
                // a loop's next iteration drops the cell the last one shared: checked for a cycle once the slot lets go
                detail::shared_cell_release replaced{engine_};
                if (const script_value* old = call_stack_.back().get_local(decl->slot_index)) { script_value::gather_shared_cells(old, 1, replaced.cells); }
                call_stack_.back().set_local(decl->slot_index, std::move(value));
                return {};
            }
            // Local variable with slot - use O(1) slot-based storage
            call_stack_.back().set_local(decl->slot_index, std::move(value));
        } else {
            // Global variable or no slot assigned - use environment
            if (!environment_->define_variable(decl->name_id, std::move(value))) [[unlikely]] { return detail::inline_redefinition(*string_symbolizer_, decl->name_id, decl); }
            if (decl->is_inline) [[unlikely]] { environment_->share(decl->name_id); }
            if (environment_->is_program_scope()) { engine_->mark_const_cell(environment_->get_env_var_ptr(decl->name_id), decl->is_const); }
        }
        return {};
    };

    // The type the declaration binds to: explicit, or inferred from a uniform literal
    type_info_ptr effective_decl_type = decl->type;

    // Check if this is a reference variable declaration
    bool is_reference = false;
    if (decl->type && decl->type->base_type == script_value_type::jai_reference_type) {
        is_reference = true;
    }
    
    // Check if this is a weak_ptr declaration
    bool is_weak_ptr = false;
    if (decl->type && decl->type->base_type == script_value_type::jai_weak_ptr_type) {
        is_weak_ptr = true;
    }
    
    // Check if this is a shared_ptr declaration
    bool is_shared_ptr = false;
    if (decl->type && decl->type->base_type == script_value_type::jai_shared_ptr_type) {
        is_shared_ptr = true;
    }
    
    if (is_weak_ptr) {
        script_value value = make_value();
        if (decl->initializer) {
            JAISCRIPT_TRY_ROOT(decl, decl->initializer.get());
            value = pop_value();
            if (is_unwinding_) { return {}; }   // a raising initializer wins over the declared-type check
            // an element read arrives as a reference wrapper: the declaration consumes the value (KEEP BYTE-PARALLEL with
            // vm_backend::exec_decl_var)
            if (value.is_reference()) {
                script_value derefed = value.deref();
                value = std::move(derefed);
            }
        }
        JAISCRIPT_TRY_ASSIGN(script_value weak, detail::weak_declaration_value(decl->type, std::move(value), engine_,
            detail::slot_site{detail::slot_kind::declaration, decl->name, {}}));
        JAISCRIPT_TRY(define_variable(std::move(weak)));
    } else if (is_shared_ptr) {
        // shared_ptr<T> variable - handle initialization.
        // shared_ptr<auto> (element_type null) INFERS the pointee from the initializer's
        // exact class, then enforces it exactly like the explicit spelling;
        // a null/missing initializer has nothing to infer.
        const bool infer_pointee = decl->type && !decl->type->element_type();
        if (!decl->initializer) {
            if (infer_pointee) {
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                    "Cannot infer shared_ptr<auto> pointee without an initializer - use var, or an explicit shared_ptr<T>");
            }
            // No initializer - create null shared_ptr
            script_value null_ptr = make_value();
            null_ptr.set_type_info(decl->type);  // Mark as shared_ptr type
            JAISCRIPT_TRY(define_variable(std::move(null_ptr)));
        } else {
            // Evaluate initializer
            JAISCRIPT_TRY_ROOT(decl, decl->initializer.get());
            script_value value = pop_value();
            if (is_unwinding_) { return {}; }   // a raising initializer (missing member) wins over the declared-type check; the vm's run loop sees the flag before exec_decl_var

            // Element/subscript reads arrive as reference wrappers (rhs-lvalue read
            // shape); `shared_ptr<T> t = arr[i]` must see the element, not 'reference'
            // (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
            if (value.is_reference()) {
                script_value derefed = value.deref();
                value = std::move(derefed);
            }

            // Handle different initialization cases
            if (value.is_null()) {
                if (infer_pointee) {
                    return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                        "Cannot infer shared_ptr<auto> pointee from null - use var, or an explicit shared_ptr<T>");
                }
                // Initialize with null - that's fine
                value.set_type_info(decl->type);  // Mark as shared_ptr type
                JAISCRIPT_TRY(define_variable(std::move(value)));
            } else if (value.is_weak_ptr()) {
                return checked_result<void>(make_error_code(runtime_error_code::invalid_weak_ptr_conversion), "Cannot initialize shared_ptr directly from weak_ptr");
            } else if (value.type() == script_value_type::jai_object_type ||
                      value.type() == script_value_type::jai_shared_ptr_type) {
                if (infer_pointee) {
                    // Take the initializer's exact class; thereafter the tag behaves
                    // exactly like the explicit spelling (reassignment enforcement
                    // reads it) - construct-and-share included.
                    std::string actual_class;
                    if (auto instance = value.get_class_instance()) {
                        actual_class = instance->get_class_name();
                    }
                    type_info* inferred = nullptr;
                    if (!actual_class.empty() && engine_) {
                        if (auto* pointee_ti = engine_->get_type_info_object(string_symbolizer_->intern(actual_class))) {
                            inferred = engine_->get_type_info_shared_ptr(pointee_ti);
                        }
                    }
                    if (!inferred) {
                        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                            "Cannot infer shared_ptr<auto> pointee from this initializer - use var, or an explicit shared_ptr<T>");
                    }
                    value.set_type_info(inferred);
                    JAISCRIPT_TRY(define_variable(std::move(value)));
                    return {};
                }
                // Typed shared_ptr declarations enforce their pointee class (handle admission: a typed source by its
                // static pointee, a var-held one by its object's class; detail::shared_handle_admits)
                // (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
                if (!detail::shared_handle_admits(value, decl->type, engine_)) {
                    return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                        "Cannot initialize shared_ptr<{0}> with {1}: type must match or be a subclass",
                        decl->type->element_type()->id, string_symbolizer_->intern(detail::shared_handle_judged_class(value, engine_)));
                }
                // Initialize with object/shared_ptr - that's fine, objects are already shared_ptr
                // Mark the type as shared_ptr to ensure reference semantics
                value.set_type_info(decl->type);
                JAISCRIPT_TRY(define_variable(std::move(value)));
            } else {   // a primitive or a container: the declaration's rejection (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
                return detail::typed_slot_rejection(value, *decl->type,
                    detail::slot_site{detail::slot_kind::declaration, decl->name, {}}, engine_);
            }
        }
    } else if (is_reference) {
        // Check if initializer is an identifier (can take reference)
        if (decl->initializer->get_type() == node_type::identifier_expr) {
            auto* identExpr = static_cast<identifier_expr*>(decl->initializer.get());
            // Get the target variable's address
            uint64_t targetSymbolId = string_symbolizer_->intern(identExpr->name);

            // SLOT-aware resolution: plain function locals live
            // in frame slots, not the environment - the escape marker cell-boxes the
            // SLOT, so the binder must look there first or `auto& r = x` errors
            // "undefined variable" for every slot-resident local. Env storage is
            // pointer-stable (unordered_map); slots are stable within the frame and
            // the share below boxes into an escape-legal cell immediately.
            script_value* targetPtr = resolve_local_or_env(identExpr->slot_index, targetSymbolId, identExpr->global_qualified);
            if (!targetPtr) {
                // A hybrid's bare C++ property: the member itself / its accessors
                // (KEEP BYTE-PARALLEL with the vm's exec_decl_ref_ident)
                if (auto minted = bind_bare_cpp_property_ref(targetSymbolId, identExpr->global_qualified)) {
                    if (!*minted) {
                        return minted->error_value();
                    }
                    JAISCRIPT_TRY(define_variable(std::move(minted->value())));
                    return {};
                }
                JAISCRIPT_TRY(bare_reference_access(targetSymbolId));
                if (auto method = bare_method_reference_error(identExpr)) { return *method; }
                return checked_result<void>(make_error_code(runtime_error_code::undefined_variable), environment_->undefined_text(targetSymbolId, "Cannot take reference of undefined variable"), targetSymbolId);
            }
            
            // Check if the target is itself a reference
            if (targetPtr->is_reference()) {
                auto aliased = decl_ref_alias(*targetPtr, engine_);
                if (!aliased) {
                    return aliased.error_value();
                }
                JAISCRIPT_TRY(define_variable(std::move(aliased.value())));
            } else {
                // Box the variable's storage into a cell and alias it (escape-legal:
                // the ref keeps the cell alive after the env dies)
                JAISCRIPT_TRY(define_variable(share_boxed_slot(*targetPtr)));
            }
        } else {
            // C::s and ns::x are lvalues: the reference binds their storage (KEEP BYTE-PARALLEL with the vm's exec_decl_ref_value)
            if (decl->initializer->get_type() == node_type::member_expr && static_cast<const member_expr*>(decl->initializer.get())->is_static) {
                if (auto cell = detail::static_member_lvalue(*engine_, static_cast<const member_expr*>(decl->initializer.get()), *environment_,
                                                             *string_symbolizer_, environment_->find_access_context())) {
                    JAISCRIPT_TRY_ROOT(decl, decl->initializer.get());   // the read's effects and errors come first, as the vm evaluates it
                    (void)pop_value();
                    if (is_unwinding_) { return {}; }
                    if (!*cell) { return cell->error_value(); }
                    script_value& storage = *cell->value();
                    JAISCRIPT_TRY(define_variable(share_boxed_slot(storage)));
                    return {};
                }
            }
            // For other expressions, evaluate them and check if they return a reference
            JAISCRIPT_TRY_ROOT(decl, decl->initializer.get());
            script_value result = pop_value();
            if (is_unwinding_) { return {}; }   // a raising initializer (missing member) wins over the declared-type check; the vm's run loop sees the flag before exec_decl_var
            
            // If the result is a reference, alias it (share the holder)
            if (result.is_reference()) {
                auto aliased = decl_ref_alias(result, engine_);
                if (!aliased) {
                    return aliased.error_value();
                }
                JAISCRIPT_TRY(define_variable(std::move(aliased.value())));
            } else if (detail::is_member_final_ref_lvalue(decl->initializer.get())) {
                // Member-final initializer (auto& p = G.player): field reads evaluate to
                // COPIES, so the evaluated result can't alias - resolve the chain through
                // the shared kernel into an owner-pinned FIELD reference instead (route-
                // independence with ref params/returns). Access/
                // removed-field errors surface as-is; the kernel's generic non-lvalue
                // verdict (computed script property, non-object receiver) keeps the
                // decl's own error text below.
                // KEEP BYTE-PARALLEL with the vm's exec_decl_ref_value
                auto resolved = detail::resolve_ref_lvalue(decl->initializer.get(),
                    detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
                    environment_.get(), engine_, string_symbolizer_);
                if (!resolved && resolved.error() != make_error_code(runtime_error_code::invalid_reference)) {
                    return resolved.error_value();
                }
                if (resolved) {
                    auto aliased = decl_ref_alias(resolved.value(), engine_);
                    if (!aliased) {
                        return aliased.error_value();
                    }
                    JAISCRIPT_TRY(define_variable(std::move(aliased.value())));
                } else {
                    auto part = declared_component_reference(decl, &result);
                    if (!part) {
                        return part.error_value();
                    }
                    JAISCRIPT_TRY(define_variable(std::move(part.value())));
                }
            } else if (result.is_non_owning_object()) {
                // a bound object a host returned by reference (KEEP BYTE-PARALLEL with the vm's exec_decl_ref_value)
                JAISCRIPT_TRY(define_variable(script_value::make_referent_reference(result, engine_)));
            } else {
                // KEEP BYTE-PARALLEL with the vm's exec_decl_ref_value
                auto part = declared_component_reference(decl, &result);
                if (!part) {
                    return part.error_value();
                }
                JAISCRIPT_TRY(define_variable(std::move(part.value())));
            }
        }
    } else {
        // Regular variable declaration
        script_value value = make_value();
        if (decl->initializer) {
            JAISCRIPT_TRY_ROOT(decl, decl->initializer.get());
            value = pop_value();
            if (is_unwinding_) { return {}; }   // a raising initializer (missing member) wins over the declared-type check; the vm's run loop sees the flag before exec_decl_var

            // Element/subscript reads arrive as reference wrappers (rhs-lvalue read
            // shape) - and so do ternaries over them, which is_lvalue_expression can't
            // see. Normalize to the VALUE and treat the read as an lvalue read, or the
            // typed enforcement below sees 'reference' and the stored local silently
            // ALIASES the element (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
            const bool reference_init = value.is_reference();
            if (reference_init) {
                script_value derefed = value.deref();
                value = std::move(derefed);
            }

            // Only copy if initializing from an lvalue (existing object); temporaries
            // move. The kernel deep-copies values and shares shared_ptr handles; a proven
            // read-only local binds the object itself (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
            if ((reference_init || is_lvalue_expression(decl->initializer.get())) && !detail::slot_converts_object(value, decl->type.get())) {
                if (decl->alias_home && detail::local_alias_binds(*engine_, *decl, value, !engine_->parallel_region_active())) {
                    if (!parallel_worker_) { engine_->note_local_alias_bind(); }
                } else {
                    value = clone_for_assignment(value);
                }
            }
            // else: Initializing from a temporary - use move semantics (no copy)
            if (value.raw_storage_index() == script_value::TYPEID_OBJECT) {
                detail::object_declaration_takes_value(value, decl->type.get());
            }

            // === HOMOGENEOUS CONTAINER VALIDATION ===
            // For auto x = [...] and array<auto> x = [...], all elements must be the same type
            // For var x = [...] and array<var> x = [...], heterogeneous elements are allowed
            // Also validates maps: auto m = {...} requires homogeneous values
            // Recursively validates nested containers to arbitrary depth
            if (value.is_array() || value.is_map()) {
                bool requires_homogeneity = false;

                if (!decl->type) {
                    // auto x = [...] or auto m = {...} - requires homogeneous elements
                    requires_homogeneity = true;
                } else if (decl->type->is_array()) {
                    // array<...> declaration
                    auto element_type = decl->type->element_type();
                    if (!element_type) {
                        // array<auto> - requires homogeneous elements
                        requires_homogeneity = true;
                    } else if (element_type->base_type == script_value_type::jai_any_type) {
                        // array<var> - heterogeneous allowed
                        requires_homogeneity = false;
                    }
                    // else: array<T> - type validation handled elsewhere
                } else if (decl->type->is_map()) {
                    // map<...> declaration
                    auto value_type = decl->type->value_type();
                    if (!value_type) {
                        // map<K, auto> - requires homogeneous values
                        requires_homogeneity = true;
                    } else if (value_type->base_type == script_value_type::jai_any_type) {
                        // map<K, var> - heterogeneous values allowed
                        requires_homogeneity = false;
                    }
                    // else: map<K, V> - type validation handled elsewhere
                } else if (decl->type->base_type == script_value_type::jai_any_type) {
                    // var x = [...] or var m = {...} - heterogeneous allowed
                    requires_homogeneity = false;
                }

                if (requires_homogeneity) {
                    JAISCRIPT_TRY(detail::validate_container_homogeneous(value, arity_count_ids_, *string_symbolizer_));
                    // A uniform primitive literal infers the element type and takes its
                    // representation (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
                    if (value.is_array()) {
                        if (type_info_ptr inferred = detail::infer_uniform_array_type(value, engine_)) {
                            effective_decl_type = inferred;
                        }
                    }
                }
                // Map twin: `auto` key/value slots infer from a uniform literal; mixed keys
                // raise (KEEP BYTE-PARALLEL with vm_backend::exec_decl_var)
                if (value.is_map()) {
                    int mixed_part = 0;
                    if (type_info_ptr inferred = detail::infer_uniform_map_type(value, decl->type, engine_, mixed_part)) {
                        effective_decl_type = inferred;
                    } else if (mixed_part != 0) {
                        return checked_result<void>(
                            make_error_code(mixed_part == 1 ? runtime_error_code::map_key_type_mismatch : runtime_error_code::map_value_type_mismatch),
                            mixed_part == 1 ? "Map 'auto' requires homogeneous keys" : "Map 'auto' requires homogeneous values");
                    }
                }
            }
        }
        // If no initializer, value remains null

        // === STRONG TYPES: Set type_info based on declaration ===
        // - Explicit type (int, float, var, etc.): Use declared type (locks the variable)
        // - auto with initializer: Infer type from value (locks to inferred type)
        // - auto without initializer: Keep nullptr (first assignment will lock type)
        if (effective_decl_type) {
            // Explicit type declaration (int x, float y, var z, etc.), or the element
            // type a uniform literal inferred for auto / array<auto>
            // Typed declarations convert their initializer exactly like assignment does
            // (int d = 4.7 truncates instead of storing a mistagged float payload)
            if (decl->initializer) {
                auto enforced = enforce_type_compatibility(std::move(value), effective_decl_type, decl->name, detail::slot_kind::declaration);
                if (!enforced) {
                    return enforced.error_value();
                }
                value = std::move(enforced.value());
            }
            // Set declared type - this locks the variable's type. Exception: a 'var' decl
            // keeps a shared_ptr-tagged initializer's marker — `var p = new P()` IS
            // `shared_ptr<P> p = P()`, so the reference-semantics
            // tag must survive the decl instead of flattening to 'any'
            if (effective_decl_type->base_type == script_value_type::jai_any_type) {
                detail::var_stamp(value, effective_decl_type, engine_);   // a handle and a typed array keep their types, rebindable
            } else {
                value.set_type_info(effective_decl_type);
            }
        } else if (decl->initializer && value.get_type_info()) {
            // auto with initializer - type is inferred from value and locked
            // Value already has type_info from make_value() or evaluation
            // Keep the value's type_info (already set). Exception: a var-held handle's
            // rebindable marker does NOT transfer - auto is infer-then-enforce
            if (value.get_type_info()->base_type == script_value_type::jai_shared_ptr_type &&
                value.get_type_info()->dynamic_pointee && engine_ &&
                value.get_type_info()->element_type()) {
                value.set_type_info(engine_->get_type_info_shared_ptr(
                    value.get_type_info()->element_type().get()));
            }
            detail::drop_var_slot(value, engine_);   // nor does a var-held array's
        }
        // else: auto without initializer - type_info remains nullptr (uninitialized)
        // First assignment will lock the type

        // Define in slot (local) or environment (global)
        JAISCRIPT_TRY(define_variable(std::move(value)));
        // After move, value is in moved-from state, so don't access it
    }
    return {};
}

// === STRONG TYPES: Type enforcement for assignment ===
// The one typed-slot table (detail::convert_for_typed_slot); this site owns the assignment
// texts. KEEP BYTE-PARALLEL with vm_backend::enforce_type_compatibility.
// A namespace variable's store (ns::x = v, op=, ++): the scope's cell takes the value under the same typed
// enforcement an environment variable gets, a reference-declared variable stores through, a host-registered
// entry is read-only. KEEP BYTE-PARALLEL with vm_backend::store_namespace_cell.
checked_result<script_value> interpreter::store_namespace_cell(const detail::namespace_store_target& target, const member_expr* member, script_value incoming) {
    if (target.missing) {
        return detail::namespace_member_missing(*environment_, member, *string_symbolizer_);
    }
    if (!target.cell) {
        return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch),
            "Cannot assign to '{0}': it is read-only", member->member_id);
    }
    script_value* storage = target.cell;
    if (storage->is_reference()) {
        auto* holder = storage->get_reference_holder();
        if (holder && holder->has_cell) {
            storage = holder->cell();
        } else {
            JAISCRIPT_TRY(detail::ref_store_through(*storage, incoming, engine_, string_symbolizer_));
            return incoming;
        }
    }
    type_info_ptr slot_type = storage->get_type_info();
    if (slot_type && slot_type->base_type == script_value_type::jai_object_type) {
        if (auto assigned = detail::user_assign_operator(*storage, incoming, engine_)) {
            if (!*assigned) { return assigned->error_value(); }
            return clone_for_assignment(*storage);   // the operator wrote the held instance
        }
    }
    if (slot_type && (slot_type->base_type != incoming.type() ||
                      slot_type->base_type == script_value_type::jai_object_type ||
                      slot_type->base_type == script_value_type::jai_array_type ||
                      slot_type->base_type == script_value_type::jai_map_type ||
                      (slot_type->base_type == script_value_type::jai_shared_ptr_type && incoming.get_type_info().get() != slot_type.get()))) {
        auto enforced = enforce_type_compatibility(std::move(incoming), slot_type, member->member);
        if (!enforced) {
            return enforced.error_value();
        }
        incoming = std::move(enforced.value());
        detail::keep_handle_slot_type(incoming, slot_type);
        if (detail::var_slot(slot_type.get())) { detail::var_stamp(incoming, slot_type, engine_); }
    }
    *storage = std::move(incoming);
    return *storage;
}

// The value a store into a named variable writes, converted by the variable's declared type (the tag its value carries): a typed
// handle keeps the declared tag through every store, a var takes its marker and a var-held handle stays rebindable (KEEP
// BYTE-PARALLEL with vm_backend::store_popped_value)
checked_result<script_value> interpreter::converted_for_variable(const type_info_ptr& tag, script_value value, std::string_view name) {
    if (!tag) {
        return value;
    }
    const bool handle_slot = tag->base_type == script_value_type::jai_shared_ptr_type || tag->base_type == script_value_type::jai_weak_ptr_type;
    if (handle_slot && value.is_null()) {
        if (tag->base_type == script_value_type::jai_weak_ptr_type) {
            return script_value::make_empty_weak_ptr(tag, engine_);
        }
        value.set_type_info(tag);
        return value;
    }
    if (tag->base_type == script_value_type::jai_shared_ptr_type && tag->dynamic_pointee && value.get_type_info() &&
        value.get_type_info()->base_type == script_value_type::jai_shared_ptr_type) {
        if (value.get_type_info()->element_type() && !value.get_type_info()->dynamic_pointee) {
            value.set_type_info(engine_->get_type_info_shared_ptr_dynamic(value.get_type_info()->element_type().get()));
        }
        return value;
    }
    if (!detail::var_slot(tag.get()) && !handle_slot && detail::same_type_payload(value, tag.get())) {
        return value;
    }
    JAISCRIPT_TRY_ASSIGN(value, enforce_type_compatibility(std::move(value), tag, name));
    detail::keep_handle_slot_type(value, tag);
    if (detail::var_slot(tag.get())) { detail::var_stamp(value, tag, engine_); }
    return value;
}

checked_result<script_value> interpreter::enforce_type_compatibility(
    script_value value,
    type_info_ptr target_type,
    std::string_view var_name,
    detail::slot_kind kind
) {
    // nullptr target (auto x;) locks to the source type; any (var x) takes anything
    if (!target_type || detail::var_slot(target_type.get())) {
        return std::move(value);
    }
    {   // same-storage scalars are the table's identity row
        const script_value& seen = value.is_reference() ? value.deref() : value;
        size_t want = script_value::TYPEID_INVALID;
        switch (target_type->base_type) {
            case script_value_type::jai_int_type: want = script_value::TYPEID_INT; break;
            case script_value_type::jai_float_type: want = script_value::TYPEID_FLOAT; break;
            case script_value_type::jai_string_type: want = script_value::TYPEID_STRING; break;
            case script_value_type::jai_bool_type: want = script_value::TYPEID_BOOL; break;
            case script_value_type::jai_char_type: want = script_value::TYPEID_CHAR; break;
            default: break;
        }
        if (want != script_value::TYPEID_INVALID && seen.raw_storage_index() == want) {
            return std::move(value);
        }
    }
    // the value is this call's own: a temporary nothing else holds is a converting constructor's parameter
    auto outcome = detail::convert_for_typed_slot(value, target_type, engine_, detail::uniquely_held(value) ? &value : nullptr);
    if (!outcome) {
        return outcome.error_value();
    }
    using verdict = detail::slot_conversion::verdict;
    detail::slot_conversion& out = outcome.value();
    switch (out.v) {
        case verdict::identity:
        case verdict::identity_exact:
            return std::move(value);
        case verdict::converted:
            return std::move(*out.value);
        case verdict::container_mismatch: {
            auto* sym = engine_->get_symbolizer();
            return checked_result<script_value>(
                make_error_code(runtime_error_code::array_element_type_mismatch),
                detail::container_boundary_mismatch_text,
                sym->intern(out.mismatch->first), sym->intern(out.mismatch->second));
        }
        case verdict::handle_target:
            // shared_ptr<T> / weak_ptr<T> `=`: a same-kind tag stores as-is when a shared handle's judged class is T or a
            // subclass; a shared handle downgrades into a weak_ptr<T> slot exactly as the declaration form does (outside the table)
            if (value.type() == target_type->base_type &&
                (target_type->base_type != script_value_type::jai_shared_ptr_type || detail::shared_handle_admits(value, target_type, engine_))) {
                return std::move(value);
            }
            if (detail::weak_slot_takes_shared(value, target_type, engine_)) {
                return script_value::make_weak_ptr(value, engine_);
            }
            [[fallthrough]];
        default:
            return detail::typed_slot_rejection(value, *target_type, detail::slot_site{kind, var_name, {}}, engine_);
    }
}

// Compound store-back: locked targets convert the promoted result like '=' does; var
// targets keep the promoted result and re-tag it 'any' so the variable stays dynamic.
// KEEP BYTE-PARALLEL with vm_backend::compound_typed_store_back.
checked_result<void> interpreter::compound_typed_store_back(script_value& target, script_value promoted, std::string_view var_name) {
    type_info_ptr tag = target.get_type_info();
    if (tag && !detail::var_slot(tag.get())) {
        auto enforced = enforce_type_compatibility(std::move(promoted), tag, var_name);
        if (!enforced) {
            return enforced.error_value();
        }
        promoted = std::move(enforced.value());
        promoted.set_type_info(tag);
    } else if (tag) {
        detail::var_stamp(promoted, tag, engine_);
    }
    target = std::move(promoted);
    return {};
}

// Helper to convert value to string, checking for to_string() method on objects
// Class operators on a parallel worker: dispatched only through the region's pin of the operator. KEEP BYTE-PARALLEL with the vm's twins
const detail::parallel_method_pin* interpreter::worker_operator_pin(const script_value& v, uint64_t id) const {
    const class_definition* cd = detail::operand_instance_class(v);
    return parallel_method_pins_ && cd ? parallel_method_pins_->find_operator(cd, id) : nullptr;
}
bool interpreter::worker_operators_pinned(const script_value& v, std::initializer_list<uint64_t> ids) const {
    const class_definition* cd = detail::operand_instance_class(v);
    if (!cd) { return true; }
    for (const uint64_t id : ids) {
        if (cd->defines_method(id) && !(parallel_method_pins_ && parallel_method_pins_->find_operator(cd, id))) { return false; }
    }
    return true;
}
std::string_view interpreter::worker_operator_refusal(const script_value& v, std::initializer_list<uint64_t> ids) const {
    return detail::parallel_operator_refusal(parallel_method_pins_, detail::operand_instance_class(v), ids);
}
script_value interpreter::operator_value(const member_target& target, uint64_t id) const {
    if (!parallel_worker_) { return target.method(id); }
    const detail::parallel_method_pin* pin = parallel_method_pins_ && target.class_def ? parallel_method_pins_->find_operator(target.class_def, id) : nullptr;
    return pin ? pin->method_value : script_value::make_invalid(nullptr);
}
checked_result<script_value> interpreter::operator_call(const script_value& self, const script_value& method_val, uint64_t id, std::vector<script_value> args) {
    if (!parallel_worker_) { return call_value(create_bound_method(self, method_val).as_function(), args); }
    const detail::parallel_method_pin* pin = worker_operator_pin(self, id);
    if (!pin) { return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation), worker_operator_refusal(self, { id })); }
    return detail::call_parallel_operator_pin(*this, *pin, id, self, std::move(args), engine_);
}
std::optional<checked_result<script_value>> interpreter::parallel_operator_call(const script_value& self, uint64_t id, std::vector<script_value> args) {
    if (!parallel_worker_) { return std::nullopt; }
    const detail::parallel_method_pin* pin = worker_operator_pin(self, id);
    if (!pin) { return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation), worker_operator_refusal(self, { id })); }
    return detail::call_parallel_operator_pin(*this, *pin, id, self, std::move(args), engine_);
}

// self's method name_id called with no arguments; a parallel worker dispatches it only through its pin. KEEP BYTE-PARALLEL with the other backend
checked_result<script_value> interpreter::invoke_method(const script_value& self, uint64_t name_id) {
    arm_native_stack();
    if (parallel_worker_ && !worker_operator_pin(self, name_id)) [[unlikely]] { return checked_result<script_value>(make_error_code(runtime_error_code::unsupported_operation), worker_operator_refusal(self, { name_id })); }
    auto target = resolve_member_target(self);
    script_value method_val = target ? operator_value(target, name_id) : script_value(std::monostate{}, engine_);
    if (!method_val.is_function()) { return checked_result<script_value>(make_error_code(runtime_error_code::member_not_found), "No method '{0}'", name_id); }
    return operator_call(self, method_val, name_id, {});
}

// An object's own to_string() for the display walk: spoke when it returned a string, silent when it has none (the walk shows
// its fields), unwinding when it threw. KEEP BYTE-PARALLEL with vm_backend::object_own_text
checked_result<detail::own_text_verdict> interpreter::object_own_text(const script_value& val, std::string& out) {
    using verdict = detail::own_text_verdict;
    if (parallel_worker_ && !worker_operators_pinned(val, { to_string_id_ })) [[unlikely]] {
        return checked_result<verdict>(make_error_code(runtime_error_code::unsupported_operation), worker_operator_refusal(val, { to_string_id_ }));
    }
    auto target = resolve_member_target(val);
    if (!target) { return verdict::silent; }
    auto method_val = operator_value(target, to_string_id_);
    if (method_val.is_null() || method_val.is_invalid() || !method_val.is_function()) { return verdict::silent; }
    auto result = operator_call(val, method_val, to_string_id_, {});
    if (!result) { return result.error_value(); }
    if (is_unwinding_) { return verdict::unwinding; }
    if (!result.value().is_string()) { return verdict::silent; }
    out += result.value().unchecked_as_string();
    return verdict::spoke;
}

checked_result<std::string> interpreter::value_to_string_with_method(const script_value& val) {
    if (detail::plain_text_value(val)) { return val.to_string(); }
    const detail::own_text_hook hook{
        [](void* self, const script_value& object, std::string& out) { return static_cast<interpreter*>(self)->object_own_text(object, out); },
        this, !parallel_worker_ };
    return detail::value_text(val, hook);
}

// String + x: each operand's text, the left operand's to_string() first; a failing to_string() is the expression's error
checked_result<script_value> interpreter::concat_with_methods(const script_value& left, const script_value& right) {
    auto left_text = value_to_string_with_method(left);
    if (!left_text) { return left_text.error_value(); }
    auto right_text = value_to_string_with_method(right);
    if (!right_text) { return right_text.error_value(); }
    script_string joined = std::move(left_text.value());
    joined += right_text.value();
    // engine::memory_cap chokepoint: deny the concat result before it exists
    if (!limits_->memory_charge(sizeof(script_value) + joined.size())) [[unlikely]] {
        return detail::raise_memory_cap(*limits_);
    }
    return make_value(std::move(joined));
}


namespace {

// a bare identifier callee in an instance method's own frame that the member ladder may answer with a method of the receiver
identifier_expr* sibling_method_callee(expression* callee, const detail::member_scope_ctx& scope) {
    if (callee->get_type() != node_type::identifier_expr) return nullptr;
    if (scope.state != detail::member_scope_state::instance || !scope.this_storage) return nullptr;
    auto* ident = static_cast<identifier_expr*>(callee);
    return ident->global_qualified || ident->slot_index != SIZE_MAX ? nullptr : ident;
}

}  // namespace

template<bool Replaying>
checked_result<void> interpreter::visit_call_expr_t(call_expr* expr, const replay_point* rs) {
    if (expr->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(expr, expr->const_check)); }
    // math:: language intrinsics (detail/math_intrinsics.hpp): evaluate args, one
    // shared-kernel call - no namespace lookup, no call machinery (KEEP BYTE-PARALLEL
    // with vm_backend::exec_math; parity by the shared kernel).
    if (expr->callee->get_type() == node_type::member_expr) {
        auto* member_callee = static_cast<member_expr*>(expr->callee.get());
        if (member_callee->is_static) {
            const detail::math_fn fn = detail::math_intrinsic_for_call(member_callee);
            if (fn != detail::math_fn::none && expr->arguments.size() <= 8) {
                const size_t argc = expr->arguments.size();
                // the operands leave the value stack as they are evaluated: a later operand's call runs statements, which
                // drop what the stack holds at their boundaries
                std::optional<script_value> margs[8];
                size_t math_start = 0;
                if constexpr (Replaying) {
                    // args evaluated before the yield are restored in order, never re-run
                    if (rs) { for (const auto& v : rs->partial) margs[math_start++].emplace(v); }
                }
                for (size_t ai = math_start; ai < argc; ++ai) {
                    auto stacked = [&]() {   // the args already evaluated, in evaluation order (cold: park only)
                        std::vector<script_value> done;
                        for (size_t k = 0; k < ai; ++k) done.push_back(*margs[k]);
                        return done;
                    };
                    JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->arguments[ai].get(), rs, ai), expr, rp_call_math, ai, stacked());
                    margs[ai].emplace(pop_value());
                    if (is_unwinding_) {
                        push_value(make_value());
                        return {};
                    }
                    capture_call_argument(engine_, expr->arg_captures, ai, *margs[ai], [] { return detail::arg_binding::opaque; });
                }
                const script_value* argp[8] = {};
                for (size_t i = 0; i < argc; ++i) {
                    argp[i] = &margs[i]->deref();
                }
                auto result = detail::eval_math_intrinsic(fn, argp, argc, engine_);
                if (!result) {
                    return result.error_value();
                }
                push_value(std::move(result.value()));
                return {};
            }
        }
    }

    // regex(literal pattern[, flags]) while the name binds the engine's constructor: the pattern the checker compiled at load;
    // anything else is the ordinary call below (KEEP BYTE-PARALLEL with vm_backend::exec_regex_constant)
    if (expr->regex_constant && expr->callee->get_type() == node_type::identifier_expr) [[unlikely]] {
        const script_value* bound = string_receiver_cell(static_cast<const identifier_expr*>(expr->callee.get()));
        script_value made = make_value();
        if (bound && detail::regex_library::make_constant(*bound, expr->regex_constant, made)) {
            push_value(std::move(made));
            return {};
        }
    }

    // Special handling for weak_from_this() and shared_from_this()
    if (expr->callee->get_type() == node_type::identifier_expr) {
        auto* ident_expr = static_cast<identifier_expr*>(expr->callee.get());
        // Use interned symbol IDs for fast comparison
        if (ident_expr->symbol_id == weak_from_this_id_ || ident_expr->symbol_id == shared_from_this_id_) {
            // These functions take no arguments
            if (!expr->arguments.empty()) {
                return checked_result<void>(make_error_code(runtime_error_code::argument_count_mismatch),
                    "{0}() takes no arguments", ident_expr->symbol_id);
            }

            // Get 'this' from the current environment
            script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id());
            if (!this_ptr) {
                return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                    "{0}() can only be called from within a method", ident_expr->symbol_id);
            }
            script_value this_val = *this_ptr;
            if (!this_val.is_object()) {
                return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                    "{0}() can only be called from within a method", ident_expr->symbol_id);
            }

            if (ident_expr->symbol_id == weak_from_this_id_) {
                // Create a weak_ptr from the 'this' object
                auto weak_result = script_value::make_weak_ptr(this_val, engine_);
                if (!weak_result) {
                    return weak_result.error_value();
                }
                push_value(std::move(weak_result.value()));
                return {};
            } else {  // shared_from_this
                // Just return the shared_ptr (which is already 'this')
                push_value(std::move(this_val));
                return {};
            }
        }
    }

    // Identifier-receiver string builtins mutate the VARIABLE in place: the receiver
    // resolves through the identifier ladder (locals -> members -> globals, ::global)
    // and the builtin lands on that cell.
    if (expr->callee->get_type() == node_type::member_expr) {
        auto* member = static_cast<member_expr*>(expr->callee.get());
        if (!member->is_static && member->object->get_type() == node_type::identifier_expr) {
            auto* ident = static_cast<identifier_expr*>(member->object.get());
            script_value* recv_cell = string_receiver_cell(ident);
            const bool recv_utf8 = recv_cell && recv_cell->is_utf8_string();
            if (recv_cell && (recv_utf8 || recv_cell->is_string())) {
                const auto& table = recv_utf8 ? utf8_methods_ : string_methods_;
                auto methodIt = table.find(member->member_id);
                if (methodIt != table.end()) {
                    // C++17 [expr.call]: the receiver resolves before the arguments. An
                    // argument that runs code may rebind or retype the variable, so the
                    // cell is re-resolved after them; a cell that no longer holds a string
                    // runs the builtin on the pre-args value (KEEP BYTE-PARALLEL with
                    // vm_backend::exec_call_method's post-args probe + stack-copy fallback)
                    std::optional<script_value> pre_args;
                    for (const auto& arg : expr->arguments) {
                        const auto arg_type = arg->get_type();
                        if (arg_type != node_type::literal_expr && arg_type != node_type::identifier_expr &&
                            arg_type != node_type::this_expr) {
                            pre_args.emplace(recv_cell->deref());
                            break;
                        }
                    }
                    std::vector<script_value> arguments;
                    arguments.reserve(expr->arguments.size());
                    if constexpr (Replaying) { if (rs) arguments = rs->partial; }
                    for (size_t ai = arguments.size(); ai < expr->arguments.size(); ++ai) {
                        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->arguments[ai].get(), rs, ai), expr, rp_call_string, ai, arguments);
                        arguments.emplace_back(pop_value());
                        if (is_unwinding_) {
                            push_value(make_value());
                            return {};
                        }
                        capture_call_argument(engine_, expr->arg_captures, ai, arguments.back(), [] { return detail::arg_binding::opaque; });
                    }
                    if (pre_args) {
                        recv_cell = string_receiver_cell(ident);
                        if (recv_cell && (recv_utf8 ? recv_cell->is_utf8_string() : recv_cell->is_string())) {
                            pre_args.reset();   // a live copy would only make the write gate copy the node
                        } else {
                            recv_cell = &*pre_args;
                        }
                    }

                    if (!member->call_result_discarded && string_mutator_ids_.count(member->member_id)) [[unlikely]] {
                        return detail::in_place_result_error(string_symbolizer_, member, in_place_.string.at(member->member_id)).error_value();
                    }
                    // Resolve through references (ref decls/cells): string builtins read
                    // self's storage RAW, and in-place mutation must land in the target; a
                    // scratch-backed alias (typed element, C++ property) writes back after
                    // (KEEP BYTE-PARALLEL with vm_backend::exec_call_method)
                    script_value& var_ref = recv_cell->deref();
                    detail::deref_builtin_args_in_place(arguments);
                    auto result = methodIt->second(builtin_ctx(), var_ref, arguments);
                    if (!result) {
                        return result.error_value();
                    }
                    if (auto* scratch_ref = recv_cell->scratch_ref_holder()) [[unlikely]] {
                        // a typed element's scratch holds what the buffer holds unless the method is one of the registry's mutators
                        if (string_mutator_ids_.count(member->member_id) || !scratch_ref->typed_element()) { scratch_ref->commit_scratch(); }
                    }
                    push_value(std::move(result.value()));
                    return {};
                }
            }
        }
    }

    // Direct script-method dispatch: callee is obj.method on a pure receiver expression
    // (identifier/this - re-evaluable without side effects, so a gate miss can fall to
    // the generic path below), or a bare call of a method of this frame's receiver (the
    // vm's park_sibling_method). Mirrors vm exec_call_method's fast-entry precedence gate
    // (same_as/super/coroutine/sp-builtin/access/getter-probe/field), skipping the
    // bound-method mint, its this+args re-copies, and the carrier method env.
    {
        member_expr* const member = expr->callee->get_type() == node_type::member_expr ? static_cast<member_expr*>(expr->callee.get()) : nullptr;
        identifier_expr* const sibling = member ? nullptr : sibling_method_callee(expr->callee.get(), member_scope_);
        if ((member && !member->is_static && member->member_id != same_as_id_ &&
             (member->object->get_type() == node_type::identifier_expr || member->object->get_type() == node_type::this_expr)) || sibling) {
            const uint64_t member_id = member ? member->member_id : sibling->symbol_id;
            script_value objectValue = make_value();
            if (member) {
                JAISCRIPT_TRY(dispatch_expr(member->object.get()));
                objectValue = pop_value();
                if (is_unwinding_) {
                    push_value(make_value());
                    return {};
                }
                objectValue = objectValue.deref();
            } else {
                objectValue = member_scope_.this_storage->deref();
            }

            const script_method_dispatch* dispatch = nullptr;
            std::shared_ptr<environment> def_env;
            std::shared_ptr<class_definition> cls_pin;
            std::shared_ptr<function_decl> parallel_pin_decl;
            class_definition* cls_raw = nullptr;
            bool ic_route = false;   // entry_epoch is the class's at the gate: an argument that reloads it refetches the dispatcher
            uint64_t entry_epoch = 0;

            if (member && objectValue.is_object()) {
                auto holder = objectValue.get_object_holder();
                if (holder && holder->type_id() != coroutine_handle_type_id_) {
                    class_instance* inst_raw = nullptr;
                    if (holder->is_instance()) {
                        inst_raw = holder->unchecked_instance();
                        cls_raw = inst_raw->get_class_definition();
                    }
                    // Worker method wall → admission verdict (parallel-method-admission
                    // rule): barrier-admitted methods dispatch through pinned
                    // worker-private state (fresh dispatcher value, pre-resolved overload —
                    // no shared count, cache, or IC is ever touched); anything unpinned
                    // keeps the wall error (detail::parallel_method_not_admitted).
                    const detail::parallel_method_pin* worker_pin = (parallel_worker_ && cls_raw && parallel_method_pins_)
                        ? parallel_method_pins_->find(cls_raw, member->member_id) : nullptr;
                    if (parallel_worker_ && cls_raw && !worker_pin) [[unlikely]] {
                        return detail::parallel_method_not_admitted(member->member_id);
                    }
                    if (worker_pin && !worker_pin->resolved) [[unlikely]] {
                        std::vector<script_value> arguments;
                        arguments.reserve(expr->arguments.size());
                        if constexpr (Replaying) { if (rs) arguments = rs->partial; }
                        for (size_t ai = arguments.size(); ai < expr->arguments.size(); ++ai) {
                            const auto& argExpr = expr->arguments[ai];
                            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(argExpr.get(), rs, ai), expr, rp_call_direct, ai, arguments);
                            arguments.emplace_back(pop_value());
                            if (is_unwinding_) {
                                push_value(make_value());
                                return {};
                            }
                            if (auto taken = capture_call_argument(engine_, expr->arg_captures, ai, arguments.back(), [&] {
                                    return detail::method_binding(cls_raw, member_id, ai, expr->arguments.size());
                                }, [&](script_value& out) { return mint_argument_place(argExpr.get(), out); }); !taken) {
                                return taken.error_value();
                            }
                        }
                        auto called = detail::call_parallel_host_pin(*worker_pin, objectValue, member_id, std::move(arguments), engine_);
                        if (!called) { return called.error_value(); }
                        push_value(std::move(called.value()));
                        return {};
                    }
                    if (worker_pin) [[unlikely]] {
                        const detail::parallel_method_pin* pin = worker_pin;
                        dispatch = pin->method_value.as_function().target<script_method_dispatch>();
                        def_env = dispatch->definition_env;
                        cls_pin = dispatch->cls;
                        parallel_pin_decl = pin->resolved;
                    } else {
                        // Full gate. A user class method WINS
                        // over a same-named builtin handle method: the
                        // sp-builtin only applies when the class does NOT define the method.
                        const bool sp_builtin = objectValue.get_type_info() &&
                            objectValue.get_type_info()->base_type == script_value_type::jai_shared_ptr_type &&
                            shared_ptr_methods_.find(member->member_id) != shared_ptr_methods_.end() &&
                            !(cls_raw && cls_raw->defines_method(member->member_id));
                        if (!sp_builtin) {
                            auto target = resolve_member_target(objectValue);
                            if (target && target.class_def) {
                                if (target.class_def->chain_has_nonpublic()) [[unlikely]] {
                                    JAISCRIPT_TRY(detail::enforce_member_access(target.class_def, member->member_id,
                                                                                environment_->find_access_context()));
                                }
                                bool getter_shadows = false;
                                if (target.class_def->has_property_getters()) {
                                    uint64_t getter_id = member->getter_id;
                                    if (getter_id == UINT64_MAX) {
                                        auto [id, _] = string_symbolizer_->get_getter_id_with_view(member->member_id);
                                        getter_id = id;
                                        member->getter_id = getter_id;
                                    }
                                    script_value getter_probe = target.method(getter_id);
                                    getter_shadows = !getter_probe.is_null() && !getter_probe.is_invalid() && getter_probe.is_function();
                                }
                                if (!getter_shadows && !target.has_field(member->member_id)) {
                                    script_value method_val = target.method(member->member_id);
                                    if (method_val.is_function()) {
                                        const auto* d = method_val.as_function().target<script_method_dispatch>();
                                        if (d && d->eng == engine_) {
                                            // Owning copies BEFORE argument evaluation: an argument expression
                                            // can hot reload the class and replace the dispatcher under us
                                            dispatch = d;
                                            def_env = d->definition_env;
                                            cls_pin = d->cls;
                                            if (cls_raw && target.class_def == cls_raw) {
                                                ic_route = true;
                                                entry_epoch = cls_raw->method_epoch();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (sibling) {
                // the member ladder's answer for this class and method epoch (no own-scope binding, no field of the name,
                // an all-public chain) is the class's dispatcher on this frame's receiver, entered as the bound method the
                // generic path would mint; a null dispatch caches "no script method of the name"
                const size_t ti = objectValue.raw_storage_index();
                class_instance* inst_raw = nullptr;
                if (ti == script_value::TYPEID_OBJECT) {
                    auto holder = objectValue.get_object_holder();
                    if (holder && holder->is_instance()) {
                        inst_raw = holder->unchecked_instance();
                    }
                }
                cls_raw = inst_raw ? inst_raw->get_class_definition() : nullptr;
                const script_method_dispatch* d = nullptr;
                if (parallel_worker_) [[unlikely]] {
                    // a worker's bare sibling call enters the region's pin of the method (the site's cache is the engine's); KEEP
                    // BYTE-PARALLEL with vm_backend::worker_park_sibling
                    const detail::parallel_method_pin* pin = cls_raw && parallel_method_pins_ ? parallel_method_pins_->find(cls_raw, member_id) : nullptr;
                    const script_method_dispatch* pd = pin && pin->resolved && pin->method_value.is_function() ? pin->method_value.as_function().target<script_method_dispatch>() : nullptr;
                    const bool own = !inst_raw || inst_raw->has_field(member_id) || detail::own_scope_lookup(environment_.get(), member_scope_.floor, member_id);
                    if (pd && !own) {
                        dispatch = pd;
                        def_env = pd->definition_env;
                        cls_pin = pd->cls;
                        parallel_pin_decl = pin->resolved;
                    } else if (!own) {
                        const script_value* bound = cls_raw->method_ptr(member_id);
                        if (bound && bound->is_function() && bound->as_function().target<script_method_dispatch>()) {
                            return detail::parallel_method_not_admitted(member_id);
                        }
                    }
                } else if (cls_raw && !cls_raw->chain_has_nonpublic()) {
                    script_value method_val = inst_raw->get_method(member_id, false);
                    d = method_val.is_function() ? method_val.as_function().target<script_method_dispatch>() : nullptr;
                    if (d && d->eng != engine_) { d = nullptr; }
                }
                if (d && !inst_raw->has_field(member_id) &&
                    !detail::own_scope_lookup(environment_.get(), member_scope_.floor, member_id)) {
                    dispatch = d;
                    def_env = d->definition_env;
                    cls_pin = d->cls;
                    ic_route = true;
                    entry_epoch = cls_raw->method_epoch();
                }
            }

            if (dispatch) {
                // Evaluate arguments (same order/unwind semantics as the generic loop)
                std::vector<script_value> arguments;
                arguments.reserve(expr->arguments.size());
                // Replay: the receiver and method resolution above are pure (identifier/this
                // receiver), so they re-run; the args evaluated before the yield are parked
                if constexpr (Replaying) { if (rs) arguments = rs->partial; }
                for (size_t ai = arguments.size(); ai < expr->arguments.size(); ++ai) {
                    const auto& argExpr = expr->arguments[ai];
                    try {
                        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(argExpr.get(), rs, ai), expr, rp_call_direct, ai, arguments);
                        arguments.emplace_back(pop_value());
                        if (is_unwinding_) {
                            push_value(make_value());
                            return {};
                        }
                        if (auto taken = capture_call_argument(engine_, expr->arg_captures, ai, arguments.back(), [&] {
                                return parallel_pin_decl ? detail::parameters_binding(parallel_pin_decl->parameters, ai)
                                                         : detail::method_binding(cls_pin.get(), dispatch->name_id, ai, expr->arguments.size());
                            }, [&](script_value& out) { return mint_argument_place(argExpr.get(), out); }); !taken) {
                            return taken.error_value();
                        }
                    } catch (const std::runtime_error& e) {
                        return raise_native_exception(e);
                    }
                }

                function_decl* direct_ast = nullptr;
                std::shared_ptr<function_decl> resolved_pin;
                if (parallel_pin_decl) {
                    // Barrier-resolved: no overload resolution, no shared-cache touch
                    resolved_pin = parallel_pin_decl;
                    direct_ast = resolved_pin.get();
                }
                if (!direct_ast) {
                    if (ic_route && entry_epoch != cls_raw->method_epoch()) {
                        // Class changed under the arguments: refetch the live dispatcher
                        auto target = resolve_member_target(objectValue);
                        script_value method_val = target ? target.method(member_id)
                                                         : script_value::make_invalid(engine_);
                        const script_method_dispatch* d = method_val.is_function()
                            ? method_val.as_function().target<script_method_dispatch>() : nullptr;
                        if (!d || d->eng != engine_) {
                            raise_no_member(target.class_name(), member ? std::string_view(member->member) : std::string_view(sibling->name), expr->callee->location);
                            push_value(make_value());
                            return {};
                        }
                        dispatch = d;
                        def_env = d->definition_env;
                        cls_pin = d->cls;
                    }
                    // Slow resolution: exactly what the dispatcher would do, with the
                    // dispatcher's identical error object on failure (and no second
                    // evaluation of side-effecting arguments)
                    auto resolved = cls_pin->resolve_method_overload(dispatch->name_id, arguments);
                    if (!resolved) {
                        return resolved.error_value();
                    }
                    resolved_pin = resolved.value();
                    if (resolved_pin->body && resolved_pin->is_coroutine) {
                        // Coroutine method: mint the handle through the same callable
                        // seam the dispatcher uses (args already evaluated once)
                        script_callable payload;
                        payload.kind = script_callable::kind_type::method;
                        payload.more().ast = resolved_pin;
                        payload.more().cls = cls_pin;
                        payload.more().definition_env = def_env;
                        payload.more().this_obj = objectValue;
                        auto coro_result = execute_callable(payload, arguments);
                        if (!coro_result) {
                            return coro_result.error_value();
                        }
                        push_value(std::move(coro_result.value()));
                        return {};
                    }
                    if (resolved_pin->body) {
                        direct_ast = resolved_pin.get();
                    }
                }
                if (direct_ast) {
                    script_defined_function script_func(
                        direct_ast->name,
                        shared_parameters_for(direct_ast),
                        direct_ast->return_type,
                        direct_ast->body,
                        nullptr,
                        direct_ast->local_count);
                    script_func.unit = direct_ast->unit;
                    method_call_override mctx{&def_env, &objectValue, cls_pin.get()};
                    detail::call_site_context ctx{&expr->arguments, &arguments};
                    const detail::call_site_context* saved_ctx = pending_call_ctx_;
                    pending_call_ctx_ = expr->arguments.empty() ? saved_ctx : &ctx;
                    std::optional<checked_result<script_value>> callOutcome;
                    try {
                        callOutcome.emplace(call_function(script_func, arguments, &mctx));
                    } catch (const std::exception& e) {
                        pending_call_ctx_ = saved_ctx;
                        return raise_native_exception(e);
                    }
                    pending_call_ctx_ = saved_ctx;
                    checked_result<script_value>& result_checked = *callOutcome;
                    if (!result_checked) {
                        return result_checked.error_value();
                    }
                    // Non-owning method results (C++ chaining surfacing through a script
                    // method) pin the receiver, as make_bound_method does
                    script_value& rv = result_checked.value();
                    if (objectValue.is_object() && rv.is_non_owning_object()) {
                        auto rv_holder = rv.get_object_holder();
                        auto recv_holder = objectValue.get_object_holder();
                        if (rv_holder && recv_holder) {
                            rv_holder->set_keep_alive(recv_holder->holds_object() ? recv_holder->held()
                                                                      : recv_holder->keep_alive());
                        }
                    }
                    push_value(std::move(result_checked.value()));
                    return {};
                }
                // body-less resolution: fall through to the generic path
            }
            // Gate miss: fall through to the generic path (receiver expression is pure,
            // so re-evaluating the full callee is side-effect-free)
        }
    }

    // Evaluate the callee expression (a member callee's lookup errors take the call shape).
    // Replay: partial[0] is the callee resolved BEFORE the yield - the parked callee is the
    // one invoked, exactly like the vm's pending_callee (callee_first_rebind_in_args).
    script_value callee;
    if (parked<Replaying>(rs, 0)) {
        callee = rs->partial[0];
    } else {
        member_callee_lookup_ = expr->callee->get_type() == node_type::member_expr;
        identifier_callee_lookup_ = expr->callee->get_type() == node_type::identifier_expr;
        auto callee_result = eval_child<Replaying>(expr->callee.get(), rs, 0);
        member_callee_lookup_ = false;
        identifier_callee_lookup_ = false;
        if (!callee_result) [[unlikely]] {
            if (is_yield_signal(callee_result)) park_operands(expr, rp_call_generic, 0, {});
            return callee_result.error_value();
        }
        callee = pop_value();
        if (is_unwinding_) {
            push_value(make_value());
            return {};
        }
    }

    // Handle null-safe method calls: obj?.method() returns null if obj was null
    if (callee.is_null() && expr->callee->get_type() == node_type::member_expr) {
        auto* member = static_cast<member_expr*>(expr->callee.get());
        if (member->null_safe) {
            push_value(make_value());  // push null
            return {};
        }
    }

    // Check if the callee is a function
    if (!callee.is_function()) {
        if (script_value call_operator = call_operator_of(callee); call_operator.is_function()) {
            callee = std::move(call_operator);
        } else {
            const script_value& callee_value = callee.deref();
            auto callee_target = resolve_member_target(callee_value);
            auto text = not_callable_diagnostic(callee_name_symbol(expr->callee.get()), callee_value.type(), base_type_ids_[callee_value.type()],
                                                callee_target.class_def ? std::string_view(callee_target.class_def->get_name()) : std::string_view{},
                                                callee_target.class_def ? callee_target.class_def->get_type_id() : 0);
            return checked_result<void>(make_error_code(runtime_error_code::not_a_function), text.message, text.sym0, text.sym1);
        }
    }
    
    // Use a local vector for arguments to avoid issues with nested calls
    std::vector<script_value> arguments;
    arguments.reserve(expr->arguments.size());
    // Replay ordinals: 0 is the callee, argument i is i+1 (partial[0] is the parked callee)
    if constexpr (Replaying) { if (rs && rs->index >= 1) arguments.assign(rs->partial.begin() + 1, rs->partial.end()); }

    for (size_t ai = arguments.size(); ai < expr->arguments.size(); ++ai) {
        const auto& argExpr = expr->arguments[ai];
        // Evaluate argument with exception handling
        try {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(argExpr.get(), rs, ai + 1), expr, rp_call_generic, ai + 1,
                                  parked_with_head(callee, arguments));
            arguments.emplace_back(pop_value());
            if (is_unwinding_) {
                push_value(make_value());
                return {};
            }
            if (auto taken = capture_call_argument(engine_, expr->arg_captures, ai, arguments.back(), [&] { return detail::callee_binding(callee, ai, false, expr->arguments.size()); },
                                                   [&](script_value& out) { return mint_argument_place(argExpr.get(), out); }); !taken) {
                return taken.error_value();
            }
        } catch (const std::runtime_error& e) {
            return raise_native_exception(e);
        }
    }

    // Call the function - now returns checked_result instead of throwing. C++ callables
    // may still throw (conversion failures, game-side exceptions) - convert those to
    // script exceptions here so nothing propagates raw through the interpreter.
    // The call-site context rides a single save/restored pointer: the next call_function
    // entered consumes it to bind reference parameters against the caller's variables
    // (the arg expressions outlive the call - they live in this frame).
    // A plain script-function thunk enters this backend's call machinery directly (vm invoke_callee's twin); any other callee
    // runs through its trampoline, which enters engine::calling_backend (a parallel chunk's own backend)
    const script_function* callee_func = &callee.as_function();
    const auto* callee_thunk = callee_func->target<script_callable_thunk>();
    if (callee_thunk && callee_thunk->eng == engine_ && callee_thunk->payload.kind == script_callable::kind_type::overload_set &&
        callee_thunk->payload.overloads()) {
        // the member these arguments select (KEEP BYTE-PARALLEL with vm_backend::invoke_callee)
        auto picked = detail::select_overload_member(*callee_thunk->payload.overloads(), arguments.data(), arguments.size(), engine_);
        if (!picked) { return picked.error_value(); }
        callee_func = &picked.value()->as_function();
        callee_thunk = callee_func->target<script_callable_thunk>();
    }
    const script_function& func = *callee_func;
    const bool direct_script_call = callee_thunk && callee_thunk->eng == engine_ &&
        callee_thunk->payload.kind == script_callable::kind_type::function && callee_thunk->payload.fn;
    detail::call_site_context ctx{&expr->arguments, &arguments};
    const detail::call_site_context* saved_ctx = pending_call_ctx_;
    pending_call_ctx_ = expr->arguments.empty() ? saved_ctx : &ctx;
    std::optional<checked_result<script_value>> callOutcome;
    try {
        if (direct_script_call) {
            callOutcome.emplace(call_function(*callee_thunk->payload.fn, arguments));
        } else if (arguments.size() == 1 && engine_->sleep_callee(&func)) {
            callOutcome.emplace(engine_->sleep_builtin(arguments[0]));   // the builtin in place (vm op_sleep's twin)
        } else if (const engine::format_literal_row* literal = expr->literal_format_call() ? engine_->format_literal_for(&func) : nullptr) {
            callOutcome.emplace(call_value(literal->literal.unchecked_as_function(), arguments));   // its format string (vm op_format_call's twin)
        } else {
            callOutcome.emplace(call_value(func, arguments));
        }
    } catch (const std::exception& e) {
        pending_call_ctx_ = saved_ctx;
        return raise_native_exception(e);
    }
    pending_call_ctx_ = saved_ctx;
    checked_result<script_value>& result_checked = *callOutcome;

    // Check if function call succeeded
    if (!result_checked) {
        // Function returned an error via checked_result - propagate it
        return result_checked.error_value();
    }

    // Push successful result onto the stack
    push_value(std::move(result_checked.value()));
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_member_expr_t(member_expr* expr, const replay_point* rs) {
    // Consumed at entry: a nested receiver's own member reads are accesses
    const bool as_callee = member_callee_lookup_;
    member_callee_lookup_ = false;
    script_value* const chain_root = std::exchange(chain_root_hold_, nullptr);
    // A structured binding's half reads the loop variable's entry cursor; without one (the container was not a map) it is the
    // member read it is spelled as (KEEP BYTE-PARALLEL with vm_backend::exec_iter_entry)
    if (expr->entry_half) [[unlikely]] {
        const auto* loop_var = static_cast<const identifier_expr*>(expr->object.get());
        if (const script_value* cell = resolve_local_or_env(loop_var->slot_index, loop_var->symbol_id)) {
            if (auto half = detail::map_entry_half(*cell, expr->entry_half, engine_)) {
                push_value(std::move(*half));
                return {};
            }
        }
    }
    // Check if this is a static member access (::)
    if (expr->is_static) {
        // For static access, the object should be an identifier (class or namespace name)
        identifier_expr* ident_expr = nullptr;
        if (expr->object->get_type() == node_type::identifier_expr) {
            ident_expr = static_cast<identifier_expr*>(expr->object.get());
        }
        // Handle nested namespace access: outer::inner::getValue()
        // Build the full namespace path
        std::string name;
        uint64_t name_id;

        if (ident_expr) {
            // Simple case: just an identifier
            name = ident_expr->name;
            name_id = ident_expr->symbol_id;
            if (parallel_worker_ && parallel_method_pins_ && name_id != UINT64_MAX) [[unlikely]] {
                if (const script_value* pinned = parallel_method_pins_->find_static(name_id, expr->member_id)) {
                    push_value(*pinned);
                    return {};
                }
            }

            // Cache symbol ID if not already done
            if (name_id == UINT64_MAX) {
                name_id = string_symbolizer_->intern(name);
                ident_expr->symbol_id = name_id;
            }
        } else if (expr->object->get_type() == node_type::member_expr) {
            auto* member_expr_obj = static_cast<member_expr*>(expr->object.get());
            // Nested namespace: outer::inner where the object is "outer::inner" (a member_expr)
            // Recursively build the full path
            std::function<std::string(expression*)> build_namespace_path = [&](expression* e) -> std::string {
                if (e->get_type() == node_type::identifier_expr) {
                    return std::string(static_cast<identifier_expr*>(e)->name);
                } else if (e->get_type() == node_type::member_expr) {
                    auto* member = static_cast<member_expr*>(e);
                    if (member->is_static) {
                        // This is a :: access, continue building the path
                        return build_namespace_path(member->object.get()) + "::" + std::string(member->member);
                    }
                }
                return "";
            };

            name = build_namespace_path(expr->object.get());
            if (name.empty()) {
                return checked_result<void>(make_error_code(jai::runtime_error_code::type_mismatch));  // [ErrorText] Invalid namespace path
            }
            name_id = string_symbolizer_->intern(name);
        } else {
            // Static member access requires an identifier or namespace path
            return checked_result<void>(make_error_code(jai::runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        // PRIORITY 1: Check for namespace with this name FIRST
        // Namespaces can override class static methods
        auto& namespaces = engine_->script_namespaces();
        auto ns_it = namespaces.find(name_id);
        bool is_namespace = (ns_it != namespaces.end());

        // PRIORITY 1.5: Special case for namespace::class::static_member
        // ONLY if name is NOT a namespace itself
        // For "my::nested::cat::meow()", name would be "my::nested::cat" which is NOT a namespace
        // We need to detect this pattern and split it into namespace="my::nested" and class="cat"
        if (!is_namespace && name.find("::") != std::string::npos) {
            size_t last_colon = name.rfind("::");
            std::string potential_ns = name.substr(0, last_colon);
            std::string potential_class = name.substr(last_colon + 2);

            uint64_t ns_id = string_symbolizer_->intern(potential_ns);
            uint64_t class_id = string_symbolizer_->intern(potential_class);

            auto ns_check = namespaces.find(ns_id);
            if (ns_check != namespaces.end()) {
                auto class_check = ns_check->second->classes.find(class_id);
                if (class_check != ns_check->second->classes.end()) {
                    // Found! This is namespace::class, and we're accessing a static member
                    auto class_def = class_check->second;

                    // Try to get the static method (use pre-computed member_id from parser)
                    script_value static_method = class_def->static_method_in_chain(expr->member_id);
                    if (!static_method.is_null()) {
                        push_value(std::move(static_method));
                        return {};
                    }

                    // Method not found - might be trying to access the class constructor
                    // Fall through to handle namespace::class access
                }
            }
        }

        // PRIORITY 2: Handle namespace members
        if (is_namespace) {
            auto& ns_data = ns_it->second;

            // a function or a variable the namespace declares, then a host entry, then a class
            if (ns_data->scope) {
                if (script_value* cell = ns_data->scope->get_own_value_ptr(expr->member_id)) {
                    push_value(*cell);
                    return {};
                }
            }
            auto var_it = ns_data->variables.find(expr->member_id);
            if (var_it != ns_data->variables.end()) {
                push_value(var_it->second);
                return {};
            }

            // Look for class in namespace
            // Note: namespace::class::static_member is handled earlier (before namespace lookup)
            auto class_it = ns_data->classes.find(expr->member_id);
            if (class_it != ns_data->classes.end()) {
                auto ctor_result = environment_->get(class_it->second->interned_name_id());
                if (!ctor_result) {
                    return ctor_result.error_value();
                }
                push_value(std::move(ctor_result.value()));
                return {};
            }

            // Member not found in namespace - fall through to check class static methods
        }

        // PRIORITY 2: Look up the class definition (use cached ID)
        auto [class_var_id, class_var_view] = string_symbolizer_->get_class_var_id_with_view(name_id);
        auto class_var_result = environment_->get(class_var_id);
        if (!class_var_result) {
            if (auto declared = detail::qualified_extern_missing(*environment_, name, expr->member, *string_symbolizer_)) { return *declared; }   // KEEP BYTE-PARALLEL with the vm
            if (engine_->script_namespaces().count(name_id)) {   // a namespace without that member: name the member, not the class fallback
                return checked_result<void>(make_error_code(runtime_error_code::undefined_variable), "Undefined variable '{0}::{1}'", name_id, expr->member_id);
            }
            if (ident_expr && engine_->is_enum_name(name_id)) {   // KEEP BYTE-PARALLEL with the vm
                if (auto enum_var = environment_->get(name_id); enum_var && enum_var.value().deref().is_map()) {
                    auto value = detail::enum_scoped_value(enum_var.value().deref().as_map(), name_id, expr->member, expr->member_id, engine_);
                    if (!value) { return value.error_value(); }
                    push_value(std::move(value.value()));
                    return {};
                }
            }
            // no namespace and no class of that name: the class is what `C::m` names (undefined names)
            return checked_result<void>(make_error_code(runtime_error_code::class_not_found), "Class '{0}' not found", name_id);
        }
        script_value class_var = std::move(class_var_result.value());

        if (!class_var.is_object()) {
            // Not a class
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        // Extract the class definition
        auto objHolder = class_var.get_object_holder();
        if (!objHolder || objHolder->type_id() != class_definition_type_id_) {
            // Not a valid class
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        auto class_def = std::static_pointer_cast<class_definition>(objHolder->host_object());

        if (class_def->chain_has_nonpublic()) [[unlikely]] {
            JAISCRIPT_TRY(detail::enforce_member_access(class_def.get(), expr->member_id,
                                                        environment_->find_access_context()));
        }

        // Try static method first (most common case for :: access)
        // get_static_method with false doesn't throw - returns null if not found
        script_value static_method = class_def->static_method_in_chain(expr->member_id);
        if (!static_method.is_null()) {
            push_value(std::move(static_method));
            return {};
        }

        // Try static field access (presence, not null-ness: a typed null static reads null)
        if (const script_value* static_value = class_def->find_static_field_in_chain(expr->member_id)) {
            push_value(*static_value);
            return {};
        }

        // Try getter method as fallback (for C++ bound properties)
        auto [getter_id, _] = string_symbolizer_->get_getter_id_with_view(expr->member_id);
        // get_static_method with false doesn't throw - returns null if not found
        script_value getter_method = class_def->static_method_in_chain(getter_id);
        if (!getter_method.is_null() && getter_method.is_function()) {
            // Call the getter with no arguments to get the live C++ value
            auto func = getter_method.as_function();
            std::vector<script_value> no_args;
            auto result = func(no_args);
            if (!result) {
                // Function returned error - propagate it up
                return result.error_value();
            }
            script_value static_value = std::move(result.value());
            push_value(std::move(static_value));
            return {};
        }

        return checked_result<void>(make_error_code(runtime_error_code::static_member_not_found),
            "class {0} has no static member '{1}'", name_id, expr->member_id);
    }

    std::optional<script_value> held_root;
    const bool object_links = detail::read_chain_link(expr->object.get());
    {
        script_value* hold = chain_root ? chain_root : (detail::held_chain_root(expr) ? &held_root.emplace(make_value()) : nullptr);
        detail::chain_root_handoff handoff{chain_root_hold_, object_links ? hold : nullptr};
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->object.get(), rs, 0), expr, rp_operand, 0, {});
    }
    script_value objectValue = pop_value();
    if (chain_root && !object_links) {
        *chain_root = objectValue;
    }
    if (is_unwinding_) {
        push_value(make_value());
        return {};
    }
    if (expr->place_read) [[unlikely]] {
        // KEEP BYTE-PARALLEL with vm_backend::exec_get_member
        script_value part = detail::math::evaluated_component_reference(objectValue, expr, engine_);
        if (!part.is_reference() && expr->place_read == member_expr::place_read_field) {
            auto field = detail::evaluated_member_reference(objectValue, expr, environment_.get(), engine_, string_symbolizer_);
            if (!field) { return field.error_value(); }
            part = std::move(field.value());
        }
        if (part.is_reference()) {
            push_value(std::move(part));
            return {};
        }
    }
    return member_read_on(expr, std::move(objectValue), as_callee);
}

// The member read on an already-evaluated receiver: the compound-member store evaluates the
// receiver once and holds it across the read, the rhs and the store-back
// A reference bound to a bare name that lands on an instance method (auto& r = heal, cast<T&>(heal)): the same violation, never
// an undefined variable. KEEP BYTE-PARALLEL with vm_backend::bare_method_reference
std::optional<checked_result<void>> interpreter::bare_method_reference_error(const identifier_expr* expr) {
    if (!member_scope_.active() || !member_scope_.this_storage) { return std::nullopt; }
    auto member = detail::probe_member_scope(member_scope_.state, member_scope_.this_storage, member_scope_.statics, expr->symbol_id,
                                             /*want_methods*/ true);
    if (member && member.what == detail::member_probe_hit::kind::method && member.instance) {
        return bare_method_value_error(expr, *member_scope_.this_storage);
    }
    return std::nullopt;
}

// A bare method name read without a call inside a method; KEEP BYTE-PARALLEL with
// vm_backend::raise_bare_method_value
checked_result<void> interpreter::bare_method_value_error(const identifier_expr* expr, const script_value& self) {
    if (parallel_worker_) [[unlikely]] {
        return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
            "'{0}' is a method: call it or bind it with reflect::bind", expr->symbol_id);
    }
    auto target = resolve_member_target(self.deref());
    const std::string owner = target.class_def ? target.class_def->get_name() : std::string("this");
    return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation), "{0}",
        string_symbolizer_->intern(detail::bare_method_value_text(expr->name, owner)));
}

// obj.method without a call: a method is called or bound, never read bare. KEEP BYTE-PARALLEL with
// vm_backend::raise_method_value
JAI_NOINLINE void interpreter::raise_script_text(const std::string& text, const source_location& location) {
    active_exception_value_ = make_value(text);
    current_exception_ = script_exception(text, location);
    is_unwinding_ = true;
}

JAI_NOINLINE void interpreter::raise_no_member(std::string_view kind, std::string_view member, const source_location& location) {
    raise_script_text(no_member_text(kind, member), location);
}

checked_result<void> interpreter::method_value_error(const member_expr* expr, const script_value& object, bool bindable) {
    if (parallel_worker_) [[unlikely]] {
        return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
            "'{0}' is a method: call it or bind it with reflect::bind", expr->member_id);
    }
    const script_value& v = object.deref();
    std::string owner;
    bool value_type = true;
    if (v.is_utf8_string()) { owner = "utf8_string"; }
    else if (v.is_string()) { owner = "string"; }
    else if (v.is_array()) { owner = "array"; }
    else if (v.is_map()) { owner = "map"; }
    else if (v.is_object() && v.get_object_holder() && v.get_object_holder()->type_id() == coroutine_handle_type_id_) { owner = "coroutine"; }
    else {
        value_type = false;
        auto target = resolve_member_target(v);
        owner = target.class_def ? target.class_def->get_name() : std::string(base_type_spelling(v.type()));
    }
    return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation), "{0}",
        string_symbolizer_->intern(detail::method_value_text(expr->member, owner, detail::method_value_receiver(expr), value_type || !bindable)));
}

checked_result<void> interpreter::member_read_on(member_expr* expr, script_value objectValue, bool as_callee) {
    const bool is_super_access = expr->object->get_type() == node_type::super_expr;

    // An in-place builtin whose result the call uses raises once its arguments ran (KEEP BYTE-PARALLEL with
    // vm_backend::exec_call_method_general's in_place_check)
    if (as_callee && !expr->call_result_discarded && !is_super_access) [[unlikely]] {
        if (const auto twin = detail::in_place_builtin(in_place_, objectValue.deref(), expr->member_id)) {
            script_function raise_used = [symbols = string_symbolizer_, expr, twin = *twin](const std::vector<script_value>&) -> checked_result<script_value> {
                return detail::in_place_result_error(symbols, expr, twin);
            };
            push_value(script_value::make_function(raise_used, engine_));
            return {};
        }
    }

    // A handle's reset() writes null into the receiver LVALUE (C++ `p.reset()`; detail/string_mutator_receiver.hpp); a class
    // that declares reset() keeps its method (KEEP BYTE-PARALLEL with vm_backend::exec_call_method's handle reset)
    if (as_callee && expr->member_id == handle_reset_id_ && !is_super_access && detail::handle_reset_applies(objectValue.deref(), handle_reset_id_)) {
        detail::string_mutator_binding binding = detail::bind_string_mutator_receiver(objectValue, expr->object.get(),
            detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
            environment_.get(), engine_, string_symbolizer_);
        const identifier_expr* ident = expr->object->get_type() == node_type::identifier_expr
            ? static_cast<const identifier_expr*>(expr->object.get()) : nullptr;
        const bool weak_receiver = objectValue.deref().raw_storage_index() == script_value::TYPEID_WEAK_PTR;
        script_value receiver = objectValue.deref();
        script_function boundMethod = [this, binding = std::move(binding), ident, weak_receiver, receiver](const std::vector<script_value>& args) mutable -> checked_result<script_value> {
            if (!args.empty() && (weak_receiver || args.size() > 1)) {
                return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch),
                    weak_receiver ? "reset() takes no arguments" : "reset() takes at most one argument");
            }
            script_value* storage = detail::handle_reset_storage(binding, ident ? string_receiver_cell(ident) : nullptr);
            if (args.empty()) {
                detail::reset_handle_storage(storage, engine_);
            } else {
                auto repointed = detail::reset_handle_to(storage, receiver, args[0], engine_);   // KEEP BYTE-PARALLEL with vm_backend::exec_call_method
                if (!repointed) { return repointed.error_value(); }
            }
            binding.commit();
            return make_value();
        };
        push_value(script_value::make_function(boundMethod, engine_));
        return {};
    }

    // A string MUTATOR called on a non-identifier receiver binds the receiver LVALUE
    // (detail/string_mutator_receiver.hpp; C++ `void to_upper()`): the popped reference or
    // the resolved field / property. Unbound = a temporary: the builtin mutates the copy alone.
    // (KEEP BYTE-PARALLEL with vm_backend::exec_call_method's builtin-direct string branch)
    if (as_callee && (objectValue.deref().raw_storage_index() == script_value::TYPEID_STRING ||
                      objectValue.deref().raw_storage_index() == script_value::TYPEID_UTF8_STRING) &&
        string_mutator_ids_.count(expr->member_id)) {
        const builtin_method& method = (objectValue.deref().raw_storage_index() == script_value::TYPEID_UTF8_STRING
            ? utf8_methods_ : string_methods_).find(expr->member_id)->second;
        script_value copy(objectValue.deref());
        detail::string_mutator_binding binding = detail::bind_string_mutator_receiver(objectValue, expr->object.get(),
            detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
            environment_.get(), engine_, string_symbolizer_);
        script_function boundMethod = [this, ctx = builtin_ctx(), method,
                                       binding = std::move(binding), copy = std::move(copy)]
                                      (const std::vector<script_value>& args) mutable -> checked_result<script_value> {
            script_value* self = detail::string_mutator_self(binding, copy);
            if (self) {
                copy = script_value(std::monostate{}, engine_);   // the copy's count would make the gate copy the node
            } else {
                self = &copy;
            }
            std::vector<script_value> scratch;
            auto result = method(ctx, *self, detail::deref_builtin_args(args, scratch));
            binding.commit();
            return result;
        };
        push_value(script_value::make_function(boundMethod, engine_));
        return {};
    }

    // Dereference if needed - subscript access returns references
    objectValue = objectValue.deref();

    // Handle null-safe member access (?.)
    if (expr->null_safe && objectValue.is_null()) {
        push_value(make_value());  // push null
        return {};
    }

    // A math value's component (x / width, y / height, z / depth; detail/math_values.hpp; KEEP BYTE-PARALLEL with
    // vm_backend::member_access_value and exec_probe_member)
    if (detail::math::is_kind(objectValue.raw_storage_index())) {
        const script_value_type kind = objectValue.unchecked_vec_kind();
        const int component = detail::math::component_of(kind, expr->member);
        if (component < 0) {
            if (const detail::math_fn method = detail::vec_method_fn(kind, expr->member); method != detail::math_fn::none) {
                push_value(detail::vec_bound_method(method, objectValue, engine_));
                return {};
            }
            return checked_result<void>(make_error_code(runtime_error_code::member_not_found), detail::math::no_member_text(kind), expr->member_id);
        }
        push_value(detail::math::component_value(objectValue, static_cast<size_t>(component), engine_));
        return {};
    }

    // Handle super:: member access specially
    if (is_super_access) {
        // objectValue is 'this' from visit_super_expr
        if (!objectValue.is_object()) {
            // super:: used on non-object
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        // Get the class instance
        auto objHolder = objectValue.get_object_holder();
        if (!objHolder || !objHolder->holds_object()) {
            // super:: used on non-class object
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        // Both script and C++ classes store class_instance in data
        auto instance = objHolder->instance_handle();
        if (!instance) {
            // super:: used on non-class object
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        // Get the class definition and its parent
        auto class_def = instance->get_class_definition();
        if (!class_def) {
            // Class definition not found for super:: access
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        auto parent_def = class_def->get_parent();
        if (!parent_def) {
            // super:: used in class with no parent
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));  // [ErrorText] Type error
        }

        if (parent_def->chain_has_nonpublic()) [[unlikely]] {
            JAISCRIPT_TRY(detail::enforce_member_access(parent_def.get(), expr->member_id,
                                                        environment_->find_access_context()));
        }

        // Look for the method in the parent class (use pre-computed member_id from parser)
        script_value method = parent_def->get_method(expr->member_id, false);
        if (method.is_invalid() || method.is_null()) {
            return checked_result<void>(make_error_code(runtime_error_code::member_not_found),
                "parent class {0} has no member '{1}'", parent_def->get_type_id(), expr->member_id);
        }

        // Return a bound method that calls the parent's implementation
        if (!as_callee) { return method_value_error(expr, objectValue, true); }
        push_value(create_bound_method(objectValue, method));
        return {};
    }

    // A worker reads a field directly: probing getters and methods copies shared method values out of the class (a cross-worker
    // count race on the non-atomic strong_ptr); fields live on the worker's own instance
    if (parallel_worker_ && objectValue.parallel_object_borrow()) [[unlikely]] {
        // a shared object: a script field as the worker's own copy, a host member on its view (KEEP BYTE-PARALLEL with
        // vm_backend::member_access_value)
        if (as_callee) { JAISCRIPT_TRY(detail::parallel_borrow_call(objectValue, expr->member_id, parallel_method_pins_)); }
        if (!as_callee) {
            if (auto field = detail::parallel_borrow_field(objectValue, expr->member_id)) {
                if (!*field) { return field->error_value(); }
                push_value(std::move(field->value()));
                return {};
            }
        }
        if (const script_value* view = detail::parallel_borrow_view(objectValue)) { return member_read_on(expr, *view, as_callee); }
        return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
            "only direct field access on class instances is allowed in a parallel body");
    }
    if (parallel_worker_ && objectValue.raw_storage_index() == script_value::TYPEID_OBJECT) [[unlikely]] {
        auto& holder = objectValue.unchecked_get_object_storage();
        if (holder && holder->is_instance()) {
            auto* inst = holder->unchecked_instance();
            const class_definition* cls = inst->get_class_definition();
            if (script_value* field = detail::parallel_script_field(*inst, expr->member_id)) {
                push_value(field->is_reference() ? field->deref() : *field);
                return {};
            }
            // a call on a receiver the call's fast entry does not take (a member chain) enters the method's pin, else meets the
            // call's wall (KEEP BYTE-PARALLEL with vm_backend::exec_probe_member)
            if (as_callee && expr->member_id != same_as_id_) {
                const detail::parallel_method_pin* pin = parallel_method_pins_ ? parallel_method_pins_->find(cls, expr->member_id) : nullptr;
                if (!pin) { return detail::parallel_method_not_admitted(expr->member_id); }
                push_value(pin->resolved ? create_bound_method(objectValue, pin->method_value) : detail::parallel_bound_pin(*pin, objectValue, expr->member_id, engine_));
                return {};
            }
            if (const detail::parallel_method_pin* getter = !as_callee && parallel_method_pins_
                    ? parallel_method_pins_->find(cls, string_symbolizer_->get_getter_id_with_view(expr->member_id).first) : nullptr) {
                auto read = detail::call_parallel_host_pin(*getter, objectValue, expr->member_id, {}, engine_, true);
                if (!read) { return read.error_value(); }
                push_value(std::move(read.value()));
                return {};
            }
            return checked_result<void>(make_error_code(runtime_error_code::unsupported_operation),
                "only direct field access on class instances is allowed in a parallel body");
        }
    }

    // Handle coroutine_handle methods
    if (objectValue.is_object()) {
        auto objHolder = objectValue.get_object_holder();
        if (objHolder && objHolder->type_id() == coroutine_handle_type_id_) {
            auto handle = std::static_pointer_cast<coroutine_handle>(objHolder->host_object());
            if (expr->member_id == next_id_) {
	            if (!as_callee) { return method_value_error(expr, objectValue, false); }
                auto captured_handle = handle;
                auto* eng = engine_;
                script_function next_method = [captured_handle, eng](const std::vector<script_value>& a_args) -> checked_result<script_value> {
                    return captured_handle->next_from_script(eng, a_args.data(), a_args.size());
                };
                push_value(script_value::make_function(next_method, engine_));
                return {};
            }
            if (expr->member_id == done_id_) {
	            if (!as_callee) { return method_value_error(expr, objectValue, false); }
                auto captured_handle = handle;
                auto* eng = engine_;
                script_function done_method = [captured_handle, eng](const std::vector<script_value>& a_args) -> checked_result<script_value> {
                    if (!a_args.empty()) {
                        return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "done() takes no arguments");
                    }
                    return script_value(captured_handle->done(), eng);
                };
                push_value(script_value::make_function(done_method, engine_));
                return {};
            }
            if (expr->member_id == advance_id_) {
	            if (!as_callee) { return method_value_error(expr, objectValue, false); }
                auto captured_handle = handle;
                auto* eng = engine_;
                script_function advance_method = [captured_handle, eng](const std::vector<script_value>& a_args) -> checked_result<script_value> {
                    return captured_handle->advance_from_script(eng, a_args);
                };
                push_value(script_value::make_function(advance_method, engine_));
                return {};
            }
            if (expr->member_id == result_id_) {
	            if (!as_callee) { return method_value_error(expr, objectValue, false); }
                auto captured_handle = handle;
                script_function result_method = [captured_handle](const std::vector<script_value>& a_args) -> checked_result<script_value> {
                    return captured_handle->result_from_script(a_args.size());
                };
                push_value(script_value::make_function(result_method, engine_));
                return {};
            }
            return checked_result<void>(make_error_code(runtime_error_code::member_not_found),
                "coroutine_handle has no member '{0}'", expr->member_id);
        }
    }

    // A captured container's count in a parallel body (KEEP BYTE-PARALLEL with vm_backend::exec_call_method)
    if (as_callee && objectValue.raw_storage_index() == script_value::TYPEID_PARALLEL_BORROW) [[unlikely]] {
        script_function counted = [borrow = objectValue, name = expr->member, eng = engine_](const std::vector<script_value>& args) -> checked_result<script_value> {
            if (auto count = detail::parallel_borrow_count(borrow, name, args.size(), eng)) { return std::move(*count); }
            return detail::parallel_borrow_method_refusal();
        };
        push_value(script_value::make_function(counted, engine_));
        return {};
    }

    // Handle string methods
    if (objectValue.is_string() || objectValue.is_utf8_string()) {
        if (!as_callee) {   // a caught error's own members and a thrown object's fields before the string methods (KEEP BYTE-PARALLEL with vm_backend::member_access_value)
            if (auto error_member = engine_->caught_error_member(objectValue, expr->member_id)) {
                push_value(std::move(*error_member));
                return {};
            }
        }
        const auto& table = objectValue.is_utf8_string() ? utf8_methods_ : string_methods_;
        auto methodIt = table.find(expr->member_id);
        if (methodIt != table.end()) {
            // Found the method in the registry
            if (!as_callee) { return method_value_error(expr, objectValue, false); }
            const builtin_method& method = methodIt->second;

            // Create a wrapper function that captures the string value by moving it
            script_function boundMethod = [ctx = builtin_ctx(), capturedValue = std::move(objectValue), method](const std::vector<script_value>& args) mutable -> checked_result<script_value> {
                std::vector<script_value> scratch;
                return method(ctx, capturedValue, detail::deref_builtin_args(args, scratch));
            };

            push_value(script_value::make_function(boundMethod, engine_));
            return {};
        }
        else {
            if (auto error_member = engine_->caught_error_member(objectValue, expr->member_id)) {   // e.file, e.line, ... on a caught error
                push_value(std::move(*error_member));
                return {};
            }
            raise_no_member(objectValue.is_utf8_string() ? "utf8_string" : "string", expr->member, expr->location);
            push_value(make_value());
            return {};
        }
    }

    // Handle array methods
    if (objectValue.is_array()) {
        auto methodIt = array_methods_.find(expr->member_id);
        if (methodIt != array_methods_.end()) {
            // Found the method in the registry
            if (!as_callee) { return method_value_error(expr, objectValue, false); }
            const builtin_method& method = methodIt->second;

            // Create a wrapper function that captures the array value by moving it
            script_function boundMethod = [ctx = builtin_ctx(), capturedValue = std::move(objectValue), method](const std::vector<script_value>& args) mutable -> checked_result<script_value> {
                std::vector<script_value> scratch;
                return method(ctx, capturedValue, detail::deref_builtin_args(args, scratch));
            };

            push_value(script_value::make_function(boundMethod, engine_));
            return {};
        }
        else {
            raise_no_member("array", expr->member, expr->location);
            push_value(make_value());
            return {};
        }
    }

    // Handle map methods
    if (objectValue.is_map()) {
        auto methodIt = map_methods_.find(expr->member_id);
        if (methodIt != map_methods_.end()) {
            // Found the method in the registry
            if (!as_callee) { return method_value_error(expr, objectValue, false); }
            const builtin_method& method = methodIt->second;

            // Create a wrapper function that captures the map value by moving it
            script_function boundMethod = [ctx = builtin_ctx(), capturedValue = std::move(objectValue), method](const std::vector<script_value>& args) mutable -> checked_result<script_value> {
                std::vector<script_value> scratch;
                return method(ctx, capturedValue, detail::deref_builtin_args(args, scratch));
            };

            push_value(script_value::make_function(boundMethod, engine_));
            return {};
        }
        else {
            // an enum's value table reads its keys with a dot (Direction.north); any other map's key is read with []
            auto& map_storage = const_cast<script_value&>(objectValue).get_map_storage();
            if (!map_storage || !map_storage->enum_values()) { return map_dot_key_error(expr->member_id); }
            auto it = map_storage->find(script_value(std::string(expr->member), engine_));
            if (it != map_storage->end()) {
                push_value(it->second);
                return {};
            }
            return checked_result<void>(make_error_code(runtime_error_code::member_not_found),
                "map has no member or key '{0}'", expr->member_id);
        }
    }

    // Handle weak_ptr methods
    if (objectValue.is_weak_ptr()) {
        auto methodIt = weak_ptr_methods_.find(expr->member_id);
        if (methodIt != weak_ptr_methods_.end()) {
            // Found the method in the registry
            if (!as_callee) { return method_value_error(expr, objectValue, false); }
            const builtin_method& method = methodIt->second;

            // Create a wrapper function that captures the weak_ptr value by moving it
            script_function boundMethod = [ctx = builtin_ctx(), capturedValue = std::move(objectValue), method](const std::vector<script_value>& args) mutable -> checked_result<script_value> {
                std::vector<script_value> scratch;
                return method(ctx, capturedValue, detail::deref_builtin_args(args, scratch));
            };

            push_value(script_value::make_function(boundMethod, engine_));
            return {};
        }
        else {
            return checked_result<void>(make_error_code(runtime_error_code::member_not_found),
                "weak_ptr has no member '{0}'", expr->member_id);
        }
    }

    // Handle shared_ptr methods (shared_ptr<T> explicitly marked objects)
    // After refactor: check type_info marker instead of storage type
    if (objectValue.get_type_info() &&
        objectValue.get_type_info()->base_type == script_value_type::jai_shared_ptr_type) {
        // This is an explicitly marked shared_ptr<T> object
        auto methodIt = shared_ptr_methods_.find(expr->member_id);
        if (methodIt != shared_ptr_methods_.end()) {
            // A user class method WINS over a same-named builtin handle method:
            // only mint the builtin when the underlying class does NOT define it.
            bool user_shadows = false;
            if (auto ci = objectValue.get_class_instance()) {
                if (auto* cd = ci->get_class_definition()) {
                    user_shadows = cd->defines_method(expr->member_id);
                }
            }
            if (!user_shadows) {
                // Found the method in the registry (reset, use_count, unique)
                if (!as_callee) { return method_value_error(expr, objectValue, false); }
                const builtin_method& method = methodIt->second;

                // Create a wrapper function that captures the shared_ptr value by moving it
                script_function boundMethod = [ctx = builtin_ctx(), capturedValue = std::move(objectValue), method](const std::vector<script_value>& args) mutable -> checked_result<script_value> {
                    std::vector<script_value> scratch;
                    return method(ctx, capturedValue, detail::deref_builtin_args(args, scratch));
                };

                push_value(script_value::make_function(boundMethod, engine_));
                return {};
            }
        }
        // Method not found in shared_ptr built-ins (or shadowed) - forward to the object
    }

    // After refactor: shared_ptr<T> uses same storage as regular objects
    // No unwrapping needed - just access the object_holder directly

    // Built-in same_as() for all values (identity/equality comparison)
    // same_as_id_ is a per-interpreter member (symbol ids are per-engine; a static here
    // cached the first engine's id and broke same_as on every later engine).
    if (expr->member_id == same_as_id_) {
	    if (!as_callee) { return method_value_error(expr, objectValue, false); }
        push_value(detail::same_as_method(objectValue, engine_));
        return {};
    }

    if (!objectValue.is_object()) {
        if (objectValue.is_null()) {
            return checked_result<void>(make_error_code(runtime_error_code::member_access_on_null), null_member_message(expr->object.get(), as_callee), expr->member_id, null_member_receiver_symbol(expr->object.get()));
        }
        if (const std::string_view cls = class_named_by_dot_receiver(expr->object.get(), objectValue, engine_); !cls.empty()) {
            const auto [text, symbol] = class_dot_member_error(*string_symbolizer_, parallel_worker_, cls, expr->member);
            return checked_result<void>(make_error_code(runtime_error_code::member_not_found), text, symbol);
        }
        return checked_result<void>(make_error_code(runtime_error_code::member_not_found),
            scalar_member_message, base_type_ids_[objectValue.type()], expr->member_id);
    }

    // One path for every object shape (class_instance wrapper, cpp_bound T&, raw holder)
    auto target = resolve_member_target(objectValue);
    if (!target) {
        // Cannot access member on non-class object
        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
    }

    if (target.class_def && target.class_def->chain_has_nonpublic()) [[unlikely]] {
        JAISCRIPT_TRY(detail::enforce_member_access(target.class_def, expr->member_id,
                                                    environment_->find_access_context()));
    }

    // Use pre-computed member_id from parser
    uint64_t member_id = expr->member_id;

    // The nearest USER _get_ in the chain (script or C++) wins over the field; the synthesized
    // accessor is skipped - its body IS the field rung below (KEEP BYTE-PARALLEL with the vm's
    // member_access_value). getter_id is cached on the node.
    if (target.class_def && target.class_def->has_property_getters()) {
        try {
            uint64_t getter_id = expr->getter_id;
            if (getter_id == UINT64_MAX) {
                auto [id, _] = string_symbolizer_->get_getter_id_with_view(member_id);
                getter_id = id;
                expr->getter_id = getter_id;
            }
            script_value getter = target.user_accessor(getter_id);
            if (!getter.is_null() && !getter.is_invalid() && getter.is_function()) {
                // The shared read (retired-layer report, receiver pin): the bare-name
                // rung of detail/member_scope.hpp runs the identical kernel
                auto result = detail::read_cpp_property(objectValue, target.class_def, target.instance.get(),
                                                        getter, getter_id);
                if (!result) {
                    return result.error_value();
                }
                push_value(std::move(result.value()));
                return {};
            }
        } catch (const std::exception&) {
            // If the getter fails (e.g., class definition expired), fall back to field access
            if (target.instance && target.instance->script_layer_retired()) { throw; }
        }
    }

    // Check if it's a field (script class fields without getters, or C++ fields)
    if (target.has_field(member_id)) {
        push_value(target.get_field(member_id));
        return {};
    }

    // Look for a method (bound to the receiver)
    script_value method = target.method(member_id);
    if (!method.is_null() && !method.is_invalid()) {
        // Return a bound method (function that has 'this' pre-bound)
        if (!as_callee) { return method_value_error(expr, objectValue, true); }
        push_value(create_bound_method(objectValue, method));
        return {};
    }

    // Check if this is a transparent wrapper - if so, unwrap and retry member access
    if (target.class_def && target.class_def->is_transparent_wrapper()) {
        script_value mutable_obj = objectValue;  // Need mutable copy for unwrap
        script_value unwrapped = target.class_def->unwrap(mutable_obj);
        // For primitive types (int, float, string, etc.), the value itself doesn't have
        // members (their operations are handled in binary operators); objects retry here
        if (!unwrapped.is_null() && unwrapped.is_object()) {
            auto unwrapped_target = resolve_member_target(unwrapped);
            if (unwrapped_target) {
                if (unwrapped_target.has_field(member_id)) {
                    push_value(unwrapped_target.get_field(member_id));
                    return {};
                }
                script_value unwrapped_method = unwrapped_target.method(member_id);
                if (!unwrapped_method.is_null() && !unwrapped_method.is_invalid()) {
                    if (!as_callee) { return method_value_error(expr, objectValue, true); }
                    push_value(create_bound_method(unwrapped, unwrapped_method));
                    return {};
                }
            }
        }
    }

    if (as_callee) {   // KEEP BYTE-PARALLEL with vm_backend::exec_probe_member and member_access_value
        if (auto refused = detail::refuse_cdtor_call(target.instance.get(), member_id)) { return *refused; }
    }
    raise_no_member(target.class_name(), expr->member, expr->location);
    push_value(make_value());  // Push null for failed member access
    return {};
}

checked_result<void> interpreter::visit_lambda_expr(lambda_expr* expr) {

    // Capture current environment for closure
    auto closure_env = environment_;

    // Check if we need a capture environment
    bool has_explicit_captures = !expr->captures.empty();
    bool has_default_capture = (expr->default_capture != lambda_expr::capture_default::none);
    
    
    const uint64_t this_id = string_symbolizer_->get_this_id();
    for (auto& capture : expr->captures) {
        if (capture.symbol_id == UINT64_MAX) {
            capture.symbol_id = string_symbolizer_->intern(capture.name);
        }
    }
    // The body's free names, once per node (KEEP BYTE-PARALLEL with vm_compiler::compile_lambda's used_variables)
    if (!expr->free_names_built) {
        std::unordered_set<uint64_t> declared;
        for (const auto& p : expr->parameters) {
            declared.insert(p.symbol_id != UINT64_MAX ? p.symbol_id : string_symbolizer_->intern(p.name));
        }
        detail::body_walker walker;
        walker.symbolizer = string_symbolizer_;
        walker.on_declared = [&declared](uint64_t id) { declared.insert(id); };
        walker.stmt(expr->body);
        walker.on_declared = {};
        std::unordered_set<uint64_t> seen;
        walker.on_identifier = [&](identifier_expr* ident) {
            if (!ident->global_qualified && !declared.count(ident->symbol_id) && seen.insert(ident->symbol_id).second) {
                expr->free_names.push_back(ident->symbol_id);
            }
        };
        walker.on_instance = [&]() {
            if (seen.insert(this_id).second) { expr->free_names.push_back(this_id); }
        };
        walker.stmt(expr->body);
        for (uint64_t sym : expr->chained_outer_names) {
            if (!declared.count(sym) && seen.insert(sym).second) { expr->free_names.push_back(sym); }
        }
        expr->uses_instance = seen.count(this_id) != 0;
        expr->free_names_built = true;
    }
    const std::vector<uint64_t>& free_names = expr->free_names;
    const bool uses_instance = expr->uses_instance;
    bool captures_this = has_default_capture && uses_instance;
    for (const auto& capture : expr->captures) {
        if (capture.symbol_id == this_id) {
            captures_this = true;
            break;
        }
    }
    if (uses_instance && !captures_this) {
        return checked_result<void>(make_error_code(runtime_error_code::capture_undefined_variable),
            "Lambda capture list does not capture 'this'; use [this], [=] or [&]");
    }

    // Outer locals carry the enclosing frame's slot numbering, so each one copies into the capture environment and its node
    // falls back to the env lookup; outer-ness is by symbol, since both frames number their slots from 0
    struct outer_slot_ref {
        identifier_expr* node;
        uint64_t symbol_id;
        size_t outer_slot;
    };
    std::vector<outer_slot_ref> outer_slot_refs;
    std::unordered_set<uint64_t> outer_slots_captured;

    if (!call_stack_.empty() && expr->outer_slot_plan_built) {
        // Later creations from this AST replay the first creation's plan: its body identifiers are already patched
        const size_t outer_slot_count = call_stack_.back().local_count();
        for (const auto& [sym, slot] : expr->outer_slot_plan) {
            if (slot < outer_slot_count && outer_slots_captured.insert(sym).second) {
                outer_slot_refs.push_back({nullptr, sym, slot});
            }
        }
    } else if (!call_stack_.empty()) {
        const size_t outer_slot_count = call_stack_.back().local_count();
        std::unordered_set<uint64_t> lambda_declared;
        for (const auto& p : expr->parameters) {
            if (p.symbol_id == UINT64_MAX) {
                p.symbol_id = string_symbolizer_->intern(p.name);
            }
            lambda_declared.insert(p.symbol_id);
        }
        std::vector<identifier_expr*> outer_slot_candidates;
        detail::body_walker walker;
        walker.symbolizer = string_symbolizer_;
        walker.on_declared = [&lambda_declared](uint64_t id) { lambda_declared.insert(id); };
        walker.on_identifier = [&](identifier_expr* ident) {
            if (ident->slot_index != SIZE_MAX && ident->slot_index < outer_slot_count) {
                outer_slot_candidates.push_back(ident);
            }
        };
        walker.stmt(expr->body);

        // Classify: anything the lambda itself declares resolves in its own frame;
        // every other slot-carrying identifier references the outer frame.
        for (auto* ident : outer_slot_candidates) {
            if (lambda_declared.count(ident->symbol_id)) continue;
            if (outer_slots_captured.insert(ident->symbol_id).second) {
                outer_slot_refs.push_back({ident, ident->symbol_id, ident->slot_index});
            } else {
                // Same symbol, different node — patch now (firsts are patched after capture)
                ident->slot_index = SIZE_MAX;
            }
        }

        for (const auto& [sym, slot] : expr->chained_outer_slots) {
            if (slot < outer_slot_count && outer_slots_captured.insert(sym).second) {
                outer_slot_refs.push_back({nullptr, sym, slot});
            }
        }

        // Persist the plan so later closure creations from this AST (whose identifier
        // slots are patched below) can replay the same captures.
        expr->outer_slot_plan.reserve(outer_slot_refs.size());
        for (const auto& ref : outer_slot_refs) {
            expr->outer_slot_plan.emplace_back(ref.symbol_id, ref.outer_slot);
        }
        expr->outer_slot_plan_built = true;
    }

    // A capture list names everything the lambda captures, checked at the node's first creation (KEEP BYTE-PARALLEL with
    // vm_backend::exec_closure): the parser already rejected an unlisted frame local, this is the environment and member half
    if (!has_default_capture && !expr->capture_list_verified) {
        auto listed = [&](uint64_t id) {
            return std::any_of(expr->captures.begin(), expr->captures.end(), [id](const auto& c) { return c.symbol_id == id; });
        };
        for (const auto& ref : outer_slot_refs) {
            if (!listed(ref.symbol_id)) {
                return checked_result<void>(make_error_code(runtime_error_code::capture_undefined_variable),
                    "Lambda capture list does not capture enclosing local '{0}'; name it or use [=] or [&]", ref.symbol_id);
            }
        }
        for (uint64_t id : free_names) {
            if (id == this_id || listed(id)) { continue; }
            auto source = detail::resolve_capture_source(nullptr, environment_.get(), member_scope_, id);
            if (source.global || source.what == detail::capture_source::kind::miss) { continue; }
            if (captures_this && (source.what == detail::capture_source::kind::member ||
                source.what == detail::capture_source::kind::cpp_property || source.what == detail::capture_source::kind::shadow)) { continue; }
            return checked_result<void>(make_error_code(runtime_error_code::capture_undefined_variable),
                "Lambda capture list does not capture enclosing name '{0}'; name it, or use [this], [=] or [&]", id);
        }
        if (!engine_->parallel_region_active()) { expr->capture_list_verified = true; }
    }
    // A closure's parent is its program scope (a namespace keeps its live variables), never the creating frame
    const environment* program_scope_raw = detail::program_scope_of(closure_env.get());
    const bool needs_capture_env = has_explicit_captures || !outer_slot_refs.empty() || (has_default_capture && !free_names.empty()) ||
        (program_scope_raw && program_scope_raw->parent_raw());
    std::shared_ptr<environment> final_closure_env;

    if (needs_capture_env) {
        // the capture environment holds every captured name itself, so it parents on the program scope, never on the creating
        // frame's scope (an escaping closure would otherwise keep that frame's whole chain)
        auto captureEnv = std::make_shared<environment>(detail::lambda_parent_scope(closure_env), string_symbolizer_);
        
        if (captures_this && has_default_capture) {
            auto result = environment_->get(this_id);
            if (!result) { return result.error_value(); }
            captureEnv->define(this_id, script_value(result.value().deref()));
        }
        // Process default captures first ([=] or [&])
        if (has_default_capture && !free_names.empty()) {
            bool capture_by_ref = (expr->default_capture == lambda_expr::capture_default::by_reference);

            for (uint64_t var_id : free_names) {
                if (var_id == this_id) { continue; }
                // Check if this variable is explicitly overridden in the capture list
                bool is_overridden = false;
                for (const auto& capture : expr->captures) {
                    if (capture.symbol_id == var_id) {
                        is_overridden = true;
                        break;
                    }
                }

                if (!is_overridden) {
                    // The capture source in the runtime's order (detail/capture_ladder.hpp):
                    // the outer frame's slot, the member scope, then the env chain - a
                    // same-named global never wins over a local or a field. [&] cell-shares
                    // the source (escape-legal: the cell outlives the frame), [=] snapshots
                    // it; a hybrid's C++ property follows the field rule through its
                    // accessors.
                    script_value* slot_val = nullptr;
                    if (!call_stack_.empty()) {
                        for (const auto& ref : outer_slot_refs) {
                            if (ref.symbol_id == var_id) {
                                slot_val = call_stack_.back().get_local(ref.outer_slot);
                                break;
                            }
                        }
                    }
                    auto src = detail::resolve_capture_source(slot_val, environment_.get(), member_scope_, var_id);
                    if (src.what == detail::capture_source::kind::denied) [[unlikely]] {
                        return *src.access_error;
                    }
                    if (src.global) { continue; }   // a global resolves when the lambda runs (KEEP BYTE-PARALLEL with vm exec_closure)
                    if (src.cell) {
                        if (capture_by_ref) {
                            captureEnv->define(var_id, share_boxed_slot(*src.cell));
                        } else {
                            captureEnv->define(var_id, detail::clone_for_capture(src.cell->deref()));
                        }
                    } else if (src.what == detail::capture_source::kind::cpp_property) {
                        auto read = capture_by_ref
                            ? detail::bind_cpp_property_ref(*member_scope_.this_storage, var_id, engine_, string_symbolizer_)
                            : detail::read_bare_cpp_property(*member_scope_.this_storage, src.property.instance,
                                                             src.property.method_value, src.property.getter_id, var_id);
                        if (!read) {
                            return read.error_value();
                        }
                        captureEnv->define(var_id, capture_by_ref ? std::move(read.value())
                                                                  : detail::clone_for_capture(read.value()));
                    }
                }
            }
        }
        
        // Process explicit captures
        for (const auto& capture : expr->captures) {
            script_value* slot_val = nullptr;
            if (!call_stack_.empty()) {
                if (capture.outer_slot != SIZE_MAX && capture.outer_slot < call_stack_.back().local_count()) {
                    slot_val = call_stack_.back().get_local(capture.outer_slot);
                }
                if (!slot_val) {
                    for (const auto& ref : outer_slot_refs) {
                        if (ref.symbol_id == capture.symbol_id) {
                            slot_val = call_stack_.back().get_local(ref.outer_slot);
                            break;
                        }
                    }
                }
            }
            if (!slot_val && capture.symbol_id == this_id) {
                // 'this' - method_environment provides it via get() / get_value_ptr()
                // even though contains() might return false
                if (!environment_->contains(this_id) && !environment_->get_value_ptr(this_id)) {
                    return checked_result<void>(make_error_code(runtime_error_code::capture_undefined_variable),
                        environment_->undefined_text(capture.symbol_id, "Cannot capture undefined variable '{0}'"), capture.symbol_id);
                }
                if (capture.by_reference) {
                    script_value* targetPtr = environment_->get_value_ptr(this_id);
                    if (!targetPtr) {
                        return checked_result<void>(make_error_code(runtime_error_code::capture_reference_failed),
                            "Cannot capture variable '{0}' by reference", capture.symbol_id);
                    }
                    captureEnv->define(this_id, share_boxed_slot(*targetPtr));
                } else {
                    // [this] captures the instance itself, as C++ captures the this pointer (KEEP BYTE-PARALLEL with vm_backend::exec_closure)
                    auto capture_result = environment_->get(this_id);
                    if (!capture_result) return capture_result.error_value();
                    captureEnv->define(this_id, script_value(capture_result.value().deref()));
                }
                continue;
            }
            // An init-capture [x = e]: e evaluated here, in the enclosing scope, into the lambda's own x (a temporary moves, as C++ initializes it)
            if (capture.initializer) {
                JAISCRIPT_TRY(dispatch_expr(capture.initializer.get()));
                script_value init = pop_value();
                if (is_unwinding_) {
                    push_value(make_value());
                    return {};
                }
                const bool takes = !init.is_reference() && detail::source_skips_copy(source_class(capture.initializer.get()), init);
                captureEnv->define(capture.symbol_id, takes ? detail::take_for_assignment(init) : clone_for_assignment(init.deref()));
                continue;
            }
            // The capture source in the runtime's order (detail/capture_ladder.hpp): the
            // outer frame's slot, the function's own scopes, then the env chain - a member
            // shadows the chain and is not a capturable variable
            auto src = detail::resolve_capture_source(slot_val, environment_.get(), member_scope_, capture.symbol_id);
            if (!src.is_variable()) {
                return checked_result<void>(make_error_code(runtime_error_code::capture_undefined_variable),
                    environment_->undefined_text(capture.symbol_id, "Cannot capture undefined variable '{0}'"), capture.symbol_id);
            }
            if (capture.by_reference && src.global) { continue; }   // [&g] of a global: the lambda reads it when it runs
            if (capture.by_reference) {
                // Cell share (escape-legal, C++ [&x] semantics): the storage boxes in
                // place; the closure and the original share the cell
                captureEnv->define(capture.symbol_id, share_boxed_slot(*src.cell));
            } else {
                captureEnv->define(capture.symbol_id, detail::clone_for_capture(src.cell->deref()));
            }
        }

        // Materialise outer-frame locals into the capture env and patch their
        // slot_index to SIZE_MAX so every subsequent call resolves via the env
        // (O(1) hash lookup) rather than the wrong call-frame slot.
        if (!call_stack_.empty()) {
            for (auto& ref : outer_slot_refs) {
                // Skip if already captured explicitly / by default-capture above - OWN
                // storage only, a same-named global behind the parent is not a capture
                // (ref.node is null when replaying a stored plan — already patched then)
                if (captureEnv->get_own_value_ptr(ref.symbol_id)) {
                    if (ref.node) { ref.node->slot_index = SIZE_MAX; }
                    continue;
                }
                script_value* slot_val = call_stack_.back().get_local(ref.outer_slot);
                if (slot_val) {
                    if (expr->default_capture == lambda_expr::capture_default::by_reference) {
                        // C++ [&] semantics: share the cell (the slot boxes in place;
                        // escape-legal - the cell outlives the frame)
                        captureEnv->define(ref.symbol_id, share_boxed_slot(*slot_val));
                    } else {
                        // Automatic / [=] capture of a local: by value
                        captureEnv->define(ref.symbol_id, detail::clone_for_capture(slot_val->deref()));
                    }
                }
                // Patch the AST node so future lookups go through the env path
                if (ref.node) { ref.node->slot_index = SIZE_MAX; }
            }
        }

        // If [this] was captured, we need to use a method_environment instead
        // so that member variables can be accessed without "this."
        if (captures_this) {
            // Get the 'this' object that was captured
            auto this_result = captureEnv->get(this_id);
            if (!this_result) {
                return checked_result<void>(make_error_code(runtime_error_code::capture_reference_failed),
                    "Failed to capture 'this' reference");
            }
            script_value this_obj = std::move(this_result.value());

            // the captured this scopes the body, parented on the environment the lambda was defined in
            auto method_env = method_environment(closure_env, this_obj);
            method_env->define(this_id, this_obj);

            // The other captures sit in an environment in front of the instance scope - the capture env's OWN storage only,
            // never a global behind its parent - so a call sees them ahead of the members, as a lambda's own names
            // (a method env as the closure is replaced by the call's own; KEEP BYTE-PARALLEL with vm_backend::exec_closure)
            std::shared_ptr<environment> carrier;
            auto carry = [&](uint64_t var_id) {
                if (var_id == this_id) { return; }
                if (script_value* captured = captureEnv->get_own_value_ptr(var_id)) {
                    if (!carrier) { carrier = std::make_shared<environment>(method_env, string_symbolizer_); }
                    carrier->define(var_id, *captured);
                }
            };
            for (const auto& capture : expr->captures) { carry(capture.symbol_id); }
            if (has_default_capture) {
                for (uint64_t var_id : free_names) { carry(var_id); }
            }

            final_closure_env = carrier ? carrier : method_env;
        } else {
            final_closure_env = captureEnv;
        }
    } else {
        // No captures needed - use current environment directly (fast path)
        final_closure_env = closure_env;
        
    }
    
    std::shared_ptr<block_stmt> lambdaBody = std::dynamic_pointer_cast<block_stmt>(expr->body);
    if (!lambdaBody) {
        std::vector<declaration_ptr> stmts;
        if (auto stmt = std::dynamic_pointer_cast<statement>(expr->body)) {
            stmts.push_back(std::make_shared<statement_decl>(expr->location, stmt));
        }
        lambdaBody = std::make_shared<block_stmt>(expr->location, std::move(stmts));
    }
    auto lambdaFunc = std::make_shared<script_defined_function>("<lambda>", shared_parameters_for(expr), expr->return_type, lambdaBody,
        needs_capture_env ? final_closure_env : nullptr, expr->local_count);
    lambdaFunc->unit = current_unit();

    // Create a script_function wrapper resolving the live backend at call time.
    // Named thunk (not an anonymous lambda) so callers can recover the payload via
    // std::function::target<script_callable_thunk>() - vm parity (exec_closure).
    script_callable payload;
    payload.kind = script_callable::kind_type::function;
    payload.fn = lambdaFunc;
    script_function funcWrapper = script_callable_thunk{engine_, std::move(payload)};

    // Push the lambda as a function value
    push_value(script_value::make_function(funcWrapper, engine_));
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_new_expr_t(new_expr* expr, const replay_point* rs) {
    // A new expression is a call boundary too: preserve its source expressions for reference parameters.
    auto invoke_constructor = [&](const script_function& function, const std::vector<script_value>& args) {
        detail::call_site_context ctx{&expr->arguments, &args};
        struct restore_context {
            const detail::call_site_context*& slot;
            const detail::call_site_context* previous;
            ~restore_context() { slot = previous; }
        } restore{pending_call_ctx_, pending_call_ctx_};
        pending_call_ctx_ = &ctx;
        return call_value(function, args);
    };

    // This handles expressions like: new Point(), new Point(3.0, 4.0), etc.
    // The new_expr contains a type and arguments

    // std::cerr << "DEBUG: visit_new_expr called for type: " << (expr->type ? expr->type->type_name : "NULL") << std::endl;

    if (!expr->type) {
        // New expression missing type information
        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
    }

    // Handle built-in types specially
    if (expr->type->base_type == script_value_type::jai_array_type) {
        // array<T>{} constructor
        if (!expr->arguments.empty()) {
            // array{} constructor does not take arguments
            return checked_result<void>(make_error_code(runtime_error_code::argument_count_mismatch));  // [ErrorText] Invalid argument count
        }

        // Create empty array with the specified element type
        auto element_type = expr->type->element_type();
        if (!element_type) {
            if (auto eng = engine_) {
                element_type = eng->get_type_info_int(); // Default to int if no type specified
            }
        }
        push_value(script_value::make_array(element_type, engine_));
        return {};
    }

    if (expr->type->base_type == script_value_type::jai_map_type) {
        // map<K,V>{} constructor
        if (!expr->arguments.empty()) {
            // map{} constructor does not take arguments
            return checked_result<void>(make_error_code(runtime_error_code::argument_count_mismatch));  // [ErrorText] Invalid argument count
        }

        // Create empty map with the specified key/value types
        auto key_type = expr->type->key_type();
        auto value_type = expr->type->value_type();
        if (auto eng = engine_) {
            if (!key_type) key_type = eng->get_type_info_string();
            if (!value_type) value_type = eng->get_type_info_int();
        }
        push_value(script_value::make_map(key_type, value_type, engine_));
        return {};
    }
    
    if (expr->type->base_type == script_value_type::jai_weak_ptr_type) {
        // weak_ptr<T>() or weak_ptr<T>(obj) constructor
        if (expr->arguments.empty()) {
            // No arguments - create empty weak_ptr
            push_value(script_value::make_empty_weak_ptr(expr->type, engine_));
        } else if (expr->arguments.size() == 1) {
            // One argument - create weak_ptr from object
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->arguments[0].get(), rs, 0), expr, rp_new_weak, 0, {});
            script_value obj = pop_value();

            // Handle null objects
            if (obj.is_null()) {
                push_value(script_value::make_empty_weak_ptr(expr->type, engine_));
                return {};
            }

            // Allow creating weak_ptr from:
            // 1. Another weak_ptr (copy constructor)
            // 2. shared_ptr<T> (jai_shared_ptr_type)
            // NOTE: Regular objects have value semantics and cannot be used with weak_ptr

            if (obj.is_weak_ptr()) {
                // Copy constructor - just return the weak_ptr as-is
                push_value(std::move(obj));
                return {};
            }

            // Check if obj is a shared_ptr (required for weak_ptr)
            if (obj.type() != script_value_type::jai_shared_ptr_type) {
                // Get expected type ID for error context
                uint64_t expected_id = (expr->type && !expr->type->type_params.empty())
                    ? expr->type->type_params[0]->id : 0;

                if (obj.type() == script_value_type::jai_object_type) {
                    // Helpful error for value-semantic objects
                    return checked_result<void>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Cannot create weak_ptr from value-semantic object. Use shared_ptr<T>.",
                        expected_id);
                } else {
                    auto type_info = obj.get_type_info();
                    uint64_t actual_id = type_info ? type_info->id : 0;
                    return checked_result<void>(
                        make_error_code(runtime_error_code::type_mismatch),
                        "Cannot create weak_ptr from non-shared_ptr type. Use shared_ptr<T>.",
                        expected_id, actual_id);
                }
            }

            // Validate type parameter - weak_ptr<T> should only accept shared_ptr<T> or subclass
            // Skip validation for weak_ptr<var> (any type)
            auto expected_type = expr->type->element_type();
            auto obj_type_info = obj.get_type_info();
            if (expected_type && obj_type_info &&
                expected_type->base_type != script_value_type::jai_any_type) {
                // Get the actual class name from the shared_ptr's inner type
                const std::string& expected_class = expected_type->type_name;
                const std::string& actual_class = obj_type_info->element_type()
                    ? obj_type_info->element_type()->type_name
                    : obj_type_info->type_name;

                // Check if types match or if actual is a subclass of expected
                if (expected_class != actual_class) {
                    auto eng = engine_;
                    if (eng) {
                        auto actual_def = eng->get_class_definition(actual_class);
                        if (!actual_def || !actual_def->is_subtype_of(expected_class)) {
                            uint64_t expected_id = expected_type->id;
                            uint64_t actual_id = obj_type_info->element_type()
                                ? obj_type_info->element_type()->id
                                : obj_type_info->id;
                            return checked_result<void>(
                                make_error_code(runtime_error_code::type_mismatch),
                                "weak_ptr type mismatch: type must match or be a subclass",
                                expected_id, actual_id);
                        }
                    }
                }
            }

            // Create weak_ptr from the shared_ptr
            auto weak_result = script_value::make_weak_ptr(obj, engine_);
            if (!weak_result) {
                return weak_result.error_value();
            }
            push_value(std::move(weak_result.value()));
        } else {
            // weak_ptr() expects 0 or 1 arguments
            return checked_result<void>(make_error_code(runtime_error_code::argument_count_mismatch));  // [ErrorText] Invalid argument count
        }
        return {};
    }
    
    if (expr->type->base_type == script_value_type::jai_shared_ptr_type) {
        // shared_ptr<T>(args...) - forward args to T's constructor, then mark as shared_ptr
        // This is like C++ make_shared - args go directly to T's constructor
        // shared_ptr is a TYPE MARKER only - it affects cloning behavior, not storage

        if (expr->arguments.empty()) {
            // No arguments - call T's default constructor
            auto inner_type = expr->type->element_type();
            if (!inner_type) {
                // shared_ptr with no type parameter and no args - create null
                push_value(make_value());
                return {};
            }
            std::string innerTypeName = inner_type->type_name;

            // Look up T's constructor
            auto ctor_result = environment_->get(innerTypeName, *string_symbolizer_);
            if (!ctor_result || !ctor_result.value().is_function()) {
                return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                    "No constructor found for class '{0}'", inner_type->id);
            }

            script_value constructorFunc = std::move(ctor_result.value());
            const script_function& func = constructorFunc.as_function();
            auto result = call_value(func, {});  // Call with no args
            if (!result) {
                return result.error_value();
            }

            script_value value = std::move(result.value());
            // Mark as shared_ptr type
            if (value.type() == script_value_type::jai_object_type) {
                value.set_type_info(expr->type);
            }
            push_value(std::move(value));
            return {};
        }

        // Has arguments - forward them to T's constructor
        auto inner_type = expr->type->element_type();
        if (!inner_type) {
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                "shared_ptr requires a type parameter when called with arguments");
        }
        std::string innerTypeName = inner_type->type_name;

        // Evaluate all arguments
        std::vector<script_value> args;
        if constexpr (Replaying) { if (rs) args = rs->partial; }
        for (size_t ai = args.size(); ai < expr->arguments.size(); ++ai) {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->arguments[ai].get(), rs, ai), expr, rp_new_shared, ai, args);
            args.push_back(pop_value());
            if (auto taken = capture_call_argument(engine_, expr->arg_captures, ai, args.back(), [&] {
                    auto ctor = environment_->get(innerTypeName, *string_symbolizer_);
                    return ctor ? detail::callee_binding(ctor.value(), ai, false, expr->arguments.size()) : detail::arg_binding::opaque;
                }, [&](script_value& out) { return mint_argument_place(expr->arguments[ai].get(), out); }); !taken) {
                return taken.error_value();
            }
        }

        // Look up T's constructor
        auto ctor_result = environment_->get(innerTypeName, *string_symbolizer_);
        if (!ctor_result || !ctor_result.value().is_function()) {
            return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
                "No constructor found for class '{0}'", inner_type->id);
        }

        script_value constructorFunc = std::move(ctor_result.value());
        const script_function& func = constructorFunc.as_function();
        auto result = invoke_constructor(func, args);
        if (!result) {
            return result.error_value();
        }

        script_value value = std::move(result.value());
        // Mark as shared_ptr type
        if (value.type() == script_value_type::jai_object_type) {
            value.set_type_info(expr->type);
        }
        push_value(std::move(value));
        return {};
    }
    
    std::string className = expr->type->type_name;

    // Evaluate all arguments
    std::vector<script_value> args;
    if constexpr (Replaying) { if (rs) args = rs->partial; }
    for (size_t ai = args.size(); ai < expr->arguments.size(); ++ai) {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->arguments[ai].get(), rs, ai), expr, rp_new_class, ai, args);
        args.push_back(pop_value());
        if (auto taken = capture_call_argument(engine_, expr->arg_captures, ai, args.back(), [&] {
                auto ctor = environment_->get(className, *string_symbolizer_);
                return ctor ? detail::callee_binding(ctor.value(), ai, false, expr->arguments.size()) : detail::arg_binding::opaque;
            }, [&](script_value& out) { return mint_argument_place(expr->arguments[ai].get(), out); }); !taken) {
            return taken.error_value();
        }
    }

    // Look for a constructor function registered with this class name
    // The class builder registers constructors as overloaded functions
    auto ctor_result = environment_->get(className, *string_symbolizer_);
    if (ctor_result && ctor_result.value().is_function()) {
        script_value constructorFunc = std::move(ctor_result.value());
        const script_function& func = constructorFunc.as_function();
        auto result = invoke_constructor(func, args);
        if (!result) {
            // Function returned error - propagate it up
            return result.error_value();
        }
        push_value(std::move(result.value()));
        return {};
    }

    // No constructor found for class
    return checked_result<void>(make_error_code(runtime_error_code::undefined_variable),
        "No constructor found for class '{0}'", expr->type->id);
}

template<bool Replaying>
checked_result<void> interpreter::visit_ternary_expr_t(ternary_expr* expr, const replay_point* rs) {
    // Evaluate the condition (a replay of the chosen arm carries it as partial[0])
    if (!parked<Replaying>(rs, 0)) {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->condition.get(), rs, 0), expr, rp_operand, 0, {});
    }
    script_value conditionValue = parked<Replaying>(rs, 0) ? rs->partial[0] : pop_value();

    // a throw unwinding out of the condition leaves a null placeholder, not a bool: neither branch runs, the unwind propagates
    if (is_unwinding_) {
        push_value(make_value());
        return {};
    }

    // Evaluate only the selected branch (short-circuit evaluation)
    if (is_truthy(conditionValue)) {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->then_expression.get(), rs, 1), expr, rp_operand, 1, (std::vector<script_value>{conditionValue}));
    } else {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->else_expression.get(), rs, 1), expr, rp_operand, 1, (std::vector<script_value>{conditionValue}));
    }
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_array_literal_expr_t(array_literal_expr* expr, const replay_point* rs) {
    // Create array script_value with no element type constraint
    // Untyped array literals (e.g., [1, 2, 3]) allow any element type
    // The type will be set by the variable declaration if one exists (e.g., array<int> arr = [...])
    type_info_ptr element_type = nullptr;  // No constraint - allows any type
    script_value arrayValue = script_value::make_array(element_type, engine_);
    size_t element_start = 0;
    if constexpr (Replaying) {
        // the literal under construction rides the frame as partial[0]
        if (rs) { arrayValue = rs->partial[0]; element_start = rs->index; }
    }

    // Get the internal vector to populate
    auto& array = const_cast<std::vector<script_value>&>(arrayValue.as_array());

    // Reserve capacity to avoid reallocations (optimization)
    array.reserve(expr->elements.size());

    // Evaluate each element and add to array. All-detach rule:
    // literal construction is a store boundary - bound primitives
    // snapshot like every assignment-shaped store, and element/subscript reads that
    // arrive as reference wrappers normalize to VALUES exactly like assignment
    // (a literal must never hold a live reference into its source; to_json emitted
    // null for them). Copy out through a temp: assigning deref() straight into elem
    // destroys the holder that owns the deref target. (KEEP BYTE-PARALLEL with
    // vm_backend::exec_array)
    for (size_t ei = element_start; ei < expr->elements.size(); ++ei) {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->elements[ei].get(), rs, ei), expr, rp_operand, ei, (std::vector<script_value>{arrayValue}));
        script_value elem = pop_value();
        const bool copy = elem.is_reference() || is_lvalue_shaped_expr(expr->elements[ei].get());   // an existing object: the literal holds its own copy
        if (elem.is_reference()) [[unlikely]] {
            script_value derefed = elem.deref();
            elem = std::move(derefed);
        }
        if (elem.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
            elem = elem.detached_for_store();
        }
        if (copy) { elem = clone_for_assignment(elem); }
        array.push_back(std::move(elem));
    }

    push_value(std::move(arrayValue));
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_map_literal_expr_t(map_literal_expr* expr, const replay_point* rs) {
    // Create map script_value with no key/value type constraints
    // Untyped map literals (e.g., {"a": 1}) allow any key/value types
    // The type will be set by the variable declaration if one exists (e.g., map<string,int> m = {...})
    type_info_ptr keyType = nullptr;    // No constraint - allows any key type
    type_info_ptr valueType = nullptr;  // No constraint - allows any value type
    script_value mapValue = script_value::make_map(keyType, valueType, engine_);
    // Replay: the map under construction rides the frame as partial[0]; a suspended value
    // expression also parks its evaluated key as partial[1] (ordinals: 2i key, 2i+1 value)
    size_t entry_start = 0;
    std::optional<script_value> parked_key;
    if constexpr (Replaying) {
        if (rs) {
            mapValue = rs->partial[0];
            entry_start = rs->index / 2;
            if (rs->index % 2 == 1) parked_key = rs->partial[1];
        }
    }

    // Get the internal map to populate
    auto& map = const_cast<script_map&>(mapValue.as_map());

    // Evaluate each key-value pair and add to map
    for (size_t ei = entry_start; ei < expr->entries.size(); ++ei) {
        const auto& entry = expr->entries[ei];
        // Evaluate key
        bool key_parked = false;
        if constexpr (Replaying) { key_parked = parked_key.has_value() && ei == entry_start; }
        if (!key_parked) {
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(entry.first.get(), rs, 2 * ei), expr, rp_operand, 2 * ei, (std::vector<script_value>{mapValue}));
        }
        script_value key = key_parked ? std::move(*parked_key) : pop_value();

        // Evaluate value
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(entry.second.get(), rs, 2 * ei + 1), expr, rp_operand, 2 * ei + 1, (std::vector<script_value>{mapValue, key}));
        script_value value = pop_value();
        const bool copy_key = key.is_reference() || is_lvalue_shaped_expr(entry.first.get());   // existing objects: the literal holds its own copies
        const bool copy_value = value.is_reference() || is_lvalue_shaped_expr(entry.second.get());

        // All-detach rule: literal construction is a store boundary -
        // bound primitives snapshot, and reference-wrapper reads normalize to VALUES
        // like assignment (see the array-literal twin for the argument; KEEP
        // BYTE-PARALLEL with vm_backend::exec_map)
        if (key.is_reference()) [[unlikely]] {
            script_value derefed = key.deref();
            key = std::move(derefed);
        }
        if (value.is_reference()) [[unlikely]] {
            script_value derefed = value.deref();
            value = std::move(derefed);
        }
        if (key.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
            key = key.detached_for_store();
        }
        if (value.raw_storage_index() == script_value::TYPEID_CPP_BOUND) [[unlikely]] {
            value = value.detached_for_store();
        }
        if (copy_key) { key = clone_for_assignment(key); }
        if (copy_value) { value = clone_for_assignment(value); }

        // Insert into map
        map.insert_or_assign(std::move(key), std::move(value));
    }

    push_value(std::move(mapValue));
    return {};
}

checked_result<void> interpreter::visit_this_expr(this_expr* expr) {
    // Try to get 'this' from the current environment
    // Use the symbolizer's cached ID (not interpreter's this_id_ which may be stale)
    script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id());
    if (!this_ptr) {
        // 'this' can only be used inside methods
        return checked_result<void>(make_error_code(runtime_error_code::this_outside_method),
            "'this' can only be used inside methods");
    }
    push_value(*this_ptr);
    return {};
}

checked_result<void> interpreter::visit_super_expr(super_expr* expr) {
    // Super expression is used for accessing parent class methods: super::method()
    // Constructor delegation (Enemy() : super()) is handled in the parser/constructor

    // Get 'this' from the environment - super only makes sense in instance methods
    // Use the symbolizer's cached ID (not interpreter's this_id_ which may be stale)
    script_value* this_ptr = environment_->get_value_ptr(string_symbolizer_->get_this_id());
    if (!this_ptr) {
        // 'this' not found - super used outside of class method
        return checked_result<void>(make_error_code(runtime_error_code::super_outside_method),
            "'super' can only be used inside methods");
    }
    script_value this_value = *this_ptr;
    if (this_value.is_null()) {
        // 'this' is null - super used outside of class method
        return checked_result<void>(make_error_code(runtime_error_code::super_outside_method),
            "'super' can only be used inside methods");
    }

    // Push 'this' onto the stack - visit_member_expr will handle the parent lookup
    // when it detects that the object expression is a super_expr
    push_value(std::move(this_value));
    return {};
}

// cast<T>(e) / cast<T&>(e) / cast<shared_ptr<T>>(e): operand shape, class verdict and
// class-target outcomes come from detail/cast_kernel.hpp; primitive/container targets
// convert through the assignment table. KEEP BYTE-PARALLEL with vm_backend::exec_cast.
template<bool Replaying>
checked_result<void> interpreter::visit_cast_expr_t(cast_expr* expr, const replay_point* rs) {
    if (expr->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(expr, expr->const_check)); }
    script_value operand;
    switch (detail::cast_operand_mode_of(expr)) {
        case detail::cast_operand_mode::identifier: {
            // The ref-decl identifier arm: slot-first, then env
            auto* ident = static_cast<identifier_expr*>(expr->operand.get());
            const uint64_t targetSymbolId = string_symbolizer_->intern(ident->name);
            script_value* targetPtr = resolve_local_or_env(ident->slot_index, targetSymbolId, ident->global_qualified);
            if (!targetPtr) {
                auto minted = bind_bare_cpp_property_ref(targetSymbolId, ident->global_qualified);
                if (!minted) {
                    JAISCRIPT_TRY(bare_reference_access(targetSymbolId));
                    if (auto method = bare_method_reference_error(ident)) { return *method; }
                    return checked_result<void>(make_error_code(runtime_error_code::undefined_variable), environment_->undefined_text(targetSymbolId, "Cannot take reference of undefined variable"), targetSymbolId);
                }
                if (!*minted) {
                    return minted->error_value();
                }
                operand = std::move(minted->value());
                break;
            }
            if (targetPtr->is_reference()) {
                auto aliased = decl_ref_alias(*targetPtr, engine_);
                if (!aliased) {
                    return aliased.error_value();
                }
                operand = std::move(aliased.value());
            } else {
                operand = share_boxed_slot(*targetPtr);
            }
            break;
        }
        case detail::cast_operand_mode::lvalue_chain: {
            auto resolved = detail::resolve_ref_lvalue(expr->operand.get(),
                detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
                environment_.get(), engine_, string_symbolizer_);
            if (!resolved) {
                if (resolved.error() != make_error_code(runtime_error_code::invalid_reference)) {
                    return resolved.error_value();
                }
                auto part = component_reference(expr->operand.get(), call_stack_.empty() ? nullptr : &call_stack_.back(), environment_);
                if (!part) {
                    return part.error_value();
                }
                if (!part.value().is_reference()) {
                    return checked_result<void>(make_error_code(runtime_error_code::invalid_reference), detail::cast_non_lvalue_text);
                }
                operand = std::move(part.value());
                break;
            }
            operand = std::move(resolved.value());
            break;
        }
        case detail::cast_operand_mode::value:
            JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->operand.get(), rs, 0), expr, rp_operand, 0, {});
            operand = pop_value();
            if (is_unwinding_) {
                push_value(make_value());
                return {};
            }
            // C::s and ns::x are lvalues: the reference binds their storage (KEEP BYTE-PARALLEL with the vm's exec_cast)
            if (expr->by_reference && expr->operand->get_type() == node_type::member_expr && static_cast<const member_expr*>(expr->operand.get())->is_static) {
                if (auto cell = detail::static_member_lvalue(*engine_, static_cast<const member_expr*>(expr->operand.get()), *environment_,
                                                             *string_symbolizer_, environment_->find_access_context())) {
                    if (!*cell) { return cell->error_value(); }
                    script_value& storage = *cell->value();
                    operand = share_boxed_slot(storage);
                }
            }
            break;
    }

    if (expr->implicit_ref) {
        if (expr->binding_ref && !operand.is_reference()) {   // the arm's storage when it names one, else its value (KEEP BYTE-PARALLEL with the vm's exec_cast)
            if (auto storage = named_storage_reference(expr->operand.get()); storage && *storage) { operand = std::move(storage->value()); }
            if (!operand.is_reference()) {
                push_value(std::move(operand));
                return {};
            }
        }
        if (!operand.is_reference()) {
            return non_lvalue_target(expr->operand.get());
        }
        push_value(std::move(operand));
        return {};
    }
    if (expr->by_reference) {
        auto out = detail::cast_reference(detail::cast_pin_temporary(std::move(operand), engine_), *expr, engine_, string_symbolizer_);
        if (!out) {
            return out.error_value();
        }
        detail::ref_declare(out.value(), expr->target.get(), engine_);
        push_value(std::move(out.value()));
        return {};
    }

    const bool lvalue_source = operand.is_reference() || detail::cast_lvalue_source(expr->operand.get());
    if (operand.is_reference()) {
        script_value derefed = operand.deref();
        operand = std::move(derefed);
    }
    if (!expr->return_of.empty()) [[unlikely]] {   // a parallel body's return: its function's conversion and text (KEEP BYTE-PARALLEL with the vm's exec_cast)
        if (lvalue_source) {
            script_value copy = clone_for_assignment(operand);
            operand = std::move(copy);
        }
        if (!detail::return_conv_identity(detail::classify_return_conv(expr->target.get()), operand)) {
            auto converted = try_convert_for_parameter(operand, expr->target,
                detail::slot_site{detail::slot_kind::return_value, {}, expr->return_of}, detail::uniquely_held(operand) ? &operand : nullptr);
            if (!converted) {
                return converted.error_value();
            }
            if (is_unwinding_) [[unlikely]] {
                push_value(make_value());
                return {};
            }
            operand = std::move(converted.value());
        }
        push_value(std::move(operand));
        return {};
    }
    if (detail::cast_target_is_class(*expr->target)) {
        auto out = detail::cast_class_value(std::move(operand), *expr, lvalue_source, engine_, string_symbolizer_);
        if (!out) {
            return out.error_value();
        }
        push_value(std::move(out.value()));
        return {};
    }
    if (detail::cast_char_rule_applies(operand, *expr->target)) {
        auto out = detail::cast_char_value(operand, *expr, engine_, string_symbolizer_);
        if (!out) {
            return out.error_value();
        }
        script_value converted = std::move(out.value());
        converted.set_type_info(expr->target);
        push_value(std::move(converted));
        return {};
    }
    if (detail::cast_text_rule_applies(operand, *expr->target)) {   // KEEP BYTE-PARALLEL with the vm's exec_cast
        auto out = detail::cast_text_value(operand, *expr->target, engine_);
        if (!out) {
            return out.error_value();
        }
        if (is_unwinding_) [[unlikely]] {
            push_value(make_value());
            return {};
        }
        script_value converted = std::move(out.value());
        converted.set_type_info(expr->target);
        push_value(std::move(converted));
        return {};
    }
    if (lvalue_source) {
        script_value copy = clone_for_assignment(operand);
        operand = std::move(copy);
    }
    auto enforced = enforce_type_compatibility(operand, expr->target, "cast");
    if (!enforced) {
		if (enforced.error() == make_error_code(runtime_error_code::invalid_numeric_operand)) {
			return enforced.error_value();
		}
        return detail::cast_error<void>(operand, *expr, string_symbolizer_);
    }
    script_value out = std::move(enforced.value());
    out.set_type_info(expr->target);
    push_value(std::move(out));
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_throw_expr_t(throw_expr* expr, const replay_point* rs) {
    if (expr->value) {
        // Evaluate the expression to throw
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_operand, 0, {});
        script_value val = pop_value();

        // the message is the value's display text, a class's own to_string() for an instance (engine::text_of, print's rule)
        std::string message;
        if (!detail::plain_text_value(val)) {
            auto text = engine_->text_of(val);
            if (!text) { return text.error_value(); }
            if (is_unwinding_) { push_value(make_value()); return {}; }   // to_string() threw: that throw propagates
            message = std::move(text.value());
        } else {
            message = val.to_string();
        }
        active_exception_value_ = val;
        current_exception_ = script_exception(message, expr->location);
        current_exception_->thrown = true;
    } else {
        // Re-throw current exception; with none active it is a catchable runtime error (KEEP BYTE-PARALLEL with vm_backend::exec_throw)
        if (!current_exception_) {
            return checked_result<void>(make_error_code(runtime_error_code::invalid_operation), detail::bare_throw_text);
        }
        // Keep the existing active_exception_value_
    }

    is_unwinding_ = true;
    // throw is an expression: it yields the unwinding placeholder every operand consumer pops
    push_value(make_value());
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_yield_expr_t(yield_expr* expr, const replay_point* rs) {
    if (!active_coroutine_) {
        return checked_result<void>(make_error_code(runtime_error_code::evaluation_failed),
            "yield used outside of coroutine");
    }

    // Evaluate the yield value (if any)
    script_value value = make_value();
    if (expr->value) {
        JAISCRIPT_TRY_OPERAND(eval_child<Replaying>(expr->value.get(), rs, 0), expr, rp_operand, 0, {});
        value = pop_value();
        if (!is_unwinding_) { detail::materialize_return_value(value, detail::yield_operand_is_lvalue(expr->value.get())); }   // KEEP BYTE-PARALLEL with the vm's op_yield
        // A yield hands out the declared type like a return (the return's table and identity lanes, the yield's text).
        // KEEP BYTE-PARALLEL with the vm's exec_yield_convert.
        if (!is_unwinding_ && !call_stack_.empty()) {
            if (const script_defined_function* fn = call_stack_.back().function) {
                const auto conv = detail::return_conv_of(*fn);
                if (conv != detail::return_conv::none && conv != detail::return_conv::ref) {
                    if (value.is_reference()) { value = value.deref(); }
                    if (!detail::return_conv_identity(conv, value)) {
                        auto converted = try_convert_for_parameter(value, fn->return_type,
                            detail::slot_site{detail::slot_kind::yield_value, {}, fn->name}, detail::uniquely_held(value) ? &value : nullptr);
                        if (!converted) { return converted.error_value(); }
                        if (is_unwinding_) [[unlikely]] { return {}; }
                        value = std::move(converted.value());
                    }
                }
            }
        }
    }

    // Publish the value and suspend HERE: the signal unwinds the enclosing expression (each
    // visitor parks its operands), the owning statement consumes it, and the resume
    // substitutes null for this yield (vm exec_yield parity)
    active_coroutine_->do_yield(std::move(value));
    hasYieldRequest_ = true;
    return checked_result<void>(make_error_code(runtime_error_code::coroutine_suspended));
}

checked_result<void> interpreter::visit_if_stmt(if_stmt* stmt) {
    bool is_true;
    bool resuming = false;
    bool resume_cond = false;

    // Check if we're resuming into a branch from a coroutine yield
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        auto* cont = coro_state.peek_continuation(stmt);
        if (cont) {
            resume_cond = cont->phase == rp_cond;   // suspended in the condition itself
            is_true = (cont->index == 0);  // 0=then branch, 1=else branch
            coro_state.pop_continuation();
            resuming = !resume_cond;
        }
    }

    if (!resuming) {
        // Evaluate the condition
        auto cond_result = resume_cond ? replay_expression(stmt->condition.get())
                                       : dispatch_expr(stmt->condition.get());
        if (!cond_result) [[unlikely]] {
            if (is_yield_signal(cond_result)) { park_operands(stmt, rp_cond, 0, {}); return {}; }
            return cond_result.error_value();
        }
        script_value conditionValue = pop_value();

        // a throw unwinding out of the condition leaves a null placeholder: neither branch runs
        if (is_unwinding_) {
            return {};
        }

        is_true = is_truthy(conditionValue);
    }

    // Execute appropriate branch based on truthiness. On yield the branch owns the frames;
    // a brace-less branch that completed AT the yield left none, and the if is done too.
    statement* branch = is_true ? stmt->then_statement.get() : stmt->else_statement.get();
    if (branch) {
        JAISCRIPT_TRY(resuming ? dispatch_stmt_resuming(branch) : dispatch_stmt(branch));
        if (hasYieldRequest_ && active_coroutine_) {
            auto& coro_state = coroutine_state(*active_coroutine_, engine_);
            if (coro_state.has_continuations()) {
                coro_state.push_continuation(stmt, is_true ? 0 : 1);
            }
        }
    }
    return {};
}

checked_result<void> interpreter::visit_while_stmt(while_stmt* stmt) {
    // Check if we're resuming into this while loop from a coroutine yield
    bool skip_first_condition = false;
    bool replay_condition = false;
    bool resume_body = false;
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        auto* cont = coro_state.peek_continuation(stmt);
        if (cont) {
            replay_condition = cont->phase == rp_cond;   // suspended in the condition itself
            // a brace-less body that completed at the yield: the next step is the condition
            skip_first_condition = cont->phase == rp_none;
            resume_body = skip_first_condition;
            coro_state.pop_continuation();
        }
    }

    while (true) {
        if (execution_limit_exhausted()) [[unlikely]] {
            return execution_limit_failure();
        }

        if (skip_first_condition) {
            skip_first_condition = false;  // Only skip once
        } else {
            note_running_statement(stmt);
            auto cond_result = replay_condition ? replay_expression(stmt->condition.get())
                                                : dispatch_expr(stmt->condition.get());
            replay_condition = false;
            if (!cond_result) [[unlikely]] {
                if (is_yield_signal(cond_result)) { park_operands(stmt, rp_cond, 0, {}); return {}; }
                return cond_result.error_value();
            }

            // a throw unwinding out of the condition leaves a null placeholder: the loop stops, the unwind propagates
            if (is_unwinding_) {
                valueStack_.discard();
                break;
            }

            const bool is_true = is_truthy(valueStack_.top());
            valueStack_.discard();

            if (!is_true) {
                break;
            }
        }

        // Execute the loop body
        auto result = resume_body ? dispatch_stmt_resuming(stmt->body.get()) : dispatch_stmt(stmt->body.get());
        resume_body = false;
        if (!result) return result;

        // A script `throw` sets is_unwinding_ and the body short-circuits
        // without advancing any loop state. Stop the loop so the exception can
        // propagate; otherwise we re-evaluate the condition forever (condition
        // evaluation no-ops while unwinding) and hang.
        if (is_unwinding_) {
            break;
        }

        // Check for control flow changes (break/continue/return)
        if (hasBreakRequest_) {
            hasBreakRequest_ = false;  // Clear the flag
            break;
        }

        if (hasContinueRequest_) {
            hasContinueRequest_ = false;  // Clear the flag
            continue;
        }

        if (hasReturnValue_) {
            break;
        }

        // On yield: record continuation and return
        if (hasYieldRequest_) {
            if (active_coroutine_) {
                auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                if (coro_state.has_continuations()) coro_state.push_continuation(stmt, 0);
                else coro_state.push_replay(stmt, rp_body_done, 0);
            }
            return {};
        }
    }
    return {};
}

void interpreter::kill_scope_slot(const statement* decl) {
    if (!decl || decl->get_type() != node_type::variable_decl) { return; }
    const auto* v = static_cast<const variable_decl*>(decl);
    if (v->slot_index == SIZE_MAX || v->is_static) { return; }
    if (v->type && !v->type->may_own_instance() && !v->owning_reference()) { return; }
    if (script_value* slot = call_stack_.back().get_local(v->slot_index)) {
        if (slot->is_reference() && !parallel_worker_) [[unlikely]] {   // a cell a lambda shared: checked for a cycle once the slot lets go
            detail::shared_cell_release killed{engine_};
            script_value::gather_shared_cells(slot, 1, killed.cells);
            script_value dying = std::move(*slot);
            return;
        }
        script_value dying = std::move(*slot);
    }
}

// The initializer's variables die after the loop (KEEP BYTE-PARALLEL with vm_compiler::compile_for)
checked_result<void> interpreter::visit_for_stmt(for_stmt* stmt) {
    auto result = run_for_stmt(stmt);
    if (!hasYieldRequest_ && stmt->initializer && !call_stack_.empty()) { kill_scope_slot(stmt->initializer.get()); }
    return result;
}

checked_result<void> interpreter::run_for_stmt(for_stmt* stmt) {
    // Check if we're resuming into this for-loop from a coroutine yield
    bool resuming_loop = false;
    uint32_t resume_phase = rp_none;
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        auto* cont = coro_state.peek_continuation(stmt);
        if (cont) {
            resuming_loop = true;
            resume_phase = cont->phase;
            // Restore the for-loop's environment (includes loop variable scope)
            if (cont->saved_env) {
                environment_ = cont->saved_env;
            }
            coro_state.pop_continuation();
            // DON'T create new scope or execute initializer - saved environment has them
        }
    }

    // For normal execution, previous is the current environment.
    // For resume, previous should be the parent of the restored for-loop scope,
    // so we correctly restore the outer scope on exit.
    auto previous = resuming_loop ? environment_->get_parent() : environment_;
    bool owns_scope = false;

    if (!resuming_loop) {
        // Create new scope for the for loop (initialization variables should be scoped)
        auto loop_env = block_environment(environment_);
        environment_ = loop_env;
        owns_scope = true;

        // Execute initialization (if present)
        if (stmt->initializer) {
            if (debug_hook_) [[unlikely]] { debug_quiet_node_ = stmt->initializer.get(); }
            auto result = dispatch_decl(stmt->initializer.get());
            if (!result) {
                end_scope(loop_env);
                environment_ = previous;
                return result;
            }
            if (hasYieldRequest_) [[unlikely]] {
                // the initializer suspended: its own frames are parked, keep the loop scope
                if (active_coroutine_) {
                    coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_init, 0, {}, environment_);
                }
                environment_ = previous;
                return {};
            }
        }
    } else if (resume_phase == rp_init && stmt->initializer) {
        auto result = dispatch_decl_resuming(stmt->initializer.get());
        if (!result) {
            environment_ = previous;
            return result;
        }
        if (hasYieldRequest_) [[unlikely]] {
            if (active_coroutine_) {
                coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_init, 0, {}, environment_);
            }
            environment_ = previous;
            return {};
        }
    }

    // Error capture for lambdas (avoid throwing from hot path)
    std::optional<checked_result<void>> error;
    // A condition/update suspension keeps the loop scope alive for the resume
    bool park_loop_env = false;

    // OPTIMIZATION: Pre-check if condition is guaranteed to return bool
    // This allows us to skip is_truthy() type dispatch entirely

    bool replay_condition = resume_phase == rp_cond;
    bool replay_update = resume_phase == rp_update;
    auto eval_condition = [&]() -> bool {
        if (!stmt->condition) return true;  // No condition = infinite loop

        note_running_statement(stmt);
        auto result = replay_condition ? replay_expression(stmt->condition.get())
                                       : dispatch_expr(stmt->condition.get());
        replay_condition = false;
        if (!result) {
            if (is_yield_signal(result)) {
                if (active_coroutine_) {
                    coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_cond, 0, {}, environment_);
                }
                park_loop_env = true;
                return false;
            }
            error = result;  // Capture error, signal loop termination
            return false;
        }

        // a throw unwinding out of the condition leaves a null placeholder: the loop exits, the unwind propagates
        if (is_unwinding_) {
            valueStack_.discard();
            return false;
        }

        const bool is_true = is_truthy(valueStack_.top());
        valueStack_.discard();
        return is_true;
    };

    auto eval_update = [&]() {
        if (!stmt->update) return;

        struct updating_scope {
            std::vector<std::pair<size_t, const for_stmt*>>& loops;
            ~updating_scope() { loops.pop_back(); }
        } updating{updating_loops_};
        updating_loops_.emplace_back(call_stack_.size(), stmt);
        auto result = replay_update ? replay_expression(stmt->update.get())
                                    : dispatch_expr(stmt->update.get());
        replay_update = false;
        if (!result) {
            if (is_yield_signal(result)) {
                if (active_coroutine_) {
                    coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_update, 0, {}, environment_);
                }
                park_loop_env = true;
                return;
            }
            error = result;  // Capture error, loop will terminate on next condition check
            return;
        }

        // Discard the update result if it leaves a value on the stack (optimization)
        if (!valueStack_.empty()) {
            valueStack_.discard();
        }
    };

    // Resuming inside the update (or after a brace-less body that completed at the yield):
    // finish the update before this iteration's condition
    if (replay_update || resume_phase == rp_body_done) {
        eval_update();
        if (park_loop_env || error) {
            environment_ = previous;
            return error ? *error : checked_result<void>{};
        }
    }

    // Skip first condition if resuming (we were mid-iteration when yield happened)
    bool skip_condition = resuming_loop && resume_phase == rp_none;
    bool resume_body = skip_condition;

    // Native C++ for-loop structure
    while (true) {
        if (execution_limit_exhausted()) [[unlikely]] {
            if (owns_scope) {
                end_scope(environment_);
            }
            environment_ = previous;
            return execution_limit_failure();
        }

        if (skip_condition) {
            skip_condition = false;  // Only skip on first resumed iteration
        } else {
            if (!eval_condition()) break;
        }
        if (error || park_loop_env) break;

        // Execute the loop body
        auto result = resume_body ? dispatch_stmt_resuming(stmt->body.get()) : dispatch_stmt(stmt->body.get());
        resume_body = false;
        if (!result) {
            if (owns_scope) {
                end_scope(environment_);
            }
            environment_ = previous;
            return result;
        }

        // A script `throw` unwinds: stop the loop (otherwise the condition/update
        // no-op while unwinding and we spin forever).
        if (is_unwinding_) {
            break;
        }

        // Check for control flow changes (break/continue/return)
        if (hasBreakRequest_) {
            hasBreakRequest_ = false;
            break;
        }

        if (hasReturnValue_) {
            break;
        }

        // On yield: record continuation and return without releasing environment
        if (hasYieldRequest_) {
            if (active_coroutine_) {
                // Save the current environment (for-loop scope or deeper) so we can
                // restore it on resume. For-loop variables are environment-based
                // (not slot-based) and must be accessible.
                auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                if (coro_state.has_continuations()) coro_state.push_continuation(stmt, 0, environment_);
                else coro_state.push_replay(stmt, rp_body_done, 0, {}, environment_);
            }
            // Restore environment_ to previous so parent scope is correct
            environment_ = previous;
            return {};
        }

        if (hasContinueRequest_) {
            hasContinueRequest_ = false;
        }

        eval_update();
    }

    // Release the loop environment to destroy all loop variables (only if we created it).
    // A parked condition/update keeps it: the resume runs in that same scope.
    if (owns_scope && !park_loop_env) {
        end_scope(environment_);
    }

    // Restore previous environment
    environment_ = previous;

    // If lambda captured an error, propagate it
    if (error) {
        return *error;
    }

    return {};
}

checked_result<void> interpreter::visit_parallel_for_stmt(parallel_for_stmt* stmt) {
    // The containers evaluate in the enclosing scope on this thread, left to right like the
    // declarators; the region kernel owns everything else (admission, element proof, capture
    // provisioning, fork-join).
    std::vector<script_value> containers;
    containers.reserve(stmt->arity());
    JAISCRIPT_TRY(dispatch_expr(stmt->container.get()));
    containers.push_back(pop_value());
    for (const auto& extra : stmt->extra_containers) {
        JAISCRIPT_TRY(dispatch_expr(extra.get()));
        containers.push_back(pop_value());
    }
    const std::function<const script_value*(size_t)> read_frame_slot = [this](size_t slot) -> const script_value* {
        return call_stack_.empty() ? nullptr : call_stack_.back().get_local(slot);
    };
    return detail::run_parallel_for(*engine_, stmt, containers, environment_, read_frame_slot);
}

// A by-value loop variable dies with its iteration: the next element's binding replaces it, the last one dies here (KEEP
// BYTE-PARALLEL with vm_compiler::compile_range_for)
checked_result<void> interpreter::visit_range_for_stmt(range_for_stmt* stmt) {
    if (stmt->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(stmt, stmt->const_check)); }
    return run_range_for_stmt(stmt);
}

checked_result<void> interpreter::run_range_for_stmt(range_for_stmt* stmt) {
    // Coroutine resume replay: continue the SUSPENDED iteration - restoring the
    // evaluated container and position - instead of re-evaluating the container
    // (which would re-mint an inner coroutine or restart the sequence).
    bool replaying = false;          // mid-iteration: the loop variable is already bound
    bool have_container = false;     // the evaluated container rides the frame
    bool replay_container = false;
    size_t resume_index = 0;
    script_value container = make_value();
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        if (auto* cont = coro_state.peek_continuation(stmt)) {
            replay_container = cont->phase == rp_container;   // suspended in the source expression
            if (cont->saved_container) {
                have_container = true;
                // a brace-less body that completed at the yield: bind the next element normally
                replaying = cont->phase != rp_body_done;
                resume_index = cont->index;
                container = *cont->saved_container;
            }
            coro_state.pop_continuation();
        }
    }
    if (!have_container) {
        // Evaluate the container expression
        auto container_result = replay_container ? replay_expression(stmt->container.get())
                                                 : dispatch_expr(stmt->container.get());
        if (!container_result) [[unlikely]] {
            if (is_yield_signal(container_result)) { park_operands(stmt, rp_container, 0, {}); return {}; }
            return container_result.error_value();
        }
        container = pop_value();
        // A script exception here (e.g. a reload-removed method as the container) must
        // unwind the statement with the ORIGINAL exception, not fall into the
        // not-iterable ladder below (which clobbered it with 'Type mismatch')
        if (is_unwinding_) {
            return {};
        }
        // Nested-read sources (for (x : m["a"])) arrive as reference wrappers -
        // iterate the TARGET; the deref'd handle shares the live node so by-ref
        // mutation lands in the original. Temp-copy discipline as everywhere.
        // (KEEP BYTE-PARALLEL with vm_backend::exec_iter_init)
        if (container.is_reference()) [[unlikely]] {
            script_value derefed = container.deref();
            container = std::move(derefed);
        }
    }

    // Determine if we can use slot-based access (inside a function with an assigned slot)
    const bool use_slot = stmt->variable_slot_index != SIZE_MAX && !call_stack_.empty();
    // The last by-value loop variable dies on every exit but a yield, before the range (C++ ends the variable's scope inside the
    // range's): declared after `container`, this guard runs first
    struct loop_variable_release {
        interpreter* self; size_t slot; bool armed;
        ~loop_variable_release() {
            if (!armed || self->hasYieldRequest_ || self->call_stack_.empty()) { return; }
            if (script_value* held = self->call_stack_.back().get_local(slot)) { script_value dying = std::move(*held); }
        }
    } release_loop_variable{this, stmt->variable_slot_index,
                            use_slot && !stmt->is_reference && (!stmt->element_type || stmt->element_type->may_own_instance())};
    // The body can move the frame's locals (a declaration at a new slot, a callee's frame): the loop variable's pointer is
    // re-resolved before each write (a use-after-free write otherwise)
    auto loop_variable = [&]() -> script_value* {
        script_value* result = use_slot ? call_stack_.back().get_local(stmt->variable_slot_index) : environment_->get_value_ptr(stmt->variable_name_id);
        if (!result) [[unlikely]] {
            if (use_slot) {
                call_stack_.back().set_local(stmt->variable_slot_index, make_value());
                result = call_stack_.back().get_local(stmt->variable_slot_index);
            } else {
                environment_->define(stmt->variable_name_id, make_value());
                result = environment_->get_value_ptr(stmt->variable_name_id);
            }
        }
        return result;
    };
    // The loop variable's reference dies with the loop: a slotted by-reference loop clears
    // its slot on every exit but a yield, so no element cursor outlives the loop (a later
    // parallel region over the same array must not see a live reference the program can no
    // longer name). KEEP BYTE-PARALLEL with vm_backend::release_iter_slots
    struct slot_reference_release {
        interpreter* self; size_t slot; bool armed;
        ~slot_reference_release() {
            if (!armed || self->hasYieldRequest_ || self->call_stack_.empty()) { return; }
            if (script_value* cell = self->call_stack_.back().get_local(slot); cell && cell->is_reference()) {
                *cell = self->make_value();
            }
        }
    } release_loop_reference{this, stmt->variable_slot_index, use_slot && (stmt->is_reference || stmt->binds_map_entry)};

    // Create a new scope for the loop variable
    push_scope();

    if (container.is_array()) {
        // Iterate over array
        auto& array_storage = container.get_array_storage();
        const size_t array_size = array_storage->size();

        if (array_size > 0) {
            script_value* loop_var_ptr = nullptr;

            if (use_slot) {
                // Slot-based O(1) access: define the variable in the call frame's local slot
                // (on replay the restored frame already holds the current element - don't clobber)
                if (!replaying || !call_stack_.back().get_local(stmt->variable_slot_index)) {
                    call_stack_.back().set_local(stmt->variable_slot_index, make_value());
                }
                loop_var_ptr = call_stack_.back().get_local(stmt->variable_slot_index);
            } else {
                // OPTIMIZATION: Define loop variable ONCE, then use pointer for direct assignment
                // Use pre-interned symbol ID from parser - no runtime string interning needed
                if (!replaying || !environment_->get_value_ptr(stmt->variable_name_id)) {
                    environment_->define(stmt->variable_name_id, make_value());
                }
                loop_var_ptr = environment_->get_value_ptr(stmt->variable_name_id);
            }

            // Re-check the LIVE size each iteration: the loop body can alias the
            // same array (arrays are shared strong_ptr storage) and shrink it
            // (clear/pop/erase). Trusting the cached array_size would index past
            // the end with unchecked operator[] -> OOB read / crash.
            for (size_t i = resume_index; i < array_storage->size(); ++i) {
                if (execution_limit_exhausted()) [[unlikely]] {
                    pop_scope();
                    return execution_limit_failure();
                }

                const bool resume_body = replaying;
                loop_var_ptr = loop_variable();
                if (replaying) {
                    // First replayed iteration: the loop variable is already restored;
                    // the body dispatch below fast-forwards to the yield point
                    replaying = false;
                } else if (stmt->is_reference) {
                    // Reallocation-safe reference (container+index, not a raw element
                    // pointer): a push in the loop body can reallocate the vector, and a
                    // raw pointer would dangle on write-through.
                    // The ref carries the container's declared element
                    // type — for (auto& x : intArr) x = "s" errors like intArr[i] = "s";
                    // var (any-tagged) containers stay unconstrained.
                    type_info_ptr element_constraint = nullptr;
                    if (auto tag = container.get_type_info(); tag && tag->base_type == script_value_type::jai_array_type) {
                        type_info_ptr et = tag->element_type();
                        if (et && et->base_type != script_value_type::jai_any_type &&
                            et->base_type != script_value_type::jai_null_type) {
                            element_constraint = et;
                        }
                    }
                    // Cursor (frame-slot loops only, like the vm twin - top-level loops keep the
                    // per-iteration mint so destructor timing stays byte-parallel): the previous
                    // iteration's holder, when nothing retained it, moves to slot i
                    if (auto* cursor = use_slot ? loop_var_ptr->element_cursor_holder() : nullptr;
                        cursor && cursor->container.get() == array_storage.get() &&
                        cursor->container_element_type == element_constraint) {
                        cursor->container_index = i;
                    } else {
                        *loop_var_ptr = script_value::make_element_reference(array_storage, i, engine_, element_constraint);
                    }
                    if (stmt->element_type) {   // a T& binds an element holding a T (KEEP BYTE-PARALLEL with vm exec_iter_next)
                        auto bound = detail::range_for_bind(*loop_var_ptr, stmt->element_type, true, stmt->variable_name, engine_);
                        if (!bound) { pop_scope(); return bound.error_value(); }
                    }
                } else {
                    // Copy binding: values deep-copy per iteration, shared_ptr
                    // elements share (for (auto e : registry) e.hurt() mutates)
                    script_value element = array_storage->kind() == script_array::kind_t::obj
                        ? clone_for_assignment(array_storage->get(i, engine_))   // object slots deep-copy like boxed elements
                        : array_storage->is_typed()
                        ? array_storage->get(i, engine_)   // raw buffer read
                        : clone_for_assignment(array_storage->values()[i]);
                    if (stmt->element_type) {   // T x : takes each element as T x = element; would
                        auto bound = detail::range_for_bind(std::move(element), stmt->element_type, false, stmt->variable_name, engine_);
                        if (!bound) { pop_scope(); return bound.error_value(); }
                        element = std::move(bound.value());
                    }
                    *loop_var_ptr = std::move(element);
                }
                if (stmt->binds_map_entry && !resume_body && !detail::map_entry_binding_decomposes(*loop_var_ptr)) [[unlikely]] {
                    checked_result<void> rejected = detail::map_entry_binding_rejection(*loop_var_ptr, detail::map_entry_binding_text(*stmt, *string_symbolizer_), engine_);
                    pop_scope();
                    return rejected;
                }

                // Execute loop body
                auto body_result = resume_body ? dispatch_stmt_resuming(stmt->body.get()) : dispatch_stmt(stmt->body.get());
                if (!body_result) {
                    pop_scope();
                    return body_result;
                }

                // A script `throw` unwinds: stop iterating so it can propagate.
                if (is_unwinding_) {
                    break;
                }

                // Check for control flow changes (break/continue/return) - mirror while/for loops
                if (hasBreakRequest_) {
                    hasBreakRequest_ = false;  // Clear the flag
                    break;
                }

                if (hasContinueRequest_) {
                    hasContinueRequest_ = false;  // Clear the flag
                    continue;  // Skip to next iteration
                }

                if (hasReturnValue_) {
                    break;
                }

                // On yield: don't pop scope, let environment stay for coroutine to save.
                // Record the container and position so the resume replay continues THIS
                // iteration instead of re-evaluating the container from scratch.
                if (hasYieldRequest_) {
                    if (active_coroutine_) {
                        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                        if (coro_state.has_continuations()) coro_state.push_continuation(stmt, i, nullptr, container);
                        else coro_state.push_replay(stmt, rp_body_done, i + 1, {}, nullptr, container);
                    }
                    return {};
                }
            }
        }

    } else if (container.is_string() || container.is_utf8_string()) {
        // Iterate a string by value, one char per element like s[i] (a utf8_string: one character, like u[i]); a char is not a
        // cell, so the by-reference binding raises (KEEP BYTE-PARALLEL with vm_backend::exec_iter_init)
        const bool utf8 = container.is_utf8_string();
        if (stmt->is_reference) {
            pop_scope();
            return checked_result<void>(make_error_code(runtime_error_code::invalid_reference),
                utf8 ? detail::utf8_range_for_by_ref_text : detail::string_range_for_by_ref_text);
        }
        const bool cache = !engine_->parallel_region_active();
        auto text_size = [&]() { return utf8 ? detail::utf8::length(container.unchecked_as_utf8(), cache) : container.unchecked_as_string().size(); };
        if (text_size() != 0) {
            script_value* loop_var_ptr = nullptr;
            if (use_slot) {
                if (!replaying || !call_stack_.back().get_local(stmt->variable_slot_index)) {
                    call_stack_.back().set_local(stmt->variable_slot_index, make_value());
                }
                loop_var_ptr = call_stack_.back().get_local(stmt->variable_slot_index);
            } else {
                if (!replaying || !environment_->get_value_ptr(stmt->variable_name_id)) {
                    environment_->define(stmt->variable_name_id, make_value());
                }
                loop_var_ptr = environment_->get_value_ptr(stmt->variable_name_id);
            }

            // Live size each iteration: the body can alias and shrink the shared string
            for (size_t i = resume_index; i < text_size(); ++i) {
                if (execution_limit_exhausted()) [[unlikely]] {
                    pop_scope();
                    return execution_limit_failure();
                }

                const bool resume_body = replaying;
                loop_var_ptr = loop_variable();
                if (replaying) {
                    replaying = false;
                } else {
                    script_value element = utf8 ? detail::utf8::char_value(container.unchecked_as_utf8(), i, engine_, cache)
                                                : script_value(static_cast<script_char>(container.unchecked_as_string()[i]), engine_);
                    if (stmt->element_type) {   // KEEP BYTE-PARALLEL with vm exec_iter_next
                        auto bound = detail::range_for_bind(std::move(element), stmt->element_type, false, stmt->variable_name, engine_);
                        if (!bound) { pop_scope(); return bound.error_value(); }
                        element = std::move(bound.value());
                    }
                    *loop_var_ptr = std::move(element);
                }
                if (stmt->binds_map_entry && !resume_body && !detail::map_entry_binding_decomposes(*loop_var_ptr)) [[unlikely]] {
                    checked_result<void> rejected = detail::map_entry_binding_rejection(*loop_var_ptr, detail::map_entry_binding_text(*stmt, *string_symbolizer_), engine_);
                    pop_scope();
                    return rejected;
                }

                auto body_result = resume_body ? dispatch_stmt_resuming(stmt->body.get()) : dispatch_stmt(stmt->body.get());
                if (!body_result) {
                    pop_scope();
                    return body_result;
                }

                if (is_unwinding_) {
                    break;
                }
                if (hasBreakRequest_) {
                    hasBreakRequest_ = false;
                    break;
                }
                if (hasContinueRequest_) {
                    hasContinueRequest_ = false;
                    continue;
                }
                if (hasReturnValue_) {
                    break;
                }
                if (hasYieldRequest_) {
                    if (active_coroutine_) {
                        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                        if (coro_state.has_continuations()) coro_state.push_continuation(stmt, i, nullptr, container);
                        else coro_state.push_replay(stmt, rp_body_done, i + 1, {}, nullptr, container);
                    }
                    return {};
                }
            }
        }

    } else if (container.is_map()) {
        // A structured binding's variable holds the entry's cursor, a plain one the entry's pair (KEEP BYTE-PARALLEL with
        // vm_backend::iter_next_map_entry; detail/map_entry_binding.hpp)
        auto& map_storage = container.get_map_storage();
        const type_info* map_tag = container.get_type_info();
        const type_info_ptr map_value_type = map_tag ? map_tag->value_type() : nullptr;

        if (!map_storage->empty()) {
            std::shared_ptr<class_definition> pair_class;
            if (!stmt->binds_map_entry) {
                pair_class = detail::map_entry_pair_class(*engine_);
                if (!pair_class) {
                    pop_scope();
                    return detail::map_entry_pair_missing();
                }
            }

            script_value* loop_var_ptr = nullptr;

            if (use_slot) {
                // Slot-based O(1) access: define the variable in the call frame's local slot
                // (on replay the restored frame already holds the current element - don't clobber)
                if (!replaying || !call_stack_.back().get_local(stmt->variable_slot_index)) {
                    call_stack_.back().set_local(stmt->variable_slot_index, make_value());
                }
                loop_var_ptr = call_stack_.back().get_local(stmt->variable_slot_index);
            } else {
                // OPTIMIZATION: Define loop variable ONCE, then use pointer for direct assignment
                // Use pre-interned symbol ID from parser - no runtime string interning needed
                if (!replaying || !environment_->get_value_ptr(stmt->variable_name_id)) {
                    environment_->define(stmt->variable_name_id, make_value());
                }
                loop_var_ptr = environment_->get_value_ptr(stmt->variable_name_id);
            }

            size_t entry_index = 0;
            for (auto it = map_storage->begin(); it != map_storage->end(); ++it, ++entry_index) {
                if (entry_index < resume_index) {
                    continue;   // fast-forward to the suspended entry
                }
                if (execution_limit_exhausted()) [[unlikely]] {
                    pop_scope();
                    return execution_limit_failure();
                }

                const bool resume_body = replaying;
                loop_var_ptr = loop_variable();
                if (replaying) {
                    // First replayed iteration: the loop variable is already restored;
                    // the body dispatch below fast-forwards to the yield point
                    replaying = false;
                } else if (stmt->binds_map_entry) {
                    const auto& entry = *it;
                    if (!detail::reaim_map_entry_cursor(loop_var_ptr, map_storage, entry.first, map_value_type)) {
                        *loop_var_ptr = script_value::make_map_entry_reference(map_storage, entry.first, &entry.second, engine_, map_value_type);
                    }
                } else {
                    script_value first = make_value();
                    script_value second = make_value();
                    detail::map_entry_pair_halves(map_storage, *it, stmt->is_reference, map_value_type, engine_, first, second);
                    if (!detail::reuse_map_entry_pair(loop_var_ptr, pair_class.get(), first, second)) {
                        *loop_var_ptr = detail::mint_map_entry_pair(pair_class, std::move(first), std::move(second), engine_);
                    }
                }

                // Execute loop body
                auto body_result = resume_body ? dispatch_stmt_resuming(stmt->body.get()) : dispatch_stmt(stmt->body.get());
                if (!body_result) {
                    pop_scope();
                    return body_result;
                }

                // A script `throw` unwinds: stop iterating so it can propagate.
                if (is_unwinding_) {
                    break;
                }

                // Check for control flow changes (break/continue/return) - mirror while/for loops
                if (hasBreakRequest_) {
                    hasBreakRequest_ = false;  // Clear the flag
                    break;
                }

                if (hasContinueRequest_) {
                    hasContinueRequest_ = false;  // Clear the flag
                    continue;  // Skip to next iteration
                }

                if (hasReturnValue_) {
                    break;
                }

                // On yield: don't pop scope, let environment stay for coroutine to save
                if (hasYieldRequest_) {
                    if (active_coroutine_) {
                        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                        if (coro_state.has_continuations()) coro_state.push_continuation(stmt, entry_index, nullptr, container);
                        else coro_state.push_replay(stmt, rp_body_done, entry_index + 1, {}, nullptr, container);
                    }
                    return {};
                }
            }
        }

    } else if (container.is_object()) {
        // Check for coroutine/generator iteration
        auto objHolder = container.get_object_holder();
        if (objHolder && objHolder->type_id() == coroutine_handle_type_id_) {
            auto handle = std::static_pointer_cast<coroutine_handle>(objHolder->host_object());

            script_value* loop_var_ptr = nullptr;
            if (use_slot) {
                if (!replaying || !call_stack_.back().get_local(stmt->variable_slot_index)) {
                    call_stack_.back().set_local(stmt->variable_slot_index, make_value());
                }
                loop_var_ptr = call_stack_.back().get_local(stmt->variable_slot_index);
            } else {
                if (!replaying || !environment_->get_value_ptr(stmt->variable_name_id)) {
                    environment_->define(stmt->variable_name_id, make_value());
                }
                loop_var_ptr = environment_->get_value_ptr(stmt->variable_name_id);
            }

            while (replaying || !handle->done()) {
                const bool resume_body = replaying;
                loop_var_ptr = loop_variable();
                if (replaying) {
                    // First replayed iteration: the current element is already restored;
                    // the body dispatch below fast-forwards to the yield point
                    replaying = false;
                } else {
                    auto resume_result = handle->next(engine_);
                    if (!resume_result) {
                        pop_scope();
                        return resume_result.error_value();
                    }
                    if (handle->done()) break;

                    script_value element = std::move(resume_result.value());
                    if (stmt->element_type) {   // KEEP BYTE-PARALLEL with vm exec_iter_next
                        auto bound = detail::range_for_bind(std::move(element), stmt->element_type, false, stmt->variable_name, engine_);
                        if (!bound) { pop_scope(); return bound.error_value(); }
                        element = std::move(bound.value());
                    }
                    *loop_var_ptr = std::move(element);
                }
                if (stmt->binds_map_entry && !resume_body && !detail::map_entry_binding_decomposes(*loop_var_ptr)) [[unlikely]] {
                    checked_result<void> rejected = detail::map_entry_binding_rejection(*loop_var_ptr, detail::map_entry_binding_text(*stmt, *string_symbolizer_), engine_);
                    pop_scope();
                    return rejected;
                }

                auto body_result = resume_body ? dispatch_stmt_resuming(stmt->body.get()) : dispatch_stmt(stmt->body.get());
                if (!body_result) {
                    pop_scope();
                    return body_result;
                }

                if (is_unwinding_) break;  // script throw unwinding: stop iterating
                if (hasBreakRequest_) { hasBreakRequest_ = false; break; }
                if (hasContinueRequest_) { hasContinueRequest_ = false; continue; }
                if (hasReturnValue_) break;
                if (hasYieldRequest_) {
                    if (active_coroutine_) {
                        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                        if (coro_state.has_continuations()) coro_state.push_continuation(stmt, 0, nullptr, container);
                        else coro_state.push_replay(stmt, rp_body_done, 0, {}, nullptr, container);
                    }
                    return {};
                }
            }
        } else {
            pop_scope();
            return detail::not_iterable(container, engine_);
        }
    } else {
        pop_scope();
        return detail::not_iterable(container, engine_);
    }

    // Pop the loop scope (only on normal exit, not yield)
    pop_scope();
    return {};
}

// The storage a T& producer (a `-> T&` return, a reference parameter's default) binds when its operand names it: a variable
// (its slot or environment cell, boxed on demand, or a hybrid's bare C++ property) or an owner-pinned lvalue chain. nullopt:
// the operand names no storage. KEEP BYTE-PARALLEL with the vm's exec_ref_return_bind / exec_ref_return_lvalue
std::optional<checked_result<script_value>> interpreter::named_storage_reference(expression* value) {
    if (value->get_type() == node_type::identifier_expr) {
        auto* ident = static_cast<identifier_expr*>(value);
        script_value* storage = nullptr;
        if (ident->global_qualified) {
            // ::name - GLOBAL env only (slots, captures, member scope all skipped)
            storage = get_global_environment()->get_value_ptr(ident->symbol_id);
        } else {
            if (ident->slot_index != SIZE_MAX && !call_stack_.empty()) {
                storage = call_stack_.back().get_local(ident->slot_index);
            }
            if (!storage) {
                storage = environment_->get_value_ptr(ident->symbol_id);
            }
        }
        if (storage) { return checked_result<script_value>(share_boxed_slot(*storage)); }
        if (auto minted = bind_bare_cpp_property_ref(ident->symbol_id, ident->global_qualified)) { return minted; }
        if (auto method = bare_method_reference_error(ident)) { return checked_result<script_value>(method->error_value()); }
        return checked_result<script_value>(make_error_code(runtime_error_code::undefined_variable),
            environment_->undefined_text(ident->symbol_id, "Cannot take reference of undefined variable"), ident->symbol_id);
    }
    if (!detail::is_ref_bindable_lvalue(value)) { return std::nullopt; }
    auto resolved = detail::resolve_ref_lvalue(value, detail::caller_frame_view{call_stack_.empty() ? nullptr : &call_stack_.back()},
        environment_.get(), engine_, string_symbolizer_);
    if (resolved) { return std::move(resolved); }
    if (resolved.error() != make_error_code(runtime_error_code::invalid_reference)) { return std::move(resolved); }
    // a math component returns a COMPONENT reference; the kernel's generic verdict leaves the rest to evaluation (an operator[]
    // returning T&, a computed index), as a reference argument binds an evaluated reference
    auto part = component_reference(value, call_stack_.empty() ? nullptr : &call_stack_.back(), environment_);
    if (!part || !part.value().is_reference()) { return std::nullopt; }
    return std::move(part);
}

// C::s and ns::x after their read: the static's storage. nullopt: the member is no static storage
std::optional<checked_result<script_value>> interpreter::static_storage_reference(const member_expr* member) {
    auto cell = detail::static_member_lvalue(*engine_, member, *environment_, *string_symbolizer_, environment_->find_access_context());
    if (!cell) { return std::nullopt; }
    if (!*cell) { return checked_result<script_value>(cell->error_value()); }
    script_value& storage = *cell->value();
    return checked_result<script_value>(share_boxed_slot(storage));
}

template<bool Replaying>
checked_result<void> interpreter::visit_return_stmt_t(return_stmt* stmt) {
    if (stmt->const_check) [[unlikely]] { JAISCRIPT_TRY(const_guard_at(stmt, stmt->const_check)); }
    std::optional<script_value> out;
    if (stmt->value) {
        if (stmt->binds_reference) {
            // Ref-return producer: bind the operand as a reference instead of
            // evaluating a copy. KEEP semantics parallel with the vm's
            // exec_ref_return_bind/exec_ref_return_lvalue.
            if (auto bound = named_storage_reference(stmt->value.get())) {
                if (!*bound) { return bound->error_value(); }
                out.emplace(std::move(bound->value()));
            } else if (stmt->value->get_type() == node_type::member_expr && static_cast<const member_expr*>(stmt->value.get())->is_static) {
                // C::s and ns::x are lvalues: the read first, then its storage replaces it (KEEP BYTE-PARALLEL with the vm's
                // exec_ref_return_lvalue)
                JAISCRIPT_TRY_ROOT(stmt, stmt->value.get());
                out.emplace(pop_value());
                if (!is_unwinding_) {
                    if (auto cell = static_storage_reference(static_cast<const member_expr*>(stmt->value.get()))) {
                        if (!*cell) { return cell->error_value(); }
                        out.emplace(std::move(cell->value()));
                    }
                }
            }
            // Fall through: calls that already yield a reference pass; anything else
            // rejects in the ref-return conversion below
        }
        if (!out) {
            JAISCRIPT_TRY_ROOT(stmt, stmt->value.get());
            out.emplace(pop_value());
        }
    } else {
        out.emplace(make_value());
    }

    if (is_unwinding_) [[unlikely]] {
        // The operand threw: the return never completes (vm parity - op_return does not
        // run); the flag unwinds the body and a catch resumes past the statement
        return {};
    }

    // By-value return materialization (detail/return_conv.hpp), before the typed
    // conversion so a converted value is never copied twice: the callee's own slot moves
    // out, every other lvalue read (and a reference result) copies, a temporary moves.
    // KEEP BYTE-PARALLEL with the vm's return doors (exec_return_ident / exec_return_convert /
    // return_from_script_frame).
    if (!detail::return_value_plain(*out) && !stmt->binds_reference && !call_stack_.empty()) {
        uint8_t operand_class = stmt->return_operand_class;
        if (operand_class == detail::return_operand_unclassified) {
            operand_class = source_class(stmt->value.get());
            if (operand_class == detail::return_operand_ident && call_stack_.back().function) {
                const auto* ident = static_cast<const identifier_expr*>(stmt->value.get());
                for (const auto& p : call_stack_.back().function->parameters()) {
                    if (!p.is_reference && p.slot_index == ident->slot_index && (!p.type || p.type->may_own_instance())) { operand_class = detail::return_operand_parameter; break; }
                }
            }
            stmt->return_operand_class = operand_class;
        }
        bool moved = false;
        // a returned parameter moves out unless a user copy constructor would run (KEEP BYTE-PARALLEL with the vm's materialize_return)
        if (operand_class == detail::return_operand_ident ||
            (operand_class == detail::return_operand_parameter && !detail::move_runs_copy_constructor(*out))) {
            auto* ident = static_cast<identifier_expr*>(stmt->value.get());
            if (!ident->global_qualified && ident->slot_index != SIZE_MAX) {
                if (script_value* payload = detail::return_own_slot_payload(call_stack_.back().get_local(ident->slot_index), *out)) {
                    *payload = make_value();
                    moved = true;
                }
            }
        }
        if (!moved) {
            const script_defined_function* fn = call_stack_.back().function;
            detail::materialize_return_value(*out, detail::return_operand_copies(operand_class, *out), fn ? fn->return_type.get() : nullptr);
        }
    }

    // The typed-return conversion is part of the statement (C++ semantics): it runs here,
    // inside the callee's try regions, so a catch around the `return` sees its failure.
    // KEEP BYTE-PARALLEL with the vm's convert_return_at_site.
    if (!call_stack_.empty()) {
        if (const script_defined_function* fn = call_stack_.back().function) {
            const auto conv = detail::return_conv_of(*fn);
            if (conv == detail::return_conv::ref) {
                auto passed = detail::ref_return_pass_through(std::move(*out), fn->return_type, string_symbolizer_, engine_);
                if (!passed) {
                    return passed.error_value();
                }
                out = std::move(passed.value());
            } else if (conv != detail::return_conv::none) {
                if (out->is_reference()) {
                    *out = out->deref();
                }
                // A storage-matching primitive is the identity
                if (!detail::return_conv_identity(conv, *out)) {
                    auto converted = try_convert_for_parameter(*out, fn->return_type,
                        detail::slot_site{detail::slot_kind::return_value, {}, fn->name}, detail::uniquely_held(*out) ? &*out : nullptr);
                    if (!converted) {
                        return converted.error_value();
                    }
                    if (is_unwinding_) [[unlikely]] {
                        return {};   // a converting ctor / to_X() threw: the throw wins
                    }
                    out = std::move(converted.value());
                }
            }
        }
    }

    returnValue_ = std::move(*out);
    hasReturnValue_ = true;
    return {};
}

checked_result<void> interpreter::visit_break_stmt(break_stmt* stmt) {
    hasBreakRequest_ = true;
    return {};
}

checked_result<void> interpreter::visit_continue_stmt(continue_stmt* stmt) {
    hasContinueRequest_ = true;
    return {};
}

checked_result<void> interpreter::visit_try_stmt(try_stmt* stmt) {
    // Save exception state
    auto saved_exception = current_exception_;
    auto saved_unwinding = is_unwinding_;
    auto saved_exception_value = active_exception_value_;
    auto saved_catch_var_id = current_catch_var_id_;

    // Coroutine resume inside the CATCH block: re-enter the catch with its exception
    // restored - the try block already ran and must not run again
    bool resuming_catch = false;
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        if (auto* cont = coro_state.peek_continuation(stmt)) {
            if (cont->phase == rp_catch) {
                resuming_catch = true;
                active_exception_value_ = cont->saved_container ? *cont->saved_container : make_value();
                current_exception_ = script_exception(active_exception_value_->to_string(), stmt->location);
            }
            coro_state.pop_continuation();
        }
    }

    // Reset state for try block
    // Don't reset exception state if we're in a catch block (allows re-throw); a try block inside a handler is still
    // inside that handler (C++), so the in-catch marker carries into it and a nested try keeps the handled exception
    if (!resuming_catch) {
        if (current_catch_var_id_ == 0) {
            current_exception_.reset();
            active_exception_value_ = make_value();
        }
        is_unwinding_ = false; trace_captured_ = false;
    }

    // Execute try block (a catch resume skips it: it completed before the suspension). A native exception an instruction of the
    // block raises is this try's exception, as in C++ (KEEP BYTE-PARALLEL with vm_backend::run)
    checked_result<void> try_result;
    if (!resuming_catch) {
        const size_t try_stack_mark = valueStack_.size();
        try {
            try_result = dispatch_stmt(stmt->try_block.get());
        } catch (const std::exception& e) {
            while (valueStack_.size() > try_stack_mark) { (void)valueStack_.pop(); }
            if (auto* se = dynamic_cast<const script_exception*>(&e)) { current_exception_ = *se; }
            else { current_exception_ = script_exception(detail::native_exception_text(e)); }
            active_exception_value_ = make_value(std::string(detail::native_exception_text(e)));
            is_unwinding_ = true;
        }
    }
    if (!resuming_catch && hasYieldRequest_) [[unlikely]] {
        // The try block suspended: its own frames carry the position, this statement adds none
        current_catch_var_id_ = saved_catch_var_id;
        return {};
    }

    // Terminal errors (budget overrun, escalated memory cap) can NEVER be caught from
    // script: restore the catch-var and let the failure keep unwinding to the host
    // execute() boundary (KEEP BYTE-PARALLEL with vm_backend::unwind_to_handler).
    if (limits_->terminal_error && (!try_result || (is_unwinding_ && current_exception_))) [[unlikely]] {
        current_catch_var_id_ = saved_catch_var_id;
        if (!try_result) {
            return try_result;
        }
        return {};
    }

    // Check if an error occurred (either checked_result error or exception unwinding)
    bool caught_error = resuming_catch;

    if (!try_result) {
        // Checked_result error - treat as catchable exception; a destructor that threw while the error left its scope goes to the
        // script error handler (KEEP BYTE-PARALLEL with vm_backend::unwind_to_handler)
        if (is_unwinding_ && current_exception_) { engine_->report_script_error(current_exception_->what(), {}); }
        std::string error_msg = format_error(try_result, *string_symbolizer_);
        active_exception_value_ = make_value(error_msg);
        current_exception_ = script_exception(error_msg);
        caught_error = true;
    } else if (is_unwinding_ && current_exception_) {
        // Old-style exception unwinding (for script throw statements)
        caught_error = true;
    }

    if (caught_error) {
        // the stack at the error, for the caught value (a callee's frame exit captured it already)
        if (!resuming_catch && !trace_captured_) { capture_stack_trace(); }
        // Reset unwinding flag
        is_unwinding_ = false; trace_captured_ = false;

        // In-catch marker of the rethrow protocol (symbolize once here)
        if (auto eng = engine_) {
            current_catch_var_id_ = stmt->catch_var_id != UINT64_MAX ? stmt->catch_var_id : eng->symbolize(stmt->catch_var);
        }

        // Bind the catch variable as a local of the catch body (KEEP BYTE-PARALLEL with
        // vm_backend::unwind_to_handler): frame slot, or a scope env popped on every exit
        // of the block. A catch resume skips the bind - the restored frame holds it.
        bool catch_env_pushed = false;
        std::optional<caught_error_scope> caught_scope;   // the thrown value dies as the handler exits (KEEP BYTE-PARALLEL with the vm's try record)
        if (stmt->catch_var_id != UINT64_MAX && resuming_catch && stmt->catch_slot_index != SIZE_MAX && !call_stack_.empty()) {
            if (const script_value* held = call_stack_.back().get_local(stmt->catch_slot_index)) { caught_scope.emplace(engine_, *held); }
        }
        if (stmt->catch_var_id != UINT64_MAX && !resuming_catch) {
            const bool by_throw = current_exception_ && current_exception_->thrown;
            script_value caught = engine_->make_caught_error(active_exception_value_ ? *active_exception_value_ : make_value(), by_throw,
                                                             by_throw ? &current_exception_->location() : nullptr, std::move(captured_trace_),
                                                             by_throw ? current_exception_->what() : nullptr);
            captured_trace_.clear();
            caught_scope.emplace(engine_, caught);
            if (stmt->catch_slot_index != SIZE_MAX && !call_stack_.empty()) {
                call_stack_.back().set_local(stmt->catch_slot_index, std::move(caught));
            } else {
                push_scope();
                environment_->define(stmt->catch_var_id, std::move(caught));
                catch_env_pushed = true;
            }
        }

        // Execute catch block
        auto catch_result = resuming_catch ? dispatch_stmt_resuming(stmt->catch_block.get())
                                           : dispatch_stmt(stmt->catch_block.get());
        if (catch_env_pushed) {
            pop_scope();
        }
        // The handler is done: its variable lets go of the error, so the scope's end decides the thrown value's death
        if (caught_scope && !hasYieldRequest_ && stmt->catch_slot_index != SIZE_MAX && !call_stack_.empty()) {
            call_stack_.back().set_local(stmt->catch_slot_index, make_value());
        }
        // Propagate errors from catch block
        if (!catch_result) {
            current_catch_var_id_ = saved_catch_var_id;
            return catch_result;
        }

        // The catch suspended: park the caught value so the resume re-enters the catch
        // instead of re-running the try block
        if (hasYieldRequest_) [[unlikely]] {
            if (caught_scope) { caught_scope->dismiss(); }
            if (active_coroutine_) {
                coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_catch, 0, {}, nullptr,
                    active_exception_value_ ? *active_exception_value_ : make_value());
            }
            current_catch_var_id_ = saved_catch_var_id;
            return {};
        }

        // Clear the in-catch marker
        current_catch_var_id_ = 0;

        // A completed handler hands the current exception back to the enclosing handler when this try sits inside one (C++:
        // the handled exception reverts, so a later bare `throw;` rethrows it); otherwise none is current
        // (KEEP BYTE-PARALLEL with vm_backend::exec_catch_end)
        if (!is_unwinding_) {
            if (saved_catch_var_id != 0) {
                current_exception_ = saved_exception;
                active_exception_value_ = saved_exception_value;
            } else {
                current_exception_.reset();
                active_exception_value_ = make_value();
            }
        }
    }

    // If still unwinding after catch, we need to be careful about state restoration
    // Don't restore if a new exception was thrown in the catch block
    if (is_unwinding_ && saved_unwinding) {
        // We were already unwinding before this try/catch, restore that state
        current_exception_ = saved_exception;
        active_exception_value_ = saved_exception_value;
    }
    // If is_unwinding_ is true but saved_unwinding was false,
    // it means a new exception was thrown in the catch block - keep it

    // Always restore the catch variable state
    current_catch_var_id_ = saved_catch_var_id;
    return {};
}

checked_result<void> interpreter::visit_switch_stmt(switch_stmt* stmt) {
    // Coroutine resume: re-enter the matched body (or the suspended condition / case value)
    // instead of re-evaluating the condition and the case values (each runs once)
    uint32_t resume_phase = rp_none;
    size_t resume_case = 0;
    script_value switch_value;
    std::shared_ptr<environment> resumed_case_env;
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        if (auto* cont = coro_state.peek_continuation(stmt)) {
            resume_phase = cont->phase;
            resume_case = cont->index;
            if (!cont->partial.empty()) switch_value = cont->partial[0];
            resumed_case_env = cont->saved_env;
            coro_state.pop_continuation();
        }
    }

    // Evaluate the switch condition
    if (resume_phase == rp_none || resume_phase == rp_cond) {
        auto cond_result = resume_phase == rp_cond ? replay_expression(stmt->condition.get())
                                                   : dispatch_expr(stmt->condition.get());
        if (!cond_result) [[unlikely]] {
            if (is_yield_signal(cond_result)) { park_operands(stmt, rp_cond, 0, {}); return {}; }
            return cond_result.error_value();
        }
        switch_value = pop_value();
        resume_phase = rp_none;
    }

    // Save and set switch state
    bool old_in_switch = in_switch_;
    bool old_should_fallthrough = should_fallthrough_;
    in_switch_ = true;
    should_fallthrough_ = false;

    {
        bool matched = false;
        bool executed_case = false;

        // Check each case. A resume into a case body enters it directly: neither the
        // condition nor any case value is evaluated a second time.
        const bool resuming_body = resume_phase == rp_case_body;
        const size_t case_start = (resume_phase == rp_default_body) ? stmt->cases.size()
                                : (resume_phase == rp_none) ? 0 : resume_case;
        for (size_t case_idx = case_start; case_idx < stmt->cases.size(); ++case_idx) {
            const auto& case_stmt = stmt->cases[case_idx];
            bool case_matches = false;
            if (resuming_body && case_idx == resume_case) {
                case_matches = true;   // the suspended case: straight back into its body
            } else {
                // Evaluate case value
                auto result = (resume_phase == rp_case_value && case_idx == resume_case)
                    ? replay_expression(case_stmt->value.get())
                    : dispatch_expr(case_stmt->value.get());
                if (!result) {
                    in_switch_ = old_in_switch;
                    should_fallthrough_ = old_should_fallthrough;
                    if (is_yield_signal(result)) {
                        park_operands(stmt, rp_case_value, case_idx, {switch_value});
                        return {};
                    }
                    return result;
                }
                script_value case_value = pop_value();

                // A case matches when the language's == holds (KEEP BYTE-PARALLEL with vm_backend::exec_case_eq)
                auto equal = language_equal(switch_value, case_value);
                if (!equal) {
                    in_switch_ = old_in_switch;
                    should_fallthrough_ = old_should_fallthrough;
                    return equal.error_value();
                }
                case_matches = equal.value();
            }

            // Execute case if it matches OR if we're falling through from a previous case
            if (case_matches || (executed_case && should_fallthrough_)) {
                matched = true;
                executed_case = true;

                // Reset fallthrough flag for this case (will be set again if case contains fallthrough statement)
                should_fallthrough_ = false;

                // Create a new scope for the case body (like an if statement); a resume
                // re-enters the scope its suspension parked
                auto previous = environment_;
                auto case_env = (resuming_body && case_idx == resume_case && resumed_case_env)
                    ? resumed_case_env : block_environment(environment_);
                environment_ = case_env;

                try {
                    // Execute case body
                    auto case_result = dispatch_stmt(case_stmt.get());
                    if (!case_result) {
                        end_scope(case_env);
                        environment_ = previous;
                        in_switch_ = old_in_switch;
                        should_fallthrough_ = old_should_fallthrough;
                        return case_result;
                    }

                    // Release the case scope (runs case-local destructors) and restore.
                    // On yield the scope must survive for coroutine resume — skip the release.
                    if (hasYieldRequest_) {
                        if (active_coroutine_) {
                            coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_case_body, case_idx, {switch_value}, case_env);
                        }
                        environment_ = previous;
                        in_switch_ = old_in_switch;
                        should_fallthrough_ = old_should_fallthrough;
                        return {};
                    }
                    end_scope(case_env);
                    environment_ = previous;

                    // An explicit `break;` inside a case terminates the switch
                    // (break-by-default already does this), so consume the request
                    // here — it must NOT leak to an enclosing loop. A `continue;`
                    // (and return/unwind) is left set so it propagates outward.
                    if (hasBreakRequest_) {
                        hasBreakRequest_ = false;
                        break;  // Terminate the switch
                    }

                    // Propagate continue/return/unwind out of the switch immediately.
                    if (hasContinueRequest_ || hasReturnValue_ || is_unwinding_) {
                        break;
                    }

                    // Check if we should continue to next case (implicit break by default)
                    if (!should_fallthrough_) {
                        break;  // Stop executing further cases
                    }
                    // If should_fallthrough_ is true, continue to next iteration
                } catch (...) {
                    // Release and restore environment before re-throwing
                    end_scope(case_env);
                    environment_ = previous;
                    throw;
                }
            }
        }

        // Execute default if no case matched OR if we're falling through from the last case
        if ((!matched || (executed_case && should_fallthrough_)) && stmt->default_case) {
            // Create a new scope for the default body
            auto previous = environment_;
            auto default_env = (resume_phase == rp_default_body && resumed_case_env)
                ? resumed_case_env : block_environment(environment_);
            environment_ = default_env;

            try {
                auto default_result = dispatch_stmt(stmt->default_case.get());
                if (!default_result) {
                    end_scope(default_env);
                    environment_ = previous;
                    in_switch_ = old_in_switch;
                    should_fallthrough_ = old_should_fallthrough;
                    return default_result;
                }

                // Release the default scope (runs its destructors) and restore.
                // On yield the scope must survive for coroutine resume — skip the release.
                if (hasYieldRequest_) {
                    if (active_coroutine_) {
                        coroutine_state(*active_coroutine_, engine_).push_replay(stmt, rp_default_body, 0, {switch_value}, default_env);
                    }
                    environment_ = previous;
                    in_switch_ = old_in_switch;
                    should_fallthrough_ = old_should_fallthrough;
                    return {};
                }
                end_scope(default_env);
                environment_ = previous;

                // Consume an explicit `break;` in default so it does not leak to
                // an enclosing loop (continue/return/unwind still propagate).
                if (hasBreakRequest_) {
                    hasBreakRequest_ = false;
                }
            } catch (...) {
                // Release and restore environment before re-throwing
                end_scope(default_env);
                environment_ = previous;
                throw;
            }
        }
    }

    // Restore switch state
    in_switch_ = old_in_switch;
    should_fallthrough_ = old_should_fallthrough;
    return {};
}

// Case/default bodies: statement lists in the scope visit_switch_stmt created; a yield
// records the statement index exactly like a block (re-enter when an inner frame exists,
// skip a completed statement otherwise)
checked_result<void> interpreter::run_case_body(ast_node* self, const std::vector<statement_ptr>& body) {
    size_t start_index = 0;
    bool resume_entry = false;
    if (active_coroutine_) {
        auto& coro_state = coroutine_state(*active_coroutine_, engine_);
        if (auto* cont = coro_state.peek_continuation(self)) {
            start_index = cont->index;
            coro_state.pop_continuation();
            resume_entry = true;
        }
    }
    for (size_t i = start_index; i < body.size(); ++i) {
        auto result = resume_entry ? dispatch_stmt_resuming(body[i].get()) : dispatch_stmt(body[i].get());
        resume_entry = false;
        if (!result) return result;

        // Stop the case body on return, exception unwind, OR a break/continue request:
        // break terminates the switch, continue targets an enclosing loop; either way
        // the rest of the case body must not run.
        if (hasReturnValue_ || is_unwinding_ || hasBreakRequest_ || hasContinueRequest_) {
            break;
        }
        if (hasYieldRequest_) {
            if (active_coroutine_) {
                auto& coro_state = coroutine_state(*active_coroutine_, engine_);
                coro_state.push_continuation(self, coro_state.has_continuations() ? i : i + 1);
            }
            return {};
        }
    }
    return {};
}

checked_result<void> interpreter::visit_case_stmt(case_stmt* stmt) {
    auto result = run_case_body(stmt, stmt->body);
    if (!hasYieldRequest_) { kill_scope_slots(stmt->body); }   // a case body is a scope (KEEP BYTE-PARALLEL with vm_compiler::compile_switch)
    return result;
}

checked_result<void> interpreter::visit_default_stmt(default_stmt* stmt) {
    auto result = run_case_body(stmt, stmt->body);
    if (!hasYieldRequest_) { kill_scope_slots(stmt->body); }
    return result;
}

checked_result<void> interpreter::visit_fallthrough_stmt(fallthrough_stmt* stmt) {
    // Set flag to continue to next case
    should_fallthrough_ = true;
    return {};
}

checked_result<void> interpreter::visit_function_decl(function_decl* decl) {
    if (decl->is_coroutine) {
        // Coroutine function - calling it returns a coroutine_handle instead of executing.
        // The shared function_decl copy the handle pins is identical on every execution
        // of this declaration - mint it once per node.
        auto& func_decl_ptr = decl->coroutine_mint_cache;
        const unit_stamp decl_unit = current_unit();
        if (!func_decl_ptr || func_decl_ptr->unit.id != decl_unit.id) {
            func_decl_ptr = std::make_shared<function_decl>(decl->location, decl->name, decl->name_id);
            func_decl_ptr->parameters = decl->parameters;
            func_decl_ptr->return_type = decl->return_type;
            func_decl_ptr->body = decl->body;
            func_decl_ptr->is_coroutine = true;
            func_decl_ptr->local_count = decl->local_count;
        }

        auto closure_env = environment_;

        // Nested (function-local) coroutine: the resumed body runs on its own call
        // frame, so enclosing-frame slot locals are invisible to it. Snapshot them by
        // value into a capture env, lambda-style: the first declaration builds the plan
        // and patches those body identifiers to env lookups; later declarations replay it.
        if (!call_stack_.empty()) {
            if (!decl->outer_slot_plan_built) {
                std::unordered_set<uint64_t> declared;
                for (const auto& p : decl->parameters) {
                    if (p.symbol_id == UINT64_MAX) {
                        p.symbol_id = string_symbolizer_->intern(p.name);
                    }
                    declared.insert(p.symbol_id);
                }
                {
                    detail::body_walker walker;
                    walker.symbolizer = string_symbolizer_;
                    walker.on_declared = [&declared](uint64_t id) { declared.insert(id); };
                    walker.stmt(decl->body);
                }
                std::vector<identifier_expr*> to_patch;
                std::unordered_set<uint64_t> seen;
                detail::body_walker walker;
                walker.symbolizer = string_symbolizer_;
                walker.on_identifier = [&](identifier_expr* ident) {
                    if (ident->slot_index == SIZE_MAX) return;
                    if (declared.count(ident->symbol_id)) return;
                    if (seen.insert(ident->symbol_id).second) {
                        decl->outer_slot_plan.emplace_back(ident->symbol_id, ident->slot_index);
                    }
                    to_patch.push_back(ident);
                };
                walker.stmt(decl->body);
                for (auto* ident : to_patch) {
                    ident->slot_index = SIZE_MAX;
                }
                decl->outer_slot_plan_built = true;
            }
            if (!decl->outer_slot_plan.empty()) {
                const size_t outer_slot_count = call_stack_.back().local_count();
                std::shared_ptr<environment> captureEnv;
                for (const auto& [sym, slot] : decl->outer_slot_plan) {
                    if (slot < outer_slot_count) {
                        if (script_value* slot_val = call_stack_.back().get_local(slot)) {
                            if (!captureEnv) {
                                // parented on the globals, as a lambda's capture environment is
                                captureEnv = std::make_shared<environment>(get_global_environment(), string_symbolizer_);
                            }
                            captureEnv->define(sym, detail::clone_for_capture(slot_val->deref()));
                        }
                    }
                }
                if (captureEnv) {
                    closure_env = captureEnv;
                }
            }
        }
        engine* eng = engine_;
        func_decl_ptr->unit = decl_unit;
        uint64_t coroutine_type_id = coroutine_handle_type_id_;
        script_value functionValue = script_value::make_function(
            [eng, coroutine_type_id, func_decl_ptr, closure_env](const std::vector<script_value>& args) -> checked_result<script_value> {
                // Create a new coroutine handle; next() resolves the live backend itself
                auto handle = std::make_shared<coroutine_handle>(eng);
                handle->set_function(func_decl_ptr, args, closure_env);

                // Wrap as a script object
                return make_coroutine_object(eng, coroutine_type_id, handle);
            }, engine_);

        environment_->define(decl->name_id, functionValue);
        return {};
    }

    // Don't capture any environment in the closure - just use nullptr (which also makes
    // the mint node-cacheable: every execution of this declaration under one unit is identical;
    // another unit executing the node, an includer of the same file, mints its own)
    auto& scriptFunc = decl->mint_cache;
    const unit_stamp decl_unit = current_unit();
    if (!scriptFunc || scriptFunc->unit.id != decl_unit.id) {
        scriptFunc = std::make_shared<script_defined_function>(
            decl->name,
            shared_parameters_for(decl),
            decl->return_type,
            decl->body,
            nullptr,  // No closure needed - environment stack handles everything
            decl->local_count  // Slot count for stack allocation
        );
        detail::stamp_local_alias_homes(*scriptFunc);
    }
    scriptFunc->unit = decl_unit;
    scriptFunc->block_defined = environment_->block_scope();
    scriptFunc->declared = true;
    scriptFunc->line = decl->location.line;
    scriptFunc->context = engine_->host_entry_generation();

    // Create wrapper function resolving the live backend at call time. Named thunk so
    // callers can recover the payload via target<script_callable_thunk>() - vm parity.
    scriptFunc->alias_candidate = true;
    script_callable payload;
    payload.kind = script_callable::kind_type::function;
    payload.fn = scriptFunc;
    script_value functionValue = script_value::make_function(
        script_function(script_callable_thunk{engine_, std::move(payload)}), engine_);
    engine_->bump_definition_epoch();

    // Define the function in current environment: alone, or in the overload set of its name (KEEP BYTE-PARALLEL with
    // vm_backend::exec_func_decl)
    auto declared = detail::declare_function_overload(*environment_, decl->name_id, scriptFunc, std::move(functionValue), engine_, decl->is_override);
    if (!declared) { return declared.error_value(); }
    environment_->define(decl->name_id, std::move(declared.value()));
    return {};
}

checked_result<void> interpreter::visit_class_decl(class_decl* decl) {
    return detail::declare_script_class(decl, detail::class_declaration_host{
        engine_, string_symbolizer_, environment_, class_definition_type_id_, class_lookup_callback_, current_unit(),
        [this](const expression_ptr& e, const std::shared_ptr<environment>& env) -> checked_result<script_value> {
            struct env_restore {
                interpreter& self;
                std::shared_ptr<environment> previous;
                ~env_restore() { self.environment_ = std::move(previous); }
            } restore{*this, environment_};
            if (env) { environment_ = env; }
            auto evaluated = dispatch_expr(e.get());
            if (!evaluated) {
                return evaluated.error_value();
            }
            return pop_value();
        },
        [this] {
            if (is_unwinding_) {
                is_unwinding_ = false;
                trace_captured_ = false;
                current_exception_.reset();
                active_exception_value_.reset();
            }
        }});
}

checked_result<void> interpreter::visit_namespace_decl(namespace_decl* decl) {
    // Namespaces are FLAT - "my::nested::deep" is a single namespace name
    // Members are stored in a registry and accessed via qualified names (ns::member)

    // Intern the namespace name if not already done
    if (decl->name_id == UINT64_MAX) {
        decl->name_id = string_symbolizer_->intern(decl->name);
    }

    // Engine-owned namespaces cannot be supplied or reopened by script.
    if (is_reserved_namespace_name(decl->name)) {
        return reserved_namespace_error(decl->name_id);
    }

    // Get or create namespace data
    if (!engine_) {
        return checked_result<void>(make_error_code(runtime_error_code::engine_destroyed));
    }
    auto& ns_data = engine_->script_namespaces()[decl->name_id];
    if (!ns_data) {
        ns_data = std::make_shared<script_namespace_data>();
    }
    detail::namespace_scope(*engine_, *ns_data, decl->name_id);

    // Process all declarations within the namespace
    // Functions, variables, and classes are stored in the namespace_data registry
    for (const auto& member_decl : decl->declarations) {
        // Check what kind of declaration this is
        if (member_decl->get_type() == node_type::function_decl) {
            auto* func_decl = static_cast<function_decl*>(member_decl.get());
            // Intern the function name if not already done
            if (func_decl->name_id == UINT64_MAX) {
                func_decl->name_id = string_symbolizer_->intern(func_decl->name);
            }

            // Overloads of the name coexist; one of the same signature collides unless it overrides (KEEP BYTE-PARALLEL with the vm)
            func_decl->unit = current_unit();
            func_decl->context = engine_->host_entry_generation();
            auto& overloads = ns_data->functions[func_decl->name_id];
            std::string admission_error;
            const auto admission = detail::admit_namespace_function(overloads, *func_decl, engine_->unit_rerun(func_decl->unit), admission_error);
            if (admission == detail::namespace_admission::collision) {
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                    "Function '{0}' already exists in namespace '{1}'. Use 'override' keyword to replace it.",
                    func_decl->name_id, decl->name_id);
            }
            if (admission == detail::namespace_admission::reference_only) {
                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "{0}", string_symbolizer_->intern(admission_error));
            }

            // Check if this namespace name matches a class name
            // If so, check for collision with class static methods
            if (!func_decl->is_override) {
                auto [class_var_id, class_var_name] = string_symbolizer_->get_class_var_id_with_view(decl->name_id);
                auto class_result = environment_->get(class_var_id);
                if (class_result && class_result.value().is_object()) {
                    script_value class_var = std::move(class_result.value());
                    {
                        auto obj_holder = class_var.get_object_holder();
                        if (obj_holder && obj_holder->type_id() == class_definition_type_id_) {
                            auto class_def = std::static_pointer_cast<class_definition>(obj_holder->host_object());

                            // Arity-aware collision check
                            if (class_def->has_static_method_with_arity(func_decl->name_id, func_decl->parameters.size())) {
                                return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                                    "Function '{0}' in namespace '{1}' collides with static method. Use 'override' to override.",
                                    func_decl->name_id, decl->name_id);
                            }
                        }
                    }
                }
            }

            overloads.emplace_back(std::make_shared<function_decl>(*func_decl));
            detail::publish_namespace_function(*engine_, *ns_data, decl->name_id, func_decl->name_id);

        } else if (member_decl->get_type() == node_type::variable_decl) {
            auto* var_decl = static_cast<variable_decl*>(member_decl.get());
            // Intern the variable name if not already done
            if (var_decl->name_id == UINT64_MAX) {
                var_decl->name_id = string_symbolizer_->intern(var_decl->name);
            }

            // The declaration runs in the namespace's scope (visit_variable_decl's every path): its cell lives there
            // and its initializer sees the members declared before it. KEEP BYTE-PARALLEL with vm_backend::exec_namespace_variable.
            struct scope_swap { std::shared_ptr<environment>& slot; std::shared_ptr<environment> saved; ~scope_swap() { slot = std::move(saved); } } swap{environment_, environment_};
            environment_ = detail::namespace_scope(*engine_, *ns_data, decl->name_id);
            if (!var_decl->is_inline && !environment_->plain_declare(var_decl->name_id)) [[unlikely]] {
                return detail::inline_redefinition(*string_symbolizer_, var_decl->name_id, var_decl);
            }
            if (debug_hook_ && debug_hook_->wants_statement(static_cast<uint32_t>(var_decl->location.line))) [[unlikely]] {
                debug_hook_->on_statement(var_decl, static_cast<int>(call_stack_.size()));
            }
            JAISCRIPT_TRY(visit_variable_decl(var_decl));
            if (is_unwinding_) {
                return {};
            }

        } else if (member_decl->get_type() == node_type::class_decl) {
            auto* class_decl_ptr = static_cast<class_decl*>(member_decl.get());
            // Intern the class name if not already done
            if (class_decl_ptr->name_id == UINT64_MAX) {
                class_decl_ptr->name_id = string_symbolizer_->intern(class_decl_ptr->name);
            }

            {   // its methods, constructors and initializers read the namespace's members by their bare names (KEEP BYTE-PARALLEL with the vm)
                struct scope_swap { std::shared_ptr<environment>& slot; std::shared_ptr<environment> saved; ~scope_swap() { slot = std::move(saved); } } swap{environment_, environment_};
                environment_ = detail::namespace_class_environment(*engine_, decl->name_id);
                JAISCRIPT_TRY(dispatch_decl(class_decl_ptr));
            }
            detail::register_namespace_class(*engine_, *ns_data, *class_decl_ptr);

        } else {
            // Other declaration types (expressions, includes, etc.) - execute normally
            JAISCRIPT_TRY(dispatch_decl(member_decl.get()));
        }
    }

    return {};
}

// The block-level statement form (the parser wraps every `expr;` inside a block as an
// expression_decl): the value stays on the stack for a top-level implicit return
template<bool Replaying>
checked_result<void> interpreter::visit_expression_decl_t(expression_decl* decl) {
    // A statement's value is not consumed: no read-back for an assignment expression (the
    // top-level implicit return hands the host the right-hand side, like the vm)
    struct restore_root { const expression*& root; const expression* saved; ~restore_root() { root = saved; } } restore{ unconsumed_root_, unconsumed_root_ };
    unconsumed_root_ = decl->expression.get();
    JAISCRIPT_TRY_ROOT(decl, decl->expression.get());
    return {};
}

// One include body for BOTH positions: statement (include_decl, value discarded by the
// caller) and expression (include_expr, value IS the expression) — identical path
// resolution, execution, and errors by construction.
checked_result<void> interpreter::include_into_value_stack(expression* path_expr_node, const std::string& literal_path) {
    // Get the engine reference
    auto engine_ptr = engine_;
    if (!engine_ptr) {
        // [ErrorText] Engine reference expired during include processing
        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
    }

    // Get the path either from literal or expression
    std::string path;
    if (path_expr_node) {
        // Evaluate the expression to get the path
        JAISCRIPT_TRY(dispatch_expr(path_expr_node));
        script_value path_value = pop_value();

        // Convert to string
        if (path_value.type() != script_value_type::jai_string_type) {
            // [ErrorText] Include path expression must evaluate to a string
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
        }
        path = path_value.as<std::string>();
    } else {
        // Use the literal path
        path = literal_path;
    }

    // Resolve the file path
    auto resolve_result = resolve_include_path(path, engine_ptr);
    if (!resolve_result) {
        return resolve_result.error_value();
    }
    std::string resolved_path = std::move(resolve_result.value());

    std::string content;
    if (!engine_ptr->read_include_file(resolved_path, content)) {   // KEEP BYTE-PARALLEL with the vm
        return checked_result<void>(make_error_code(runtime_error_code::file_not_found), "Failed to open include file: {0}",
                                    string_symbolizer_->intern(resolved_path));
    }

    // Execute the included file. Pass the resolved path so every node in the included
    // unit is stamped with its real filename (stack traces / breakpoints name the file).
    std::string scope;
    if (!call_stack_.empty()) {
        const std::string_view fn = call_stack_.back().function_name;
        scope = fn.empty() || fn.front() == '<' ? std::string("a lambda") : "function '" + std::string(fn) + "'";
    } else if (environment_ != get_global_environment()) {
        scope = "a block";
    }
    nested_failure_.parked = false;
    try {
        push_value(engine_ptr->execute_include_source(resolved_path, content, scope));
    } catch (const std::exception& e) {
        return raise_nested_failure(e);
    }
    return {};
}

checked_result<void> interpreter::raise_native_exception(const std::exception& e) {
    active_exception_value_ = make_value(std::string(detail::native_exception_text(e)));
    if (const auto* thrown = dynamic_cast<const script_exception*>(&e)) {
        current_exception_ = *thrown;
    } else {
        current_exception_ = script_exception(detail::native_exception_text(e));
    }
    is_unwinding_ = true;
    push_value(make_value());
    return {};
}

// A failure of a nested run (include/import) re-raised in this run as if the nested code ran here: its runtime error
// itself (caught with its text alone), or its thrown value; a load failure (parse, static check, missing file) raises its
// text. KEEP BYTE-PARALLEL with vm_backend::raise_nested_failure.
checked_result<void> interpreter::raise_nested_failure(const std::exception& e) {
    if (!nested_failure_.parked) {
        active_exception_value_ = make_value(std::string(e.what()));
        current_exception_ = script_exception(e.what());
        is_unwinding_ = true;
        push_value(make_value());
        return {};
    }
    absorb_nested_trace();
    nested_failure_.parked = false;
    if (nested_failure_.error) { return *nested_failure_.error; }
    active_exception_value_ = nested_failure_.value ? std::move(nested_failure_.value) : std::optional<script_value>(make_value());
    current_exception_ = nested_failure_.exception ? std::move(nested_failure_.exception) : std::optional<script_exception>(script_exception(e.what()));
    is_unwinding_ = true;
    push_value(make_value());
    return {};
}

checked_result<void> interpreter::visit_include_decl(include_decl* decl) {
    return include_into_value_stack(decl->path_expr.get(), decl->path);
}

checked_result<void> interpreter::visit_include_expr(include_expr* expr) {
    return include_into_value_stack(expr->path_expr.get(), expr->path);
}

// The import step both forms take (KEEP BYTE-PARALLEL with vm_backend::exec_import): the engine's check pass decides
// whether this reach looks at the disk; the value is whether a module (re)ran (the statement form's is null)
checked_result<void> interpreter::import_step(import_site& site, const std::vector<std::string>* literal, const std::string* single,
                                              const std::vector<expression_ptr>& computed, bool force, bool yields) {
    if (!engine_) {
        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch));
    }
    std::vector<std::string> paths;
    if (!computed.empty()) {
        const size_t n = computed.size();
        for (size_t i = 0; i < n; ++i) {   // every argument evaluates before any is read (the vm's order)
            JAISCRIPT_TRY(dispatch_expr(computed[i].get()));
            if (is_unwinding_) {
                for (size_t k = 0; k <= i; ++k) { valueStack_.discard(); }
                push_value(make_value());
                return {};
            }
        }
        const script_value* args[8];
        if (!force && n <= 8) {
            for (size_t i = 0; i < n; ++i) { args[i] = &valueStack_.peek(n - 1 - i); }
            if (engine_->import_skips(site, args, n)) {
                for (size_t i = 0; i < n; ++i) { valueStack_.discard(); }
                push_value(yields ? make_value(false) : make_value());
                return {};
            }
        }
        checked_result<void> added;
        for (size_t i = 0; i < n && added; ++i) { added = detail::append_import_paths(valueStack_.peek(n - 1 - i), paths, engine_); }
        for (size_t i = 0; i < n; ++i) { valueStack_.discard(); }
        if (!added) { return added; }
    } else {
        if (!force && engine_->import_skips(site)) {
            push_value(yields ? make_value(false) : make_value());
            return {};
        }
        if (literal) { paths = *literal; } else { paths.push_back(*single); }   // built past the skip: an idle reach allocates nothing
    }
    nested_failure_.parked = false;
    try {
        auto ran = engine_->import_modules(site, paths, force);
        if (!ran) { return ran.error_value(); }
        push_value(yields ? make_value(ran.value()) : make_value());
    } catch (const std::exception& e) {
        return raise_nested_failure(e);
    }
    return {};
}

checked_result<void> interpreter::visit_import_decl(import_decl* decl) {
    if (decl->path_expr) {
        return import_step(decl->site, nullptr, nullptr, std::vector<expression_ptr>{decl->path_expr}, false, false);
    }
    return import_step(decl->site, nullptr, &decl->path, {}, false, false);
}

checked_result<void> interpreter::visit_import_expr(import_expr* expr) {
    return import_step(expr->site, &expr->paths, nullptr, expr->path_exprs, expr->force, true);
}

// Execute a method AST with a given environment
checked_result<script_value> interpreter::execute_method_ast(std::shared_ptr<function_decl> ast,
                                           std::shared_ptr<environment> method_env,
                                           const std::vector<script_value>& args) {
    // Coroutine methods mint a handle instead of executing (free-coroutine parity).
    // The handle pins its own method env carrying 'this' (the caller ends method_env when this returns) plus the receiver, so
    // the instance stays alive while suspended and 'this' resolves identically across resumes (KEEP BYTE-PARALLEL with
    // vm_backend::execute_method_ast)
    if (ast->is_coroutine) {
        const uint64_t this_id = string_symbolizer_->get_this_id();
        script_value this_obj(std::monostate{}, engine_);
        if (auto this_result = method_env->get(this_id)) {
            this_obj = std::move(this_result.value());
        }
        auto static_class = method_env->get_class_definition();
        auto coro_env = static_class
            ? std::make_shared<environment>(method_env->get_parent(), string_symbolizer_, static_class)
            : std::make_shared<environment>(method_env->get_parent(), string_symbolizer_, this_obj);
        coro_env->set_access_context(method_env->get_access_context());
        coro_env->define(this_id, this_obj);
        auto handle = std::make_shared<coroutine_handle>(engine_);
        handle->set_function(ast, args, coro_env);
        handle->set_receiver(std::move(this_obj));
        return make_coroutine_object(engine_, coroutine_handle_type_id_, handle);
    }

    // Create a script_defined_function with the method environment (parameter storage
    // shared from the node - no per-call vector copy)
    script_defined_function script_func(
        ast->name,
        shared_parameters_for(ast.get()),
        ast->return_type,
        ast->body,
        method_env,  // Method environment with 'this'
        ast->local_count
    );
    script_func.unit = ast->unit;

    // Execute method with the arguments - propagate errors to caller
    return call_function(script_func, args);
}

// Evaluate field initializers for a script class instance at construction time
// If skip_parent_recursion is true, only evaluate THIS class's field initializers (parents already handled)
checked_result<void> interpreter::evaluate_field_initializers(strong_ptr<class_instance> instance,
                                             std::shared_ptr<script_class_definition> class_def,
                                             std::shared_ptr<environment> init_env,
                                             bool skip_parent_recursion,
                                             const detail::aggregate_plan* aggregate) {
    const unit_stamp class_unit = class_def->unit();   // the initializers are the class's code (no frame of their own)
    unit_override_scope unitScope(this, &class_unit);
    // First, evaluate parent class field initializers (if any)
    // Support multiple inheritance by iterating over all parent classes
    // Skip this if parents were already processed (e.g., by multi-level super() chain handling)
    if (!skip_parent_recursion) {
        for (const auto& parent : class_def->get_parent_classes()) {
            auto parent_script_class = std::dynamic_pointer_cast<script_class_definition>(parent);
            if (parent_script_class) {
                // Recursively evaluate parent field initializers
                auto initialized = evaluate_field_initializers(instance, parent_script_class, init_env, false, aggregate);
                if (!initialized) { return initialized.error_value(); }
            }
        }
    }

    // Now evaluate this class's field initializers
    const auto& field_initializers = class_def->get_field_initializer_asts();

    // Temporarily switch to the init environment for evaluation. Field initializers
    // run in the new instance's member scope (no call frame is pushed here), so the
    // shadowing ladder needs an explicit stamp - earlier fields and members of the
    // class under construction outrank same-named globals, exactly like ctor bodies.
    struct initializer_scope_restore {
        interpreter* self;
        std::shared_ptr<environment> previous_env;
        detail::member_scope_ctx previous_scope;
        ~initializer_scope_restore() {
            self->environment_ = std::move(previous_env);
            self->member_scope_ = previous_scope;
        }
    } scope_restore{this, environment_, member_scope_};
    environment_ = init_env;
    member_scope_ = {};
    script_value* init_this = init_env->method_this_storage();
    if (!init_this) { init_this = init_env->get_own_value_ptr(string_symbolizer_->get_this_id()); }   // a construction scope's `this`
    if (init_this) {
        member_scope_.state = detail::member_scope_state::instance;
        member_scope_.this_storage = init_this;
        member_scope_.statics = detail::lexical_access_class(init_env.get());
        member_scope_.floor = init_env->parent_raw();
    }

    detail::dispatch_level_scope level_dispatch(*instance, class_def.get(), class_instance::cdtor_phase::construction);   // KEEP BYTE-PARALLEL with the vm
    // in a constructor's activation its frame is at the initializer; outside one (an aggregate, a reload) the class is a frame of its own
    const bool in_activation = prologue_frame_ != SIZE_MAX && prologue_frame_ + 1 == call_stack_.size();
    std::optional<construction_trace_scope> level_trace;
    if (!in_activation) { level_trace.emplace(this, class_def->get_name(), nullptr); }
    auto initialize_field = [&](uint64_t field_id, const expression_ptr& initializer_ast) -> checked_result<void> {
        if (in_activation) { call_stack_.back().current_node = initializer_ast.get(); } else { construction_traces_.back().node = initializer_ast.get(); }
        auto result = dispatch_expr(initializer_ast.get());
        if (!result) {   // KEEP BYTE-PARALLEL with the vm: the initializer's own error, the class the frame it stands in
            if (!in_activation && !trace_captured_) { capture_stack_trace(); }
            return result.error_value();
        }
        script_value field_value = initializer_store_value(pop_value(), initializer_ast.get());
        if (is_unwinding_) {   // a throw left the initializer: its placeholder lands unchecked, the throw goes on as raised
            instance->set_field(field_id, field_value);
            return {};
        }
        instance->set_initialized_field(field_id, field_value);
        return {};
    };

    if (aggregate) [[unlikely]] {
        return detail::aggregate_fill_own_fields(*instance, *class_def, *aggregate, engine_, initialize_field);
    }
    for (const auto& [field_id, initializer_ast] : field_initializers) {
        if (initializer_ast) {
            auto initialized = initialize_field(field_id, initializer_ast);
            if (!initialized) { return initialized; }
        }
    }

    return {};
}

void interpreter::apply_field_initializers(const strong_ptr<class_instance>& instance,
                                           const std::shared_ptr<class_definition>& class_def) {
    auto script_def = std::dynamic_pointer_cast<script_class_definition>(class_def);
    if (!script_def || !instance) { return; }
    auto this_value = script_value::make_class_object(*class_def, instance, engine_);
    auto init_env = std::make_shared<environment>(get_global_environment(), string_symbolizer_);
    init_env->define("this", this_value);
    init_env->set_access_context(class_def.get());
    auto initialized = evaluate_field_initializers(instance, script_def, init_env, false);
    if (!initialized) { throw runtime_error(format_error(initialized, *string_symbolizer_)); }
}

// A construction leaves the environment it found
struct interpreter::construction_lender final : detail::construction_host {
    interpreter& self;
    std::shared_ptr<environment> entry_env;
    const detail::call_site_context* call_ctx = nullptr;   // the constructor call's site, while it names these arguments
    const std::vector<script_value>* call_args;

    explicit construction_lender(interpreter& a_self, const std::vector<script_value>* a_callArgs = nullptr) :
        detail::construction_host(a_self.engine_, a_self.string_symbolizer_, a_self.env_symbolizer_, a_self.current_call_depth_,
                                  a_self.limits_->interpreter_call_depth_limit, a_self.is_unwinding_),
        self(a_self),
        entry_env(a_self.environment_),
        call_args(a_callArgs) {
        if (a_callArgs && a_self.pending_call_ctx_ && a_self.pending_call_ctx_->values == a_callArgs && a_self.pending_call_ctx_->arg_exprs) {
            call_ctx = a_self.pending_call_ctx_;
        }
    }
    ~construction_lender() { self.environment_ = std::move(entry_env); }

    void at(const ast_node* node) {
        if (self.prologue_frame_ != SIZE_MAX && self.prologue_frame_ + 1 == self.call_stack_.size()) { self.call_stack_.back().current_node = node; }
    }

    // an initializer-list argument, in the activation's scope and at its frame's place
    checked_result<script_value> evaluate_in(detail::construction_scope& scope, const expression_ptr& expr) {
        struct env_restore {
            interpreter& self;
            std::shared_ptr<environment> previous;
            ~env_restore() { self.environment_ = std::move(previous); }
        } restore{self, self.environment_};
        self.environment_ = scope.env;
        if (!self.call_stack_.empty()) { self.call_stack_.back().current_node = expr.get(); }
        auto result = self.dispatch_expr(expr.get());
        if (!result) { return result.error_value(); }
        return self.pop_value();
    }

    // A constructor's activation: the call binds its parameters, then runs `prologue` in its frame (the parameters' names cells over
    // its slots), then its body
    template <class Prologue>
    checked_result<void> run_constructor(const std::shared_ptr<script_class_definition>& level, const std::shared_ptr<function_decl>& ctor,
                                         const std::vector<script_value>& args, const std::shared_ptr<environment>& definition_env,
                                         const script_value& this_value, Prologue&& prologue) {
        interpreter::constructor_prologue_hook hook;
        hook.frame = self.call_stack_.size();
        hook.body = ctor->body.get();
        hook.definition_env = &definition_env;
        hook.this_value = &this_value;
        hook.level = level.get();
        hook.ctor = ctor.get();
        hook.context = &prologue;
        hook.run = &run_prologue<std::remove_reference_t<Prologue>>;
        struct pending_restore {
            interpreter& self;
            interpreter::constructor_prologue_hook* saved;
            ~pending_restore() { self.pending_prologue_ = saved; }
        } restore{self, self.pending_prologue_};
        self.pending_prologue_ = &hook;
        // the method door a call takes: `this` in the frame, the method scope over the definition environment
        script_defined_function body(ctor->name, shared_parameters_for(ctor.get()), ctor->return_type, ctor->body, nullptr, ctor->local_count);
        body.unit = ctor->unit;
        method_call_override method{&definition_env, &this_value, level.get()};
        struct owned_restore {
            interpreter& self;
            const std::vector<script_value>* saved;
            ~owned_restore() { self.owned_args_ = saved; }
        } owned{self, self.owned_args_};
        if (&args != call_args) { self.owned_args_ = &args; }   // a list the construction built, not the caller's
        auto ran = self.call_function(body, args, &method);
        if (!ran) { return ran.error_value(); }
        return {};
    }
    template <class Prologue>
    static checked_result<void> run_prologue(void* context, detail::construction_scope& scope) { return (*static_cast<Prologue*>(context))(scope); }

    checked_result<void> construct_implicitly(const std::shared_ptr<script_class_definition>& cls, const std::shared_ptr<environment>& definition_env,
                                              const script_value& this_value) override {
        static const std::vector<script_value> no_arguments;
        return detail::construct_instance_members(*this, cls, definition_env, this_value, cls->implicit_default_constructor(), no_arguments);
    }

    checked_result<void> evaluate_field_initializers(const strong_ptr<class_instance>& instance, const std::shared_ptr<script_class_definition>& cls,
                                                     detail::construction_scope& scope, bool skip_parent_recursion,
                                                     const detail::aggregate_plan* aggregate) override {
        return self.evaluate_field_initializers(instance, cls, scope.members ? scope.members : scope.env, skip_parent_recursion, aggregate);
    }

    bool owns_arguments(const std::vector<script_value>&) const { return call_ctx != nullptr; }
    checked_result<void> bind_reference_arguments(const function_decl& ctor, std::vector<script_value>& args) override {
        if (!call_ctx) { return {}; }
        call_frame* caller_locals = self.call_stack_.empty() ? nullptr : &self.call_stack_.back();
        for (size_t i = 0; i < ctor.parameters.size() && i < args.size() && i < call_ctx->arg_exprs->size(); ++i) {
            if (!ctor.parameters[i].is_reference) { continue; }
            if (const script_value* early = detail::early_bound_reference(args[i])) {
                script_value bound = *early;
                args[i] = std::move(bound);
                continue;
            }
            script_value bound(std::monostate{}, self.engine_);
            auto site = self.bind_site_reference((*call_ctx->arg_exprs)[i].get(), caller_locals, entry_env, bound);
            if (!site) { return site.error_value(); }
            if (site.value()) {
                args[i] = std::move(bound);
                continue;
            }
            script_value taken = detail::argument_for_binding(args[i], detail::arg_binding::reference);
            JAISCRIPT_TRY(detail::bind_placeless_reference_argument(self.engine_, ctor, i, taken, !self.parallel_worker_));
            args[i] = std::move(taken);
        }
        return {};
    }
    checked_result<bool> initializer_place(detail::construction_scope& scope, const expression* argument, script_value& out) {
        if (!scope.env || !argument) { return false; }
        if (argument->get_type() == node_type::identifier_expr) {
            const auto* name = static_cast<const identifier_expr*>(argument);
            if (!scope.env->get_value_ptr(name->symbol_id)) { return false; }   // a name the level's scope does not hold (a field by its bare name) binds as its value did
        }
        return self.bind_site_reference(argument, nullptr, scope.env, out);
    }
    std::optional<error_propagator> pass_depth_gate() override { return detail::raise_call_depth(); }
    std::shared_ptr<environment> current_environment() override { return self.environment_; }
    std::shared_ptr<environment> global_environment() override { return self.get_global_environment(); }
    const class_definition* caller_access_context() override { return self.environment_->find_access_context(); }
    std::shared_ptr<environment> parallel_sandbox() override {
        return self.parallel_worker_ && self.parallel_method_pins_ ? self.parallel_method_pins_->definition_env : nullptr;
    }
};

checked_result<script_value> interpreter::execute_callable(const script_callable& payload, const std::vector<script_value>& args) {
    arm_native_stack();
    detail::release_sync release_first(engine_);
    // A destructor a throw's unwinding released runs under its own exception state and the throw resumes after it (C++:
    // destructors run during unwinding); what the destructor raised goes to the script error handler, where C++ would terminate
    // (KEEP BYTE-PARALLEL with vm_backend::execute_callable)
    struct unwind_park {
        interpreter* self;
        std::optional<script_exception> exception;
        std::optional<script_value> value;
        std::vector<stack_trace_entry> trace;
        bool trace_captured = false;
        uint64_t catch_var = 0;
        explicit unwind_park(interpreter* in) : self(in) {
            exception = std::move(self->current_exception_);
            self->current_exception_.reset();
            value = std::move(self->active_exception_value_);
            self->active_exception_value_.reset();
            trace = std::move(self->captured_trace_);
            self->captured_trace_.clear();
            trace_captured = self->trace_captured_;
            self->trace_captured_ = false;
            catch_var = self->current_catch_var_id_;
            self->current_catch_var_id_ = 0;
            self->is_unwinding_ = false;
        }
        ~unwind_park() {
            if (self->is_unwinding_ && self->current_exception_) { self->engine_->report_script_error(self->current_exception_->what(), {}); }
            self->current_exception_ = std::move(exception);
            self->active_exception_value_ = std::move(value);
            self->captured_trace_ = std::move(trace);
            self->trace_captured_ = trace_captured;
            self->current_catch_var_id_ = catch_var;
            self->is_unwinding_ = true;
        }
    };
    std::optional<unwind_park> park;
    if (is_unwinding_) [[unlikely]] { park.emplace(this); }
    // A callable returns its result; what its statements leave on the value stack (an expression declaration's value, kept for
    // a block's statement boundary) is gone when it returns, so a destructor running in the middle of an expression never hands
    // the expression a value of its own
    struct stack_trim {
        interpreter* self;
        size_t mark;
        ~stack_trim() { while (self->valueStack_.size() > mark) { (void)self->valueStack_.pop(); } }
    } trim{this, valueStack_.size()};
    switch (payload.kind) {
        case script_callable::kind_type::function:
            return call_function(*payload.fn, args);
        case script_callable::kind_type::overload_set: {
            if (!payload.overloads()) {
                return checked_result<script_value>(make_error_code(runtime_error_code::internal_error), "Overload set payload without members");
            }
            auto picked = detail::select_overload_member(*payload.overloads(), args.data(), args.size(), engine_);
            if (!picked) { return picked.error_value(); }
            const script_callable* member = detail::script_callable_of(*picked.value(), engine_);
            return member && member->fn ? call_function(*member->fn, args) : call_value(picked.value()->as_function(), args);
        }
        case script_callable::kind_type::method: {
            auto method_env = method_environment(payload.definition_env(), *payload.this_obj(), payload.cls().get());
            method_env->define("this", *payload.this_obj());
            auto result = execute_method_ast(payload.ast(), method_env, args);
            end_scope(method_env);   // the receiver dies with the call, as the vm's method frame drops it
            return result;
        }
        case script_callable::kind_type::static_method: {
            auto static_env = std::make_shared<environment>(payload.definition_env(), string_symbolizer_, payload.cls());
            return execute_method_ast(payload.ast(), static_env, args);
        }
        case script_callable::kind_type::constructor: {
            auto script_cls = std::dynamic_pointer_cast<script_class_definition>(payload.cls());
            if (!script_cls) {
                return checked_result<script_value>(make_error_code(runtime_error_code::internal_error), "Constructor payload without script class definition");
            }
            const bool waived = !parallel_worker_ && engine_->take_construction_access_waiver();
            if (script_cls->get_constructor_asts().empty()) {
                if (auto made = detail::construct_literal_default(script_cls, args, engine_, current_call_depth_, limits_->interpreter_call_depth_limit)) {
                    return std::move(*made);
                }
                construction_lender host(*this);
                return detail::construct_default_instance(host, script_cls, args);
            }
            return construct_instance(script_cls, payload.definition_env(), args, !waived);
        }
    }
    return checked_result<script_value>(make_error_code(runtime_error_code::internal_error), "Unknown callable kind");
}

checked_result<script_value> interpreter::construct_instance(std::shared_ptr<script_class_definition> class_def,
                                                              std::shared_ptr<environment> definition_env,
                                                              const std::vector<script_value>& args, bool check_access) {
    construction_lender host(*this, &args);
    std::optional<detail::call_depth_guard> initializer_depth;   // the pick's, until the constructor's activation counts its own
    std::shared_ptr<function_decl> matching_ctor;
    std::vector<script_value> captured;
    auto pre = detail::prepare_construction(host, class_def, args, initializer_depth, matching_ctor, captured, check_access);
    if (!pre || is_unwinding_) { return pre; }
    script_value this_value = std::move(pre.value());
    initializer_depth.reset();
    auto constructed = detail::construct_instance_members(host, class_def, definition_env, this_value, matching_ctor, captured.empty() ? args : captured);
    if (!constructed) { return constructed.error_value(); }
    if (!is_unwinding_) { detail::construction_completed(this_value); }
    return this_value;   // a constructor call yields its instance (a bare `return;` ends the body early)
}

// RAII wrapper for method environments
scoped_method_environment::scoped_method_environment(
    interpreter* interp,
    std::shared_ptr<environment> parent,
    const script_value& this_obj,
    class_definition* access_ctx)
    : interp_(interp)
    , env_(interp->method_environment(parent, this_obj, access_ctx))
{
    // Define 'this' as a regular variable for compatibility
    // method_environment::get() also has special handling for 'this'
    env_->define("this", this_obj);
}

scoped_method_environment::~scoped_method_environment() {
    // Always clear immediately to release 'this' reference
    interp_->end_scope(env_);
}

// KEEP BYTE-PARALLEL with vm_backend::bind_bare_cpp_property_ref: the member scope's
// receiver, else the env's 'this' (lambdas made in methods reach it through the chain).
// `env` = a CALLER env at a ref-arg bind (the current frame's scope is the callee's).
std::optional<checked_result<script_value>> interpreter::bind_bare_cpp_property_ref(uint64_t symbol_id, bool global_qualified,
                                                                                    environment* env) {
    if (global_qualified) {
        return std::nullopt;
    }
    script_value* this_ptr = !env && member_scope_.state == detail::member_scope_state::instance && member_scope_.this_storage
        ? member_scope_.this_storage
        : (env ? env : environment_.get())->get_value_ptr(string_symbolizer_->get_this_id());
    if (!this_ptr || !this_ptr->is_object() || this_ptr->is_null()) {
        return std::nullopt;
    }
    auto instance = this_ptr->get_class_instance();
    if (!instance || !detail::cpp_property_getter_id(instance.get(), symbol_id)) {
        return std::nullopt;
    }
    return detail::bind_cpp_property_ref(*this_ptr, symbol_id, engine_, string_symbolizer_);
}

// Resolves a call-site argument expression to the reference a T& binds: a static, a variable (its cell), a member of the
// caller's scope, a field/subscript/chain lvalue or a C++ property. False: the argument names none of those (the caller
// decides between an evaluated reference, a temporary and the non-lvalue error). KEEP BYTE-PARALLEL with
// vm_backend::bind_site_reference.
checked_result<bool> interpreter::bind_site_reference(const expression* argExpr, call_frame* caller_locals,
                                                      const std::shared_ptr<environment>& caller_env, script_value& out,
                                                      host_slot* slot_out, size_t caller_index) {
    // C::s and ns::x are lvalues: the reference binds their storage (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
    if (argExpr && argExpr->get_type() == node_type::member_expr && caller_env) {
        if (auto cell = detail::static_member_lvalue(*engine_, static_cast<const member_expr*>(argExpr), *caller_env, *string_symbolizer_,
                                                     caller_env->find_access_context())) {
            if (!*cell) { return cell->error_value(); }
            return reference_to_storage(*cell->value(), out);
        }
    }

    if (argExpr && argExpr->get_type() == node_type::identifier_expr) {
        auto* identExpr = static_cast<const identifier_expr*>(argExpr);
        const uint64_t symbol_id = identExpr->symbol_id;

        // ::name ref args bind against the GLOBAL env only
        if (identExpr->global_qualified) {
            script_value* g = get_global_environment()->get_value_ptr(symbol_id);
            if (!g) {
                return checked_result<bool>(
                    make_error_code(runtime_error_code::undefined_variable),
                    environment_->undefined_text(symbol_id, "Cannot take reference of undefined variable"), symbol_id
                );
            }
            return reference_to_storage(*g, out);
        }

        // Caller frame-slot local (env lookup can't see slot locals and would walk to
        // an unrelated same-named outer variable)
        if (identExpr->slot_index != SIZE_MAX && caller_locals) {
            if (script_value* slotPtr = caller_locals->get_local(identExpr->slot_index)) {
                if (slot_out && !slotPtr->is_reference()) {   // an unboxed local: nothing else reaches it during a host call
                    *slot_out = host_slot{ reinterpret_cast<const void*>(caller_index + 1), identExpr->slot_index };
                    out = *slotPtr;
                    return true;
                }
                return reference_to_storage(*slotPtr, out);
            }
        }

        // Caller member scope: a field/static outranks a same-named global as a ref
        // target (bound as raw storage, exactly like the old post-walk fallback bound
        // the same pointer when no global collided).
        const auto caller_state = caller_locals
            ? static_cast<detail::member_scope_state>(caller_locals->scope_state)
            : detail::member_scope_state::none;
        if (caller_state == detail::member_scope_state::instance ||
            caller_state == detail::member_scope_state::statics) {
            if (script_value* own = caller_env
                    ? detail::own_scope_lookup(caller_env.get(), caller_locals->scope_floor, symbol_id)
                    : nullptr) {
                return reference_to_storage(*own, out);
            }
            auto member = detail::probe_member_scope(caller_state, caller_locals->scope_this,
                                                     caller_locals->scope_static, symbol_id,
                                                     /*want_methods*/ false);
            if (member) {
                JAISCRIPT_TRY(detail::bare_member_access(member, symbol_id,
                                                         caller_env ? caller_env->find_access_context() : nullptr));
            }
            if (member.value_ptr) {
                return reference_to_storage(*member.value_ptr, out);
            }
            // A C++ property binds the member itself / its accessors
            if (member.what == detail::member_probe_hit::kind::cpp_property) {
                auto minted = detail::bind_cpp_property_ref(*caller_locals->scope_this, symbol_id, engine_, string_symbolizer_);
                if (!minted) {
                    return minted.error_value();
                }
                out = std::move(minted.value());
                return true;
            }
            // A member METHOD shadows (never a global)
            if (detail::member_scope_has_method(caller_state, caller_locals->scope_this,
                                                caller_locals->scope_static, symbol_id)) {
                return checked_result<bool>(
                    make_error_code(runtime_error_code::undefined_variable),
                    "Cannot take reference of undefined variable"
                );
            }
            script_value* beyond = caller_locals->scope_floor
                ? caller_locals->scope_floor->get_value_ptr(symbol_id)
                : (caller_env ? caller_env->get_value_ptr(symbol_id) : nullptr);
            if (!beyond) {
                return checked_result<bool>(
                    make_error_code(runtime_error_code::undefined_variable),
                    environment_->undefined_text(symbol_id, "Cannot take reference of undefined variable"), symbol_id
                );
            }
            return reference_to_storage(*beyond, out);
        }

        script_value* argPtr = caller_env ? caller_env->get_value_ptr(symbol_id) : nullptr;
        if (!argPtr) {
            // A hybrid's C++ property reached through the env's 'this' (lambdas made in
            // methods); KEEP BYTE-PARALLEL with vm_backend::bind_parameters
            if (auto minted = caller_env ? bind_bare_cpp_property_ref(symbol_id, false, caller_env.get()) : std::nullopt) {
                if (!*minted) {
                    return minted->error_value();
                }
                out = std::move(minted->value());
                return true;
            }
            return checked_result<bool>(
                make_error_code(runtime_error_code::undefined_variable),
                environment_->undefined_text(symbol_id, "Cannot take reference of undefined variable"), symbol_id
            );
        }
        return reference_to_storage(*argPtr, out);
    }

    // Field/subscript/chain lvalue argument: resolve through the shared helper into
    // an owner-pinned reference
    if (argExpr && detail::is_ref_bindable_lvalue(argExpr)) {
        auto resolved = detail::resolve_ref_lvalue(argExpr, detail::caller_frame_view{caller_locals},
                                                   caller_env.get(), engine_, string_symbolizer_);
        if (resolved) {
            out = std::move(resolved.value());
            return true;
        }
        // the kernel's generic non-lvalue verdict falls through to the evaluated argument below (identifier-keyed map entries
        // classify as lvalues but only evaluation resolves them); access enforcement and other specific errors propagate
        if (resolved.error() != make_error_code(runtime_error_code::invalid_reference)) {
            return resolved.error_value();
        }
    }
    // A math component (f(v.x)) binds a COMPONENT reference over the vector's storage
    auto part = component_reference(argExpr, caller_locals, caller_env);
    if (!part) {
        return part.error_value();
    }
    if (part.value().is_reference()) {
        out = std::move(part.value());
        return true;
    }

    return false;
}

uint8_t interpreter::source_class(const expression* e) {
    return detail::classify_source(e, [this](const call_expr& call) { return detail::call_returns_class_value(call, *engine_); });
}

checked_result<script_value> interpreter::declared_component_reference(variable_decl* decl, script_value* temporary) {
    auto part = component_reference(decl->initializer.get(), call_stack_.empty() ? nullptr : &call_stack_.back(), environment_);
    if (!part) {
        return part;
    }
    if (!part.value().is_reference() && decl->is_const && temporary) {   // KEEP BYTE-PARALLEL with the vm's exec_decl_ref_value
        return detail::const_reference_temporary(*decl, std::move(*temporary), engine_, [&](script_value v, const type_info_ptr& t) {
            return enforce_type_compatibility(std::move(v), t, decl->name, detail::slot_kind::declaration);
        });
    }
    if (!part.value().is_reference()) {
        return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference),
            "Cannot take reference of non-lvalue expression (var&/auto& declarations bind locals, "
            "subscript chains like arr[i] or grid[y][x], map entries, object fields like obj.field and C++ properties; "
            "computed script properties are not ref-bindable)");
    }
    return part;
}

checked_result<script_value> interpreter::component_reference(const expression* e, call_frame* caller_locals,
                                                              const std::shared_ptr<environment>& caller_env) {
    return detail::math::component_reference(e, detail::caller_frame_view{caller_locals}, caller_env.get(),
        caller_env ? caller_env->find_access_context() : nullptr, engine_, string_symbolizer_,
        [this](script_value& storage) { return share_boxed_slot(storage); });
}

// Boxing a frame's slot on demand (a reference argument, a reference declaration) marks that frame: its exit checks the
// cells it shared, as a frame whose locals a lambda captures by reference does
script_value interpreter::share_boxed_slot(script_value& storage) {
    if (!storage.is_reference() && !parallel_worker_) [[unlikely]] {
        for (size_t i = call_stack_.size(); i-- > 0;) {
            auto& locals = call_stack_[i].locals;
            if (!locals.empty() && &storage >= locals.data() && &storage < locals.data() + locals.size()) {
                call_stack_[i].boxed_cells = true;
                break;
            }
        }
    }
    return share_boxed_env_storage(storage, engine_);
}

checked_result<bool> interpreter::reference_to_storage(script_value& storage, script_value& out) {
    // Share the holder (zero alloc; cells alias the same box, element/field refs keep
    // their container/instance re-resolution), boxing on demand when the variable
    // predates its escape mark (cross-execute global, C++ define, dynamic shape)
    out = share_boxed_slot(storage);
    return true;
}

// Stateless ref-param binding straight from the call-site arg expression. Kept out of
// call_function so its locals stay off the per-recursion-level native frame (Debug
// stack ceiling). KEEP BYTE-PARALLEL with vm_backend::bind_parameters' ref branch.
checked_result<void> interpreter::bind_reference_parameter(const parameter& param, size_t frame_index,
                                                           const expression* argExpr,
                                                           const std::shared_ptr<environment>& caller_env,
                                                           const script_value* evaluated_arg,
                                                           unit_stamp caller_unit,
                                                           const script_defined_function* callee, size_t param_index) {
    // the argument is the caller's code: its identifier resolves under the caller's unit; the
    // callee's unit, entered by the call ahead of the binding, resumes when the binding returns
    unit_sync_scope unitSync{ this };   // the callee's unit again once the argument is bound
    enter_unit(caller_unit);
    call_frame* caller_locals = frame_index >= 1 ? &call_stack_[frame_index - 1] : nullptr;

    script_value bound(std::monostate{}, engine_);
    auto site = bind_site_reference(argExpr, caller_locals, caller_env, bound);
    if (!site) {
        return site.error_value();
    }
    if (site.value()) {
        call_stack_[frame_index].set_local(param.slot_index, std::move(bound));
        return {};
    }

    // General lvalues: the argument already evaluated
    // in normal left-to-right order, and subscript/map reads over lvalue bases mint
    // owner-pinned references with full index generality - computed indices
    // (grid[y+1][x]), member-expr indices (arr[o.idx]), map keys (m["k"]). A reference
    // VALUE is an lvalue: share its holder, exactly like the external-invocation path
    // always has. Nothing runs at the bind point, so the fast-path constraint holds.
    // (KEEP BYTE-PARALLEL with vm_backend::bind_parameters' ref branch)
    if (evaluated_arg && evaluated_arg->is_reference()) {
        call_stack_[frame_index].set_local(param.slot_index, script_value(*evaluated_arg));
        return {};
    }
    // a host T& result names the host's object: an lvalue, as in C++ (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
    if (evaluated_arg && detail::borrows_host_object(*evaluated_arg)) {
        call_stack_[frame_index].set_local(param.slot_index, script_value(*evaluated_arg));
        return {};
    }
    // a string& takes a writable temporary (lambdas too); another T& takes one only when a declared function never writes
    // through it (detail/ref_param_writes.hpp)
    if (evaluated_arg && (detail::is_string_reference_temporary(param, *evaluated_arg) || param.is_const ||
        (callee && callee->name != "<lambda>" &&
            detail::ref_parameter_takes_temporary(*engine_, callee->parameters(), callee->body.get(), param_index, !parallel_worker_)))) {
        call_stack_[frame_index].set_local(param.slot_index, detail::make_reference_argument_temporary(param, *evaluated_arg, engine_));
        return {};
    }

    return checked_result<void>(
        make_error_code(runtime_error_code::invalid_reference),
        "Cannot pass non-lvalue to reference parameter"
    );
}

// The prologue's scopes: the field initializers' (`this` and the class as the access context), and over it the initializer list's,
// each parameter's name a cell over its slot, so what the prologue stores to a parameter the body reads
checked_result<void> interpreter::run_constructor_prologue(constructor_prologue_hook& hook, size_t frame_index, const script_defined_function& function) {
    detail::construction_scope scope;
    if (detail::prologue_evaluates(static_cast<const script_class_definition&>(*hook.level), *hook.ctor)) {
        scope.members = std::make_shared<environment>(*hook.definition_env, env_symbolizer_);
        scope.members->define(string_symbolizer_->get_this_id(), *hook.this_value);
        scope.members->set_access_context(hook.level);
        scope.env = scope.members;
        for (const auto& param : function.parameters()) {
            if (param.slot_index == SIZE_MAX) { continue; }
            if (script_value* slot = call_stack_[frame_index].get_local(param.slot_index)) {
                if (scope.env == scope.members) {
                    scope.env = std::make_shared<environment>(scope.members, env_symbolizer_);
                    scope.env->set_access_context(hook.level);
                }
                if (param.symbol_id == UINT64_MAX) { param.symbol_id = string_symbolizer_->intern(param.name); }
                scope.env->define(param.symbol_id, share_boxed_slot(*slot));
            }
        }
    }
    struct frame_restore {
        size_t& at;
        size_t saved;
        ~frame_restore() { at = saved; }
    } restore{prologue_frame_, prologue_frame_};
    prologue_frame_ = frame_index;
    return hook.run(hook.context, scope);
}

// A stamped lvalue argument's place, resolved where the argument is evaluated (detail::capture_call_argument)
checked_result<bool> interpreter::mint_argument_place(const expression* argExpr, script_value& out) {
    return bind_site_reference(argExpr, call_stack_.empty() ? nullptr : &call_stack_.back(), environment_, out);
}

// a T& parameter after `from` whose argument names a place evaluation decides, its reference not yet bound
bool interpreter::later_place_argument(const script_defined_function& function, const detail::call_site_context* call_ctx, size_t from,
                                       const std::vector<script_value>& args) const {
    if (!call_ctx || !call_ctx->arg_exprs) { return false; }
    const auto& params = function.parameters();
    for (size_t j = from; j < args.size() && j < params.size() && j < call_ctx->arg_exprs->size(); ++j) {
        if (params[j].is_reference && !detail::early_bound_reference(args[j]) &&
            detail::transient_read_marker::place_lvalue_argument((*call_ctx->arg_exprs)[j].get())) { return true; }
    }
    return false;
}

// a conversion that may run code binds after the arguments after it were evaluated, and nothing ran between: the T& parameters
// among them bind their places now, as their evaluation would have
checked_result<void> interpreter::bind_later_places(const script_defined_function& function, const detail::call_site_context* call_ctx,
                                                    size_t from, std::vector<script_value>& args, size_t frame_index,
                                                    const std::shared_ptr<environment>& caller_env, unit_stamp caller_unit) {
    if (!call_ctx || !call_ctx->arg_exprs) { return {}; }
    const auto& params = function.parameters();
    for (size_t j = from; j < args.size() && j < params.size() && j < call_ctx->arg_exprs->size(); ++j) {
        const expression* e = (*call_ctx->arg_exprs)[j].get();
        if (!params[j].is_reference || detail::early_bound_reference(args[j]) || !detail::transient_read_marker::place_lvalue_argument(e)) { continue; }
        unit_sync_scope unitSync{ this };
        enter_unit(caller_unit);
        script_value minted(std::monostate{}, engine_);
        auto placed = bind_site_reference(e, frame_index >= 1 ? &call_stack_[frame_index - 1] : nullptr, caller_env, minted);
        if (!placed) { return placed.error_value(); }
        if (placed.value()) { detail::hold_minted_reference(engine_, args[j], detail::arg_binding::reference, std::move(minted)); }
    }
    return {};
}

// A host callable's T& argument (execution_backend::host_reference_argument): the argument the call in flight passed, when
// the callable received it (a callback a builtin makes with its own arguments did not), resolved as a T& parameter binds it
checked_result<script_value> interpreter::host_reference_argument(const std::vector<script_value>& args, size_t index, size_t offset,
                                                                  host_slot& slot) {
    slot = {};
    script_value none(std::monostate{}, engine_);
    const detail::call_site_context* ctx = pending_call_ctx_;
    if (!ctx || !ctx->values || !ctx->arg_exprs || ctx->values->size() != ctx->arg_exprs->size() ||
        ctx->values->size() + offset != args.size() || index >= ctx->values->size()) {
        return none;
    }
    const script_value& passed = (*ctx->values)[index];
    if (ctx->values != &args && !detail::same_argument(passed, args[index + offset])) {
        return none;
    }
    if (const script_value* early = detail::early_bound_reference(passed)) {
        return *early;
    }
    const expression* argExpr = (*ctx->arg_exprs)[index].get();
    call_frame* caller_locals = call_stack_.empty() ? nullptr : &call_stack_.back();
    script_value bound(std::monostate{}, engine_);
    auto site = bind_site_reference(argExpr, caller_locals, environment_, bound, &slot, call_stack_.empty() ? 0 : call_stack_.size() - 1);
    if (!site) {
        return site.error_value();
    }
    if (site.value()) {
        return bound;
    }
    if (const auto* holder = detail::deferred_argument(passed)) {
        const script_value& lvalue = holder->argument_source->lvalue;
        return lvalue.is_reference() ? lvalue : none;
    }
    return passed.is_reference() ? passed : none;
}

checked_result<void> interpreter::host_slot_store(const host_slot& slot, script_value& value) {
    const size_t frame_index = reinterpret_cast<size_t>(slot.frame) - 1;
    script_value* storage = frame_index < call_stack_.size() ? call_stack_[frame_index].get_local(slot.slot) : nullptr;
    if (!storage) {
        return checked_result<void>(make_error_code(runtime_error_code::invalid_reference), "Reference target is null");
    }
    return detail::variable_store(*storage, value, engine_);
}

namespace {
void note_scope_slot(size_t slot, std::string_view name, std::vector<std::pair<size_t, std::string_view>>& out) {
    if (slot != SIZE_MAX && !name.empty() && name[0] != '\x01') out.emplace_back(slot, name);
}

void note_scope_decl(const statement* d, std::vector<std::pair<size_t, std::string_view>>& out) {
    if (d->get_type() == node_type::variable_decl) {
        const auto* v = static_cast<const variable_decl*>(d);
        if (!v->is_static) note_scope_slot(v->slot_index, v->name, out);
    } else if (d->get_type() == node_type::destructuring_decl) {
        const auto* dd = static_cast<const destructuring_decl*>(d);
        for (size_t i = 0; i < dd->names.size() && i < dd->slot_indices.size(); ++i) note_scope_slot(dd->slot_indices[i], dd->names[i].first, out);
    }
}

// Debugger: the frame-slot locals in scope at `at`, the statement the frame is running, outermost first: what each scope
// enclosing it declared before it (the vm's chunk::debug_locals at the frame's ip). A for statement running mid-statement is
// in its condition or update, past its initializer (which runs as a statement of its own). Nested function, lambda and
// parallel bodies are frames of their own.
bool scope_slots_at(const statement* s, const ast_node* at, bool mid_statement, std::vector<std::pair<size_t, std::string_view>>& out) {
    if (!s) return false;
    const size_t mark = out.size();
    auto in_scope_list = [&](const auto& list) {
        for (const auto& d : list) {
            if (scope_slots_at(d.get(), at, mid_statement, out)) return true;
            note_scope_decl(d.get(), out);
        }
        return false;
    };
    bool found = false;
    switch (s->get_type()) {
    case node_type::statement_decl:
        found = s == at || scope_slots_at(static_cast<const statement_decl*>(s)->statement.get(), at, mid_statement, out);
        break;
    case node_type::block_stmt:
        found = s == at || in_scope_list(static_cast<const block_stmt*>(s)->declarations);
        break;
    case node_type::if_stmt: {
        auto* is = static_cast<const if_stmt*>(s);
        found = s == at || scope_slots_at(is->then_statement.get(), at, mid_statement, out) || scope_slots_at(is->else_statement.get(), at, mid_statement, out);
        break;
    }
    case node_type::while_stmt:
        found = s == at || scope_slots_at(static_cast<const while_stmt*>(s)->body.get(), at, mid_statement, out);
        break;
    case node_type::for_stmt: {
        auto* fs = static_cast<const for_stmt*>(s);
        if (s == at) {
            if (mid_statement && fs->initializer) note_scope_decl(fs->initializer.get(), out);
            return true;
        }
        if (fs->initializer && fs->initializer.get() == at) return true;
        if (fs->initializer) note_scope_decl(fs->initializer.get(), out);
        found = scope_slots_at(fs->body.get(), at, mid_statement, out);
        break;
    }
    case node_type::range_for_stmt: {
        auto* rf = static_cast<const range_for_stmt*>(s);
        if (s == at) return true;
        note_scope_slot(rf->variable_slot_index, rf->variable_name, out);
        found = scope_slots_at(rf->body.get(), at, mid_statement, out);
        break;
    }
    case node_type::switch_stmt: {
        auto* sw = static_cast<const switch_stmt*>(s);
        found = s == at;
        for (size_t i = 0; !found && i < sw->cases.size(); ++i) {
            found = sw->cases[i].get() == at || in_scope_list(sw->cases[i]->body);
            if (!found) out.resize(mark);
        }
        if (!found && sw->default_case) found = sw->default_case.get() == at || in_scope_list(sw->default_case->body);
        break;
    }
    case node_type::try_stmt: {
        auto* tr = static_cast<const try_stmt*>(s);
        found = s == at || scope_slots_at(tr->try_block.get(), at, mid_statement, out);
        if (!found) {
            note_scope_slot(tr->catch_slot_index, tr->catch_var, out);
            found = scope_slots_at(tr->catch_block.get(), at, mid_statement, out);
        }
        break;
    }
    default:
        found = s == at;
        break;
    }
    if (!found) out.resize(mark);
    return found;
}
}

void interpreter::note_running_statement(statement* stmt) {
    if (!call_stack_.empty()) call_stack_.back().current_node = stmt;
    else top_level_node_ = stmt;
}

std::vector<std::pair<size_t, std::string_view>> interpreter::frame_scope_slots(size_t frame_index) const {
    std::vector<std::pair<size_t, std::string_view>> out;
    const call_frame& frame = call_stack_[call_stack_.size() - 1 - frame_index];
    const script_defined_function* fn = frame.function;
    if (!fn) return out;
    // a for statement's update runs past its body's statements, the last of which stays the frame's current node (the vm's line)
    const size_t depth = call_stack_.size() - frame_index;
    const auto updating = std::find_if(updating_loops_.rbegin(), updating_loops_.rend(), [&](const auto& u) { return u.first == depth; });
    const ast_node* at = updating != updating_loops_.rend() ? updating->second : frame.current_node;
    for (const auto& p : fn->parameters())
        if (!p.binds_capture) note_scope_slot(p.slot_index, p.name, out);
    scope_slots_at(fn->body.get(), at, frame_index > 0, out);
    return out;
}

std::vector<std::pair<std::string, script_value>> interpreter::get_frame_locals(size_t frame_index) const {
    std::vector<std::pair<std::string, script_value>> out;
    if (frame_index >= call_stack_.size()) return out;   // the top level: the caller reads its environment
    const call_frame& frame = call_stack_[call_stack_.size() - 1 - frame_index];
    for (const auto& [slot, name] : frame_scope_slots(frame_index)) {
        if (const script_value* v = frame.get_local(slot)) out.emplace_back(std::string(name), *v);
    }
    detail::fold_lowered_locals(out, engine_);
    return out;
}

script_value* interpreter::debug_local_storage(const std::string& name) {
    if (call_stack_.empty()) return nullptr;
    const auto slots = frame_scope_slots(0);
    for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
        if (it->second == name) return call_stack_.back().get_local(it->first);
    }
    return nullptr;
}

// Function call implementation - returns checked_result for consistent error handling
// Uses stack-based call frames for fast parameter access instead of hash map environments

checked_result<script_value> interpreter::call_function(const script_defined_function& function, const std::vector<script_value>& args,
                                                        const method_call_override* method_ctx) {
    // Check recursion depth limit FIRST
    if (current_call_depth_ >= limits_->interpreter_call_depth_limit) {
        return detail::raise_call_depth();
    }
    // Wide native frames can exhaust a small host stack before the cap: a catchable error of its own, never 0xC00000FD
    if (detail::native_stack_low()) [[unlikely]] {
        return detail::raise_native_stack();
    }

    if (execution_limit_exhausted()) [[unlikely]] {
        return execution_limit_failure();
    }

    // RAII guard for call depth tracking - ensures decrement even on early return
    detail::call_depth_guard depth_guard(current_call_depth_);
    // A callee's own statements decide whether their assignment expressions are consumed; the
    // caller's statement position (an expression statement clears the flag) must not leak in
    struct unconsumed_root_scope { const expression*& root; const expression* saved; ~unconsumed_root_scope() { root = saved; } } root_scope{ unconsumed_root_, unconsumed_root_ };
    unconsumed_root_ = nullptr;

    // Consume the pending call-site context (set by visit_call around the invoke): ref
    // params bind against the caller's variables through it. Consumed exactly once -
    // nested calls set their own; external (C++) invocations arrive with none.
    const detail::call_site_context* call_ctx = pending_call_ctx_;
    pending_call_ctx_ = nullptr;
    if (call_ctx && !detail::same_arguments(call_ctx->values, args)) {
        call_ctx = nullptr;   // a callback a builtin or a host makes with its own arguments
    }


    if (args.size() != function.parameters().size() && !arity_accepts(function.parameters(), args.size())) {
        return detail::arity_error(function.name, function.parameters().size(), args.size(), arity_count_ids_, *string_symbolizer_);
    }

    // the frame is held by index: a parameter's conversion can run a constructor that pushes frames and moves call_stack_
    call_stack_.emplace_back();
    const size_t frame_index = call_stack_.size() - 1;
    call_stack_[frame_index].function_name = function.name;
    call_stack_[frame_index].function = &function;   // the return site + debugger read it

    // Pre-allocate locals for all slots (params + local variables)
    // Slot indices are assigned by the parser at parse time
    call_stack_[frame_index].reserve_locals(function.local_count);

    // Set up closure environment for non-local lookups
    auto previousEnv = environment_;
    const detail::member_scope_ctx previousScope = member_scope_;
    unit_sync_scope unitSync{ this };   // the caller's unit again once this frame is gone
    const unit_stamp caller_unit = frame_index >= 1 && call_stack_[frame_index - 1].function ? call_stack_[frame_index - 1].function->unit : string_symbolizer_->current_unit();   // a by-reference argument resolves under it
    enter_unit(function.unit);
    member_scope_ = {};

    // Determine closure environment and method context; each branch stamps the
    // frame's member scope (detail/member_scope.hpp) - the C++ shadowing ladder
    // needs the callee KIND, which only these branches know (a plain function's
    // env parents on the CALLER's chain, so the chain alone cannot distinguish
    // lexical member scope from dynamic parenting).
    // NOTE: Re-access call_stack_[frame_index] each time instead of caching reference
    if (method_ctx) {
        // Direct method dispatch: same net effect as the closure_env->is_method_env()
        // branch below (this in the frame, method scope env on definition_env), minus
        // the carrier method env and its per-call define("this")
        call_stack_[frame_index].set_this(*method_ctx->this_obj);
        call_stack_[frame_index].closure_env = *method_ctx->definition_env;
        environment_ = method_environment(*method_ctx->definition_env, *method_ctx->this_obj,
                                                     method_ctx->access_ctx);
        member_scope_.state = detail::member_scope_state::instance;
        member_scope_.this_storage = call_stack_[frame_index].this_object_ptr.get();
        member_scope_.statics = method_ctx->access_ctx;
        member_scope_.floor = method_ctx->definition_env->get();
    } else if (function.closure_env) {
        if (function.closure_env->is_method_env()) {
            // Method call - store 'this' in the call frame
            auto this_obj = function.closure_env->get_this_object();
            call_stack_[frame_index].set_this(this_obj);  // Uses set_this which sets is_method = true
            call_stack_[frame_index].closure_env = function.closure_env->get_parent();
            // Set environment to a method environment for 'this' field lookups in body
            environment_ = method_environment(function.closure_env->get_parent(), this_obj,
                                                         function.closure_env->get_access_context());
            member_scope_.state = detail::member_scope_state::instance;
            member_scope_.this_storage = call_stack_[frame_index].this_object_ptr.get();
            member_scope_.statics = function.closure_env->get_access_context();
            member_scope_.floor = function.closure_env->parent_raw();
        } else if (function.closure_env->is_static_method_env()) {
            // Static method - store class definition in frame
            call_stack_[frame_index].static_class_def = function.closure_env->get_class_definition();
            call_stack_[frame_index].is_static_method = true;
            call_stack_[frame_index].closure_env = function.closure_env->get_parent();
            // Create static method environment for static field access
            environment_ = std::make_shared<environment>(
                function.closure_env->get_parent(),
                env_symbolizer_,
                function.closure_env->get_class_definition()
            );
            member_scope_.state = detail::member_scope_state::statics;
            member_scope_.statics = call_stack_[frame_index].static_class_def.get();
            member_scope_.floor = function.closure_env->parent_raw();
        } else {
            // Regular closure
            call_stack_[frame_index].closure_env = function.closure_env;
            environment_ = std::make_shared<environment>(function.closure_env, env_symbolizer_);
            // A closure created inside a method sees that method's class scope
            // (lexically, through its captured chain) between its own scopes and
            // the globals - the boundary env's OWN storage (the enclosing call's
            // escaped locals) stays on the far side of the members.
            if (environment* boundary = detail::find_member_boundary(function.closure_env.get())) {
                if (boundary->is_method_env()) {
                    if (script_value* this_storage = boundary->method_this_storage()) {
                        member_scope_.state = detail::member_scope_state::instance;
                        member_scope_.this_storage = this_storage;
                        member_scope_.statics = boundary->get_access_context();
                        member_scope_.floor = (boundary == function.closure_env.get())
                                                  ? boundary->parent_raw() : boundary;
                    }
                } else if (auto boundary_class = boundary->get_class_definition()) {
                    member_scope_.state = detail::member_scope_state::statics;
                    member_scope_.statics = boundary_class.get();
                    member_scope_.floor = (boundary == function.closure_env.get())
                                              ? boundary->parent_raw() : boundary;
                }
            }
        }
    } else {
        // Regular function: the caller's chain past its block scopes (C++: a callee never sees a block's declarations)
        auto parent = environment::call_parent(previousEnv, function.block_defined);
        call_stack_[frame_index].closure_env = parent;
        environment_ = std::make_shared<environment>(std::move(parent), env_symbolizer_);
    }

    // Mirror the member scope onto the frame: the shared ref-lvalue kernel
    // (detail/ref_lvalue.hpp) reads it through caller_frame_view, and the vm reads
    // the same call_frame fields (its frames compute them lazily instead).
    call_stack_[frame_index].scope_state = static_cast<uint8_t>(member_scope_.state);
    call_stack_[frame_index].scope_floor = member_scope_.floor;
    call_stack_[frame_index].scope_this = member_scope_.this_storage;
    call_stack_[frame_index].scope_static = member_scope_.statics;

    // Store previous return state
    bool previousHasReturn = hasReturnValue_;
    std::optional<script_value> previousReturn = returnValue_;
    hasReturnValue_ = false;

    // Helper lambda to cleanup and restore state
    auto cleanup = [&](bool clear_this = true) {
        // capture before pop_back: deeper frames are still present
        if (is_unwinding_ && !trace_captured_) capture_stack_trace();
        detail::shared_cell_release shared_cells{engine_};   // declared first: its check runs after the frame's locals die
        {
            // The locals die while this frame is still the innermost one, as C++ destroys them inside the function: a destructor
            // they fire traces through it (KEEP BYTE-PARALLEL with the vm). Moved out first, so a destructor reentering
            // call_function only grows call_stack_ above this frame.
            const size_t dying_index = call_stack_.size() - 1;
            if ((call_stack_[dying_index].boxed_cells || (function.body && function.body->shares_cells)) && !parallel_worker_) [[unlikely]] {
                script_value::gather_shared_cells(call_stack_[dying_index].locals.data(), call_stack_[dying_index].locals.size(), shared_cells.cells);
            }
            reverse_destroyed_values dying_locals = std::move(call_stack_[dying_index].locals);
        }
        call_frame dying_frame = std::move(call_stack_.back());
        call_stack_.pop_back();

        auto function_env = environment_;
        if (clear_this) {
            // only the callee's own method scope lets go of this: a plain callee's scope may be parented on the caller's
            if (function_env->is_method_env()) {
                function_env->clear_this_reference();
            }
        }
        environment_ = previousEnv;
        member_scope_ = previousScope;
        hasReturnValue_ = previousHasReturn;
        returnValue_ = previousReturn;
    };

    // parameters bind into the call frame; a reference parameter binds to its argument's storage

    std::vector<script_value> held_args;
    const std::vector<script_value>* argv = &args;
    // A native exception unwinding through this activation (a C++ callable, a removed-element reference, a bare rethrow
    // with nothing to rethrow) pops the frame first and converts at the caller's call boundary
    size_t last_body_idx = 0;
    try {
    for (size_t i = 0; i < function.parameters().size(); ++i) {
        const auto& param = function.parameters()[i];

        if (i >= args.size()) {
            // Must have a default value (validated by argument count check above)
            if (param.default_value) {
                // The default binds as the argument it stands in for: a reference parameter the storage it names, a by-value
                // one its converted copy (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
                script_value default_val = make_value();
                auto storage = param.is_reference ? named_storage_reference(param.default_value.get()) : std::nullopt;
                if (storage) {
                    if (!*storage) {
                        cleanup();
                        return storage->error_value();
                    }
                    default_val = std::move(storage->value());
                } else {
                    auto default_result = dispatch_expr(param.default_value.get());
                    if (!default_result) {
                        cleanup();
                        return default_result.error_value();
                    }
                    default_val = pop_value();
                    if (!is_unwinding_ && param.is_reference) {
                        const expression* d = param.default_value.get();
                        if (d->get_type() == node_type::member_expr && static_cast<const member_expr*>(d)->is_static) {
                            if (auto cell = static_storage_reference(static_cast<const member_expr*>(d))) {
                                if (!*cell) {
                                    cleanup();
                                    return cell->error_value();
                                }
                                default_val = std::move(cell->value());
                            }
                        }
                    } else if (!is_unwinding_) {
                        auto bound = detail::default_argument_value(default_val, param, [&](const script_value& v) {
                            return try_convert_for_parameter(v, param.type, detail::slot_site{detail::slot_kind::parameter, param.name, function.name, false});
                        });
                        if (!bound) {
                            cleanup();
                            return bound.error_value();
                        }
                        default_val = std::move(bound.value());
                    }
                }
                if (!is_unwinding_ && !detail::default_binds_reference(param, default_val)) {
                    cleanup();
                    return detail::non_lvalue_default_error(param, *string_symbolizer_);
                }
                if (param.ref_escaping && !default_val.is_reference()) {
                    default_val = script_value::make_cell_reference(std::move(default_val), engine_);
                }
                call_stack_[frame_index].set_local(param.slot_index, std::move(default_val));
                continue;
            }
        }

        if (param.is_reference) {
            if (const script_value* early = detail::early_bound_reference((*argv)[i])) {
                call_stack_[frame_index].set_local(param.slot_index, script_value(*early));
                continue;
            }
        }
        const auto& arg = detail::argument_for_binding((*argv)[i], detail::parameter_binding(param));

        // Use pre-cached symbol ID (parameter binding optimization)
        // Symbol IDs are cached at function definition time in visit_function_decl
        if (param.is_reference) {
            if (call_ctx && i < call_ctx->arg_exprs->size()) {
                auto bound = bind_reference_parameter(param, frame_index,
                                                      (*call_ctx->arg_exprs)[i].get(), previousEnv, &arg, caller_unit, &function, i);
                if (!bound) {
                    cleanup();
                    return bound.error_value();
                }
            } else {
                // No metadata: external (C++) invocation. Object values are handles, so a
                // shallow copy aliases the same underlying object - reference semantics hold.
                // A reference VALUE is already an lvalue: copying it shares the holder, so
                // the parameter aliases the same storage (parallel_for binds each owned
                // element this way; C++ callers can pass make_*_reference values too).
                auto arg_type = arg.current_type();
                if (arg.is_reference()) {
                    call_stack_[frame_index].set_local(param.slot_index, script_value(arg));
                } else if (arg_type == script_value_type::jai_object_type ||
                    arg_type == script_value_type::jai_shared_ptr_type) {
                    call_stack_[frame_index].set_local(param.slot_index, script_value(arg));
                } else if (detail::is_string_reference_temporary(param, arg) || param.is_const ||
                           (function.name != "<lambda>" &&
                            detail::ref_parameter_takes_temporary(*engine_, function.parameters(), function.body.get(), i, !parallel_worker_))) {
                    call_stack_[frame_index].set_local(param.slot_index, detail::make_reference_argument_temporary(param, arg, engine_));
                } else {
                    cleanup();
                    return checked_result<script_value>(
                        make_error_code(runtime_error_code::invalid_reference),
                        "Cannot pass non-lvalue to reference parameter"
                    );
                }
            }
        } else {
            // Escape-marked value params box into a cell so downstream ref binds share it
            auto boxed_param = [&](script_value&& v) -> script_value {
                if (param.ref_escaping && !v.is_reference()) {
                    return script_value::make_cell_reference(std::move(v), engine_);
                }
                return std::move(v);
            };
            // A prvalue argument (the call site's only holder of its aggregate) is the parameter itself: C++17 guaranteed
            // elision (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
            bool elide = ((call_ctx && call_ctx->values == &args) || owned_args_ == &args) && argv == &args && &arg == &args[i] &&
                         !arg.is_reference() && detail::uniquely_held(arg);
            // the argument stands for its parameter only as C++ allows it (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
            if (elide && call_ctx && call_ctx->values == &args && call_ctx->arg_exprs && i < call_ctx->arg_exprs->size()) {
                elide = detail::source_skips_copy(source_class((*call_ctx->arg_exprs)[i].get()), arg);
            }
            auto bind_elided = [&]() {
                call_stack_[frame_index].set_local(param.slot_index, boxed_param(std::move(const_cast<script_value&>(arg))));
            };

            // Exact-class match: a pointer-identical interned type_info (with the
            // non-empty name the identity branch requires) replicates try_convert's
            // exact-name branch (which returns the derefed arg) without the conversion
            // machinery; should_share is false for objects, so the clone matches the
            // full path
            {
                const script_value& derefed = arg.deref();
                if (param.type && param.type->base_type == script_value_type::jai_object_type &&
                    derefed.current_type() == script_value_type::jai_object_type &&
                    derefed.get_type_info().get() == param.type.get() && !param.type->type_name.empty()) {
                    // Read-only parameter elision: a proven parameter shares the argument's handle
                    // (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
                    if (!param.ref_escaping && detail::parameter_alias_allows(derefed) && detail::readonly_parameter_alias(*engine_, function, i, !parallel_worker_)) {
                        if (!parallel_worker_) { engine_->note_parameter_alias_bind(); }
                        call_stack_[frame_index].set_local(param.slot_index, script_value(derefed));
                        continue;
                    }
                    if (elide) { bind_elided(); continue; }
                    // the copy may run a copy constructor, which may grow call_stack_: the frame is indexed after it
                    script_value shielded(derefed);
                    script_value copied = shielded.clone();
                    call_stack_[frame_index].set_local(param.slot_index, boxed_param(std::move(copied)));
                    continue;
                }
            }

            // Non-reference parameter - try to convert the argument to the parameter type if needed
            // (cleanup before propagating: an early return here would leak the callee frame/env)
            if (argv == &args && i + 1 < args.size() && detail::binding_may_run_code(arg, param.type.get()) &&
                (detail::later_capture_candidate(i + 1, args.size(), [&](size_t j) -> const script_value& { return args[j]; }) ||
                 later_place_argument(function, call_ctx, i + 1, args))) [[unlikely]] {
                held_args = args;
                for (size_t j = i + 1; j < held_args.size(); ++j) {
                    detail::capture_argument(engine_, held_args[j], detail::transient_read_marker::arg_capture_full, detail::parameters_binding(function.parameters(), j));
                }
                if (auto placed = bind_later_places(function, call_ctx, i + 1, held_args, frame_index, previousEnv, caller_unit); !placed) {
                    cleanup();
                    return placed.error_value();
                }
                argv = &held_args;
            }
            // a prvalue argument a converting constructor takes is that constructor's parameter
            auto convert_result = try_convert_for_parameter(arg, param.type,
                detail::slot_site{detail::slot_kind::parameter, param.name, function.name, param.is_reference},
                elide ? &const_cast<script_value&>(arg) : nullptr);
            if (!convert_result) {
                cleanup();
                return convert_result.error_value();
            }
            script_value converted_arg = std::move(convert_result.value());

            // Decide between value semantics (clone) or reference semantics (share).
            // Element-read args arrive as reference wrappers - probe the referent
            // (deref() returns *this for plain values) so the shared_ptr marker is
            // visible (KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
            bool should_share = false;
            const script_value& shared_probe = converted_arg.deref();

            // Check if parameter type is shared_ptr<T>
            if (param.type && param.type->base_type == script_value_type::jai_shared_ptr_type) {
                should_share = true;
            }

            // Check if argument is shared_ptr<T> (preserve reference semantics)
            if (shared_probe.get_type_info() && shared_probe.get_type_info()->base_type == script_value_type::jai_shared_ptr_type) {
                should_share = true;
            }

            // Add parameter to call frame using slot index (O(1) access)
            // IMPORTANT: Use call_stack_[frame_index] not a cached reference (vector may have reallocated)
            if (should_share) {
                // Shallow copy - share ownership (reference semantics)
                call_stack_[frame_index].set_local(param.slot_index, boxed_param(script_value(shared_probe)));
            } else {
                // Deep copy - value semantics (C++-like default); a proven read-only parameter
                // keeps the converted value as is (an identity conversion shares the argument,
                // a converting one is already a fresh value - neither needs the clone)
                const bool heavy = shared_probe.is_object() || shared_probe.is_array() || shared_probe.is_map();
                if (heavy && !param.ref_escaping && detail::parameter_alias_allows(shared_probe) && detail::readonly_parameter_alias(*engine_, function, i, !parallel_worker_)) {
                    if (!parallel_worker_) { engine_->note_parameter_alias_bind(); }
                    call_stack_[frame_index].set_local(param.slot_index, script_value(shared_probe));
                    continue;
                }
                if (elide && detail::same_heavy_node(shared_probe, arg)) { bind_elided(); continue; }
                // a converting constructor's result is a temporary nothing else holds: it is the parameter
                if (detail::converted_temporary(converted_arg, arg)) {
                    call_stack_[frame_index].set_local(param.slot_index, boxed_param(std::move(converted_arg)));
                    continue;
                }
                script_value copied = converted_arg.clone();   // a copy constructor may grow call_stack_: the frame is indexed after it
                call_stack_[frame_index].set_local(param.slot_index, boxed_param(std::move(copied)));
            }
        }
    }
    // A temporary a by-value parameter was copied from dies once the parameters are bound, as the vm's argument window lets it go
    if (call_ctx && call_ctx->values == &args && argv == &args && !is_unwinding_) {
        for (size_t i = 0; i < args.size() && i < function.parameters().size(); ++i) {
            const script_value& arg = args[i];
            if (!function.parameters()[i].is_reference && !arg.is_reference() && detail::uniquely_held(arg) && detail::value_copies_observably(arg)) {
                script_value dying = std::move(const_cast<script_value&>(arg));
            }
        }
    }
    // A T& parameter binds a T, judged on the bound lvalue so a non-lvalue argument keeps its own text (KEEP BYTE-PARALLEL with
    // vm_backend::bind_parameters)
    for (size_t i = 0; i < function.parameters().size() && !is_unwinding_; ++i) {
        const auto& param = function.parameters()[i];
        if (!param.is_reference) {
            if (param.declared_var()) {
                if (script_value* bound = call_stack_[frame_index].get_local(param.slot_index)) { detail::var_parameter_stamp(*bound, param.type, engine_); }
            }
            continue;
        }
        if (!param.type) { continue; }
        const script_value* bound = call_stack_[frame_index].get_local(param.slot_index);
        if (bound && !detail::reference_binding_admits(param.type.get(), *bound, engine_)) {
            // a const T& binds a T made from the argument, as a T parameter converts it (C++; KEEP BYTE-PARALLEL with vm_backend::bind_parameters)
            if (param.is_const) {
                auto converted = try_convert_for_parameter(bound->deref(), param.type,
                    detail::slot_site{detail::slot_kind::parameter, param.name, function.name, false}, nullptr);
                if (converted) {
                    call_stack_[frame_index].set_local(param.slot_index, script_value::make_cell_reference(std::move(converted.value()), engine_));
                    continue;
                }
            }
            auto rejection = detail::typed_slot_rejection(bound->deref(), *param.type, detail::slot_site{detail::slot_kind::parameter, param.name, function.name, param.is_reference}, engine_);
            cleanup();
            return rejection;
        }
        if ((param.ref_escaping || param.declared_ref_escapes) && bound) [[unlikely]] {   // its reference reaches a binding that cannot see the name
            detail::ref_declare(*const_cast<script_value*>(bound), const_cast<type_info*>(detail::parameter_declared_ref(param)), engine_);
        }
    }

    // a constructor's activation runs its prologue (bases, then fields) in this frame, the parameters bound, ahead of its body
    bool run_body = true;
    if (pending_prologue_ && pending_prologue_->frame == frame_index && pending_prologue_->body == function.body.get()) [[unlikely]] {
        constructor_prologue_hook* hook = std::exchange(pending_prologue_, nullptr);
        auto ran = run_constructor_prologue(*hook, frame_index, function);
        if (!ran) {
            if (!trace_captured_) capture_stack_trace();
            cleanup();
            return ran.error_value();
        }
        run_body = !is_unwinding_;
    }

    // Execute function body without creating another environment
    // (since we already created one for the function call)
    // Track which declaration index we're at (needed for coroutine yield)
    const size_t body_stack_mark = valueStack_.size();
    for (size_t body_idx = 0; run_body && body_idx < function.body->declarations.size(); ++body_idx) {
        last_body_idx = body_idx;
        const auto& decl = function.body->declarations[body_idx];
        auto result = dispatch_decl(decl.get());
        while (valueStack_.size() > body_stack_mark) { (void)valueStack_.pop(); }   // a statement's value dies at its end (visit_block_stmt parity)

        // Check for error codes - propagate errors
        if (!result && result.error() != std::error_code()) {
            if (!trace_captured_) capture_stack_trace();
            cleanup();
            // Propagate the error with static message - nice formatting at API boundary
            return result.error_value();
        }

        // Check if we hit a return statement, yield, or a script throw and break early.
        // Without the is_unwinding_ check, statements after a top-level `throw` in the
        // function body would still execute (blocks check the flag; this loop must too).
        if (hasReturnValue_ || hasYieldRequest_ || is_unwinding_) {
            break;
        }
    }
    } catch (...) {
        if (call_stack_.size() > frame_index) {
            while (call_stack_.size() > frame_index + 1) { call_stack_.pop_back(); }   // frames an unwound callee left behind
            cleanup();
        }
        throw;
    }

    // Get return value
    script_value result = make_value();

    // If yielding, save state for coroutine resume instead of cleaning up. The flag is this
    // frame's suspension only when the body running is the coroutine's own (a callee can no
    // longer be entered with it set - the yield unwinds its expression first).
    if (hasYieldRequest_ && active_coroutine_ &&
        active_coroutine_->get_function() && active_coroutine_->get_function()->body == function.body) {
        {
            result = active_coroutine_->last_value();
            auto& coro_state = coroutine_state(*active_coroutine_, engine_);
            // Record where to resume in the function body.
            // If inner constructs pushed continuations, re-enter this statement.
            // If no inner continuations, the yield was a direct child, skip it.
            size_t resume_idx = coro_state.has_continuations() ? last_body_idx : last_body_idx + 1;
            coro_state.push_continuation(function.body.get(), resume_idx);
            // Save interpreter state into the coroutine for later resume
            // The visit_* methods have already pushed continuations onto the coroutine
            // and left the environment chain intact (no releases on yield path).
            // Only the coroutine's OWN frames move into the suspended state - a resume
            // made inside a running script function must leave the caller's frames on
            // the live stack, or the rest of the caller's body executes frameless.
            coro_state.saved_environment = environment_;
            coro_state.saved_call_stack.clear();
            coro_state.saved_call_stack.reserve(call_stack_.size() - frame_index);
            for (size_t fi = frame_index; fi < call_stack_.size(); ++fi) {
                coro_state.saved_call_stack.push_back(std::move(call_stack_[fi]));
            }
            coro_state.saved_return_value = std::move(returnValue_);
            coro_state.saved_has_return = hasReturnValue_;
            coro_state.saved_member_scope = member_scope_;
        }
        call_stack_.erase(call_stack_.begin() + frame_index, call_stack_.end());
        // Restore caller's environment WITHOUT releasing
        // (the coroutine now owns its environment chain and call frames)
        environment_ = previousEnv;
        member_scope_ = previousScope;
        hasReturnValue_ = previousHasReturn;
        returnValue_ = previousReturn;
        return result;
    }

    // The return site (visit_return_stmt) validated a reference return / materialized the
    // by-value result (own slot moved, lvalue reads copied) and ran the typed conversion
    // inside the callee's try regions; a stray reference still flattens BEFORE cleanup
    // destroys the call frame
    if (hasReturnValue_ && !is_unwinding_) {
        if (function.return_type && function.return_type->base_type == script_value_type::jai_reference_type) {
            result = std::move(returnValue_.value());
        } else if (returnValue_.value().is_reference()) {
            // Copy the target - can't move since target might be in outer scope
            result = returnValue_.value().deref();
        } else {
            result = std::move(returnValue_.value());
        }
    } else {
        // Check if this is a constructor (method with no explicit return)
        // Constructors implicitly return 'this'
        if (active_coroutine_ && active_coroutine_->get_function() &&
            active_coroutine_->get_function()->body == function.body) {
            // Coroutine body activation ran off the end: completes with null (vm
            // run_fiber parity) - the implicit-'this' method rule must not apply.
            result = make_value();
        } else if (call_stack_[frame_index].is_method) {
            result = call_stack_[frame_index].get_this();
        } else {
            // Check environment for method context (for closures inside methods)
            auto function_env = environment_;
            if (function_env->is_method_env()) {
                result = function_env->get_this_object();
            } else if (function_env->get_parent() && function_env->get_parent()->is_method_env()) {
                result = function_env->get_parent()->get_this_object();
            } else {
                // Regular function with no return statement returns null
                result = make_value();
            }
        }
    }

    // Cleanup and return success
    cleanup();
    return result;
}

// The one typed-slot table (detail::convert_for_typed_slot); this site owns the parameter /
// return texts and the identity return shapes (the arg itself; the derefed arg for exact-name
// objects - the I-C1 lane and the exact-class lane replicate them).
// KEEP BYTE-PARALLEL with vm_backend::try_convert_for_parameter.
checked_result<script_value> interpreter::try_convert_for_parameter(const script_value& arg, type_info_ptr target_type,
                                                                    const detail::slot_site& site, script_value* consumable) {
    if (!target_type) {
        return arg;  // No type specified = any type accepted
    }

    // deref() follows chains to a non-reference or throws - no residual-ref case
    const script_value& derefed_arg = arg.deref();
    const auto target_base_type = target_type->base_type;
    if (target_base_type == script_value_type::jai_any_type) return arg;
    switch (derefed_arg.raw_storage_index()) {   // same-storage scalars and handles bind as-is (the table's identity / handle rows)
        case script_value::TYPEID_INT: if (target_base_type == script_value_type::jai_int_type) return arg; break;
        case script_value::TYPEID_FLOAT: if (target_base_type == script_value_type::jai_float_type) return arg; break;
        case script_value::TYPEID_STRING: if (target_base_type == script_value_type::jai_string_type) return arg; break;
        case script_value::TYPEID_BOOL: if (target_base_type == script_value_type::jai_bool_type) return arg; break;
        case script_value::TYPEID_CHAR: if (target_base_type == script_value_type::jai_char_type) return arg; break;
        case script_value::TYPEID_WEAK_PTR: if (target_base_type == script_value_type::jai_weak_ptr_type) return arg; break;
        default: break;
    }

    auto outcome = detail::convert_for_typed_slot(arg, target_type, engine_, consumable);
    if (!outcome) {
        return outcome.error_value();
    }
    using verdict = detail::slot_conversion::verdict;
    detail::slot_conversion& out = outcome.value();
    switch (out.v) {
        case verdict::identity:
            return arg;
        case verdict::identity_exact:
            return derefed_arg;
        case verdict::converted:
            return std::move(*out.value);
        case verdict::container_mismatch: {
            auto* sym = engine_->get_symbolizer();
            return checked_result<script_value>(
                make_error_code(runtime_error_code::array_element_type_mismatch),
                detail::container_boundary_mismatch_text,
                sym->intern(out.mismatch->first), sym->intern(out.mismatch->second));
        }
        case verdict::handle_target: {
            // shared_ptr<T> / weak_ptr<T> parameters and returns (outside the table): a same-kind
            // payload, a matching or derived handle binds as-is
            if (derefed_arg.current_type() == target_base_type) {
                return arg;
            }
            auto source_type_info = derefed_arg.get_type_info();
            if (source_type_info && source_type_info->base_type == script_value_type::jai_shared_ptr_type &&
                target_base_type == script_value_type::jai_shared_ptr_type &&
                detail::shared_handle_admits(derefed_arg, target_type, engine_)) {
                // handle admission: the parameter or return value has the declared static type (KEEP BYTE-PARALLEL with vm_backend)
                if (arg.is_reference() || source_type_info.get() == target_type.get()) { return arg; }
                script_value bound = arg;
                detail::keep_handle_slot_type(bound, target_type);
                return bound;
            }
            break;
        }
        default:
            break;
    }

    if (target_base_type == script_value_type::jai_object_type && !target_type->type_name.empty() &&
        (out.v == verdict::reject || out.v == verdict::reject_class)) {
        return detail::no_converting_constructor_error(string_symbolizer_, derefed_arg, *target_type, engine_);
    }
    return detail::typed_slot_rejection(derefed_arg, *target_type, site, engine_);
}

interpreter_coroutine_state::~interpreter_coroutine_state() {
    // a suspended loop's interior (its variable's slot and the slots above it) dies before its range, the innermost loop first
    // (KEEP BYTE-PARALLEL with vm_backend::vm_coroutine_state)
    if (saved_call_stack.size() == 1) {
        auto& locals = saved_call_stack.back().locals;
        for (continuation_point& point : continuations_) {
            if (!point.saved_container || !point.node || point.node->get_type() != node_type::range_for_stmt) { continue; }
            const size_t first = static_cast<const range_for_stmt*>(point.node)->variable_slot_index;
            if (first == SIZE_MAX) { continue; }
            for (size_t at = locals.size(); at-- > first;) {
                if (!locals[at].is_reference()) { script_value dying = std::move(locals[at]); }
            }
            point.saved_container.reset();
        }
    }
    drop_saved_containers(interpreter::rp_catch);
    parked_exception_value.reset();
    std::vector<script_value> shared;
    for (auto& frame : saved_call_stack) {
        if (frame.boxed_cells || (frame.function && frame.function->body && frame.function->body->shares_cells)) {
            script_value::gather_shared_cells(frame.locals.data(), frame.locals.size(), shared);
        }
    }
    if (eng) {
        for (size_t f = saved_call_stack.size(); f-- > 0;) { detail::engine_access::release_suspended_values(*eng, saved_call_stack[f].locals.data(), saved_call_stack[f].locals.size()); }
    }
    if (shared.empty()) { return; }
    saved_call_stack.clear();
    script_value::release_cell_cycles(shared, eng);
}

script_value interpreter::make_coroutine_object(engine* eng, uint64_t type_id, std::shared_ptr<coroutine_handle> handle) {
    return script_value::make_coroutine_handle(type_id, std::static_pointer_cast<void>(handle), eng);
}

checked_result<script_value> interpreter::resume_coroutine(coroutine_handle& handle) {
    arm_native_stack();
    // Host-level resume is its own budgeted entry; a resume nested inside an
    // executing script keeps the outer deadline (and the outer limit state).
    // Depth alone is not enough: a TOP-LEVEL script statement calling next() also
    // runs at call depth 0, and re-arming there let `for(..){ h.next(); }` push the
    // deadline forever (fuzz livelock seeds 3507/8285/1213/8356/8391).
    if (current_call_depth_ == 0 && execute_depth_ == 0 && !handle.continues_host_entry()) {
        arm_execution_deadline();
        limits_->reset_for_execute();
    }

    // Check before activating or moving any coroutine state. A first resume is
    // counted by call_function; continuation replay below owns its own guard.
    if (current_call_depth_ >= limits_->interpreter_call_depth_limit) {
        return detail::raise_call_depth();
    }
    if (detail::native_stack_low()) [[unlikely]] {
        return detail::raise_native_stack();
    }
    // the body resolves in the unit that declared the coroutine on every resume (continuation replay runs it without call_function)
    auto declared = handle.get_function();
    const unit_stamp replay_unit = declared ? declared->unit : current_unit();
    unit_override_scope unitScope(this, &replay_unit);

    auto& state = coroutine_state(handle, engine_);

    coroutine_handle* prev_coroutine = active_coroutine_;
    bool prev_yield_request = hasYieldRequest_;
    active_coroutine_ = &handle;
    hasYieldRequest_ = false;

    auto prev_status = handle.get_status();
    handle.set_status(coroutine_handle::status::running);
    if (prev_status == coroutine_handle::status::created) { limits_->memory_charge_deferred(detail::k_coroutine_state_charge); }

    // Every exit hands the resumer back its coroutine and yield request; a C++ exception escaping the body fails the
    // handle (vm run_fiber parity). Declared ahead of the exception swap, so it runs after it.
    struct resume_exit {
        interpreter* in;
        coroutine_handle& handle;
        coroutine_handle* prev_coroutine;
        bool prev_yield_request;
        int uncaught = std::uncaught_exceptions();
        ~resume_exit() {
            if (std::uncaught_exceptions() > uncaught) { handle.set_status(coroutine_handle::status::failed); }
            in->active_coroutine_ = prev_coroutine;
            in->hasYieldRequest_ = prev_yield_request;
        }
    } exit_guard{this, handle, prev_coroutine, prev_yield_request};

    // The exception trio follows the frames (KEEP BYTE-PARALLEL with vm_backend::run_fiber's
    // fiber_exception_swap): the resumer's is saved here and comes back on every exit (a
    // throw escaping the body keeps its exception pair live for the resumer's catch); a
    // suspend parks the body's own for the next resume. A fresh coroutine starts with no
    // active exception.
    struct coroutine_exception_swap {
        interpreter* in;
        interpreter_coroutine_state* st;
        coroutine_handle* handle;
        std::optional<script_exception> resumer_exception;
        std::optional<script_value> resumer_exception_value;
        uint64_t resumer_marker;
        coroutine_exception_swap(interpreter* i, interpreter_coroutine_state* s, coroutine_handle* h, bool fresh)
            : in(i), st(s), handle(h), resumer_exception(std::move(i->current_exception_)),
              resumer_exception_value(std::move(i->active_exception_value_)), resumer_marker(i->current_catch_var_id_) {
            if (fresh) {
                in->current_exception_.reset();
                in->active_exception_value_.reset();
                in->current_catch_var_id_ = 0;
            } else {
                in->current_exception_ = std::move(st->parked_exception);
                in->active_exception_value_ = std::move(st->parked_exception_value);
                in->current_catch_var_id_ = st->parked_catch_marker;
            }
        }
        ~coroutine_exception_swap() {
            const bool park = handle->get_status() == coroutine_handle::status::suspended;
            if (park) {
                st->parked_exception = std::move(in->current_exception_);
                st->parked_exception_value = std::move(in->active_exception_value_);
                st->parked_catch_marker = in->current_catch_var_id_;
            }
            in->current_catch_var_id_ = resumer_marker;
            if (park || !in->is_unwinding_) {
                in->current_exception_ = std::move(resumer_exception);
                in->active_exception_value_ = std::move(resumer_exception_value);
            }
        }
    } exception_guard(this, &state, &handle, prev_status == coroutine_handle::status::created);

    // A runtime error in the (re)started segment must reach the caller as an error;
    // falling through to `return handle.yield_value_` would hand back the PREVIOUS
    // yield as a bogus success and silently swallow the failure.
    std::optional<checked_result<script_value>> error_result;

    auto function = handle.get_function();

    if (prev_status == coroutine_handle::status::created) {
        // On yield, call_function saves environment/call_stack into this coroutine
        // (via active_coroutine_) and returns the yield value WITHOUT running cleanup;
        // on normal return it cleans up and returns the result.
        // Coroutine parameters always bind by VALUE (VM parity): resume runs far from
        // the creation-site lvalues, so reference binding is meaningless here.
        std::vector<parameter> value_params = function->parameters;
        for (auto& p : value_params) { p.is_reference = false; }
        // Owned by the coroutine state, not this stack: the saved frames point at it
        // across every later resume (return-site conversion, debugger locals)
        state.function.emplace(
            function->name,
            value_params,
            function->return_type,
            function->body,
            handle.get_closure_env(),
            function->local_count
        );
        state.function->unit = function->unit;

        // A first resume nested inside another call's argument list must not see that
        // call's in-flight arg metadata (a zero-arg .next() never saves/clears it)
        // the handle's captured arguments are owned: a prvalue one moves into its parameter, and the rest go once bound (KEEP
        // BYTE-PARALLEL with vm_backend::run_fiber)
        std::vector<script_value>& args = handle.binding_args();
        std::vector<script_value>* previous_owned = exchange_owned_arguments(&args);
        auto call_result = call_function(*state.function, args);
        exchange_owned_arguments(previous_owned);
        handle.release_args();

        if (hasYieldRequest_) {
            // call_function already saved state into this coroutine
            // (saved_environment, saved_call_stack, etc.)
            handle.set_status(coroutine_handle::status::suspended);
        } else if (!call_result) {
            handle.set_status(coroutine_handle::status::failed);
            error_result.emplace(std::move(call_result));
        } else if (is_unwinding_) {
            // Script throw escaped the body: the handle is failed, the throw itself
            // keeps propagating to the caller through the unwinding flag
            handle.set_status(coroutine_handle::status::failed);
        } else {
            handle.yield_value_ = std::move(call_result.value());
            handle.complete(handle.yield_value_);
        }

    } else {
        detail::call_depth_guard depth_guard(current_call_depth_);
        // Detached frames die LAST (after the caller state is restored): releasing their
        // locals can fire destructors that reenter call_function
        detail::shared_cell_release dying_cells{engine_};   // the cells the finished frames shared, checked after they die
        std::vector<call_frame> dying_frames;
        struct gather_dying_cells {
            std::vector<call_frame>& frames;
            std::vector<script_value>& cells;
            ~gather_dying_cells() {
                for (auto& frame : frames) {
                    if (frame.boxed_cells || (frame.function && frame.function->body && frame.function->body->shares_cells)) {
                        script_value::gather_shared_cells(frame.locals.data(), frame.locals.size(), cells);
                    }
                }
            }
        } gather_dying{dying_frames, dying_cells.cells};
        struct caller_state_restore {
            interpreter* in;
            std::shared_ptr<environment> env;
            std::optional<script_value> return_value;
            bool has_return;
            detail::member_scope_ctx member_scope;
            ~caller_state_restore() {
                in->environment_ = std::move(env);
                in->returnValue_ = std::move(return_value);
                in->hasReturnValue_ = has_return;
                in->member_scope_ = member_scope;
            }
        } caller_state{this, environment_, std::move(returnValue_), hasReturnValue_, member_scope_};

        // The coroutine's frames go ABOVE the resumer's (vm frames_ parity): a trace taken
        // inside the segment names the whole resume chain
        const size_t frame_base = call_stack_.size();
        for (auto& saved_frame : state.saved_call_stack) {
            call_stack_.push_back(std::move(saved_frame));
        }
        state.saved_call_stack.clear();
        auto detach_frames = [&](std::vector<call_frame>& into) {
            into.assign(std::make_move_iterator(call_stack_.begin() + frame_base),
                        std::make_move_iterator(call_stack_.end()));
            call_stack_.erase(call_stack_.begin() + frame_base, call_stack_.end());
        };
        // A C++ exception escaping the segment takes the coroutine's frames down with it, ahead of the caller state
        struct frames_on_throw {
            interpreter* in;
            size_t base;
            std::vector<call_frame>& dying;
            int uncaught = std::uncaught_exceptions();
            ~frames_on_throw() {
                if (std::uncaught_exceptions() > uncaught && in->call_stack_.size() > base) {
                    dying.assign(std::make_move_iterator(in->call_stack_.begin() + base), std::make_move_iterator(in->call_stack_.end()));
                    in->call_stack_.erase(in->call_stack_.begin() + base, in->call_stack_.end());
                }
            }
        } frames_guard{this, frame_base, dying_frames};

        environment_ = state.saved_environment;
        returnValue_ = std::move(state.saved_return_value);
        hasReturnValue_ = state.saved_has_return;
        member_scope_ = state.saved_member_scope;

        // The continuations are consumed LIFO (outermost first).
        // The outermost continuation is for the function body (call_function's loop).
        // Pop it to get the statement index where yield occurred.
        auto* body_cont = state.peek_continuation(function->body.get());
        size_t start_index = 0;
        if (body_cont) {
            start_index = body_cont->index;
            state.pop_continuation();
        }
        bool had_error = false;
        size_t last_body_idx = start_index;

        for (size_t i = start_index; i < function->body->declarations.size(); ++i) {
            last_body_idx = i;
            // For the first statement (start_index), the inner continuations
            // are still on the stack and will be consumed by visit_block_stmt,
            // visit_if_stmt, visit_while_stmt, etc. to fast-forward to the
            // exact point where yield occurred.
            checked_result<void> decl_result = i == start_index
                ? dispatch_decl_resuming(function->body->declarations[i].get())
                : dispatch_decl(function->body->declarations[i].get());
            // Statement boundary (visit_block_stmt parity): partial operands never
            // accumulate across the resumed body
            if (valueStack_.size() > 0) {
                valueStack_.clear();
            }

            if (!decl_result) {
                if (decl_result.error() != std::error_code()) {
                    if (!trace_captured_) capture_stack_trace();
                    handle.set_status(coroutine_handle::status::failed);
                    had_error = true;
                    error_result.emplace(decl_result.error_value());
                    break;
                }
            }

            if (hasReturnValue_ || hasYieldRequest_ || is_unwinding_) {
                break;
            }
        }

        if (had_error) {
            detach_frames(dying_frames);
        } else {
            if (is_unwinding_) {
                // Script throw escaped the resumed segment: fail the handle; the throw
                // keeps propagating to the caller through the unwinding flag
                if (!trace_captured_) capture_stack_trace();
                handle.set_status(coroutine_handle::status::failed);
                state.saved_environment.reset();
                detach_frames(dying_frames);
            } else if (hasYieldRequest_) {
                // Record where to resume in the function body.
                // If inner constructs pushed continuations, re-enter this statement.
                // If no inner continuations, the yield was a direct child, skip it.
                size_t resume_idx = state.has_continuations() ? last_body_idx : last_body_idx + 1;
                state.push_continuation(function->body.get(), resume_idx);
                state.saved_environment = environment_;
                detach_frames(state.saved_call_stack);
                state.saved_return_value = std::move(returnValue_);
                state.saved_has_return = hasReturnValue_;
                state.saved_member_scope = member_scope_;
                handle.set_status(coroutine_handle::status::suspended);
            } else if (hasReturnValue_) {
                if (returnValue_) {
                    // call_function epilogue parity: the return site already converted; deref
                    handle.yield_value_ = returnValue_.value().is_reference()
                        ? returnValue_.value().deref()
                        : std::move(returnValue_.value());
                }
                handle.complete(returnValue_ ? handle.yield_value_ : make_value());
                state.saved_environment.reset();
                detach_frames(dying_frames);
            } else {
                // Ran past the final yield: completion yields null (vm run_fiber parity),
                // never a replay of the stale last-yielded value.
                handle.yield_value_ = make_value();
                handle.complete(handle.yield_value_);
                state.saved_environment.reset();
                detach_frames(dying_frames);
            }
        }
    }

    if (error_result) {
        return std::move(*error_result);
    }
    return handle.yield_value_;
}


std::shared_ptr<environment> interpreter::block_environment(std::shared_ptr<environment> parent) {
    auto env = std::make_shared<environment>(std::move(parent), env_symbolizer_);
    env->set_block_scope(true);
    return env;
}

std::shared_ptr<environment> interpreter::method_environment(std::shared_ptr<environment> parent, script_value this_obj, class_definition* access_ctx) {
    auto env = std::make_shared<environment>(std::move(parent), env_symbolizer_, std::move(this_obj));
    env->set_access_context(access_ctx);
    return env;
}

// A scope that ends: its values die now, in the order a block's locals die (an escaped scope, a suspended coroutine's, is never
// ended here)
void interpreter::end_scope(const std::shared_ptr<environment>& env) {
    if (env) { env->reset(nullptr); }
}

// A suspendable run's execution state (vm_backend::vm_run_state's twin): swapped whole with the live members at every switch,
// so a suspended run keeps its native frames' interpreter state while another entry runs
struct interpreter::interpreter_run_state : backend_run_state {
    std::shared_ptr<jai::environment> env;
    std::vector<std::shared_ptr<environment>> instance_scope_saved;
    std::shared_ptr<environment> pending_instance_env;
    detail::member_scope_ctx member_scope{};
    const detail::call_site_context* pending_call_ctx = nullptr;
    std::vector<const detail::call_site_context*> external_ctx_stack;
    const std::vector<script_value>* owned_args = nullptr;
    script_valueStack values;
    std::optional<script_value> return_value;
    bool has_return_value = false;
    bool has_break_request = false;
    bool has_continue_request = false;
    int execute_depth = 0;
    coroutine_handle* active_coroutine = nullptr;
    bool has_yield_request = false;
    std::optional<script_exception> current_exception;
    bool is_unwinding = false;
    std::optional<script_value> active_exception_value;
    std::vector<stack_trace_entry> captured_trace;
    bool trace_captured = false;
    const ast_node* top_level_node = nullptr;
    std::optional<error_propagator> run_error;
    detail::nested_failure nested_failure;
    uint64_t current_catch_var_id = 0;
    bool in_switch = false;
    bool should_fallthrough = false;
    bool lvalue_target_context = false;
    bool string_target_context = false;
    int current_call_depth = 0;
    std::chrono::steady_clock::time_point execution_deadline{};
    uint32_t budget_tick = 0;
    bool budget_active = false;
    std::vector<call_frame> call_stack;
    bool identifier_callee_lookup = false;
    bool member_callee_lookup = false;
    bool expression_result_needed = true;
    const expression* unconsumed_root = nullptr;
    const ast_node* debug_quiet_node = nullptr;
    script_value current_method_this;
    const unit_stamp* unit_override = nullptr;
    size_t unit_override_depth = 0;
    unit_stamp run_unit{};
    size_t structural_equality_depth = 0;
    std::vector<construction_trace_frame> construction_traces;
    constructor_prologue_hook* pending_prologue = nullptr;
    size_t prologue_frame = SIZE_MAX;
    detail::native_stack_mark stack_mark;   // a run's walk stops near its fiber's stack end, the host's near the host thread's
};

std::unique_ptr<backend_run_state> interpreter::make_run_state() {
    auto state = std::make_unique<interpreter_run_state>();
    state->env = get_global_environment();
    return state;
}

void interpreter::swap_run_state(backend_run_state& a_state) noexcept {
    auto& s = static_cast<interpreter_run_state&>(a_state);
    using std::swap;
    swap(environment_, s.env);
    swap(instance_scope_saved_, s.instance_scope_saved);
    swap(pending_instance_env_, s.pending_instance_env);
    swap(member_scope_, s.member_scope);
    swap(pending_call_ctx_, s.pending_call_ctx);
    swap(owned_args_, s.owned_args);
    swap(external_ctx_stack_, s.external_ctx_stack);
    valueStack_.swap(s.values);
    swap(returnValue_, s.return_value);
    swap(hasReturnValue_, s.has_return_value);
    swap(hasBreakRequest_, s.has_break_request);
    swap(hasContinueRequest_, s.has_continue_request);
    swap(execute_depth_, s.execute_depth);
    swap(active_coroutine_, s.active_coroutine);
    swap(hasYieldRequest_, s.has_yield_request);
    swap(current_exception_, s.current_exception);
    swap(is_unwinding_, s.is_unwinding);
    swap(active_exception_value_, s.active_exception_value);
    swap(captured_trace_, s.captured_trace);
    swap(trace_captured_, s.trace_captured);
    swap(top_level_node_, s.top_level_node);
    swap(run_error_, s.run_error);
    swap(nested_failure_, s.nested_failure);
    swap(current_catch_var_id_, s.current_catch_var_id);
    swap(in_switch_, s.in_switch);
    swap(should_fallthrough_, s.should_fallthrough);
    swap(lvalue_target_context_, s.lvalue_target_context);
    swap(string_target_context_, s.string_target_context);
    swap(current_call_depth_, s.current_call_depth);
    swap(execution_deadline_, s.execution_deadline);
    swap(budget_tick_, s.budget_tick);
    swap(budget_active_, s.budget_active);
    swap(call_stack_, s.call_stack);
    swap(identifier_callee_lookup_, s.identifier_callee_lookup);
    swap(member_callee_lookup_, s.member_callee_lookup);
    swap(expression_result_needed_, s.expression_result_needed);
    swap(unconsumed_root_, s.unconsumed_root);
    swap(debug_quiet_node_, s.debug_quiet_node);
    swap(current_method_this_, s.current_method_this);
    swap(unit_override_, s.unit_override);
    swap(unit_override_depth_, s.unit_override_depth);
    swap(run_unit_, s.run_unit);
    swap(structural_equality_depth_, s.structural_equality_depth);
    swap(construction_traces_, s.construction_traces);
    swap(pending_prologue_, s.pending_prologue);
    swap(prologue_frame_, s.prologue_frame);
    swap(stack_mark_, s.stack_mark);
    sync_unit();
}

void interpreter::reset_run_state(backend_run_state& a_state) {
    auto& s = static_cast<interpreter_run_state&>(a_state);
    s.env = get_global_environment();
    s.instance_scope_saved.clear();
    s.pending_instance_env.reset();
    s.member_scope = {};
    s.pending_call_ctx = nullptr;
    s.owned_args = nullptr;
    s.external_ctx_stack.clear();
    s.values.clear();
    s.return_value.reset();
    s.has_return_value = s.has_break_request = s.has_continue_request = false;
    s.execute_depth = 0;
    s.active_coroutine = nullptr;
    s.has_yield_request = false;
    s.current_exception.reset();
    s.is_unwinding = false;
    s.active_exception_value.reset();
    s.captured_trace.clear();
    s.trace_captured = false;
    s.top_level_node = nullptr;
    s.run_error.reset();
    s.nested_failure = detail::nested_failure{};
    s.current_catch_var_id = 0;
    s.in_switch = s.should_fallthrough = s.lvalue_target_context = s.string_target_context = false;
    s.current_call_depth = 0;
    s.budget_tick = 0;
    s.budget_active = false;
    s.call_stack.clear();
    s.identifier_callee_lookup = s.member_callee_lookup = false;
    s.expression_result_needed = true;
    s.unconsumed_root = nullptr;
    s.debug_quiet_node = nullptr;
    s.current_method_this = script_value();
    s.unit_override = nullptr;
    s.unit_override_depth = 0;
    s.run_unit = {};
    s.structural_equality_depth = 0;
    s.construction_traces.clear();
    s.pending_prologue = nullptr;
    s.prologue_frame = SIZE_MAX;
}

void interpreter::adopt_run_trace(backend_run_state& a_state) {
    auto& s = static_cast<interpreter_run_state&>(a_state);
    captured_trace_ = std::move(s.captured_trace);
    trace_captured_ = s.trace_captured;
    s.captured_trace.clear();
}

checked_result<void> interpreter::visit_enum_decl(enum_decl* decl) {
    auto enum_map = script_value::make_map(
        engine_->get_type_info_string(),
        engine_->get_type_info_int(),
        engine_);
    auto& map_ref = const_cast<script_map&>(enum_map.as_map());

    for (size_t i = 0; i < decl->values.size(); ++i) {
        auto key = script_value(std::string(decl->values[i].first), engine_);
        map_ref[std::move(key)] = make_value(static_cast<script_int>(decl->numbers[i]));
    }
    map_ref.mark_enum_values();

    environment_->define(decl->name_id, std::move(enum_map));
    engine_->note_enum_name(decl->name_id);
    if (environment_->is_program_scope()) { engine_->mark_const_cell(environment_->get_env_var_ptr(decl->name_id), true); }   // vm parity: exec_enum_decl
    return {};
}

// extern declares nothing: the name resolves to whatever defines it, and a use while nothing does raises the
// extern's own text. KEEP BYTE-PARALLEL with vm_backend::exec_extern_decl.
checked_result<void> interpreter::visit_extern_decl(extern_decl* decl) {
    environment_->note_extern(decl->name_id, decl->is_function);
    if (decl->written_at && decl->written().find("::") == std::string_view::npos) { environment_->note_extern(string_symbolizer_->intern(decl->written()), decl->is_function); }   // the namespace's own blocks read it bare
    return {};
}

template<bool Replaying>
checked_result<void> interpreter::visit_destructuring_decl_t(destructuring_decl* decl) {
    JAISCRIPT_TRY_ROOT(decl, decl->initializer.get());
    script_value source = pop_value();

    if (!source.is_array()) {
        return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
            "Destructuring requires an array on the right-hand side");
    }

    const auto& node = *get_array_storage(source);

    for (size_t i = 0; i < decl->names.size(); ++i) {
        script_value val = i < node.size()
            ? (node.is_typed() ? node.get(i, engine_) : clone_for_assignment(node.values()[i]))
            : make_value();

        if (decl->slot_indices[i] != SIZE_MAX && !call_stack_.empty()) {
            call_stack_.back().set_local(decl->slot_indices[i], std::move(val));
        } else if (!environment_->define_variable(decl->names[i].second, std::move(val))) [[unlikely]] {
            return detail::inline_redefinition(*string_symbolizer_, decl->names[i].second, nullptr);
        }
    }

    return {};
}

void interpreter::park_operands(ast_node* node, uint32_t phase, size_t index, std::vector<script_value> partial) {
    if (active_coroutine_) {
        coroutine_state(*active_coroutine_, engine_).push_replay(node, phase, index, std::move(partial));
    }
}

// A statement whose expression suspended: it owns the replay root, so the enclosing block
// re-enters THIS statement on resume (has_continuations() is true) and hands it to
// dispatch_decl_resuming. A bare `yield x;` parks nothing - the block skips it as before.
void interpreter::note_statement_yield(ast_node* stmt, expression* root) {
    if (!active_coroutine_) return;
    auto& coro_state = coroutine_state(*active_coroutine_, engine_);
    const node_type kind = stmt->get_type();
    if ((kind == node_type::expression_stmt || kind == node_type::expression_decl) &&
        root->get_type() == node_type::yield_expr && coro_state.back_node() != root) {
        return;   // the statement IS the yield and its value is discarded: nothing to continue
    }
    coro_state.push_replay(stmt, rp_root, 0, {});
}

// Resume side: run the suspended child of a parked expression node. A child with no frame
// of its own is the recorded yield - consumed exactly once, its operand never re-evaluated.
checked_result<void> interpreter::replay_expression(expression* expr) {
    auto& coro_state = coroutine_state(*active_coroutine_, engine_);
    if (coro_state.back_node() != expr) {
        if (expr->get_type() == node_type::yield_expr) {
            push_value(make_value());   // vm exec_yield: the yield expression is null on resume
            return {};
        }
        return checked_result<void>(make_error_code(runtime_error_code::coroutine_suspended));
    }
    const replay_point frame = *coro_state.peek_continuation(expr);
    coro_state.pop_continuation();
    switch (expr->get_type()) {
        case node_type::binary_expr:        return visit_binary_expr_t<true>(static_cast<binary_expr*>(expr), &frame);
        case node_type::unary_expr:         return visit_unary_expr_t<true>(static_cast<unary_expr*>(expr), &frame);
        case node_type::assignment_expr:    return visit_assignment_expr_t<true>(static_cast<assignment_expr*>(expr), &frame);
        case node_type::call_expr:          return visit_call_expr_t<true>(static_cast<call_expr*>(expr), &frame);
        case node_type::member_expr:        return visit_member_expr_t<true>(static_cast<member_expr*>(expr), &frame);
        case node_type::new_expr:           return visit_new_expr_t<true>(static_cast<new_expr*>(expr), &frame);
        case node_type::ternary_expr:       return visit_ternary_expr_t<true>(static_cast<ternary_expr*>(expr), &frame);
        case node_type::array_literal_expr: return visit_array_literal_expr_t<true>(static_cast<array_literal_expr*>(expr), &frame);
        case node_type::map_literal_expr:   return visit_map_literal_expr_t<true>(static_cast<map_literal_expr*>(expr), &frame);
        case node_type::cast_expr:          return visit_cast_expr_t<true>(static_cast<cast_expr*>(expr), &frame);
        case node_type::throw_expr:         return visit_throw_expr_t<true>(static_cast<throw_expr*>(expr), &frame);
        case node_type::yield_expr:         return visit_yield_expr_t<true>(static_cast<yield_expr*>(expr), &frame);
        default:
            return checked_result<void>(make_error_code(runtime_error_code::coroutine_suspended));
    }
}

// The one statement/declaration a resume re-enters: it consumes its own replay root, every
// other statement runs cold
checked_result<void> interpreter::dispatch_stmt_resuming(statement* stmt) {
    if (!active_coroutine_) return dispatch_stmt(stmt);
    auto& coro_state = coroutine_state(*active_coroutine_, engine_);
    auto* cont = coro_state.peek_continuation(stmt);
    if (!cont) return dispatch_stmt(stmt);
    if (cont->phase != rp_root) {
        if (debug_hook_) [[unlikely]] { debug_quiet_node_ = stmt; }   // continued, not started
        return dispatch_stmt(stmt);
    }
    coro_state.pop_continuation();
    switch (stmt->get_type()) {
        case node_type::expression_stmt: return visit_expression_stmt_t<true>(static_cast<expression_stmt*>(stmt));
        case node_type::return_stmt:     return visit_return_stmt_t<true>(static_cast<return_stmt*>(stmt));
        default:
            return checked_result<void>(make_error_code(runtime_error_code::coroutine_suspended));
    }
}

checked_result<void> interpreter::dispatch_decl_resuming(declaration* decl) {
    if (!active_coroutine_) return dispatch_decl(decl);
    if (decl->get_type() == node_type::statement_decl) {
        return dispatch_stmt_resuming(static_cast<statement_decl*>(decl)->statement.get());
    }
    auto& coro_state = coroutine_state(*active_coroutine_, engine_);
    auto* cont = coro_state.peek_continuation(decl);
    if (!cont) return dispatch_decl(decl);
    if (cont->phase != rp_root) {
        if (debug_hook_) [[unlikely]] { debug_quiet_node_ = decl; }
        return dispatch_decl(decl);
    }
    coro_state.pop_continuation();
    switch (decl->get_type()) {
        case node_type::variable_decl:        return visit_variable_decl_t<true>(static_cast<variable_decl*>(decl));
        case node_type::destructuring_decl:   return visit_destructuring_decl_t<true>(static_cast<destructuring_decl*>(decl));
        case node_type::expression_decl:      return visit_expression_decl_t<true>(static_cast<expression_decl*>(decl));
        default:
            return checked_result<void>(make_error_code(runtime_error_code::coroutine_suspended));
    }
}

checked_result<void> interpreter::visit_binary_expr(binary_expr* expr) { return visit_binary_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_unary_expr(unary_expr* expr) { return visit_unary_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_assignment_expr(assignment_expr* expr) { return visit_assignment_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_call_expr(call_expr* expr) { return visit_call_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_member_expr(member_expr* expr) { return visit_member_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_new_expr(new_expr* expr) { return visit_new_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_ternary_expr(ternary_expr* expr) { return visit_ternary_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_array_literal_expr(array_literal_expr* expr) { return visit_array_literal_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_map_literal_expr(map_literal_expr* expr) { return visit_map_literal_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_cast_expr(cast_expr* expr) { return visit_cast_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_throw_expr(throw_expr* expr) { return visit_throw_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_yield_expr(yield_expr* expr) { return visit_yield_expr_t<false>(expr, nullptr); }
checked_result<void> interpreter::visit_expression_stmt(expression_stmt* stmt) { return visit_expression_stmt_t<false>(stmt); }
checked_result<void> interpreter::visit_return_stmt(return_stmt* stmt) { return visit_return_stmt_t<false>(stmt); }
checked_result<void> interpreter::visit_variable_decl(variable_decl* decl) { return visit_variable_decl_t<false>(decl); }
checked_result<void> interpreter::visit_destructuring_decl(destructuring_decl* decl) { return visit_destructuring_decl_t<false>(decl); }
checked_result<void> interpreter::visit_expression_decl(expression_decl* decl) { return visit_expression_decl_t<false>(decl); }

checked_result<void> interpreter::dispatch_expr(expression* expr) {
    if (stack_mark_.low()) [[unlikely]] { return detail::raise_nesting_depth(); }
    switch (expr->get_type()) {
        case node_type::literal_expr:
            return visit_literal_expr(static_cast<literal_expr*>(expr));
        case node_type::identifier_expr:
            return visit_identifier_expr(static_cast<identifier_expr*>(expr));
        case node_type::binary_expr:
            return visit_binary_expr(static_cast<binary_expr*>(expr));
        case node_type::unary_expr:
            return visit_unary_expr(static_cast<unary_expr*>(expr));
        case node_type::assignment_expr:
            return visit_assignment_expr(static_cast<assignment_expr*>(expr));
        case node_type::call_expr:
            return visit_call_expr(static_cast<call_expr*>(expr));
        case node_type::member_expr:
            return visit_member_expr(static_cast<member_expr*>(expr));
        case node_type::lambda_expr:
            return visit_lambda_expr(static_cast<lambda_expr*>(expr));
        case node_type::new_expr:
            return visit_new_expr(static_cast<new_expr*>(expr));
        case node_type::include_expr:
            return visit_include_expr(static_cast<include_expr*>(expr));
        case node_type::import_expr:
            return visit_import_expr(static_cast<import_expr*>(expr));
        case node_type::ternary_expr:
            return visit_ternary_expr(static_cast<ternary_expr*>(expr));
        case node_type::array_literal_expr:
            return visit_array_literal_expr(static_cast<array_literal_expr*>(expr));
        case node_type::map_literal_expr:
            return visit_map_literal_expr(static_cast<map_literal_expr*>(expr));
        case node_type::this_expr:
            return visit_this_expr(static_cast<this_expr*>(expr));
        case node_type::super_expr:
            return visit_super_expr(static_cast<super_expr*>(expr));
        case node_type::throw_expr:
            return visit_throw_expr(static_cast<throw_expr*>(expr));
        case node_type::yield_expr:
            return visit_yield_expr(static_cast<yield_expr*>(expr));
        case node_type::cast_expr:
            return visit_cast_expr(static_cast<cast_expr*>(expr));
        default:
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                "Unknown expression type in dispatch_expr");
    }
}

void interpreter::capture_stack_trace() {
    captured_trace_.clear();
    size_t constructions = construction_traces_.size();
    auto levels_from = [&](size_t floor) {   // the construction levels evaluating with at least `floor` frames below them
        while (constructions > 0 && construction_traces_[constructions - 1].depth >= floor) {
            const construction_trace_frame& level = construction_traces_[--constructions];
            captured_trace_.push_back(trace_frame_at(std::string(level.level), level.node ? &level.node->location : nullptr));
        }
    };
    for (size_t i = call_stack_.size(); i-- > 0;) {
        levels_from(i + 1);
        if (captured_trace_.size() >= k_stack_trace_frames) {
            cap_stack_trace(captured_trace_, i + constructions + 2);
            trace_captured_ = true;
            return;
        }
        captured_trace_.push_back(call_frame_trace(call_stack_[i]));
    }
    levels_from(0);
    captured_trace_.push_back(trace_frame_at("<script>", top_level_node_ ? &top_level_node_->location : nullptr));
    cap_stack_trace(captured_trace_);
    trace_captured_ = true;
}

stack_frame interpreter::call_frame_trace(const call_frame& frame) {
    return trace_frame_at(frame.function_name.empty() ? std::string("<anonymous>") : std::string(frame.function_name),
                          frame.current_node ? &frame.current_node->location : nullptr);
}

// capture_stack_trace's walk, uncapped, with the top level only while a top-level run is in flight
std::vector<stack_frame> interpreter::debug_frames() const {
    std::vector<stack_frame> out;
    out.reserve(call_stack_.size() + 1);
    for (auto it = call_stack_.rbegin(); it != call_stack_.rend(); ++it) { out.push_back(call_frame_trace(*it)); }
    if (execute_depth_ > 0) { out.push_back(trace_frame_at("<script>", top_level_node_ ? &top_level_node_->location : nullptr)); }
    return out;
}

void interpreter::note_clock_sample(std::chrono::steady_clock::time_point now) {
    if (engine_ && !engine_->parallel_region_active()) { engine_->execution_limits().note_clock_sample(now); }
}

// KEEP BYTE-PARALLEL with vm_backend::park_nested_failure / absorb_nested_trace
void interpreter::park_nested_failure() {
    nested_failure_.parked = true;
    nested_failure_.error = run_error_;
    nested_failure_.value = active_exception_value_;
    nested_failure_.exception = current_exception_;
    nested_failure_.trace = captured_trace_;
}

void interpreter::absorb_nested_trace() {
    if (!nested_failure_.parked) { return; }
    std::vector<stack_trace_entry> inner = std::move(nested_failure_.trace);
    nested_failure_.trace.clear();
    capture_stack_trace();
    captured_trace_.insert(captured_trace_.begin(), inner.begin(), inner.end());
    cap_stack_trace(captured_trace_);
}

std::string interpreter::current_source_file() const {
    for (auto it = call_stack_.rbegin(); it != call_stack_.rend(); ++it) {
        if (it->current_node && !it->current_node->location.filename.empty()) return it->current_node->location.filename;
    }
    return top_level_node_ ? top_level_node_->location.filename : std::string();
}

std::string interpreter::format_stack_trace() const {
    std::string out;
    for (const auto& e : captured_trace_) {
        if (e.elided) { out += "  " + elided_frames_text(e.elided) + '\n'; continue; }
        out += "  at " + e.function + " (" +
            (e.file.empty() ? std::string("<script>") : e.file) + ":" + std::to_string(e.line) + ")\n";
    }
    return out;
}

checked_result<void> interpreter::dispatch_stmt(statement* stmt) {
    if (stack_mark_.low()) [[unlikely]] { return detail::raise_nesting_depth(); }
    if (!call_stack_.empty()) call_stack_.back().current_node = stmt;
    else top_level_node_ = stmt;
    if (debug_hook_) [[unlikely]] {
        if (stmt == debug_quiet_node_) debug_quiet_node_ = nullptr;
        else if (debug_hook_->wants_statement(static_cast<uint32_t>(stmt->location.line)))
            debug_hook_->on_statement(stmt, static_cast<int>(call_stack_.size()));
    }
    switch (stmt->get_type()) {
        case node_type::expression_stmt:
            return visit_expression_stmt(static_cast<expression_stmt*>(stmt));
        case node_type::block_stmt:
            return visit_block_stmt(static_cast<block_stmt*>(stmt));
        case node_type::if_stmt:
            return visit_if_stmt(static_cast<if_stmt*>(stmt));
        case node_type::while_stmt:
            return visit_while_stmt(static_cast<while_stmt*>(stmt));
        case node_type::for_stmt:
            return visit_for_stmt(static_cast<for_stmt*>(stmt));
        case node_type::range_for_stmt:
            return visit_range_for_stmt(static_cast<range_for_stmt*>(stmt));
        case node_type::parallel_for_stmt:
            return visit_parallel_for_stmt(static_cast<parallel_for_stmt*>(stmt));
        case node_type::return_stmt:
            return visit_return_stmt(static_cast<return_stmt*>(stmt));
        case node_type::break_stmt:
            return visit_break_stmt(static_cast<break_stmt*>(stmt));
        case node_type::continue_stmt:
            return visit_continue_stmt(static_cast<continue_stmt*>(stmt));
        case node_type::try_stmt:
            return visit_try_stmt(static_cast<try_stmt*>(stmt));
        case node_type::switch_stmt:
            return visit_switch_stmt(static_cast<switch_stmt*>(stmt));
        case node_type::case_stmt:
            return visit_case_stmt(static_cast<case_stmt*>(stmt));
        case node_type::default_stmt:
            return visit_default_stmt(static_cast<default_stmt*>(stmt));
        case node_type::fallthrough_stmt:
            return visit_fallthrough_stmt(static_cast<fallthrough_stmt*>(stmt));
        default:
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                "Unknown statement type in dispatch_stmt");
    }
}

checked_result<void> interpreter::dispatch_decl(declaration* decl) {
    if (stack_mark_.low()) [[unlikely]] { return detail::raise_nesting_depth(); }
    if (!call_stack_.empty()) call_stack_.back().current_node = decl;
    else top_level_node_ = decl;
    if (debug_hook_) [[unlikely]] {
        // statement_decl is a pass-through wrapper: the inner dispatch_stmt fires the
        // hook — firing here too would stop twice on one statement (and a second park
        // after the resume is a join hang for anyone driving the controller).
        if (decl == debug_quiet_node_) debug_quiet_node_ = nullptr;
        else if (decl->get_type() != node_type::statement_decl &&
            debug_hook_->wants_statement(static_cast<uint32_t>(decl->location.line)))
            debug_hook_->on_statement(decl, static_cast<int>(call_stack_.size()));
    }
    switch (decl->get_type()) {
        case node_type::variable_decl:
            return visit_variable_decl(static_cast<variable_decl*>(decl));
        case node_type::function_decl:
            return visit_function_decl(static_cast<function_decl*>(decl));
        case node_type::class_decl:
            return visit_class_decl(static_cast<class_decl*>(decl));
        case node_type::namespace_decl:
            return visit_namespace_decl(static_cast<namespace_decl*>(decl));
        case node_type::expression_decl:
            return visit_expression_decl(static_cast<expression_decl*>(decl));
        case node_type::include_decl:
            return visit_include_decl(static_cast<include_decl*>(decl));
        case node_type::import_decl:
            return visit_import_decl(static_cast<import_decl*>(decl));
        case node_type::enum_decl:
            return visit_enum_decl(static_cast<enum_decl*>(decl));
        case node_type::destructuring_decl:
            return visit_destructuring_decl(static_cast<destructuring_decl*>(decl));
        case node_type::extern_decl:
            return visit_extern_decl(static_cast<extern_decl*>(decl));
        case node_type::statement_decl:
            return dispatch_stmt(static_cast<statement_decl*>(decl)->statement.get());
        default:
            return checked_result<void>(make_error_code(runtime_error_code::type_mismatch),
                "Unknown declaration type in dispatch_decl");
    }
}

namespace detail {

bool interpreter_compiled() noexcept { return true; }

std::unique_ptr<execution_backend> make_interpreter_backend(string_symbolizer* symbolizer, std::shared_ptr<environment> global_env) {
    return std::make_unique<interpreter>(symbolizer, std::move(global_env));
}

} // namespace detail

} // namespace jai
