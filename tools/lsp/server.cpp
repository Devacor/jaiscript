#include "server.hpp"
#include "analysis_internal.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace jai_lsp {

namespace {

constexpr int k_method_not_found = -32601;
constexpr int k_request_failed = -32803;
constexpr int k_request_cancelled = -32800;
constexpr int k_server_not_initialized = -32002;
constexpr int k_parse_error = -32700;
constexpr int k_invalid_request = -32600;

constexpr size_t k_max_header_line = 64 * 1024;

// A line longer than k_max_header_line is consumed but not kept: `too_long` reports it
bool read_line(std::string& out, bool& too_long) {
	out.clear();
	too_long = false;
	for (;;) {
		int c = std::fgetc(stdin);
		if (c == EOF) { return !out.empty() || too_long; }
		if (c == '\n') { break; }
		if (out.size() < k_max_header_line) {
			out += static_cast<char>(c);
		} else {
			too_long = true;
		}
	}
	if (!out.empty() && out.back() == '\r') { out.pop_back(); }
	return true;
}

// The value of a Content-Length header found anywhere in the line (so a header glued to a skipped body still frames);
// nullopt for a missing, signed, empty, non-numeric or past-size_t value
std::optional<size_t> content_length_in(const std::string& line) {
	std::string lower = line;
	for (auto& ch : lower) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
	const std::string key = "content-length:";
	size_t at = lower.find(key);
	if (at == std::string::npos) { return std::nullopt; }
	size_t i = at + key.size();
	while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) { ++i; }
	size_t end = i;
	while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) { ++end; }
	size_t trail = end;
	while (trail < line.size() && (line[trail] == ' ' || line[trail] == '\t')) { ++trail; }
	if (end == i || trail != line.size()) { return std::nullopt; }
	size_t value = 0;
	for (size_t k = i; k < end; ++k) {
		const size_t digit = static_cast<size_t>(line[k] - '0');
		if (value > (SIZE_MAX - digit) / 10) { return std::nullopt; }
		value = value * 10 + digit;
	}
	return value;
}

bool discard_input(size_t count) {
	char sink[64 * 1024];
	while (count > 0) {
		size_t want = std::min(count, sizeof(sink));
		size_t got = std::fread(sink, 1, want, stdin);
		if (got == 0) { return false; }
		count -= got;
	}
	return true;
}

} // namespace

server::server(engine_factory make_engine, std::string host, std::vector<std::string> include_paths, bool log, size_t max_message)
	: make_engine_(std::move(make_engine)), host_(std::move(host)), include_paths_(std::move(include_paths)), log_(log), max_message_(max_message) {}

void server::log(const std::string& text) {
	if (!log_) { return; }
	std::fprintf(stderr, "[jaiscript_lsp] %s\n", text.c_str());
	std::fflush(stderr);
}

// A header block with no usable Content-Length is skipped (what follows is read as headers until one frames again);
// a body over max_message_ is read and dropped, never held
bool server::read_message(std::string& body) {
	for (;;) {
		std::optional<size_t> length;
		bool any = false;
		bool malformed = false;
		std::string line;
		for (;;) {
			bool too_long = false;
			if (!read_line(line, too_long)) { return false; }
			if (too_long) {
				malformed = true;
				any = true;
				continue;
			}
			if (line.empty()) {
				if (any) { break; }
				continue;
			}
			any = true;
			if (auto value = content_length_in(line)) {
				length = value;
			} else if (line.find(':') == std::string::npos) {
				malformed = true;
			}
		}
		if (malformed || !length) {
			log("skipped a message header without a usable Content-Length");
			continue;
		}
		if (*length > max_message_) {
			log("dropped a message of " + std::to_string(*length) + " bytes (limit " + std::to_string(max_message_) + ")");
			if (!discard_input(*length)) { return false; }
			continue;
		}
		body.resize(*length);
		size_t got = *length ? std::fread(body.data(), 1, *length, stdin) : 0;
		return got == *length;
	}
}

