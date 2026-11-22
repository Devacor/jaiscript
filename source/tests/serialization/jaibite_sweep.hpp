#pragma once
// What the jaibite corruption sweeps share. JAIBITE_SWEEP_FROM=<byte> starts a sweep further on and JAIBITE_SWEEP_TO=<byte>
// ends it early, JAIBITE_SWEEP_STRIDE=<bytes> sets its step (1: every byte), JAIBITE_SWEEP_UNIT=<file> sweeps the bite of that
// program in place of the test's own and JAIBITE_SWEEP_NOTES=<file> names each mutant before it runs, so a crash names its mutant.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace jai::foundry::tests {

inline size_t sweep_start() {
    const char* from = std::getenv("JAIBITE_SWEEP_FROM");
    return from ? static_cast<size_t>(std::atoll(from)) : 0;
}

inline size_t sweep_end(size_t a_size) {
    const char* to = std::getenv("JAIBITE_SWEEP_TO");
    return to ? std::min(a_size, static_cast<size_t>(std::atoll(to))) : a_size;
}

inline bool sweep_whole() { return !std::getenv("JAIBITE_SWEEP_FROM") && !std::getenv("JAIBITE_SWEEP_TO"); }

inline size_t sweep_stride(size_t a_default) {
    const char* stride = std::getenv("JAIBITE_SWEEP_STRIDE");
    const size_t result = stride ? static_cast<size_t>(std::atoll(stride)) : a_default;
    return result == 0 ? 1 : result;
}

inline std::string sweep_unit(const char* a_default) {
    const char* path = std::getenv("JAIBITE_SWEEP_UNIT");
    if (!path) { return a_default; }
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void sweep_note(const std::string& a_what) {
    if (const char* path = std::getenv("JAIBITE_SWEEP_NOTES")) { std::ofstream(path, std::ios::app | std::ios::binary) << a_what << "\n"; }
}

}
