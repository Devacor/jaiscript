#include "../../include/jaiscript/detail/lexer.hpp"
#include "../../include/jaiscript/detail/string_symbolizer.hpp"
#include "../../include/jaiscript/detail/format_spec.hpp"
#include "../../include/jaiscript/detail/float_text.hpp"
#include <cctype>
#include <sstream>

namespace jai {

namespace {
    // \xNN escapes take EXACTLY two hex digits ("\x41BC" is 'A' then "BC"), unlike C++'s
    // consume-all-digits rule whose overflow behavior is a known wart.
    inline int hex_digit_value(char c) {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    }
}

// Initialize keyword map
const std::unordered_map<std::string, token_type, lexer::keyword_hash, std::equal_to<>> lexer::keywords_ = {
    {"bool", token_type::bool_keyword},
    {"break", token_type::break_keyword},
    {"char", token_type::char_keyword},
    {"class", token_type::class_keyword},
    {"const", token_type::const_keyword},
    {"continue", token_type::continue_keyword},
    {"else", token_type::else_keyword},
    {"false", token_type::false_keyword},
    {"float", token_type::float_keyword},
    {"for", token_type::for_keyword},
    {"function", token_type::function_keyword},
    {"if", token_type::if_keyword},
    {"int", token_type::int_keyword},
    {"map", token_type::map_keyword},
    {"array", token_type::array_keyword},
    {"namespace", token_type::namespace_keyword},
    {"null", token_type::null_keyword},
    {"nullptr", token_type::null_keyword},
    {"override", token_type::override_keyword},
    {"private", token_type::private_keyword},
    {"public", token_type::public_keyword},
    {"return", token_type::return_keyword},
    {"static", token_type::static_keyword},
    {"string", token_type::string_keyword},
    {"this", token_type::this_keyword},
    {"true", token_type::true_keyword},
    {"void", token_type::void_keyword},
    {"while", token_type::while_keyword},
    {"auto", token_type::auto_keyword},
    {"var", token_type::var_keyword},
    {"let", token_type::var_keyword},
    {"super", token_type::super_keyword},
    {"weak_ptr", token_type::weak_ptr_keyword},
    {"shared_ptr", token_type::shared_ptr_keyword},
    {"try", token_type::try_keyword},
    {"catch", token_type::catch_keyword},
    {"throw", token_type::throw_keyword},
    {"switch", token_type::switch_keyword},
    {"case", token_type::case_keyword},
    {"default", token_type::default_keyword},
    {"fallthrough", token_type::fallthrough_keyword},
    {"include", token_type::include_keyword},
    {"import", token_type::import_keyword},
    {"coroutine", token_type::coroutine_keyword},
    {"yield", token_type::yield_keyword},
    {"enum", token_type::enum_keyword},
    {"new", token_type::new_keyword},
    {"protected", token_type::protected_keyword},
    {"parallel_for", token_type::parallel_for_keyword},
    {"nocopy", token_type::nocopy_keyword},
    {"cast", token_type::cast_keyword},
    {"extern", token_type::extern_keyword},
    {"inline", token_type::inline_keyword},
};

lexer::lexer(const std::string& source, string_symbolizer* symbolizer, const std::string& filename)
    : source_(source), filename_(filename), symbolizer_(symbolizer), current_(0), line_(1), column_(1) {
    // A leading UTF-8 BOM (Notepad's / PowerShell Set-Content's default) is invisible
    // content, not a token — strip it once here, at position 0 only. A U+FEFF later in
    // the file is left alone (it is a real, if exotic, character there).
    if (source_.rfind("\xEF\xBB\xBF", 0) == 0) source_.erase(0, 3);
}

std::vector<token> lexer::tokenize() {
    std::vector<token> tokens;
    // Loop until next_token() actually produces EOF, rather than gating on
    // is_at_end(). A template-string literal (`...`) desugars into a queue of
    // pending tokens; scan_template_string() consumes the source up to and
    // including the closing backtick, so if the literal is the LAST content in
    // the source, is_at_end() becomes true after the first pending token is
    // returned. Gating the loop on !is_at_end() would then drop the remaining
    // pending tokens. next_token() drains the pending queue first and only
    // returns EOF once it is empty AND the source is exhausted.
    while (true) {
        token token = next_token();
        tokens.push_back(token);
        if (token.type == token_type::eof) {
            break;
        }
    }
    return tokens;
}

token lexer::next_token() {
    if (pending_index_ < pending_tokens_.size()) {
        return pending_tokens_[pending_index_++];
    }
    if (!pending_tokens_.empty()) {
        pending_tokens_.clear();
        pending_index_ = 0;
    }

    // A UTF-16 BOM can only lead the file; catch it here (position 1:1) with a clear
    // encoding message instead of a garbage 'Unexpected character' on the 0xFF/0xFE byte.
    // Consume the rest so the run ends in eof rather than re-firing the error forever.
    if (current_ == 0 && source_.size() >= 2) {
        unsigned char b0 = static_cast<unsigned char>(source_[0]);
        unsigned char b1 = static_cast<unsigned char>(source_[1]);
        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
            current_ = source_.size();
            return error_token("UTF-16 encoding is not supported - save the file as UTF-8");
        }
    }