void server::read_loop() {
	std::string body;
	for (;;) {
		bool more = false;
		try {
			more = read_message(body);
		} catch (const std::exception& ex) {
			log(std::string("cannot read a message: ") + ex.what());
		}
		if (!more) { break; }
		json message;
		try {
			message = json::parse(body);
		} catch (const std::exception& ex) {
			log(std::string("bad message: ") + ex.what());
			reply_error(json(), k_parse_error, ex.what());
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const json& view = message;
			if (view["method"].str() == "$/cancelRequest") {
				// only a request still queued can be cancelled: ids of answered requests are not kept
				const json& params = view["params"];
				const std::string id = params.contains("id") ? params["id"].dump() : std::string();
				for (const auto& queued : queue_) {
					if (!id.empty() && queued.contains("id") && !queued["method"].str().empty() && queued["id"].dump() == id) {
						cancelled_.insert(id);
						break;
					}
				}
			} else {
				queue_.push_back(std::move(message));
			}
		}
		wake_.notify_one();
	}
	std::lock_guard<std::mutex> lock(mutex_);
	eof_ = true;
	wake_.notify_one();
}

void server::send(const json& message) {
	std::string body = message.dump();
	std::lock_guard<std::mutex> lock(out_mutex_);
	std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
	std::fwrite(header.data(), 1, header.size(), stdout);
	std::fwrite(body.data(), 1, body.size(), stdout);
	std::fflush(stdout);
}

void server::reply(const json& id, json result) {
	json m = json::object();
	m.set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
	send(m);
}

void server::reply_error(const json& id, int code, const std::string& message) {
	json err = json::object();
	err.set("code", code).set("message", message);
	json m = json::object();
	m.set("jsonrpc", "2.0").set("id", id).set("error", std::move(err));
	send(m);
}

void server::notify(const std::string& method, json params) {
	json m = json::object();
	m.set("jsonrpc", "2.0").set("method", method).set("params", std::move(params));
	send(m);
}

int server::run() {
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	std::thread reader([this] { read_loop(); });
	while (!exit_) {
		json message;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (queue_.empty() && idle_work() && ws_) {
				lock.unlock();
				flush_deferred_change();
				idle_step();
				continue;
			}
			wake_.wait(lock, [&] { return !queue_.empty() || eof_; });
			if (queue_.empty()) { break; }
			message = std::move(queue_.front());
			queue_.pop_front();
		}
		try {
			handle(message);
		} catch (const std::exception& ex) {
			log(std::string("handler failed: ") + ex.what());
			if (message.contains("id")) { reply_error(message["id"], k_request_failed, ex.what()); }
		}
	}
	reader.detach();
	return exit_code_;
}

bool server::next_is_change_of(const std::string& uri) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (queue_.empty()) { return false; }
	const json& next = queue_.front();
	return next["method"].str() == "textDocument/didChange" && next["params"]["textDocument"]["uri"].str() == uri;
}

void server::flush_deferred_change() {
	if (!deferred_change_) { return; }
	deferred_change change = std::move(*deferred_change_);
	deferred_change_.reset();
	ws_->change(change.path, std::move(change.text), change.version);
	dirty_.insert(change.path);
}

// A request's id is a string or an integer; a message with no method is a response (the server sends no requests, so
// nothing waits on one) unless it has no id either
static bool well_formed(const json& message) {
	if (!message.is_object()) { return false; }
	if (!message.contains("method")) { return message.contains("id"); }
	const json& method = message["method"];
	const json& id = message["id"];
	return method.is_string() && !method.str().empty() && (!message.contains("id") || id.is_string() || id.is_integer());
}

