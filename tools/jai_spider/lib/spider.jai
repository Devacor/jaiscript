// jai_spider's script surface, all under spider::: Request, Upload, the response helpers, Router and spin. Response is
// the library's spider::Response, the class spider::catch answers too. The host imports this file before the app and
// calls spider_host_serve once per request; the app hands its handler to spider::spin.
//
// The host functions it stands on: parse_cookies, parse_multipart, read_file and write_file;
// web::parse_query is the web library's, crypto::random_hex the crypto library's.

namespace spider {

class Upload {
	string name;            // the form field
	string filename;        // as the browser sent it: a name to show, never a path to trust
	string content_type;
	string data;

	int size() { return data.size(); }
	bool save(string path) { return write_file(path, data); }
}

class Request {
	string method;
	string target;          // as it arrived: path and query, percent-encoded
	string path;            // decoded, without the query
	string query_string;
	string version;
	string body;
	string remote_addr;
	map<string, string> query;
	map<string, string> headers;      // names lowercased
	map<string, string> cookies;
	map<string, string> params;       // the route's :name and *name captures

	Request() {}
	Request(map<string, var> raw) {
		method = raw["method"];
		target = raw["target"];
		path = raw["path"];
		query_string = raw["query_string"];
		version = raw["version"];
		body = raw["body"];
		remote_addr = raw["remote_addr"];
		headers = raw["headers"];
		query = web::parse_query(query_string);
		cookies = parse_cookies(headers.get("cookie", ""));
	}

	// any spelling of the name: header("Content-Type") reads content-type
	string header(string name) { return headers.get(string::to_lower(name), ""); }
	string cookie(string name) { return cookies.get(name, ""); }
	string param(string name) { return params.get(name, ""); }
	string content_type() { return string::to_lower(header("content-type")); }
	bool is_multipart() { return content_type().starts_with("multipart/form-data"); }

	// the fields of an urlencoded or multipart form body; a file field is in files(), not here
	map<string, string> form() {
		if (!is_multipart()) { return web::parse_query(body); }
		map<string, string> fields;
		array<map<string, var>> parts = parse_multipart(body, header("content-type"));
		for (auto part : parts) {
			if (!part["is_file"]) { fields[part["name"]] = part["data"]; }
		}
		return fields;
	}

	map<string, spider::Upload> files() {
		map<string, spider::Upload> uploads;
		if (!is_multipart()) { return uploads; }
		array<map<string, var>> parts = parse_multipart(body, header("content-type"));
		for (auto part : parts) {
			if (part["is_file"]) {
				spider::Upload u;
				u.name = part["name"];
				u.filename = part["filename"];
				u.content_type = part["content_type"];
				u.data = part["data"];
				uploads[u.name] = u;
			}
		}
		return uploads;
	}

	var json() { return from_json(body); }
}

spider::Response html(string text, int status = 200) {
	spider::Response r = spider::Response(status, text);
	r.header("Content-Type", "text/html; charset=utf-8");
	return r;
}

// a page a template made (web::html), sent as it is
spider::Response html(web::html page, int status = 200) { return spider::html(page.to_string(), status); }

spider::Response plain_text(string text, int status = 200) {
	spider::Response r = spider::Response(status, text);
	r.header("Content-Type", "text/plain; charset=utf-8");
	return r;
}

spider::Response json(var value, int status = 200) {
	spider::Response r = spider::Response(status, to_json(value));
	r.header("Content-Type", "application/json");
	return r;
}

// 303 sends a browser back with GET: the answer to a form POST
spider::Response redirect(string url, int status = 303) {
	spider::Response r = spider::Response(status);
	r.header("Location", url);
	return r;
}

// The files this request's handler named through spider::file, by a random token. The X-Sendfile header carries only
// the token, so a header that arrived from elsewhere (an upstream answer passed through) never names a file.
map<string, string> sent_files;

// a file from disk, relative to the app's folder, streamed by the host; its Content-Type follows the extension unless
// one is given
spider::Response file(string path, string content_type = "") {
	spider::Response r = spider::Response(200);
	string token = crypto::random_hex(16);
	spider::sent_files[token] = path;
	r.header("X-Sendfile", token);
	if (content_type != "") { r.header("Content-Type", content_type); }
	return r;
}

spider::Response not_found(string text = "Not Found") { return spider::plain_text(text, 404); }

// a handler may answer a Response, a string or a web::html (an HTML page), null (204) or any other value (JSON)
spider::Response to_response(var result) {
	if (result == null) { return spider::Response(204); }
	string kind = type_of(result);
	if (kind == "spider::Response") { return result; }
	if (kind == "string" || kind == "utf8_string") { return spider::html(result); }
	if (kind == "web::html") { return spider::html(result.to_string()); }
	return spider::json(result);
}

class Route {
	string method;
	array<string> parts;
	function handler;
}

array<string> split_path(string path) {
	array<string> parts;
	for (auto piece : path.split("/")) {
		if (piece != "") { parts.push(piece); }
	}
	return parts;
}

bool match_parts(array<string> pattern, array<string> parts, map<string, string>& captures) {
	for (int i = 0; i < pattern.size(); i++) {
		string want = pattern[i];
		if (want.starts_with("*")) {
			array<string> rest;
			for (int j = i; j < parts.size(); j++) { rest.push(parts[j]); }
			captures[want.substr(1)] = rest.join("/");
			return true;
		}
		if (i >= parts.size()) { return false; }
		if (want.starts_with(":")) {
			captures[want.substr(1)] = parts[i];
		} else if (want != parts[i]) {
			return false;
		}
	}
	return pattern.size() == parts.size();
}

// Routes match in the order they were added: "/posts/:id" captures one segment as params["id"], "/files/*rest" the
// rest of the path. A HEAD request takes a GET route; a path that matches only other methods answers 405.
class Router {
	array<spider::Route> routes;
	array<function> filters;

