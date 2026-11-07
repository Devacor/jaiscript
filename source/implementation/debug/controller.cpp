#include <jaiscript/debug/controller.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/detail/ast.hpp>
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
        for (const auto& init : fd->initializers) { for (const auto& a : init.arguments) { expr(a); } }
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

void controller::publish_breakpoints(const std::string& file, std::vector<int> lines, bool source_path) {
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    auto current = breakpoints_.load(std::memory_order_acquire);
    auto next = std::make_shared<breakpoint_snapshot>(*current);
    auto& records = source_path ? next->source : next->native;
    records[file] = breakpoint_record{std::move(lines), ++next->sequence};
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
    if (path.empty() || path == "<script>") return;
    normalize_sources_.store(engine_->is_enabled(jai::filesystem), std::memory_order_relaxed);
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
    if (script_value* local = backend->debug_local_storage(name)) { return local; }
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
    auto stored = detail::variable_store(storage->deref(), assigned, engine_);
    if (!stored) { throw runtime_error(stored.message().empty() ? stored.error().message() : std::string(stored.message())); }
}


namespace {

// A Variables row shows the structural display text (no to_string() runs on a parked thread), cut short past this many bytes
constexpr size_t k_variable_text_limit = 1000;

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

// A list leaves callables out (noise in a Variables view); text past the limit is cut, a string shows quoted
std::vector<variable_info> rendered_variables(const std::vector<std::pair<std::string, script_value>>& named, bool list = true) {
    std::vector<variable_info> out;
    for (const auto& [name, value] : named) {
        if (list && value.is_function()) continue;
        const script_value& held = value.deref();
        std::string text = held.is_string() ? quoted_text(held.unchecked_as_string())
                         : held.is_utf8_string() ? quoted_text(held.unchecked_as_utf8().bytes)
                         : detail::plain_text_value(value) ? value.to_string() : detail::value_text(value, {}, k_variable_text_limit).value();
        out.push_back(variable_info{ name, std::move(text), variable_kind(value) });
    }
    std::sort(out.begin(), out.end(),
              [](const variable_info& a, const variable_info& b) { return a.name < b.name; });
    return out;
}

// The scopes between the innermost environment and the program scope, an inner name hiding an outer one
std::vector<std::pair<std::string, script_value>> block_scope_variables(const environment* env, const environment* program) {
    std::vector<std::pair<std::string, script_value>> out;
    std::unordered_set<std::string_view> seen;
    for (; env && env != program; env = env->get_parent().get()) {
        for (const auto& [name, value] : env->get_local_variables()) {
            if (seen.insert(name).second) { out.emplace_back(std::string(name), value); }
        }
    }
    return out;
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

std::vector<variable_info> controller::list_locals(size_t frame) const {
    auto* backend = detail::engine_access::get_execution_backend(*engine_);
    if (!backend) return {};
    // A frame's slot locals come outermost first; an inner declaration hides an outer one of its name. The top level's block
    // scopes are environments, reachable only while it is the innermost frame.
    const auto frames = backend->debug_frames();
    const bool top_level = frames.empty() || (frame + 1 == frames.size() && frames.back().function == "<script>");
    if (top_level && frame != 0) return {};
    auto named = backend->get_frame_locals(frame);
    for (size_t i = named.size(); i-- > 0;) {
        const auto hidden = std::find_if(named.begin() + static_cast<std::ptrdiff_t>(i) + 1, named.end(), [&](const auto& later) { return later.first == named[i].first; });
        if (hidden != named.end()) { named.erase(named.begin() + static_cast<std::ptrdiff_t>(i)); }
    }
    if (!top_level) return rendered_variables(named);
    const auto env = backend->get_current_environment();
    const auto program = detail::engine_access::get_global_environment(*engine_);
    for (auto& scoped : block_scope_variables(env.get(), program.get())) {
        if (std::none_of(named.begin(), named.end(), [&](const auto& n) { return n.first == scoped.first; })) { named.push_back(std::move(scoped)); }
    }
    return rendered_variables(named);
}

// The program's own variables: the engine's internals (__class_*) and what the host registered (add_global) stay out
std::vector<variable_info> controller::list_globals() const {
    std::vector<std::pair<std::string, script_value>> named;
    if (const auto program = detail::engine_access::get_global_environment(*engine_)) {
        for (const auto& [name, value] : program->get_local_variables()) {
            std::string text(name);
            if (text.starts_with("__") || detail::engine_access::host_global_registered(*engine_, text)) { continue; }
            named.emplace_back(std::move(text), value);
        }
    }
    return rendered_variables(named);
}

variable_info controller::evaluate(const std::string& path) const {
    auto is_name_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    size_t at = 0;
    auto name = [&]() {
        const size_t start = at;
        while (at < path.size() && is_name_char(path[at])) { ++at; }
        if (at == start || std::isdigit(static_cast<unsigned char>(path[start]))) { throw runtime_error("Only a variable, its .fields and its [index] or [\"key\"] elements can be evaluated"); }
        return path.substr(start, at - start);
    };
    script_value value = get_variable(name());
    while (at < path.size()) {
        const script_value& held = value.deref();
        if (path[at] == '.') {
            ++at;
            const std::string field = name();
            const auto* holder = held.raw_storage_index() == script_value::TYPEID_OBJECT ? held.peek_object_holder() : nullptr;
            const script_value* found = holder && holder->is_instance()
                ? holder->unchecked_instance()->find_script_field_value(detail::engine_access::get_symbolizer(*engine_)->intern(field)) : nullptr;
            if (!found) { throw runtime_error("'" + path.substr(0, at - field.size() - 1) + "' has no field '" + field + "'"); }
            value = *found;
        } else if (path[at] == '[') {
            const size_t close = path.find(']', at);
            if (close == std::string::npos) { throw runtime_error("Expected ']' in " + path); }
            const std::string key = path.substr(at + 1, close - at - 1);
            const std::string container = path.substr(0, at);
            at = close + 1;
            const bool quoted = key.size() >= 2 && key.front() == '"' && key.back() == '"';
            if (held.raw_storage_index() == script_value::TYPEID_ARRAY && !quoted) {
                const auto* node = held.unchecked_array_node();
                size_t index = 0;
                const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
                if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index >= node->size()) {
                    throw runtime_error("'" + container + "' has no element [" + key + "]");
                }
                value = node->get(index, engine_);
            } else if (held.raw_storage_index() == script_value::TYPEID_MAP) {
                script_value k = quoted ? script_value(key.substr(1, key.size() - 2), engine_) : script_value(static_cast<script_int>(std::strtoll(key.c_str(), nullptr, 10)), engine_);
                const auto& map = held.unchecked_as_map();
                const auto found = map.find(k);
                if (found == map.end()) { throw runtime_error("'" + container + "' has no key " + key); }
                value = found->second;
            } else {
                throw runtime_error("'" + container + "' is not an array or a map");
            }
        } else {
            throw runtime_error("Only a variable, its .fields and its [index] or [\"key\"] elements can be evaluated");
        }
    }
    auto rendered = rendered_variables({{path, value}}, false);
    return rendered.front();
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
    hot_step_or_pause_.store(pending_pause_.load(std::memory_order_relaxed) != 0
        || step_mode_.load(std::memory_order_relaxed) != static_cast<int>(step_mode::none));
    if (sync_requested_.load()) { hot_step_or_pause_.store(true); }   // raised between the exchange and the store above
    normalize_sources_.store(engine_->is_enabled(jai::filesystem), std::memory_order_relaxed);
    const uint64_t version = bp_version_.load(std::memory_order_acquire);
    const uint64_t lines_version = stop_lines_version_.load(std::memory_order_acquire);
    if (version != hot_bp_version_ || lines_version != hot_stop_lines_version_) {
        auto snapshot = breakpoints_.load(std::memory_order_acquire);
        auto installed = hot_bps_;
        if (!installed || snapshot->sequence != applied_bp_sequence_) {
            auto next = installed ? std::make_shared<breakpoint_table>(*installed)
                                  : std::make_shared<breakpoint_table>();
            if (snapshot->clear_sequence > applied_bp_sequence_) next->clear();

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
                const std::string file = change.source_path && engine_->is_enabled(jai::filesystem)
                    ? normalize_bp_path(*change.file) : *change.file;
                if (change.record->lines.empty()) next->erase(file);
                else (*next)[file] = change.record->lines;
            }
            installed = std::move(next);
        }

        breakpoint_table bound;
        {
            std::lock_guard<std::mutex> lk(sources_mtx_);
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

bool controller::should_stop(const ast_node* node, int depth, std::string& reason_out) const {
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
    if (breakpoint_hit(node->location.filename, static_cast<int>(node->location.line))) {
        reason_out = "breakpoint";
        return true;
    }
    return false;
}

bool controller::stops_at(const ast_node* node) noexcept {
    switch (node->get_type()) {
        case node_type::block_stmt:
        case node_type::case_stmt:
        case node_type::default_stmt:
        case node_type::statement_decl:
            return false;
        default:
            return true;
    }
}

void controller::on_statement(const ast_node* node, int depth) {
    // Reentrancy: a repl/inspection closure that runs script code must not itself trip
    // breakpoints or stepping (we'd park recursively). Same thread, so a plain flag.
    if (in_command_) return;
    // An armed session's next statement applies a pending breakpoint edit, pause or detach
    if (sync_requested_.load(std::memory_order_relaxed)) [[unlikely]] { detail::engine_access::get_execution_backend(*engine_)->sync_debug_hook(); }
    if (!stops_at(node)) return;
    const uint64_t detaches = detaches_.load(std::memory_order_acquire);
    // A detach can race the cached gate; never park for a session that just ended.
    if (!enabled_.load(std::memory_order_relaxed)) return;
    std::string reason;
    if (!should_stop(node, depth, reason)) return;
    // Consume a one-shot pause so it fires exactly once.
    pending_pause_.store(0, std::memory_order_relaxed);
    park({reason, node->location.filename, static_cast<int>(node->location.line), depth}, detaches);
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
        const auto frames = backend->debug_frames();
        park({"pause", frames.empty() ? std::string() : frames.front().file,
              frames.empty() ? 0 : static_cast<int>(frames.front().line), backend->debug_depth()}, detaches);
        until = clock::now() + remaining;   // the pause does not eat into the sleep
    }
}

} // namespace jai::debug