void server::handle(const json& message) {
	if (!well_formed(message)) {
		reply_error(json(), k_invalid_request, "not a JSON-RPC request, notification or response");
		return;
	}
	const std::string& method = message["method"].str();
	if (method.empty()) { return; }
	if (method != "textDocument/didChange") { flush_deferred_change(); }
	const bool is_request = message.contains("id");
	const json& id = message["id"];
	if (is_request) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (cancelled_.erase(id.dump())) {
			reply_error(id, k_request_cancelled, "cancelled");
			return;
		}
	}
	if (shutdown_ && method != "exit") {
		if (is_request) { reply_error(id, k_invalid_request, "the server is shutting down: only exit is served"); }
		return;
	}
	if (!ws_ && method != "initialize" && method != "exit") {
		if (is_request) { reply_error(id, k_server_not_initialized, "server not initialized"); }
		return;
	}
	if (ws_ && method == "initialize") {
		if (is_request) { reply_error(id, k_invalid_request, "the server is already initialized"); }
		return;
	}
	bool handled = true;
	auto start = std::chrono::steady_clock::now();
	std::string error;
	json result;
	if (method == "textDocument/prepareRename") {
		result = on_prepare_rename(message["params"], error);
	} else if (method == "textDocument/rename") {
		result = on_rename(message["params"], error);
	} else {
		result = dispatch(method, message["params"], handled);
	}
	if (log_) {
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		log(method + " " + std::to_string(ms) + " ms");
	}
	if (!is_request) { return; }
	if (!handled) { reply_error(id, k_method_not_found, "unsupported method: " + method); return; }
	if (!error.empty()) { reply_error(id, k_request_failed, error); return; }
	reply(id, std::move(result));
}

json server::dispatch(const std::string& method, const json& params, bool& handled) {
	if (method == "initialize") { return on_initialize(params); }
	if (method == "initialized") {
		ws_->scan_folders();
		for (const auto& path : std::vector<std::string>(dirty_.begin(), dirty_.end())) { mark_program_stale(path, false); }
		return nullptr;
	}
	if (method == "shutdown") { shutdown_ = true; return nullptr; }
	if (method == "exit") { exit_ = true; exit_code_ = shutdown_ ? 0 : 1; return nullptr; }
	if (method == "textDocument/didOpen") {
		const json& td = params["textDocument"];
		std::string path = path_of(td["uri"].str());
		uri_of_[path] = td["uri"].str();
		ws_->open(path, td["text"].str(), static_cast<int>(td["version"].integer()));
		dirty_.insert(path);
		mark_program_stale(path, false);
		return nullptr;
	}
	if (method == "textDocument/didChange") {
		// a burst of edits to one file parses once, at its last edit
		const std::string& uri = params["textDocument"]["uri"].str();
		std::string path = path_of(uri);
		const bool continues = deferred_change_ && deferred_change_->path == path;
		if (!continues) { flush_deferred_change(); }
		unit* u = ws_->find(path);
		if (!u) { return nullptr; }
		std::string text = continues ? std::move(deferred_change_->text) : u->text;
		deferred_change_.reset();
		for (const auto& change : params["contentChanges"].items()) { apply_change(change, text); }
		const int version = static_cast<int>(params["textDocument"]["version"].integer());
		if (next_is_change_of(uri)) {
			deferred_change_ = deferred_change{path, std::move(text), version};
			return nullptr;
		}
		ws_->change(path, std::move(text), version);
		dirty_.insert(path);
		return nullptr;
	}
	if (method == "textDocument/didSave") {
		std::string path = path_of(params["textDocument"]["uri"].str());
		dirty_.insert(path);
		mark_program_stale(path, false);
		return nullptr;
	}
	if (method == "textDocument/didClose") {
		std::string path = path_of(params["textDocument"]["uri"].str());
		ws_->close(path);
		stale_.insert(path);
		return nullptr;
	}
	if (method == "workspace/didChangeWatchedFiles") {
		for (const auto& change : params["changes"].items()) {
			std::string path = path_of(change["uri"].str());
			ws_->file_changed_on_disk(path);
			if (change["type"].integer() == 3) {
				published_.erase(path);
				json p = json::object();
				p.set("uri", uri_for(path)).set("diagnostics", json::array());
				notify("textDocument/publishDiagnostics", std::move(p));
			} else if (ws_->find(path)) {
				mark_program_stale(path, true);
			}
		}
		return nullptr;
	}
	if (method == "workspace/didChangeConfiguration" || method == "$/setTrace" || method == "window/workDoneProgress/cancel") { return nullptr; }
	if (method == "jaiscript/status") {
		json result = json::object();
		result.set("pending", dirty_.size() + stale_.size()).set("parses", ws_->parses());
		{
			std::lock_guard<std::mutex> lock(mutex_);
			result.set("cancelled", cancelled_.size());
		}
		json units = json::array();
		for (const unit* u : ws_->loaded_units()) {
			if (u->checked_epoch == UINT64_MAX) { continue; }
			json item = json::object();
			item.set("uri", uri_for(u->path)).set("diagnostics", u->diagnostics.size()).set("checkMs", u->last_check_ms).set("checks", u->checks).set("parsed", u->parse_ok);
			units.push(std::move(item));
		}
		result.set("units", std::move(units));
		return result;
	}
	if (method == "textDocument/hover") { return on_hover(params); }
	if (method == "textDocument/definition") { return on_definition(params); }
	if (method == "textDocument/references") { return on_references(params); }
	if (method == "textDocument/documentHighlight") {
		unit* u = doc(params);
		json out = json::array();
		if (!u) { return out; }
		for (const auto& o : ws_->highlights(*u, from_lsp(*u, params["position"]))) {
			json h = json::object();
			h.set("range", range_json(u->path, o.span)).set("kind", o.declaration ? 3 : 2);
			out.push(std::move(h));
		}
		return out;
	}
	if (method == "textDocument/completion") { return on_completion(params); }
	if (method == "textDocument/signatureHelp") { return on_signature_help(params); }
	if (method == "textDocument/documentSymbol") { return on_document_symbol(params); }
	if (method == "workspace/symbol") { return on_workspace_symbol(params); }
	handled = false;
	return nullptr;
}

