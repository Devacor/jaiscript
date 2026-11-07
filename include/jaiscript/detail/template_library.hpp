#pragma once

#ifndef __JAISCRIPT_DETAIL_TEMPLATE_LIBRARY_HPP__
#define __JAISCRIPT_DETAIL_TEMPLATE_LIBRARY_HPP__

// HTML templates (docs/site/guide/19-templates.html): template(path, a, b, ...) renders in its caller (the parser expands a
// literal path there), scoped_template(path, map) and scoped_template_text(text, map) render through the sandboxed evaluator
// in source/implementation/templates.cpp. The builtins below carry names no script can spell.

#include <jaiscript/core/types.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace jai {

	class engine;
	class expression;
	class string_symbolizer;

	namespace detail::template_library {

		inline constexpr std::string_view k_joiner = "template#html";            // the parts of a body, joined into a web::html
		inline constexpr std::string_view k_splice = "template#splice";          // ${expr}, ${expr:spec}: escaped text
		inline constexpr std::string_view k_raw = "template#raw";                // raw(text) in a trusted template
		inline constexpr std::string_view k_dynamic = "template#dynamic";        // template(path, ...) with a path known at run time
		inline constexpr std::string_view k_scoped = "template#scoped";          // scoped_template's text, its label, its map
		inline constexpr std::string_view k_scoped_file = "template#scoped_file";   // scoped_template(path, map) with a path known at run time
		inline constexpr std::string_view k_text_label = "<scoped_template_text>";
		inline constexpr size_t k_default_output_limit = size_t(1) << 20;

		// A scoped_template rule the body breaks, at the node that breaks it
		struct violation {
			source_location at;
			std::string message;
		};
		void scoped_violations(const expression& body, string_symbolizer& symbols, std::vector<violation>& out);

		// A scoped_template file's path, checked and resolved before any file opens (engine::scoped_template_dirs): its canonical
		// path, or the error text that refuses it. `from` is the file of the script that calls.
		struct scoped_file {
			std::string resolved;
			std::string error;
		};
		scoped_file resolve_scoped_file(engine& eng, const std::string& path, const std::string& from);

		void register_library(engine& eng);

	} // namespace detail::template_library

} // namespace jai

#endif // __JAISCRIPT_DETAIL_TEMPLATE_LIBRARY_HPP__
