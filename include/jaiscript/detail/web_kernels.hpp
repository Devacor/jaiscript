#pragma once

#include <jaiscript/core/types.hpp>
#include <string>
#include <string_view>

namespace jai {

class engine;
class script_value;

namespace stdlib {
namespace web_text_detail {

	// web::url: the parts of a URL as written (web::parse_url), and the URL they spell (to_string); port 0 is no port
	struct url_record {
		std::string scheme;
		std::string user;
		std::string password;
		std::string host;
		script_int port = 0;
		std::string path;
		std::string query;
		std::string fragment;

		std::string to_string() const {
			std::string result;
			if (!scheme.empty()) { result += scheme + ":"; }
			if (!host.empty() || !user.empty() || port != 0) {
				result += "//";
				if (!user.empty() || !password.empty()) { result += user + (password.empty() ? "" : ":" + password) + "@"; }
				result += host;
				if (port != 0) { result += ":" + std::to_string(port); }
			}
			result += path;
			if (!query.empty()) { result += "?" + query; }
			if (!fragment.empty()) { result += "#" + fragment; }
			return result;
		}
	};

	// The web:: kernels spider:: shares (source/implementation/stdlib/web_text.cpp)
	void append_percent_encoded(std::string_view a_text, bool a_form, std::string& a_out);
	bool query_value_text(const script_value& a_value, std::string& a_out, engine* a_engine);
	bool split_url(std::string_view a_text, url_record& a_out);
	// a_text with & < > " ' as web::html_escape writes them, appended to a_out (templates.cpp splices through it)
	void append_html_escaped(std::string_view a_text, std::string& a_out);

} // namespace web_text_detail
} // namespace stdlib
} // namespace jai
