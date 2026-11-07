#pragma once
// Database: SQLite for scripts, injection-proof by construction. The SQL text of every statement is one of the program's
// own constants (engine::constant_argument: a literal, a const holding one, a ternary or + of them): the checker refuses
// any other text at load, and at run time a text the program does not spell is refused before SQLite sees it. Values
// only ever bind as parameters. There is no escape or quote function, and no way to splice a value into the text.
//
//   Database db = open_database("app.db");          // relative to the app's folder; ":memory:" for a scratch one
//   db.execute(sql, a, b, ...) -> int               // rows changed; a text of several statements (no parameters) runs them all
//   db.query(sql, a, b, ...) -> array<map<string, var>>
//   db.query_one(sql, ...) -> map<string, var> or null
//   db.value(sql, ...) -> the first column of the first row, or null
//   db.transaction(fn)  db.begin()  db.commit()  db.rollback()  db.last_insert_id()  db.close()  db.is_open()
//
// Each ? takes the next argument; an array argument expands its ? to one placeholder per element (IN (?) with [1, 2, 3]).
// Each {} takes the next argument too, which must be sql_choose(value, ["created", "title"]): one of the constant choices,
// for what a parameter cannot be (a column to sort by). One map argument binds :name, @name or $name instead. A row maps
// column names to int, float, string (TEXT, and BLOB as its bytes) or null.
#include <sqlite3.h>
#include <jaiscript/jaiscript.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spider_host {

inline constexpr const char* k_sql_refusal = "SQL text must be a constant; pass values as parameters (?)";

// One of a sql_choose call's constant choices: the only thing a {} slot of a statement takes
class sql_fragment {
public:
	sql_fragment() = default;
	explicit sql_fragment(std::string chosen) : text(std::move(chosen)) {}
	const std::string& get_text() const { return text; }

private:
	std::string text;
};

class sqlite_connection {
public:
	explicit sqlite_connection(sqlite3* opened) : handle(opened) {}
	sqlite_connection(const sqlite_connection&) = delete;
	sqlite_connection& operator=(const sqlite_connection&) = delete;
	~sqlite_connection() { close(); }

	void close() {
		drop_statements();
		if (handle) {
			sqlite3_close_v2(handle);
			handle = nullptr;
		}
	}

	void drop_statements() {
		for (auto& [text, statement] : statements) { sqlite3_finalize(statement); }
		statements.clear();
	}

	sqlite3* handle = nullptr;
	std::unordered_map<std::string, sqlite3_stmt*> statements;
	int savepoint_depth = 0;
};

// A constant SQL text split at its slots: pieces[i] slot[i] pieces[i + 1] ..., 'v' a ? value slot, 'f' a {} fragment slot;
// named when the text uses ?NNN, :name, @name or $name (bound from one map, no slots)
struct sql_template {
	std::vector<std::string> pieces;
	std::vector<char> slots;
	bool named = false;
};

inline sql_template split_sql(const std::string& sql) {
	sql_template result;
	std::string piece;
	const size_t n = sql.size();
	auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
	for (size_t i = 0; i < n; ++i) {
		const char c = sql[i];
		if (c == '\'' || c == '"' || c == '`' || c == '[') {
			const char close = c == '[' ? ']' : c;
			piece += c;
			for (++i; i < n; ++i) {
				piece += sql[i];
				if (sql[i] == close) {
					if (close != ']' && i + 1 < n && sql[i + 1] == close) { piece += sql[++i]; continue; }
					break;
				}
			}
			continue;
		}
		if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
			while (i < n && sql[i] != '\n') { piece += sql[i++]; }
			if (i < n) { piece += sql[i]; }
			continue;
		}
		if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
			const size_t end = sql.find("*/", i + 2);
			const size_t stop = end == std::string::npos ? n : end + 2;
			piece.append(sql, i, stop - i);
			i = stop - 1;
			continue;
		}
		if (c == '?' && !(i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
			result.pieces.push_back(std::move(piece));
			piece.clear();
			result.slots.push_back('v');
			continue;
		}
		if (c == '{' && i + 1 < n && sql[i + 1] == '}') {
			result.pieces.push_back(std::move(piece));
			piece.clear();
			result.slots.push_back('f');
			++i;
			continue;
		}
		if ((c == '?' || c == ':' || c == '@' || c == '$') && i + 1 < n && ident(sql[i + 1])) { result.named = true; }
		piece += c;
	}
	result.pieces.push_back(std::move(piece));
	return result;
}

class database {
public:
	static constexpr size_t statement_cache_limit = 256;

