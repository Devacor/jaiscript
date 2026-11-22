#include <jaiscript/debug/controller.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/builtin_api_arities.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/environment.hpp>
#include <jaiscript/detail/expr_children.hpp>
#include <jaiscript/detail/ref_lvalue.hpp>
#include <jaiscript/detail/value_text.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>

namespace jai::debug {

namespace {

// Match the canonical filename stamped by execute_file/import: the canonical path, else the weakly canonical one, else the
// path as given. Runs only where the engine grants filesystem access; a denied path stays an exact virtual label.
std::string normalize_bp_path(const std::string& path) {
    // "<script>" is execute()'s label, never a file: libc++'s weakly_canonical would prefix it with the working directory
    if (path.empty() || path == "<script>") { return path; }
    std::error_code ec;
    auto canon = std::filesystem::canonical(std::filesystem::path(path), ec);
    if (!ec) { return canon.string(); }
    canon = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return ec ? path : canon.string();
}

using stop_line_map = std::unordered_map<std::string, std::vector<int>>;

void collect_stop_lines(const statement* s, stop_line_map& out);

// A refused store raises the text the script's own store would, its names substituted
[[noreturn]] JAI_NOINLINE void throw_store_failure(const checked_result<void>& a_stored, engine& a_engine) {
    throw runtime_error(format_error(a_stored, *detail::engine_access::get_symbolizer(a_engine)));
}

// Lambda bodies inside an expression hold statements of their own
void collect_expression_stop_lines(const expression* e, stop_line_map& out) {
    if (!e) return;
    if (e->get_type() == node_type::lambda_expr) { collect_stop_lines(static_cast<const lambda_expr*>(e)->body.get(), out); }
    detail::for_each_child_expression(e, [&](const expression* child) { collect_expression_stop_lines(child, out); });
}

// Every line a statement the backends stop at starts on, by file (a spliced include's statements carry their own file).
// A for initializer belongs to its loop's stop; a parallel body runs atomically to the debugger.
void collect_stop_lines(const statement* s, stop_line_map& out) {
    if (!s) return;
    if (controller::stops_at(s)) { out[s->location.filename].push_back(static_cast<int>(s->location.line)); }
    auto stmts = [&](const auto& list) { for (const auto& d : list) collect_stop_lines(d.get(), out); };
    auto expr = [&](const expression_ptr& e) { collect_expression_stop_lines(e.get(), out); };
    switch (s->get_type()) {
    case node_type::statement_decl: collect_stop_lines(static_cast<const statement_decl*>(s)->statement.get(), out); break;
    case node_type::block_stmt: stmts(static_cast<const block_stmt*>(s)->declarations); break;
    case node_type::expression_stmt: expr(static_cast<const expression_stmt*>(s)->expression); break;
    case node_type::expression_decl: expr(static_cast<const expression_decl*>(s)->expression); break;
    case node_type::variable_decl: expr(static_cast<const variable_decl*>(s)->initializer); break;
    case node_type::destructuring_decl: expr(static_cast<const destructuring_decl*>(s)->initializer); break;
    case node_type::return_stmt: expr(static_cast<const return_stmt*>(s)->value); break;
    case node_type::if_stmt: {
        auto* is = static_cast<const if_stmt*>(s);
        expr(is->condition);
        collect_stop_lines(is->then_statement.get(), out);
        collect_stop_lines(is->else_statement.get(), out);
        break;
    }
    case node_type::while_stmt: {
        auto* ws = static_cast<const while_stmt*>(s);
        expr(ws->condition);
        collect_stop_lines(ws->body.get(), out);
        break;
    }
    case node_type::for_stmt: {
        auto* fs = static_cast<const for_stmt*>(s);
        if (fs->initializer && fs->initializer->get_type() == node_type::variable_decl) { expr(static_cast<const variable_decl*>(fs->initializer.get())->initializer); }
        expr(fs->condition);
        expr(fs->update);
        collect_stop_lines(fs->body.get(), out);
        break;
    }
    case node_type::range_for_stmt: {
        auto* rf = static_cast<const range_for_stmt*>(s);
        expr(rf->container);
        collect_stop_lines(rf->body.get(), out);
        break;
    }
    case node_type::parallel_for_stmt: expr(static_cast<const parallel_for_stmt*>(s)->container); break;
    case node_type::switch_stmt: {
        auto* sw = static_cast<const switch_stmt*>(s);
        expr(sw->condition);
        for (const auto& c : sw->cases) { expr(c->value); stmts(c->body); }
        if (sw->default_case) { stmts(sw->default_case->body); }
        break;
    }
    case node_type::try_stmt: {
        auto* tr = static_cast<const try_stmt*>(s);
        collect_stop_lines(tr->try_block.get(), out);
        collect_stop_lines(tr->catch_block.get(), out);
        break;
    }
    case node_type::function_decl: {
        auto* fd = static_cast<const function_decl*>(s);
        for (const auto& p : fd->parameters) { expr(p.default_value); }
        for (const auto& init : fd->initializers) { for (const auto& a : init.arguments) { expr(a); } expr(init.value); }
        collect_stop_lines(fd->body.get(), out);
        break;
    }
    case node_type::class_decl:
        for (const auto& m : static_cast<const class_decl*>(s)->members) { collect_stop_lines(m.declaration.get(), out); }
        break;
    case node_type::namespace_decl: stmts(static_cast<const namespace_decl*>(s)->declarations); break;
    default: break;
    }
}

} // namespace

controller::controller(engine* eng) : engine_(eng) {}

void controller::set_on_stopped(std::function<void(const stop_info&)> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    on_stopped_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Control-side (transport thread)
// ---------------------------------------------------------------------------

void controller::set_enabled(bool on) {
    bool was = enabled_.exchange(on, std::memory_order_acq_rel);
    if (on == was) return;
    if (on) { pending_pause_.store(0, std::memory_order_relaxed); }   // a pause asked for before the session never stops it
    raise_sync();
    // The backend notices at its next debug sync point (execute entry, a sleep, the
    // 1024-tick budget sample). While its hook is armed it suspends wall-clock deadline
    // checks itself and re-arms a fresh deadline when the session ends — a script parked
    // for minutes is never killed by the budget, and nothing here touches engine state
    // cross-thread. Leaving a session must never strand a parked thread or a live step mode.
    if (!on) detach();
}

void controller::set_breakpoints(const std::string& file, std::vector<int> lines) {
    publish_breakpoints(file, std::move(lines), false);
}

void controller::set_source_breakpoints(const std::string& file, std::vector<int> lines) {
    publish_breakpoints(file, std::move(lines), true);
}

void controller::set_source_breakpoints(const std::string& file, const std::vector<breakpoint>& breakpoints) {
    std::vector<int> lines;
    std::vector<breakpoint> rich;
    for (const breakpoint& bp : breakpoints) {
        lines.push_back(bp.line);
        if (!bp.condition.empty() || !bp.hit_condition.empty() || !bp.log_message.empty()) { rich.push_back(bp); }
    }
    publish_breakpoints(file, std::move(lines), true, std::move(rich));
}

void controller::set_exception_breakpoints(bool on_throw, bool on_uncaught) {
    break_on_throw_.store(on_throw, std::memory_order_relaxed);
    break_on_uncaught_.store(on_uncaught, std::memory_order_relaxed);
}

void controller::set_on_output(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    on_output_ = std::move(cb);
}

void controller::set_on_uncaught_error(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    reports_uncaught_.store(static_cast<bool>(cb), std::memory_order_relaxed);
    on_uncaught_error_ = std::move(cb);
}

void controller::publish_breakpoints(const std::string& file, std::vector<int> lines, bool source_path, std::vector<breakpoint> rich) {
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    auto current = breakpoints_.load(std::memory_order_acquire);
    auto next = std::make_shared<breakpoint_snapshot>(*current);
    auto& records = source_path ? next->source : next->native;
    records[file] = breakpoint_record{std::move(lines), ++next->sequence, std::move(rich)};
    breakpoints_.store(std::move(next), std::memory_order_release);
    bp_version_.fetch_add(1, std::memory_order_release);   // after publication: sync sees an at-least-as-new snapshot
    raise_sync();
}

// The flag first, then the gate: a sync that cleared the flag before this store re-raises the gate after its own write
// (sync_hot_state), so neither a sleeping thread nor an armed statement gate misses the change
void controller::raise_sync() {
    sync_requested_.store(true);
    hot_step_or_pause_.store(true);
    { std::lock_guard<std::mutex> lk(mtx_); }   // a sleeper between its predicate check and its wait gets the notify
    cv_.notify_all();
}

void controller::set_on_source_loaded(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(sources_mtx_);
    on_source_loaded_ = std::move(cb);
}

void controller::note_source(const std::string& path, const std::vector<std::shared_ptr<declaration>>* declarations) {
    if (path.empty() || path == "<script>" || path == evaluation_source) return;
    normalize_sources_.store(engine_->is_enabled(jai::permissions::filesystem), std::memory_order_relaxed);
    stop_line_map lines;
    if (declarations) {
        for (const auto& d : *declarations) { collect_stop_lines(d.get(), lines); }
        for (auto& [file, at] : lines) {
            std::sort(at.begin(), at.end());
            at.erase(std::unique(at.begin(), at.end()), at.end());
        }
    }
    std::function<void(const std::string&)> cb;
    bool lines_changed = false;
    {
        std::lock_guard<std::mutex> lk(sources_mtx_);
        for (auto& [file, at] : lines) {
            if (file.empty() || file == "<script>") { continue; }
            auto& known = stop_lines_[file];
            if (known != at) { known = std::move(at); lines_changed = true; }
        }
        if (lines_changed) { stop_lines_version_.fetch_add(1, std::memory_order_release); }
        if (sources_.insert(path).second) { cb = on_source_loaded_; }
    }
    if (lines_changed) { raise_sync(); }   // breakpoints already installed in this file move onto its statements
    if (cb) cb(path);
}

std::optional<int> controller::breakpoint_line(const std::string& path, int line) const {
    const std::string normalized = normalize_sources_.load(std::memory_order_relaxed) ? normalize_bp_path(path) : path;
    std::lock_guard<std::mutex> lk(sources_mtx_);
    auto known = stop_lines_.find(path);
    if (known == stop_lines_.end()) { known = stop_lines_.find(normalized); }
    if (known == stop_lines_.end()) { return line; }
    const auto at = std::lower_bound(known->second.begin(), known->second.end(), line);
    if (at == known->second.end()) { return std::nullopt; }
    return *at;
}

bool controller::source_known(const std::string& path) const {
    if (path == "<script>") return true;
    const std::string normalized = normalize_sources_.load(std::memory_order_relaxed) ? normalize_bp_path(path) : path;
    std::lock_guard<std::mutex> lk(sources_mtx_);
    return sources_.contains(path) || sources_.contains(normalized);
}

void controller::clear_breakpoints() {
    auto current = breakpoints_.load(std::memory_order_acquire);
    auto next = std::make_shared<breakpoint_snapshot>();
    next->sequence = current->sequence + 1;
    next->clear_sequence = next->sequence;
    breakpoints_.store(std::move(next), std::memory_order_release);
    bp_version_.fetch_add(1, std::memory_order_release);
    raise_sync();
}

void controller::request_pause() {
    pending_pause_.store(1, std::memory_order_relaxed);
    raise_sync();
}

void controller::resume() {
    std::lock_guard<std::mutex> lk(mtx_);
    step_mode_.store(static_cast<int>(step_mode::none), std::memory_order_relaxed);
    resume_requested_ = true;
    cv_.notify_all();
}

void controller::step_over() {
    std::lock_guard<std::mutex> lk(mtx_);
    step_base_depth_.store(current_stop_depth_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    step_mode_.store(static_cast<int>(step_mode::over), std::memory_order_relaxed);
    resume_requested_ = true;
    cv_.notify_all();
}

void controller::step_into() {
    std::lock_guard<std::mutex> lk(mtx_);
    step_mode_.store(static_cast<int>(step_mode::into), std::memory_order_relaxed);
    resume_requested_ = true;
    cv_.notify_all();
}

void controller::step_out() {
    std::lock_guard<std::mutex> lk(mtx_);
    step_base_depth_.store(current_stop_depth_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    step_mode_.store(static_cast<int>(step_mode::out), std::memory_order_relaxed);
    resume_requested_ = true;
    cv_.notify_all();
}

void controller::post_command(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    commands_.push_back(std::move(fn));
    cv_.notify_all();
}

bool controller::post_command_while_paused(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!paused_.load(std::memory_order_relaxed)) { return false; }
    commands_.push_back(std::move(fn));
    cv_.notify_all();
    return true;
}

void controller::detach() {
    std::lock_guard<std::mutex> lk(mtx_);
    step_mode_.store(static_cast<int>(step_mode::none), std::memory_order_relaxed);
    pending_pause_.store(0, std::memory_order_relaxed);
    sync_requested_.store(true);
    hot_step_or_pause_.store(true);
    resume_requested_ = true;
    detaches_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Inspection (parked thread only)
// ---------------------------------------------------------------------------

script_value* controller::variable_storage(const std::string& name) const {
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    if (!backend) { return nullptr; }
    if (script_value* local = backend->debug_local_storage(name, 0)) { return local; }
    const auto env = backend->get_current_environment();
    return env ? env->get_env_var_ptr(detail::engine_access::get_symbolizer(*engine_)->intern(name)) : nullptr;
}

script_value controller::get_variable(const std::string& name) const {
    if (const script_value* storage = variable_storage(name)) { return storage->deref(); }
    if (auto* backend = detail::engine_access::get_execution_backend(*engine_)) {
        for (const auto& [local, value] : backend->get_frame_locals(0)) {   // a lowered aggregate: its components folded back
            if (local == name) { return value; }
        }
    }
    throw runtime_error("No variable '" + name + "' is in scope at this stop");
}

void controller::set_variable(const std::string& name, const script_value& value) {
    script_value* storage = variable_storage(name);
    if (!storage) { throw runtime_error("No variable '" + name + "' is in scope at this stop"); }
    const auto program = detail::engine_access::get_global_environment(*engine_);
    if (detail::engine_access::is_const_global(*engine_, name) && program && storage == program->get_env_var_ptr(detail::engine_access::get_symbolizer(*engine_)->intern(name))) {
        throw runtime_error("Cannot assign to '" + name + "': it is read-only");
    }
    script_value assigned = value;
    auto stored = detail::variable_store(storage->deref(), assigned, engine_, detail::slot_site{detail::slot_kind::assignment, name});
    if (!stored) { throw_store_failure(stored, *engine_); }
}


namespace {

// A Variables row shows the structural display text (no to_string() runs on a parked thread), cut short past this many bytes
constexpr size_t k_variable_text_limit = 1000;
// A container lists at most this many parts in one request; an editor pages through the rest
constexpr size_t k_children_limit = 10000;
// The name the evaluated text reaches a method frame's `this` by (instance variables cannot be named this)
constexpr std::string_view k_this_name = "__jai_this";

std::string variable_kind(const script_value& value) {
    if (value.is_array()) return "array";
    if (value.is_map()) return "map";
    if (value.is_object()) return "object";
    if (value.is_math()) return std::string(math_type_name(value.deref().current_type()));
    if (value.is_function()) return "function";
    if (value.is_string() || value.is_utf8_string()) return "string";
    if (value.is_int()) return "int";
    if (value.is_float()) return "float";
    if (value.is_bool()) return "bool";
    if (value.is_char()) return "char";
    if (value.is_null()) return "null";
    return {};
}

// A string as its literal is written, cut past the limit
std::string quoted_text(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (out.size() >= k_variable_text_limit) { return out + "..."; }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out + "\"";
}

std::string display_text(const script_value& value) {
    const script_value& held = value.deref();
    if (held.is_string()) return quoted_text(held.unchecked_as_string());
    if (held.is_utf8_string()) return quoted_text(held.unchecked_as_utf8().bytes);
    if (detail::plain_text_value(value)) return value.to_string();
    auto text = detail::value_text(value, {}, k_variable_text_limit);
    return text ? text.value() : std::string(text.message());
}

// A logpoint's {expression} text: what print shows, with no script to_string() run
std::string printed_text(const script_value& value) {
    const script_value& held = value.deref();
    if (held.is_string()) return std::string(held.unchecked_as_string());
    if (held.is_utf8_string()) return held.unchecked_as_utf8().bytes;
    return display_text(value);
}

script_value expansion_target(const script_value& value) { return detail::shown_object(value); }

bool same_value(const script_value& a, const script_value& b) {
    return std::memcmp(static_cast<const void*>(&a), static_cast<const void*>(&b), sizeof(script_value)) == 0;
}

// An evaluate_name that can take a .field or [index] after it
std::string postfix_base(const std::string& name) {
    const bool simple = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.' || c == '[' || c == ']' || c == '"';
    });
    return simple ? name : "(" + name + ")";
}

// A map key written as the literal that reaches it again; empty for a key no literal spells
std::string key_literal(const script_value& key) {
    const script_value& k = key.deref();
    if (k.is_string()) return quoted_text(k.unchecked_as_string());
    if (k.is_int() || k.is_bool()) return k.to_string();
    return {};
}

// Why a watch cannot run a parsed expression (empty: it only reads). A call is admitted only for a builtin whose catalog effect
// reads; script code it reaches anyway (an operator, a getter, a constructor) is refused as it runs.
std::string watch_refusal(const expression* e);

std::string qualified_callee(const expression* callee) {
    if (callee->get_type() == node_type::identifier_expr) { return std::string(static_cast<const identifier_expr*>(callee)->name); }
    if (callee->get_type() == node_type::member_expr) {
        const auto* m = static_cast<const member_expr*>(callee);
        if (!m->is_static || !m->object) { return {}; }
        const std::string base = qualified_callee(m->object.get());
        return base.empty() ? std::string() : base + "::" + std::string(m->member);
    }
    return {};
}

std::string call_refusal(const call_expr* call) {
    const expression* callee = call->callee.get();
    if (callee->get_type() == node_type::member_expr && !static_cast<const member_expr*>(callee)->is_static) {
        const auto* m = static_cast<const member_expr*>(callee);
        if (!detail::isObservingBuiltin(m->member, true)) { return "a call of '" + std::string(m->member) + "'"; }
        return watch_refusal(m->object.get());
    }
    const std::string name = qualified_callee(callee);
    if (name.empty()) { return "a call of a computed function"; }
    if (!detail::isObservingBuiltin(name, false)) { return "a call of '" + name + "'"; }
    return {};
}

std::string watch_refusal(const expression* e) {
    if (!e) return {};
    switch (e->get_type()) {
    case node_type::assignment_expr: return "an assignment";
    case node_type::unary_expr: {
        const auto op = static_cast<const unary_expr*>(e)->op.type;
        if (op == token_type::plus_plus) return "an increment";
        if (op == token_type::minus_minus) return "a decrement";
        break;
    }
    case node_type::call_expr: {
        std::string refused = call_refusal(static_cast<const call_expr*>(e));
        if (!refused.empty()) return refused;
        for (const auto& argument : static_cast<const call_expr*>(e)->arguments) {
            refused = watch_refusal(argument.get());
            if (!refused.empty()) return refused;
        }
        return {};
    }
    case node_type::lambda_expr: return "a lambda";
    case node_type::throw_expr: return "a throw";
    case node_type::yield_expr: return "a yield";
    case node_type::include_expr: return "an include";
    case node_type::import_expr: return "an import";
    default: break;
    }
    std::string refused;
    detail::for_each_child_expression(e, [&](const expression* child) {
        if (refused.empty()) refused = watch_refusal(child);
    });
    return refused;
}

// Each `this` in text spelled k_this_name (the lexer's tokens find it outside strings and comments)
// A bare name a method frame calls (`twice(2)`, `tagged<int>(7)`) that the frame's scope does not hold: one of its class's methods,
// called through the frame's `this` as the method's own code calls it (a local or a field of the name wins, as there)
bool bare_method_call(const std::vector<token>& tokens, size_t i, const class_definition* cls, const std::vector<std::pair<std::string, script_value>>& scope,
                      engine& eng) {
    const token& name = tokens[i];
    if (!cls || name.type != token_type::identifier || i + 1 >= tokens.size()) { return false; }
    if (i > 0) {
        switch (tokens[i - 1].type) {
            case token_type::dot: case token_type::question_dot: case token_type::arrow: case token_type::colon_colon: case token_type::identifier:
            case token_type::greater: case token_type::right_angle: case token_type::tilde: case token_type::int_keyword: case token_type::float_keyword:
            case token_type::bool_keyword: case token_type::char_keyword: case token_type::string_keyword: case token_type::void_keyword:
            case token_type::auto_keyword: case token_type::var_keyword: case token_type::function_keyword: case token_type::class_keyword:
            case token_type::coroutine_keyword: case token_type::new_keyword:
                return false;
            default:
                break;
        }
    }
    const std::string spelled(name.lexeme);
    if (std::any_of(scope.begin(), scope.end(), [&](const auto& entry) { return entry.first == spelled; })) { return false; }
    if (tokens[i + 1].type == token_type::left_paren) { return cls->defines_method(detail::engine_access::get_symbolizer(eng)->intern(spelled)); }
    return tokens[i + 1].type == token_type::less && detail::engine_access::names_generic_method(eng, spelled);
}

std::string with_this_renamed(const std::string& text, engine& eng, const class_definition* cls, const std::vector<std::pair<std::string, script_value>>& scope) {
    lexer lex(text, detail::engine_access::get_symbolizer(eng), std::string(evaluation_source));
    const auto tokens = lex.tokenize();
    std::vector<size_t> line_starts{0};
    for (size_t i = 0; i < text.size(); ++i) { if (text[i] == '\n') line_starts.push_back(i + 1); }
    std::string out = text;
    for (size_t i = tokens.size(); i-- > 0;) {
        const token& t = tokens[i];
        if (t.location.line == 0 || t.location.line > line_starts.size()) continue;
        const size_t at = line_starts[t.location.line - 1] + t.location.column - 1;
        if (t.type == token_type::this_keyword) {
            if (out.compare(at, 4, "this") == 0) { out.replace(at, 4, k_this_name); }
        } else if (bare_method_call(tokens, i, cls, scope, eng) && out.compare(at, t.lexeme.size(), t.lexeme) == 0) {
            out.insert(at, std::string(k_this_name) + ".");
        }
    }
    return out;
}

std::string watch_text_refusal(const std::string& text, engine& eng) {
    string_symbolizer* symbols = detail::engine_access::get_symbolizer(eng);
    lexer lex(text, symbols, std::string(evaluation_source));
    const auto tokens = lex.tokenize();
    const auto templates = detail::engine_access::get_registered_template_types(eng);
    parser parse(tokens, symbols, &eng, templates, std::string(evaluation_source));
    parse.source_text(text);
    auto parsed = parse.parse();
    if (!parsed) { return {}; }   // the evaluation reports the parse error itself
    for (const auto& decl : parsed.value()) {
        const statement* s = decl.get();
        if (s->get_type() == node_type::statement_decl) { s = static_cast<const statement_decl*>(s)->statement.get(); }
        const expression* e = s->get_type() == node_type::expression_stmt ? static_cast<const expression_stmt*>(s)->expression.get()
                            : s->get_type() == node_type::expression_decl ? static_cast<const expression_decl*>(s)->expression.get() : nullptr;
        if (!e) { return "a statement"; }
        std::string refused = watch_refusal(e);
        if (!refused.empty()) return refused;
    }
    return {};
}

} // namespace

std::vector<frame_info> controller::list_frames() const {
    std::vector<frame_info> out;
    if (auto* backend = detail::engine_access::get_execution_backend(*engine_)) {
        for (const auto& f : backend->debug_frames()) {
            out.push_back(frame_info{ f.function, f.file, static_cast<int>(f.line) });
        }
    }
    if (out.empty()) { out.push_back(frame_info{ "<script>", {}, 0 }); }
    // the innermost frame stands where it stopped (a statement that holds the next one stops first)
    out.front().file = current_stop_.file;
    out.front().line = current_stop_.line;
    return out;
}

std::string controller::frame_text(const std::string& function, const std::string& file, size_t line) {
    return function + " (" + (file.empty() ? std::string("<script>") : file) + ":" + std::to_string(line) + ")";
}

std::string controller::dump() const {
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    if (!backend || !backend->is_executing()) return {};
    backend->debug_settle_frames();
    std::string out;
    const auto frames = backend->debug_frames();
    for (size_t k = 0; k < frames.size(); ++k) {
        out += "  at " + frame_text(frames[k].function, frames[k].file, frames[k].line) + "\n";
        auto named = backend->get_frame_locals(k);
        std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [name, value] : named) { out += "      " + name + " = " + display_text(value) + "\n"; }
    }
    return out;
}

uint32_t controller::register_expandable(expandable entry) {
    if (expandables_.size() >= (1u << 19)) { return 0; }   // past what a DAP handle carries: a leaf
    expandables_.push_back(std::move(entry));
    return static_cast<uint32_t>(expandables_.size());
}

uint32_t controller::locals_reference(size_t frame) {
    expandable entry;
    entry.kind = expandable::kind_t::locals;
    entry.frame = frame;
    return register_expandable(std::move(entry));
}

uint32_t controller::globals_reference() {
    expandable entry;
    entry.kind = expandable::kind_t::globals;
    return register_expandable(std::move(entry));
}

variable_info controller::row(const std::string& name, const script_value& value, size_t frame, std::string evaluate_name) {
    variable_info out{ name, display_text(value), variable_kind(value) };
    const script_value target = expansion_target(value);
    size_t parts = 0;
    bool indexed = false;
    if (target.raw_storage_index() == script_value::TYPEID_ARRAY) {
        parts = target.unchecked_array_node()->size();
        indexed = true;
    } else if (target.raw_storage_index() == script_value::TYPEID_MAP) {
        parts = target.unchecked_as_map().size();
        indexed = true;
    } else if (target.raw_storage_index() == script_value::TYPEID_OBJECT) {
        parts = detail::shown_fields(target, engine_).size();
    }
    if (parts) {
        expandable entry;
        entry.frame = frame;
        entry.value = value;
        entry.evaluate_name = evaluate_name;
        out.reference = register_expandable(std::move(entry));
        (indexed ? out.indexed_children : out.named_children) = parts;
    }
    out.evaluate_name = std::move(evaluate_name);
    return out;
}

std::vector<variable_info> controller::list_locals(size_t frame) {
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    if (!backend) return {};
    std::vector<variable_info> out;
    for (const auto& [name, value] : backend->get_frame_locals(frame)) { out.push_back(row(name, value, frame, name)); }
    std::sort(out.begin(), out.end(), [](const variable_info& a, const variable_info& b) { return a.name < b.name; });
    return out;
}

// The program's own variables: the engine's internals (__class_*), what the host registered (add_global) and callables stay out
std::vector<variable_info> controller::list_globals() {
    std::vector<variable_info> out;
    if (const auto program = detail::engine_access::get_global_environment(*engine_)) {
        for (const auto& [name, value] : program->get_local_variables()) {
            std::string text(name);
            if (value.is_function() || text.starts_with("__") || detail::engine_access::host_global_registered(*engine_, text)) { continue; }
            out.push_back(row(text, value, 0, "::" + text));
        }
    }
    std::sort(out.begin(), out.end(), [](const variable_info& a, const variable_info& b) { return a.name < b.name; });
    return out;
}

std::vector<variable_info> controller::list_children(uint32_t reference, size_t start, size_t count) {
    if (reference == 0 || reference > expandables_.size()) return {};
    const expandable entry = expandables_[reference - 1];
    if (entry.kind == expandable::kind_t::globals) return list_globals();
    if (entry.kind == expandable::kind_t::locals) return list_locals(entry.frame);
    const script_value target = expansion_target(entry.value);
    const std::string base = postfix_base(entry.evaluate_name);
    const size_t take = count ? std::min(count, k_children_limit) : k_children_limit;
    std::vector<variable_info> out;
    if (target.raw_storage_index() == script_value::TYPEID_ARRAY) {
        const auto* node = target.unchecked_array_node();
        for (size_t i = start; i < node->size() && out.size() < take; ++i) {
            const std::string index = "[" + std::to_string(i) + "]";
            out.push_back(row(index, node->get(i, engine_), entry.frame, entry.evaluate_name.empty() ? std::string() : base + index));
        }
    } else if (target.raw_storage_index() == script_value::TYPEID_MAP) {
        size_t at = 0;
        for (const auto& [key, value] : target.unchecked_as_map()) {
            if (at++ < start) continue;
            if (out.size() >= take) break;
            const std::string literal = key_literal(key);
            out.push_back(row(literal.empty() ? display_text(key) : literal, value, entry.frame,
                              entry.evaluate_name.empty() || literal.empty() ? std::string() : base + "[" + literal + "]"));
        }
    } else if (target.raw_storage_index() == script_value::TYPEID_OBJECT) {
        for (const auto& [field, value] : detail::shown_fields(target, engine_)) {
            out.push_back(row(field, value, entry.frame, entry.evaluate_name.empty() ? std::string() : base + "." + field));
        }
    }
    return out;
}

// A frame's names as an evaluation sees them: its locals, then `this` (as k_this_name) and the fields it names bare
std::vector<std::pair<std::string, script_value>> controller::frame_scope(size_t frame) const {
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    std::vector<std::pair<std::string, script_value>> out;
    if (!backend || !backend->is_executing()) return out;
    out = backend->get_frame_locals(frame);
    const auto self = std::find_if(out.begin(), out.end(), [](const auto& entry) { return entry.first == "this"; });
    if (self == out.end()) return out;
    self->first = k_this_name;
    const script_value instance = expansion_target(self->second);
    if (instance.raw_storage_index() == script_value::TYPEID_OBJECT) {
        for (auto& [field, value] : detail::shown_fields(instance, engine_)) {
            if (std::none_of(out.begin(), out.end(), [&](const auto& entry) { return entry.first == field; })) { out.emplace_back(std::move(field), std::move(value)); }
        }
    }
    return out;
}

script_value controller::run_evaluation(const std::string& text, size_t frame, evaluation_mode mode) {
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    if (!backend) { throw runtime_error("No script is running"); }
    const auto scope = frame_scope(frame);
    const auto self = std::find_if(scope.begin(), scope.end(), [](const auto& entry) { return entry.first == k_this_name; });
    const class_definition* self_class = nullptr;
    if (self != scope.end()) {
        const script_value instance = expansion_target(self->second);
        if (instance.raw_storage_index() == script_value::TYPEID_OBJECT) {
            if (const auto held = const_cast<script_value&>(instance).get_class_instance()) { self_class = held->get_class_definition(); }
        }
    }
    std::string source = self != scope.end() ? with_this_renamed(text, *engine_, self_class, scope) : text;
    // the checker reads a held object's class into the unit (a generic method's instance it calls): the classes join the text the
    // unit is cached under, so a frame of another class checks its own
    std::string classes;
    for (const auto& [name, value] : scope) {
        const script_value instance = expansion_target(value);
        if (instance.raw_storage_index() != script_value::TYPEID_OBJECT) { continue; }
        const auto held = const_cast<script_value&>(instance).get_class_instance();
        if (const class_definition* cls = held ? held->get_class_definition() : nullptr; cls && cls->is_script_class()) { classes += " " + name + ":" + cls->get_name(); }
    }
    if (!classes.empty()) { source += "\n//" + classes; }
    if (mode == evaluation_mode::watch) {
        const std::string refused = watch_text_refusal(source, *engine_);
        if (!refused.empty()) { throw runtime_error("A watch only reads: " + refused + " changes state (evaluate it in the Debug Console)"); }
    }
    instance_variables names(scope.begin(), scope.end());
    const instance_variables before = names;
    ++evaluating_;
    const bool outer_read_only = evaluation_read_only_;
    evaluation_read_only_ = mode == evaluation_mode::watch;
    hot_step_or_pause_.store(true);   // every statement the evaluation runs reaches on_statement
    const auto budget = std::chrono::nanoseconds(evaluation_budget_.load(std::memory_order_relaxed));
    backend->begin_debug_evaluation(budget);
    script_value result(std::monostate{}, engine_);
    std::string failure;
    try {
        result = detail::engine_access::debug_evaluate(*engine_, source, names);
    } catch (const std::exception& e) {
        failure = e.what();
        if (failure.find("execution budget exceeded") != std::string::npos) {
            failure = "The evaluation ran past its budget of " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(budget).count()) + " ms";
        }
    }
    backend->end_debug_evaluation();
    evaluation_read_only_ = outer_read_only;
    --evaluating_;
    hot_step_or_pause_.store(pending_pause_.load(std::memory_order_relaxed) != 0
        || step_mode_.load(std::memory_order_relaxed) != static_cast<int>(step_mode::none) || sync_requested_.load());
    if (!failure.empty()) { throw runtime_error(failure); }
    if (mode == evaluation_mode::console) {
        const auto self = names.find(std::string(k_this_name));
        for (auto& [name, now] : names) {
            if (name == k_this_name || same_value(now, before.at(name))) continue;
            if (script_value* storage = backend->debug_local_storage(name, frame)) {
                auto stored = detail::variable_store(storage->deref(), now, engine_, detail::slot_site{detail::slot_kind::assignment, name});
                if (!stored) { throw_store_failure(stored, *engine_); }
            } else if (self != names.end()) {   // a field the frame's `this` names bare: the script's own field store takes it
                instance_variables field_write{{std::string(k_this_name), self->second}, {"__jai_value", now}};
                detail::engine_access::debug_evaluate(*engine_, std::string(k_this_name) + "." + name + " = __jai_value;", field_write);
            }
        }
    }
    return result;
}