void server::mark_program_stale(const std::string& path, bool include_self) {
	if (!ws_) { return; }
	for (const auto& p : ws_->program_units(path)) {
		if (p != path || include_self) { stale_.insert(p); }
	}
}

void server::idle_step() {
	if (!dirty_.empty()) {
		std::string path = *dirty_.begin();
		dirty_.erase(dirty_.begin());
		unit* u = ws_->get(path);
		if (!u) { return; }
		auto start = std::chrono::steady_clock::now();
		ws_->diagnostics(*u);
		publish(path);
		stale_.erase(path);
		if (log_) {
			log("diagnostics " + path + " " + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()) +
				" ms (check " + std::to_string(u->last_check_ms) + " ms)");
		}
		auto seen = surface_seen_.find(path);
		if (seen != surface_seen_.end() && seen->second != u->surface) { mark_program_stale(path, false); }
		surface_seen_[path] = u->surface;
		return;
	}
	if (!stale_.empty()) {
		std::string path = *stale_.begin();
		stale_.erase(stale_.begin());
		unit* u = ws_->get(path);
		if (!u) {
			published_.erase(path);
			json p = json::object();
			p.set("uri", uri_for(path)).set("diagnostics", json::array());
			notify("textDocument/publishDiagnostics", std::move(p));
			return;
		}
		ws_->diagnostics(*u);
		surface_seen_.emplace(path, u->surface);
		publish(path);
	}
}

