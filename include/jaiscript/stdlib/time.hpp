#pragma once

#ifndef __JAISCRIPT_STDLIB_TIME_HPP__
#define __JAISCRIPT_STDLIB_TIME_HPP__

#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/engine_time.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace jai {
namespace stdlib {

    // time::stopwatch: running seconds, read from the engine's monotonic clock once per call; paused
    // intervals never count
    struct stopwatch {
        engine* eng = nullptr;
        std::chrono::nanoseconds accumulated{};   // running time before run_start
        std::chrono::nanoseconds run_start{};
        std::chrono::nanoseconds delta_mark{};    // elapsed at the last delta()
        bool running = false;
        bool paused = false;

        explicit stopwatch(engine* e) : eng(e) {}

        static double seconds(std::chrono::nanoseconds n) { return std::chrono::duration<double>(n).count(); }
        bool idle() const { return !running && !paused; }
        std::chrono::nanoseconds elapsed_now() const { return running ? accumulated + (eng->time().monotonic_now() - run_start) : accumulated; }

        void start() {
            accumulated = {};
            delta_mark = {};
            run_start = eng->time().monotonic_now();
            running = true;
            paused = false;
        }
        double stop() {
            if (idle()) return 0.0;
            const auto e = elapsed_now();
            accumulated = {};
            delta_mark = {};
            running = paused = false;
            return seconds(e);
        }
        double delta() {
            if (idle()) { start(); return 0.0; }
            const auto e = elapsed_now();
            const auto d = e - delta_mark;
            delta_mark = e;
            return seconds(d);
        }
        double elapsed() const { return idle() ? 0.0 : seconds(elapsed_now()); }
        double pause() {
            if (running) {
                accumulated += eng->time().monotonic_now() - run_start;
                running = false;
                paused = true;
            }
            return seconds(accumulated);
        }
        double resume() {
            if (idle()) { start(); return 0.0; }
            if (running) return seconds(elapsed_now());
            run_start = eng->time().monotonic_now();
            running = true;
            paused = false;
            return seconds(accumulated);
        }
    };