variable_info controller::evaluate(const std::string& text, size_t frame, evaluation_mode mode) {
    const script_value result = run_evaluation(text, frame, mode);
    return row(text, result, frame, text);
}

variable_info controller::set_variable(uint32_t reference, const std::string& name, const std::string& value, size_t frame) {
    if (reference == 0 || reference > expandables_.size()) { throw runtime_error("This variable is from before the script last ran"); }
    const expandable entry = expandables_[reference - 1];
    std::string target;
    if (entry.kind == expandable::kind_t::globals) {
        target = "::" + name;
    } else if (entry.kind == expandable::kind_t::locals) {
        target = name == "this" ? std::string() : name;
        frame = entry.frame;
    } else {
        frame = entry.frame;
        for (const auto& child : list_children(reference)) {
            if (child.name == name) { target = child.evaluate_name; break; }
        }
    }
    if (target.empty()) { throw runtime_error("'" + name + "' cannot be assigned"); }
    run_evaluation(target + " = (" + value + ");", frame, evaluation_mode::console);
    variable_info out = row(name, run_evaluation(target, frame, evaluation_mode::watch), frame, target);
    return out;
}

// ---------------------------------------------------------------------------
// Hot path
// ---------------------------------------------------------------------------

// Runs only when wants_statement() passed (stepping, or the line bloom matched) — the
// snapshot is the script thread's own cache, so no atomics/locks even here. The bloom
// re-check keeps a step-over across a hot region from hashing the filename per statement.
bool controller::breakpoint_hit(const std::string& file, int line) const {
    const uint64_t* bloom = hot_line_bloom_;
    if (!bloom) return false;
    const uint32_t b = static_cast<uint32_t>(line) & (line_bloom_bits - 1);
    if (!((bloom[b >> 6] >> (b & 63u)) & 1u)) return false;
    auto it = hot_bound_bps_.find(file);
    if (it == hot_bound_bps_.end()) return false;
    return std::binary_search(it->second.begin(), it->second.end(), line);
}

