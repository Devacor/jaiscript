// Regular expressions: regex, regex::match and regex::escape (detail/regex_library.hpp) over the linear-time engine
// (detail/regex.hpp), on both backends with proofs on and off; the load-time check and compile of a constant pattern; the
// engine against std::regex's ECMAScript matcher on generated patterns.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/detail/regex.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <cctype>
#include <chrono>
#include <functional>
#include <random>
#include <regex>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class regex_tests : public suite {
public:
    regex_tests() : suite("Regex") {}

    void forge_tests() override {
        auto run = [](const std::string& src, bool use_vm, bool proofs) {
            auto e = jai::foundry::make_default_engine();
            e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
            e->type_proofs(proofs);
            e->execution_budget(0);
            jai::stdlib::register_all(e);
            try { return e->execute(src).to_string(); }
            catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
        };
        auto expect = [this, run](const std::string& src, const std::string& expected, const std::string& what) {
            for (bool proofs : {true, false}) {
                for (bool use_vm : {false, true}) {
                    check_eq(expected, run(src, use_vm, proofs), what + (use_vm ? " (vm" : " (interpreter") + (proofs ? ")" : ", proofs off)"));
                }
            }
        };
        auto caught = [](const std::string& body) { return "var out = \"none\"; try { " + body + "; } catch (e) { out = e; } out"; };
        // the span of the first match, as "start-end", or "-" for none
        auto span = [](const std::string& pattern, const std::string& text, const std::string& flags = "") {
            return "var m = regex(" + pattern + ", \"" + flags + "\").search(" + text + "); m == null ? \"-\" : to_string(m.start()) + \"-\" + to_string(m.end())";
        };

        test("literals_classes_and_escapes", [expect, span]() {
            expect(span("\"cat\"", "\"concatenate\""), "3-6", "a literal");
            expect(span("\"a.c\"", "\"xxabcx\""), "2-5", "dot");
            expect(span("\"a.c\"", "\"a\\nc\""), "-", "dot skips a newline");
            expect(span("\"a.c\"", "\"a\\nc\"", "s"), "0-3", "the s flag");
            expect(span("\"[b-d]+\"", "\"aabcdde\""), "2-6", "a range class");
            expect(span("\"[^a-c]\"", "\"abcd\""), "3-4", "a negated class");
            expect(span("\"[]\"", "\"abc\""), "-", "the empty class matches nothing");
            expect(span("\"[^]\"", "\"\\n\""), "0-1", "[^] matches anything");
            expect(span("\"[a-]+\"", "\"x-a-\""), "1-4", "a trailing - is literal");
            expect(span("\"[\\\\d-z]+\"", "\"..1-z\""), "2-5", "a class escape before - leaves it literal");
            expect(span("R\"(\\d+)\"", "\"ab 123 c\""), "3-6", "\\d");
            expect(span("R\"(\\D+)\"", "\"12ab34\""), "2-4", "\\D");
            expect(span("R\"(\\w+)\"", "\"  hi_9 \""), "2-6", "\\w");
            expect(span("R\"(\\W)\"", "\"ab.c\""), "2-3", "\\W");
            expect(span("R\"(\\s+)\"", "\"a \\t\\n b\""), "1-5", "\\s");
            expect(span("R\"(\\S+)\"", "\"  xy \""), "2-4", "\\S");
            expect(span("R\"([\\d\\s]+)\"", "\"ab1 2c\""), "2-5", "class escapes in a class");
            expect(span("R\"(\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\^\\$\\\\)\"", "R\"(x.*+?()[]{}|^$\\)\""), "1-15", "escaped punctuation");
            expect(span("R\"(\\t\\n\\r\\f\\v\\0)\"", "\"\\t\\n\\r\\x0C\\x0B\\x00\""), "0-6", "control escapes");
            expect(span("R\"(\\x41\\u0042\\u{43})\"", "\"xABC\""), "1-4", "\\xNN, \\uXXXX and \\u{X}");
            expect(span("R\"([\\x41-\\x43]+)\"", "\"zABCD\""), "1-4", "\\xNN in a class");
            expect(span("\"a{2}\"", "\"aaa\""), "0-2", "an exact count");
            expect(span("\"a{2,}\"", "\"aaaa\""), "0-4", "a minimum count");
            expect(span("\"a{1,3}\"", "\"aaaa\""), "0-3", "a bounded count");
            expect(span("\"a{,3}\"", "\"a{,3}\""), "0-5", "{,n} is literal text");
            expect(span("\"x{y\"", "\"ax{y\""), "1-4", "a lone brace is literal");
            expect(span("\"a{0}b\"", "\"ab\""), "1-2", "a{0} matches nothing");
            expect(span("\"(?:ab)+\"", "\"xababab\""), "1-7", "a group repeats");
            expect(span("\"ab|cd|\"", "\"zcd\""), "0-0", "an empty alternative matches empty");
            expect(span("\"(?:x|)y\"", "\"y\""), "0-1", "an empty alternative in a group");
            expect(span("\"\"", "\"abc\""), "0-0", "the empty pattern");
        });

        test("anchors_and_boundaries", [expect, span]() {
            expect(span("\"^b\"", "\"ab\""), "-", "^ is the start of the text");
            expect(span("\"^b\"", "\"a\\nb\"", "m"), "2-3", "^ with m is a line start");
            expect(span("\"a$\"", "\"a\\nb\""), "-", "$ is the end of the text");
            expect(span("\"a$\"", "\"a\\nb\"", "m"), "0-1", "$ with m is a line end");
            expect(span("\"a$\"", "\"ba\\n\""), "-", "$ does not match before a final newline");
            expect(span("R\"(\\Aab\\z)\"", "\"ab\""), "0-2", "\\A and \\z");
            expect(span("R\"(\\Ab)\"", "\"a\\nb\"", "m"), "-", "\\A ignores m");
            expect(span("R\"(\\bfoo\\b)\"", "\"a foo b\""), "2-5", "\\b");
            expect(span("R\"(\\bfoo\\b)\"", "\"afoob\""), "-", "\\b needs a boundary");
            expect(span("R\"(\\Boo\\B)\"", "\"foob\""), "1-3", "\\B");
            expect(span("\"(?m)^x$\"", "\"a\\nx\\nb\""), "2-3", "inline (?m)");
        });

        test("quantifiers_greedy_and_lazy", [expect, span]() {
            expect(span("\"<.+>\"", "\"<a><b>\""), "0-6", "greedy");
            expect(span("\"<.+?>\"", "\"<a><b>\""), "0-3", "lazy");
            expect(span("\"a*?\"", "\"aaa\""), "0-0", "lazy star takes nothing");
            expect(span("\"a+?\"", "\"aaa\""), "0-1", "lazy plus takes one");
            expect(span("\"a??b\"", "\"ab\""), "0-2", "lazy optional still matches");
            expect(span("\"a{2,4}?\"", "\"aaaa\""), "0-2", "lazy bounded");
            expect(span("\"a{2,}?b\"", "\"aaaab\""), "0-5", "lazy minimum extends to match");
            expect(span("\"a|ab\"", "\"ab\""), "0-1", "leftmost-first: the first alternative wins");
            expect(span("\"ab|a\"", "\"ab\""), "0-2", "leftmost-first: the first alternative wins (longer)");
            expect(span("\"(a|ab)(c|bcd)\"", "\"abcd\""), "0-4", "priority across groups");
        });

        test("flags_and_inline_flags", [expect, span, caught]() {
            expect(span("\"HeLLo\"", "\"say hello\"", "i"), "4-9", "i folds ASCII");
            expect(span("\"[a-c]+\"", "\"xABCx\"", "i"), "1-4", "i folds a class");
            expect(span("\"[^a]\"", "\"A\"", "i"), "-", "i folds a negated class");
            expect(span("\"a(?i)b\"", "\"aB\""), "0-2", "(?i) to the end of the group");
            expect(span("\"(?i:a)b\"", "\"Ab\""), "0-2", "(?i:...)");
            expect(span("\"(?i:a)b\"", "\"AB\""), "-", "(?i:...) ends with its group");
            expect(span("\"(?i)a(?-i)b\"", "\"AB\""), "-", "(?-i)");
            expect(span("\"(a(?i)b)c\"", "\"aBC\""), "-", "an inline flag ends with its group");
            expect(span("\"a b # comment\\n c\"", "\"abc\"", "x"), "0-3", "x ignores space and comments");
            expect(span("\"a[ ]b\"", "\"a b\"", "x"), "0-3", "x keeps a class's space");
            expect(span("\"a\\\\ b\"", "\"a b\"", "x"), "0-3", "x keeps an escaped space");
            expect(span("\"(?s).\"", "\"\\n\""), "0-1", "inline (?s)");
            expect(span("\"(?x) a b \"", "\"ab\""), "0-2", "inline (?x)");
            expect("regex(\"a\", \"imsx\").flags", "imsx", "every flag");
            expect(caught("var f = \"g\"; regex(\"a\", f)"), "regex: unknown flag 'g' (the flags are i, m, s and x)", "an unknown flag");
            expect(caught("var f = \"ii\"; regex(\"a\", f)"), "regex: flag 'i' appears twice", "a repeated flag");
            expect(caught("var p = \"(?q)\"; regex(p)"), "regex: unknown group syntax '(?q' at position 0", "an unknown inline flag");
            expect(caught("var p = \"(?i-q:x)\"; regex(p)"), "regex: unknown flag 'q' in a group (the flags are i, m, s and x) at position 4", "an unknown flag in a group");
        });

        test("capture_groups_named_groups_and_positions", [expect, caught]() {
            const std::string date = "var m = regex(R\"((?<year>\\d{4})-(?<month>\\d\\d)(-(\\d\\d))?)\").search(\"on 2024-10 ok\"); ";
            expect(date + "m.text() + \" \" + m[0] + \" \" + m[1] + \" \" + m[\"month\"]", "2024-10 2024-10 2024 10", "texts by number and name");
            expect(date + "to_string(m.start()) + \" \" + to_string(m.end()) + \" \" + to_string(m.start(\"month\")) + \" \" + to_string(m.end(2))", "3 10 8 10", "positions");
            expect(date + "to_string(m[3] == null) + \" \" + to_string(m.start(4)) + \" \" + to_string(m.end(3))", "true -1 -1", "an unmatched group");
            expect(date + "m.size()", "5", "size counts the whole match and every group");
            expect(caught(date + "m[5]"), "regex::match has no group 5 (it has 4 groups)", "a group past the last");
            expect(caught(date + "m[\"day\"]"), "regex::match has no group named 'day'", "an unknown name");
            expect(caught(date + "m.start(1.5)"), "regex::match: start expects a group number or name, got float", "a bad group argument");
            expect("var m = regex(\"(a)|b\").search(\"b\"); to_string(m[1] == null) + m[0]", "trueb", "the other alternative's group stays unmatched");
            expect("var m = regex(\"(a)*\").search(\"aaa\"); m[1] + \" \" + to_string(m.start(1))", "a 2", "a repeated group keeps its last iteration");
            expect("var m = regex(\"(?:(a)|b)+\").search(\"ab\"); to_string(m[1])", "a", "a group set in an earlier iteration stays set");
            expect("var m = regex(\"x(y)?\").search(\"x\"); to_string(m.size()) + to_string(m[1] == null)", "2true", "an optional group");
            expect("regex::match m = regex(\"b+\").search(\"abbb\"); m.text()", "bbb", "a typed regex::match");
            expect("var m = regex(\"z\").search(\"abc\"); to_string(m == null)", "true", "no match is null");
            expect(caught("var p = \"(?<a>x)(?<a>y)\"; regex(p)"), "regex: duplicate group name 'a' at position 10", "a duplicate name");
            expect(caught("var p = \"(?<1a>x)\"; regex(p)"), "regex: invalid group name at position 3", "a name starting with a digit");
        });

        test("search_from_a_position_and_matches", [expect, caught]() {
            expect("var r = regex(\"a\"); to_string(r.search(\"banana\", 2).start()) + \" \" + to_string(r.search(\"banana\", 6) == null)", "3 true", "a start position");
            expect("to_string(regex(\"^a\").search(\"ba\", 1) == null) + \" \" + to_string(regex(R\"(\\ba)\").search(\"ba\", 1) == null)", "true true", "the text before the start stays the context");
            expect(caught("regex(\"a\").search(\"abc\", 4)"), "regex: search position 4 is outside the text (0 to 3)", "past the end");
            expect(caught("regex(\"a\").search(\"abc\", -1)"), "regex: search position -1 is outside the text (0 to 3)", "negative");
            expect("var r = regex(R\"(\\d{3}-\\d{4})\"); to_string(r.matches(\"555-1234\")) + to_string(r.matches(\"555-12345\")) + to_string(r.matches(\"x555-1234\"))", "truefalsefalse", "matches is the whole text");
            expect("to_string(regex(\"a|ab\").matches(\"ab\")) + to_string(regex(\"(a|ab)(c|bcd)(d*)\").matches(\"abcd\"))", "truetrue", "matches takes any way through");
            expect("to_string(regex(\"\").matches(\"\")) + to_string(regex(\"a*\").matches(\"\")) + to_string(regex(\"a+\").matches(\"\"))", "truetruefalse", "the empty text");
            expect(caught("regex(\"a\").matches(5)"), "regex: matches expects a string or utf8_string, got int", "a non-text subject");
        });

        test("find_all_advances_past_empty_matches_as_ecmascript_does", [expect]() {
            auto all = [](const std::string& pattern, const std::string& text) {
                return "string out = \"\"; for (var m : regex(" + pattern + ").find_all(" + text + ")) { out += \"[\" + m.text() + \"]\" + to_string(m.start()); } out";
            };
            expect(all("\"a*\"", "\"baaa\""), "[]0[aaa]1[]4", "an empty match, a match, and the empty match at the end");
            expect(all("\"x*\"", "\"abc\""), "[]0[]1[]2[]3", "an empty match at every position");
            expect(all(R"("\\d+")", "\"a1b22c333\""), "[1]1[22]3[333]6", "digit runs");
            expect(all("\"\"", "\"\""), "[]0", "the empty text has one empty match");
            expect(all("\"aa\"", "\"aaaaa\""), "[aa]0[aa]2", "matches do not overlap");
            expect(all("\"\\\\b\"", "\"ab cd\""), "[]0[]2[]3[]5", "boundaries");
            expect("regex(\"z\").find_all(\"abc\").size()", "0", "none");
            expect("var ms = regex(\"(.)(.)\").find_all(\"abcd\"); ms[1][2] + ms[0][1]", "da", "groups of each match");
        });

        test("replace_with_references_and_functions", [expect, caught]() {
            const std::string r = "var r = regex(R\"((?<first>\\w+) (?<last>\\w+))\"); ";
            expect(r + "r.replace(\"ada lovelace, alan turing\", \"$2 $1\")", "lovelace ada, turing alan", "numbered references");
            expect(r + "r.replace(\"ada lovelace\", \"${last}, ${first}\")", "lovelace, ada", "named references");
            expect(r + "r.replace(\"ada lovelace\", \"<$0> ${0} $$5 ${2}\")", "<ada lovelace> ada lovelace $5 lovelace", "$0, ${n} and $$");
            expect(r + "r.replace_first(\"a b c d\", \"[$1]\")", "[a] c d", "replace_first");
            expect("regex(\"x*\").replace(\"abc\", \"-\")", "-a-b-c-", "empty matches replace between every character");
            expect("regex(\"a\").replace(\"banana\", \"\")", "bnn", "removing");
            expect("regex(\"(a)(b)?\").replace(\"a\", \"[$2]\")", "[]", "an unmatched group is empty");
            expect("var r = regex(\"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)\"); r.replace(\"abcdefghijk\", \"$11|$10|$1x\")", "k|j|ax", "two-digit references");
            expect("regex(\"(a)\").replace(\"a\", \"$10\")", "a0", "$10 with one group is $1 then 0");
            expect(caught("regex(\"(a)\").replace(\"a\", \"$2\")"), "regex: the replacement names group 2 at position 0, but the pattern has 1 group", "a missing number");
            expect(caught("regex(\"(a)\").replace(\"a\", \"${x}\")"), "regex: the replacement names group 'x' at position 0, which the pattern does not have", "a missing name");
            expect(caught("regex(\"a\").replace(\"a\", \"5$\")"), "regex: a '$' at position 1 of the replacement starts no group reference (write $$ for a dollar sign)", "a stray $");
            expect(caught("regex(\"a\").replace(\"a\", \"${\")"), "regex: '${' at position 0 of the replacement needs a group number or name and '}'", "an open ${");
            expect("regex(R\"(\\d+)\").replace(\"a1b22\", (m) => \"<\" + to_string(m.text().size()) + \">\")", "a<1>b<2>", "a function replacement");
            expect("int n = 0; regex(\"o\").replace(\"foo boo\", (m) => { n += 1; return to_string(n); })", "f12 b34", "the function runs once per match, in order");
            expect("regex(\"(.)\").replace_first(\"ab\", (m) => { var c = m[1]; c.to_upper(); return c; })", "Ab", "a function with replace_first");
            expect("string s = \"a1b2\"; regex r = regex(R\"(\\d)\"); string out = r.replace(s, (m) => { s = \"zzzz\"; r = regex(\"q\"); return \"#\"; }); out + \"|\" + s + \"|\" + r.pattern",
                   "a#b#|zzzz|q", "the function may write the text and the regex the call read");
            expect("var keep = []; regex(\"(.)\").replace(\"xy\", (m) => { keep.push(m); return \"\"; }); keep[1][1] + to_string(keep[0].start())", "y0", "a match the function keeps stays whole");
            expect(caught("regex(\"a\").replace(\"a\", (m) => 5)"), "regex: the replace function must return a string, got int", "a non-text result");
            expect(caught("regex(\"a\").replace(\"a\", (m) => { throw \"boom\"; })"), "boom", "a throw in the function leaves the call");
            expect(caught("regex(\"a\").replace(\"a\", 5)"), "regex: replace expects a string or a function as the replacement, got int", "a bad replacement");
        });

        test("split_follows_ecmascript", [expect]() {
            auto split = [](const std::string& pattern, const std::string& text, const std::string& more = "") {
                return "to_json(regex(" + pattern + ").split(" + text + more + "))";
            };
            expect(split(R"("\\s*,\\s*")", "\"a , b,c ,d\""), "[\"a\",\"b\",\"c\",\"d\"]", "a separator pattern");
            expect(split("\"x*\"", "\"abc\""), "[\"a\",\"b\",\"c\"]", "empty matches split between characters");
            expect(split("\"(,)\"", "\"a,b\""), "[\"a\",\",\",\"b\"]", "a group's text joins the pieces");
            expect(split("\"(,)|(;)\"", "\"a,b;c\""), "[\"a\",\",\",\"\",\"b\",\"\",\";\",\"c\"]", "an unmatched group joins as empty");
            expect(split("\",\"", "\",a,,b,\""), "[\"\",\"a\",\"\",\"b\",\"\"]", "empty pieces stay");
            expect(split("\",\"", "\"a,b,c,d\"", ", 2"), "[\"a\",\"b\",\"c,d\"]", "a split count keeps the rest");
            expect(split("\",\"", "\"a,b\"", ", 0"), "[\"a,b\"]", "zero splits");
            expect(split("\",\"", "\"a,b\"", ", -1"), "[\"a\",\"b\"]", "a negative count is no limit");
            expect(split("\"x\"", "\"\""), "[\"\"]", "the empty text without a match");
            expect(split("\"x*\"", "\"\""), "[]", "the empty text the pattern matches");
            expect(split("\"$\"", "\"ab\""), "[\"ab\"]", "a match at the end splits nothing");
            expect("var p = regex(\",\").split(\"a,b\"); array<string> s = p; s.size()", "2", "a string's pieces are an array<string>");
        });

        test("utf8_strings_match_by_character", [expect, caught]() {
            expect("utf8_string u = \"Zo\xC3\xAB \xF0\x9F\x99\x82!\"; var m = regex(\"\xC3\xAB.\").search(u); m.text() + \" \" + to_string(m.start()) + \" \" + to_string(m.end())", "\xC3\xAB  2 4", "positions are characters");
            expect("utf8_string u = \"\xF0\x9F\x99\x82\xF0\x9F\x99\x82x\"; regex(\"^..$\").matches(u.substr(0, 2))", "true", "dot is one character");
            expect("string s = \"\xC3\xA9\"; regex(\"^..$\").matches(s)", "true", "on a string dot is one byte");
            expect("utf8_string u = \"caf\xC3\xA9s\"; regex(\"[\xC3\xA0-\xC3\xBF]\").search(u).start()", "3", "a class of characters");
            expect("utf8_string u = \"\xC3\xA9x\"; regex(\"[^x]\").search(u).text()", "\xC3\xA9", "a negated class takes a whole character");
            expect("utf8_string u = \"\xC3\xA9\"; regex(\"x*\").find_all(u).size()", "2", "an empty match advances a whole character");
            expect("utf8_string u = \"a\xC3\xA9" "b\"; to_json(regex(\"\").split(u))", "[\"a\",\"\xC3\xA9\",\"b\"]", "splitting into characters");
            expect("utf8_string u = \"\xC3\xA9\xC3\xA9\"; type_of(regex(\"\xC3\xA9\").replace(u, \"e\"))", "utf8_string", "a utf8_string's result is a utf8_string");
            expect("utf8_string u = \"\xC3\xA9-\"; var m = regex(\"(.)-\").search(u); type_of(m[1]) + \" \" + type_of(m.text())", "utf8_string utf8_string", "a utf8_string's group texts");
            expect("utf8_string u = \"\xC3\xA9\"; regex(R\"(\\xE9)\").search(u).start()", "0", "\\xNN is U+00NN on a utf8_string");
            expect("string s = \"\xE9\"; regex(R\"(\\xE9)\").search(s).start()", "0", "\\xNN is the byte on a string");
            expect("utf8_string u = \"\xC3\xA9\"; string out = \"\"; for (var m : regex(R\"(\\B)\").find_all(u)) { out += to_string(m.start()); } out", "01", "\\B is never inside a character");
            expect("string s = \"\xC3\xA9\"; regex(R\"(\\B)\").find_all(s).size()", "3", "on a string every byte position is a position");
            expect("utf8_string u = \"a\xFF" "b\"; to_string(regex(\"a.b\").search(u) == null) + to_string(regex(\"b\").search(u).start())", "true2", "an invalid byte matches nothing, and counts as a character");
            expect("utf8_string u = \"\xC3\x89T\xC3\xA9\"; to_string(regex(\"\xC3\xA9\", \"i\").search(u).start())", "2", "case folding stays ASCII");
            expect(caught("regex(\"[\xC3\xA9]\").search(\"caf\xC3\xA9\")"), "regex: the class at position 0 holds '\xC3\xA9', a character of more than one byte, but a string matches by bytes (match a utf8_string, or write its bytes as \\xNN)", "a string's class holds bytes");
            expect("string s = \"caf\xC3\xA9\"; regex(\"\xC3\xA9\").search(s).start()", "3", "a multi-byte literal matches a string's bytes");
            expect(caught("var p = \"\\xFF\"; regex(p)"), "regex: the pattern is not valid UTF-8 at byte 0 (write a byte as \\xNN)", "a pattern is UTF-8 text");
        });

        test("unsupported_features_name_themselves", [expect, caught]() {
            const std::string tail = " is not supported: matching runs in linear time, without backtracking";
            expect(caught("var p = \"(a)\\\\1\"; regex(p)"), "regex: the backreference '\\1' at position 3" + tail, "a backreference");
            expect(caught("var p = \"(?<n>a)\\\\k<n>\"; regex(p)"), "regex: the backreference '\\k<n>' at position 7" + tail, "a named backreference");
            expect(caught("var p = \"a(?=b)\"; regex(p)"), "regex: the lookahead '(?=' at position 1" + tail, "a lookahead");
            expect(caught("var p = \"a(?!b)\"; regex(p)"), "regex: the negative lookahead '(?!' at position 1" + tail, "a negative lookahead");
            expect(caught("var p = \"(?<=a)b\"; regex(p)"), "regex: the lookbehind '(?<=' at position 0" + tail, "a lookbehind");
            expect(caught("var p = \"(?<!a)b\"; regex(p)"), "regex: the negative lookbehind '(?<!' at position 0" + tail, "a negative lookbehind");
            expect(caught("var p = \"(?>a+)b\"; regex(p)"), "regex: the atomic group '(?>' at position 0" + tail, "an atomic group");
            expect(caught("var p = \"a*+b\"; regex(p)"), "regex: the possessive quantifier '*+' at position 1" + tail, "a possessive quantifier");
            expect(caught("var p = \"a(?R)?b\"; regex(p)"), "regex: the recursion '(?R)' at position 1" + tail, "recursion");
            expect(caught("var p = \"\\\\p{L}\"; regex(p)"), "regex: Unicode property class '\\p' is not supported at position 0", "a Unicode property");
        });

        test("pattern_errors_name_their_position", [expect, caught]() {
            auto err = [caught](const std::string& pattern) { return caught("var p = \"" + pattern + "\"; regex(p)"); };
            expect(err("(ab"), "regex: missing ')' to close the group at position 0", "an open group");
            expect(err("ab)"), "regex: unmatched ')' at position 2", "a stray )");
            expect(err("[ab"), "regex: missing ']' to close the class at position 0", "an open class");
            expect(err("*a"), "regex: nothing to repeat before '*' at position 0", "a leading quantifier");
            expect(err("a**"), "regex: '*' follows another quantifier at position 2", "two quantifiers");
            expect(err("^*"), "regex: nothing to repeat before '*' (an anchor matches no characters) at position 1", "a repeated anchor");
            expect(err("[z-a]"), "regex: range 'z-a' is out of order at position 1", "a backwards range");
            expect(err("a{3,2}"), "regex: repetition '{3,2}' has its minimum above its maximum at position 1", "min over max");
            expect(err("a{1001}"), "regex: repetition '{1001}' counts past 1000 at position 1", "a count past the limit");
            expect(err("\\\\q"), "regex: unknown escape '\\q' at position 0", "an unknown escape");
            expect(err("[\\\\q]"), "regex: unknown escape '\\q' in a class at position 1", "an unknown escape in a class");
            expect(err("a\\\\"), "regex: trailing '\\' at position 1", "a trailing backslash");
            expect(err("\\\\x4"), "regex: invalid \\x escape: expected two hex digits at position 0", "a short \\x");
            expect(err("\\\\u{110000}"), "regex: \\u escape '\\u{110000}' names no character (a surrogate, or past U+10FFFF) at position 0", "past U+10FFFF");
            expect(err("\\\\01"), "regex: octal escape '\\01' is not supported (write \\xNN) at position 0", "an octal escape");
            expect(caught("regex(5)"), "regex: the pattern must be a string, got int", "a non-text pattern");
            expect(caught("regex(\"a\", \"i\", \"x\")"), "regex expects a pattern and optional flags, got 3 arguments", "too many arguments");
            expect("regex r; to_string(r.pattern == \"\") + to_string(r.matches(\"\"))", "truetrue", "a default regex is the empty pattern");
        });

        test("a_constant_pattern_is_checked_and_compiled_at_load", [this, run]() {
            for (bool use_vm : {false, true}) {
                const std::string text = run("print(\"ran\"); var r = regex(R\"(a(b)\");", use_vm, true);
                check_true(text.find("static check failed") != std::string::npos && text.find("regex: missing ')' to close the group at position 1") != std::string::npos &&
                           text.find(":1:29:") != std::string::npos, text);
                const std::string viaconst = run("const string P = \"[a\"; var r = regex(P);", use_vm, true);
                check_true(viaconst.find("regex: missing ']' to close the class at position 0") != std::string::npos, viaconst);
                const std::string flags = run("var r = regex(\"a\", \"q\");", use_vm, true);
                check_true(flags.find("regex: unknown flag 'q'") != std::string::npos, flags);
            }
            auto e = jai::foundry::make_default_engine();
            e->set_backend(jai::backend_type::vm);
            jai::stdlib::register_all(e);
            std::string listing;
            e->disassembly_sink([&listing](const std::string& l, const std::string&) { listing += l; });
            e->execute("int n = 0; for (int i = 0; i < 3; ++i) { if (regex(\"a\" + \"b\", \"i\").matches(\"AB\")) { ++n; } } string p = \"x\"; regex(p);");
            check_true(listing.find("REGEX_CONSTANT site=") != std::string::npos, listing);
            check_eq(size_t(1), [&] { size_t k = 0, at = 0; while ((at = listing.find("REGEX_CONSTANT site=", at)) != std::string::npos) { ++k; ++at; } return k; }(), "only the literal call folds");
        });

        test("a_constant_pattern_follows_the_name_it_calls", [expect]() {
            expect("var regex = (p) => \"shadowed \" + p; regex(\"a\")", "shadowed a", "a local named regex");
            expect("function f(var regex) { return regex(\"a\"); } f((p) => p + \"!\")", "a!", "a parameter named regex");
            expect("int n = 0; for (int i = 0; i < 4; ++i) { if (regex(\"b\").matches(\"b\")) { ++n; } } n", "4", "a loop over a constant pattern");
            expect("function make() { return regex(\"(\\\\d)\"); } var a = make(); var b = make(); a.search(\"x5\")[1] + b.search(\"7\")[1]", "57", "each evaluation is its own value");
        });

        test("pathological_patterns_run_in_linear_time", [this, expect]() {
            const std::string as = "string s = string::repeat(\"a\", 100000); ";
            expect(as + "to_string(regex(\"(a*)*b\").search(s) == null)", "true", "nested stars");
            expect(as + "to_string(regex(\"(a|aa)+b\").search(s) == null)", "true", "overlapping alternatives");
            expect(as + "to_string(regex(\"(x+x+)+y|a{0,1000}c\").search(s) == null)", "true", "a bounded repeat");
            expect(as + "to_string(regex(\"^(a+)+$\").matches(s))", "true", "the classic ReDoS shape matches");
            expect(as + "to_string(regex(\"^(a+)+$\").matches(s + \"!\"))", "false", "and fails fast");
            expect(as + "regex(\"(a|b)*?c|(a+)\").search(s)[2].size()", "100000", "groups over the whole text");
        });

        test("long_texts_and_deep_patterns_use_no_native_recursion", [expect]() {
            const std::string big = "string s = string::repeat(\"abcdefgh\", 500000) + \"XY\"; ";   // four megabytes
            expect(big + "regex(\"X(Y)\").search(s).start(1)", "4000001", "a search across megabytes");
            expect(big + "regex(\"(?:abcdefgh)+X\").search(s).end()", "4000001", "a match spanning megabytes");
            expect("string s = string::repeat(\"abcdefgh\", 50000) + \"XY\"; regex(\"(a)(?:bcdefgh(a))*bcdefghX\").search(s).start(2)", "399992", "groups over a match of 400 KB");
            expect(big + "to_string(regex(\"(abcdefgh)*XY\").matches(s))", "true", "a group repeated half a million times");
            expect("string s = string::repeat(\"ab,\", 100000); regex(\",\").split(s).size()", "100001", "split into a hundred thousand pieces");
            expect("string s = string::repeat(\"ab,\", 100000); regex(\"b\").replace(s, \"\").size()", "200000", "replace a hundred thousand matches");
            const std::string deep = "string p = string::repeat(\"(\", 50000) + \"a\" + string::repeat(\")\", 50000); ";
            expect(deep + "regex(p).search(\"xa\").start()", "1", "fifty thousand nested groups");
            const std::string alts = "string p = string::repeat(\"(?:b|\", 50000) + \"a\" + string::repeat(\")\", 50000); ";
            expect(alts + "regex(p).search(\"xa\").start()", "1", "fifty thousand nested alternations");
        });

        test("programs_past_the_size_limit_are_refused", [expect, caught]() {
            expect(caught("var p = \"(?:(?:a{1000}){1000}){1000}\"; regex(p)"), "regex: the pattern is too large to match a utf8_string (its program passes 250000 instructions)", "repetition blows up");
        });

        test("escape_and_round_trips", [expect, caught]() {
            expect("regex::escape(\"1+1=2? (yes) [a-z] {x} | ^$ \\\\ . * # -\")", R"(1\+1=2\?\ \(yes\)\ \[a\-z\]\ \{x\}\ \|\ \^\$\ \\\ \.\ \*\ \#\ \-)", "every metacharacter");
            expect("string t = \"a.b*(c)[d]{e}|f^g$h\\\\i?j+k#l m-n\"; to_string(regex(\"^\" + regex::escape(t) + \"$\").matches(t))", "true", "an escaped text matches itself");
            expect("string t = \"a.b\"; to_string(regex(regex::escape(t), \"x\").matches(\"a.b\"))", "true", "escape survives x");
            expect("utf8_string u = \"\xC3\xA9.\"; type_of(regex::escape(u))", "utf8_string", "escape keeps the kind");
            expect("to_json(regex(R\"(\\d+)\", \"i\"))", "{\"$type\":\"regex\",\"$val\":{\"pattern\":\"\\\\d+\",\"flags\":\"i\"}}", "to_json writes the pattern and flags");
            expect("var r = from_json(to_json(regex(\"(\\\\w)x\", \"i\"))); type_of(r) + \" \" + r.pattern + \" \" + r.flags + \" \" + r.search(\"aX\")[1]", "regex (\\w)x i a", "from_json rebuilds it");
            expect("regex r = from_binary(to_binary(regex(\"b+\"))); r.search(\"abb\").text()", "bb", "the binary archive too");
            expect(caught("var r = from_json(\"{\\\"$type\\\":\\\"regex\\\",\\\"$val\\\":{\\\"pattern\\\":\\\"(\\\",\\\"flags\\\":\\\"\\\"}}\")"), "regex: missing ')' to close the group at position 0", "a saved pattern that does not compile");
            expect(caught("regex r = regex(\"a\"); r.pattern = \"b\""), "Cannot assign to 'pattern': it is read-only", "the pattern is read-only");
            expect("regex a = regex(\"x\"); regex b = a; b.pattern + to_string(b.matches(\"x\"))", "xtrue", "a regex copies");
        });

        test("parallel_bodies_make_their_own_regex", [expect]() {
            const std::string lines = "array<string> lines = []; for (int i = 0; i < 400; ++i) { lines.push(\"id-\" + to_string(i)); } ";
            expect(lines + "parallel_for (auto& s : lines) { s = regex(R\"(\\d+)\").replace(s, \"<$0>\"); } lines[123]", "id-<123>", "a constant pattern in the body");
            expect(lines + "array<int> n = []; for (auto s : lines) { n.push(0); } parallel_for (auto& s, auto& v : lines, n) { var m = regex(R\"(id-(\\d+))\").search(s); v = m[1].to_int(); } int t = 0; for (int v : n) { t += v; } t", "79800", "match groups in the body");
            expect(lines + "parallel_for (auto& s : lines) { string p = \"-\"; s = to_string(regex(p).split(s).size()); } lines[7]", "2", "a dynamic pattern in the body");
            expect(lines + "regex r = regex(R\"(i(d)-(\\d+))\"); parallel_for (auto& s : lines) { var m = r.search(s); s = r.replace(s, \"$1\") + m[2]; } lines[123]",
                   "d123", "a captured regex runs on every worker");
            expect(lines + "var out = \"\"; try { parallel_for (auto& s : lines) { s = regex(\"i\").replace(s, (m) => \"I\"); } } catch (e) { out = e; } out.starts_with(\"parallel_for: lambdas are not allowed\")",
                   "true", "a function replacement is refused as every lambda is");
        });

        test("the_engine_agrees_with_ecmascript_on_generated_patterns", [this]() {
            // std::regex runs ECMAScript's backtracking leftmost-first matcher: on patterns whose groups sit outside every
            // loop the two agree on the match and on every group
            std::mt19937 rng(20261021);
            auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
            struct piece { std::string text; bool nullable; };
            std::function<piece(int, bool)> gen = [&](int depth, bool groups_ok) -> piece {
                const int k = depth <= 0 ? pick(4) : pick(12);
                auto solid = [&](int d, bool g) { piece p = gen(d, g); while (p.nullable) { p = gen(d, g); } return p; };
                switch (k) {
                    case 0: return {std::string(1, "ab"[pick(2)]), false};
                    case 1: return {pick(2) ? "[ab]" : "[^a]", false};
                    case 2: return {".", false};
                    case 3: return pick(3) == 0 ? piece{"\\b", true} : piece{std::string(1, "abc"[pick(3)]), false};
                    case 4: { piece x = gen(depth - 1, groups_ok), y = gen(depth - 1, groups_ok); return {x.text + y.text, x.nullable && y.nullable}; }
                    case 5: { piece x = gen(depth - 1, groups_ok), y = gen(depth - 1, groups_ok); return {x.text + "|" + y.text, x.nullable || y.nullable}; }
                    case 6: return {"(?:" + solid(depth - 1, false).text + ")*" + (pick(2) ? "?" : ""), true};
                    case 7: return {"(?:" + solid(depth - 1, false).text + ")+" + (pick(2) ? "?" : ""), false};
                    case 8: return {"(?:" + solid(depth - 1, groups_ok).text + ")?" + (pick(2) ? "?" : ""), true};
                    case 9: { const int lo = pick(3); return {"(?:" + solid(depth - 1, false).text + "){" + std::to_string(lo) + "," + std::to_string(2 + pick(2)) + "}", lo == 0}; }
                    case 10: { if (!groups_ok) { return gen(depth - 1, false); } piece x = gen(depth - 1, true); return {"(" + x.text + ")", x.nullable}; }
                    default: { piece x = gen(depth - 1, groups_ok), y = gen(depth - 1, groups_ok), z = gen(depth - 1, groups_ok); return {x.text + y.text + z.text, x.nullable && y.nullable && z.nullable}; }
                }
            };
            auto ecmascript_search = [](const std::string& pattern, const std::regex& theirs, const std::string& text) {
                std::smatch m;
                const bool want = std::regex_search(text, m, theirs);
                std::string result = want ? "match" : "none";
                if (want) { for (size_t g = 0; g < m.size(); ++g) { result += !m[g].matched ? " -" : " " + std::to_string(m.position(g)) + "," + std::to_string(m.position(g) + m.length(g)); } }
#ifdef _LIBCPP_VERSION
                // libc++ reads \b as false in the attempt that starts at the end of a non-empty text (it ignores match_prev_avail
                // there), so that attempt is rerun on an empty text with each \b fixed to its value at the end
                const bool end_attempt_decides = !want || static_cast<size_t>(m.position(0)) == text.size();
                if (end_attempt_decides && !text.empty() && pattern[0] != '^' && pattern.find("\\b") != std::string::npos) {
                    const bool word_before = std::isalnum(static_cast<unsigned char>(text.back())) || text.back() == '_';
                    std::string fixed;
                    for (size_t i = 0; i < pattern.size(); ++i) {
                        if (pattern.compare(i, 2, "\\b") == 0) {
                            fixed += word_before ? "(?:)" : "(?!)";
                            ++i;
                        } else {
                            fixed += pattern[i];
                        }
                    }
                    const std::string empty;
                    std::smatch at_end;
                    result = std::regex_search(empty, at_end, std::regex(fixed, std::regex::ECMAScript)) ? "match" : "none";
                    const std::string end = std::to_string(text.size());
                    if (result == "match") { for (size_t g = 0; g < at_end.size(); ++g) { result += at_end[g].matched ? " " + end + "," + end : " -"; } }
                }
#endif
                return result;
            };
            size_t compared = 0, mismatches = 0;
            std::string first;
            for (int trial = 0; trial < 3000; ++trial) {
                std::string pattern = gen(3, true).text;
                if (pick(5) == 0) { pattern = "^" + pattern; }
                if (pick(5) == 0) { pattern += "$"; }
                std::string error;
                auto ours = jai::detail::regex::compile(pattern, "", error);
                if (!ours) { continue; }
                std::regex theirs;
                try { theirs = std::regex(pattern, std::regex::ECMAScript); } catch (...) { continue; }
                for (int t = 0; t < 4; ++t) {
                    std::string text;
                    const int len = pick(9);
                    for (int i = 0; i < len; ++i) { text.push_back("ab c"[pick(4)]); }
                    std::vector<size_t> slots;
                    jai::detail::regex::searcher searcher(*ours, jai::detail::regex::subject::bytes);
                    const bool found = searcher.find(text, 0, true, slots);
                    std::string a = found ? "match" : "none", b = ecmascript_search(pattern, theirs, text);
                    if (found) { for (size_t g = 0; g * 2 < slots.size(); ++g) { a += slots[2 * g] == jai::detail::regex::npos ? " -" : " " + std::to_string(slots[2 * g]) + "," + std::to_string(slots[2 * g + 1]); } }
                    ++compared;
                    if (a != b && mismatches++ == 0) { first = "/" + pattern + "/ on '" + text + "': ours " + a + ", ECMAScript " + b; }
                    const bool full = jai::detail::regex::searcher(*ours, jai::detail::regex::subject::bytes).full(text);
                    if (full != std::regex_match(text, theirs) && mismatches++ == 0) { first = "/" + pattern + "/ full match on '" + text + "' disagrees"; }
                }
            }
            check_gt(compared, size_t(5000), "comparisons");
            check_eq(size_t(0), mismatches, first);
        });
    }
};

} // namespace jai::foundry::tests

using regex_tests = jai::foundry::tests::regex_tests;
FOUNDRY_REGISTER(regex_tests)