void server::publish(const std::string& path) {
	unit* u = ws_->find(path);
	if (!u) { return; }
	json list = json::array();
	auto token_length = [&](text_pos start) -> uint32_t {
		size_t i = token_at_or_after(u->tokens, start);
		if (i < u->tokens.size() && u->tokens[i].pos == start && u->tokens[i].length) { return u->tokens[i].length; }
		std::string_view line = u->lines.line_text(u->text, start.line);
		uint32_t e = start.col;
		while (e < line.size() && (std::isalnum(static_cast<unsigned char>(line[e])) || line[e] == '_')) { ++e; }
		return e > start.col ? e - start.col : 1;
	};
	for (const auto& d : u->diagnostics) {
		text_pos start{d.line, d.col};
		uint32_t length = d.length ? d.length : token_length(start);
		json item = json::object();
		json range = json::object();
		range.set("start", to_lsp(*u, start)).set("end", to_lsp(*u, {d.line, d.col + length}));
		item.set("range", std::move(range));
		item.set("severity", d.error ? 1 : 2);
		item.set("source", d.source == "parse" ? "jaiscript parser" : "jaiscript");
		item.set("message", d.message);
		if (d.related_line != UINT32_MAX) {
			json rel = json::object();
			json loc = json::object();
			text_pos where{d.related_line, d.related_col};
			loc.set("uri", uri_for(path)).set("range", range_json(path, {where, {where.line, where.col + token_length(where)}}));
			rel.set("location", std::move(loc)).set("message", "declared here");
			json related = json::array();
			related.push(std::move(rel));
			item.set("relatedInformation", std::move(related));
		}
		list.push(std::move(item));
	}
	std::string text = list.dump();
	auto it = published_.find(path);
	if (!u->open && it != published_.end() && it->second == text) { return; }
	if (!u->open && it == published_.end() && list.size() == 0) { published_[path] = text; return; }
	published_[path] = text;
	json params = json::object();
	params.set("uri", uri_for(path));
	if (u->open) { params.set("version", u->version); }
	params.set("diagnostics", std::move(list));
	notify("textDocument/publishDiagnostics", std::move(params));
}

std::string server::path_of(const std::string& uri) {
	std::string path = canonical_path(uri_to_path(uri));
	uri_of_.emplace(path, uri);
	return path;
}

std::string server::uri_for(const std::string& path) {
	auto it = uri_of_.find(path);
	return it != uri_of_.end() ? it->second : path_to_uri(path);
}

unit* server::doc(const json& params) {
	std::string path = path_of(params["textDocument"]["uri"].str());
	return ws_->get(path);
}

text_pos server::from_lsp(unit& u, const json& position) {
	uint32_t line = static_cast<uint32_t>(position["line"].integer());
	uint32_t character = static_cast<uint32_t>(position["character"].integer());
	return {line, line_index::from_utf16(u.lines.line_text(u.text, line), character)};
}

json server::to_lsp(unit& u, text_pos p) {
	json j = json::object();
	j.set("line", p.line).set("character", line_index::to_utf16(u.lines.line_text(u.text, p.line), p.col));
	return j;
}

json server::range_json(const std::string& path, text_span span) {
	unit* u = ws_->find(path);
	if (!u) { u = ws_->get(path); }
	json r = json::object();
	if (!u) {
		json s = json::object();
		s.set("line", span.start.line).set("character", span.start.col);
		json e = json::object();
		e.set("line", span.end.line).set("character", span.end.col);
		r.set("start", std::move(s)).set("end", std::move(e));
		return r;
	}
	r.set("start", to_lsp(*u, span.start)).set("end", to_lsp(*u, span.end));
	return r;
}

json server::location_json(const location& loc) {
	json j = json::object();
	j.set("uri", uri_for(loc.path)).set("range", range_json(loc.path, loc.span));
	return j;
}

void server::apply_change(const json& change, std::string& text) {
	if (!change.contains("range")) {
		text = change["text"].str();
		return;
	}
	line_index lines;
	lines.build(text);
	auto offset_of = [&](const json& p) {
		uint32_t line = static_cast<uint32_t>(p["line"].integer());
		if (line >= lines.line_count()) { return text.size(); }
		std::string_view lt = lines.line_text(text, line);
		uint32_t col = line_index::from_utf16(lt, static_cast<uint32_t>(p["character"].integer()));
		return lines.line_start(line) + col;
	};
	size_t a = offset_of(change["range"]["start"]);
	size_t b = offset_of(change["range"]["end"]);
	if (a > b) { std::swap(a, b); }
	a = std::min(a, text.size());
	b = std::min(b, text.size());
	text.replace(a, b - a, change["text"].str());
}

