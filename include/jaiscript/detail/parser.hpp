#pragma once

#include "lexer.hpp"
#include "ast.hpp"
#include "foreign_keyword.hpp"
#include "generics.hpp"
#include "native_stack.hpp"
#include "source_loading.hpp"
#include <jaiscript/core/types.hpp>
#include <jaiscript/core/checked_result.hpp>
#include <jaiscript/core/parse_errors.hpp>
#include <vector>
#include <memory>
#include <optional>
#include <compare>
#include <map>
#include <set>
#include <unordered_set>
#include <algorithm>

namespace jai {

    class string_symbolizer;
    class engine;

    class parser {
    public:
        parser(const std::vector<token>& tokens, string_symbolizer* symbolizer, engine* eng, const std::unordered_set<std::string>& registeredTemplateTypes, const std::string& filename = "<script>");

        // Parse the entire program
        checked_result<std::vector<declaration_ptr>> parse();
        
        bool has_errors() const { return !errors_.empty(); }
        // each error as "file:line:col: message"
        std::vector<std::string> get_errors() const;
        // the source text the tokens came from: a failed parse's listing shows each error's line with a caret under it
        void source_text(std::string_view source) { source_ = source; }
        // the files spliced into function bodies by this parse (resolved paths), each under the stamp its text was loaded
        // with, for the parse cache's stamps; and the first one that never read whole (the parse failed on it)
        const std::vector<detail::source_stamp>& spliced_files() const { return spliced_files_; }
        const std::string& unsettled_file() const { return unsettled_file_; }
        // the namespace classes, enums and nested namespaces this parse asked the engine about, sorted: "space::Name" (space declares
        // a class or an enum Name), a bare "Name" (some namespace does), "space::Name::" (space holds a namespace Name), a bare
        // "Name::" (some nested namespace is named so); the parse holds wherever the engine answers them alike (engine.cpp
        // namespace_answers)
        std::vector<std::string> namespace_queries() const { return {namespace_queries_.begin(), namespace_queries_.end()}; }
        // '<' after a name: the queries this parse's sites asked, what answered each, and whether
        // the parse is settled. A unit parsed before the files it names are pooled asks to be provisional: it is, when it names one.
        const std::vector<std::string>& less_queries() const { return less_queries_; }
        const std::string& less_answers() const { return less_answers_; }
        bool less_settled() const { return less_settled_; }
        void less_provisional(bool a_on) { less_may_be_provisional_ = a_on; }
        // after a failed parse: the top-level declarations that parsed around the errors (a tool checks what it can)
        std::vector<declaration_ptr>& recovered_declarations() { return recovered_; }
        // each error as its place and its text (an instance's parse reports them in its generic's file)
        struct issue {
            source_location at;
            std::string message;
        };
        std::vector<issue> issues() const;
        // The token stream as one type (a generic's bound with its arguments in place); the instances it names are requested in `requests`
        checked_result<type_info_ptr> parse_type_text(std::vector<declaration_ptr>& requests);
        // The token stream as one template body (lexer::tokenize_template) under a untrusted_template's rules: the expression, or the
        // listing of what the text breaks (docs/site/guide/19-templates.html)
        checked_result<expression_ptr> parse_untrusted_template_body();
        // The region the parsed text's generics check their instances in when the text names none (a generic class's instance: its class's)
        void inherit_check_mode(uint8_t a_mode) { inherited_check_mode_ = a_mode; }

