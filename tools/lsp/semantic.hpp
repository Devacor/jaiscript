#pragma once

#include "analysis_internal.hpp"

#include <map>

namespace jai_lsp {

// One unit's top-level declarations by qualified name (namespace members as "ns::name")
struct unit_index {
	struct entry {
		const jai::declaration* node = nullptr;
		symbol_kind kind = symbol_kind::global;
		std::string_view name;       // the bare name (a destructured name, a namespace's last segment)
		bool is_extern = false;
	};
	std::unordered_map<std::string, std::vector<entry>> names;
	std::unordered_map<std::string, const jai::enum_decl*> enum_values;
	std::unordered_map<std::string, const jai::class_decl*> classes;
	std::unordered_set<std::string> namespaces;
};

void build_unit_index(const std::vector<jai::declaration_ptr>& decls, const std::string& prefix, unit_index& out);

// The program as a lookup surface: the current unit first, then the other units of its program,
// then the host's registrations.
class program_view {
public:
	program_view(workspace& ws, const program& prog, unit& current);

	struct member_ref {
		bool found = false;
		const jai::class_decl* owner = nullptr;
		const jai::declaration* decl = nullptr;
		std::string host_class;      // a registered (C++) class answered
	};
	struct top_ref {
		const unit_index::entry* entry = nullptr;
		unit* owner = nullptr;
	};

	const jai::class_decl* find_class(std::string_view name);
	bool class_known(std::string_view name);
	member_ref find_member(std::string_view cls, std::string_view member, bool want_static, int depth = 0);
	std::vector<top_ref> top_level(const std::string& qualified);
	bool has_top(const std::string& qualified);
	const jai::enum_decl* find_enum_value(std::string_view value);
	const jai::enum_decl* find_enum(std::string_view name);
	bool is_namespace(const std::string& path);
	bool host_namespace(const std::string& path);
	const unit_index& index_of(unit& u);
	unit* unit_of_node(const jai::ast_node* node);
	std::vector<unit*> units();

	uint32_t global_symbol(unit_model& m, const std::string& qualified);
	uint32_t member_symbol(unit_model& m, const member_ref& r, std::string_view member);
	uint32_t enum_value_symbol(unit_model& m, const jai::enum_decl* e, std::string_view value);
	uint32_t host_symbol(unit_model& m, const std::string& name, symbol_kind kind);
	uint32_t builtin_symbol(unit_model& m, std::string_view receiver, std::string_view member);
	nav_type declared_type(const jai::declaration* d);

	std::optional<text_span> decl_span(const jai::declaration* node, std::string_view name);
	std::vector<std::string> method_signatures(const jai::class_decl* owner, std::string_view name, bool constructor);
	std::vector<std::string> host_signatures(const std::string& name);
	std::string class_detail(const jai::class_decl* cd);

	workspace& ws;
	const program& prog;
	unit& current;
};

uint32_t add_symbol(unit_model& m, symbol s);

class semantic_walker {
public:
	semantic_walker(workspace& ws, unit& u, const program& prog, unit_model& m);
	void run();

private:
	unit& u_;
	unit_model& m_;
	program_view view_;
	jai::engine& eng_;

	std::vector<std::unordered_map<std::string, uint32_t>> scopes_;
	std::vector<size_t> regions_;
	std::vector<std::string> class_stack_;
	std::vector<std::string> ns_stack_;
	std::vector<std::string> fn_stack_;
	std::unordered_map<const jai::expression*, uint32_t> expr_symbol_;
	std::unordered_map<uint32_t, nav_type> symbol_type_;
	std::unordered_map<const jai::expression*, nav_type> nav_memo_;

	void occur(text_span span, uint32_t sym, bool declaration);
	void push_scope(text_pos start, text_pos end);
	void pop_scope();
	uint32_t declare_local(std::string_view name, text_span span, symbol_kind kind, const std::string& type_shown, nav_type type, bool with_occurrence = true, const std::string& inferred = {});
	std::string qualify(std::string_view name) const;
	uint32_t resolve_name(std::string_view name, bool global_only);
	nav_type type_of(const jai::expression* e);
	nav_type own_type(const jai::expression* e);
	uint32_t symbol_for_type(nav_type t);
	void capture_scope();

	void walk_decls(const std::vector<jai::declaration_ptr>& decls);
	void walk_decl(const jai::declaration* d);
	void walk_variable(const jai::variable_decl* v);
	void walk_function(const jai::function_decl* fn);
	void walk_function_body(const jai::function_decl* fn);
	void declare_params(const std::vector<jai::parameter>& params, text_pos after);
	void walk_class(const jai::class_decl* cd);
	void walk_stmt(const jai::statement* s);
	void walk_expr(const jai::expression* e);
	void walk_member(const jai::member_expr* m);
	bool walk_static_path(const jai::member_expr* m);
	void resolve_member_on(const nav_type& t, const jai::member_expr* m);
	void walk_lambda(const jai::lambda_expr* l);
	void type_name_pass();

	text_pos stmt_end(const jai::statement* s);
	text_pos after_open_paren(text_pos pos);
};

} // namespace jai_lsp
