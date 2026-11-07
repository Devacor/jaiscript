#pragma once

#include <jaiscript/core/engine.hpp>

namespace jai {
namespace stdlib {

	// The web:: library, registered only by this call (never by register_all): web::html_escape, web::html_unescape,
	// web::url_encode, web::url_decode, web::form_encode, web::form_decode, web::parse_query, web::parse_query_all,
	// web::build_query, web::parse_url and the web::url record
	void register_web(engine& eng_ref);

} // namespace stdlib
} // namespace jai