    if (current_ == 0 && source_.compare(0, 2, "#!") == 0) {
        skip_comment();   // a shebang line: the program loader's, never the script's
    }

    skip_whitespace();
    token_line_ = line_;
    token_column_ = column_;

    if (is_at_end()) {
        return make_token(token_type::eof);
    }

    char c = advance();
    
    // Numbers
    if (is_digit(c)) {
        current_--;  // Back up to rescan
        column_--;
        return scan_number();
    }
    
    // C++ raw string literals: R"(...)" and R"delim(...)delim"
    if (c == 'R' && peek() == '"') {
        return scan_raw_string();
    }

    // Identifiers and keywords
    if (is_alpha(c) || c == '_') {
        current_--;  // Back up to rescan
        column_--;
        return scan_identifier();
    }
    
    if (c == '`') {
        scan_template_string();
        if (pending_index_ < pending_tokens_.size()) {
            return pending_tokens_[pending_index_++];
        }
        return make_token(token_type::eof);
    }

    // script_string literals
    if (c == '"') {
        return scan_string();
    }
    
    // Character literals
    if (c == '\'') {
        return scan_char();
    }
    
    // Single character tokens
    switch (c) {
        case '(': return make_token(token_type::left_paren);
        case ')': return make_token(token_type::right_paren);
        case '{': return make_token(token_type::left_brace);
        case '}': return make_token(token_type::right_brace);
        case '[': return make_token(token_type::left_bracket);
        case ']': return make_token(token_type::right_bracket);
        case ';': return make_token(token_type::semicolon);
        case ',': return make_token(token_type::comma);
        case '?':
            if (peek() == '.' && !is_digit(peek_next())) {   // c?.5:1 is a ternary over the literal .5, as in C++
                advance();
                return make_token(token_type::question_dot);
            }
            if (match('?')) {
                if (match('=')) return make_token(token_type::question_question_equal);
                return make_token(token_type::question_question);
            }
            return make_token(token_type::question);
        case '~': return make_token(token_type::tilde);
        case '^':
            if (match('=')) return make_token(token_type::caret_equal);
            return make_token(token_type::caret);
        
        // #include and #import are include and import (C++ muscle memory); '#' begins nothing else
        case '#': {
            const size_t hash_line = token_line_;
            const size_t hash_column = token_column_;
            skip_whitespace();
            token next = next_token();
            if (next.type == token_type::include_keyword || next.type == token_type::import_keyword) {
                return next;
            }
            token_line_ = hash_line;
            token_column_ = hash_column;
            if (next.type == token_type::identifier) {
                const std::string name(next.lexeme);
                return error_token("unexpected '#' before '" + name + "': JaiScript has no length operator; write " + name + ".size()");
            }
            return error_token("unexpected '#': it begins only #include, #import and a first-line #! shebang");
        }
        
        // Operators that might be compound
        case '+':
            if (match('+')) return make_token(token_type::plus_plus);
            if (match('=')) return make_token(token_type::plus_equal);
            return make_token(token_type::plus);
            
        case '-':
            if (match('-')) return make_token(token_type::minus_minus);
            if (match('=')) return make_token(token_type::minus_equal);
            if (match('>')) return make_token(token_type::arrow);
            return make_token(token_type::minus);
            
        case '*':
            if (match('=')) return make_token(token_type::star_equal);
            return make_token(token_type::star);
            
        case '/':
            if (match('/')) {
                // Single line comment
                skip_comment();
                return next_token();
            }
            if (match('*')) {
                while (!(peek() == '*' && peek_next() == '/')) {
                    if (is_at_end()) { return error_token("unterminated block comment: no */ closes it"); }
                    if (advance() == '\n') {
                        line_++;
                        column_ = 1;
                    }
                }
                advance();
                advance();
                return next_token();
            }
            if (match('=')) return make_token(token_type::slash_equal);
            return make_token(token_type::slash);
            
        case '%':
            if (match('=')) return make_token(token_type::percent_equal);
            return make_token(token_type::percent);
            
        case '&':
            if (match('&')) return make_token(token_type::ampersand_ampersand);
            if (match('=')) return make_token(token_type::ampersand_equal);
            return make_token(token_type::ampersand);
            
        case '|':
            if (match('|')) return make_token(token_type::pipe_pipe);
            if (match('=')) return make_token(token_type::pipe_equal);
            return make_token(token_type::pipe);
            
        case '!':
            if (match('=')) return make_token(token_type::bang_equal);
            return make_token(token_type::bang);
            
        case '=':
            if (match('=')) return make_token(token_type::equal_equal);
            if (match('>')) return make_token(token_type::fat_arrow);
            return make_token(token_type::equal);
            
        case '<':
            if (match('=')) {
                if (match('>')) return make_token(token_type::spaceship); // <=>
                return make_token(token_type::less_equal); // <=
            }
            if (match('<')) {
                if (match('=')) return make_token(token_type::left_shift_equal); // <<=
                return make_token(token_type::left_shift); // <<
            }
            return make_token(token_type::less);
            
        case '>':
            if (match('=')) return make_token(token_type::greater_equal);
            if (match('>')) {
                if (match('=')) return make_token(token_type::right_shift_equal); // >>=
                return make_token(token_type::right_shift); // >>
            }
            return make_token(token_type::greater);
            
        case '.':
            if (is_digit(peek())) {   // C++'s .5
                current_--;
                column_--;
                return scan_number();
            }
            return make_token(token_type::dot);
            
        case ':':
            if (match(':')) return make_token(token_type::colon_colon);
            return make_token(token_type::colon);
    }
    
