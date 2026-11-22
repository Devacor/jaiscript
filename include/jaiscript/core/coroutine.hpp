#pragma once

#include <jaiscript/core/value.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <memory>
#include <string>
#include <vector>

namespace jai {

class engine;
class environment;
class interpreter;
class function_decl;
class execution_backend;

// Backend-owned continuation state. Each backend stores its own representation
// (interpreter: saved environment + AST continuation stack; VM: frames + ip).
struct coroutine_backend_state {
    virtual ~coroutine_backend_state() = default;
};

class coroutine_handle : public std::enable_shared_from_this<coroutine_handle> {
public:
    enum class status { created, running, suspended, completed, failed };

    coroutine_handle(engine* eng);
    ~coroutine_handle();
    coroutine_handle(const coroutine_handle&) = delete;
    coroutine_handle& operator=(const coroutine_handle&) = delete;

    // Advances a_count yields (each step runs to the next yield, the last step's value is returned); a step past the
    // end raises "Cannot resume a completed coroutine" after the earlier steps ran. Once its engine is destroyed the
    // coroutine is done and every resume returns the error "Value outlived its engine." without reading eng
    checked_result<script_value> next(engine* eng, script_int a_count = 1);
    // h.next() / h.next(count) as script calls it: both backends dispatch the builtin method here
    checked_result<script_value> next_from_script(engine* eng, const script_value* a_args, size_t a_argCount);
    // h.advance(v) as script calls it, C++'s bool advance(T& v): v binds at the call, one resume, then true with the yielded value
    // stored into v, or false when the body ran to its end or had ended (v untouched, the return value dropped). Both backends
    // dispatch here with the call site armed, so the argument resolves as a T& parameter binds it
    checked_result<script_value> advance_from_script(engine* eng, const std::vector<script_value>& a_args);
    // h.result(): a copy of the return value the body finished with (null for none, and for a body that failed), once done()
    checked_result<script_value> result_from_script(size_t a_argCount) const;
    // The backends finish a body here: the status and a copy of its return value, which next() also hands out
    void complete(const script_value& a_returned);
    bool done() const { return status_ == status::completed || status_ == status::failed; }
    status get_status() const { return status_; }
    // A later step of one host next(n): the backend keeps the entry's deadline instead of arming its own
    bool continues_host_entry() const { return continues_host_entry_; }

    // Internal — called by the backend when creating from a coroutine function declaration
    void set_function(std::shared_ptr<function_decl> func,
                      std::vector<script_value> args,
                      std::shared_ptr<environment> closure_env);

    // Internal — yield support
    void do_yield(script_value value);
    const script_value& last_value() const { return yield_value_; }

    std::shared_ptr<function_decl> get_function() const { return function_; }
    const std::vector<script_value>& get_args() const { return initial_args_; }
    // The first resume moves the arguments into the body's parameters and lets the rest go: no second copy outlives it
    std::vector<script_value>& binding_args() { return initial_args_; }
    void release_args() noexcept { std::vector<script_value>().swap(initial_args_); }
    std::shared_ptr<environment> get_closure_env() const { return closure_env_; }

    // Method coroutines pin their receiver here: the handle keeps the instance alive
    // across suspension (the closure env also carries 'this' for body resolution).
    void set_receiver(script_value receiver) { receiver_ = std::move(receiver); }
    const script_value& receiver() const { return receiver_; }

    void set_status(status s) { status_ = s; }

    // Where the body last yielded, for a save (detail/coroutine_snapshot.hpp): the VM notes the site's number, the interpreter
    // the yield expression
    void note_yield_site(uint32_t a_site) noexcept { yield_site_ = a_site; }
    void note_yield_node(const void* a_node) noexcept { yield_node_ = a_node; }
    uint32_t yield_site() const noexcept { return yield_site_; }
    const void* yield_node() const noexcept { return yield_node_; }

    // A copy of an object stands this handle where a coroutine of the object had no copy (detail/copy_rebind.hpp): it is
    // never done, and a resume raises the reason
    struct uncopied_state {
        std::string name;
        std::string reason;
        uint64_t resume_text = 0;   // the resume refusal, parked in the engine's symbols (an error names it, past this handle)
    };
    void mark_uncopied(std::string a_name, std::string a_reason);
    const uncopied_state* uncopied() const noexcept { return uncopied_.get(); }

    coroutine_backend_state* backend_state() const { return backend_state_.get(); }
    void set_backend_state(std::unique_ptr<coroutine_backend_state> state) {
        backend_state_ = std::move(state);
    }

private:
    friend class interpreter;
    friend class engine;

    // The engine's teardown: everything the coroutine holds dies while the engine still runs destructors, and the handle names
    // no engine afterwards; a resume is refused with "Value outlived its engine."
    void retire() noexcept;

    checked_result<script_value> resume_once(engine* eng);
    checked_result<script_value> resume_steps(execution_backend& a_backend, script_int a_count);
    checked_result<script_value> resume_refusal() const;

    engine* engine_ = nullptr;
    coroutine_handle* previous_ = nullptr;   // the engine's registry of live coroutines (engine::register_coroutine)
    coroutine_handle* next_ = nullptr;
    status status_ = status::created;
    bool continues_host_entry_ = false;
    uint32_t yield_site_ = UINT32_MAX;
    const void* yield_node_ = nullptr;
    script_value yield_value_;
    script_value result_;

    std::shared_ptr<function_decl> function_;
    std::vector<script_value> initial_args_;
    std::shared_ptr<environment> closure_env_;
    script_value receiver_;

    std::unique_ptr<coroutine_backend_state> backend_state_;
    std::unique_ptr<uncopied_state> uncopied_;
};

namespace detail {
// coroutine<Y> as reflect::type_of names a handle: Y is its coroutine's declared type, var when it declares none
std::string coroutine_type_name(const script_value& value);
// that name interned, for an error text's argument (out of line: the member paths that raise with it stay small)
uint64_t coroutine_type_symbol(const script_value& value, engine* eng);
}

// The value a coroutine declaration defines: a call mints a handle over the declaration and the scope it was declared in
struct coroutine_mint {
    engine* eng = nullptr;
    uint64_t type_id = 0;
    std::shared_ptr<function_decl> declaration;
    std::shared_ptr<environment> closure;
    checked_result<script_value> operator()(const std::vector<script_value>& a_args) const;
};

} // namespace jai