bool controller::sync_hot_state() {
    sync_requested_.exchange(false);   // before the reads: a change raised after it syncs again
    if (!enabled_.load(std::memory_order_acquire)) {
        hot_step_or_pause_.store(false, std::memory_order_relaxed);   // the gate is not consulted while disarmed
        hot_line_bloom_ = nullptr;
        hot_line_bloom_storage_.clear();
        // Disarming hides the hot cache, not the installed table or pending edits.
        hot_bp_version_ = ~0ull;   // a re-armed session rebuilds even an unchanged bloom
        return false;
    }
    hot_step_or_pause_.store(pending_pause_.load(std::memory_order_relaxed) != 0 || evaluating_ > 0
        || step_mode_.load(std::memory_order_relaxed) != static_cast<int>(step_mode::none));
    if (sync_requested_.load()) { hot_step_or_pause_.store(true); }   // raised between the exchange and the store above
    normalize_sources_.store(engine_->is_enabled(jai::permissions::filesystem), std::memory_order_relaxed);
    const uint64_t version = bp_version_.load(std::memory_order_acquire);
    const uint64_t lines_version = stop_lines_version_.load(std::memory_order_acquire);
    if (version != hot_bp_version_ || lines_version != hot_stop_lines_version_) {
        auto snapshot = breakpoints_.load(std::memory_order_acquire);
        auto installed = hot_bps_;
        if (!installed || snapshot->sequence != applied_bp_sequence_) {
            auto next = installed ? std::make_shared<breakpoint_table>(*installed)
                                  : std::make_shared<breakpoint_table>();
            if (snapshot->clear_sequence > applied_bp_sequence_) { next->clear(); installed_rich_.clear(); }

            struct pending_record {
                const std::string* file;
                const breakpoint_record* record;
                bool source_path;
            };
            std::vector<pending_record> changed;
            auto collect = [&](const auto& records, bool source_path) {
                for (const auto& [file, record] : records) {
                    if (record.sequence > applied_bp_sequence_) {
                        changed.push_back({&file, &record, source_path});
                    }
                }
            };
            collect(snapshot->native, false);
            collect(snapshot->source, true);
            std::sort(changed.begin(), changed.end(), [](const auto& a, const auto& b) {
                return a.record->sequence < b.record->sequence;
            });
            for (const auto& change : changed) {
                const std::string file = change.source_path && engine_->is_enabled(jai::permissions::filesystem)
                    ? normalize_bp_path(*change.file) : *change.file;
                if (change.record->lines.empty()) next->erase(file);
                else (*next)[file] = change.record->lines;
                if (change.record->rich.empty()) installed_rich_.erase(file);
                else installed_rich_[file] = change.record->rich;
            }
            installed = std::move(next);
        }

        breakpoint_table bound;
        std::unordered_map<std::string, std::unordered_map<int, breakpoint>> bound_rich;
        {
            std::lock_guard<std::mutex> lk(sources_mtx_);
            for (const auto& [file, rich] : installed_rich_) {
                const auto known = stop_lines_.find(file);
                for (const breakpoint& bp : rich) {
                    int line = bp.line;
                    if (known != stop_lines_.end()) {
                        const auto next = std::lower_bound(known->second.begin(), known->second.end(), line);
                        if (next == known->second.end()) { continue; }
                        line = *next;
                    }
                    bound_rich[file][line] = bp;
                }
            }
            for (const auto& [file, lines] : *installed) {
                const auto known = stop_lines_.find(file);
                auto& at = bound[file];
                for (int line : lines) {
                    if (known == stop_lines_.end()) { at.push_back(line); continue; }
                    const auto next = std::lower_bound(known->second.begin(), known->second.end(), line);
                    if (next != known->second.end()) { at.push_back(*next); }
                }
                std::sort(at.begin(), at.end());
                at.erase(std::unique(at.begin(), at.end()), at.end());
            }
        }
        // Commit table, bloom and sequence together after all fallible work. A
        // failed rebuild leaves the previous installed table available for retry.
        std::vector<uint64_t> bloom;
        if (!bound.empty()) {
            bloom.assign(line_bloom_bits / 64, 0);
            for (const auto& [file, lines] : bound) {
                for (int line : lines) {
                    const uint32_t b = static_cast<uint32_t>(line) & (line_bloom_bits - 1);
                    bloom[b >> 6] |= (uint64_t{1} << (b & 63u));
                }
            }
        }
        hot_bps_ = std::move(installed);
        hot_bound_bps_ = std::move(bound);
        hot_rich_bps_ = std::move(bound_rich);
        hot_stop_lines_version_ = lines_version;
        hot_line_bloom_storage_.swap(bloom);
        hot_line_bloom_ = hot_line_bloom_storage_.empty() ? nullptr : hot_line_bloom_storage_.data();
        applied_bp_sequence_ = snapshot->sequence;
        hot_bp_version_ = version;
    }
    return true;
}

