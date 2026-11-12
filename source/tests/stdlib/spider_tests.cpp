#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/http_transport.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace jai::foundry::tests {

namespace {

	// Answers each exchange from a script of replies (or a function of the request) and keeps every request it saw
	class scripted_transport : public http_transport {
	public:
		std::vector<http_request> seen;
		std::deque<http_reply> replies;
		std::function<http_reply(const http_request&)> answer;

		http_reply exchange(const http_request& a_request) override {
			seen.push_back(a_request);
			if (answer) { return answer(a_request); }
			if (replies.empty()) {
				http_reply result;
				result.status = 200;
				return result;
			}
			http_reply result = replies.front();
			replies.pop_front();
			return result;
		}

		std::string header(size_t a_index, const std::string& a_name) const {
			for (const auto& [name, value] : seen.at(a_index).headers) {
				if (name == a_name) { return value; }
			}
			return "<none>";
		}
	};

	http_reply reply(int a_status, std::vector<std::pair<std::string, std::string>> a_headers = {}, std::string a_body = "") {
		http_reply result;
		result.status = a_status;
		result.headers = std::move(a_headers);
		result.body = std::move(a_body);
		return result;
	}

}

class spider_tests : public suite {
public:
	spider_tests() : suite("Spider Client") {}

	std::shared_ptr<engine> spider_engine(const std::shared_ptr<scripted_transport>& a_transport) {
		auto result = make_engine();
		stdlib::register_all(*result);
		stdlib::register_spider(*result);
		result->enable(jai::permissions::network);
		result->http_transport(a_transport);
		return result;
	}

	void forge_tests() override {
		auto text = [](const std::shared_ptr<engine>& a_engine, const std::string& a_source) { return a_engine->execute(a_source).as<std::string>(); };
		auto caught = [text](const std::shared_ptr<engine>& a_engine, const std::string& a_source) {
			return text(a_engine, "string m = \"\"; try { " + a_source + " } catch (e) { m = e; } m;");
		};

		test("without_a_transport_a_call_raises_a_catchable_error_naming_the_install", [this, caught]() {
			auto e = make_engine();
			stdlib::register_all(*e);
			stdlib::register_spider(*e);
			e->enable(jai::permissions::network);
			check_eq(std::string("spider::get: no HTTP transport installed: the host provides one with engine::http_transport()"), caught(e, "spider::get(\"http://example.org/\");"));
			check_eq(std::string("spider::post: no HTTP transport installed: the host provides one with engine::http_transport()"), caught(e, "spider::post(\"http://example.org/\", \"x\");"));
			check_eq(std::string("spider::sling: no HTTP transport installed: the host provides one with engine::http_transport()"), caught(e, "spider::sling(\"x\", \"http://example.org/\");"));
		});

		test("the_network_permission_starts_disabled_and_gates_every_call", [this, caught, text]() {
			auto transport = std::make_shared<scripted_transport>();
			auto e = make_engine();
			stdlib::register_all(*e);
			stdlib::register_spider(*e);
			e->http_transport(transport);
			check_eq(std::string("false"), text(e, "to_string(jai::permissions::is_enabled(jai::permissions::network));"));
			check_eq(std::string("Insufficient permission: network."), caught(e, "spider::get(\"http://example.org/\");"));
			check_eq(std::string("Insufficient permission: network."), caught(e, "spider::post(\"http://example.org/\", \"x\");"));
			check_eq(size_t{0}, transport->seen.size());
			e->enable(jai::permissions::network);
			check_eq(std::string("200"), text(e, "to_string(spider::get(\"http://example.org/\").status);"));
			check_eq(size_t{1}, transport->seen.size());
		});

		test("get_sends_a_get_and_answers_a_response", [this, text]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->replies.push_back(reply(200, {{"Content-Type", "application/json"}, {"X-Many", "a"}, {"x-many", "b"}, {"Set-Cookie", "a=1; Path=/"}, {"Set-Cookie", "b=2"}}, "{\"n\": 3, \"list\": [1, 2]}"));
			auto e = spider_engine(transport);
			check_eq(std::string("200|true|application/json|a, b|2|a=1; Path=/|3|2|http://example.org/data?x=1|application/json"), text(e, R"(
				spider::Response r = spider::get("http://example.org/data?x=1#frag");
				var j = r.json();
				`${r.status}|${r.ok()}|${r.headers["content-type"]}|${r.headers["x-many"]}|${r.cookies.size()}|${r.cookies[0]}|${j["n"]}|${j["list"].size()}|${r.url}|${r.header("CONTENT-TYPE")}`;
			)"));
			check_eq(size_t{1}, transport->seen.size());
			check_eq(std::string("GET"), transport->seen[0].method);
			check_eq(std::string("http://example.org/data?x=1"), transport->seen[0].url);
			check_eq(std::string(""), transport->seen[0].body);
			check_eq(std::string("*/*"), transport->header(0, "Accept"));
			check(transport->header(0, "User-Agent").rfind("jaiscript-spider/", 0) == 0, "a default User-Agent names the spider");
			check(transport->seen[0].timeout_seconds > 29.0 && transport->seen[0].timeout_seconds <= 30.0, "the default timeout is 30 s");
		});

		test("an_http_error_status_is_a_response_not_an_error", [this, text]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->replies.push_back(reply(404, {}, "missing"));
			auto e = spider_engine(transport);
			check_eq(std::string("404 false missing"), text(e, "auto r = spider::get(\"https://example.org/x\"); `${r.status} ${r.ok()} ${r.body}`;"));
		});