    private:
        std::vector<declaration_ptr> recovered_;
        const std::vector<token>* tokens_;   // the program's tokens as given: a pass that parses them again starts here
        const std::vector<token>* stream_;   // the token stream being parsed: the program's, or a spliced include's while its declarations are read
        // One pass over the tokens; parse() runs another, in a parser of its own, when a '<' site's reading was not the one its
        // head's lookup gives
        checked_result<std::vector<declaration_ptr>> parse_pass();
        void adopt_pass(parser& a_pass);
        struct less_place {
            std::string file;
            uint32_t line = 0;
            uint32_t column = 0;
            friend auto operator<=>(const less_place&, const less_place&) = default;
        };
        struct less_site {
            less_place at;
            uint32_t query = 0;   // its index in less_queries_
            bool opens = true;    // the reading this pass took
        };
        std::vector<less_site> less_sites_;
        std::map<less_place, size_t> less_site_index_;
        std::map<less_place, bool> less_fixed_;   // the readings this pass is given
        std::vector<std::string> less_queries_;
        std::string less_answers_;
        bool less_settled_ = true;
        bool less_may_be_provisional_ = false;
        bool less_in_pass_ = false;   // parse() answers the sites once the pass has its declarations; any other entry answers each as it is met
        struct type_parameter_name {
            std::string_view name;      // T
            std::string_view generic;   // the generic that declares it: Stack
        };
        std::vector<type_parameter_name> generic_type_parameters_;   // of the generic whose text is being parsed: types there
        // the generic that declares `a_name` as a type parameter of the text being parsed; empty when none does
        std::string_view type_parameter_generic(std::string_view a_name) const;
        std::vector<less_site>* generic_text_readings_ = nullptr;  // every site reading taken in that text, for its tokens to carry
        bool less_site_reading(const jai::expression& a_head);
        bool names_type_parameter(const jai::expression& a_head) const;
        std::vector<token> namespace_stream_;   // the program's stream with its namespaces' bare class and enum names qualified
        std::vector<token> using_stream_;    // the program's stream after its using declarations rewrote it
        std::set<std::string> namespace_queries_;
        // Inside a namespace's blocks a class or an enum that namespace declares (here or in a unit the engine ran) reads by its
        // bare name as ns::Name, its one name (C++); false when the stream opens no namespace that names one
        bool qualify_namespace_classes(const std::vector<token>& in, std::vector<token>& out);
        // `using ns::name;` (a comma list) at a statement's start: the rest of THIS token stream reads each name as its qualified
        // spelling (file-local, not transitive, any namespace); false when the stream names no using
        bool apply_using_declarations(const std::vector<token>& in, std::vector<token>& out);
        bool reject_using_name(const token& t, const char* what);
        // hot_reload_migrate at a unit's top level is the unit's reload hook: one, declared void hot_reload_migrate(map<string, var> previous)
        void check_reload_hook(const jai::declaration& d, bool& seen);
        std::string filename_;
        // a '>>' that closed one type argument list leaves its second '>' here, read before the token at current_
        std::optional<token> pushed_back_token_;
        mutable token last_advanced_;  // Storage for advance() when consuming pushed_back_token_
        // The index of the next token. The pushed-back '>' stands before one index only, the one after its '>>': a write that
        // moves the index drops it, so a lookahead that rewinds reads its tokens again and never finds that '>' before them
        class token_cursor {
        public:
            explicit token_cursor(std::optional<token>& pushed_back) : pushed_back_(pushed_back) {}
            token_cursor(const token_cursor&) = delete;
            token_cursor& operator=(const token_cursor&) = delete;
            operator size_t() const { return index_; }
            token_cursor& operator=(size_t index) {
                if (index != index_) {
                    index_ = index;
                    pushed_back_.reset();
                }
                return *this;
            }
            token_cursor& operator+=(size_t count) { return *this = index_ + count; }
            token_cursor& operator-=(size_t count) { return *this = index_ - count; }
            size_t operator++(int) { const size_t before = index_; *this = index_ + 1; return before; }
            size_t operator--(int) { const size_t before = index_; *this = index_ - 1; return before; }
        private:
            size_t index_ = 0;
            std::optional<token>& pushed_back_;
        };
        token_cursor current_{pushed_back_token_};
        size_t foreign_stop_ = SIZE_MAX;   // the error count at which a foreign construct ended the parse
        struct parse_diagnostic {
            source_location at;
            std::string message;
        };
        std::vector<parse_diagnostic> errors_;
        std::string_view source_;
        checked_result<std::vector<declaration_ptr>> failed_parse();
        std::unordered_set<std::string> registered_template_types_;
        string_symbolizer* symbolizer_ = nullptr;  // Optional: for interning identifiers at parse time
        engine* engine_ = nullptr;  // Engine for interning type_info objects
        expression_ptr desugar_incdec(const token& op, const expression_ptr& target, bool postfix);
        expression_ptr lvalue_target(const expression_ptr& target);
        expression_ptr bound_reference(const expression_ptr& piece);
        expression_ptr bound_conditional(expression_ptr value);
        expression_ptr reference_binding(expression_ptr value);

        // A literal include inside a function body is spliced here at parse time: the included
        // declarations become the function's own, so they see its parameters and locals and it sees theirs; the files read
        // (the engine stamps them: a change re-parses the includer), the chain being spliced (a cycle guard) and its depth
        std::vector<detail::source_stamp> spliced_files_;
        std::string unsettled_file_;
        bool load_spliced(const std::string& resolved_path, std::string& content);
        std::vector<std::string> splice_active_;
        std::vector<std::string> splice_spelled_;   // the paths as written, for the include's own texts
        int splice_depth_ = 0;
        bool splice_include(const declaration_ptr& decl, std::vector<declaration_ptr>& out);

        // template(path, a, b, ...), untrusted_template(path, map), untrusted_template_text(text, map) (docs/site/guide/19-templates.html).
        // A template body sees the names its own text declares, the names passed to it and the program's names: a barrier
        // hides every enclosing scope (function scopes below scope_count, member scopes, block scopes at program level) from a
        // name it does not admit. An untrusted barrier admits `args` and nothing past it.
        struct template_barrier {
            size_t scope_count = 0;
            std::vector<uint64_t> names;
            bool untrusted = false;
        };
        std::vector<template_barrier> template_barriers_;
        std::vector<std::string> template_chain_;   // the template files being expanded: a call back into one renders through the dynamic form
        bool template_call_ahead() const;
        checked_result<expression_ptr> template_call();
        checked_result<expression_ptr> untrusted_template_call(bool text_form);
        checked_result<expression_ptr> template_text_body(const std::string& content, const std::string& file, template_barrier barrier);
        bool template_hides(uint64_t symbol_id) const;
        void resolve_name(identifier_expr& id);
        expression_ptr template_builtin(std::string_view name, const source_location& at, std::vector<expression_ptr> arguments);
        void reject_class_in_function_body(const token& at);

