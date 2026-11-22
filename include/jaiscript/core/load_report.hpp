#pragma once

#ifndef __JAISCRIPT_CORE_LOAD_REPORT_HPP__
#define __JAISCRIPT_CORE_LOAD_REPORT_HPP__

// What engine::load_report collects: where loading scripts spent its time, per file and per phase. Times are exclusive (a
// file imported from another file's top level is charged to the imported file, a parse is not also part of the check that
// asked for it), so every row's phases, the engine row's and nothing else add up to wall_nanoseconds.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jai {

    enum class load_phase : uint8_t {
        source_read,         // reading the file's bytes (the engine's one source loader)
        stamp_query,         // asking a file's (mtime, size) stamp
        freshness,           // hashing the source, comparing it with a cache file's header
        cache_read,          // reading a .jaibite cache file
        decode,              // turning its bytes into the program's tree
        lex,
        parse,
        generics,            // making the instances of generics
        reparse,             // the second lex and parse of a unit that read a namespace class before its program was pooled
        check,               // the static check's diagnostics walk
        proofs,              // the type proofs and the aggregate lowering they read
        cache_write,
        vm_compile,          // the unit's top level, on the VM
        body_compile,        // function bodies, on the VM, when first called; charged to the file that declares them
        run,                 // the unit's top-level code
        template_compile,    // lexing and parsing a template the program names
        import_bookkeeping,  // resolving imports and the program's name pools: charged to no file
        other                // the rest of the unit's load, or of the engine's
    };
    inline constexpr size_t k_load_phase_count = static_cast<size_t>(load_phase::other) + 1;
    const char* load_phase_name(load_phase phase) noexcept;

    enum class load_cache : uint8_t {
        none,            // no load asked for this file's program (a file that was only read: a template, a spliced include)
        memory,          // the engine already held the parsed program
        hit,             // a cache file loaded
        miss,            // no cache file served, and the source was parsed; cache_reason says why
        not_cacheable    // this unit keeps no cache file; cache_reason says why
    };
    const char* load_cache_name(load_cache cache) noexcept;

    struct load_phase_cost {
        uint64_t nanoseconds = 0;
        uint64_t count = 0;
    };

    struct load_report_row {
        std::string file;   // the canonical path; "<script>" for source text; empty on the engine row
        std::array<load_phase_cost, k_load_phase_count> phases{};
        uint64_t source_bytes = 0;
        uint64_t source_reads = 0;    // reads that opened the file
        uint64_t source_reuses = 0;   // loads a text read earlier in the same load answered

        load_cache cache = load_cache::none;
        // miss: "no file", "different source", "fingerprint", "format version", "namespace classes" or "corrupt". not_cacheable:
        // "source text", "cache off", "filesystem off", "no cache path", "spliced files" or "another file's generics".
        std::string cache_reason;
        // "written", "up to date", "failed", or why nothing can be written (cache_reason's "spliced files" and
        // "another file's generics"); empty when the unit has no cache file
        std::string cache_write;
        uint64_t cache_bytes = 0;     // the cache file read, or written when none was read

        uint64_t checks_run = 0;
        uint64_t checks_trusted = 0;  // first checks a cache file's checked-clean stamp stood in for

        // Functions and methods declared at file, namespace and class scope (lambdas and nested functions compile with
        // their enclosing code). The bytes are each body's encoded size, what decoding it costs. Compiled counts stay 0 on
        // the interpreter, which compiles nothing.
        uint64_t functions = 0;
        uint64_t functions_compiled = 0;
        uint64_t function_bytes = 0;
        uint64_t function_bytes_compiled = 0;

        const load_phase_cost& phase(load_phase which) const { return phases[static_cast<size_t>(which)]; }
        uint64_t nanoseconds() const;
    };

    struct load_report_summary {
        std::vector<load_report_row> rows;   // one per file, in the order the load first reached them
        load_report_row engine;              // the time that belongs to no file
        load_report_row totals;              // rows and engine added up
        uint64_t wall_nanoseconds = 0;       // every span the engine spent in reported work
        uint64_t setup_nanoseconds = 0;      // engine::load_report_setup_time
        bool vm = false;                     // the backend is the VM

        // wall time without the first file's own top-level run, which for a program whose top level calls main is the program
        uint64_t load_nanoseconds() const;
        std::string format() const;   // the table
        std::string json() const;     // one line
    };

}

#endif