    return error_token("unexpected character");
}

char lexer::advance() {
    column_++;
    return source_[current_++];
}

char lexer::peek() const {
    if (is_at_end()) return '\0';
    return source_[current_];
}

char lexer::peek_next() const {
    if (current_ + 1 >= source_.length()) return '\0';
    return source_[current_ + 1];
}

bool lexer::match(char expected) {
    if (is_at_end()) return false;
    if (source_[current_] != expected) return false;
    advance();
    return true;
}

void lexer::skip_whitespace() {
    while (!is_at_end()) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\t':
                advance();
                break;
            case '\r':
                advance();
                // A lone CR (classic-Mac line ending) is a newline; a CR that leads a CRLF
                // is not counted here — the following '\n' counts it (no double-count).
                if (peek() != '\n') {
                    line_++;
                    column_ = 1;
                }
                break;
            case '\n':
                advance();
                line_++;
                column_ = 1;
                break;
            default:
                return;
        }
    }
}

void lexer::skip_comment() {
    while (!is_at_end() && peek() != '\n') {
        advance();
    }
}

token lexer::make_token(token_type type) {
    // Use static string_views for operators (string literals have static storage duration)
    std::string_view lexeme;

    switch (type) {
        case token_type::left_paren: lexeme = "("; break;
        case token_type::right_paren: lexeme = ")"; break;
        case token_type::left_brace: lexeme = "{"; break;
        case token_type::right_brace: lexeme = "}"; break;
        case token_type::left_bracket: lexeme = "["; break;
        case token_type::right_bracket: lexeme = "]"; break;
        case token_type::less: lexeme = "<"; break;
        case token_type::greater: lexeme = ">"; break;
        case token_type::semicolon: lexeme = ";"; break;
        case token_type::comma: lexeme = ","; break;
        case token_type::dot: lexeme = "."; break;
        case token_type::question: lexeme = "?"; break;
        case token_type::colon: lexeme = ":"; break;
        case token_type::plus: lexeme = "+"; break;
        case token_type::minus: lexeme = "-"; break;
        case token_type::star: lexeme = "*"; break;
        case token_type::slash: lexeme = "/"; break;
        case token_type::percent: lexeme = "%"; break;
        case token_type::ampersand: lexeme = "&"; break;
        case token_type::pipe: lexeme = "|"; break;
        case token_type::caret: lexeme = "^"; break;
        case token_type::tilde: lexeme = "~"; break;
        case token_type::bang: lexeme = "!"; break;
        case token_type::equal: lexeme = "="; break;

        // Multi-character operators
        case token_type::plus_plus: lexeme = "++"; break;
        case token_type::minus_minus: lexeme = "--"; break;
        case token_type::plus_equal: lexeme = "+="; break;
        case token_type::minus_equal: lexeme = "-="; break;
        case token_type::star_equal: lexeme = "*="; break;
        case token_type::slash_equal: lexeme = "/="; break;
        case token_type::percent_equal: lexeme = "%="; break;
        case token_type::arrow: lexeme = "->"; break;
        case token_type::colon_colon: lexeme = "::"; break;
        case token_type::equal_equal: lexeme = "=="; break;
        case token_type::bang_equal: lexeme = "!="; break;
        case token_type::less_equal: lexeme = "<="; break;
        case token_type::greater_equal: lexeme = ">="; break;
        case token_type::ampersand_ampersand: lexeme = "&&"; break;
        case token_type::pipe_pipe: lexeme = "||"; break;
        case token_type::spaceship: lexeme = "<=>"; break;
        case token_type::left_shift: lexeme = "<<"; break;
        case token_type::right_shift: lexeme = ">>"; break;
        case token_type::ampersand_equal: lexeme = "&="; break;
        case token_type::pipe_equal: lexeme = "|="; break;
        case token_type::caret_equal: lexeme = "^="; break;
        case token_type::left_shift_equal: lexeme = "<<="; break;
        case token_type::right_shift_equal: lexeme = ">>="; break;
        case token_type::question_question: lexeme = "??"; break;
        case token_type::question_question_equal: lexeme = "??="; break;
        case token_type::fat_arrow: lexeme = "=>"; break;

        default:
            // For other tokens like EOF, just leave empty
            break;
    }

    return token(type, lexeme, token_start());
}

