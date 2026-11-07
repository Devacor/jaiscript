// jaiscript_lsp: the JaiScript language server. Speaks the Language Server Protocol over
// stdin/stdout and answers from the engine's own lexer, parser and strict checker.
//
//   jaiscript_lsp [--host=runner|stdlib|bare] [--include=<dir>]... [--max-message=<bytes>] [--log]
//
// --host picks the registration surface scripts are checked against: `runner` (default) is the
// jaiscript runner's (stdlib, file IO, console, networking, ARGS), `stdlib` is
// jai::stdlib::register_all alone, `bare` is a fresh engine.

#include "server.hpp"

#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include <jaiscript/testing/unattended.hpp>

std::shared_ptr<jai::engine> jaiscript_runner_host_engine();

namespace {

std::shared_ptr<jai::engine> make_engine(const std::string& host) {
	std::shared_ptr<jai::engine> eng;
	if (host == "bare") {
		eng = jai::engine::make();
	} else if (host == "stdlib") {
		eng = jai::engine::make();
		jai::stdlib::register_all(eng);
	} else {
		eng = jaiscript_runner_host_engine();
	}
	eng->jaibite_cache(false);
	return eng;
}

void usage() {
	std::cout << "jaiscript_lsp - the JaiScript language server (LSP over stdio)\n"
	             "  --host=runner|stdlib|bare   registration surface to check against (default runner)\n"
	             "  --include=<dir>             extra include path for import/include resolution (repeatable)\n"
	             "  --max-message=<bytes>       largest message body read (default 64 MiB, at least 65536; a larger one is skipped)\n"
	             "  --log                       log requests and timings to stderr\n";
}

} // namespace

int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	std::string host = "runner";
	std::vector<std::string> includes;
	bool log = false;
	size_t max_message = jai_lsp::server::default_max_message;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a.rfind("--host=", 0) == 0) {
			host = a.substr(7);
			if (host != "runner" && host != "stdlib" && host != "bare") {
				std::cerr << "jaiscript_lsp: --host takes runner, stdlib or bare\n";
				return 2;
			}
		}
		else if (a.rfind("--include=", 0) == 0) { includes.push_back(a.substr(10)); }
		else if (a.rfind("--max-message=", 0) == 0) {
			const std::string text = a.substr(14);
			auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), max_message);
			if (ec != std::errc{} || end != text.data() + text.size() || max_message < 65536) {
				std::cerr << "jaiscript_lsp: --max-message takes a byte count of at least 65536\n";
				return 2;
			}
		}
		else if (a == "--log") { log = true; }
		else if (a == "--stdio" || a.rfind("--clientProcessId", 0) == 0) {}
		else if (a == "--help" || a == "-h") { usage(); return 0; }
		else if (a == "--version") { std::cout << "jaiscript_lsp 0.5.1-beta\n"; return 0; }
		else { std::cerr << "jaiscript_lsp: ignoring unknown argument " << a << " (--help lists the flags)\n"; }
	}
	jai_lsp::server s(make_engine, host, includes, log, max_message);
	int code = s.run();
	std::fflush(stdout);
	std::_Exit(code);   // the stdin reader may still be blocked in a read: skip teardown rather than wait on its stream lock
}
