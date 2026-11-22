#pragma once

// The language server's model of a workspace: units (one per file, the editor's text when open),
// programs (a root's literal import/include closure, checked as one program exactly like the
// engine's gather), the strict checker's diagnostics per unit, and a semantic model per unit
// (symbols, occurrences, scopes) built from the parse and the checker's inferred types.

#include "text.hpp"

#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/type_checker.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai_lsp {

enum class symbol_kind : uint8_t {
	local, parameter, global, function, class_type, field, method, constructor, enum_type, enum_value,
	namespace_name, host_function, host_global, host_class, host_member, host_namespace, type_parameter
};

struct symbol {
	std::string key;          // identity across files: g:qualified, m:Class.member, e:Enum.value, l:path@line:col, h:name
	symbol_kind kind = symbol_kind::local;
	std::string name;
	std::string container;    // owning class, enum, namespace or function
	std::string detail;       // the declaration in one line
	std::string type;         // the declared or inferred type, when it has one
	std::string path;         // declaring unit (empty for host symbols)
	text_span span;           // the declaring name
	bool is_static = false;
	std::vector<std::string> signatures;   // functions, methods, constructors: one per overload
	std::string instance;     // a generic class's instance this name spells (Stack<int>): the class a hover shows
};

struct occurrence {
	text_span span;
	uint32_t symbol = 0;
	bool declaration = false;
	std::string narrowed;   // the type a flow test gave the name here
};

struct scope_local {
	uint32_t symbol = 0;
	text_pos declared;
};

struct scope_region {
	text_span span;
	std::vector<scope_local> locals;
};

struct unresolved_member {
	std::string name;
	text_span span;
};

struct lex_token {
	jai::token_type type;
	text_pos pos;
	std::string_view lexeme;   // identifiers, keywords and punctuation only (permanent storage)
	uint32_t length = 0;       // source length
};

struct file_diagnostic {
	uint32_t line = 0;        // 0-based
	uint32_t col = 0;         // 0-based byte column
	uint32_t length = 0;      // 0 = the token at the position
	bool error = true;
	std::string message;
	uint32_t related_line = UINT32_MAX;
	uint32_t related_col = 0;
	std::string source;       // "parse" or "check"
};

struct unit_model {
	std::vector<symbol> symbols;
	std::unordered_map<std::string, uint32_t> by_key;
	std::vector<occurrence> occurrences;          // sorted by start
	std::vector<unresolved_member> unresolved_members;
	std::vector<scope_region> scopes;
	std::unordered_map<const jai::expression*, jai::check_expression_type> types;
	std::string class_at_capture;                 // completion capture: the class whose members bare names reach
	std::vector<std::string> namespaces_at_capture;
	bool captured = false;
	std::vector<uint32_t> capture_locals;
	std::string capture_member_receiver;          // member completion: the receiver's class ("" = none)
	std::string capture_member_kind;              // object, class, enum, namespace, array, map, string
	const jai::type_info* capture_member_ti = nullptr;
};

// A generic as its text reads with the type parameters left as names, parsed on its own: the class or function its author
// wrote, which the walks and lookups read as they read a declaration with no parameters
struct generic_view {
	std::shared_ptr<const jai::detail::generic_definition> definition;
	jai::declaration_ptr holder;                  // the parse (a method's sits in its class, a namespace's in its namespace)
	jai::declaration_ptr node;                    // the class_decl or function_decl inside it; null when the text did not parse
	const jai::declaration* decl = nullptr;       // node.get()
};
using generic_view_map = std::unordered_map<const jai::detail::generic_definition*, generic_view>;

struct unit {
	std::string path;
	std::string text;
	bool open = false;
	int version = 0;
	uint64_t epoch = 0;                           // moves on every text change
	line_index lines;
	std::vector<lex_token> tokens;
	std::vector<jai::declaration_ptr> decls;      // the parse, or what parsed around its errors
	std::vector<jai::declaration_ptr> good_decls; // the last clean parse
	std::vector<jai::declaration_ptr> checked_decls; // decls with the generic instances its check declared: what the checker walks
	generic_view_map generic_views;               // the generics its parses declare, those in their text included
	generic_view_map instance_views;              // the generic methods its check's instances declare, made as a lookup reaches them
	bool parse_ok = false;
	std::vector<std::string> salvaged;            // failing parse: top-level names read off the tokens (no cascade of undeclared names)
	std::vector<file_diagnostic> parse_errors;
	std::vector<std::string> imports;             // resolved literal top-level import/include targets
	std::vector<std::string> namespace_queries;   // the namespace classes the parse asked about (parser::namespace_queries)
	std::string namespace_answers;                // and how the program pooled then answered each
	std::vector<std::string> less_queries;        // what the parse's '<' sites asked (parser::less_queries), how each was answered,
	std::string less_answers;                     // and whether the parse was made with its program pooled
	bool less_settled = true;
	bool settling = false;                        // the parse under way is made with the unit's program pooled
	uint64_t surface = 0;                         // hash of what other units' checks read from this one
	uint64_t disk_stamp = 0;
	double disk_checked_at = -1.0e9;              // seconds (steady clock) of the last stat

