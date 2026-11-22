// jaiscript_image_fuzz: the image:: decoders against mutated fixtures, on their own (no engine, no test framework), so a
// sanitizer build links only this file and the codecs.
//
//   jaiscript_image_fuzz [--seed N] [--iterations N] [--format png|jpeg|gif] [--dump FILE]
//
// Runs cases seed .. seed + iterations - 1 of each format (default 0 and 2000) and prints one line a format: the cases run,
// the images returned, the errors by catalogued text and the wall time. --dump writes the one case --seed names (with
// --format) to FILE and stops. Exit 1 when any case is wrong: an uncatalogued error, an image that disagrees with its
// header, or a case over its time cap.

#include "fuzz/image_mutation.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

int main(int a_count, char** a_arguments) {
	uint64_t seed = 0;
	uint64_t iterations = 2000;
	const char* only = nullptr;
	const char* dump = nullptr;
	for (int i = 1; i < a_count; ++i) {
		const std::string argument = a_arguments[i];
		const char* value = i + 1 < a_count ? a_arguments[i + 1] : nullptr;
		if (argument == "--seed" && value) { seed = std::strtoull(value, nullptr, 10); ++i; }
		else if (argument == "--iterations" && value) { iterations = std::strtoull(value, nullptr, 10); ++i; }
		else if (argument == "--format" && value) { only = value; ++i; }
		else if (argument == "--dump" && value) { dump = value; ++i; }
		else {
			std::fprintf(stderr, "usage: jaiscript_image_fuzz [--seed N] [--iterations N] [--format png|jpeg|gif] [--dump FILE]\n");
			return 2;
		}
	}
	if (dump) {
		const char* format = only ? only : "png";
		const auto pool = jai::image_mutation::fixtures_of(format);
		if (pool.empty()) { std::fprintf(stderr, "no fixtures of that format\n"); return 2; }
		const std::vector<uint8_t> bytes = jai::image_mutation::mutated_case(format, pool, seed);
		std::ofstream file(dump, std::ios::binary);
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!file) { std::fprintf(stderr, "cannot write %s\n", dump); return 2; }
		return 0;
	}
	bool wrong = false;
	for (const char* format : jai::image_mutation::k_formats) {
		if (only && std::strcmp(only, format) != 0) { continue; }
		if (jai::image_mutation::fixtures_of(format).empty()) { continue; }
		const jai::image_mutation::summary result = jai::image_mutation::run(format, seed, iterations, 0);
		std::printf("%s\n", jai::image_mutation::describe(format, result).c_str());
		std::fflush(stdout);
		wrong = wrong || result.wrong != 0;
	}
	return wrong ? 1 : 0;
}
