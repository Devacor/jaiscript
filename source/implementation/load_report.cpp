#include <jaiscript/core/load_report.hpp>
#include <jaiscript/detail/load_collector.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace jai {

    namespace {

        struct load_phase_column {
            load_phase phase;
            const char* name;
            const char* heading;
        };
        constexpr load_phase_column k_load_phase_columns[k_load_phase_count] = {
            {load_phase::source_read, "source_read", "read"},
            {load_phase::stamp_query, "stamp_query", "stamp"},
            {load_phase::freshness, "freshness", "fresh"},
            {load_phase::cache_read, "cache_read", "c.read"},
            {load_phase::decode, "decode", "decode"},
            {load_phase::lex, "lex", "lex"},
            {load_phase::parse, "parse", "parse"},
            {load_phase::generics, "generics", "gener"},
            {load_phase::reparse, "reparse", "repars"},
            {load_phase::check, "check", "check"},
            {load_phase::proofs, "proofs", "proofs"},
            {load_phase::cache_write, "cache_write", "c.writ"},
            {load_phase::vm_compile, "vm_compile", "compil"},
            {load_phase::body_compile, "body_compile", "bodies"},
            {load_phase::run, "run", "run"},
            {load_phase::template_compile, "template_compile", "templ"},
            {load_phase::import_bookkeeping, "import_bookkeeping", "import"},
            {load_phase::other, "other", "other"},
        };

        double ms_of(uint64_t nanoseconds) { return static_cast<double>(nanoseconds) / 1e6; }

        std::string decimal_text(double value, int decimals) {
            char text[48];
            std::snprintf(text, sizeof(text), "%.*f", decimals, value);
            return text;
        }

        std::string pad_to(const std::string& text, size_t width) { return text.size() >= width ? text : std::string(width - text.size(), ' ') + text; }
        std::string pad_after(const std::string& text, size_t width) { return text.size() >= width ? text : text + std::string(width - text.size(), ' '); }

        std::string load_json_text(const std::string& text) {
            std::string out = "\"";
            for (const char c : text) {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    out += escaped;
                }
                else { out += c; }
            }
            return out + '"';
        }

        void add_load_row(load_report_row& into, const load_report_row& row) {
            for (size_t i = 0; i < k_load_phase_count; ++i) {
                into.phases[i].nanoseconds += row.phases[i].nanoseconds;
                into.phases[i].count += row.phases[i].count;
            }
            into.source_bytes += row.source_bytes;
            into.source_reads += row.source_reads;
            into.source_reuses += row.source_reuses;
            into.cache_bytes += row.cache_bytes;
            into.checks_run += row.checks_run;
            into.checks_trusted += row.checks_trusted;
            into.functions += row.functions;
            into.functions_compiled += row.functions_compiled;
            into.function_bytes += row.function_bytes;
            into.function_bytes_compiled += row.function_bytes_compiled;
        }

        // The folder every file sits under, so the table names them briefly
        std::string common_load_folder(const std::vector<load_report_row>& rows) {
            std::string common;
            bool first = true;
            for (const auto& row : rows) {
                if (row.file.empty() || row.file[0] == '<') { continue; }
                const std::string folder = row.file.substr(0, row.file.find_last_of('/') + 1);
                if (first) { common = folder; first = false; continue; }
                size_t same = 0;
                while (same < common.size() && same < folder.size() && common[same] == folder[same]) { ++same; }
                common.resize(same);
                const size_t slash = common.rfind('/');
                common.resize(slash == std::string::npos ? 0 : slash + 1);
            }
            return common;
        }

        std::string load_check_text(const load_report_row& row) {
            if (row.checks_trusted > 0 && row.checks_run == 0) { return "trusted"; }
            if (row.checks_run > 0) { return "ran"; }
            return "-";
        }

        std::string load_cache_text(const load_report_row& row) {
            if (row.cache == load_cache::none) { return "-"; }
            std::string text = load_cache_name(row.cache);
            if (!row.cache_reason.empty()) { text += ": " + row.cache_reason; }
            if (row.cache_write == "written" || row.cache_write == "failed") { text += " (" + std::string(row.cache_write == "written" ? "written" : "write failed") + ")"; }
            return text;
        }

    }

    const char* load_phase_name(load_phase phase) noexcept { return k_load_phase_columns[static_cast<size_t>(phase)].name; }

    const char* load_cache_name(load_cache cache) noexcept {
        switch (cache) {
            case load_cache::memory: return "memory";
            case load_cache::hit: return "hit";
            case load_cache::miss: return "miss";
            case load_cache::not_cacheable: return "not cacheable";
            case load_cache::none: break;
        }
        return "none";
    }

    uint64_t load_report_row::nanoseconds() const {
        uint64_t total = 0;
        for (const auto& cost : phases) { total += cost.nanoseconds; }
        return total;
    }

    uint64_t load_report_summary::load_nanoseconds() const {
        return wall_nanoseconds - (rows.empty() ? 0 : rows.front().phase(load_phase::run).nanoseconds);
    }

    std::string load_report_summary::format() const {
        std::vector<const load_phase_column*> columns;
        for (const auto& column : k_load_phase_columns) {
            if (totals.phase(column.phase).nanoseconds != 0) { columns.push_back(&column); }
        }
        const std::string folder = common_load_folder(rows);
        std::vector<std::string> names;
        size_t name_width = 8;
        for (const auto& row : rows) {
            names.push_back(!folder.empty() && row.file.rfind(folder, 0) == 0 ? row.file.substr(folder.size()) : row.file);
            name_width = std::max(name_width, names.back().size());
        }
        const auto line = [&](const std::string& name, const load_report_row& row, const std::string& check, const std::string& cache) {
            std::string out = pad_after(name, name_width) + pad_to(decimal_text(static_cast<double>(row.source_bytes) / 1024.0, 1), 8);
            for (const load_phase_column* column : columns) {
                const uint64_t ns = row.phase(column->phase).nanoseconds;
                out += pad_to(ns == 0 ? "." : decimal_text(ms_of(ns), 2), 9);
            }
            out += pad_to(decimal_text(ms_of(row.nanoseconds()), 2), 10) + "  " + pad_after(check, 8) + cache;
            while (!out.empty() && out.back() == ' ') { out.pop_back(); }
            return out + "\n";
        };

        std::string out = "load report (" + std::string(vm ? "vm" : "interpreter") + "), times in ms";
        if (!folder.empty()) { out += ", files under " + folder; }
        out += "\n" + pad_after("file", name_width) + pad_to("KB", 8);
        for (const load_phase_column* column : columns) { out += pad_to(column->heading, 9); }
        out += pad_to("total", 10) + "  " + pad_after("check", 8) + "cache\n";
        for (size_t i = 0; i < rows.size(); ++i) { out += line(names[i], rows[i], load_check_text(rows[i]), load_cache_text(rows[i])); }
        out += line("(engine)", engine, "", "");
        out += line("total", totals, "", "");
        out += "load " + decimal_text(ms_of(load_nanoseconds()), 2) + " ms";
        if (!rows.empty() && rows.front().phase(load_phase::run).nanoseconds != 0) {
            out += " + first file's top-level run " + decimal_text(ms_of(rows.front().phase(load_phase::run).nanoseconds), 2) + " ms";
        }
        out += " = " + decimal_text(ms_of(wall_nanoseconds), 2) + " ms";
        if (setup_nanoseconds != 0) { out += "; engine setup " + decimal_text(ms_of(setup_nanoseconds), 2) + " ms"; }
        out += "\n";
        if (vm && totals.functions != 0) {
            const auto percent = [](uint64_t part, uint64_t whole) { return whole == 0 ? std::string("0") : decimal_text(100.0 * static_cast<double>(part) / static_cast<double>(whole), 0); };
            out += "function bodies compiled: " + std::to_string(totals.functions_compiled) + " of " + std::to_string(totals.functions) +
                " (" + percent(totals.functions_compiled, totals.functions) + "%), " +
                percent(totals.function_bytes_compiled, totals.function_bytes) + "% of their encoded bytes\n";
        }
        return out;
    }

    std::string load_report_summary::json() const {
        std::ostringstream out;
        const auto row_json = [&](const load_report_row& row) {
            out << "{\"file\":" << load_json_text(row.file) << ",\"phases\":{";
            bool first = true;
            for (const auto& column : k_load_phase_columns) {
                const load_phase_cost& cost = row.phase(column.phase);
                if (cost.nanoseconds == 0 && cost.count == 0) { continue; }
                out << (first ? "" : ",") << '"' << column.name << "\":{\"ns\":" << cost.nanoseconds << ",\"count\":" << cost.count << '}';
                first = false;
            }
            out << "},\"source_bytes\":" << row.source_bytes << ",\"source_reads\":" << row.source_reads << ",\"source_reuses\":" << row.source_reuses
                << ",\"cache\":\"" << load_cache_name(row.cache) << "\",\"cache_reason\":" << load_json_text(row.cache_reason)
                << ",\"cache_write\":" << load_json_text(row.cache_write) << ",\"cache_bytes\":" << row.cache_bytes
                << ",\"checks_run\":" << row.checks_run << ",\"checks_trusted\":" << row.checks_trusted
                << ",\"functions\":" << row.functions << ",\"functions_compiled\":" << row.functions_compiled
                << ",\"function_bytes\":" << row.function_bytes << ",\"function_bytes_compiled\":" << row.function_bytes_compiled << '}';
        };
        out << "{\"backend\":\"" << (vm ? "vm" : "interpreter") << "\",\"wall_ns\":" << wall_nanoseconds << ",\"load_ns\":" << load_nanoseconds()
            << ",\"setup_ns\":" << setup_nanoseconds << ",\"rows\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i != 0) { out << ','; }
            row_json(rows[i]);
        }
        out << "],\"engine\":";
        row_json(engine);
        out << ",\"totals\":";
        row_json(totals);
        out << '}';
        return out.str();
    }

    namespace detail {

        int load_collector::row_of(const std::string& path) {
            const auto known = row_by_spelling_.find(path);
            if (known != row_by_spelling_.end()) { return known->second; }
            std::string file = path.empty() ? std::string("<script>") : path;
            if (file[0] != '<') {
                std::error_code ec;
                const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, ec);
                if (!ec) { file = canonical.generic_string(); }
            }
            const auto [slot, added] = row_by_file_.try_emplace(file, static_cast<int>(rows_.size()));
            if (added) {
                rows_.emplace_back();
                rows_.back().file = file;
            }
            row_by_spelling_.emplace(path, slot->second);
            return slot->second;
        }

        void load_collector::charge(clock::time_point now) {
            const frame& top = stack_.back();
            row(top.row).phases[static_cast<size_t>(top.phase)].nanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count());
            last_ = now;
        }

        size_t load_collector::enter(int a_row, load_phase phase) {
            if (reparsing_ > 0 && (phase == load_phase::lex || phase == load_phase::parse)) { phase = load_phase::reparse; }
            const clock::time_point now = clock::now();
            if (stack_.empty()) {
                window_start_ = now;
                last_ = now;
            } else {
                charge(now);
            }
            ++row(a_row).phases[static_cast<size_t>(phase)].count;
            stack_.push_back({a_row, phase});
            return stack_.size() - 1;
        }

        void load_collector::leave(size_t depth) {
            if (stack_.size() <= depth) { return; }
            const clock::time_point now = clock::now();
            charge(now);
            stack_.resize(depth);
            if (stack_.empty()) { wall_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - window_start_).count()); }
        }

        void load_collector::retarget(int a_row) {
            frame& top = stack_.back();
            if (top.row == a_row) { return; }
            --row(top.row).phases[static_cast<size_t>(top.phase)].count;
            ++row(a_row).phases[static_cast<size_t>(top.phase)].count;
            top.row = a_row;
        }

        void load_collector::note_cache(const std::string& path, load_cache outcome, const char* reason) {
            load_report_row& target = row(row_of(path));
            if (outcome == load_cache::memory && target.cache != load_cache::none) { return; }   // the load that first decided it says more
            target.cache = outcome;
            target.cache_reason = reason;
        }

        load_report_summary load_collector::summary() const {
            load_report_summary result;
            result.rows = rows_;
            result.engine = engine_;
            result.wall_nanoseconds = wall_;
            if (!stack_.empty()) { result.wall_nanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(last_ - window_start_).count()); }   // a load still under way, as far as it is charged
            result.setup_nanoseconds = setup_nanoseconds;
            for (const auto& each : rows_) { add_load_row(result.totals, each); }
            add_load_row(result.totals, engine_);
            return result;
        }

    }

}