	database() = default;
	database(std::shared_ptr<sqlite_connection> shared, jai::engine* owner) :
		connection_(std::move(shared)),
		eng_(owner) {
	}

	static database open(const std::string& path, jai::engine* owner) {
		sqlite3* handle = nullptr;
		const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI;
		if (sqlite3_open_v2(path.c_str(), &handle, flags, nullptr) != SQLITE_OK) {
			std::string message = handle ? sqlite3_errmsg(handle) : "out of memory";
			if (handle) { sqlite3_close_v2(handle); }
			throw std::runtime_error("SQLite error: cannot open '" + path + "': " + message);
		}
		sqlite3_busy_timeout(handle, 5000);
		sqlite3_extended_result_codes(handle, 1);
		// WAL: readers never wait for the writer, and a commit costs one sync instead of two
		sqlite3_exec(handle, "PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL", nullptr, nullptr, nullptr);
		return database(std::make_shared<sqlite_connection>(handle), owner);
	}

	bool is_open() const { return connection_ && connection_->handle; }

	void close() {
		if (connection_) { connection_->close(); }
	}

	jai::script_int execute(const std::string& sql, const std::vector<jai::script_value>& params) {
		admit(sql, "Database.execute");
		sqlite3* handle = open_handle();
		if (!params.empty() || !template_for(sql).slots.empty()) {
			bound_statement use(*this, sql, params);
			step_to_end(use.statement);
			return static_cast<jai::script_int>(sqlite3_changes64(handle));
		}
		const char* tail = nullptr;
		sqlite3_stmt* first = prepare_uncached(sql.c_str(), &tail);
		jai::script_int changed = 0;
		sqlite3_stmt* statement = first;
		for (;;) {
			if (statement) {
				try {
					step_to_end(statement);
				} catch (...) {
					sqlite3_finalize(statement);
					throw;
				}
				sqlite3_finalize(statement);
				changed += static_cast<jai::script_int>(sqlite3_changes64(handle));
			}
			if (!tail || only_whitespace(tail)) { break; }
			statement = prepare_uncached(tail, &tail);
		}
		return changed;
	}

	jai::script_value query(const std::string& sql, const std::vector<jai::script_value>& params) {
		admit(sql, "Database.query");
		bound_statement use(*this, sql, params);
		jai::script_value rows = jai::script_value::make_array(nullptr, eng_);
		auto& out = rows.as_array();
		while (step_row(use.statement)) { out.push_back(row_of(use.statement)); }
		return rows;
	}

	jai::script_value query_one(const std::string& sql, const std::vector<jai::script_value>& params) {
		admit(sql, "Database.query_one");
		bound_statement use(*this, sql, params);
		if (!step_row(use.statement)) { return jai::script_value(std::monostate{}, eng_); }
		return row_of(use.statement);
	}

	jai::script_value value(const std::string& sql, const std::vector<jai::script_value>& params) {
		admit(sql, "Database.value");
		bound_statement use(*this, sql, params);
		if (!step_row(use.statement) || sqlite3_column_count(use.statement) == 0) { return jai::script_value(std::monostate{}, eng_); }
		return column_value(use.statement, 0);
	}

	jai::script_int last_insert_id() { return static_cast<jai::script_int>(sqlite3_last_insert_rowid(open_handle())); }
	jai::script_int changes() { return static_cast<jai::script_int>(sqlite3_changes64(open_handle())); }
	bool in_transaction() { return sqlite3_get_autocommit(open_handle()) == 0; }

	void begin() { run_plain("BEGIN"); }
	void commit() { run_plain("COMMIT"); }
	void rollback() { run_plain("ROLLBACK"); }

	// fn runs inside BEGIN/COMMIT (a SAVEPOINT when a transaction is open); its failure rolls back and raises again
	jai::script_value transaction(const jai::script_value& fn) {
		sqlite3* handle = open_handle();
		const bool nested = sqlite3_get_autocommit(handle) == 0;
		const std::string savepoint = "spider_" + std::to_string(connection_->savepoint_depth + 1);
		auto body = jai::convert_script_function<jai::script_value()>(fn);   // the throwing flavor: a failure inside raises here
		run_plain(nested ? "SAVEPOINT " + savepoint : std::string("BEGIN"));
		++connection_->savepoint_depth;
		jai::script_value result(std::monostate{}, eng_);
		try {
			result = body();
		} catch (...) {
			--connection_->savepoint_depth;
			const std::string sql = nested ? "ROLLBACK TO " + savepoint + "; RELEASE " + savepoint : std::string("ROLLBACK");
			sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr);
			throw;
		}
		--connection_->savepoint_depth;
		run_plain(nested ? "RELEASE " + savepoint : std::string("COMMIT"));
		return result;
	}