        // Current namespace context for nested namespace declarations
        std::vector<std::string> current_namespace_path_;
        
        // Error handling
        void report_error(const std::string& message, const token& token);
        void report_error_at(const std::string& message, const source_location& at);
        void synchronize();  // Error recovery
        void synchronize_class_member(size_t class_open_index);  // Brace-aware recovery for the class-member loop
        // Name-position diagnostics: when the offending token is a type keyword
        // ('map', 'array', ...) the error names the cause at the true site instead
        // of the generic "Expected X name" (and instead of desyncing downstream)
        void report_expected_name(const std::string& expected_message, const char* what);
        checked_result<token> consume_name(const std::string& expected_message, const char* what);
        
        // token management (return by reference to avoid copying strings)
        const token& peek() const;
        const token& previous() const;
        const token& advance();
        bool is_at_end() const;
        bool check(token_type type) const;
        bool match(token_type type);
        bool match(std::initializer_list<token_type> types);
        checked_result<token> consume(token_type type, std::string_view message);
        checked_result<void> expect(token_type type, std::string_view message);   // consume, when the token itself is not needed
        error_propagator expected_token(std::string_view message);
        
        // declaration parsing
        checked_result<declaration_ptr> declaration();
        checked_result<declaration_ptr> declaration_body();
        checked_result<declaration_ptr> attributed_declaration();
        checked_result<declaration_ptr> declaration_rest();
        checked_result<declaration_ptr> statement_declaration();
        checked_result<declaration_ptr> class_declaration(bool nocopy = false, std::string_view space = {});
        checked_result<declaration_ptr> enum_declaration(std::string_view space = {});
        checked_result<declaration_ptr> namespace_declaration();
        checked_result<declaration_ptr> function_declaration();
        bool bare_coroutine_head() const;   // coroutine IDENT '(' / coroutine operator<sym>: no type, no 'function'
        bool coroutine_modifier_run() const;   // class member: [static|override]* coroutine ...
        checked_result<declaration_ptr> bare_coroutine_declaration();
        checked_result<declaration_ptr> variable_declaration();
        expression_ptr default_initializer(const type_info_ptr& type, const source_location& loc);   // a typed declaration without an initializer
        expression_ptr math_constructor_call(script_value_type kind, std::vector<expression_ptr> arguments, const source_location& loc);
        checked_result<expression_ptr> initializer_from_arguments(const type_info_ptr& type, std::vector<expression_ptr> arguments, bool brace, const source_location& loc);
        void resolve_constructor_initializers(class_decl& cls);
        expression_ptr field_initializer_value(const variable_decl& field, std::vector<expression_ptr> arguments, bool brace, const source_location& at);
        checked_result<expression_ptr> construction(const type_info_ptr& type, std::vector<expression_ptr> arguments, bool brace, const source_location& loc);
        bool paren_group_ends_statement(size_t lparen) const;   // `name(...)` followed by ';': an object declaration, never a function
        checked_result<declaration_ptr> include_declaration();
        checked_result<std::string> include_literal_path();
        checked_result<std::vector<expression_ptr>> construction_arguments(bool a_brace, const char* a_closeError = nullptr);
        checked_result<declaration_ptr> import_declaration();
        checked_result<expression_ptr> import_call(const source_location& at);
        checked_result<declaration_ptr> extern_declaration();
        checked_result<declaration_ptr> inline_variable_declaration();
        // const T x = e; / const x = e; is the declaration, then finish_const marks it and enforces the initializer
        checked_result<declaration_ptr> const_declaration();
        checked_result<declaration_ptr> destructuring_declaration();
        bool finish_const(const declaration_ptr& decl, const token& at, bool class_constant);
        bool const_method_suffix();   // `) const`: JaiScript has no const methods
        bool untyped_declaration_ahead() const;   // IDENT followed by ';' or '='
        bool untyped_member_declaration_ahead() const;   // the same, or a keyword other than a type naming a class or namespace member
        bool check_mode_attribute_ahead() const;
        checked_result<uint8_t> check_mode_attribute();
        bool nodiscard_attribute_ahead() const;
        bool mark_nodiscard(const declaration_ptr& decl, const source_location& at, std::string_view class_name = {});
        bool reject_coroutine_reference_parameter(const function_decl& coroutine);
        std::optional<std::string> parse_operator_method_symbol();
        