void controller::swap_step_state(int& mode, int& base_depth, int& stop_depth) noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    mode = step_mode_.exchange(mode, std::memory_order_relaxed);
    base_depth = step_base_depth_.exchange(base_depth, std::memory_order_relaxed);
    stop_depth = current_stop_depth_.exchange(stop_depth, std::memory_order_relaxed);
    hot_step_or_pause_.store(enabled_.load(std::memory_order_acquire) && (pending_pause_.load(std::memory_order_relaxed) != 0
        || step_mode_.load(std::memory_order_relaxed) != static_cast<int>(step_mode::none) || sync_requested_.load()), std::memory_order_relaxed);
}

bool controller::should_stop(const ast_node* node, int depth, std::string& reason_out) {
    const int line = static_cast<int>(node->location.line);
    // a breakpoint's condition and logpoint run on every hit, a step landing on its line included
    const bool breakpoint_stops = breakpoint_hit(node->location.filename, line) && breakpoint_fires(node, line);
    if (pending_pause_.load(std::memory_order_relaxed)) {
        reason_out = "pause";
        return true;
    }
    switch (static_cast<step_mode>(step_mode_.load(std::memory_order_relaxed))) {
        case step_mode::into: reason_out = "step"; return true;
        case step_mode::over:
            if (depth <= step_base_depth_.load(std::memory_order_relaxed)) { reason_out = "step"; return true; }
            break;
        case step_mode::out:
            if (depth < step_base_depth_.load(std::memory_order_relaxed)) { reason_out = "step"; return true; }
            break;
        case step_mode::none: break;
    }
    if (breakpoint_stops) {
        reason_out = "breakpoint";
        return true;
    }
    return false;
}