private:
	// The statement a constant text and its arguments make, prepared (or reused) and bound; reset when it goes
	struct bound_statement {
		bound_statement(database& used, const std::string& sql, const std::vector<jai::script_value>& params) {
			std::vector<const jai::script_value*> values;
			const std::string text = used.expand(sql, params, values);
			statement = used.cached_statement(text);
			if (values.size() == 1 && values[0] == nullptr) {
				used.bind_named(statement, params[0]);
			} else {
				for (size_t i = 0; i < values.size(); ++i) { used.bind_one(statement, static_cast<int>(i + 1), *values[i]); }
			}
			used.array_elements_.clear();   // SQLite copied what it binds
		}
		~bound_statement() {
			sqlite3_reset(statement);
			sqlite3_clear_bindings(statement);
		}
		sqlite3_stmt* statement = nullptr;
	};

	void admit(const std::string& sql, const char* callable) {
		if (!eng_ || !eng_->constant_argument_admits(callable, 0, sql)) { throw std::runtime_error(k_sql_refusal); }
	}

	// The text SQLite prepares: each {} replaced by its sql_choose choice, each ? of an array argument by one ? per element;
	// values receives the arguments the ?s bind, in order (a lone null entry: bind the one map argument by name)
	std::string expand(const std::string& sql, const std::vector<jai::script_value>& params, std::vector<const jai::script_value*>& values) {
		const sql_template& t = template_for(sql);
		if (t.named) {
			if (params.size() == 1 && params[0].is_map() && t.slots.empty()) {
				values.push_back(nullptr);
				return sql;
			}
			if (!params.empty() || !t.slots.empty()) { fail("a statement with named parameters (:name) takes one map, and no ? or {}"); }
			return sql;
		}
		if (params.size() != t.slots.size()) {
			fail(("the statement takes " + std::to_string(t.slots.size()) + " argument(s), got " + std::to_string(params.size())).c_str());
		}
		std::string text = t.pieces[0];
		for (size_t i = 0; i < t.slots.size(); ++i) {
			const jai::script_value& argument = params[i].deref();
			const sql_fragment* fragment = fragment_of(argument);
			if (t.slots[i] == 'f') {
				if (!fragment) { fail(("{} slot " + std::to_string(i + 1) + " takes sql_choose(value, [choices]), not a value").c_str()); }
				if (!eng_->constant_argument_admits("sql_choose", 1, fragment->get_text())) { throw std::runtime_error(k_sql_refusal); }
				text += fragment->get_text();
			} else if (fragment) {
				fail(("argument " + std::to_string(i + 1) + " is a sql_choose choice, which fills a {} slot, not a ?").c_str());
			} else if (argument.is_array()) {
				size_t count = 0;
				argument.unchecked_array_node()->for_each(eng_, [&](const jai::script_value&) { ++count; });
				array_elements_.emplace_back();
				auto& elements = array_elements_.back();
				elements.reserve(count);
				argument.unchecked_array_node()->for_each(eng_, [&](const jai::script_value& element) { elements.push_back(element); });
				for (size_t k = 0; k < elements.size(); ++k) {
					text += k == 0 ? "?" : ",?";
					values.push_back(&elements[k]);
				}
			} else {
				text += "?";
				values.push_back(&params[i]);
			}
			text += t.pieces[i + 1];
		}
		return text;
	}

	const sql_template& template_for(const std::string& sql) {
		auto found = templates_.find(sql);
		if (found == templates_.end()) {
			if (templates_.size() >= statement_cache_limit) { templates_.clear(); }
			found = templates_.emplace(sql, split_sql(sql)).first;
		}
		return found->second;
	}

	const sql_fragment* fragment_of(const jai::script_value& v) {
		if (!v.is_object()) { return nullptr; }
		try {
			return v.as<std::shared_ptr<sql_fragment>>().get();
		} catch (const std::exception&) {
			return nullptr;
		}
	}

	sqlite3* open_handle() {
		if (!is_open()) { throw std::runtime_error("SQLite error: the database is closed"); }
		return connection_->handle;
	}

	[[noreturn]] void fail(const char* what = nullptr) {
		array_elements_.clear();
		throw std::runtime_error(std::string("SQLite error: ") + (what ? what : sqlite3_errmsg(connection_->handle)));
	}

	static bool only_whitespace(const char* text) {
		for (; *text; ++text) {
			if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n' && *text != ';') { return false; }
		}
		return true;
	}

	sqlite3_stmt* prepare_uncached(const char* sql, const char** tail) {
		sqlite3_stmt* statement = nullptr;
		if (sqlite3_prepare_v2(open_handle(), sql, -1, &statement, tail) != SQLITE_OK) { fail(); }
		return statement;
	}

	sqlite3_stmt* cached_statement(const std::string& sql) {
		sqlite3* handle = open_handle();
		auto& statements = connection_->statements;
		if (auto it = statements.find(sql); it != statements.end()) { return it->second; }
		sqlite3_stmt* statement = nullptr;
		const char* tail = nullptr;
		if (sqlite3_prepare_v3(handle, sql.c_str(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &statement, &tail) != SQLITE_OK) { fail(); }
		if (!statement) { fail("the text holds no statement"); }
		if (tail && !only_whitespace(tail)) {
			sqlite3_finalize(statement);
			fail("query takes one statement (execute runs a text of several)");
		}
		if (statements.size() >= statement_cache_limit) { connection_->drop_statements(); }
		statements.emplace(sql, statement);
		return statement;
	}

	void run_plain(const std::string& sql) {
		char* message = nullptr;
		if (sqlite3_exec(open_handle(), sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
			std::string text = message ? message : sqlite3_errmsg(connection_->handle);
			sqlite3_free(message);
			fail(text.c_str());
		}
	}

	void step_to_end(sqlite3_stmt* statement) {
		while (step_row(statement)) {}
	}

	bool step_row(sqlite3_stmt* statement) {
		const int code = sqlite3_step(statement);
		if (code == SQLITE_ROW) { return true; }
		if (code == SQLITE_DONE) { return false; }
		fail();
	}

	void bind_named(sqlite3_stmt* statement, const jai::script_value& fields) {
		for (const auto& [key, item] : fields.deref().as_map()) {
			const std::string* name = key.text_bytes();
			if (!name) { fail("a named parameter's key is a string"); }
			int index = 0;
			if (!name->empty() && (name->front() == ':' || name->front() == '@' || name->front() == '$')) {
				index = sqlite3_bind_parameter_index(statement, name->c_str());
			} else {
				for (const char* prefix : {":", "@", "$"}) {
					if ((index = sqlite3_bind_parameter_index(statement, (prefix + *name).c_str())) != 0) { break; }
				}
			}
			if (index == 0) { fail(("the statement has no parameter named '" + *name + "'").c_str()); }
			bind_one(statement, index, item);
		}
	}

	void bind_one(sqlite3_stmt* statement, int index, const jai::script_value& given) {
		const jai::script_value& item = given.deref();
		int code = SQLITE_OK;
		if (item.is_null()) {
			code = sqlite3_bind_null(statement, index);
		} else if (item.is_bool()) {
			code = sqlite3_bind_int64(statement, index, item.as_bool() ? 1 : 0);
		} else if (item.is_int()) {
			code = sqlite3_bind_int64(statement, index, item.as_int());
		} else if (item.is_float()) {
			code = sqlite3_bind_double(statement, index, item.as_float());
		} else if (const std::string* text = item.text_bytes()) {
			code = sqlite3_bind_text64(statement, index, text->data(), text->size(), SQLITE_TRANSIENT, SQLITE_UTF8);
		} else if (item.is_string()) {
			const std::string& bound = item.as_string();
			code = sqlite3_bind_text64(statement, index, bound.data(), bound.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
		} else {
			fail(("parameter " + std::to_string(index) + " is not an int, float, bool, string or null").c_str());
		}
		if (code != SQLITE_OK) { fail(); }
	}

	jai::script_value column_value(sqlite3_stmt* statement, int column) {
		switch (sqlite3_column_type(statement, column)) {
			case SQLITE_INTEGER: return jai::script_value(static_cast<jai::script_int>(sqlite3_column_int64(statement, column)), eng_);
			case SQLITE_FLOAT: return jai::script_value(sqlite3_column_double(statement, column), eng_);
			case SQLITE_TEXT: {
				const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
				return jai::script_value(std::string(text ? text : "", static_cast<size_t>(sqlite3_column_bytes(statement, column))), eng_);
			}
			case SQLITE_BLOB: {
				const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, column));
				return jai::script_value(std::string(bytes ? bytes : "", static_cast<size_t>(sqlite3_column_bytes(statement, column))), eng_);
			}
			default: return jai::script_value(std::monostate{}, eng_);
		}
	}

	jai::script_value row_of(sqlite3_stmt* statement) {
		jai::script_value row = jai::script_value::make_map(nullptr, nullptr, eng_);
		auto& fields = row.as_map();
		const int columns = sqlite3_column_count(statement);
		for (int column = 0; column < columns; ++column) {
			const char* name = sqlite3_column_name(statement, column);
			fields[jai::script_value(std::string(name ? name : ""), eng_)] = column_value(statement, column);
		}
		return row;
	}

	std::shared_ptr<sqlite_connection> connection_;
	jai::engine* eng_ = nullptr;
	std::unordered_map<std::string, sql_template> templates_;
	std::vector<std::vector<jai::script_value>> array_elements_;   // the elements an array argument binds, alive until bound
};