        // statement parsing
        checked_result<statement_ptr> statement();
        checked_result<statement_ptr> statement_body();
        checked_result<statement_ptr> attributed_statement();
        checked_result<statement_ptr> block_map_error();
        checked_result<statement_ptr> import_statement();
        checked_result<statement_ptr> fallthrough_statement();
        checked_result<statement_ptr> case_body_statement();
        checked_result<statement_ptr> expression_statement();
        checked_result<statement_ptr> block_statement();
        void mark_bare_condition(const expression_ptr& condition, size_t first_token);
        checked_result<statement_ptr> if_statement();
        checked_result<statement_ptr> while_statement();
        checked_result<statement_ptr> for_statement();
        checked_result<statement_ptr> parallel_for_statement();
        checked_result<expression_ptr> counted_parallel_range(const type_info_ptr& element_type, bool is_reference, bool is_const, const token& variable);
        checked_result<statement_ptr> return_statement();
        checked_result<statement_ptr> break_statement();
        checked_result<statement_ptr> continue_statement();
        checked_result<statement_ptr> try_statement();
        checked_result<statement_ptr> switch_statement();
        // do { body } while (cond); is `for (bool first = true; first || cond; first = false) body` over a name no script can
        // spell, so continue reaches the condition as C++'s does
        bool do_while_ahead() const;
        checked_result<statement_ptr> do_while_statement();
        bool init_statement_ahead() const;
        checked_result<declaration_ptr> unterminated_declaration(bool condition);
        checked_result<expression_ptr> condition_clause(std::vector<declaration_ptr>& declarations, bool init_allowed);
        // A name only the parser can spell (it holds a character no identifier may): a desugaring's own variable
        std::pair<uint64_t, std::string_view> hidden_name(std::string_view stem);
        expression_ptr hidden_identifier(uint64_t id, std::string_view name, const source_location& loc);
        // A word or spelling another language uses, where it would otherwise fail to parse (detail/foreign_keyword.hpp)
        bool foreign_word_at_statement();
        void report_foreign(detail::foreign_word w, const token& at);
        // The construct's one error ends the parse: its remains would only report errors of their own
        void stop_at_foreign(detail::foreign_word w, const token& at);
        bool unit_declares_type_name(std::string_view name) const;
        std::optional<detail::foreign_word> foreign_type_word(std::string_view name) const;
        bool std_qualifier_ahead() const;
        
        // expression parsing (precedence climbing)
        checked_result<expression_ptr> expression();
        checked_result<expression_ptr> null_chain_rest(size_t chain_start, const std::vector<size_t>& links, expression_ptr expr);
        static void mark_null_chain(jai::expression* rewritten);
        std::vector<detail::generic_definition*> file_generics_;   // the generics of the declarations this parse keeps (their check mode)
        uint8_t inherited_check_mode_ = check_mode_attr_none;
        // an operand and every operator after it that binds at least min_precedence (= is 0, * / % the tightest)
        checked_result<expression_ptr> operator_expression(int min_precedence);
        checked_result<expression_ptr> unary();
        checked_result<expression_ptr> postfix();
        checked_result<expression_ptr> primary();
        // what follows a production's first operand parses in a continuation of its own: a level of nesting holds only small frames
        checked_result<expression_ptr> operator_rest(expression_ptr left, int min_precedence, size_t operand_start);
        checked_result<expression_ptr> assignment_rest(expression_ptr target, size_t target_start);
        checked_result<expression_ptr> coalesce_assignment(expression_ptr target, size_t target_start);
        checked_result<expression_ptr> ternary_rest(expression_ptr condition);
        checked_result<expression_ptr> coalesce_rest(expression_ptr left);
        checked_result<expression_ptr> unary_operator();
        bool postfix_operator_ahead(const expression_ptr& expr);
        bool type_arguments_ahead(jai::expression& head);
        checked_result<expression_ptr> type_argument_call(expression_ptr callee);
        checked_result<expression_ptr> generic_instance_reference(expression_ptr callee, std::vector<type_info_ptr> arguments, const token& at);