token lexer::make_token(token_type type, std::string_view lexeme) {
    return token(type, lexeme, token_start());
}

source_location lexer::token_start() const {
    source_location result = current_location();
    result.line = token_line_;
    result.column = token_column_;
    return result;
}

token lexer::error_token(const std::string& message) {
    auto [id, view] = symbolizer_->intern_with_view(message);
    return token(token_type::error, view, token_start());
}

token lexer::scan_number() {
    size_t start = current_;
    size_t startColumn = column_;

    bool is_float = false;
    bool is_hex = false;
    bool is_binary = false;

    if (peek() == '0' && !is_at_end()) {
        advance(); // consume '0'
        if (peek() == 'x' || peek() == 'X') {
            is_hex = true;
            advance(); // consume 'x'/'X'
            while (!is_at_end() && (is_digit(peek()) || (peek() >= 'a' && peek() <= 'f') || (peek() >= 'A' && peek() <= 'F'))) {
                advance();
            }
        } else if (peek() == 'b' || peek() == 'B') {
            is_binary = true;
            advance(); // consume 'b'/'B'
            while (!is_at_end() && (peek() == '0' || peek() == '1')) {
                advance();
            }
        } else {
            // Octal or plain 0 — continue with normal digit scanning
            while (is_digit(peek())) {
                advance();
            }
        }
    } else {
        while (is_digit(peek())) {
            advance();
        }
    }

    if (!is_hex && !is_binary) {
        // C++'s floating literal: digits, a '.', optional digits, an optional exponent (1.5, 1., 1.e5); a '.' before a name
        // or another '.' stays the member access or the operator it begins
        if (peek() == '.' && peek_next() != '.' && (!is_alpha(peek_next()) || peek_next() == 'e' || peek_next() == 'E')) {
            is_float = true;
            advance(); // Consume '.'
            while (is_digit(peek())) {
                advance();
            }
        }

        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            if (!is_digit(peek())) {
                std::string_view lexeme_view(source_.data() + start, current_ - start);
                return error_token("invalid number literal: exponent has no digits in '" + std::string(lexeme_view) + "'");
            }
            while (is_digit(peek())) {
                advance();
            }
        }
    }

    // Use string_view into source_ (permanent storage)
    std::string_view lexeme_view(source_.data() + start, current_ - start);
    source_location loc = current_location();
    loc.column = startColumn;

    token tok(is_float ? token_type::float_literal : token_type::integer_literal, lexeme_view, loc);

    // Need temporary string for parsing (stod/stoll need null-terminated)
    std::string lexeme_str(lexeme_view);

    try {
        if (is_float) {
            const auto parsed = detail::parse_float_text(lexeme_str.c_str());
            if (parsed.out_of_range) {
                return error_token("number literal out of range: '" + lexeme_str + "'");
            }
            tok.float_value = parsed.value;
        } else if (is_hex) {
            tok.int_value = std::stoll(lexeme_str, nullptr, 16);
        } else if (is_binary) {
            tok.int_value = std::stoll(lexeme_str.substr(2), nullptr, 2);
        } else if (lexeme_str.size() > 1 && lexeme_str[0] == '0' && is_digit(lexeme_str[1])) {
            // C-style octal (leading 0). std::stoll(base 8) stops at the first
            // non-octal digit WITHOUT erroring (e.g. "08" -> 0, "019" -> 1), so
            // verify the whole literal was consumed; otherwise it is malformed.
            size_t consumed = 0;
            tok.int_value = std::stoll(lexeme_str, &consumed, 8);
            if (consumed != lexeme_str.size()) {
                return error_token("invalid octal literal (digits 8/9 not allowed): '" + lexeme_str + "'");
            }
        } else {
            tok.int_value = std::stoll(lexeme_str);
        }
    } catch (const std::out_of_range&) {
        return error_token("number literal out of range: '" + lexeme_str + "'");
    } catch (const std::invalid_argument&) {
        return error_token("invalid number literal: '" + lexeme_str + "'");
    }

    return tok;
}