	std::shared_ptr<void> index;                  // unit_index of this parse (semantic.hpp)
	uint64_t index_epoch = UINT64_MAX;

	uint64_t checked_epoch = UINT64_MAX;
	uint64_t checked_program = 0;
	std::vector<file_diagnostic> diagnostics;     // parse errors + checker findings
	std::shared_ptr<unit_model> model;
	uint64_t model_epoch = UINT64_MAX;
	uint64_t model_program = 0;
	double last_check_ms = 0.0;
	uint64_t checks = 0;                          // checker runs over this unit
};

struct program {
	std::vector<std::string> roots;
	std::vector<std::string> units;               // closure, depth-first from the roots
	uint64_t signature = 0;
	std::unordered_set<std::string> names;
	jai::detail::program_class_map classes;
	jai::detail::program_enum_map enums;
	jai::detail::program_function_map functions;   // by kind: '<' after a name asks whether another unit's name is a variable
	jai::detail::program_global_map globals;
};

struct location {
	std::string path;
	text_span span;
};

struct completion_item {
	std::string label;
	int kind = 1;                                 // LSP CompletionItemKind
	std::string detail;
	std::string sort;
	std::string insert;
};

struct signature_info {
	std::string label;
	std::vector<std::pair<uint32_t, uint32_t>> parameters;   // label offsets
};

struct signature_result {
	std::vector<signature_info> signatures;
	uint32_t active_signature = 0;
	uint32_t active_parameter = 0;
};

struct document_symbol {
	std::string name;
	std::string detail;
	int kind = 13;                                // LSP SymbolKind
	text_span range;
	text_span selection;
	std::vector<document_symbol> children;
};

struct workspace_symbol_hit {
	std::string name;
	std::string container;
	int kind = 13;
	location where;
};

struct rename_result {
	std::string error;                            // non-empty: refused, with the reason
	std::vector<location> edits;
};

class workspace {
public:
	explicit workspace(std::shared_ptr<jai::engine> eng);

	void add_folder(const std::string& folder);
	void add_include_path(const std::string& path);
	void scan_folders();

	unit& open(const std::string& path, std::string text, int version);
	unit& change(const std::string& path, std::string text, int version);
	void close(const std::string& path);
	void file_changed_on_disk(const std::string& path);
	unit* find(const std::string& path);
	unit* get(const std::string& path);           // an open unit, or the file from disk (nullptr: unreadable)

	const program& program_of(const std::string& path);
	const std::vector<file_diagnostic>& diagnostics(unit& u);
	const unit_model& model(unit& u);
	std::vector<std::string> program_units(const std::string& path);

	std::optional<location> definition(unit& u, text_pos pos);
	std::vector<location> references(unit& u, text_pos pos, bool include_declaration);
	std::vector<occurrence> highlights(unit& u, text_pos pos);
	std::optional<std::string> hover(unit& u, text_pos pos, text_span& range);
	std::vector<completion_item> completion(unit& u, text_pos pos);
	std::optional<signature_result> signature_help(unit& u, text_pos pos);
	rename_result rename(unit& u, text_pos pos, const std::string& new_name);
	std::optional<text_span> prepare_rename(unit& u, text_pos pos, std::string& error);
	std::vector<document_symbol> document_symbols(unit& u);
	std::vector<workspace_symbol_hit> workspace_symbols(const std::string& query);

	jai::engine& engine() { return *engine_; }
	const std::vector<std::string>& folders() const { return folders_; }
	std::vector<const unit*> loaded_units() const;
	uint64_t parses() const { return epoch_clock_; }

private:
	std::shared_ptr<jai::engine> engine_;
	std::vector<std::string> folders_;
	std::unordered_map<std::string, std::unique_ptr<unit>> units_;
	std::unordered_map<std::string, std::unordered_set<std::string>> importers_;   // target -> units importing it
	std::unordered_map<std::string, std::vector<std::string>> scanned_imports_;    // files seen by the folder scan
	std::unordered_map<std::string, program> programs_;
	std::vector<std::string> workspace_files_;
	uint64_t epoch_clock_ = 0;

	void parse(unit& u, bool publish = true);
	uint64_t pooled_program_ = 0;                 // the signature of the program whose classes the engine's parser reads
	void pool_classes(const program& prog);
	std::string namespace_answers(const unit& u) const;
	bool less_moved(unit& u);
	void set_imports(const std::string& path, const std::vector<std::string>& imports);
	std::vector<std::string> roots_of(const std::string& path);
	void analyze(unit& u, bool want_model);
	std::shared_ptr<unit_model> analyze_text(unit& base, std::string text, std::shared_ptr<unit>& keep);
	const symbol* symbol_at(unit& u, text_pos pos, uint32_t* index = nullptr);
	std::vector<std::string> reference_scope(unit& u, const symbol& s);
	std::vector<location> collect_references(const std::vector<std::string>& paths, const std::vector<std::string>& keys, bool include_declaration);
	std::vector<std::string> rename_group(unit& u, const symbol& s);

	friend class semantic_walker;
	friend class completion_engine;
};

// Parse errors come back as "path:line:col: message" lines.
std::vector<file_diagnostic> parse_error_lines(const std::string& all, const std::string& path);

} // namespace jai_lsp
