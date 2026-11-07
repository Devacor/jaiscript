// The spider:: client against a live jai_spider serving tests/app: `jaiscript client.jai http://127.0.0.1:PORT`.
// Prints "ok <check>" or "FAIL <check>: <detail>" per check, then "client: N passed, M failed".
string base = ARGS[0];
int passed = 0;
int failed = 0;

void expect(string name, bool held, var detail = "") {
	if (held) {
		passed += 1;
		print("ok   " + name);
	} else {
		failed += 1;
		print("FAIL " + name + ": " + to_string(detail));
	}
}

string error_of(function call) {
	try {
		call();
	} catch (e) {
		return to_string(e);
	}
	return "";
}

spider::Response r = spider::get(base + "/client/echo?a=1");
var echo = r.json();
expect("get answers 200 and ok()", r.status == 200 && r.ok(), r.status);
expect("get sends GET with the query", echo["method"] == "GET" && echo["query"]["a"] == "1", r.body);
expect("a default User-Agent names the spider", echo["agent"].starts_with("jaiscript-spider/"), r.body);
expect("headers come back lowercased", r.headers["content-type"] == "application/json" && r.header("Content-Type") == "application/json", r.headers);
expect("the response knows its URL", r.url == base + "/client/echo?a=1", r.url);

spider::Response posted = spider::post(base + "/client/echo", "hello");
spider::Response slung = spider::sling("hello", base + "/client/echo");
expect("post(url, content) and sling(content, url) send the same request", posted.body == slung.body && posted.json()["method"] == "POST" && posted.json()["body"] == "hello", posted.body + " | " + slung.body);
expect("text content is text/plain", posted.json()["type"] == "text/plain; charset=utf-8", posted.body);

var as_json = spider::post(base + "/client/echo", {"n": 1, "list": [1, 2]}).json();
expect("other content is JSON", as_json["type"] == "application/json" && from_json(as_json["body"])["list"].size() == 2, as_json);
var as_form = spider::post(base + "/client/echo", {"name": "Ada L", "msg": "a&b"}, {"type": "form"}).json();
expect("type form encodes a form", as_form["type"] == "application/x-www-form-urlencoded" && as_form["body"] == "msg=a%26b&name=Ada+L", as_form);

expect("option method PUT", spider::post(base + "/client/echo", "p", {"method": "PUT"}).json()["method"] == "PUT");
expect("option method DELETE", spider::get(base + "/client/echo", {"method": "DELETE"}).json()["method"] == "DELETE");
expect("option method PATCH", spider::sling("p", base + "/client/echo", {"method": "PATCH"}).json()["method"] == "PATCH");
spider::Response head = spider::get(base + "/", {"method": "HEAD"});
string page_length = to_string(spider::get(base + "/").body.size());
expect("option method HEAD has no body", head.status == 200 && head.body == "" && head.headers["content-length"] == page_length, to_json(head.headers));
expect("option headers reach the server", spider::get(base + "/client/echo", {"headers": {"X-Spider": "silk"}}).json()["x"] == "silk");

spider::Response missing = spider::get(base + "/nowhere");
expect("an error status is a response", missing.status == 404 && !missing.ok() && missing.body == "Not Found", missing.status);
expect("a status the handler chose arrives", spider::get(base + "/status/418").status == 418);

spider::Response hopped = spider::get(base + "/client/hop/5");
expect("five redirects are followed", hopped.body == "landed" && hopped.url == base + "/client/hop/0", hopped.url);
string too_many = error_of(() => spider::get(base + "/client/hop/6"));
expect("a sixth redirect is an error", too_many == "spider::get: " + base + "/client/hop/6: more than 5 redirects", too_many);
expect("redirects 9 follows nine", spider::get(base + "/client/hop/9", {"redirects": 9}).body == "landed");
spider::Response unfollowed = spider::get(base + "/client/hop/1", {"redirects": 0});
expect("redirects 0 answers the redirect", unfollowed.status == 302 && unfollowed.headers["location"] == "/client/hop/0", unfollowed.headers);
var after_post = spider::post(base + "/client/form", "x").json();
expect("a 303 after a POST is followed with a GET", after_post["method"] == "GET" && after_post["query"]["after"] == "post" && after_post["body"] == "", after_post);
var kept = spider::post(base + "/client/keep", "keep me").json();
expect("a 307 resends the method and body", kept["method"] == "POST" && kept["body"] == "keep me", kept);

string slow = error_of(() => spider::get(base + "/client/slow", {"timeout": 0.3}));
expect("a timeout is an error naming it", slow == "spider::get: " + base + "/client/slow: timed out after 0.3 s", slow);
expect("a generous timeout waits", spider::get(base + "/client/slow", {"timeout": 600}).body == "slow");
string refused = error_of(() => spider::get("http://127.0.0.1:1/"));
expect("a refused connection is an error with the URL", refused.starts_with("spider::get: http://127.0.0.1:1/: "), refused);
string framing = error_of(() => spider::get(base + "/", {"headers": {"Host": "elsewhere"}}));
expect("the transport's own headers are refused", framing == "spider::get: header 'Host' is the transport's to set", framing);
string broken = error_of(() => spider::get(base + "/", {"headers": {"X-A": "b\r\nX-B: c"}}));
expect("a header value with a line break is refused", broken == "spider::get: header 'X-A' holds a line break or NUL in its value", broken);
string unknown = error_of(() => spider::get(base + "/", {"retries": 2}));
expect("an unknown option is refused", unknown == "spider::get: unknown option 'retries' (method, headers, timeout, type, redirects)", unknown);

spider::Response cookies = spider::get(base + "/cookies");
expect("Set-Cookie lines arrive in cookies", cookies.cookies.size() == 2 && cookies.cookies[0].starts_with("flavor=oat%20meal"), cookies.cookies);
spider::Response written = spider::get(base + "/client/fetch-self-headers");
expect("a server's header() and headers[] both reach the wire", written.headers["x-twice"] == "a" && written.headers["x-map"] == "written through the map", written.headers);
spider::Response big = spider::get(base + "/big.bin");
expect("a large body arrives whole", big.status == 200 && big.body.size() == 256 * 8192, big.body.size());

print("client: " + to_string(passed) + " passed, " + to_string(failed) + " failed");
