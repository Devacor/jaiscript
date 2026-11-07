#pragma once

#ifndef __JAISCRIPT_VM_COVERAGE_HPP__
#define __JAISCRIPT_VM_COVERAGE_HPP__

// The execution-coverage census (a diagnostic build: JAISCRIPT_VM_COVERAGE).
// Every instruction the vm executes is counted where it runs: in a cell block (a closed loop, a straight-line region,
// a frame program) or on the general form, typed (op_t_*) or untyped. A chunk is classified once, when it first runs:
// each general instruction's region (the closed loop whose general form it is, the refused loop and the refusal the
// compiler recorded, the straight-line span the region builder dropped, a loop the translation never attempted) and the
// reason it runs outside the cells, attributed to the function and the source line. Each backend owns its tables; the
// engine keeps every backend's tables (the parallel workers' too) and builds the report from them.

#ifdef JAISCRIPT_VM_COVERAGE

#include "chunk.hpp"
#include <array>
#include <cstdint>
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#else
#include <chrono>
#endif
#endif
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jai {
    class class_definition;
    class string_symbolizer;
    struct vm_coverage_totals;
}

namespace jai::vm {

    // where a general instruction stands
    enum class coverage_region : uint8_t { none, closed, boundary, straight, refused, unattempted, trampoline, frame_program };

    struct coverage_chunk {
        std::string name;
        const void* identity = nullptr;   // the chunk's code buffer and length: a reused chunk address with another body starts a fresh row
        size_t size = 0;
        std::vector<uint64_t> general;    // per ip: executions on the dispatcher
        std::vector<uint64_t> cells;      // per ip: executions in a block (a copy instruction; a fused pair counts both halves)
        std::vector<uint32_t> line;       // per ip (a copy instruction: its origin's line)
        std::vector<uint16_t> file;       // per ip: index into files (a graft's statements keep their own file)
        std::vector<std::string> files;   // the source files' names
        std::vector<uint16_t> op;         // per ip
        std::vector<coverage_region> region;   // per general ip
        std::vector<uint32_t> region_index;    // per general ip: the cell_loops index (closed, boundary, straight) or the refusal's site
        std::vector<uint32_t> site;            // per ip: index into sites
        struct site_row { uint32_t line = 0; uint16_t file = 0; std::string reason; std::string detail; };
        std::vector<site_row> sites;
        std::unordered_map<uint32_t, uint8_t> binary_class;   // per ip of a fused binary or compare: the checker's operand class (0 unproven, 1 int, 2 float)
        std::unordered_map<uint32_t, std::string> details;   // per ip of a member, method, arithmetic or comparison: its opcode and the name it touches
        struct loop_row {
            uint32_t head_line = 0, back_line = 0;
            uint16_t head_file = 0;
            bool straight = false, frame_program = false;
            uint64_t entries = 0, misses = 0;
            uint64_t refused[4] = {};   // the run-time entry refusals: 0 debugger or numeric operator, 1 no cell area, 2 the refusal valve, 3 a guard or a site
        };
        std::vector<loop_row> loops;      // per cell_loops index
        // member and method sites on the general form: the receiver kinds seen and the classes behind them
        struct receiver_row { uint64_t kinds[8] = {}; std::vector<std::pair<const class_definition*, uint64_t>> classes; };
        std::unordered_map<uint32_t, receiver_row> receivers;
        // untyped arithmetic and comparisons on the general form: per ip, the operand kinds seen (a mask per execution, counted)
        std::unordered_map<uint32_t, std::array<uint64_t, 8>> operands;
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
        std::vector<uint64_t> cycles;     // per general ip: the instruction's own clock (a block entry carries the block's run)
        uint64_t inclusive_cycles = 0;    // frame entry to exit (a recursive activation counts again)
        uint64_t entries = 0;
#endif
    };

    // receiver kinds (coverage_chunk::receiver_row::kinds)
    inline constexpr unsigned k_cov_recv_instance = 0, k_cov_recv_host = 1, k_cov_recv_array = 2, k_cov_recv_map = 3, k_cov_recv_string = 4,
                              k_cov_recv_null = 5, k_cov_recv_other = 6;
    // operand kind buckets (coverage_chunk::operands): the worst kind of the operation's operands
    inline constexpr unsigned k_cov_opnd_int = 0, k_cov_opnd_float = 1, k_cov_opnd_mixed = 2, k_cov_opnd_bool = 3, k_cov_opnd_string = 4,
                              k_cov_opnd_object = 5, k_cov_opnd_container = 6, k_cov_opnd_unknown = 7;

    struct coverage_tables {
        std::unordered_map<const chunk*, coverage_chunk> chunks;
        std::vector<coverage_chunk> retired;   // rows whose chunk address another body reused
        const chunk* last = nullptr;
        coverage_chunk* last_row = nullptr;
        uint64_t cell_dispatches = 0;          // block-loop dispatches (a fused pair is one)
        coverage_chunk& bind(const chunk& c, const string_symbolizer* symbolizer);
        void reset();
#ifdef JAISCRIPT_VM_COVERAGE_TIMING
        // the running instruction, its clock started at timed_since (a null row: nothing is charged)
        coverage_chunk* timed_row = nullptr;
        size_t timed_ip = 0;
        uint64_t timed_since = 0;
        void charge(uint64_t now) {
            if (timed_row && timed_ip < timed_row->cycles.size()) { timed_row->cycles[timed_ip] += now - timed_since; }
            timed_since = now;
        }
#endif
    };

#ifdef JAISCRIPT_VM_COVERAGE_TIMING
    // the timing build's clock: cycles on x86, nanoseconds elsewhere
    inline uint64_t coverage_clock() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
        return __rdtsc();
#else
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }
#endif

    // the census over every backend's tables: totals per class, the general form's reasons ranked by executed
    // operations, the top source sites per reason, the receivers of member operations, the operand kinds of untyped
    // arithmetic and the closed loops' entries; top = the rows per ranked table
    std::string coverage_report(const std::vector<std::shared_ptr<coverage_tables>>& tables, size_t top);
    // the report's class totals and reasons as numbers (engine::vm_coverage_summary)
    void coverage_summary(const std::vector<std::shared_ptr<coverage_tables>>& tables, vm_coverage_totals& out);

} // namespace jai::vm

#endif // JAISCRIPT_VM_COVERAGE

#endif // __JAISCRIPT_VM_COVERAGE_HPP__
