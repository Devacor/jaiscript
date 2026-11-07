#include "../../include/jaiscript/core/coroutine.hpp"
#include "../../include/jaiscript/core/engine.hpp"
#include "../../include/jaiscript/core/execution_backend.hpp"
#include "../../include/jaiscript/detail/arg_capture.hpp"
#include "../../include/jaiscript/core/function_binder.hpp"

namespace jai {

coroutine_handle::coroutine_handle(engine* eng)
    : engine_(eng)
    , yield_value_(std::monostate{}, eng)
    , result_(std::monostate{}, eng)
    , receiver_(std::monostate{}, eng) {
}

void coroutine_handle::set_function(std::shared_ptr<function_decl> func,
                                     std::vector<script_value> args,
                                     std::shared_ptr<environment> closure_env) {
    function_ = std::move(func);
    initial_args_ = std::move(args);
    closure_env_ = std::move(closure_env);
    // the call's arguments take their values at the call; the body binds them at the first resume
    for (size_t i = 0; i < initial_args_.size(); ++i) {
        if (!detail::capture_candidate(initial_args_[i], detail::transient_read_marker::arg_capture_full)) continue;
        detail::capture_argument(engine_, initial_args_[i], detail::transient_read_marker::arg_capture_full,
                                 function_ ? detail::parameters_binding(function_->parameters, i) : detail::arg_binding::opaque);
    }
}

void coroutine_handle::do_yield(script_value value) {
    yield_value_ = std::move(value);
    status_ = status::suspended;
}

namespace {

JAI_NOINLINE checked_result<script_value> next_argument_count_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "next() takes 0 or 1 arguments ([count])");
}

JAI_NOINLINE checked_result<script_value> next_count_type_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "next() count must be an integer");
}

JAI_NOINLINE checked_result<script_value> next_count_range_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation), "next() count must be at least 1");
}

JAI_NOINLINE checked_result<script_value> advance_argument_count_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "advance() takes 1 argument (the variable that receives the yielded value)");
}

JAI_NOINLINE checked_result<script_value> advance_non_lvalue_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::invalid_reference), "Cannot pass non-lvalue to reference parameter");
}

JAI_NOINLINE checked_result<script_value> result_argument_count_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::argument_count_mismatch), "result() takes no arguments");
}

JAI_NOINLINE checked_result<script_value> result_unfinished_error() {
    return checked_result<script_value>(make_error_code(runtime_error_code::invalid_operation),
        "Coroutine has not finished: result() is available once done() is true");
}

} // namespace

checked_result<script_value> coroutine_handle::next(engine* eng, script_int a_count) {
    if (a_count < 1) { return next_count_range_error(); }
    if (a_count == 1) { return resume_once(eng); }
    execution_backend* backend = detail::engine_access::get_execution_backend(*eng);
    if (backend->is_executing() || backend->is_unwinding()) { return resume_steps(*backend, a_count); }
    struct step_request { coroutine_handle* self; script_int count; } request{this, a_count};
    return detail::engine_access::checked_host_entry(*eng, [](execution_backend& a_backend, void* a_request) {
        auto* steps = static_cast<step_request*>(a_request);
        return steps->self->resume_steps(a_backend, steps->count);
    }, &request);
}

// The steps of one next(n) are one entry: the entry armed the deadline, no step re-arms it, and a step's throw ends them
checked_result<script_value> coroutine_handle::resume_steps(execution_backend& a_backend, script_int a_count) {
    struct entry_scope {
        bool& continues;
        bool saved;
        ~entry_scope() { continues = saved; }
    } scope{continues_host_entry_, continues_host_entry_};
    continues_host_entry_ = true;
    for (script_int step = 1;; ++step) {
        if (done() || status_ == status::running) { return resume_refusal(); }
        checked_result<script_value> result = a_backend.resume_coroutine(*this);
        if (step == a_count || !result || a_backend.is_unwinding()) { return result; }
    }
}

checked_result<script_value> coroutine_handle::next_from_script(engine* eng, const script_value* a_args, size_t a_argCount) {
    if (a_argCount == 0) { return resume_once(eng); }
    if (a_argCount > 1) { return next_argument_count_error(); }
    if (!a_args[0].is_int()) { return next_count_type_error(); }
    return next(eng, a_args[0].as_int());
}

checked_result<script_value> coroutine_handle::advance_from_script(engine* eng, const std::vector<script_value>& a_args) {
    if (a_args.size() != 1) { return advance_argument_count_error(); }
    if (status_ == status::running) { return resume_refusal(); }
    detail::host_reference out = detail::host_reference_argument(eng, a_args, 0, 0);
    if (!out.target.is_reference() && !out.slot.frame) { return advance_non_lvalue_error(); }
    if (done()) { return script_value(false, eng); }
    checked_result<script_value> resumed = resume_once(eng);
    if (!resumed || detail::engine_access::get_execution_backend(*eng)->is_unwinding()) { return resumed; }
    if (status_ != status::suspended) { return script_value(false, eng); }
    if (auto stored = detail::host_reference_store(eng, out.slot, out.target, std::move(resumed.value())); !stored) {
        return stored.error_value();
    }
    return script_value(true, eng);
}

void coroutine_handle::complete(const script_value& a_returned) {
    status_ = status::completed;
    result_ = a_returned.deref().clone_element();
}

checked_result<script_value> coroutine_handle::result_from_script(size_t a_argCount) const {
    if (a_argCount != 0) { return result_argument_count_error(); }
    if (!done()) { return result_unfinished_error(); }
    return result_.clone_element();
}

checked_result<script_value> coroutine_handle::resume_refusal() const {
    if (done()) {
        return checked_result<script_value>(
            make_error_code(runtime_error_code::evaluation_failed),
            "Cannot resume a completed coroutine");
    }
    return checked_result<script_value>(
        make_error_code(runtime_error_code::evaluation_failed),
        "Coroutine is already running");
}

checked_result<script_value> coroutine_handle::resume_once(engine* eng) {
    if (done() || status_ == status::running) { return resume_refusal(); }

    execution_backend* backend = detail::engine_access::get_execution_backend(*eng);
    if (backend->is_executing() || backend->is_unwinding()) { return backend->resume_coroutine(*this); }
    return detail::engine_access::checked_host_entry(*eng, [](execution_backend& be, void* self) { return be.resume_coroutine(*static_cast<coroutine_handle*>(self)); }, this);
}

} // namespace jai