        // Generics (docs/site/guide/21-generics.html, detail/generics.hpp)
        // the stream index after the type-argument list that opens at `open` ('<'), or SIZE_MAX when the run is not one; `split`
        // when the list closes on the first half of a '>>' (the index is then that token's)
        size_t type_arguments_end(size_t open, bool& split, bool placeholders = false) const;
        size_t type_run_end(size_t at) const;            // the index after the type spelled at `at`, or SIZE_MAX
        size_t type_parameters_end(size_t open) const;   // the index after a declaration's '<' ... '>' list, or SIZE_MAX
        // [[attribute]]* [static | override | coroutine]* (function | Type [&]) Name '<' ... '>' '(' at `at`: the index of Name, or
        // SIZE_MAX. The modifiers are a class member's.
        size_t generic_function_head(size_t at, bool member = false) const;
        // A generic's declaration as its tokens, from stream index `begin` (an attribute, a modifier or the head) with the name at
        // `name_at`: read once here for its syntax, its parameters standing as classes; the declaration it would have been is discarded
        struct generic_site {
            detail::generic_kind kind;
            size_t begin;
            size_t name_at;
            std::string_view space;   // the namespace around it
            std::string_view owner;   // a method's class, by its qualified name
            uint8_t visibility = 0;   // a method's: class_decl::member_visibility
        };
        checked_result<declaration_ptr> generic_declaration(const generic_site& site);
        bool generic_parameters(detail::generic_definition& definition, size_t open, size_t& list_end);
        bool generic_extent(detail::generic_kind kind, size_t list_end, size_t& end);
        bool reserved_parameter_names(const detail::generic_definition& definition);
        // A class member's head that lists type parameters where no generic can stand (a constructor's, an operator's): its error, and true
        bool refuse_generic_member_head(std::string_view class_name);
        declaration_ptr parse_generic_text(const std::vector<token>& text, detail::generic_definition* definition = nullptr);
        size_t attributes_end(size_t at) const;   // past the [[attribute]] groups at `at`
        // The instances this parse's text names since its last top-level declaration began: each becomes a generic_instance_decl
        // ahead of that declaration
        struct generic_use {
            std::string_view name;
            uint64_t name_id = 0;
            std::string_view generic;
            std::vector<type_info_ptr> arguments;
            source_location at;
            bool as_value = false;
        };
        std::vector<generic_use> generic_uses_;
        std::unordered_set<uint64_t> requested_instances_;
        void note_generic_use(std::string_view generic, std::vector<type_info_ptr> arguments, const source_location& at, bool as_value = false);
        void flush_generic_uses(std::vector<declaration_ptr>& out);
        checked_result<expression_ptr> postfix_rest(expression_ptr expr, size_t chain_start, bool chain_starts_clean);
        checked_result<expression_ptr> postfix_operator(expression_ptr expr, std::vector<size_t>& null_links);
        checked_result<expression_ptr> brace_construction(expression_ptr expr);
        checked_result<expression_ptr> static_member_access(expression_ptr expr);
        checked_result<expression_ptr> subscript(expression_ptr expr);
        checked_result<expression_ptr> primary_rest();
        checked_result<expression_ptr> new_expression(bool make_shared_spelling);
        checked_result<expression_ptr> identifier_expression();
        std::optional<checked_result<expression_ptr>> type_constructor_expression();
        checked_result<expression_ptr> parenthesized();
        checked_result<expression_ptr> bracket_expression();
        checked_result<expression_ptr> cast_expression();
        checked_result<expression_ptr> dynamic_cast_expression();
        int infix_precedence() const;
        
        // Helper parsers
        checked_result<expression_ptr> finish_call(expression_ptr callee);
        checked_result<expression_ptr> finish_member_access(expression_ptr object, bool is_arrow);
        bool looks_like_map_literal();
        bool json_map_key_follows();  // Lookahead to distinguish map literals from blocks
        bool json_map_ahead(size_t offset = 1) const;
        checked_result<expression_ptr> parse_map_literal();
        expression_ptr identifier_map_key();
        checked_result<type_info_ptr> parse_type();
        checked_result<type_info_ptr> parse_type_head();
        // the generic types nest: each parses in its own frame and builds its type_info in another (a type_info is large)
        checked_result<type_info_ptr> array_type();
        checked_result<type_info_ptr> map_type();
        checked_result<type_info_ptr> weak_ptr_type();
        checked_result<type_info_ptr> shared_ptr_type();
        bool callable_type_ahead() const;   // function '<' / coroutine '<': a type, never a declaration's keyword
        bool callable_type_at(size_t at) const;   // the same asked of the stream at an index (type_arguments_end)
        checked_result<type_info_ptr> signature_type();
        checked_result<type_info_ptr> coroutine_type();
        checked_result<declaration_ptr> callable_typed_declaration();
        checked_result<type_info_ptr> named_type();
        // Name<Args> with the '<' next: the instance's type; a script generic's instance is asked for (note_generic_use)
        checked_result<type_info_ptr> instance_type(const std::string& type_name, bool host_template);
        // A builtin type just read: with type arguments after it (int<string>) it is read as an instance all the same, so the
        // load refuses it once, by the kind of the name
        checked_result<type_info_ptr> builtin_type(std::string_view name, type_info_ptr type);
        type_info_ptr byte_element_type();
        type_info_ptr array_type_of(const type_info_ptr& element);
        type_info_ptr map_type_of(const type_info_ptr& key, const type_info_ptr& value);
        type_info_ptr weak_ptr_type_of(const type_info_ptr& pointee);
        type_info_ptr shared_ptr_type_of(const type_info_ptr& pointee);

        // Constant folding optimization: evaluates literal operations at parse time
        expression_ptr try_constant_fold(expression_ptr left, const token& op, expression_ptr right);

        type_info_ptr store_type_info(type_info&& info);

        checked_result<std::vector<parameter>> parse_parameter_list();

