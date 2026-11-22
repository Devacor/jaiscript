#pragma once

#ifndef __JAISCRIPT_DETAIL_SOURCE_LOADING_HPP__
#define __JAISCRIPT_DETAIL_SOURCE_LOADING_HPP__

// What the engine's one source loader hands back (engine.cpp implementation::load_source, reached through
// engine_access::load_source): a file's bytes with the stamp of exactly those bytes. Everything that reads a source file
// (execute_file, import, include, a file spliced into a function body, a template, the checker's gather) loads it there, and
// every stamp the engine remembers is one this loader returned with the bytes it was remembered for.

#include <jaiscript/core/types.hpp>
#include <cstdint>
#include <string>

namespace jai::detail {

	// the reads a load tries before it gives the file up as unsettled
	inline constexpr int k_source_load_tries = 4;

	// What the caller parses. text: what the platform's text mode reads, as a script always was (on Windows a "\r\n" is one
	// '\n' and the text ends at a Ctrl-Z; elsewhere the bytes). binary: the bytes, as a template and a file spliced into a
	// function body always were. One file read both ways is one read of its bytes.
	enum class source_mode : uint8_t { text, binary };

	// The stamp of a text the loader returned. A file the stamp source cannot stamp loads unstamped, and is never remembered.
	struct text_stamp {
		uint64_t mtime = 0;
		uint64_t size = 0;
		bool stamped = false;
	};

	// A file under the stamp its bytes were loaded with; a zero stamp is one no file carries, so it always reads as moved
	struct source_stamp {
		std::string path;
		uint64_t mtime = 0;
		uint64_t size = 0;
	};

	struct loaded_source {
		// missing: the file cannot be opened. unsettled: its stamp moved during every read, so no read stands for a version
		// of the file; nothing is parsed, run or remembered from it.
		enum class state : uint8_t { loaded, missing, unsettled };
		state status = state::missing;
		text_stamp stamp;
		std::string text;
	};

	// The error of a file that never settled, the same text wherever it was being loaded
	std::string unsettled_source_text(const std::string& a_path);

	// A first load raises it as the load's error; a reload that meets it keeps the code it had and tries again on its next pass
	class source_unsettled : public runtime_error {
	public:
		explicit source_unsettled(const std::string& a_path) : runtime_error(unsettled_source_text(a_path)), path_(a_path) {}
		const std::string& path() const noexcept { return path_; }

	private:
		std::string path_;
	};

}

#endif