		test("post_and_sling_send_the_same_request_with_their_arguments_in_their_own_order", [this, text]() {
			auto transport = std::make_shared<scripted_transport>();
			auto e = spider_engine(transport);
			text(e, "spider::post(\"http://example.org/in\", \"hello\"); \"\";");
			text(e, "spider::sling(\"hello\", \"http://example.org/in\"); \"\";");
			check_eq(size_t{2}, transport->seen.size());
			for (size_t i = 0; i < 2; ++i) {
				check_eq(std::string("POST"), transport->seen[i].method);
				check_eq(std::string("http://example.org/in"), transport->seen[i].url);
				check_eq(std::string("hello"), transport->seen[i].body);
				check_eq(std::string("text/plain; charset=utf-8"), transport->header(i, "Content-Type"));
			}
		});

		test("content_is_text_as_it_is_anything_else_json_and_a_form_with_type_form", [this, text]() {
			auto transport = std::make_shared<scripted_transport>();
			auto e = spider_engine(transport);
			text(e, R"(spider::post("http://h/", {"a": 1, "b": [true, null]}); "";)");
			check_eq(std::string("{\"a\":1,\"b\":[true,null]}"), transport->seen[0].body);
			check_eq(std::string("application/json"), transport->header(0, "Content-Type"));
			text(e, R"(spider::post("http://h/", {"name": "Ada L", "msg": "a&b=c", "n": 2}, {"type": "form"}); "";)");
			check_eq(std::string("msg=a%26b%3Dc&n=2&name=Ada+L"), transport->seen[1].body);
			check_eq(std::string("application/x-www-form-urlencoded"), transport->header(1, "Content-Type"));
			text(e, R"(spider::post("http://h/", "<p>hi</p>", {"type": "text/html"}); "";)");
			check_eq(std::string("<p>hi</p>"), transport->seen[2].body);
			check_eq(std::string("text/html"), transport->header(2, "Content-Type"));
			text(e, R"(spider::post("http://h/", "quoted", {"type": "json"}); "";)");
			check_eq(std::string("\"quoted\""), transport->seen[3].body);
			text(e, R"(spider::post("http://h/", "x", {"headers": {"Content-Type": "application/x-custom"}}); "";)");
			check_eq(std::string("application/x-custom"), transport->header(4, "Content-Type"));
		});

		test("options_set_the_method_headers_and_timeout", [this, text]() {
			auto transport = std::make_shared<scripted_transport>();
			auto e = spider_engine(transport);
			text(e, R"(spider::get("http://h/a", {"method": "delete", "headers": {"Authorization": "Bearer t", "X-N": 4}, "timeout": 2.5}); "";)");
			check_eq(std::string("DELETE"), transport->seen[0].method);
			check_eq(std::string("Bearer t"), transport->header(0, "Authorization"));
			check_eq(std::string("4"), transport->header(0, "X-N"));
			check(transport->seen[0].timeout_seconds > 2.0 && transport->seen[0].timeout_seconds <= 2.5, "the timeout option reaches the transport");
			text(e, R"(spider::post("http://h/a", {"v": 1}, {"method": "PUT"}); spider::sling("p", "http://h/a", {"method": "PATCH"}); spider::get("http://h/a", {"method": "HEAD"}); "";)");
			check_eq(std::string("PUT"), transport->seen[1].method);
			check_eq(std::string("{\"v\":1}"), transport->seen[1].body);
			check_eq(std::string("PATCH"), transport->seen[2].method);
			check_eq(std::string("p"), transport->seen[2].body);
			check_eq(std::string("HEAD"), transport->seen[3].method);
		});

		test("bad_options_and_urls_are_refused_before_anything_is_sent", [this, caught]() {
			auto transport = std::make_shared<scripted_transport>();
			auto e = spider_engine(transport);
			check_eq(std::string("spider::get: unknown option 'retries' (method, headers, timeout, type, redirects)"), caught(e, R"(spider::get("http://h/", {"retries": 3});)"));
			check_eq(std::string("spider::get: option 'method' takes GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS"), caught(e, R"(spider::get("http://h/", {"method": "BREW"});)"));
			check_eq(std::string("spider::post: option 'timeout' takes a number of seconds above 0"), caught(e, R"(spider::post("http://h/", "x", {"timeout": 0});)"));
			check_eq(std::string("spider::get: option 'redirects' takes an int from 0 to 100"), caught(e, R"(spider::get("http://h/", {"redirects": -1});)"));
			check_eq(std::string("spider::get: header 'X-A' holds a line break or NUL in its value"), caught(e, "spider::get(\"http://h/\", {\"headers\": {\"X-A\": \"a\\r\\nInjected: 1\"}});"));
			check_eq(std::string("spider::get: 'X A' is not a header name"), caught(e, R"(spider::get("http://h/", {"headers": {"X A": "v"}});)"));
			check_eq(std::string("spider::get: header 'host' is the transport's to set"), caught(e, R"(spider::get("http://h/", {"headers": {"host": "elsewhere"}});)"));
			check_eq(std::string("spider::post: header 'Content-Length' is the transport's to set"), caught(e, R"(spider::post("http://h/", "x", {"headers": {"Content-Length": "99"}});)"));
			check_eq(std::string("spider::post: type \"form\" takes a map of fields"), caught(e, R"(spider::post("http://h/", "x", {"type": "form"});)"));
			check_eq(std::string("spider::get: 'ftp://h/file' is not an http or https URL"), caught(e, R"(spider::get("ftp://h/file");)"));
			check_eq(std::string("spider::get: '/relative' is not an http or https URL"), caught(e, R"(spider::get("/relative");)"));
			check_eq(size_t{0}, transport->seen.size());
		});

		test("redirects_are_followed_up_to_five_and_the_sixth_is_an_error", [this, text, caught]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->answer = [](const http_request& a_request) {
				const size_t hop = static_cast<size_t>(a_request.url.back() - '0');
				if (a_request.url.find("/hop") != std::string::npos && hop < 9) { return reply(302, {{"Location", "/hop" + std::to_string(hop + 1)}}); }
				return reply(200, {}, "landed " + a_request.url);
			};
			auto e = spider_engine(transport);
			check_eq(std::string("landed http://h/hop9"), text(e, R"(spider::get("http://h/hop4").body;)"));
			check_eq(size_t{6}, transport->seen.size());
			transport->seen.clear();
			check_eq(std::string("spider::get: http://h/hop3: more than 5 redirects"), caught(e, R"(spider::get("http://h/hop3");)"));
			check_eq(size_t{6}, transport->seen.size());
			check_eq(std::string("landed http://h/hop9"), text(e, R"(spider::get("http://h/hop0", {"redirects": 9}).body;)"));
			check_eq(std::string("302 /hop1"), text(e, R"(auto r = spider::get("http://h/hop0", {"redirects": 0}); `${r.status} ${r.headers["location"]}`;)"));
		});

		test("a_redirect_resolves_its_location_and_changes_the_method_as_browsers_do", [this, text]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->replies = {reply(303, {{"Location", "../done?ok=1"}}), reply(200, {}, "after 303")};
			auto e = spider_engine(transport);
			check_eq(std::string("after 303 http://h/done?ok=1"), text(e, R"(auto r = spider::post("http://h/a/form", "x"); r.body + " " + r.url;)"));
			check_eq(std::string("GET"), transport->seen[1].method);
			check_eq(std::string(""), transport->seen[1].body);
			check_eq(std::string("<none>"), transport->header(1, "Content-Type"));
			transport->seen.clear();
			transport->replies = {reply(307, {{"Location", "http://h/b"}}), reply(200)};
			text(e, R"(spider::post("http://h/a", "keep"); "";)");
			check_eq(std::string("POST"), transport->seen[1].method);
			check_eq(std::string("keep"), transport->seen[1].body);
			transport->seen.clear();
			transport->replies = {reply(301, {{"Location", "//other.example/x"}}), reply(200)};
			text(e, R"(spider::get("https://h/a", {"headers": {"Authorization": "secret", "X-Keep": "1"}}); "";)");
			check_eq(std::string("https://other.example/x"), transport->seen[1].url);
			check_eq(std::string("<none>"), transport->header(1, "Authorization"));
			check_eq(std::string("1"), transport->header(1, "X-Keep"));
		});

		test("a_redirect_from_https_to_http_is_refused", [this, caught]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->replies = {reply(302, {{"Location", "http://h/plain"}})};
			auto e = spider_engine(transport);
			check_eq(std::string("spider::get: https://h/secure: a redirect from https to http (http://h/plain) is refused"), caught(e, R"(spider::get("https://h/secure");)"));
		});

		test("transport_failures_raise_their_reason_with_the_url", [this, caught]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->answer = [](const http_request& a_request) {
				if (a_request.url.find("slow") != std::string::npos) {
					http_reply result;
					result.error = "timed out";
					return result;
				}
				if (a_request.url.find("throws") != std::string::npos) { throw std::runtime_error("the stack fell over"); }
				http_reply result;
				result.error = "cannot connect to h:81";
				return result;
			};
			auto e = spider_engine(transport);
			check_eq(std::string("spider::get: http://h:81/: cannot connect to h:81"), caught(e, R"(spider::get("http://h:81/");)"));
			check_eq(std::string("spider::post: http://h/slow: timed out after 0.25 s"), caught(e, R"(spider::post("http://h/slow", "x", {"timeout": 0.25});)"));
			check_eq(std::string("spider::get: http://h/throws: the stack fell over"), caught(e, R"(spider::get("http://h/throws");)"));
		});

		test("a_response_is_built_and_edited_by_a_server", [this, text]() {
			auto e = spider_engine(std::make_shared<scripted_transport>());
			check_eq(std::string("404|missing|text/plain|2|ok"), text(e, R"(
				spider::Response r = spider::Response(404, "missing");
				r.header("Content-Type", "text/plain").header("X-A", "1");
				r.header("content-type", "text/plain");
				`${r.status}|${r.body}|${r.header("CONTENT-type")}|${r.headers.size()}|${(r.ok() ? "bad" : "ok")}`;
			)"));
			check_eq(std::string("2 v 1"), text(e, R"(
				spider::Response r;
				r.headers["X-B"] = "v";
				r.headers["X-C"] = "w";
				spider::Response copy = r;
				copy.headers["X-D"] = "x";
				`${r.headers.size()} ${r.header("x-b")} ${(copy.headers.size() - r.headers.size())}`;
			)"));
			check_eq(std::string("200 0 true"), text(e, "spider::Response r; `${r.status} ${r.body.size()} ${r.ok()}`;"));
		});

		test("set_cookie_writes_one_line_per_cookie_with_the_safe_defaults", [this, text, caught]() {
			auto e = spider_engine(std::make_shared<scripted_transport>());
			check_eq(std::string("flavor=oat%20meal; Path=/; Max-Age=60; HttpOnly; SameSite=Lax|plain=1; Path=/|gone=; Path=/app; Max-Age=0; HttpOnly; SameSite=Lax|"
				"s=1; Path=/; Domain=example.org; Expires=Thu, 01 Jan 1970 00:00:10 GMT; Secure; HttpOnly; SameSite=Strict"), text(e, R"(
				spider::Response r = spider::Response();
				r.set_cookie("flavor", "oat meal", {"max_age": 60});
				r.set_cookie("plain", "1", {"http_only": false, "same_site": ""});
				r.clear_cookie("gone", {"path": "/app"});
				r.set_cookie("s", "1", {"domain": "example.org", "expires": 10, "secure": true, "same_site": "Strict"});
				r.cookies.join("|");
			)"));
			check(caught(e, R"(spider::Response r; r.set_cookie("a", "b", {"maxage": 1});)").find("Response.set_cookie: option 'maxage' is unknown") != std::string::npos, "an unknown cookie option is refused");
			check(caught(e, R"(spider::Response r; r.set_cookie("a", "b", {"path": "/; Domain=evil.example"});)").find("Response.set_cookie: option 'path' holds a ';' or a control character") != std::string::npos,
				"an option holding ';' cannot add an attribute");
			check(caught(e, R"(spider::Response r; r.set_cookie("a=b; x", "c");)").find("Response.set_cookie: 'a=b; x' is not a cookie name") != std::string::npos, "a cookie name is a token");
			check(caught(e, R"(spider::Response r; r.header("X-A", "a\nB: c");)").find("Response.header: 'X-A' is not a header name, or its value holds a line break or NUL") != std::string::npos,
				"a header value with a line break is refused");
		});

		test("the_checker_knows_the_signatures", [this]() {
			auto e = spider_engine(std::make_shared<scripted_transport>());
			auto report = e->check("spider::Response r = spider::get(\"http://h/\"); int s = r.status; spider::Response p = spider::post(\"http://h/\", {\"a\": 1}, {\"timeout\": 1}); spider::Response q = spider::sling(\"x\", \"http://h/\");");
			check(report.diagnostics.empty(), "typed uses of the client check clean");
			auto bad = e->check("spider::get();");
			check(!bad.diagnostics.empty(), "a call with no URL is a check error");
		});

		test("catch_is_get_by_its_idiomatic_name", [this, text, caught]() {
			auto transport = std::make_shared<scripted_transport>();
			transport->replies.push_back(reply(200, {}, "caught"));
			auto e = spider_engine(transport);
			check_eq(std::string("caught"), text(e, "spider::catch(\"http://h/fly\").body;"));
			check_eq(std::string("GET"), transport->seen[0].method);
			check_eq(std::string("spider::catch: unknown option 'x' (method, headers, timeout, type, redirects)"), caught(e, "spider::catch(\"http://h/\", {\"x\": 1});"));
		});
	}
};

}

using spider_tests = jai::foundry::tests::spider_tests;
FOUNDRY_REGISTER(spider_tests)