// The splice after "${": its expression's tokens and an optional format spec, through the closing '}'. False when the splice
// failed; the lexer's error token is then in `out`.
bool lexer::scan_splice(std::vector<token>& out, std::vector<token>& expr_tokens, std::string& spec_text, bool& has_spec) {
    has_spec = false;
    int brace_depth = 1;
    int paren_depth = 0;
    int bracket_depth = 0;
    int ternary_depth = 0;   // top-level '?' awaiting its ':' — ternaries keep working
    while (!is_at_end() && brace_depth > 0) {
        // Recursive next_token() is safe here because pending_tokens_
        // is not being used (the caller builds into a local vector).
        // Depth bookkeeping happens on the LEXED brace tokens, not raw peeks:
        // whitespace before a brace made next_token() consume it without a
        // depth adjustment (`${ x }` was a parse error, and a space-prefixed
        // '{' swallowed the splice terminator).
        token expr_tok = next_token();
        if (expr_tok.type == token_type::eof) break;
        if (expr_tok.type == token_type::error) {
            out.push_back(expr_tok);   // surface the lexer's message, not a cascade
            return false;
        }
        const bool at_top_level = brace_depth == 1 && paren_depth == 0 && bracket_depth == 0;
        if (expr_tok.type == token_type::left_brace) {
            ++brace_depth;
        } else if (expr_tok.type == token_type::right_brace) {
            --brace_depth;
            if (brace_depth == 0) {
                break;   // splice terminator: not part of the expression
            }
        } else if (expr_tok.type == token_type::left_paren) {
            ++paren_depth;
        } else if (expr_tok.type == token_type::right_paren) {
            --paren_depth;
        } else if (expr_tok.type == token_type::left_bracket) {
            ++bracket_depth;
        } else if (expr_tok.type == token_type::right_bracket) {
            --bracket_depth;
        } else if (expr_tok.type == token_type::question && at_top_level) {
            ++ternary_depth;
        } else if (expr_tok.type == token_type::colon && at_top_level) {
            if (ternary_depth > 0) {
                --ternary_depth;   // ternary's ':' — part of the expression
            } else {
                // A top-level ':' that closes no ternary starts a format spec:
                // raw chars from here to the closing '}' (a bare ':' is never a
                // valid expression continuation, so this claims nothing legal).
                size_t spec_start = current_;
                while (!is_at_end() && peek() != '}' && peek() != '`' && peek() != '\n') {
                    advance();
                }
                if (is_at_end() || peek() != '}') {
                    out.push_back(error_token("unterminated format spec in template string"));
                    return false;
                }
                spec_text = source_.substr(spec_start, current_ - spec_start);
                advance();   // consume '}': splice terminator
                detail::format_spec parsed;
                if (!detail::parse_format_spec(spec_text, parsed)) {
                    out.push_back(error_token("unsupported format spec ':" + spec_text + "' in template string"));
                    return false;
                }
                has_spec = true;
                return true;
            }
        }
        expr_tokens.push_back(expr_tok);
    }
    return true;
}

// `callee((expr))` or `callee((expr), "spec")`, or `(expr)` when callee is 0: a splice as a call
void lexer::emit_splice_call(std::vector<token>& out, uint64_t callee, std::vector<token>& expr_tokens, const std::string& spec_text, bool has_spec,
                             const std::string* description) {
    const source_location at = current_location();
    auto punct = [&](token_type type, const char* text) { out.push_back(token(type, symbolizer_->intern_with_view(text).second, at)); };
    if (callee != 0) {
        out.push_back(token(token_type::identifier, symbolizer_->get_string(callee), callee, at));
        punct(token_type::left_paren, "(");
    }
    punct(token_type::left_paren, "(");
    for (auto& expr_tok : expr_tokens) {
        out.push_back(std::move(expr_tok));
    }
    punct(token_type::right_paren, ")");
    auto text = [&](const std::string& value) {
        punct(token_type::comma, ",");
        token literal(token_type::string_literal, symbolizer_->intern_with_view(value).second, at);
        literal.string_value = value;
        out.push_back(std::move(literal));
    };
    if (has_spec || description) { text(has_spec ? spec_text : std::string()); }
    if (description) { text(*description); }
    if (callee != 0) {
        punct(token_type::right_paren, ")");
    }
}

