#include <jaiscript/stdlib/spider.hpp>
#include <jaiscript/stdlib/json.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/http_transport.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/web_kernels.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/jaiscript.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jai {
namespace stdlib {
namespace spider_detail {

	using function_result = checked_result<script_value>;
	using web_text_detail::url_record;

	char ascii_lower(char a_c) { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c - 'A' + 'a') : a_c; }
	char ascii_upper(char a_c) { return a_c >= 'a' && a_c <= 'z' ? static_cast<char>(a_c - 'a' + 'A') : a_c; }

	std::string lowercase(std::string_view a_text) {
		std::string result(a_text);
		for (char& c : result) { c = ascii_lower(c); }
		return result;
	}

	bool same_name(std::string_view a_left, std::string_view a_right) {
		if (a_left.size() != a_right.size()) { return false; }
		for (size_t i = 0; i < a_left.size(); ++i) {
			if (ascii_lower(a_left[i]) != ascii_lower(a_right[i])) { return false; }
		}
		return true;
	}

	// RFC 7231 7.1.1.1 IMF-fixdate, the form Set-Cookie's Expires takes
	std::string http_date_text(std::time_t a_when) {
		std::tm parts {};
#ifdef _WIN32
		gmtime_s(&parts, &a_when);
#else
		gmtime_r(&a_when, &parts);
#endif
		static const char* const k_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
		static const char* const k_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		char text[40];
		std::snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d GMT", k_days[parts.tm_wday % 7], parts.tm_mday,
			k_months[parts.tm_mon % 12], parts.tm_year + 1900, parts.tm_hour, parts.tm_min, parts.tm_sec);
		return text;
	}

	// spider::Response: one HTTP answer, the one a client call returns and the one a server handler builds. headers is a
	// map<string, string> held as a script value, so r.headers["X"] = v writes the response's own map
	struct response_record {
		script_int status = 200;
		script_value headers {std::monostate{}, nullptr};
		script_value cookies {std::monostate{}, nullptr};
		std::string body;
		std::string url;

		response_record() = default;
		response_record(const response_record& a_other) :
			status(a_other.status),
			headers(a_other.headers.is_map() ? a_other.headers.clone() : a_other.headers),
			cookies(a_other.cookies.is_array() ? a_other.cookies.clone() : a_other.cookies),
			body(a_other.body),
			url(a_other.url) {
		}
		response_record& operator=(const response_record& a_other) {
			if (this != &a_other) {
				status = a_other.status;
				headers = a_other.headers.is_map() ? a_other.headers.clone() : a_other.headers;
				cookies = a_other.cookies.is_array() ? a_other.cookies.clone() : a_other.cookies;
				body = a_other.body;
				url = a_other.url;
			}
			return *this;
		}

		script_value& header_map(engine* a_engine) {
			if (!headers.is_map()) { headers = script_value::make_map(detail::engine_access::get_type_info_string(*a_engine), detail::engine_access::get_type_info_string(*a_engine), a_engine); }
			return headers;
		}

		script_value& cookie_list(engine* a_engine) {
			if (!cookies.is_array()) { cookies = script_value::make_array(detail::engine_access::get_type_info_string(*a_engine), a_engine); }
			return cookies;
		}

		void add_cookie(engine* a_engine, std::string a_line) {
			cookie_list(a_engine).unchecked_get_array_storage()->push(script_value(std::move(a_line), a_engine));
		}

		std::string header(engine* a_engine, std::string_view a_name) {
			for (const auto& [key, value] : header_map(a_engine).as_map()) {
				const std::string* name = key.text_bytes();
				const std::string* text = value.deref().text_bytes();
				if (name && text && same_name(*name, a_name)) { return *text; }
			}
			return {};
		}

		void set_header(engine* a_engine, const std::string& a_name, const std::string& a_value) {
			auto& fields = header_map(a_engine).as_map();
			for (auto it = fields.begin(); it != fields.end(); ++it) {
				const std::string* name = it->first.text_bytes();
				if (name && *name != a_name && same_name(*name, a_name)) {
					fields.erase(it);
					break;
				}
			}
			fields[script_value(a_name, a_engine)] = script_value(a_value, a_engine);
		}
	};

	JAI_NOINLINE function_result raise(engine* a_engine, runtime_error_code a_code, const std::string& a_text) {
		return function_result(make_error_code(a_code), "{0}", detail::engine_access::park_error_text(*a_engine, a_text));
	}

	bool is_token(std::string_view a_text) {
		return !a_text.empty() && std::all_of(a_text.begin(), a_text.end(), [](char a_c) {
			return (a_c >= '0' && a_c <= '9') || (a_c >= 'a' && a_c <= 'z') || (a_c >= 'A' && a_c <= 'Z') || std::string_view("!#$%&'*+-.^_`|~").find(a_c) != std::string_view::npos;
		});
	}

	// An attribute value (RFC 6265 4.1.1 av-octet): visible ASCII and space, never ';'
	bool is_cookie_attribute(std::string_view a_text) {
		return std::all_of(a_text.begin(), a_text.end(), [](char a_c) { return a_c >= ' ' && a_c <= '~' && a_c != ';'; });
	}

	// The Set-Cookie line for name=value (RFC 6265 4.1): the value percent-encoded; options max_age, expires (unix seconds),
	// path ("/"), domain, secure (false), http_only (true), same_site ("Lax"; "" leaves it out)
	std::string cookie_line(engine* a_engine, const std::string& a_name, const std::string& a_value, const script_value& a_options) {
		if (!is_token(a_name)) { throw runtime_error("Response.set_cookie: '" + a_name + "' is not a cookie name (letters, digits and !#$%&'*+-.^_`|~)"); }
		std::string result = a_name + "=";
		web_text_detail::append_percent_encoded(a_value, false, result);
		std::string path = "/";
		std::string domain;
		std::string max_age;
		std::string expires;
		std::string same_site = "Lax";
		bool secure = false;
		bool http_only = true;
		if (a_options.is_map()) {
			for (const auto& [key, raw] : a_options.as_map()) {
				const std::string* name = key.text_bytes();
				const script_value& value = raw.deref();
				std::string text;
				const bool spelled = web_text_detail::query_value_text(value, text, a_engine);
				if (name && *name == "path" && spelled) {
					path = text;
				} else if (name && *name == "domain" && spelled) {
					domain = text;
				} else if (name && *name == "max_age" && value.is_int()) {
					max_age = std::to_string(value.as_int());
				} else if (name && *name == "expires" && value.is_int()) {
					expires = http_date_text(static_cast<std::time_t>(value.as_int()));
				} else if (name && *name == "same_site" && spelled) {
					same_site = text;
				} else if (name && *name == "secure" && value.is_bool()) {
					secure = value.as_bool();
				} else if (name && *name == "http_only" && value.is_bool()) {
					http_only = value.as_bool();
				} else {
					throw runtime_error("Response.set_cookie: option '" + (name ? *name : std::string("?")) + "' is unknown or of the wrong kind (max_age, expires: int; path, domain, same_site: string; secure, http_only: bool)");
				}
			}
		}
		for (const auto& [option, text] : {std::pair<const char*, const std::string*>{"path", &path}, {"domain", &domain}, {"same_site", &same_site}}) {
			if (!is_cookie_attribute(*text)) { throw runtime_error(std::string("Response.set_cookie: option '") + option + "' holds a ';' or a control character"); }
		}
		result += "; Path=" + path;
		if (!domain.empty()) { result += "; Domain=" + domain; }
		if (!max_age.empty()) { result += "; Max-Age=" + max_age; }
		if (!expires.empty()) { result += "; Expires=" + expires; }
		if (secure) { result += "; Secure"; }
		if (http_only) { result += "; HttpOnly"; }
		if (!same_site.empty()) { result += "; SameSite=" + same_site; }
		return result;
	}

	// The options a client call takes, read once before anything is sent
	struct call_options {
		std::string method;
		std::vector<std::pair<std::string, std::string>> headers;
		double timeout_seconds = 30.0;
		std::string type;
		int redirects = 5;
	};

	checked_result<void> read_options(engine* a_engine, std::string_view a_function, const script_value& a_options, call_options& a_out) {
		auto refuse = [&](const std::string& a_what) {
			return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "{0}", detail::engine_access::park_error_text(*a_engine, std::string(a_function) + ": " + a_what));
		};
		if (a_options.is_null()) { return {}; }
		if (!a_options.is_map()) { return refuse("the options are a map (method, headers, timeout, type, redirects)"); }
		for (const auto& [key, raw] : a_options.as_map()) {
			const std::string* name = key.text_bytes();
			const script_value& value = raw.deref();
			if (!name) { return refuse("an option name is not text"); }
			if (*name == "method") {
				static constexpr std::string_view k_methods[] = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
				std::string method = value.is_text() ? value.unchecked_text() : std::string();
				for (char& c : method) { c = ascii_upper(c); }
				if (std::find(std::begin(k_methods), std::end(k_methods), method) == std::end(k_methods)) {
					return refuse("option 'method' takes GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS");
				}
				a_out.method = method;
			} else if (*name == "headers") {
				if (!value.is_map()) { return refuse("option 'headers' takes a map of header names to values"); }
				for (const auto& [header_key, header_raw] : value.as_map()) {
					std::string header_name;
					std::string header_value;
					if (!web_text_detail::query_value_text(header_key, header_name, a_engine) || header_name.empty() ||
						!web_text_detail::query_value_text(header_raw.deref(), header_value, a_engine)) {
						return refuse("option 'headers' takes text names and text, number or bool values");
					}
					if (!is_token(header_name)) { return refuse("'" + header_name + "' is not a header name"); }
					if (header_value.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) { return refuse("header '" + header_name + "' holds a line break or NUL in its value"); }
					for (std::string_view framing : {"host", "content-length", "transfer-encoding", "connection"}) {
						if (same_name(header_name, framing)) { return refuse("header '" + header_name + "' is the transport's to set"); }
					}
					a_out.headers.emplace_back(std::move(header_name), std::move(header_value));
				}
			} else if (*name == "timeout") {
				const double seconds = value.is_int() ? static_cast<double>(value.as_int()) : value.is_float() ? value.as_float() : -1.0;
				if (!(seconds > 0.0)) { return refuse("option 'timeout' takes a number of seconds above 0"); }
				a_out.timeout_seconds = seconds;
			} else if (*name == "type") {
				if (!value.is_text() || value.unchecked_text().empty()) { return refuse("option 'type' takes \"json\", \"form\", \"text\" or a media type"); }
				a_out.type = value.unchecked_text();
			} else if (*name == "redirects") {
				if (!value.is_int() || value.as_int() < 0 || value.as_int() > 100) { return refuse("option 'redirects' takes an int from 0 to 100"); }
				a_out.redirects = static_cast<int>(value.as_int());
			} else {
				return refuse("unknown option '" + *name + "' (method, headers, timeout, type, redirects)");
			}
		}
		return {};
	}

	bool has_header(const std::vector<std::pair<std::string, std::string>>& a_headers, std::string_view a_name) {
		for (const auto& [name, value] : a_headers) {
			if (same_name(name, a_name)) { return true; }
		}
		return false;
	}

	void drop_header(std::vector<std::pair<std::string, std::string>>& a_headers, std::string_view a_name) {
		a_headers.erase(std::remove_if(a_headers.begin(), a_headers.end(), [&](const auto& a_header) { return same_name(a_header.first, a_name); }), a_headers.end());
	}

	std::string json_text(engine* a_engine, const script_value& a_value) {
		serialization::json_archive_writer writer(-1, a_engine);
		writer.write_value(a_value);
		return writer.str();
	}

	// The body and its Content-Type: text as it is (text/plain), anything else as JSON, a map as a form with type "form"
	checked_result<void> encode_content(engine* a_engine, std::string_view a_function, const script_value& a_content, const std::string& a_type, std::string& a_body, std::string& a_content_type) {
		auto refuse = [&](const std::string& a_what) {
			return checked_result<void>(make_error_code(runtime_error_code::type_mismatch), "{0}", detail::engine_access::park_error_text(*a_engine, std::string(a_function) + ": " + a_what));
		};
		const script_value& content = a_content.deref();
		if (a_type == "form") {
			if (!content.is_map()) { return refuse("type \"form\" takes a map of fields"); }
			for (const auto& [key, raw] : content.as_map()) {
				std::string name;
				std::string value;
				if (!web_text_detail::query_value_text(key, name, a_engine)) { return refuse("a form field name is not text, a number or a bool"); }
				const script_value& item = raw.deref();
				if (item.is_null()) { continue; }
				if (!web_text_detail::query_value_text(item, value, a_engine)) { return refuse("form field '" + name + "' is not text, a number or a bool"); }
				if (!a_body.empty()) { a_body += '&'; }
				web_text_detail::append_percent_encoded(name, true, a_body);
				a_body += '=';
				web_text_detail::append_percent_encoded(value, true, a_body);
			}
			a_content_type = "application/x-www-form-urlencoded";
			return {};
		}
		if (a_type == "json" || (a_type.empty() && !content.is_text())) {
			a_body = json_text(a_engine, content);
			a_content_type = "application/json";
			return {};
		}
		if (!content.is_text()) { return refuse("type \"" + a_type + "\" sends text; pass a string, or use type \"json\" or \"form\""); }
		a_body = content.unchecked_text();
		a_content_type = a_type.empty() || a_type == "text" ? "text/plain; charset=utf-8" : a_type;
		return {};
	}

	// RFC 3986 5.2.4
	std::string without_dot_segments(std::string_view a_path) {
		std::vector<std::string_view> kept;
		size_t start = 0;
		const bool absolute = !a_path.empty() && a_path.front() == '/';
		if (absolute) { start = 1; }
		bool trailing_slash = false;
		while (start <= a_path.size()) {
			size_t end = a_path.find('/', start);
			if (end == std::string_view::npos) { end = a_path.size(); }
			const std::string_view segment = a_path.substr(start, end - start);
			trailing_slash = end == a_path.size() && (segment == "." || segment == "..");
			if (segment == "..") {
				if (!kept.empty()) { kept.pop_back(); }
			} else if (segment != ".") {
				kept.push_back(segment);
			}
			start = end + 1;
		}
		std::string result = absolute ? "/" : "";
		for (size_t i = 0; i < kept.size(); ++i) {
			if (i) { result += '/'; }
			result += kept[i];
		}
		if (trailing_slash && !result.empty() && result.back() != '/') { result += '/'; }
		return result;
	}

	// a Location against the URL that answered with it (RFC 3986 5.2.2); false when the result is no http(s) URL
	bool resolve_location(const url_record& a_base, const std::string& a_location, url_record& a_out) {
		url_record reference;
		if (!web_text_detail::split_url(a_location, reference)) { return false; }
		if (!reference.scheme.empty()) {
			a_out = reference;
			a_out.scheme = lowercase(a_out.scheme);
		} else if (a_location.rfind("//", 0) == 0) {
			a_out = reference;
			a_out.scheme = a_base.scheme;
		} else {
			a_out = a_base;
			a_out.fragment.clear();
			if (reference.path.empty()) {
				if (!reference.query.empty()) { a_out.query = reference.query; }
			} else {
				a_out.query = reference.query;
				if (reference.path.front() == '/') {
					a_out.path = reference.path;
				} else {
					const size_t slash = a_base.path.rfind('/');
					a_out.path = (slash == std::string::npos ? std::string("/") : a_base.path.substr(0, slash + 1)) + reference.path;
				}
			}
		}
		a_out.path = without_dot_segments(a_out.path.empty() ? "/" : a_out.path);
		a_out.fragment.clear();
		return (a_out.scheme == "http" || a_out.scheme == "https") && !a_out.host.empty();
	}

	bool same_origin(const url_record& a_left, const url_record& a_right) {
		return a_left.scheme == a_right.scheme && lowercase(a_left.host) == lowercase(a_right.host) && a_left.port == a_right.port;
	}

	std::string seconds_text(double a_seconds) {
		char text[32];
		std::snprintf(text, sizeof(text), "%g", a_seconds);
		return text;
	}

	// One client call: the options read, the request sent through the host's transport, redirects followed by the policy
	// (up to options.redirects, 5 by default; 301/302 turn a POST into a GET, 303 turns anything but HEAD into a GET,
	// 307/308 resend as they were; never from https to http; Authorization and Cookie stay with the origin they were
	// given for), and the answer as a spider::Response
	function_result exchange(engine* a_engine, std::string_view a_function, const script_value& a_url, const script_value* a_content, const script_value& a_options) {
		const std::string function(a_function);
		if (!a_url.deref().is_text()) { return raise(a_engine, runtime_error_code::type_mismatch, function + ": the URL is a string"); }
		const std::string url_text = a_url.deref().unchecked_text();
		call_options options;
		options.method = a_content ? "POST" : "GET";
		if (auto read = read_options(a_engine, a_function, a_options.deref(), options); !read) { return read.error_value(); }
		const std::shared_ptr<http_transport>& transport = a_engine->http_transport();
		if (!transport) {
			return raise(a_engine, runtime_error_code::invalid_operation, function + ": no HTTP transport installed: the host provides one with engine::http_transport()");
		}
		url_record target;
		if (!web_text_detail::split_url(url_text, target) || (lowercase(target.scheme) != "http" && lowercase(target.scheme) != "https") || target.host.empty()) {
			return raise(a_engine, runtime_error_code::invalid_operation, function + ": '" + url_text + "' is not an http or https URL");
		}
		target.scheme = lowercase(target.scheme);
		target.fragment.clear();
		if (target.path.empty()) { target.path = "/"; }

		http_request request;
		request.method = options.method;
		request.headers = options.headers;
		if (a_content) {
			std::string content_type;
			if (auto encoded = encode_content(a_engine, a_function, *a_content, options.type, request.body, content_type); !encoded) { return encoded.error_value(); }
			if (!has_header(request.headers, "content-type")) { request.headers.emplace_back("Content-Type", content_type); }
		}
		if (!has_header(request.headers, "user-agent")) { request.headers.emplace_back("User-Agent", "jaiscript-spider/" + jai::version()); }
		if (!has_header(request.headers, "accept")) { request.headers.emplace_back("Accept", "*/*"); }

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(options.timeout_seconds));
		int redirects_followed = 0;
		http_reply reply;
		while (true) {
			request.url = target.to_string();
			const double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
			if (left <= 0.0) { return raise(a_engine, runtime_error_code::invalid_operation, function + ": " + request.url + ": timed out after " + seconds_text(options.timeout_seconds) + " s"); }
			request.timeout_seconds = left;
			try {
				reply = transport->exchange(request);
			} catch (const std::exception& e) {
				reply = http_reply{};
				reply.error = e.what();
			}
			if (!reply.error.empty()) {
				const std::string reason = reply.error == "timed out" ? "timed out after " + seconds_text(options.timeout_seconds) + " s" : reply.error;
				return raise(a_engine, runtime_error_code::invalid_operation, function + ": " + request.url + ": " + reason);
			}
			const int status = reply.status;
			const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
			std::string location;
			for (const auto& [name, value] : reply.headers) {
				if (same_name(name, "location")) { location = value; }
			}
			if (!redirect || location.empty() || options.redirects == 0) { break; }
			if (redirects_followed == options.redirects) {
				return raise(a_engine, runtime_error_code::invalid_operation, function + ": " + url_text + ": more than " + std::to_string(options.redirects) + " redirects");
			}
			url_record next;
			if (!resolve_location(target, location, next)) {
				return raise(a_engine, runtime_error_code::invalid_operation, function + ": " + request.url + ": redirected to '" + location + "', which is not an http or https URL");
			}
			if (target.scheme == "https" && next.scheme == "http") {
				return raise(a_engine, runtime_error_code::invalid_operation, function + ": " + request.url + ": a redirect from https to http (" + next.to_string() + ") is refused");
			}
			if ((status == 303 && request.method != "HEAD") || ((status == 301 || status == 302) && request.method == "POST")) {
				request.method = "GET";
				request.body.clear();
				drop_header(request.headers, "content-type");
				drop_header(request.headers, "content-length");
			}
			if (!same_origin(target, next)) {
				drop_header(request.headers, "authorization");
				drop_header(request.headers, "cookie");
			}
			target = next;
			++redirects_followed;
		}

		if (!detail::builtin_memory_charge(a_engine, reply.body.size())) [[unlikely]] { return detail::builtin_memory_cap(a_engine); }
		auto response = std::make_shared<response_record>();
		response->status = reply.status;
		response->url = request.url;
		response->body = std::move(reply.body);
		auto& fields = response->header_map(a_engine).as_map();
		for (auto& [name, value] : reply.headers) {
			std::string key = lowercase(name);
			if (key == "set-cookie") {
				response->add_cookie(a_engine, std::move(value));
				continue;
			}
			script_value key_value(key, a_engine);
			auto it = fields.find(key_value);
			if (it == fields.end()) {
				fields[key_value] = script_value(std::move(value), a_engine);
			} else {
				it->second = script_value(it->second.deref().unchecked_text() + ", " + value, a_engine);
			}
		}
		return detail::engine_access::make_value_object(*a_engine, response);
	}

	const script_value& argument_or_null(const std::vector<script_value>& a_args, size_t a_index, const script_value& a_null) {
		return a_index < a_args.size() ? a_args[a_index] : a_null;
	}

} // namespace spider_detail

	void register_spider(engine& eng_ref) {
		using namespace spider_detail;
		using svt = script_value_type;
		engine* eng = &eng_ref;
		eng_ref.register_privilege("network");

		dynamic_binder<response_record>(eng_ref, "spider::Response")
			.constructor<>()
			.property("status", &response_record::status)
			.property("headers", [eng](response_record& a_self) -> script_value { return a_self.header_map(eng); },
				[eng](response_record& a_self, const script_value& a_value) {
					script_value& fields = a_self.header_map(eng);
					fields.as_map().clear();
					if (!a_value.deref().is_map()) { return; }
					for (const auto& [key, value] : a_value.deref().as_map()) {
						std::string name;
						std::string text;
						if (web_text_detail::query_value_text(key, name, eng) && web_text_detail::query_value_text(value.deref(), text, eng)) { a_self.set_header(eng, name, text); }
					}
				})
			.property("cookies", [eng](response_record& a_self) -> script_value { return a_self.cookie_list(eng); },
				[eng](response_record& a_self, const script_value& a_value) {
					a_self.cookie_list(eng).unchecked_get_array_storage()->clear();
					if (!a_value.deref().is_array()) { return; }
					a_value.deref().unchecked_array_node()->for_each(eng, [&](const script_value& a_line) {
						if (const std::string* line = a_line.deref().text_bytes()) { a_self.add_cookie(eng, *line); }
					});
				})
			.property("body", &response_record::body)
			.property("url", &response_record::url)
			.method("ok", [](const response_record& a_self) { return a_self.status >= 200 && a_self.status < 300; })
			.method("json", [eng](const response_record& a_self) -> script_value {
				detail::engine_access::load_session session(eng);
				serialization::json_archive_reader reader(a_self.body, eng);
				script_value result = load_archive_value(eng, reader.read_value(), true);
				session.complete();
				return result;
			})
			.method("header", [eng](response_record& a_self, const std::string& a_name) { return a_self.header(eng, a_name); })
			.method("header", [eng](response_record& a_self, const std::string& a_name, const std::string& a_value) -> response_record& {
				if (!is_token(a_name) || a_value.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
					throw runtime_error("Response.header: '" + a_name + "' is not a header name, or its value holds a line break or NUL");
				}
				a_self.set_header(eng, a_name, a_value);
				return a_self;
			})
			.method("set_cookie", [eng](response_record& a_self, const std::string& a_name, const std::string& a_value) {
				a_self.add_cookie(eng, cookie_line(eng, a_name, a_value, script_value(std::monostate{}, eng)));
			})
			.method("set_cookie", [eng](response_record& a_self, const std::string& a_name, const std::string& a_value, const script_value& a_options) {
				a_self.add_cookie(eng, cookie_line(eng, a_name, a_value, a_options.deref()));
			})
			.method("clear_cookie", [eng](response_record& a_self, const std::string& a_name) {
				script_value expired = script_value::make_map(nullptr, nullptr, eng);
				expired.as_map()[script_value(std::string("max_age"), eng)] = script_value(script_int{0}, eng);
				a_self.add_cookie(eng, cookie_line(eng, a_name, "", expired));
			})
			.method("clear_cookie", [eng](response_record& a_self, const std::string& a_name, const script_value& a_options) {
				script_value expired = a_options.deref().is_map() ? a_options.deref().clone() : script_value::make_map(nullptr, nullptr, eng);
				expired.as_map()[script_value(std::string("max_age"), eng)] = script_value(script_int{0}, eng);
				a_self.add_cookie(eng, cookie_line(eng, a_name, "", expired));
			})
			.build();

		auto& ns = detail::engine_access::script_namespaces(eng_ref)[detail::engine_access::symbolize(eng_ref, "spider")];
		if (!ns) { ns = std::make_shared<script_namespace_data>(); }
		auto def = [&](const char* a_name, uint8_t a_min, uint8_t a_max, std::vector<svt> a_params, script_function a_call, bool a_network) {
			const uint64_t id = detail::engine_access::symbolize(eng_ref, a_name);
			ns->variables[id] = script_value::make_function(a_network ? detail::engine_access::guard_function(eng_ref, std::move(a_call), "network") : std::move(a_call), &eng_ref);
			host_entry_signature signature;
			signature.min_args = a_min;
			signature.max_args = a_max;
			signature.params = std::move(a_params);
			signature.result = svt::jai_object_type;
			signature.result_class = "spider::Response";
			ns->signatures[id] = std::move(signature);
		};
		auto arity_refusal = [](const char* a_text) { return function_result(make_error_code(runtime_error_code::argument_count_mismatch), a_text); };   // static texts: the result holds a view

		def("Response", 0, 2, {svt::jai_int_type, svt::jai_string_type}, [eng, arity_refusal](const std::vector<script_value>& a_args) -> function_result {
			if (a_args.size() > 2) { return arity_refusal("spider::Response expects 0 to 2 arguments (status, body)"); }
			auto response = std::make_shared<response_record>();
			if (!a_args.empty()) {
				if (!a_args[0].deref().is_int()) { return raise(eng, runtime_error_code::type_mismatch, "spider::Response: the status is an int"); }
				response->status = a_args[0].deref().as_int();
			}
			if (a_args.size() == 2) {
				if (!a_args[1].deref().is_text()) { return raise(eng, runtime_error_code::type_mismatch, "spider::Response: the body is a string"); }
				response->body = a_args[1].deref().unchecked_text();
			}
			response->header_map(eng);
			return detail::engine_access::make_value_object(*eng, response);
		}, false);

		auto fetcher = [eng, arity_refusal](const char* a_name, const char* a_arity_text) {
			return [eng, arity_refusal, a_name, a_arity_text](const std::vector<script_value>& a_args) -> function_result {
				if (a_args.empty() || a_args.size() > 2) { return arity_refusal(a_arity_text); }
				const script_value none(std::monostate{}, eng);
				return exchange(eng, a_name, a_args[0], nullptr, argument_or_null(a_args, 1, none));
			};
		};
		def("catch", 1, 2, {svt::jai_string_type, svt::jai_map_type}, fetcher("spider::catch", "spider::catch expects 1 or 2 arguments (url, options)"), true);
		def("get", 1, 2, {svt::jai_string_type, svt::jai_map_type}, fetcher("spider::get", "spider::get expects 1 or 2 arguments (url, options)"), true);

		def("sling", 2, 3, {svt::jai_any_type, svt::jai_string_type, svt::jai_map_type}, [eng, arity_refusal](const std::vector<script_value>& a_args) -> function_result {
			if (a_args.size() < 2 || a_args.size() > 3) { return arity_refusal("spider::sling expects 2 or 3 arguments (content, url, options)"); }
			const script_value none(std::monostate{}, eng);
			return exchange(eng, "spider::sling", a_args[1], &a_args[0], argument_or_null(a_args, 2, none));
		}, true);
		def("post", 2, 3, {svt::jai_string_type, svt::jai_any_type, svt::jai_map_type}, [eng, arity_refusal](const std::vector<script_value>& a_args) -> function_result {
			if (a_args.size() < 2 || a_args.size() > 3) { return arity_refusal("spider::post expects 2 or 3 arguments (url, content, options)"); }
			const script_value none(std::monostate{}, eng);
			return exchange(eng, "spider::post", a_args[0], &a_args[1], argument_or_null(a_args, 2, none));
		}, true);
	}

} // namespace stdlib
} // namespace jai