// A hit of a bound breakpoint line: a plain one stops; a conditional one stops while its condition holds and its hit condition
// takes the count; a logpoint prints and goes on. An expression that fails stops with its error printed.
bool controller::breakpoint_fires(const ast_node* node, int line) {
    const auto file = hot_rich_bps_.find(node->location.filename);
    if (file == hot_rich_bps_.end()) return true;
    const auto found = file->second.find(line);
    if (found == file->second.end()) return true;
    const breakpoint bp = found->second;   // an evaluation may resync the table
    std::string failure;
    try {
        if (!bp.condition.empty()) {
            const script_value held = run_evaluation(bp.condition, 0, evaluation_mode::watch);
            const script_value& value = held.deref();
            const bool truth = value.is_bool() ? value.as_bool() : value.is_int() ? value.as_int() != 0 : !value.is_null();
            if (!truth) return false;
        }
        const uint64_t hits = ++hit_counts_[node->location.filename][line];
        if (!bp.hit_condition.empty()) {
            std::string_view text = bp.hit_condition;
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            std::string op;
            while (!text.empty() && (text.front() == '=' || text.front() == '>' || text.front() == '<' || text.front() == '%')) { op += text.front(); text.remove_prefix(1); }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            uint64_t n = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), n);
            if (parsed.ec != std::errc{}) { throw runtime_error("A hit condition is a count with an optional ==, >=, >, <=, < or %: '" + bp.hit_condition + "'"); }
            const bool met = op.empty() || op == "==" ? hits == n : op == ">=" ? hits >= n : op == ">" ? hits > n
                           : op == "<=" ? hits <= n : op == "<" ? hits < n : op == "%" ? (n != 0 && hits % n == 0) : false;
            if (!met) return false;
        }
        if (!bp.log_message.empty()) {
            const std::string text = log_text(bp.log_message);
            std::function<void(const std::string&)> out;
            { std::lock_guard<std::mutex> lk(mtx_); out = on_output_; }
            if (out) out(text + "\n");
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        failure = e.what();
    }
    std::function<void(const std::string&)> out;
    { std::lock_guard<std::mutex> lk(mtx_); out = on_output_; }
    if (out) out("breakpoint at " + node->location.filename + ":" + std::to_string(line) + ": " + failure + "\n");
    return true;
}

