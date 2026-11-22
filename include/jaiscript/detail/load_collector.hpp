#pragma once

#ifndef __JAISCRIPT_DETAIL_LOAD_COLLECTOR_HPP__
#define __JAISCRIPT_DETAIL_LOAD_COLLECTOR_HPP__

// The engine's load report while it is on (engine::load_report). A stack of (file, phase) frames: each clock reading charges
// the time since the last one to the top frame, so phases never overlap and they add up to the time the stack was not empty.

#include <jaiscript/core/load_report.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai::detail {

    class load_collector {
    public:
        static constexpr int k_no_file = -1;

        int row_of(const std::string& path);
        int current_row() const noexcept { return stack_.empty() ? k_no_file : stack_.back().row; }
        load_report_row& row(int index) { return index == k_no_file ? engine_ : rows_[static_cast<size_t>(index)]; }

        size_t enter(int row, load_phase phase);
        // a frame a suspended run left behind is closed with the frame under it: the depth says where this scope stood
        void leave(size_t depth);
        // the top frame belongs to this row after all (a file known to exist only once the frame was open)
        void retarget(int row);
        bool active() const noexcept { return !stack_.empty(); }

        void note_cache(const std::string& path, load_cache outcome, const char* reason);
        void note_body_compiled(const void* body) { compiled_bodies_.insert(body); }
        bool body_compiled(const void* body) const { return compiled_bodies_.count(body) != 0; }

        // while a re-parse is open its lex and parse are charged to load_phase::reparse
        void reparsing(int delta) noexcept { reparsing_ += delta; }

        uint64_t setup_nanoseconds = 0;

        // rows and engine as they stand, totals and wall time filled in
        load_report_summary summary() const;

    private:
        using clock = std::chrono::steady_clock;
        struct frame {
            int row;
            load_phase phase;
        };
        void charge(clock::time_point now);

        std::vector<load_report_row> rows_;
        load_report_row engine_;
        std::unordered_map<std::string, int> row_by_spelling_;
        std::unordered_map<std::string, int> row_by_file_;
        std::unordered_set<const void*> compiled_bodies_;
        std::vector<frame> stack_;
        clock::time_point last_{};
        clock::time_point window_start_{};
        uint64_t wall_ = 0;
        int reparsing_ = 0;
    };

    // One phase of one file; a null collector (the report is off) costs the test and nothing else
    class load_phase_scope {
    public:
        load_phase_scope(load_collector* collector, const std::string& path, load_phase phase) : collector_(collector) {
            if (collector_) { depth_ = collector_->enter(collector_->row_of(path), phase); }
        }
        load_phase_scope(load_collector* collector, int row, load_phase phase) : collector_(collector) {
            if (collector_) { depth_ = collector_->enter(row, phase); }
        }
        // the phase, in the file the load is in
        load_phase_scope(load_collector* collector, load_phase phase) : collector_(collector) {
            if (collector_) { depth_ = collector_->enter(collector_->current_row(), phase); }
        }
        load_phase_scope(const load_phase_scope&) = delete;
        load_phase_scope& operator=(const load_phase_scope&) = delete;
        ~load_phase_scope() {
            if (collector_) { collector_->leave(depth_); }
        }

    private:
        load_collector* collector_;
        size_t depth_ = 0;
    };

    class load_reparse_scope {
    public:
        explicit load_reparse_scope(load_collector* collector) : collector_(collector) {
            if (collector_) { collector_->reparsing(1); }
        }
        load_reparse_scope(const load_reparse_scope&) = delete;
        load_reparse_scope& operator=(const load_reparse_scope&) = delete;
        ~load_reparse_scope() {
            if (collector_) { collector_->reparsing(-1); }
        }

    private:
        load_collector* collector_;
    };

}

#endif
