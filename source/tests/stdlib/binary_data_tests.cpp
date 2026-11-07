// Binary data: string's to_bytes() / string::from_bytes, binary::reader, binary::writer, the width tokens and binary::layout
// (detail/binary_data.hpp), on both backends with proofs on and off; the checker's load-time knowledge of the library.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <string>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class binary_data_tests : public suite {
public:
    binary_data_tests() : suite("Binary Data") {}

    void forge_tests() override {
        auto run = [](const std::string& src, bool use_vm, bool proofs) {
            auto e = jai::foundry::make_default_engine();
            e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
            e->type_proofs(proofs);
            e->execution_budget(0);
            jai::stdlib::register_all(e);
            e->add_function("host_echo", [](const std::string& s) -> std::string { return s; });
            try { return e->execute(src).to_string(); }
            catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
        };
        auto expect = [this, run](const std::string& src, const std::string& expected, const char* what) {
            for (bool proofs : {true, false}) {
                for (bool use_vm : {false, true}) {
                    check_eq(expected, run(src, use_vm, proofs), std::string(what) + (use_vm ? " (vm" : " (interpreter") + (proofs ? ")" : ", proofs off)"));
                }
            }
        };
        auto caught = [](const std::string& body) { return "var out = \"none\"; try { " + body + "; } catch (e) { out = e; } out"; };
        const std::string all256 = "array<int> ALL = []; for (int i = 0; i < 256; ++i) { ALL.push(i); } ";

        test("to_bytes_reads_every_byte_as_an_int", [expect, caught]() {
            expect("to_json(\"\\x01\\x02\\xFF\\x00AB\".to_bytes())", "[1,2,255,0,65,66]", "the whole string");
            expect("to_json(\"abcdef\".to_bytes(2))", "[99,100,101,102]", "from an offset");
            expect("to_json(\"abcdef\".to_bytes(1, 3))", "[98,99,100]", "an offset and a count");
            expect("to_json(\"abc\".to_bytes(3))", "[]", "the offset at the end");
            expect("to_json(\"abc\".to_bytes(1, 0))", "[]", "a zero count");
            expect("to_json(\"\".to_bytes())", "[]", "an empty string");
            expect("to_json(\"a\\x00b\".to_bytes())", "[97,0,98]", "an embedded NUL");
            expect("array<uint8> b = \"xyz\".to_bytes(); b.push(7); b.size()", "4", "the result is an array<uint8>");
            expect(caught("auto b = \"xyz\".to_bytes(); int v = 256; b.push(v)"), "Cannot store 256 in array<uint8> (0..255)", "the result keeps the byte range");
            expect("array<int> b = \"xyz\".to_bytes(); b.push(1000); to_json(b)", "[120,121,122,1000]", "an array<int> takes the bytes by value");
            expect("var r = \"\x01\x02\".reader(); array<uint8> b = r.bytes(2); int v = 256; string e = \"\"; try { b.push(v); } catch (x) { e = x; } to_json(b) + \" \" + e", "[1,2] Cannot store 256 in array<uint8> (0..255)", "r.bytes(n) is an array<uint8>");
            expect(caught("\"abc\".to_bytes(4)"), "to_bytes(4) out of bounds for string of size 3", "an offset past the end");
            expect(caught("\"abc\".to_bytes(1, 3)"), "to_bytes(1, 3) out of bounds for string of size 3", "a count past the end");
            expect(caught("\"abc\".to_bytes(-1)"), "to_bytes(-1) out of bounds for string of size 3", "a negative offset");
            expect(caught("\"abc\".to_bytes(0, -1)"), "to_bytes(0, -1) out of bounds for string of size 3", "a negative count");
            expect(caught("\"abc\".to_bytes(\"x\")"), "to_bytes() arguments must be integers", "a non-int argument");
            expect(caught("\"abc\".to_bytes(0, 1, 2)"), "to_bytes() takes 0 to 2 arguments ([offset], [count])", "three arguments");
        });

        test("to_bytes_on_a_utf8_string_gives_its_utf8_bytes", [expect]() {
            expect("utf8_string u = \"h\\xC3\\xA9\"; to_json(u.to_bytes())", "[104,195,169]", "two-byte character");
            expect("utf8_string u = \"h\\xC3\\xA9\"; u.to_bytes(1).size()", "2", "offsets count bytes");
        });

        test("from_bytes_builds_a_string_from_ints_or_chars", [expect, caught, all256]() {
            expect("string::from_bytes([72, 105])", "Hi", "an untyped array");
            expect("array<int> b = [0, 65, 0]; var s = string::from_bytes(b); to_string(s.size()) + \" \" + to_json(s.to_bytes())", "3 [0,65,0]", "NULs survive");
            expect("array<char> c = ['o', 'k']; string::from_bytes(c)", "ok", "an array<char>");
            expect("array<uint8> u = [104, 0, 255]; var s = string::from_bytes(u); to_string(s.size()) + \" \" + to_json(s.to_bytes())", "3 [104,0,255]", "an array<uint8>");
            expect("array<var> v = [104, 105]; string::from_bytes(v)", "hi", "an array<var>");
            expect(caught("array<int8> s = [104, -1]; string::from_bytes(s)"), "string::from_bytes expects bytes (0 to 255): element 1 is -1", "an array<int8> element below 0");
            expect("string::from_bytes([]).size()", "0", "an empty array");
            expect(all256 + "var s = string::from_bytes(ALL); to_string(s.size()) + \" \" + to_string(s.to_bytes() == ALL)", "256 true", "all 256 values round trip");
            expect(all256 + "var s = string::from_bytes(ALL); to_string(host_echo(s) == s) + \" \" + to_string(host_echo(s).size())", "true 256", "the host boundary keeps every byte");
            expect(caught("string::from_bytes([1, 256])"), "string::from_bytes expects bytes (0 to 255): element 1 is 256", "a value above a byte");
            expect(caught("array<int> b = [5, -1]; string::from_bytes(b)"), "string::from_bytes expects bytes (0 to 255): element 1 is -1", "a negative value");
            expect(caught("string::from_bytes([1, \"x\"])"), "string::from_bytes expects bytes (0 to 255): element 1 is string", "a string element");
            expect(caught("string::from_bytes(5)"), "string::from_bytes expects an array of bytes, got int", "not an array");
        });

        test("reader_reads_each_width_little_endian", [expect]() {
            const std::string data = "var r = \"\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08\".reader(); ";
            expect(data + "to_string(r.uint8()) + \" \" + to_string(r.uint8())", "1 2", "uint8");
            expect(data + "to_string(r.uint16()) + \" \" + to_string(r.pos())", "513 2", "uint16");
            expect(data + "to_string(r.uint32())", "67305985", "uint32");
            expect(data + "to_string(r.int64())", "578437695752307201", "int64");
            expect(data + "r.skip(2); to_string(r.int16()) + \" \" + to_string(r.remaining())", "1027 4", "skip then int16");
        });

        test("reader_sign_extends_by_width", [expect]() {
            expect("to_string(\"\\xFF\".reader().int8())", "-1", "int8");
            expect("to_string(\"\\xFF\".reader().uint8())", "255", "uint8");
            expect("to_string(\"\\x00\\x80\".reader().int16())", "-32768", "int16");
            expect("to_string(\"\\x00\\x80\".reader().uint16())", "32768", "uint16");
            expect("to_string(\"\\x00\\x00\\x00\\x80\".reader().int32())", "-2147483648", "int32");
            expect("to_string(\"\\xFF\\xFF\\xFF\\xFF\".reader().uint32())", "4294967295", "uint32");
            expect("to_string(\"\\xFF\\xFF\\xFF\\xFF\\xFF\\xFF\\xFF\\xFF\".reader().int64())", "-1", "int64");
        });

        test("big_endian_switches_the_byte_order", [expect]() {
            expect("var r = \"\\x01\\x02\\x01\\x02\".reader().big_endian(); to_string(r.uint16()) + \" \" + to_string(r.little_endian().uint16())", "258 513", "reader");
            expect("binary::writer w; w.big_endian().uint32(0x01020304); to_json(w.to_string().to_bytes())", "[1,2,3,4]", "writer");
            expect("binary::writer w; w.big_endian().int16(-2).little_endian().int16(-2); to_json(w.to_string().to_bytes())", "[255,254,254,255]", "writer switches back");
        });

        test("floats_round_trip_through_their_bits", [expect]() {
            expect("binary::writer w; w.float32(1.5).float64(-2.25); var r = w.to_string().reader(); to_string(r.float32()) + \" \" + to_string(r.float64())", "1.5 -2.25", "float32 and float64");
            expect("binary::writer w; w.float32(0.1); to_string(w.to_string().reader().float32())", "0.10000000149011612", "float32 widens to the script float");
            expect("binary::writer w; w.float64(-0.0); var r = w.to_string().reader(); to_string(r.float64()) + \" \" + to_string(math::atan2(w.to_string().reader().float64(), -1.0) < 0.0)", "-0.0 true", "negative zero keeps its sign");
            expect("binary::writer w; w.float64(-0.0); to_json(w.to_string().to_bytes())", "[0,0,0,0,0,0,0,128]", "negative zero bits");
            expect("var nan = string::from_bytes([1, 0, 0, 0, 0, 0, 248, 127]); binary::writer w; w.float64(nan.reader().float64()); w.to_string() == nan", "true", "a float64 NaN keeps its payload");
            expect("var nan = string::from_bytes([1, 0, 192, 127]); binary::writer w; w.float32(nan.reader().float32()); w.to_string() == nan", "true", "a quiet float32 NaN keeps its payload");
            expect("var inf = string::from_bytes([0, 0, 0, 0, 0, 0, 240, 127]).reader().float64(); binary::writer w; w.float32(inf); to_json(w.to_string().to_bytes())", "[0,0,128,127]", "float32 infinity");
        });

        test("reader_text_bytes_and_cursor", [expect]() {
            expect("var r = \"AB\\x00CDEF\".reader(); var b = r.bytes(3); to_json(b) + \" \" + r.text(4)", "[65,66,0] CDEF", "bytes keep NULs, text stops at one");
            expect("var r = \"\\x05\\x06\\x07\".reader(); var b = r.bytes(2); to_json(b) + \" \" + to_string(r.at_end())", "[5,6] false", "bytes");
            expect("var r = \"abcd\".reader(); r.seek(3); to_string(r.pos()) + \" \" + to_string(r.remaining()) + \" \" + r.text(1) + \" \" + to_string(r.at_end())", "3 1 d true", "seek, remaining, at_end");
            expect("var r = \"abcd\".reader(); r.seek(4); to_string(r.at_end()) + \" \" + to_string(r.bytes(0).size()) + \" \" + r.text(0)", "true 0 ", "reads of nothing at the end");
            expect("\"\\x01\\x02\\x03\".reader().seek(1).skip(1).uint8()", "3", "seek and skip chain");
            expect("var r = \"abc\".reader(); to_string(r.size()) + \" \" + to_string(\"\".reader().size())", "3 0", "size");
        });

        test("text_fields_and_c_strings_are_bounded", [expect, caught]() {
            // the field: exactly n bytes, the text before the first NUL (all n without one)
            expect("var r = \"E1M1\\x00\\x00\\x00\\x00x\".reader(); r.text(8) + \" \" + to_string(r.pos())", "E1M1 8", "exactly n bytes");
            expect("var r = \"AB\\x00ZZZZZ\".reader(); r.text(8)", "AB", "bytes after the NUL are not the text");
            expect("\"TEXTURE1\".reader().text(8)", "TEXTURE1", "a full field with no NUL is all n bytes");
            expect(caught("var r = \"abc\".reader(); r.text(5)"), "Cannot read text(5) at position 0: 5 bytes wanted, 3 remain", "short data");
            expect("var r = \"abc\".reader(); try { r.text(5); } catch (e) {} to_string(r.pos())", "0", "short data leaves the cursor");
            expect(caught("\"abc\".reader().text(-1)"), "Cannot read text(-1): a count cannot be negative", "a negative field length");
            // the C string: up to the next NUL, the cursor past it, bounded by the end of the data
            expect("var r = \"ab\\x00cd\\x00\".reader(); r.text() + \"|\" + r.text() + \" \" + to_string(r.at_end())", "ab|cd true", "two C strings");
            expect("var r = \"abc\\x00\".reader(); var t = r.text(); t + \" \" + to_string(r.at_end())", "abc true", "the terminator at the last byte");
            expect("var r = \"\\x00z\".reader(); var t = r.text(); to_string(t.size()) + \" \" + to_string(r.pos())", "0 1", "an empty string");
            expect(caught("var r = \"xyz\".reader(); r.skip(1); r.text()"), "Cannot read null-terminated text at position 1: no NUL terminator in the remaining 2 bytes", "no terminator");
            expect(caught("\"\".reader().text()"), "Cannot read null-terminated text at position 0: no NUL terminator in the remaining 0 bytes", "no data");
            expect("var r = \"xyz\".reader(); try { r.text(); } catch (e) {} to_string(r.pos())", "0", "no terminator leaves the cursor");
            // the writer: w.text(s) ends in a NUL; w.text(s, n) pads with NULs, a text of exactly n fills the field, a longer one raises
            expect("binary::writer w; w.text(\"hi\").text(\"\"); to_json(w.to_string().to_bytes())", "[104,105,0,0]", "text then a NUL");
            expect("binary::writer w; w.text(\"abc\", 4).text(\"abcd\", 4).text(\"\", 0); to_json(w.to_string().to_bytes())", "[97,98,99,0,97,98,99,100]", "a field pads, exactly n fills it");
            expect("binary::writer w; w.text(\"one\").text(\"two\", 6).text(\"TEXTURE1\", 8); var r = w.to_string().reader(); r.text() + \" \" + r.text(6) + \" \" + r.text(8)", "one two TEXTURE1", "the writer's forms read back");
            expect(caught("binary::writer w; w.text(\"abcdef\", 4)"), "Cannot write text: the text is 6 bytes, longer than text(4)", "a text longer than its field");
            expect(caught("binary::writer w; w.text(\"a\", 0)"), "Cannot write text: the text is 1 byte, longer than text(0)", "a zero-byte field takes only empty text");
            expect(caught("binary::writer w; w.text(string::from_bytes([97, 0, 98]))"), "Cannot write text: the text holds a NUL at byte 1", "a NUL in the text");
            expect(caught("binary::writer w; w.text(string::from_bytes([0]), 4)"), "Cannot write text: the text holds a NUL at byte 0", "a NUL in a field's text");
            expect(caught("binary::writer w; w.text(\"a\", -1)"), "Cannot write text(s, -1): a count cannot be negative", "a negative field length");
            expect("binary::writer w; try { w.text(string::from_bytes([97, 0])); } catch (e) {} try { w.text(\"toolong\", 3); } catch (e) {} to_string(w.size())", "0", "a refused write writes nothing");
            // the layout tokens
            expect("var t = binary::text(4); var b = binary::bytes(3); t.name() + \" \" + to_string(t.size()) + \" \" + b.name() + \" \" + to_string(b.size())", "text(4) 4 bytes(3) 3", "the tokens");
            expect("var L = binary::layout(\"n\", binary::text(4), \"v\", binary::uint8); binary::writer w; w.pack(L, {\"n\": \"abc\", \"v\": 7}).pack(L, {\"n\": \"abcd\", \"v\": 8}); var r = w.to_string().reader(); "
                   "to_json(w.to_string().to_bytes()) + \" \" + to_json(r.unpack(L)) + \" \" + to_json(r.unpack(L))", "[97,98,99,0,7,97,98,99,100,8] {\"n\":\"abc\",\"v\":7} {\"n\":\"abcd\",\"v\":8}", "text(n) pads and fills like w.text(s, n)");
            expect("class N { string n; } var L = binary::layout(\"n\", binary::text(6)); var o = \"hey\\x00\\x00\\x00\".reader().read(N, L); o.n", "hey", "read fills a string field");
            expect(caught("var L = binary::layout(\"n\", binary::text(4)); binary::writer w; w.pack(L, {\"n\": string::from_bytes([120, 0])})"), "Cannot pack field 'n': the text holds a NUL at byte 1", "pack refuses a NUL");
            expect(caught("var L = binary::layout(\"n\", binary::text(4)); binary::writer w; w.pack(L, {\"n\": \"abcde\"})"), "Cannot pack field 'n': the text is 5 bytes, longer than text(4)", "pack refuses a longer text");
            expect(caught("var k = -1; binary::bytes(k)"), "binary::bytes expects a length of 0 or more, got -1", "a negative bytes length");
        });

        test("the_engine_bounds_every_null_terminated_scan", [this, caught]() {
            check_eq((size_t)1048576, jai::foundry::make_default_engine()->binary_scan_limit(), "the default limit is 1 MiB");
            auto run_limited = [](const std::string& src, bool use_vm, bool proofs, size_t limit) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? jai::backend_type::vm : jai::backend_type::interpreter);
                e->type_proofs(proofs);
                e->binary_scan_limit(limit);
                jai::stdlib::register_all(e);
                try { return e->execute(src).to_string(); }
                catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
            };
            const std::pair<std::string, std::string> rows[] = {
                {caught("\"abcdef\\x00\".reader().text()"), "Cannot read null-terminated text at position 0: no NUL terminator within 4 bytes (the engine's binary scan limit; raise it from C++ with engine->binary_scan_limit)"},
                {"\"abc\\x00\".reader().text()", "abc"},
                {caught("\"abcd\\x00\".reader().text()"), "Cannot read null-terminated text at position 0: no NUL terminator within 4 bytes (the engine's binary scan limit; raise it from C++ with engine->binary_scan_limit)"},
                {caught("\"abc\".reader().text()"), "Cannot read null-terminated text at position 0: no NUL terminator in the remaining 3 bytes"},
                {caught("\"abcd\".reader().text()"), "Cannot read null-terminated text at position 0: no NUL terminator in the remaining 4 bytes"},
                {"\"abcdefgh\\x00\".reader().text(9)", "abcdefgh"},
                {"var r = \"abcdefgh\\x00\".reader(); r.skip(1); try { r.text(); } catch (e) {} to_string(r.pos())", "1"},
                {"var r = \"abcdefgh\\x00\".reader(); r.skip(5); r.text()", "fgh"},
            };
            for (const auto& [src, want] : rows) {
                for (bool proofs : {true, false}) {
                    for (bool use_vm : {false, true}) {
                        check_eq(want, run_limited(src, use_vm, proofs, 4), std::string("scan limit 4: ") + src + (use_vm ? " (vm" : " (interpreter") + (proofs ? ")" : ", proofs off)"));
                    }
                }
            }
        });

        test("reading_past_the_end_names_the_position_and_width", [expect, caught]() {
            expect(caught("var r = \"abc\".reader(); r.skip(1); r.int32()"), "Cannot read int32 at position 1: 4 bytes wanted, 2 remain", "int32");
            expect(caught("var r = \"a\".reader(); r.uint8(); r.uint16()"), "Cannot read uint16 at position 1: 2 bytes wanted, 0 remain", "uint16 at the end");
            expect(caught("var r = \"ab\".reader(); r.float64()"), "Cannot read float64 at position 0: 8 bytes wanted, 2 remain", "float64");
            expect(caught("var r = \"ab\".reader(); r.skip(1); r.int16()"), "Cannot read int16 at position 1: 2 bytes wanted, 1 remains", "one byte remains");
            expect(caught("\"abc\".reader().text(5)"), "Cannot read text(5) at position 0: 5 bytes wanted, 3 remain", "text");
            expect(caught("\"abc\".reader().bytes(4)"), "Cannot read bytes(4) at position 0: 4 bytes wanted, 3 remain", "bytes");
            expect(caught("\"abc\".reader().bytes(-1)"), "Cannot read bytes(-1): a count cannot be negative", "a negative count");
            expect(caught("\"abc\".reader().seek(4)"), "Cannot seek to position 4: the data is 3 bytes", "seek past the end");
            expect(caught("\"abc\".reader().seek(-1)"), "Cannot seek to position -1: the data is 3 bytes", "a negative seek");
            expect(caught("var r = \"abc\".reader(); r.skip(2); r.skip(2)"), "Cannot skip 2 bytes at position 2: the data is 3 bytes", "skip past the end");
            expect("var r = \"abc\".reader(); try { r.int32(); } catch (e) {} to_string(r.pos())", "0", "a failed read leaves the cursor");
        });

        test("a_reader_is_a_value_over_a_snapshot", [expect]() {
            expect("var r = \"\\x01\\x02\".reader(); var c = r; c.uint8(); to_string(r.pos()) + \" \" + to_string(c.pos())", "0 1", "a copy has its own cursor");
            expect("string s = \"\\x01\\x02\"; var r = s.reader(); s = \"\\x09\"; to_string(r.uint8()) + \" \" + to_string(r.size())", "1 2", "rebinding the source leaves the reader");
            expect("string s = \"ab\"; var r = s.reader(); s.to_upper(); r.text(2) + s", "abAB", "mutating the source leaves the reader");
            expect("function first(binary::reader& r) -> int { return r.uint8(); } var r = \"\\x07\\x08\".reader(); first(r); to_string(first(r))", "8", "a reference parameter shares the cursor");
            expect("function first(binary::reader r) -> int { return r.uint8(); } var r = \"\\x07\\x08\".reader(); first(r); to_string(first(r))", "7", "a by-value parameter copies it");
            expect("binary::reader r = binary::reader(\"\\x05\"); to_string(r.uint8())", "5", "the constructor form");
            expect("binary::reader r; to_string(r.size()) + \" \" + to_string(r.at_end())", "0 true", "a default reader is empty");
            expect("utf8_string u = \"\\xC3\\xA9\"; var r = u.reader(); to_string(r.uint8()) + \" \" + to_string(r.size())", "195 2", "a utf8_string reader reads bytes");
        });

        test("writer_writes_each_width_and_checks_its_range", [expect, caught]() {
            expect("binary::writer w; w.int8(-1).uint8(255).int16(-32768).uint16(65535).int32(-2147483648).uint32(4294967295).int64(-2); to_json(w.to_string().to_bytes())",
                   "[255,255,0,128,255,255,0,0,0,128,255,255,255,255,254,255,255,255,255,255,255,255]", "every integer width at its extremes");
            expect(caught("binary::writer w; w.uint8(256)"), "Cannot write 256 as uint8 (0 to 255)", "uint8 too big");
            expect(caught("binary::writer w; w.int8(-129)"), "Cannot write -129 as int8 (-128 to 127)", "int8 too small");
            expect(caught("binary::writer w; w.int16(32768)"), "Cannot write 32768 as int16 (-32768 to 32767)", "int16");
            expect(caught("binary::writer w; w.uint16(-1)"), "Cannot write -1 as uint16 (0 to 65535)", "uint16 negative");
            expect(caught("binary::writer w; w.int32(2147483648)"), "Cannot write 2147483648 as int32 (-2147483648 to 2147483647)", "int32");
            expect(caught("binary::writer w; w.uint32(4294967296)"), "Cannot write 4294967296 as uint32 (0 to 4294967295)", "uint32");
            expect(caught("binary::writer w; w.float32(1e300)"), "Cannot write 1e+300 as float32 (-3.4028234663852886e+38 to 3.4028234663852886e+38)", "float32 overflow");
            expect("binary::writer w; try { w.uint8(1000); } catch (e) {} to_string(w.size())", "0", "a failed write writes nothing");
        });

        test("writer_text_bytes_seek_and_copies", [expect, caught]() {
            expect("binary::writer w; w.bytes(\"ab\").text(\"xyz\", 5).bytes(string::from_bytes([0, 9])); to_json(w.to_string().to_bytes())", "[97,98,120,121,122,0,0,0,9]", "bytes raw, text padded");
            expect("binary::writer w; w.bytes([1, 2, 255]); to_json(w.to_string().to_bytes())", "[1,2,255]", "bytes");
            expect(caught("binary::writer w; w.bytes([1, 300])"), "Cannot write bytes: element 1 is 300, not 0 to 255", "a bytes element out of range");
            expect(caught("binary::writer w; w.text(\"a\", -1)"), "Cannot write text(s, -1): a count cannot be negative", "a negative text length");
            expect("binary::writer w; w.uint32(0).bytes(\"payload\"); int end = w.pos(); w.seek(0).uint32(end).seek(end); to_string(w.to_string().reader().uint32()) + \" \" + to_string(w.size())", "11 11", "seek back to patch a field");
            expect("binary::writer w; w.uint8(1).uint8(2).uint8(3); w.seek(1).uint16(0x0909); to_json(w.to_string().to_bytes())", "[1,9,9]", "an overwrite past the end extends in place");
            expect(caught("binary::writer w; w.uint8(1); w.seek(2)"), "Cannot seek to position 2: 1 byte written", "seek past the written bytes");
            expect("binary::writer a; a.uint8(1); binary::writer b = a; b.uint8(2); to_string(a.size()) + \" \" + to_string(b.size())", "1 2", "a copy is independent");
            expect("binary::writer w = binary::writer(); w.uint16(7); to_string(w.pos()) + \" \" + to_string(w.size())", "2 2", "the constructor form");
        });

        test("layouts_unpack_pack_and_read", [expect, caught]() {
            const std::string layout = "var L = binary::layout(\"name\", binary::text(8), \"pos\", binary::int32, \"size\", binary::uint16); ";
            expect(layout + "to_string(L.size()) + \" \" + to_string(L.count())", "14 3", "size and count");
            expect(layout + "binary::writer w; w.pack(L, {\"name\": \"E1M1\", \"pos\": -5, \"size\": 7}); to_string(w.size()) + \" \" + to_json(w.to_string().reader().unpack(L))",
                   "14 {\"name\":\"E1M1\",\"pos\":-5,\"size\":7}", "pack then unpack");
            expect(layout + "binary::writer w; for (int i = 0; i < 3; ++i) { w.pack(L, {\"name\": \"L\" + to_string(i), \"pos\": i * 10, \"size\": i}); } "
                   "var r = w.to_string().reader(); var out = \"\"; while (!r.at_end()) { var e = r.unpack(L); out += e[\"name\"] + \":\" + to_string(e[\"pos\"]) + \" \"; } out",
                   "L0:0 L1:10 L2:20 ", "one layout over many records");
            expect("var r = \"\\x01\\x00\\x02\".reader(); to_json(r.unpack(\"a\", binary::uint16, \"b\", binary::int8))", "{\"a\":1,\"b\":2}", "inline name, width pairs");
            expect("binary::writer w; w.pack(binary::layout(\"f\", binary::float32, \"d\", binary::float64), {\"f\": 1, \"d\": 2.5}); to_json(w.to_string().reader().unpack(binary::layout(\"f\", binary::float32, \"d\", binary::float64)))",
                   "{\"d\":2.5,\"f\":1.0}", "floats in a layout (an int value into a float width)");
            expect("binary::writer w; w.big_endian().pack(binary::layout(\"v\", binary::uint16), {\"v\": 258}); to_json(w.to_string().to_bytes())", "[1,2]", "pack honors the byte order");
            expect(layout + "class Entry { string name; int pos; int size; } binary::writer w; w.pack(L, {\"name\": \"PLAYPAL\", \"pos\": 12, \"size\": 99}); "
                   "var e = w.to_string().reader().read(Entry, L); e.name + \" \" + to_string(e.pos) + \" \" + to_string(e.size)", "PLAYPAL 12 99", "read fills a class in field order");
            expect("class Base { int a; } class Derived : Base { var b; } var d = \"\\x01\\x02\".reader().read(Derived, binary::layout(\"x\", binary::uint8, \"y\", binary::uint8)); to_string(d.a) + \" \" + to_string(d.b)", "1 2", "read fills base fields first");
            expect("class T { utf8_string n; } var t = \"ab\".reader().read(T, binary::layout(\"n\", binary::text(2))); type_of(t.n)", "utf8_string", "a utf8_string field takes text");
        });

        test("layout_text_and_bytes_match_the_reader_and_writer", [expect, caught]() {
            // binary::text(n) is r.text(n) / w.text(s, n); binary::bytes(n) is exactly n raw bytes, r.bytes(n) / w.bytes(b)
            expect("var L = binary::layout(\"t\", binary::text(5)); to_json(string::from_bytes([97, 0, 98, 0, 0]).reader().unpack(L))", "{\"t\":\"a\"}", "text(n) ends at the first NUL");
            expect("var L = binary::layout(\"b\", binary::bytes(5)); to_json(string::from_bytes([97, 0, 98, 0, 0]).reader().unpack(L)[\"b\"])", "[97,0,98,0,0]", "bytes(n) keeps embedded and trailing NULs");
            expect("class C { array<int> b; } var c = string::from_bytes([0, 1, 0, 2, 0]).reader().read(C, binary::layout(\"b\", binary::bytes(5))); to_json(c.b)", "[0,1,0,2,0]", "read fills an array field");
            for (const char* value : {"\"ab\"", "\"abcde\"", "\"\""}) {
                expect(std::string("var L = binary::layout(\"t\", binary::text(5)); var v = ") + value + "; binary::writer a; a.pack(L, {\"t\": v}); binary::writer b; b.text(v, 5); "
                       "to_string(a.to_string() == b.to_string()) + \" \" + to_string(a.to_string().reader().unpack(L)[\"t\"] == b.to_string().reader().text(5))",
                       "true true", (std::string("pack and w.text(s, n) agree byte for byte: ") + value).c_str());
            }
            for (const char* value : {"string::from_bytes([97, 0, 98, 0, 0])", "[1, 2, 3, 0, 0]", "\"\\x00\\x00\\x00\\x00\\x00\""}) {
                expect(std::string("var L = binary::layout(\"b\", binary::bytes(5)); var v = ") + value + "; binary::writer a; a.pack(L, {\"b\": v}); binary::writer b; b.bytes(v); "
                       "to_string(a.to_string() == b.to_string()) + \" \" + to_string(a.to_string().reader().unpack(L)[\"b\"] == b.to_string().reader().bytes(5))",
                       "true true", (std::string("pack and w.bytes(b) agree byte for byte: ") + value).c_str());
            }
            expect(caught("var L = binary::layout(\"b\", binary::bytes(4)); binary::writer w; w.pack(L, {\"b\": \"abc\"})"), "Cannot pack field 'b': bytes(4) takes exactly 4 bytes, got 3", "bytes(n) packs exactly n");
            expect(caught("var L = binary::layout(\"b\", binary::bytes(2)); binary::writer w; w.pack(L, {\"b\": [1, 300]})"), "Cannot pack field 'b': element 1 is 300, not 0 to 255", "bytes(n) values are bytes");
            expect(caught("var L = binary::layout(\"b\", binary::bytes(2)); binary::writer w; w.pack(L, {\"b\": 5})"), "Cannot pack field 'b': int is not bytes (a string or an array of 0 to 255)", "bytes(n) takes a string or an array");
        });

        test("layout_errors_are_catchable", [expect, caught]() {
            const std::string layout = "var L = binary::layout(\"name\", binary::text(4), \"pos\", binary::int32); ";
            expect(caught("var n = 3; var w = binary::int8; binary::layout(\"a\", w, \"b\", n)"), "binary::layout argument 4 must be a binary width (binary::int32, binary::text(8), ...), got int", "a non-width");
            expect(caught("var make = binary::layout; make(\"a\")"), "binary::layout expects name, width pairs, got 1 argument", "an odd count");
            expect(caught("var k = 5; binary::layout(k, binary::int8)"), "binary::layout argument 1 must be a field name string, got int", "a non-string name");
            expect(caught("binary::layout(\"a\", binary::int8, \"a\", binary::int8)"), "binary::layout repeats the field name 'a'", "a repeated name");
            expect(caught("binary::text(-1)"), "binary::text expects a length of 0 or more, got -1", "a negative text length");
            expect(caught(layout + "binary::writer w; w.pack(L, {\"name\": \"x\"})"), "Cannot pack field 'pos': the map has no 'pos'", "a missing field");
            expect(caught(layout + "binary::writer w; w.pack(L, {\"name\": \"x\", \"pos\": 4294967296})"), "Cannot pack field 'pos': cannot write 4294967296 as int32 (-2147483648 to 2147483647)", "a value that does not fit");
            expect(caught(layout + "binary::writer w; w.pack(L, {\"name\": 5, \"pos\": 1})"), "Cannot pack field 'name': int does not fit text(4)", "a value of the wrong kind");
            expect(layout + "binary::writer w; try { w.pack(L, {\"name\": \"x\"}); } catch (e) {} to_string(w.size())", "0", "a failed pack writes nothing");
            expect(caught(layout + "\"abcdef\".reader().unpack(L)"), "Cannot read field 'pos' (int32) at position 4: 4 bytes wanted, 2 remain", "unpack past the end names the field");
            expect(layout + "var r = \"abcdef\".reader(); try { r.unpack(L); } catch (e) {} to_string(r.pos())", "0", "a failed unpack leaves the cursor");
            expect(caught(layout + "class One { int a; } var k = One; \"abcdefgh\".reader().read(k, L)"), "Cannot read One from a layout of 2 fields: One has 1 field", "a field count mismatch");
            expect(caught(layout + "class Two { int name; int pos; } var k = Two; \"abcdefgh\".reader().read(k, L)"), "Cannot read field 'name' (text(4)) into Two.name (declared int)", "a field kind mismatch");
            expect(caught(layout + "\"abcdefgh\".reader().read(5, L)"), "read expects a class, got int", "not a class");
            // widths whose total wraps would size a pack's staging below what its fields write
            const std::string huge = "binary::text(9223372036854775807)";
            const std::string too_big = "binary::layout fields total more than 9223372036854775807 bytes";
            expect(caught("binary::layout(\"a\", " + huge + ", \"b\", " + huge + ", \"c\", binary::bytes(2))"), too_big, "a total that wraps size_t");
            expect(caught("binary::layout(\"a\", " + huge + ", \"b\", binary::int8)"), too_big, "a total past the int range");
            expect(caught("binary::writer w; \"hello\".reader().unpack(\"a\", " + huge + ", \"b\", " + huge + ", \"c\", binary::bytes(2))"), "unpack fields total more than 9223372036854775807 bytes", "an inline layout");
            expect("to_string(binary::layout(\"a\", " + huge + ").size())", "9223372036854775807", "one width of the whole range");
        });

        test("direct_lanes_agree_with_the_bound_methods", [this, run]() {
            // the vm reads hot members through class_definition::native_method lanes; shapes a lane declines take the
            // bound method, so every spelling answers as the interpreter's bound call does
            const char* shapes[] = {
                "var r = \"\\x01\\x02\\x03\\x04\".reader(); to_string(r.uint16()) + \" \" + to_string(r.seek(1).int8()) + \" \" + to_string(r.pos())",
                "var r = \"abcd\".reader(); var t = \"\"; try { t = r.text(2.0); } catch (e) { t = e; } t",
                "var r = \"abcd\".reader(); var t = \"\"; try { r.seek(true); t = to_string(r.pos()); } catch (e) { t = e; } t",
                "var r = \"abcd\".reader(); var t = \"\"; try { r.int8(1); } catch (e) { t = e; } t",
                "var r = \"ab\".reader(); var t = \"\"; try { r.int32(); } catch (e) { t = e; } t + \" \" + to_string(r.pos())",
                "var w = binary::writer(); var t = \"\"; try { w.uint8(3.0); t = to_string(w.size()); } catch (e) { t = e; } t",
                "var w = binary::writer(); utf8_string u = \"\\xC3\\xA9\"; w.text(u).uint8(1); to_json(w.to_string().to_bytes())",
                "var w = binary::writer(); var t = \"\"; try { w.uint16(70000); } catch (e) { t = e; } t + \" \" + to_string(w.size())",
                "var r = \"\\x05\".reader(); var same = r.skip(0); same.uint8(); to_string(r.pos()) + \" \" + to_string(same.pos())",
                "var r = \"ab\\x00cd\".reader(); var t = r.text(); var u = \"\"; try { u = r.text(); } catch (e) { u = e; } t + \"|\" + u + \"|\" + to_string(r.pos())",
                "var r = \"abcdef\\x00x\".reader(); var t = r.text(); t + \" \" + r.text(1.0)",
            };
            for (const char* src : shapes) {
                for (bool proofs : {true, false}) {
                    check_eq(run(src, false, proofs), run(src, true, proofs), std::string("vm lane matches the interpreter: ") + src);
                }
            }
        });

        test("qualified_type_spellings", [expect]() {
            expect("binary::writer w; w.uint8(1); binary::reader r = w.to_string().reader(); to_string(r.uint8())", "1", "declarations");
            expect("function mk(string s) -> binary::reader { return s.reader().seek(1); } to_string(mk(\"\\x01\\x02\").uint8())", "2", "a return type");
            expect("class Holder { binary::writer out; } Holder h; h.out.uint16(1); to_string(h.out.size())", "2", "a field");
            expect("array<binary::layout> ls; ls.push(binary::layout(\"a\", binary::uint8)); to_string(ls[0].size())", "1", "an element type");
            expect("namespace geo { class Rect { int w = 3; } } geo::Rect r; to_string(r.w)", "3", "a class of a namespace this unit declares");
            expect("to_string(binary::int32.size()) + \" \" + binary::text(8).name()", "4 text(8)", "the width tokens");
        });

        test("the_checker_knows_the_library", [this]() {
            auto e = jai::foundry::make_engine();
            jai::stdlib::register_all(e);
            auto errors = [&](const std::string& src) { return e->check(src).error_count(); };
            auto says = [&](const std::string& src, const std::string& text) {
                for (const auto& d : e->check(src).diagnostics) { if (d.message.find(text) != std::string::npos) { return true; } }
                return false;
            };
            check_eq((size_t)0, errors("binary::writer w; binary::reader r = w.to_string().reader(); int v = r.uint16(); string t = r.text(2); array<int> b = r.bytes(2); array<uint8> u = r.bytes(2);"), "a clean program");
            check_true(says("var L = binary::layout(\"a\", binary::int8, \"b\");", "binary::layout expects name, width pairs, got 3 arguments"), "an odd pairing");
            check_true(says("var L = binary::layout(\"a\", 3);", "binary::layout argument 2 must be a binary width"), "a non-width");
            check_eq((size_t)0, errors("class C { array<int> b; } var c = \"\".reader().read(C, binary::layout(\"b\", binary::bytes(8)));"), "a bytes token fills an array field");
            check_true(says("class D { string b; } var d = \"\".reader().read(D, binary::layout(\"b\", binary::bytes(8)));", "Cannot read layout field 1 into D.b (declared string)"), "a bytes token into a string field");
            check_true(says("binary::reader r = \"\".reader(); r.text(1, 2);", "'binary::reader.text' expects 0 or 1 argument(s), got 2"), "both reader text forms are known");
            check_true(says("var L = binary::layout(5, binary::int8);", "binary::layout argument 1 must be a field name string, got int"), "a non-string name");
            check_true(says("binary::reader r = \"\".reader(); var m = r.unpack(\"a\", binary::int8, \"b\", \"x\");", "unpack argument 4 must be a binary width"), "unpack pairs");
            check_true(says("class Bad { string name; float pos; } binary::layout L = binary::layout(\"name\", binary::text(8), \"pos\", binary::int32); var b = \"\".reader().read(Bad, L);",
                            "Cannot read layout field 2 into Bad.pos (declared float)"), "read's field kinds");
            check_true(says("class One { int a; } var o = \"\".reader().read(One, binary::layout(\"a\", binary::int8, \"b\", binary::int8));",
                            "Cannot read One from a layout of 2 fields: One has 1 field"), "read's field count");
            check_eq((size_t)0, errors("class Bad { string name; } var L = binary::layout(\"a\", binary::int32); L = binary::layout(\"name\", binary::text(2)); var b = \"\".reader().read(Bad, L);"),
                     "a reassigned layout variable stays dynamic");
            check_true(says("binary::reader r = \"\".reader(); r.int8(5);", "'binary::reader.int8' expects 0 argument(s), got 1"), "a strict signature's arity");
            check_true(says("auto r = \"\".reader(); r.seek(\"x\");", "no overload of 'binary::reader.seek' accepts these argument types"), "a strict signature's argument kinds");
            check_true(says("binary::reader r = \"\".reader(); int n = r.text(3);", "cannot initialize 'n' declared 'int' with 'string'"), "a strict signature's result");
            check_eq((size_t)0, errors("var r = \"\".reader(); r.int8(5);"), "a var receiver stays dynamic");
            check_true(says("binary::writer w; w.pack(5, {});", "pack expects a binary::layout, got int"), "pack's layout");
            check_true(says("array<string> a = \"x\".to_bytes();", "with 'array<uint8>'"), "to_bytes is an array<uint8>");
            check_eq((size_t)0, errors("array<uint8> a = \"x\".to_bytes(); array<int> b = \"x\".to_bytes();"), "to_bytes fills array<uint8> and array<int>");
        });
    }
};

} // namespace jai::foundry::tests

using binary_data_tests = jai::foundry::tests::binary_data_tests;
FOUNDRY_REGISTER(binary_data_tests)