void lexer::scan_template_string() {
    // Desugar `hello ${expr} world` into: ("" + "hello " + (expr) + " world")
    // Each '+' is marked joins_text: it makes its splice text, which a plain string '+' refuses for a number, bool, char or null.
    // A splice with a format spec, `${expr:spec}`, becomes a call instead:
    // (... + (format_value((expr), "spec")) + ...) — format_value is an
    // engine-core builtin, so both backends execute the same registered entry.
    // IMPORTANT: Build into a local vector, not pending_tokens_, because
    // next_token() is called recursively for ${} expressions and would
    // drain pending_tokens_ instead of scanning the source.
    std::vector<token> result;

    source_location start_loc = current_location();

    auto emit_string_part = [&](const std::string& part) {
        auto [id, view] = symbolizer_->intern_with_view(part);
        token tok(token_type::string_literal, view, current_location());
        tok.string_value = part;
        result.push_back(tok);
    };

    auto emit_plus = [&]() {
        result.push_back(token(token_type::plus, symbolizer_->intern_with_view("+").second, current_location()));
        result.back().joins_text = true;
    };

    // Opening paren and empty string to establish string context
    result.push_back(token(token_type::left_paren, symbolizer_->intern_with_view("(").second, start_loc));
    const size_t leading_part = result.size();
    emit_string_part("");
    bool leading_empty = true;   // literal text before the first splice replaces the leading "" (no "" + "text" concat)
    auto emit_literal = [&](const std::string& part) {
        if (part.empty()) return;   // an empty part joins nothing
        if (leading_empty) { result[leading_part].string_value = part; leading_empty = false; return; }
        emit_plus();
        emit_string_part(part);
    };

    std::string current_part;

    while (!is_at_end() && peek() != '`') {
        if (peek() == '$' && peek_next() == '{') {
            emit_literal(current_part);
            current_part.clear();
            leading_empty = false;

            emit_plus();

            advance(); // skip $
            advance(); // skip {

            std::vector<token> expr_tokens;
            std::string spec_text;
            bool has_spec = false;
            if (scan_splice(result, expr_tokens, spec_text, has_spec)) {
                emit_splice_call(result, has_spec ? symbolizer_->intern("format_value") : 0, expr_tokens, spec_text, has_spec);
            }

        } else if (peek() == '\\') {
            advance(); // skip backslash
            if (!is_at_end()) {
                char escaped = advance();
                switch (escaped) {
                    case 'n': current_part += '\n'; break;
                    case 't': current_part += '\t'; break;
                    case 'r': current_part += '\r'; break;
                    case '\\': current_part += '\\'; break;
                    case '"': current_part += '"'; break;
                    case '\'': current_part += '\''; break;
                    case '`': current_part += '`'; break;
                    case '$': current_part += '$'; break;
                    case 'x': {
                        const int hi = hex_digit_value(peek());
                        const int lo = hi < 0 ? -1 : hex_digit_value(peek_next());
                        if (lo < 0) {
                            result.push_back(error_token("invalid \\x escape: expected two hex digits"));
                            break;
                        }
                        advance();
                        advance();
                        current_part += static_cast<char>(static_cast<unsigned char>(hi * 16 + lo));
                        break;
                    }
                    default: current_part += '\\'; current_part += escaped; break;
                }
            }
        } else if (peek() == '\r') {
            // Template strings may span source lines, so a raw line ending lands in the
            // string VALUE. Normalize CRLF and lone CR to '\n' so a script saved with any
            // line-ending style yields byte-identical strings. (An explicit \r escape,
            // handled above, is untouched.)
            advance();                       // consume '\r'
            if (peek() == '\n') advance();   // fold a trailing '\n' into the one newline
            current_part += '\n';
            line_++; column_ = 1;
        } else {
            if (peek() == '\n') { line_++; column_ = 0; }
            current_part += advance();
        }
    }

    emit_literal(current_part);

    if (!is_at_end() && peek() == '`') {
        advance();
    }

    result.push_back(token(token_type::right_paren, symbolizer_->intern_with_view(")").second, current_location()));

    pending_tokens_ = std::move(result);
    pending_index_ = 0;
}

// A template's text (docs/site/guide/19-templates.html): joiner(part, splice((expr)), ...) then eof. Only "\${" is an escape (a
// literal "${"); every other byte, a backtick or a backslash included, is the page's own. Line endings read as '\n'.
std::vector<token> lexer::tokenize_template(uint64_t joiner, uint64_t splice) {
    std::vector<token> result;
    const source_location start = current_location();
    auto punct = [&](token_type type, const char* text) { result.push_back(token(type, symbolizer_->intern_with_view(text).second, current_location())); };
    result.push_back(token(token_type::identifier, symbolizer_->get_string(joiner), joiner, start));
    result.push_back(token(token_type::left_paren, symbolizer_->intern_with_view("(").second, start));
    bool first = true;
    std::string current_part;
    source_location part_at = current_location();
    auto separate = [&] {
        if (!first) { punct(token_type::comma, ","); }
        first = false;
    };
    auto emit_literal = [&] {
        if (current_part.empty()) { return; }
        separate();
        token tok(token_type::string_literal, symbolizer_->intern_with_view(current_part).second, part_at);
        tok.string_value = std::move(current_part);
        current_part.clear();
        result.push_back(std::move(tok));
    };
    auto note_part_start = [&] { if (current_part.empty()) { part_at = current_location(); } };
    while (!is_at_end()) {
        if (peek() == '$' && peek_next() == '{') {
            emit_literal();
            advance();
            advance();
            std::vector<token> expr_tokens;
            std::string spec_text;
            bool has_spec = false;
            separate();
            const size_t splice_start = current_;
            const size_t splice_line = line_;
            if (!scan_splice(result, expr_tokens, spec_text, has_spec)) { break; }
            // the splice as written and where, for the error a null value raises
            const std::string description = "${" + source_.substr(splice_start, current_ - splice_start) + " at " + filename_ + ":" + std::to_string(splice_line);
            emit_splice_call(result, splice, expr_tokens, spec_text, has_spec, &description);
        } else if (peek() == '\\' && peek_next() == '$' && current_ + 2 < source_.size() && source_[current_ + 2] == '{') {
            note_part_start();
            advance();
            current_part += advance();
            current_part += advance();
        } else if (peek() == '\r') {
            note_part_start();
            advance();
            if (peek() == '\n') advance();
            current_part += '\n';
            line_++; column_ = 1;
        } else {
            note_part_start();
            if (peek() == '\n') { line_++; column_ = 0; }
            current_part += advance();
        }
    }
    emit_literal();
    punct(token_type::right_paren, ")");
    result.push_back(token(token_type::eof, std::string_view(), current_location()));
    return result;
}

