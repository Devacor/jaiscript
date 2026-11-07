#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

namespace jai::foundry::tests {

class web_text_tests : public suite {
public:
	web_text_tests() : suite("Web Text Tests") {}

	void forge_tests() override {
		auto es = [this](const std::string& a_source) { auto engine = make_engine(); stdlib::register_all(*engine); stdlib::register_web(*engine); return engine->execute(a_source).as<std::string>(); };
		auto caught = [es](const std::string& a_source) { return es("string m = \"\"; try { " + a_source + " } catch (e) { m = e; } m;"); };

		test("html_escape_escapes_the_five_and_keeps_everything_else", [this, es]() {
			check_eq(std::string("&lt;a href=&quot;x&quot;&gt;Tom &amp; Jerry&#39;s&lt;/a&gt;"), es("web::html_escape(\"<a href=\\\"x\\\">Tom & Jerry's</a>\");"));
			check_eq(std::string(""), es("web::html_escape(\"\");"));
			check_eq(std::string("plain text, no change"), es("web::html_escape(\"plain text, no change\");"));
			check_eq(std::string("&amp;amp;"), es("web::html_escape(\"&amp;\");"));
			check_eq(std::string("caf\xC3\xA9 &lt;\xF0\x9F\x99\x82&gt; \xFF\x00x", 22), es("web::html_escape(\"caf\xC3\xA9 <\xF0\x9F\x99\x82> \\xFF\\x00x\");"));
			check_eq(std::string("utf8_string 10 \xC3\xA9&lt;b&gt;"), es("var u = web::html_escape(utf8_string(\"\xC3\xA9<b>\")); type_of(u) + \" \" + to_string(u.size()) + \" \" + u;"));
			check_eq(std::string("string utf8_string"), es("`${type_of(web::html_escape(\"a\"))} ${type_of(web::html_escape(utf8_string(\"a\")))}`;"));
			check_eq(std::string("x untouched"), es("string s = \"x\"; string t = web::html_escape(s); t += \" untouched\"; `${s} untouched`;"));
		});

		test("html_unescape_reads_references_as_a_browser_does", [this, es]() {
			const auto unescape = [es](const std::string& a_text) { return es("web::html_unescape(\"" + a_text + "\");"); };
			check_eq(std::string("<>&\"''"), unescape("&lt;&gt;&amp;&quot;&apos;&#39;"));
			check_eq(std::string("& < > \" \xC2\xA9 \xC2\xAE & < > \" \xC2\xA9 \xC2\xAE"), unescape("&amp &lt &gt &quot &copy &reg &AMP &LT &GT &QUOT &COPY &REG"));
			check_eq(std::string("\xE2\x88\x89 \xC2\xACit; \xC2\xAC \xC2\xACi &x; &x <x"), unescape("&notin; &notit; &not &noti &ampx; &amp;x &ltx"));
			check_eq(std::string("\xE2\x89\xAA\xE2\x83\x92 \xE2\x89\xAB\xE2\x83\x92 \xE2\x89\x82\xCC\xB8 \xE2\x88\xB3 \xE2\x80\x8C \xC3\x86 \xC3\x86"),
				unescape("&nLt; &nGt; &NotEqualTilde; &CounterClockwiseContourIntegral; &zwnj; &AElig &AElig;"));
			check_eq(std::string(";:,.!?#$%*"), unescape("&semi;&colon;&comma;&period;&excl;&quest;&num;&dollar;&percnt;&ast;"));
			check_eq(std::string("fj \xE2\x81\x9F\xE2\x80\x8A =\xE2\x83\xA5 \xE2\x88\xBE\xCC\xB3"), unescape("&fjlig; &ThickSpace; &bne; &acE;"));
			check_eq(std::string("A A A AB AG A A"), unescape("&#65; &#x41; &#X41; &#65B &#x41G &#0065; &#x0000041;"));
			check_eq(std::string("\xE2\x82\xAC \xE2\x82\xAC \xE2\x80\x93 \xC5\xB8 \xC2\x81 \xC2\x8D \xC5\xB8"), unescape("&#128; &#x80; &#150; &#x9F; &#x81; &#x8D; &#159;"));
			check_eq(std::string("& &; &#; &#x; &#xG; &bogus; &Amp; &#-1; && &&"), unescape("& &; &#; &#x; &#xG; &bogus; &Amp; &#-1; && &&amp;"));
			check_eq(std::string("trailing &"), unescape("trailing &amp"));
			check_eq(std::string("&"), unescape("&"));
			check_eq(std::string("a&b&c"), unescape("a&b&c"));
			check_eq(std::string("&&#&#x"), unescape("&&#&#x"));
		});

		test("html_unescape_turns_impossible_code_points_into_the_replacement_character", [this, es]() {
			check_eq(std::string("\xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD"),
				es("web::html_unescape(\"&#0; &#xD800; &#xDFFF; &#x110000; &#99999999999999999999; &#x0;\");"));
			check_eq(std::string("\xF4\x8F\xBF\xBF \xF4\x8F\xBF\xBF \x01 \x7F"), es("web::html_unescape(\"&#x10FFFF; &#1114111; &#1; &#127;\");"));
		});

		test("html_unescape_keeps_the_text_kind_and_bytes_it_does_not_read", [this, es]() {
			check_eq(std::string("utf8_string 4 \xC3\xA9<\xE2\x82\xAC>"), es("var u = web::html_unescape(utf8_string(\"\xC3\xA9&lt;&euro;&gt;\")); type_of(u) + \" \" + to_string(u.size()) + \" \" + u;"));
			check_eq(std::string("\xFF<\x00", 3), es("web::html_unescape(\"\\xFF&lt;\\x00\");"));
			check_eq(std::string("string no references"), es("`${type_of(web::html_unescape(\"no references\"))} ${web::html_unescape(\"no references\")}`;"));
		});

		test("html_escape_then_unescape_round_trips_every_byte_string", [this, es]() {
			check_eq(std::string("0"), es(R"(
				string alphabet = "&;#xX<>\"'amp lt gt quot 39 0123456789 \xC3\xA9\xFF\x00z";
				int seed = 7;
				int bad = 0;
				for (int round = 0; round < 400; ++round) {
					string s = "";
					for (int i = 0; i < round % 37; ++i) {
						seed = (seed * 1103515245 + 12345) % 2147483648;
						s += alphabet[seed % alphabet.size()];
					}
					if (web::html_unescape(web::html_escape(s)) != s) { ++bad; }
					utf8_string u = s;
					if (web::html_unescape(web::html_escape(u)) != u) { ++bad; }
				}
				to_string(bad);)"));
		});

		test("arguments_are_checked_at_load_and_at_run_time", [this, caught]() {
			check_eq(std::string("web::html_escape expects a string"), caught("var n = 5; web::html_escape(n);"));
			check_eq(std::string("web::html_unescape expects a string"), caught("var n = null; web::html_unescape(n);"));
			auto engine = make_engine();
			stdlib::register_all(*engine);
			stdlib::register_web(*engine);
			for (const char* source : {"web::html_escape(5);", "web::html_unescape();", "web::html_unescape(\"a\", \"b\");"}) {
				bool rejected = false;
				try { engine->execute(source); } catch (const std::exception&) { rejected = true; }
				check_true(rejected, source);
			}
		});

		test("the_html_functions_need_no_filesystem_privilege", [this]() {
			auto engine = make_engine();
			stdlib::register_all(*engine);
			stdlib::register_web(*engine);
			engine->disable(privilege::filesystem);
			check_eq(std::string("&lt; <"), engine->execute("web::html_escape(\"<\") + \" \" + web::html_unescape(\"&lt;\");").as<std::string>());
		});

		test("the_html_functions_run_in_parallel_bodies", [this, es]() {
			check_eq(std::string("true true"), es(R"(
				array<string> texts = [];
				for (int i = 0; i < 64; ++i) { texts.push(`<${i}> & '${i}'`); }
				array<string> serial = [];
				for (auto t : texts) { serial.push(web::html_escape(t) + web::html_unescape(web::html_escape(t))); }
				var parallel = parallel_transform(texts, [](string t) { return web::html_escape(t) + web::html_unescape(web::html_escape(t)); });
				array<string> looped = texts;
				parallel_for (auto& t : looped) { t = web::html_escape(t) + web::html_unescape(web::html_escape(t)); }
				to_string(to_json(parallel) == to_json(serial)) + " " + to_string(to_json(looped) == to_json(serial));)"));
		});

		test("url_encode_and_url_decode_are_rfc_3986_percent_encoding", [this, es]() {
			check_eq(std::string("a%20b%26c%2Fd~-._%C3%A9%3F%3D"), es("web::url_encode(\"a b&c/d~-._\xC3\xA9?=\");"));
			check_eq(std::string("a b/c+d%zz%4"), es("web::url_decode(\"a%20b%2fc+d%zz%4\");"));
			check_eq(std::string("100%"), es("web::url_decode(\"100%\");"));
			check_eq(std::string("0 bad"), es(R"(
				int bad = 0;
				for (int b = 0; b < 256; ++b) {
					string s = `x${to_char(b)}y`;
					if (web::url_decode(web::url_encode(s)) != s) { ++bad; }
				}
				to_string(bad) + " bad";)"));
		});

		test("form_encode_and_form_decode_are_the_form_encoding", [this, es]() {
			check_eq(std::string("a+b*-._%7E%2B%26"), es("web::form_encode(\"a b*-._~+&\");"));
			check_eq(std::string("a b+&"), es("web::form_decode(\"a+b%2B%26\");"));
			check_eq(std::string("0 bad"), es(R"(
				int bad = 0;
				for (int b = 0; b < 256; ++b) {
					string s = `${to_char(b)} ${to_char(b)}`;
					if (web::form_decode(web::form_encode(s)) != s) { ++bad; }
				}
				to_string(bad) + " bad";)"));
		});

		test("parse_query_reads_pairs_and_parse_query_all_keeps_every_value", [this, es]() {
			check_eq(std::string(R"({"a":"last","b":"two words","c":"&","empty":""})"), es(R"(to_json(web::parse_query("?a=1&b=two+words&c=%26&empty&a=last&=x&&"));)"));
			check_eq(std::string(R"({"t":["a","b c"],"u":[""]})"), es(R"(to_json(web::parse_query_all("t=a&t=b+c&u="));)"));
			check_eq(std::string("1 0"), es(R"(map<string, string> m = web::parse_query("a=1"); m["a"] + " " + to_string(web::parse_query("").size());)"));
		});

		test("build_query_writes_the_map_in_key_order", [this, es, caught]() {
			check_eq(std::string("a=1&b=two+words&f=2.5&ok=true&t=x&t=y+z"),
				es(R"(web::build_query({"b": "two words", "a": 1, "n": null, "t": ["x", "y z"], "f": 2.5, "ok": true});)"));
			check_eq(std::string(R"({"k&":"v=1","x":"a b"})"), es(R"(to_json(web::parse_query(web::build_query({"k&": "v=1", "x": "a b"})));)"));
			check_eq(std::string("web::build_query: the value of 'm' is a map"), caught(R"(web::build_query({"m": {"x": 1}});)"));
			check_eq(std::string(""), es("web::build_query({});"));
		});

		test("parse_url_splits_rfc_3986_references_and_to_string_joins_them", [this, es, caught]() {
			check_eq(std::string("https|user|pw|example.com|8443|/a/b|x=1&y=2|frag|https://user:pw@example.com:8443/a/b?x=1&y=2#frag"), es(R"(
				web::url u = web::parse_url("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
				`${u.scheme}|${u.user}|${u.password}|${u.host}|${u.port}|${u.path}|${u.query}|${u.fragment}|${u.to_string()}`;)"));
			check_eq(std::string("||0|/relative/path|q|"), es(R"(web::url u = web::parse_url("/relative/path?q"); `${u.scheme}|${u.host}|${u.port}|${u.path}|${u.query}|${u.fragment}`;)"));
			check_eq(std::string("[::1] 80 /"), es(R"(web::url u = web::parse_url("http://[::1]:80/"); `${u.host} ${u.port} ${u.path}`;)"));
			check_eq(std::string("mailto||ada@example.org"), es(R"(web::url u = web::parse_url("mailto:ada@example.org"); `${u.scheme}|${u.host}|${u.path}`;)"));
			check_eq(std::string("web::parse_url: 'http://host:99999/' has a malformed host or port"), caught(R"(web::parse_url("http://host:99999/");)"));
			check_eq(std::string("https://x.org/p?q=a+b"), es(R"(
				web::url u;
				u.scheme = "https";
				u.host = "x.org";
				u.path = "/p";
				u.query = web::build_query({"q": "a b"});
				u.to_string();)"));
		});

		test("the_web_library_is_registered_only_by_register_web_and_its_calls_are_checked", [this]() {
			auto plain = make_engine();
			stdlib::register_all(*plain);
			bool missing = false;
			try { plain->execute("web::url_encode(\"a\");"); } catch (const std::exception&) { missing = true; }
			check_true(missing, "register_all registers no web:: functions");
			auto engine = make_engine();
			stdlib::register_all(*engine);
			stdlib::register_web(*engine);
			for (const char* source : {"web::url_encode(1);", "int n = web::url_encode(\"a\");", "web::parse_query();", "web::build_query(\"a=1\");", "web::html_escape(\"a\", \"b\");"}) {
				bool rejected = false;
				try { engine->execute(source); } catch (const std::exception&) { rejected = true; }
				check_true(rejected, source);
			}
		});

		test("the_web_functions_run_in_parallel_bodies", [this, es]() {
			check_eq(std::string("true true"), es(R"(
				string mix(string t) {
					web::url u = web::parse_url(`http://h/${web::form_encode(t)}`);
					return web::url_encode(t) + web::html_escape(t) + web::build_query(web::parse_query(t)) + u.path;
				}
				array<string> texts = [];
				for (int i = 0; i < 64; ++i) { texts.push(`a=${i}&b=<${i}> & '${i}'`); }
				array<string> serial = [];
				for (auto t : texts) { serial.push(mix(t)); }
				var parallel = parallel_transform(texts, [](string t) { return mix(t); });
				array<string> looped = texts;
				parallel_for (auto& t : looped) { t = mix(t); }
				to_string(to_json(parallel) == to_json(serial)) + " " + to_string(to_json(looped) == to_json(serial));)"));
		});
	}
};

} // namespace jai::foundry::tests

using web_text_tests = jai::foundry::tests::web_text_tests;
FOUNDRY_REGISTER(web_text_tests)