json server::on_initialize(const json& params) {
	const json& options = params["initializationOptions"];
	if (options["host"].is_string() && !options["host"].str().empty()) { host_ = options["host"].str(); }
	ws_ = std::make_unique<workspace>(make_engine_(host_));
	for (const auto& p : include_paths_) { ws_->add_include_path(p); }
	for (const auto& p : options["includePaths"].items()) { if (p.is_string() && !p.str().empty()) { ws_->add_include_path(p.str()); } }
	for (const auto& folder : params["workspaceFolders"].items()) { ws_->add_folder(uri_to_path(folder["uri"].str())); }
	if (params["rootUri"].is_string() && ws_->folders().empty()) { ws_->add_folder(uri_to_path(params["rootUri"].str())); }
	if (!ws_->folders().empty()) {
		std::error_code ec;
		std::filesystem::current_path(ws_->folders().front(), ec);
	}
	log("host surface: " + host_ + ", folders: " + std::to_string(ws_->folders().size()));

	json sync = json::object();
	json save = json::object();
	save.set("includeText", false);
	sync.set("openClose", true).set("change", 2).set("save", std::move(save));
	json completion = json::object();
	json triggers = json::array();
	for (const char* t : {".", ":", ">"}) { triggers.push(t); }
	completion.set("triggerCharacters", std::move(triggers)).set("resolveProvider", false);
	json signature = json::object();
	signature["triggerCharacters"] = json::array();
	signature["triggerCharacters"].push("(");
	signature["triggerCharacters"].push(",");
	json rename = json::object();
	rename.set("prepareProvider", true);
	json caps = json::object();
	caps.set("positionEncoding", "utf-16");
	caps.set("textDocumentSync", std::move(sync));
	caps.set("hoverProvider", true);
	caps.set("definitionProvider", true);
	caps.set("referencesProvider", true);
	caps.set("documentHighlightProvider", true);
	caps.set("completionProvider", std::move(completion));
	caps.set("signatureHelpProvider", std::move(signature));
	caps.set("renameProvider", std::move(rename));
	caps.set("documentSymbolProvider", true);
	caps.set("workspaceSymbolProvider", true);
	json info = json::object();
	info.set("name", "jaiscript_lsp").set("version", "0.5.3-beta");
	json result = json::object();
	result.set("capabilities", std::move(caps)).set("serverInfo", std::move(info));
	return result;
}

json server::on_hover(const json& params) {
	unit* u = doc(params);
	if (!u) { return nullptr; }
	text_span range;
	auto text = ws_->hover(*u, from_lsp(*u, params["position"]), range);
	if (!text) { return nullptr; }
	json contents = json::object();
	contents.set("kind", "markdown").set("value", *text);
	json result = json::object();
	result.set("contents", std::move(contents)).set("range", range_json(u->path, range));
	return result;
}

json server::on_definition(const json& params) {
	unit* u = doc(params);
	if (!u) { return nullptr; }
	auto loc = ws_->definition(*u, from_lsp(*u, params["position"]));
	if (!loc) { return nullptr; }
	return location_json(*loc);
}

json server::on_references(const json& params) {
	unit* u = doc(params);
	json out = json::array();
	if (!u) { return out; }
	bool include = params["context"]["includeDeclaration"].boolean(true);
	for (const auto& loc : ws_->references(*u, from_lsp(*u, params["position"]), include)) { out.push(location_json(loc)); }
	return out;
}

