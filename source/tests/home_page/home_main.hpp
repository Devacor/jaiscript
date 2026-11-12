// The entry both landing-page tab programs share: with no arguments the page's own main, on the engine the page makes; with
// --interpreter and a file, that file's script on an interpreter engine bound the same way. A script error prints and fails.
#pragma once

#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/registrar.hpp>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace home_page {
int main();
}

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::string_view(argv[1]) == "--interpreter") {
			std::ifstream in(argv[2], std::ios::binary);
			const std::string script((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			auto engine = jai::engine::make();
			engine->set_backend(jai::backend_type::interpreter);
			jai::bind_registrar(*engine);
			engine->execute(script);
			return 0;
		}
		return home_page::main();
	} catch (const std::exception& e) {
		std::cout << "threw: " << e.what() << "\n";
		return 1;
	}
}