    namespace time_calendar {
        constexpr int64_t floor_div(int64_t a, int64_t b) noexcept { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
        constexpr bool is_leap(int64_t y) noexcept { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
        constexpr int64_t days_in_month(int64_t y, int64_t m) noexcept {
            constexpr int8_t lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            return m == 2 && is_leap(y) ? 29 : lengths[m - 1];
        }
        // days since 1970-01-01 in the proleptic Gregorian calendar (H. Hinnant's civil algorithms)
        constexpr int64_t days_from_civil(int64_t y, int64_t m, int64_t d) noexcept {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const int64_t yoe = y - era * 400;
            const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + doe - 719468;
        }
        struct civil { int64_t year, month, day; };
        constexpr civil civil_from_days(int64_t z) noexcept {
            z += 719468;
            const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const int64_t doe = z - era * 146097;
            const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const int64_t mp = (5 * doy + 2) / 153;
            const int64_t d = doy - (153 * mp + 2) / 5 + 1;
            const int64_t m = mp < 10 ? mp + 3 : mp - 9;
            return {yoe + era * 400 + (m <= 2), m, d};
        }
        constexpr int64_t min_year = 1;
        constexpr int64_t max_year = 9999;
        constexpr int64_t min_unix_ms = days_from_civil(min_year, 1, 1) * 86400000;
        constexpr int64_t max_unix_ms = (days_from_civil(max_year, 12, 31) + 1) * 86400000 - 1;

        inline const char* month_name(int64_t m) {
            static const char* const names[] = {"January", "February", "March", "April", "May", "June", "July",
                                                "August", "September", "October", "November", "December"};
            return names[m - 1];
        }
        inline const char* weekday_name(int64_t w) {
            static const char* const names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
            return names[w];
        }
    }

    // time::date: a calendar date and time of day to the millisecond, in UTC or in the local zone
    struct date {
        int64_t year = 1970;
        int64_t month = 1;
        int64_t day = 1;
        int64_t hour = 0;
        int64_t minute = 0;
        int64_t second = 0;
        int64_t millisecond = 0;
        bool utc = true;
        int8_t dst = -1;   // the zone's daylight flag when read from an instant: keeps a local round trip exact in a repeated hour

        int64_t days() const { return time_calendar::days_from_civil(year, month, day); }
        int64_t weekday() const { const int64_t w = (days() + 4) % 7; return w < 0 ? w + 7 : w; }   // 0 = Sunday; 1970-01-01 was a Thursday
        int64_t yearday() const { return days() - time_calendar::days_from_civil(year, 1, 1) + 1; }

        static date from_unix_ms_utc(int64_t ms) {
            date d;
            const int64_t days = time_calendar::floor_div(ms, 86400000);
            int64_t rest = ms - days * 86400000;
            const auto c = time_calendar::civil_from_days(days);
            d.year = c.year; d.month = c.month; d.day = c.day;
            d.hour = rest / 3600000; rest %= 3600000;
            d.minute = rest / 60000; rest %= 60000;
            d.second = rest / 1000;
            d.millisecond = rest % 1000;
            return d;
        }
        // false when the platform cannot place the instant in the local zone
        static bool from_unix_ms_local(int64_t ms, date& out) {
            const int64_t secs = time_calendar::floor_div(ms, 1000);
            const std::time_t t = static_cast<std::time_t>(secs);
            if (static_cast<int64_t>(t) != secs) return false;
            std::tm tm{};
#ifdef _WIN32
            if (localtime_s(&tm, &t) != 0) return false;
#else
            if (!localtime_r(&t, &tm)) return false;
#endif
            out.year = tm.tm_year + 1900; out.month = tm.tm_mon + 1; out.day = tm.tm_mday;
            out.hour = tm.tm_hour; out.minute = tm.tm_min; out.second = tm.tm_sec > 59 ? 59 : tm.tm_sec;
            out.millisecond = ms - secs * 1000;
            out.utc = false;
            out.dst = static_cast<int8_t>(tm.tm_isdst > 0 ? 1 : 0);
            return true;
        }
        static bool from_unix_ms(int64_t ms, bool in_utc, date& out) {
            if (ms < time_calendar::min_unix_ms || ms > time_calendar::max_unix_ms) return false;
            if (in_utc) { out = from_unix_ms_utc(ms); return true; }
            return from_unix_ms_local(ms, out) && out.year >= time_calendar::min_year && out.year <= time_calendar::max_year;
        }
        bool unix_ms(int64_t& out) const {
            if (utc) {
                out = days() * 86400000 + hour * 3600000 + minute * 60000 + second * 1000 + millisecond;
                return true;
            }
            std::tm tm{};
            tm.tm_year = static_cast<int>(year - 1900); tm.tm_mon = static_cast<int>(month - 1); tm.tm_mday = static_cast<int>(day);
            tm.tm_hour = static_cast<int>(hour); tm.tm_min = static_cast<int>(minute); tm.tm_sec = static_cast<int>(second);
            tm.tm_isdst = dst;
            const std::time_t t = std::mktime(&tm);
            if (t == static_cast<std::time_t>(-1) && !(year == 1969 && month == 12 && day == 31)) return false;
            out = static_cast<int64_t>(t) * 1000 + millisecond;
            return true;
        }
        int64_t unix_ms_or_throw() const {
            int64_t ms = 0;
            if (!unix_ms(ms)) throw runtime_error("time::date: " + to_string() + " is outside the local time the platform can represent");
            return ms;
        }
        date in_zone(bool in_utc) const {
            date out;
            if (!from_unix_ms(unix_ms_or_throw(), in_utc, out)) throw runtime_error("time::date: " + to_string() + " is outside the supported range (years 1 to 9999)");
            return out;
        }
        date plus_ms(int64_t delta) const {
            const int64_t ms = unix_ms_or_throw();
            date out;
            if ((delta > 0 && ms > time_calendar::max_unix_ms - delta) || (delta < 0 && ms < time_calendar::min_unix_ms - delta) ||
                !from_unix_ms(ms + delta, utc, out)) {
                throw runtime_error("time::date: the result is outside the supported range (years 1 to 9999)");
            }
            return out;
        }
        date plus_days(int64_t n) const {
            const int64_t limit = time_calendar::days_from_civil(time_calendar::max_year, 12, 31) - time_calendar::days_from_civil(time_calendar::min_year, 1, 1);
            if (n > limit || n < -limit) throw runtime_error("time::date: the result is outside the supported range (years 1 to 9999)");
            const auto c = time_calendar::civil_from_days(days() + n);
            if (c.year < time_calendar::min_year || c.year > time_calendar::max_year) throw runtime_error("time::date: the result is outside the supported range (years 1 to 9999)");
            date out = *this;
            out.year = c.year; out.month = c.month; out.day = c.day; out.dst = -1;
            return out;
        }
        // calendar months: the day clamps to the last day of the target month (Jan 31 + 1 month = Feb 28 or 29)
        date plus_months(int64_t n) const {
            const int64_t limit = (time_calendar::max_year - time_calendar::min_year + 1) * 12;
            if (n > limit || n < -limit) throw runtime_error("time::date: the result is outside the supported range (years 1 to 9999)");
            const int64_t index = year * 12 + (month - 1) + n;
            const int64_t y = time_calendar::floor_div(index, 12);
            if (y < time_calendar::min_year || y > time_calendar::max_year) throw runtime_error("time::date: the result is outside the supported range (years 1 to 9999)");
            date out = *this;
            out.year = y;
            out.month = index - y * 12 + 1;
            out.day = day > time_calendar::days_in_month(y, out.month) ? time_calendar::days_in_month(y, out.month) : day;
            out.dst = -1;
            return out;
        }

        std::string two(int64_t v) const { return (v < 10 ? "0" : "") + std::to_string(v); }
        std::string to_string() const {
            std::string y = std::to_string(year);
            while (y.size() < 4) y.insert(y.begin(), '0');
            std::string ms = std::to_string(millisecond);
            while (ms.size() < 3) ms.insert(ms.begin(), '0');
            return y + "-" + two(month) + "-" + two(day) + "T" + two(hour) + ":" + two(minute) + ":" + two(second) + "." + ms + (utc ? "Z" : "");
        }

        // A field store checks its own range, and a store that leaves the day past the month's end is refused;
        // each setter answers the refusal text, empty when the store happened
        static std::string range_error(const char* field, int64_t v, int64_t lo, int64_t hi) {
            if (v >= lo && v <= hi) return {};
            return std::string("time::date: ") + field + " must be " + std::to_string(lo) + " to " + std::to_string(hi) + ", got " + std::to_string(v);
        }
        static std::string day_error(int64_t y, int64_t m, int64_t d) {
            if (d <= time_calendar::days_in_month(y, m)) return {};
            std::string ys = std::to_string(y);
            while (ys.size() < 4) ys.insert(ys.begin(), '0');
            return "time::date: day " + std::to_string(d) + " does not exist in " + ys + "-" + (m < 10 ? "0" : "") + std::to_string(m);
        }
        std::string store(int64_t& field, int64_t v, std::string error) {
            if (error.empty()) { field = v; dst = -1; }
            return error;
        }
        std::string set_year(int64_t v) {
            std::string e = range_error("year", v, time_calendar::min_year, time_calendar::max_year);
            return store(year, v, e.empty() ? day_error(v, month, day) : e);
        }
        std::string set_month(int64_t v) {
            std::string e = range_error("month", v, 1, 12);
            return store(month, v, e.empty() ? day_error(year, v, day) : e);
        }
        std::string set_day(int64_t v) {
            std::string e = range_error("day", v, 1, 31);
            return store(day, v, e.empty() ? day_error(year, month, v) : e);
        }
        std::string set_hour(int64_t v) { return store(hour, v, range_error("hour", v, 0, 23)); }
        std::string set_minute(int64_t v) { return store(minute, v, range_error("minute", v, 0, 59)); }
        std::string set_second(int64_t v) { return store(second, v, range_error("second", v, 0, 59)); }
        std::string set_millisecond(int64_t v) { return store(millisecond, v, range_error("millisecond", v, 0, 999)); }
    };

    // time:: namespace (docs/site/guide/14-time.html): clocks through the engine's time source
    // (engine_time::source), stopwatches and calendar dates (source/implementation/stdlib/time.cpp)
    void register_time_functions(engine& eng_ref);

} // namespace stdlib
} // namespace jai

#endif // __JAISCRIPT_STDLIB_TIME_HPP__
