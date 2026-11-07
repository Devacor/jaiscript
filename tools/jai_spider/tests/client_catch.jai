// spider::catch, the idiomatic name of spider::get, against a live jai_spider: `jaiscript client_catch.jai http://127.0.0.1:PORT`
string base = ARGS[0];
spider::Response caught = spider::catch(base + "/client/echo?by=catch");
spider::Response got = spider::get(base + "/client/echo?by=catch");
string refused = "";
try {
	spider::catch(base + "/", {"retries": 1});
} catch (e) {
	refused = e;
}
print(caught.status == 200 && caught.body == got.body && refused == "spider::catch: unknown option 'retries' (method, headers, timeout, type, redirects)" ? "catch: ok" : "catch: FAIL " + caught.body + " | " + refused);