        // Lambda parsing (anonymous `function (params) {...}` desugars to a no-capture
        // lambda through the shared tail)
        checked_result<expression_ptr> lambda_expression();
        checked_result<void> lambda_captures(lambda_expr& lambda);
        checked_result<void> lambda_signature(lambda_expr& lambda);
        // the body text connect_source saves, for a lambda that captures only values and whose body is a block from this source
        void note_saved_source(lambda_expr& lambda, const token& body_open);
        checked_result<expression_ptr> anonymous_function_expression();
        checked_result<expression_ptr> finish_lambda_after_captures(std::shared_ptr<lambda_expr> lambda);
        // the scope, the parameter slots and the body of a lambda whose parameters are parsed; an arrow function's body may
        // be one expression, its value returned
        checked_result<expression_ptr> finish_lambda_body(std::shared_ptr<lambda_expr> lambda, bool expression_body);
        checked_result<expression_ptr> lambda_expression_body(std::shared_ptr<lambda_expr> lambda);
        void discard_returned_call(return_stmt& a_return) const;
        static bool declares_a_value(const type_info* a_type);
        // (params) => expr, (params) => { body }, name => expr: a function expression, closing over its scope by reference
        bool arrow_function_ahead() const;
        // the ')' that closes each '(' of a token stream, found once per stream: nested groups each ask arrow_function_ahead, which
        // would otherwise rescan the whole group at every level
        uint32_t paren_close(size_t open) const;
        mutable std::unordered_map<const std::vector<token>*, std::vector<uint32_t>> paren_close_;
        checked_result<expression_ptr> arrow_function_expression();
        checked_result<std::pair<std::vector<lambda_expr::capture>, lambda_expr::capture_default>> parse_capture_list();

        // Helper for parsing function bodies
        checked_result<declaration_ptr> parse_function_body(std::string_view name, uint64_t name_id, type_info_ptr return_type, bool allow_ctor_initializers = false);

        // Consumes an optional '&' after a return type and wraps it as a reference
        // return (int& f() / -> int& / auto&). No-op when the next token isn't '&'.
        type_info_ptr wrap_reference_return_type(type_info_ptr return_type);
        bool reference_method_head() const;
        static std::string reference_storage_text(const type_info_ptr& referent, std::string_view slot);
        
        // Helper for parsing > in generic contexts (handles >> token splitting)
        void consume_greater_in_generic(std::string_view message);
        
        // every recursive production asks it first: nesting past the host thread's stack is one parse error, never 0xC00000FD
        detail::native_stack_mark stack_;

        // what a break or continue in the statement being parsed would leave, within its function body: a parallel_for body is a
        // loop for continue alone (its iterations are independent)
        struct jump_targets { int breakable = 0; int continuable = 0; bool parallel_body = false; };
        jump_targets jumps_;
        struct jump_scope {
            jump_targets& held;
            jump_targets saved;
            jump_scope(jump_targets& targets, jump_targets inner) : held(targets), saved(targets) { held = inner; }
            ~jump_scope() { held = saved; }
            jump_scope(const jump_scope&) = delete;
            jump_scope& operator=(const jump_scope&) = delete;
        };

        // Context tracking for context-sensitive parsing
        bool in_switch_case_ = false;  // Track if we're inside a switch case
        bool in_coroutine_ = false;
        bool in_constructor_ = false;   // the body being parsed is a constructor's own (a value return is an error)
        bool postfix_marker_ahead_ = false;
        std::string_view enclosing_class_;   // the class whose body is being parsed (empty: none): what `this` names   // the next parameter list is operator++'s or operator--'s: a lone (int) is C++'s postfix marker
        bool returns_reference_ = false;   // the body being parsed returns T&: its return operand binds as a reference
        bool returns_value_ = false;       // the body being parsed declares a value to return (return_stmt::value_discarded)
        // Armed by a coroutine head; parse_function_body consumes it for exactly the next
        // body, so nested functions and lambdas never inherit yield permission
        bool coroutine_body_pending_ = false;

        bool is_registered_template_type(const std::string& type_name) const;

        uint64_t get_symbol_id(const token& tok) const;

        // Slot-based local variable tracking: the local slots of the function being parsed, read by index at run time
        struct function_scope {
            size_t next_slot = 0;  // Next available slot index
            std::unordered_map<uint64_t, size_t> symbol_to_slot;  // symbol_id -> slot
            // Lexical scoping for the lookup map: block scopes log (symbol, previous slot
            // or SIZE_MAX) so end_block_scope restores shadowed/expired names. Slots stay
            // unique per declaration; only the name->slot visibility is scoped.
            std::vector<std::pair<uint64_t, size_t>> shadow_undo;
            std::vector<size_t> block_marks;
            lambda_expr* owner = nullptr;  // the lambda this scope belongs to (chain-capture target)
            uint64_t nested_function = 0;  // a named function declared inside another function's body: it captures nothing
        };
        std::vector<function_scope> function_scope_stack_;  // Stack for nested functions/lambdas
        int nesting_depth_ = 0;   // function bodies, blocks, class and namespace bodies: an import is a top-level declaration only
        struct nesting_guard { int& depth; explicit nesting_guard(int& d) : depth(d) { ++depth; } ~nesting_guard() { --depth; } };