// A logpoint's message with each {expression} replaced by its value as print shows it ({{ and }} are braces)
std::string controller::log_text(const std::string& message) {
    std::string out;
    for (size_t i = 0; i < message.size(); ++i) {
        const char c = message[i];
        if ((c == '{' || c == '}') && i + 1 < message.size() && message[i + 1] == c) { out += c; ++i; continue; }
        if (c != '{') { out += c; continue; }
        const size_t close = message.find('}', i + 1);
        if (close == std::string::npos) { out += message.substr(i); break; }
        try {
            out += printed_text(run_evaluation(message.substr(i + 1, close - i - 1), 0, evaluation_mode::watch));
        } catch (const std::exception& e) {
            out += std::string("<") + e.what() + ">";
        }
        i = close;
    }
    return out;
}

bool controller::stops_at(const ast_node* node) noexcept {
    switch (node->get_type()) {
        case node_type::block_stmt:
        case node_type::case_stmt:
        case node_type::default_stmt:
        case node_type::statement_decl:
        case node_type::generic_decl:
        case node_type::generic_instance_decl:
            return false;
        default:
            return true;
    }
}

bool controller::on_statement(const ast_node* node, int depth) {
    // An evaluation never stops; one that only reads runs no script code but its own
    if (evaluating_) return !evaluation_read_only_ || node->location.filename == evaluation_source;
    // Reentrancy: a repl/inspection closure that runs script code must not itself trip
    // breakpoints or stepping (we'd park recursively). Same thread, so a plain flag.
    if (in_command_) return true;
    // An armed session's next statement applies a pending breakpoint edit, pause or detach
    if (sync_requested_.load(std::memory_order_relaxed)) [[unlikely]] { detail::engine_access::get_execution_backend(*engine_)->sync_debug_hook(); }
    if (!stops_at(node)) return true;
    const uint64_t detaches = detaches_.load(std::memory_order_acquire);
    // A detach can race the cached gate; never park for a session that just ended.
    if (!enabled_.load(std::memory_order_relaxed)) return true;
    std::string reason;
    if (!should_stop(node, depth, reason)) return true;
    // Consume a one-shot pause so it fires exactly once.
    pending_pause_.store(0, std::memory_order_relaxed);
    park({reason, node->location.filename, static_cast<int>(node->location.line), depth}, detaches);
    return true;
}

