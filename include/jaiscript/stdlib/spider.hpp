#pragma once

#include <jaiscript/core/engine.hpp>

namespace jai {
namespace stdlib {

	// The spider:: HTTP client, registered only by this call (never by register_all): spider::catch(url[, options]) and
	// its plain name spider::get, spider::sling(content, url[, options]) and its plain name spider::post(url, content
	// [, options]), the spider::Response class and its spider::Response(status, body) constructor
	// (source/implementation/stdlib/spider.cpp). The calls send through the host's engine::http_transport and require the
	// "network" permission, which this call registers (disabled until the host enables it).
	void register_spider(engine& eng_ref);
	inline void register_spider(const std::shared_ptr<engine>& e) { register_spider(*e); }
	inline void register_spider(engine* e) { register_spider(*e); }

} // namespace stdlib
} // namespace jai