        // Enter a new function scope (call at start of function/lambda parsing)
        void enter_function_scope(lambda_expr* owner = nullptr) {
            function_scope_stack_.emplace_back();
            function_scope_stack_.back().owner = owner;
            ++nesting_depth_;
        }

        // Exit current function scope, returns the total slot count
        size_t exit_function_scope() {
            if (function_scope_stack_.empty()) return 0;
            size_t count = function_scope_stack_.back().next_slot;
            function_scope_stack_.pop_back();
            --nesting_depth_;
            return count;
        }

        void begin_block_scope() {
            if (function_scope_stack_.empty()) {
                program_block_marks_.push_back(program_block_variables_.size());
                return;
            }
            auto& scope = function_scope_stack_.back();
            scope.block_marks.push_back(scope.shadow_undo.size());
        }

        void end_block_scope() {
            if (function_scope_stack_.empty()) {
                if (!program_block_marks_.empty()) {
                    program_block_variables_.resize(program_block_marks_.back());
                    program_block_marks_.pop_back();
                }
                return;
            }
            auto& scope = function_scope_stack_.back();
            if (scope.block_marks.empty()) return;
            const size_t mark = scope.block_marks.back();
            scope.block_marks.pop_back();
            while (scope.shadow_undo.size() > mark) {
                const auto [symbol_id, previous_slot] = scope.shadow_undo.back();
                scope.shadow_undo.pop_back();
                if (previous_slot == SIZE_MAX) {
                    scope.symbol_to_slot.erase(symbol_id);
                } else {
                    scope.symbol_to_slot[symbol_id] = previous_slot;
                }
            }
        }

        // RAII for begin/end_block_scope across the parser's early-return error paths
        struct block_scope_guard {
            parser& p_;
            explicit block_scope_guard(parser& p) : p_(p) { p_.begin_block_scope(); }
            ~block_scope_guard() { p_.end_block_scope(); }
            block_scope_guard(const block_scope_guard&) = delete;
            block_scope_guard& operator=(const block_scope_guard&) = delete;
        };

        // Declares a variable in the scope being parsed: its slot in a function. At program scope it has none (SIZE_MAX), and a
        // block's variable is kept by name until the block closes
        size_t allocate_slot(uint64_t symbol_id) {
            if (function_scope_stack_.empty()) {
                if (!program_block_marks_.empty()) { program_block_variables_.push_back(symbol_id); }
                return SIZE_MAX;
            }
            auto& scope = function_scope_stack_.back();
            size_t slot = scope.next_slot++;
            if (!scope.block_marks.empty()) {
                auto existing = scope.symbol_to_slot.find(symbol_id);
                scope.shadow_undo.emplace_back(symbol_id,
                    existing != scope.symbol_to_slot.end() ? existing->second : SIZE_MAX);
            }
            scope.symbol_to_slot[symbol_id] = slot;
            return slot;
        }