token lexer::scan_string() {
    size_t start = current_ - 1;  // Include opening quote
    size_t startColumn = column_ - 1;
    size_t startLine = line_;
    std::string value;

    while (!is_at_end() && peek() != '"') {
        if (peek() == '\n') {
            return error_token("unterminated string literal");
        }

        if (peek() == '\\') {
            advance();
            if (is_at_end()) {
                return error_token("unterminated string literal");
            }

            char escaped = advance();
            switch (escaped) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case '\'': value += '\''; break;
                case 'x': {
                    if (is_at_end()) { return error_token("unterminated string literal"); }
                    int hi = hex_digit_value(advance());
                    if (hi < 0 || is_at_end()) { return error_token("invalid \\x escape: expected two hex digits"); }
                    int lo = hex_digit_value(advance());
                    if (lo < 0) { return error_token("invalid \\x escape: expected two hex digits"); }
                    value += static_cast<char>(static_cast<unsigned char>(hi * 16 + lo));
                    break;
                }
                default:
                    return error_token("invalid escape sequence");
            }
        } else {
            value += advance();
        }
    }

    if (is_at_end()) {
        return error_token("unterminated string literal");
    }

    advance(); // Consume closing quote

    source_location loc = current_location();
    loc.line = startLine;
    loc.column = startColumn;

    // Use string_view into source_ for the raw lexeme (includes quotes)
    std::string_view lexeme_view(source_.data() + start, current_ - start);
    token tok(token_type::string_literal, lexeme_view, loc);
    tok.string_value = std::move(value);
    return tok;
}

// The text between R"delim( and )delim" as it stands, no escapes; a line ending inside is one '\n' (CRLF and a lone CR
// included), as in a template string
token lexer::scan_raw_string() {
    const size_t start = current_ - 1;
    const size_t startColumn = column_ - 1;
    const size_t startLine = line_;
    advance();   // the quote
    std::string delimiter;
    while (!is_at_end() && peek() != '(') {
        const char d = peek();
        if (d == ' ' || d == ')' || d == '\\' || d == '"' || d == '\t' || d == '\v' || d == '\f' || d == '\n' || d == '\r' || delimiter.size() == 16) {
            return error_token("invalid raw string delimiter: up to 16 characters before '(', none a space, parenthesis, backslash or quote");
        }
        delimiter += advance();
    }
    if (is_at_end()) { return error_token("unterminated raw string literal"); }
    advance();   // '('
    const std::string close = ")" + delimiter + "\"";
    const size_t end = source_.find(close, current_);
    if (end == std::string::npos) {
        while (!is_at_end()) {
            if (advance() == '\n') { line_++; column_ = 1; }
        }
        return error_token("unterminated raw string literal: no )" + delimiter + "\" closes it");
    }
    std::string value;
    value.reserve(end - current_);
    while (current_ < end) {
        const char ch = advance();
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && current_ < end && peek() == '\n') { advance(); }
            value += '\n';
            line_++;
            column_ = 1;
        } else {
            value += ch;
        }
    }
    for (size_t k = 0; k < close.size(); ++k) { advance(); }
    source_location loc = current_location();
    loc.line = startLine;
    loc.column = startColumn;
    token tok(token_type::string_literal, std::string_view(source_.data() + start, current_ - start), loc);
    tok.string_value = std::move(value);
    return tok;
}