void controller::on_exception(const std::string& message, bool caught, int depth) {
    if (evaluating_ || in_command_) return;
    if (!caught && reports_uncaught_.load(std::memory_order_relaxed)) {
        std::function<void(const std::string&)> report;
        { std::lock_guard<std::mutex> lk(mtx_); report = on_uncaught_error_; }
        if (report) report(message);
    }
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (!break_on_throw_.load(std::memory_order_relaxed) && (caught || !break_on_uncaught_.load(std::memory_order_relaxed))) return;
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    const uint64_t detaches = detaches_.load(std::memory_order_acquire);
    backend->debug_settle_statement();
    const auto frames = backend->debug_frames();
    stop_info at{"exception", frames.empty() ? std::string() : frames.front().file, frames.empty() ? 0 : static_cast<int>(frames.front().line), depth, message};
    park(at, detaches);
}

void controller::park(const stop_info& si, uint64_t detaches_at_entry) {
    std::function<void(const stop_info&)> notify;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (detaches_.load(std::memory_order_relaxed) != detaches_at_entry) { return; }
        resume_requested_ = false;
        notify = on_stopped_;
    }
    current_stop_ = si;   // read by list_frames/list_locals via posted closures (this thread)
    expandables_.clear();
    current_stop_depth_.store(si.depth, std::memory_order_relaxed);
    paused_.store(true, std::memory_order_release);
    if (notify) notify(si);   // notify transport (runs on this, the script thread)

    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        cv_.wait(lk, [&] { return resume_requested_ || !commands_.empty(); });
        while (!commands_.empty()) {
            auto fn = std::move(commands_.front());
            commands_.pop_front();
            lk.unlock();
            in_command_ = true;
            fn();               // inspection/edit against live engine state, on this thread
            in_command_ = false;
            lk.lock();
        }
        if (resume_requested_) break;
    }
    paused_.store(false, std::memory_order_release);
    lk.unlock();
    expandables_.clear();   // the stop's values go back on this thread before the script runs on
    // Park exit is a debug sync point: a step mode set by the resume side and any
    // breakpoints edited during the pause must be live from the very next statement,
    // and a detach during the pause disarms the backend's gate at once.
    detail::engine_access::get_execution_backend(*engine_)->sync_debug_hook();
}

void controller::service() {
    if (!sync_requested_.load(std::memory_order_relaxed)) { return; }
    sleep_for(std::chrono::nanoseconds{0});
}

void controller::sleep_for(std::chrono::nanoseconds duration) {
    using clock = std::chrono::steady_clock;
    auto until = clock::now() + duration;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (!cv_.wait_until(lk, until, [&] { return sync_requested_.load(); })) return;
        }
        auto* backend = detail::engine_access::get_execution_backend(*engine_);
        backend->sync_debug_hook();
        const uint64_t detaches = detaches_.load(std::memory_order_acquire);
        if (in_command_ || !enabled_.load(std::memory_order_relaxed) || !backend->is_executing()) continue;
        if (!pending_pause_.exchange(0, std::memory_order_relaxed)) continue;
        const auto remaining = until - clock::now();
        backend->debug_settle_statement();
        const auto frames = backend->debug_frames();
        park({"pause", frames.empty() ? std::string() : frames.front().file,
              frames.empty() ? 0 : static_cast<int>(frames.front().line), backend->debug_depth()}, detaches);
        until = clock::now() + remaining;   // the pause does not eat into the sleep
    }
}

} // namespace jai::debug