        // Slot resolution floor: scopes BELOW this stack index are invisible to
        // lookup_slot. parallel_for bodies raise it to their own scope — they execute
        // as synthesized worker functions, so an enclosing function's slot stamped on
        // a body identifier would read a foreign frame. Body names resolve by symbol
        // through the worker environment instead (captures are provisioned there).
        size_t slot_lookup_boundary_ = 0;
        // Per parallel_for being parsed: (symbol, slot) of the enclosing frame's locals its
        // body names, drained into parallel_for_stmt::frame_captures (the barrier reads that frame)
        std::vector<std::vector<std::pair<uint64_t, size_t>>> parallel_frame_captures_;
        // A use resolves directly to its own scope's slots or its immediate parent's
        // (read from that frame when the lambda is created). A deeper local is chained
        // instead: the lambda created in the declaring frame captures the slot, every
        // lambda between carries the name through its default capture, and the use
        // itself resolves by name at runtime (SIZE_MAX).
        size_t lookup_slot(uint64_t symbol_id) {
            return resolve_outer_use(symbol_id, function_scope_stack_.size());
        }
        // The name is a variable the parse holds and nothing nearer can take: a local variable or a parameter of a function being
        // parsed (its own or one around it), a variable of a block open at program scope, or, outside every class and namespace, a
        // program-scope variable the unit declares above. '<' after it compares; every other head is asked by name.
        bool names_variable(const jai::expression& e) const {
            if (e.get_type() != node_type::identifier_expr) { return false; }
            const uint64_t symbol_id = static_cast<const identifier_expr&>(e).symbol_id;
            // a program-scope variable is the nearest declaration only outside a class and a namespace, whose member may take the name
            if (unit_variables_.count(symbol_id) && enclosing_class_.empty() && current_namespace_path_.empty()) { return true; }
            if (static_cast<const identifier_expr&>(e).global_qualified) { return false; }
            for (const auto& scope : function_scope_stack_) {
                if (scope.symbol_to_slot.count(symbol_id)) { return true; }
            }
            return std::find(program_block_variables_.begin(), program_block_variables_.end(), symbol_id) != program_block_variables_.end();
        }
        std::unordered_set<uint64_t> unit_variables_;   // the program-scope variables declared so far, by name
        // the variables of the blocks open at program scope (a braced block's, a loop's, a condition's, a catch's), by name, and
        // for each open block where its variables start
        std::vector<uint64_t> program_block_variables_;
        std::vector<size_t> program_block_marks_;
        // Capture lists are parsed in the enclosing scope, before the lambda's own opens
        size_t lookup_capture_slot(uint64_t symbol_id) {
            return resolve_outer_use(symbol_id, function_scope_stack_.size() + 1);
        }
        // `this` and `super` cross every lambda between the use and its method: each must capture the instance
        void check_instance_capture(const token& at) {
            for (size_t i = slot_lookup_boundary_; i < function_scope_stack_.size(); ++i) {
                const lambda_expr* owner = function_scope_stack_[i].owner;
                if (!owner || owner->default_capture != lambda_expr::capture_default::none) { continue; }
                if (std::none_of(owner->captures.begin(), owner->captures.end(), [](const auto& c) { return c.name == "this"; })) {
                    report_error("lambda capture list does not capture 'this'; use [this], [=] or [&]", at);
                    return;
                }
            }
        }
        size_t resolve_outer_use(uint64_t symbol_id, size_t use_scope_count) {
            for (size_t i = function_scope_stack_.size(); i-- > slot_lookup_boundary_;) {
                auto found = function_scope_stack_[i].symbol_to_slot.find(symbol_id);
                if (found == function_scope_stack_[i].symbol_to_slot.end()) continue;
                for (size_t j = i + 1; j < function_scope_stack_.size() && j < use_scope_count; ++j) {
                    if (const uint64_t nested = function_scope_stack_[j].nested_function) {
                        report_error("nested function '" + std::string(symbolizer_->get_string(nested)) + "' cannot use enclosing local '" +
                                     std::string(symbolizer_->get_string(symbol_id)) + "': a named function captures nothing; use a lambda with a capture ([&], [=] or [" +
                                     std::string(symbolizer_->get_string(symbol_id)) + "]) or function(...) / =>", previous());
                        return SIZE_MAX;
                    }
                    auto* owner = function_scope_stack_[j].owner;
                    if (!owner || owner->default_capture != lambda_expr::capture_default::none) { continue; }
                    if (std::none_of(owner->captures.begin(), owner->captures.end(), [symbol_id](const auto& c) { return c.symbol_id == symbol_id; })) {
                        report_error("lambda capture list does not capture enclosing local '" + std::string(symbolizer_->get_string(symbol_id)) +
                                     "'; name it or use [=] or [&]", previous());
                        return SIZE_MAX;
                    }
                }
                if (i + 2 >= use_scope_count) return found->second;
                if (lambda_expr* capturer = function_scope_stack_[i + 1].owner) {
                    auto& slots = capturer->chained_outer_slots;
                    if (std::find_if(slots.begin(), slots.end(), [&](const auto& e) { return e.first == symbol_id; }) == slots.end()) {
                        slots.emplace_back(symbol_id, found->second);
                    }
                }
                for (size_t j = i + 2; j < function_scope_stack_.size(); ++j) {
                    if (lambda_expr* carrier = function_scope_stack_[j].owner) {
                        auto& names = carrier->chained_outer_names;
                        if (std::find(names.begin(), names.end(), symbol_id) == names.end()) names.push_back(symbol_id);
                    }
                }
                return SIZE_MAX;
            }
            // A parallel body's free name that is a local of the frame its statement runs in:
            // recorded for the region barrier, never stamped on the body identifier
            if (slot_lookup_boundary_ > 0 && !parallel_frame_captures_.empty()) {
                const auto& frame = function_scope_stack_[slot_lookup_boundary_ - 1];
                if (auto found = frame.symbol_to_slot.find(symbol_id); found != frame.symbol_to_slot.end()) {
                    auto& captures = parallel_frame_captures_.back();
                    if (std::find_if(captures.begin(), captures.end(), [&](const auto& e) { return e.first == symbol_id; }) == captures.end()) {
                        captures.emplace_back(symbol_id, found->second);
                    }
                }
            }
            return SIZE_MAX;
        }

        // Check if we're currently inside a function scope
        bool in_function_scope() const {
            return !function_scope_stack_.empty();
        }
    };

    namespace detail {
        // The parse-tail AST marking passes (ref-escape/value-decl/typed-store-proof
        // flags + transient-read classification), re-runnable on DESERIALIZED trees:
        // jaibite does not carry the parser-pass flags, so loads must re-derive them
        // from the one source of truth — the passes themselves. True when the tree names
        // time::execute_elapsed (engine::note_execute_elapsed_use).
        bool run_ast_marking_passes(const std::vector<declaration_ptr>& declarations,
                                    string_symbolizer* symbolizer);
    }

} // namespace jai