template<size_t>
using database_argument = const jai::script_value&;

template<typename Body, size_t... I>
auto database_method(Body body, std::index_sequence<I...>) {
	return [body](database& self, const std::string& sql, database_argument<I>... params) {
		return body(self, sql, std::vector<jai::script_value>{params...});
	};
}

// one overload per argument count, 0 to 16 parameters after the SQL text
template<typename Binder, typename Body, size_t... Counts>
void bind_database_method(Binder& binder, const char* name, Body body, std::index_sequence<Counts...>) {
	(binder.method(name, database_method(body, std::make_index_sequence<Counts>{})), ...);
}

inline void register_database(jai::engine& eng, const std::filesystem::path& app_dir) {
	jai::dynamic_binder<sql_fragment>(eng, "SqlFragment")
		.method("text", [](const sql_fragment& self) { return self.get_text(); })
		.build();
	auto binder = jai::dynamic_binder<database>(eng, "Database");
	binder.constructor<>();
	constexpr auto counts = std::make_index_sequence<17>{};
	bind_database_method(binder, "execute", [](database& self, const std::string& sql, const std::vector<jai::script_value>& params) {
		return self.execute(sql, params);
	}, counts);
	bind_database_method(binder, "query", [](database& self, const std::string& sql, const std::vector<jai::script_value>& params) {
		return self.query(sql, params);
	}, counts);
	bind_database_method(binder, "query_one", [](database& self, const std::string& sql, const std::vector<jai::script_value>& params) {
		return self.query_one(sql, params);
	}, counts);
	bind_database_method(binder, "value", [](database& self, const std::string& sql, const std::vector<jai::script_value>& params) {
		return self.value(sql, params);
	}, counts);
	binder.method("transaction", [](database& self, const jai::script_value& fn) { return self.transaction(fn); });
	binder.method("begin", [](database& self) { self.begin(); });
	binder.method("commit", [](database& self) { self.commit(); });
	binder.method("rollback", [](database& self) { self.rollback(); });
	binder.method("last_insert_id", [](database& self) { return self.last_insert_id(); });
	binder.method("changes", [](database& self) { return self.changes(); });
	binder.method("in_transaction", [](database& self) { return self.in_transaction(); });
	binder.method("is_open", [](const database& self) { return self.is_open(); });
	binder.method("close", [](database& self) { self.close(); });
	binder.build();
	for (const char* method : {"Database.execute", "Database.query", "Database.query_one", "Database.value"}) {
		eng.constant_argument(method, 0, k_sql_refusal);
	}
	eng.constant_argument("sql_choose", 1, "sql_choose's choices must be constants: an array literal of strings, or a const of one");
	jai::engine* engine_pointer = &eng;
	eng.add_function("sql_choose", [engine_pointer](const jai::script_value& wanted, std::vector<std::string> choices) {
		const std::string* text = wanted.deref().text_bytes();
		for (const auto& choice : choices) {
			if (text && *text == choice && engine_pointer->constant_argument_admits("sql_choose", 1, choice)) { return std::make_shared<sql_fragment>(choice); }
		}
		std::string listed;
		for (const auto& choice : choices) { listed += (listed.empty() ? "" : ", ") + choice; }
		throw std::runtime_error("sql_choose: the value is not one of: " + listed);
	});
	eng.add_function("open_database", [engine_pointer, app_dir](const std::string& path) {
		std::string resolved = path;
		if (path != ":memory:" && path.rfind("file:", 0) != 0 && std::filesystem::path(path).is_relative()) {
			resolved = (app_dir / path).lexically_normal().string();
		}
		return std::make_shared<database>(database::open(resolved, engine_pointer));
	}, jai::filesystem);
}

} // namespace spider_host
