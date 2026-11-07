#pragma once

#include <jaiscript/core/types.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace jai {

    enum class token_type {
        integer_literal,
        float_literal,
        string_literal,
        char_literal,
        boolean_literal,
        null_literal,

        identifier,

        bool_keyword, break_keyword, char_keyword, class_keyword, const_keyword, continue_keyword, else_keyword, false_keyword, float_keyword, for_keyword,
        function_keyword, if_keyword, int_keyword, map_keyword, array_keyword, namespace_keyword, null_keyword, override_keyword, private_keyword, public_keyword, return_keyword, static_keyword, string_keyword,
        this_keyword, true_keyword, void_keyword, while_keyword, auto_keyword, var_keyword, super_keyword, weak_ptr_keyword, shared_ptr_keyword,
        try_keyword, catch_keyword, throw_keyword,
        switch_keyword, case_keyword, default_keyword, fallthrough_keyword,
        include_keyword, import_keyword,
        coroutine_keyword, yield_keyword,
        enum_keyword, new_keyword, protected_keyword,
        parallel_for_keyword, nocopy_keyword, cast_keyword, extern_keyword,

        plus, minus, star, slash, percent,
        plus_equal, minus_equal, star_equal, slash_equal, percent_equal,
        equal, equal_equal, bang_equal,
        less, greater, less_equal, greater_equal,
        ampersand_ampersand, pipe_pipe, bang,
        plus_plus, minus_minus,
        dot, arrow,
        ampersand, pipe, caret,
        colon_colon,
        question, question_dot, colon,
        tilde,  // For bitwise NOT and destructors: ~ClassName()
        spaceship,  // C++20 three-way comparison operator <=>
        left_shift, right_shift,

        left_paren, right_paren,
        left_brace, right_brace,
        left_bracket, right_bracket,
        left_angle, right_angle,
        semicolon, comma,

        eof,
        error,

        ampersand_equal, pipe_equal, caret_equal, left_shift_equal, right_shift_equal,   // appended: .jaibite stores token numbers
        inline_keyword,
        question_question, question_question_equal, fat_arrow
    };
    inline constexpr token_type k_last_token_type = token_type::fat_arrow;   // the bound a .jaibite's token numbers are read against: move it with a new last member
    
    class string_symbolizer;

    struct token {
        token_type type;
        bool joins_text = false;   // a template string's own '+': its splice becomes text, as `${x}` promises
        std::string_view lexeme;  // Points to symbolizer storage (permanent) or static strings
        uint64_t symbol_id = 0;   // Pre-interned symbol ID for identifiers (0 = not interned)
        source_location location;
        std::string_view using_name;   // the first token of a name a using declaration rewrote: the name as written (declaration sites reject it)

        union {
            script_int int_value;
            script_float float_value;
            script_bool bool_value;
            script_char char_value;
        };
        std::string string_value;  // For string literals (can't be in union)

        token() : type(token_type::eof), lexeme(), location(), int_value(0) {}

        token(token_type t, std::string_view lex, const source_location& loc)
            : type(t), lexeme(lex), location(loc), int_value(0) {}

        token(token_type t, std::string_view lex, uint64_t sym_id, const source_location& loc)
            : type(t), lexeme(lex), symbol_id(sym_id), location(loc), int_value(0) {}

        std::string to_string() const;
        bool is_keyword() const;
    };
    
    class lexer {
    public:
        // Constructors require symbolizer for proper identifier interning
        lexer(const std::string& source, string_symbolizer* symbolizer, const std::string& filename = "<script>");

        std::vector<token> tokenize();
        // the source as a template file's text: joiner(part, splice((expr)), splice((expr), "spec"), ...) then eof
        std::vector<token> tokenize_template(uint64_t joiner, uint64_t splice);
        token next_token();
        bool is_at_end() const { return current_ >= source_.length(); }
        static bool is_reserved_word(std::string_view word);

    private:
        std::string source_;
        std::string filename_;
        string_symbolizer* symbolizer_;  // Required: for interning identifiers to permanent storage
        size_t current_ = 0;
        size_t line_ = 1;
        size_t column_ = 1;


        struct keyword_hash {
            using is_transparent = void;
            size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
        };
        static const std::unordered_map<std::string, token_type, keyword_hash, std::equal_to<>> keywords_;   // looked up by view

        char advance();
        char peek() const;
        char peek_next() const;
        bool match(char expected);
        void skip_whitespace();
        void skip_comment();

        token make_token(token_type type);
        token make_token(token_type type, std::string_view lexeme);
        token error_token(const std::string& message);
        size_t token_line_ = 1;     // where the token being scanned starts: its location, an error's too
        size_t token_column_ = 1;
        source_location token_start() const;

        token scan_number();
        token scan_string();
        token scan_raw_string();
        token scan_char();
        token scan_identifier();
        void scan_template_string();
        bool scan_splice(std::vector<token>& out, std::vector<token>& expr_tokens, std::string& spec_text, bool& has_spec);
        void emit_splice_call(std::vector<token>& out, uint64_t callee, std::vector<token>& expr_tokens, const std::string& spec_text, bool has_spec,
                              const std::string* description = nullptr);

        // Pending token queue for template string desugaring
        std::vector<token> pending_tokens_;
        size_t pending_index_ = 0;

        bool is_digit(char c) const;
        bool is_alpha(char c) const;
        bool is_alpha_numeric(char c) const;
        source_location current_location() const;
    };
    
} // namespace jai