json server::on_completion(const json& params) {
	unit* u = doc(params);
	json result = json::object();
	json items = json::array();
	if (u) {
		auto list = ws_->completion(*u, from_lsp(*u, params["position"]));
		std::sort(list.begin(), list.end(), [](const completion_item& a, const completion_item& b) { return a.sort < b.sort; });
		const size_t cap = 400;
		for (size_t i = 0; i < list.size() && i < cap; ++i) {
			json item = json::object();
			item.set("label", list[i].label).set("kind", list[i].kind).set("sortText", list[i].sort);
			if (!list[i].detail.empty()) { item.set("detail", list[i].detail); }
			items.push(std::move(item));
		}
		result.set("isIncomplete", list.size() > cap);
	} else {
		result.set("isIncomplete", false);
	}
	result.set("items", std::move(items));
	return result;
}

json server::on_signature_help(const json& params) {
	unit* u = doc(params);
	if (!u) { return nullptr; }
	auto sig = ws_->signature_help(*u, from_lsp(*u, params["position"]));
	if (!sig) { return nullptr; }
	json sigs = json::array();
	for (const auto& s : sig->signatures) {
		json item = json::object();
		item.set("label", s.label);
		json ps = json::array();
		for (const auto& [a, b] : s.parameters) {
			json label = json::array();
			label.push(a);
			label.push(b);
			json p = json::object();
			p.set("label", std::move(label));
			ps.push(std::move(p));
		}
		item.set("parameters", std::move(ps));
		sigs.push(std::move(item));
	}
	json result = json::object();
	result.set("signatures", std::move(sigs)).set("activeSignature", sig->active_signature).set("activeParameter", sig->active_parameter);
	return result;
}

json server::on_prepare_rename(const json& params, std::string& error) {
	unit* u = doc(params);
	if (!u) { error = "no document"; return nullptr; }
	auto span = ws_->prepare_rename(*u, from_lsp(*u, params["position"]), error);
	if (!span) { return nullptr; }
	json result = json::object();
	std::string_view line = u->lines.line_text(u->text, span->start.line);
	result.set("range", range_json(u->path, *span)).set("placeholder", std::string(line.substr(span->start.col, span->end.col - span->start.col)));
	return result;
}

json server::on_rename(const json& params, std::string& error) {
	unit* u = doc(params);
	if (!u) { error = "no document"; return nullptr; }
	auto r = ws_->rename(*u, from_lsp(*u, params["position"]), params["newName"].str());
	if (!r.error.empty()) { error = r.error; return nullptr; }
	std::map<std::string, json> by_uri;
	for (const auto& e : r.edits) {
		json edit = json::object();
		edit.set("range", range_json(e.path, e.span)).set("newText", params["newName"].str());
		auto& list = by_uri[uri_for(e.path)];
		if (!list.is_array()) { list = json::array(); }
		list.push(std::move(edit));
	}
	json changes = json::object();
	for (auto& [uri, list] : by_uri) { changes.set(uri, std::move(list)); }
	json result = json::object();
	result.set("changes", std::move(changes));
	return result;
}

json server::on_document_symbol(const json& params) {
	unit* u = doc(params);
	json out = json::array();
	if (!u) { return out; }
	std::function<json(const document_symbol&)> convert = [&](const document_symbol& ds) {
		json j = json::object();
		j.set("name", ds.name).set("kind", ds.kind);
		if (!ds.detail.empty()) { j.set("detail", ds.detail); }
		j.set("range", range_json(u->path, ds.range)).set("selectionRange", range_json(u->path, ds.selection));
		if (!ds.children.empty()) {
			json kids = json::array();
			for (const auto& c : ds.children) { kids.push(convert(c)); }
			j.set("children", std::move(kids));
		}
		return j;
	};
	for (const auto& ds : ws_->document_symbols(*u)) { out.push(convert(ds)); }
	return out;
}

json server::on_workspace_symbol(const json& params) {
	json out = json::array();
	for (const auto& hit : ws_->workspace_symbols(params["query"].str())) {
		json j = json::object();
		j.set("name", hit.name).set("kind", hit.kind).set("location", location_json(hit.where));
		if (!hit.container.empty()) { j.set("containerName", hit.container); }
		out.push(std::move(j));
	}
	return out;
}

} // namespace jai_lsp
