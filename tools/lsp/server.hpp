#pragma once

#include "analysis.hpp"
#include "json.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace jai_lsp {

using engine_factory = std::function<std::shared_ptr<jai::engine>(const std::string& host)>;

class server {
public:
	static constexpr size_t default_max_message = 64 * 1024 * 1024;

	server(engine_factory make_engine, std::string host, std::vector<std::string> include_paths, bool log, size_t max_message = default_max_message);
	int run();

private:
	engine_factory make_engine_;
	std::string host_;
	std::vector<std::string> include_paths_;
	bool log_ = false;
	size_t max_message_ = default_max_message;

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<json> queue_;
	bool eof_ = false;
	std::mutex out_mutex_;

	std::unique_ptr<workspace> ws_;
	bool shutdown_ = false;
	bool exit_ = false;
	int exit_code_ = 1;
	std::unordered_map<std::string, std::string> uri_of_;
	std::set<std::string> dirty_;
	std::set<std::string> stale_;
	std::unordered_map<std::string, uint64_t> surface_seen_;
	std::map<std::string, std::string> published_;
	std::set<std::string> cancelled_;   // the ids of queued requests a $/cancelRequest named, as JSON text
	struct deferred_change { std::string path; std::string text; int version = 0; };
	std::optional<deferred_change> deferred_change_;

	void read_loop();
	bool read_message(std::string& body);
	void send(const json& message);
	void reply(const json& id, json result);
	void reply_error(const json& id, int code, const std::string& message);
	void notify(const std::string& method, json params);
	void log(const std::string& text);

	void handle(const json& message);
	bool next_is_change_of(const std::string& uri);
	void flush_deferred_change();
	json dispatch(const std::string& method, const json& params, bool& handled);
	void idle_step();
	bool idle_work() const { return !dirty_.empty() || !stale_.empty(); }
	void publish(const std::string& path);
	void mark_program_stale(const std::string& path, bool include_self);

	std::string path_of(const std::string& uri);
	std::string uri_for(const std::string& path);
	unit* doc(const json& params);
	text_pos from_lsp(unit& u, const json& position);
	json to_lsp(unit& u, text_pos p);
	json range_json(const std::string& path, text_span span);
	json location_json(const location& loc);
	void apply_change(const json& change, std::string& text);

	json on_initialize(const json& params);
	json on_hover(const json& params);
	json on_definition(const json& params);
	json on_references(const json& params);
	json on_completion(const json& params);
	json on_signature_help(const json& params);
	json on_prepare_rename(const json& params, std::string& error);
	json on_rename(const json& params, std::string& error);
	json on_document_symbol(const json& params);
	json on_workspace_symbol(const json& params);
};

} // namespace jai_lsp