	void add(string method, string pattern, function handler) {
		spider::Route r;
		r.method = method;
		r.parts = spider::split_path(pattern);
		r.handler = handler;
		routes.push(r);
	}
	void get(string pattern, function handler) { add("GET", pattern, handler); }
	void post(string pattern, function handler) { add("POST", pattern, handler); }
	void put(string pattern, function handler) { add("PUT", pattern, handler); }
	void patch(string pattern, function handler) { add("PATCH", pattern, handler); }
	void delete(string pattern, function handler) { add("DELETE", pattern, handler); }
	void any(string pattern, function handler) { add("*", pattern, handler); }

	// a filter runs before every route: null lets the request through, anything else is the answer
	void before(function filter) { filters.push(filter); }

	spider::Response dispatch(spider::Request& req) {
		for (auto& filter : filters) {
			var early = filter(req);
			if (early != null) { return spider::to_response(early); }
		}
		array<string> path_parts = spider::split_path(req.path);
		array<string> allowed;
		for (auto& r : routes) {
			map<string, string> captures;
			if (!spider::match_parts(r.parts, path_parts, captures)) { continue; }
			if (r.method == req.method || r.method == "*" || (req.method == "HEAD" && r.method == "GET")) {
				req.params = captures;
				return spider::to_response(r.handler(req));
			}
			allowed.push(r.method);
		}
		if (allowed.size() > 0) {
			spider::Response r = spider::plain_text("Method Not Allowed", 405);
			r.header("Allow", allowed.join(", "));
			return r;
		}
		return spider::not_found();
	}
}

// The app's handler and its options; spin again (a reload re-runs the app's spin) to replace them
function spun;
map<string, var> spun_options;

// handler(Request) answers every request that is not a file under the static root. options: budget (seconds a request
// may run, over --budget), max_body (bytes, over --max-body)
void spin(function handler, map<string, var> options = {}) {
	for (auto [key, value] : options) {
		if (key != "budget" && key != "max_body") { throw "spider::spin: unknown option '" + key + "' (budget, max_body)"; }
	}
	spun = handler;
	spun_options = options;
}

// a Router spins as its dispatch: add the routes first, then spin it
void spin(spider::Router routes, map<string, var> options = {}) {
	spider::spin((spider::Request req) => routes.dispatch(req), options);
}

// the plain names of spin
void serve(function handler, map<string, var> options = {}) { spider::spin(handler, options); }
void serve(spider::Router routes, map<string, var> options = {}) { spider::spin(routes, options); }

}

// the host's entries: the options the app spun with, and one request in, its response's parts out
map<string, var> spider_host_options() { return spider::spun_options; }

map<string, var> spider_host_serve(map<string, var> raw) {
	if (spider::spun == null) { throw "the app never handed a handler to spider::spin(handler)"; }
	spider::sent_files = {};
	spider::Request req = spider::Request(raw);
	spider::Response resp = spider::to_response(spider::spun(req));
	string token = resp.header("X-Sendfile");
	string file = token == "" ? "" : spider::sent_files.get(token, "");
	return {"status": resp.status, "headers": resp.headers, "cookies": resp.cookies, "body": resp.body, "file": file};
}