token lexer::scan_char() {
    size_t start = current_ - 1;  // Include opening quote
    size_t startColumn = column_ - 1;

    if (is_at_end()) {
        return error_token("unterminated character literal");
    }

    char value;
    if (peek() == '\\') {
        advance();
        if (is_at_end()) {
            return error_token("unterminated character literal");
        }

        char escaped = advance();
        switch (escaped) {
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case '\\': value = '\\'; break;
            case '"': value = '"'; break;
            case '\'': value = '\''; break;
            case 'x': {
                if (is_at_end()) { return error_token("unterminated character literal"); }
                int hi = hex_digit_value(advance());
                if (hi < 0 || is_at_end()) { return error_token("invalid \\x escape: expected two hex digits"); }
                int lo = hex_digit_value(advance());
                if (lo < 0) { return error_token("invalid \\x escape: expected two hex digits"); }
                value = static_cast<char>(static_cast<unsigned char>(hi * 16 + lo));
                break;
            }
            default:
                return error_token("invalid escape sequence in character literal");
        }
    } else {
        value = advance();
    }

    if (is_at_end() || peek() != '\'') {
        if (!is_at_end() && peek() != '\n' && source_.find('\'', current_) < source_.find('\n', current_)) {
            return error_token("a character literal holds one character: write text in double quotes");
        }
        return error_token("unterminated character literal");
    }

    advance(); // Consume closing quote

    source_location loc = current_location();
    loc.column = startColumn;

    // Use string_view into source_ for the raw lexeme (includes quotes)
    std::string_view lexeme_view(source_.data() + start, current_ - start);
    token tok(token_type::char_literal, lexeme_view, loc);
    tok.char_value = value;
    return tok;
}

token lexer::scan_identifier() {
    size_t start = current_;
    size_t startColumn = column_;

    while (is_alpha_numeric(peek()) || peek() == '_') {
        advance();
    }

    // Create string_view into source for comparisons (no allocation)
    std::string_view lexeme_view(source_.data() + start, current_ - start);

    source_location loc = current_location();
    loc.column = startColumn;

    // Check for 'super::' special case BEFORE keyword lookup
    if (lexeme_view == "super" && peek() == ':' && peek_next() == ':') {
        advance(); // :
        advance(); // :
        return token(token_type::super_keyword, "super::", loc);
    }

    // math::vec2 / vec3 / vec4 / ivec2 / ivec3 / color spelled contiguously is ONE identifier token "math::K": the parser's
    // type-name rules then take it wherever a class name may stand (declarations, fields, parameters, returns,
    // array<...>, cast<...>), and expression position rewrites it to the static member math::K
    if (lexeme_view == "math" && peek() == ':' && peek_next() == ':') {
        size_t end = current_ + 2;
        while (end < source_.size() && (is_alpha_numeric(source_[end]) || source_[end] == '_')) {
            ++end;
        }
        const std::string_view kind(source_.data() + current_ + 2, end - (current_ + 2));
        if (kind == "vec2" || kind == "vec3" || kind == "vec4" || kind == "ivec2" || kind == "ivec3" || kind == "color") {
            while (current_ < end) {
                advance();
            }
            uint64_t fused_id = symbolizer_->intern(std::string_view(source_.data() + start, end - start));
            return token(token_type::identifier, symbolizer_->get_string(fused_id), fused_id, loc);
        }
    }

    // a keyword's lexeme views the map's own key
    auto it = keywords_.find(lexeme_view);
    if (it != keywords_.end()) {
        // For keywords, use string_view to the keyword string in the map (permanent storage)
        token tok(it->second, it->first, loc);

        // Handle boolean literals
        if (it->second == token_type::true_keyword) {
            tok.bool_value = true;
        } else if (it->second == token_type::false_keyword) {
            tok.bool_value = false;
        }

        return tok;
    }

    // For regular identifiers - symbolizer is always required
    // Intern the identifier so lexeme points to permanent symbolizer storage
    uint64_t sym_id = symbolizer_->intern(lexeme_view);
    std::string_view interned = symbolizer_->get_string(sym_id);
    return token(token_type::identifier, interned, sym_id, loc);
}

bool lexer::is_digit(char c) const {
    return c >= '0' && c <= '9';
}

bool lexer::is_alpha(char c) const {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

bool lexer::is_alpha_numeric(char c) const {
    return is_alpha(c) || is_digit(c);
}

source_location lexer::current_location() const {
    source_location loc;
    loc.filename = filename_;
    loc.line = line_;
    loc.column = column_;
    return loc;
}

bool lexer::is_reserved_word(std::string_view word) {
    return keywords_.find(word) != keywords_.end();
}

// token method implementations
std::string token::to_string() const {
    std::stringstream ss;
    ss << location.to_string() << " " << static_cast<int>(type) << " '" << lexeme << "'";
    return ss.str();
}

bool token::is_keyword() const {
    return (type >= token_type::bool_keyword && type <= token_type::extern_keyword) || type == token_type::inline_keyword;
}

} // namespace jai