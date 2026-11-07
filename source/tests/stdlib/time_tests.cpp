#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

namespace {
    using ns = std::chrono::nanoseconds;

    // A host time source the tests drive: advance(seconds) moves both clocks, reads are counted
    struct virtual_clock {
        ns mono = std::chrono::seconds(100);
        ns wall = ns(1700000000123000000LL);   // 2023-11-14T22:13:20.123Z
        int mono_reads = 0;
        int wall_reads = 0;
        std::vector<ns> sleeps;
    };

    std::shared_ptr<engine> make_timed(bool use_vm, virtual_clock& clk, bool install = true) {
        auto e = jai::foundry::make_default_engine();
        e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
        e->execution_budget(0);
        stdlib::register_all(e);
        engine* raw = e.get();
        e->add_function("advance", [&clk](script_float s) {
            const auto d = std::chrono::duration_cast<ns>(std::chrono::duration<double>(s));
            clk.mono += d;
            clk.wall += d;
        });
        e->add_function("nested", [raw](const std::string& src) -> std::string { return raw->execute(src).to_string(); });
        if (install) {
            e->time().source([&clk] { ++clk.mono_reads; return clk.mono; },
                           [&clk] { ++clk.wall_reads; return clk.wall; },
                           [&clk](ns d) { clk.sleeps.push_back(d); clk.mono += d; clk.wall += d; });
        }
        return e;
    }

    std::string run_timed(const std::string& src, bool use_vm) {
        virtual_clock clk;
        auto e = make_timed(use_vm, clk);
        try { return e->execute(src).to_string(); }
        catch (const std::exception& ex) { return std::string("ERROR: ") + ex.what(); }
    }
}

class time_stdlib_tests : public suite {
public:
    time_stdlib_tests() : suite("Time Stdlib Tests") {}

    void forge_tests() override {
        auto expect = [this](const std::string& src, const std::string& expected, const char* what) {
            for (bool use_vm : {false, true}) {
                check_eq(expected, run_timed(src, use_vm), std::string(what) + (use_vm ? " (vm)" : " (interpreter)"));
            }
        };
        auto expect_error = [this](const std::string& src, const std::string& fragment, const char* what) {
            for (bool use_vm : {false, true}) {
                const std::string got = run_timed(src, use_vm);
                check_true(got.rfind("ERROR:", 0) != 0 && got.find(fragment) != std::string::npos, std::string(what) + (use_vm ? " (vm): " : " (interpreter): ") + got);
            }
        };

        // ------------------------------------------------------------ stopwatch

        test("stopwatch_start_elapsed_stop", [expect]() {
            expect(R"(
                time::stopwatch sw;
                string out = `${sw.elapsed()} ${sw.is_running()} ${sw.is_paused()}`;
                sw.start(); advance(1.5);
                out += ` | ${sw.elapsed()} ${sw.is_running()}`;
                advance(0.5);
                out += ` | ${sw.stop()} ${sw.elapsed()} ${sw.is_running()} ${sw.stop()}`;
                out;)", "0.0 false false | 1.5 true | 2.0 0.0 false 0.0", "idle, running, stopped");
        });

        test("stopwatch_start_restarts_from_zero", [expect]() {
            expect(R"(
                time::stopwatch sw; sw.start(); advance(2.0);
                string out = `${sw.elapsed()}`;
                sw.start();
                out += ` ${sw.elapsed()}`;
                advance(1.0);
                out += ` ${sw.elapsed()}`;
                sw.pause(); advance(3.0); sw.start();
                out += ` ${sw.elapsed()} ${sw.is_running()} ${sw.is_paused()}`;
                out;)", "2.0 0.0 1.0 0.0 true false", "start on a running or paused stopwatch");
        });

        test("stopwatch_pause_and_resume", [expect]() {
            expect(R"(
                time::stopwatch sw; sw.start(); advance(1.0);
                string out = `${sw.pause()} ${sw.is_paused()} ${sw.is_running()}`;
                advance(5.0);
                out += ` | ${sw.elapsed()} ${sw.pause()}`;
                out += ` | ${sw.resume()} ${sw.is_running()} ${sw.is_paused()}`;
                advance(2.0);
                out += ` | ${sw.elapsed()} ${sw.resume()}`;
                out;)", "1.0 true false | 1.0 1.0 | 1.0 true false | 3.0 3.0", "paused time never counts");
            expect(R"(
                time::stopwatch a;
                string out = `${a.pause()} ${a.is_paused()} ${a.is_running()}`;
                out += ` | ${a.resume()} ${a.is_running()}`;
                advance(0.5);
                out += ` | ${a.elapsed()}`;
                out;)", "0.0 false false | 0.0 true | 0.5", "pause on idle is a no-op, resume on idle starts");
        });

        test("stopwatch_delta_sequences", [expect]() {
            expect(R"(
                time::stopwatch sw;
                string out = `${sw.delta()} ${sw.is_running()}`;
                advance(0.25); out += ` ${sw.delta()}`;
                advance(0.5); out += ` ${sw.delta()}`;
                out += ` ${sw.delta()}`;
                sw.pause(); advance(10.0);
                out += ` | ${sw.delta()}`;
                sw.resume(); advance(1.0);
                out += ` ${sw.delta()} | ${sw.elapsed()}`;
                out;)", "0.0 true 0.25 0.5 0.0 | 0.0 1.0 | 1.75", "delta starts, measures since the last delta, skips pauses");
            expect(R"(
                time::stopwatch sw; sw.start(); advance(1.0);
                string out = `${sw.delta()}`;
                advance(1.0);
                out += ` ${sw.stop()}`;
                out += ` ${sw.delta()} ${sw.is_running()}`;
                advance(0.5);
                out += ` ${sw.delta()} ${sw.elapsed()}`;
                out;)", "1.0 2.0 0.0 true 0.5 0.5", "stop resets elapsed and delta");
        });

        test("stopwatch_is_a_value", [expect]() {
            expect(R"(
                function lap(time::stopwatch s) { s.stop(); return s.is_running(); }
                time::stopwatch a; a.start(); advance(1.0);
                auto b = a;
                time::stopwatch c = a;
                b.pause(); advance(1.0);
                c.stop();
                string out = `${a.elapsed()} ${b.elapsed()} ${c.elapsed()} ${a.is_running()} ${b.is_paused()} ${c.is_running()}`;
                out += ` | ${lap(a)} ${a.is_running()} ${a.elapsed()}`;
                out;)", "2.0 1.0 0.0 true true false | false true 2.0", "copies are independent");
            expect(R"(
                class Lap { time::stopwatch sw; }
                Lap l; l.sw.start(); advance(0.5);
                Lap m = l;
                m.sw.stop();
                `${l.sw.is_running()} ${l.sw.elapsed()} ${m.sw.is_running()} ${type_of(l.sw)}`;)",
                "true 0.5 false time::stopwatch", "a stopwatch field copies with its instance");
        });

        test("stopwatch_reads_the_clock_once_per_call", [this]() {
            for (bool use_vm : {false, true}) {
                virtual_clock clk;
                auto e = make_timed(use_vm, clk);
                check_eq(0, clk.mono_reads, "construction reads nothing");
                auto reads = [&](const std::string& src) {
                    const int m = clk.mono_reads, w = clk.wall_reads;
                    e->execute(src);
                    return std::to_string(clk.mono_reads - m) + "/" + std::to_string(clk.wall_reads - w);
                };
                const char* be = use_vm ? " (vm)" : " (interpreter)";
                check_eq(std::string("0/0"), reads("time::stopwatch sw;"), std::string("declaration") + be);
                check_eq(std::string("1/0"), reads("sw.start();"), std::string("start") + be);
                check_eq(std::string("1/0"), reads("sw.elapsed();"), std::string("elapsed while running") + be);
                check_eq(std::string("1/0"), reads("sw.delta();"), std::string("delta while running") + be);
                check_eq(std::string("1/0"), reads("sw.pause();"), std::string("pause") + be);
                check_eq(std::string("0/0"), reads("sw.elapsed(); sw.pause(); sw.delta(); sw.is_running(); sw.is_paused();"), std::string("paused reads") + be);
                check_eq(std::string("1/0"), reads("sw.resume();"), std::string("resume") + be);
                check_eq(std::string("1/0"), reads("sw.stop();"), std::string("stop") + be);
                check_eq(std::string("0/0"), reads("sw.stop(); sw.elapsed(); sw.pause();"), std::string("idle reads") + be);
                check_eq(std::string("1/0"), reads("time::now();"), std::string("now") + be);
                check_eq(std::string("1/0"), reads("time::engine_elapsed();"), std::string("engine_elapsed") + be);
                check_eq(std::string("0/1"), reads("time::unix_time();"), std::string("unix_time") + be);
                check_eq(std::string("0/1"), reads("time::unix_ms();"), std::string("unix_ms") + be);
                check_eq(std::string("0/1"), reads("time::date_now(\"${year}\");"), std::string("date_now") + be);
                check_eq(std::string("0/1"), reads("time::local_now();"), std::string("local_now") + be);
                check_eq(std::string("0/0"), reads("time::utc_date(2024, 1, 1).add_days(3).format(\"${day}\");"), std::string("pure date work") + be);
            }
        });

        // ------------------------------------------------------------ engine and entry clocks

        test("now_and_engine_elapsed_follow_the_source", [this, expect]() {
            expect(R"(`${time::now()} ${time::engine_elapsed()}`;)", "100.0 100.0", "a host source's reading is engine_elapsed");
            expect(R"(advance(1.5); `${time::now()} ${time::engine_elapsed()}`;)", "101.5 101.5", "advancing the host clock");
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(e);
                auto r = e->execute(R"(var a = time::now(); var b = time::engine_elapsed(); var c = time::now(); a >= 0.0 && b >= a && c >= b && a < 3600.0;)");
                check_true(r.as<bool>(), "the system source counts from construction");
                check_true(e->engine_elapsed() >= 0.0, "the host reads the same clock");
            }
        });

        test("execute_elapsed_measures_the_running_host_entry", [this]() {
            for (bool use_vm : {false, true}) {
                virtual_clock clk;
                auto e = make_timed(use_vm, clk);
                const char* be = use_vm ? " (vm)" : " (interpreter)";
                check_eq(std::string("2.0"), e->execute("advance(2.0); time::execute_elapsed();").to_string(), std::string("an execute") + be);
                check_eq(std::string("0.0"), e->execute("time::execute_elapsed();").to_string(), std::string("the next execute starts over") + be);
                e->execute("function f() { advance(0.5); return time::execute_elapsed(); }");
                auto f = e->get_variable("f");
                check_eq(std::string("0.5"), f.as_function()({}).value().to_string(), std::string("a script function the host calls") + be);
                check_eq(std::string("2.0 2.0"), e->execute(R"JAI(advance(1.0); var inner = nested("advance(1.0); time::execute_elapsed()"); `${inner} ${time::execute_elapsed()}`;)JAI").to_string(),
                         std::string("a nested execute belongs to the running entry") + be);
                const int before = clk.mono_reads;
                e->execute("1;");
                check_eq(1, clk.mono_reads - before, std::string("an entry reads the clock once") + be);
            }
        });

        test("execute_elapsed_without_a_literal_mention_counts_from_its_first_call", [this]() {
            for (bool use_vm : {false, true}) {
                virtual_clock clk;
                auto e = make_timed(use_vm, clk);
                const char* be = use_vm ? " (vm)" : " (interpreter)";
                const int before = clk.mono_reads;
                e->execute("1;");
                check_eq(0, clk.mono_reads - before, std::string("no program names execute_elapsed: entries read nothing") + be);
                check_eq(std::string("0.0 1.0"), e->execute(R"(advance(1.0); var a = reflect::call("time::execute_elapsed"); advance(1.0); var b = reflect::call("time::execute_elapsed"); `${a} ${b}`;)").to_string(),
                         std::string("the first call stamps the entry") + be);
                check_eq(std::string("1.0"), e->execute(R"(advance(1.0); reflect::call("time::execute_elapsed");)").to_string(), std::string("later entries are stamped") + be);
            }
        });

        test("a_loaded_jaibite_that_names_execute_elapsed_stamps_its_entries", [this]() {
            for (bool use_vm : {false, true}) {
                virtual_clock saving_clock;
                auto saver = make_timed(use_vm, saving_clock);
                auto bytes = saver->jaibite("advance(2.0); time::execute_elapsed();").save_bytes();
                virtual_clock clk;
                auto e = make_timed(use_vm, clk);
                auto bite = e->jaibite_load_bytes(bytes.data(), bytes.size(), "");
                check_eq(std::string("2.0"), e->execute(bite).to_string(), use_vm ? "vm" : "interpreter");
            }
        });

        // ------------------------------------------------------------ wall clock

        test("unix_time_and_unix_ms", [this, expect]() {
            expect(R"(`${time::unix_ms()} ${(time::unix_time() - 1700000000.0 > 0.1229)} ${(time::unix_time() - 1700000000.0 < 0.1231)}`;)", "1700000000123 true true", "a host wall clock");
            expect(R"(advance(0.5); `${time::unix_ms()}`;)", "1700000000623", "advancing the wall clock");
            for (bool use_vm : {false, true}) {
                auto e = jai::foundry::make_default_engine();
                e->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(e);
                // each reading lies between the ones taken before and after it, however long the machine takes between them
                auto host_ms = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); };
                const auto before_ms = host_ms();
                const auto script_ms = e->execute("time::unix_ms();").as_int();
                const auto after_ms = host_ms();
                check_true(before_ms <= script_ms && script_ms <= after_ms, "the system wall clock");
                check_true(e->execute("var m0 = time::unix_ms(); var t = time::unix_time(); var m1 = time::unix_ms(); m0 <= t * 1000.0 + 1.0 && t * 1000.0 < m1 + 1.0;").as<bool>(), "unix_time and unix_ms agree");
            }
        });

        test("date_now_matches_local_now_format", [expect, expect_error]() {
            const std::string all = "${year}|${month}|${month_name}|${month_short}|${day}|${weekday_name}|${weekday_short}|${hours}|${military_hours}|${minutes}|${seconds}|${milliseconds}|${am_pm}";
            expect("time::date_now(\"" + all + "\") == time::local_now().format(\"" + all + "\");", "true", "every placeholder, one frozen instant");
            expect(R"(time::date_now("${year}-${month:02}-${day:02} ${military_hours:02}:${minutes:02}:${seconds:02}.${milliseconds:03}") == time::local_now().format("${year}-${month:02}-${day:02} ${military_hours:02}:${minutes:02}:${seconds:02}.${milliseconds:03}");)", "true", "format specs");
            expect(R"(var d = time::local_now(); `${time::date_now("${milliseconds:03}")} ${d.millisecond}`;)", "123 123", "milliseconds come from the wall clock");
            expect_error(R"(try { time::date_now("${nope}"); } catch (e) { return e; } return "no-error";)", "time::date_now: unknown placeholder '${nope}'", "an unknown placeholder");
            expect_error(R"(try { var n = 5; time::date_now(n); } catch (e) { return e; } return "no-error";)", "time::date_now expects a string template", "a non-string template");
        });

        // ------------------------------------------------------------ dates

        test("date_format_placeholders_and_specs", [expect, expect_error]() {
            expect(R"(time::utc_date(2026, 9, 27, 14, 3, 5, 7).format("${year}|${month}|${month_name}|${month_short}|${day}|${weekday_name}|${weekday_short}|${hours}|${military_hours}|${minutes}|${seconds}|${milliseconds}|${am_pm}");)",
                   "2026|9|September|Sep|27|Sunday|Sun|2|14|3|5|7|PM", "every placeholder");
            expect(R"(time::utc_date(2026, 9, 27, 14, 3, 5, 7).format("${day:02}/${month:02}/${year} ${hours:02}:${minutes:02}:${seconds:02}.${milliseconds:03} ${am_pm}");)",
                   "27/09/2026 02:03:05.007 PM", "zero padding");
            expect(R"(time::utc_date(2026, 9, 27, 14, 3, 5, 7).format("[${month_name:>10}] [${month_short:*^7}] [${year:x}] [${milliseconds:+}] [${weekday_name:<8}]");)",
                   "[ September] [**Sep**] [7ea] [+7] [Sunday  ]", "align, fill, hex, sign");
            expect(R"(var f = "${hours} ${am_pm}"; time::utc_date(2026, 1, 1).format(f) + ", " + time::utc_date(2026, 1, 1, 12).format(f) + ", " + time::utc_date(2026, 1, 1, 23).format(f) + ", " + time::utc_date(2026, 1, 1, 1).format(f);)",
                   "12 AM, 12 PM, 11 PM, 1 AM", "12-hour clock");
            expect(R"(time::utc_date(2026, 5, 3).format("cost $5 on ${month_short} ${day}, no braces {day}");)", "cost $5 on May 3, no braces {day}", "text outside placeholders");
            expect_error(R"(var d = time::utc_date(2026, 1, 1); try { d.format("${bogus}"); } catch (e) { return e; } return "no-error";)", "time::date::format: unknown placeholder '${bogus}'", "unknown placeholder");
            expect_error(R"(var d = time::utc_date(2026, 1, 1); try { d.format("x ${year"); } catch (e) { return e; } return "no-error";)", "time::date::format: unterminated placeholder '${year'", "unterminated placeholder");
            expect_error(R"(var d = time::utc_date(2026, 1, 1); try { d.format("${day:q}"); } catch (e) { return e; } return "no-error";)", "time::date::format: unsupported format spec ':q' in '${day:q}'", "unsupported spec");
            expect_error(R"(var d = time::utc_date(2026, 1, 1); try { d.format("${month_name:05}"); } catch (e) { return e; } return "no-error";)", "time::date::format: format spec ':05' does not apply to string value", "spec for another type");
        });

        test("date_fields_and_validation", [expect, expect_error]() {
            expect(R"(var d = time::utc_date(2024, 2, 29, 23, 59, 58, 999);
                `${d.year}-${d.month}-${d.day} ${d.hour}:${d.minute}:${d.second}.${d.millisecond} ${d.weekday} ${d.yearday} ${d.utc} ${d.to_string()}`;)",
                   "2024-2-29 23:59:58.999 4 60 true 2024-02-29T23:59:58.999Z", "field reads");
            expect(R"(time::date z; string before = z.to_string(); z.year = 2000; z.month = 2; z.day = 29; z.hour = 12; z.minute = 30; z.second = 15; z.millisecond = 250; `${before} ${z.to_string()} ${type_of(z)}`;)",
                   "1970-01-01T00:00:00.000Z 2000-02-29T12:30:15.250Z time::date", "default date and field stores");
            expect(R"(`${time::utc_date(2026, 12, 31).yearday} ${time::utc_date(2024, 12, 31).yearday} ${time::utc_date(1970, 1, 1).weekday} ${time::utc_date(2000, 1, 1).weekday}`;)",
                   "365 366 4 6", "yearday and weekday");
            const std::string z = "var z = time::utc_date(2000, 2, 29, 12, 30, 15, 250); ";
            const std::pair<const char*, const char*> bad[] = {
                {"z.month = 13;", "time::date: month must be 1 to 12, got 13"},
                {"z.day = 0;", "time::date: day must be 1 to 31, got 0"},
                {"z.day = 30;", "time::date: day 30 does not exist in 2000-02"},
                {"z.year = 2001;", "time::date: day 29 does not exist in 2001-02"},
                {"z.year = 0;", "time::date: year must be 1 to 9999, got 0"},
                {"z.year = 10000;", "time::date: year must be 1 to 9999, got 10000"},
                {"z.hour = 24;", "time::date: hour must be 0 to 23, got 24"},
                {"z.minute = 60;", "time::date: minute must be 0 to 59, got 60"},
                {"z.second = -1;", "time::date: second must be 0 to 59, got -1"},
                {"z.millisecond = 1000;", "time::date: millisecond must be 0 to 999, got 1000"},
                {"z.weekday = 3;", "Cannot assign to 'weekday': it is read-only"},
                {"z.utc = false;", "Cannot assign to 'utc': it is read-only"},
            };
            for (const auto& [store, text] : bad) {
                expect_error(z + "try { " + store + " } catch (e) { return `${e} / ${z.to_string()}`; } return \"no-error\";", text, store);
                expect_error(z + "try { " + store + " } catch (e) { return `${e} / ${z.to_string()}`; } return \"no-error\";", "/ 2000-02-29T12:30:15.250Z", "a refused store leaves the date unchanged");
            }
            expect_error(R"(try { time::utc_date(2023, 2, 29); } catch (e) { return e; } return "no-error";)", "time::date: day 29 does not exist in 2023-02", "constructor validation");
            expect_error(R"(try { time::local_date(2023, 4, 31); } catch (e) { return e; } return "no-error";)", "time::date: day 31 does not exist in 2023-04", "local constructor validation");
            expect_error(R"(try { var f = 1.5; time::utc_date(2024, f, 1); } catch (e) { return e; } return "no-error";)", "time::utc_date expects integer fields", "fields are integers");
            expect_error(R"(try { var args = [2024, 1]; reflect::call("time::utc_date", 2024, 1); } catch (e) { return e; } return "no-error";)", "time::utc_date expects 3 to 7 arguments", "field count");
        });

        test("date_add_arithmetic", [expect, expect_error]() {
            expect(R"(var d = time::utc_date(2024, 1, 31, 10, 0, 0);
                d.add_months(1).to_string() + " " + time::utc_date(2023, 1, 31).add_months(1).to_string() + " " + time::utc_date(2024, 3, 31).add_months(-1).to_string() + " " + d.to_string();)",
                   "2024-02-29T10:00:00.000Z 2023-02-28T00:00:00.000Z 2024-02-29T00:00:00.000Z 2024-01-31T10:00:00.000Z", "month-end clamping, the receiver unchanged");
            expect(R"(time::utc_date(2024, 12, 15).add_months(1).format("${year}-${month}-${day}") + " " + time::utc_date(2024, 1, 15).add_months(-13).format("${year}-${month}-${day}") + " " + time::utc_date(2024, 5, 31).add_months(4).format("${year}-${month}-${day}");)",
                   "2025-1-15 2022-12-15 2024-9-30", "months across years");
            expect(R"(var leap = time::utc_date(2024, 2, 29); leap.add_years(1).format("${year}-${month}-${day}") + " " + leap.add_years(4).format("${year}-${month}-${day}") + " " + leap.add_years(-4).format("${year}-${month}-${day}") + " " + leap.add_years(-1).format("${year}-${month}-${day}");)",
                   "2025-2-28 2028-2-29 2020-2-29 2023-2-28", "years from a leap day");
            expect(R"(time::utc_date(2023, 12, 31, 23).add_days(1).to_string() + " " + time::utc_date(1900, 2, 28).add_days(1).format("${year}-${month}-${day}") + " " + time::utc_date(2000, 2, 28).add_days(1).format("${year}-${month}-${day}") + " " + time::utc_date(2100, 2, 28).add_days(1).format("${year}-${month}-${day}") + " " + time::utc_date(2024, 3, 1).add_days(-1).format("${year}-${month}-${day}") + " " + time::utc_date(2024, 1, 1).add_days(366).format("${year}-${month}-${day}");)",
                   "2024-01-01T23:00:00.000Z 1900-3-1 2000-2-29 2100-3-1 2024-2-29 2025-1-1", "days across months, years and century leap rules");
            expect(R"(time::utc_date(2024, 2, 28, 23, 30).add_hours(25).to_string() + " " + time::utc_date(2024, 1, 1, 0, 30).add_minutes(-90).to_string() + " " + time::utc_date(2024, 1, 1).add_seconds(1.5).to_string() + " " + time::utc_date(2024, 1, 1).add_seconds(-0.001).to_string() + " " + time::utc_date(2024, 1, 1).add_hours(0.5).to_string();)",
                   "2024-03-01T00:30:00.000Z 2023-12-31T23:00:00.000Z 2024-01-01T00:00:01.500Z 2023-12-31T23:59:59.999Z 2024-01-01T00:30:00.000Z", "hours, minutes and seconds");
            expect_error(R"(try { time::utc_date(2024, 1, 1).add_days(1.5); } catch (e) { return e; } return "no-error";)", "time::date::add_days expects a whole number", "whole days");
            expect_error(R"(try { time::utc_date(2024, 1, 1).add_days(9223372036854775807); } catch (e) { return e; } return "no-error";)", "time::date::add_days is out of range", "a whole number past the step range");
            expect_error(R"(try { time::utc_date(9999, 12, 31).add_days(1); } catch (e) { return e; } return "no-error";)", "time::date: the result is outside the supported range (years 1 to 9999)", "range");
            expect_error(R"(try { time::utc_date(1, 1, 1).add_seconds(-1); } catch (e) { return e; } return "no-error";)", "time::date: the result is outside the supported range (years 1 to 9999)", "range below");
        });

        test("date_subtraction_comparisons_between", [expect]() {
            const std::string ab = "var a = time::utc_date(2024, 1, 1); var b = time::utc_date(2024, 1, 2, 12, 0, 0, 500); ";
            expect(ab + R"(`${(b - a)} ${(a - b)} ${(a < b)} ${(a <= b)} ${(a > b)} ${(a >= b)} ${(a == b)} ${(a != b)}`;)",
                   "129600.5 -129600.5 true true false false false true", "subtraction and every comparison");
            expect(ab + R"(var c = time::utc_date(2024, 1, 1); `${(a == c)} ${(a <= c)} ${(a >= c)} ${(a < c)} ${(a != c)}`;)",
                   "true true true false false", "equal instants");
            expect(ab + R"(var m = time::utc_date(2024, 1, 1, 12); `${m.between(a, b)} ${a.between(a, b)} ${b.between(a, b)} ${m.between(b, a)} ${b.add_seconds(0.001).between(a, b)} ${a.add_seconds(-0.001).between(b, a)}`;)",
                   "true true true true false false", "inclusive ranges, either order");
            expect(R"(var l = time::date_from_unix(1700000000); var u = time::date_from_unix(1700000000, true); `${(l == u)} ${(l - u)} ${l.between(u, u)} ${l.utc} ${u.utc}`;)",
                   "true 0.0 true false true", "local and UTC dates compare as instants");
        });

        test("date_operators_take_dates", [expect, expect_error]() {
            expect(R"(var d = time::utc_date(2024, 1, 1); var five = 5; `${(d == five)} ${(d != five)} ${(d == null)} ${(d != null)}`;)", "false true false true", "equality with a non-date");
            expect_error(R"(var d = time::utc_date(2024, 1, 1); var five = 5; try { var r = d < five; } catch (e) { return e; } return "no-error";)", "time::date: '<' needs two dates, got int", "ordering needs two dates");
            expect_error(R"(var d = time::utc_date(2024, 1, 1); var s = "x"; try { var r = d - s; } catch (e) { return e; } return "no-error";)", "time::date: '-' needs two dates, got string", "subtraction needs two dates");
            expect_error(R"(var z = time::utc_date(2024, 1, 1); try { z.hour = 1.5; } catch (e) { return e; } return "no-error";)", "time::date: hour takes an int, got float", "a field takes an int");
        });

        test("date_unix_round_trips", [expect, expect_error]() {
            expect(R"(time::date_from_unix(0, true).to_string() + " " + time::date_from_unix(1700000000.5, true).to_string() + " " + time::date_from_unix(-1, true).to_string() + " " + time::date_from_unix(-0.001, true).to_string();)",
                   "1970-01-01T00:00:00.000Z 2023-11-14T22:13:20.500Z 1969-12-31T23:59:59.000Z 1969-12-31T23:59:59.999Z", "UTC from unix seconds");
            expect(R"(var d = time::utc_date(2023, 11, 14, 22, 13, 20, 500); `${d.unix()} ${d.unix_ms()} ${(time::date_from_unix(123456789.25, true).unix() == 123456789.25)}`;)",
                   "1700000000.5 1700000000500 true", "unix and unix_ms");
            expect(R"(time::date_from_unix(-62135596800, true).to_string() + " " + time::date_from_unix(253402300799.999, true).to_string();)",
                   "0001-01-01T00:00:00.000Z 9999-12-31T23:59:59.999Z", "the supported range's ends");
            expect_error(R"(try { time::date_from_unix(-62135596801, true); } catch (e) { return e; } return "no-error";)", "time::date_from_unix: the instant is outside the supported range (years 1 to 9999)", "below the range");
            expect_error(R"(try { time::date_from_unix(1e13, true); } catch (e) { return e; } return "no-error";)", "time::date_from_unix: the instant is outside the supported range (years 1 to 9999)", "above the range");
            expect_error(R"(try { var s = "x"; time::date_from_unix(s); } catch (e) { return e; } return "no-error";)", "time::date_from_unix expects a number of seconds", "seconds are a number");
            expect_error(R"(try { var flag = 1; time::date_from_unix(5, flag); } catch (e) { return e; } return "no-error";)", "time::date_from_unix expects a bool for utc", "utc is a bool");
        });

        test("local_dates_are_consistent_with_utc", [expect]() {
            expect(R"(var t = 1700000000;
                var l = time::date_from_unix(t);
                `${l.utc} ${(l.unix() == t)} ${(l.to_utc().to_string() == time::date_from_unix(t, true).to_string())} ${(l.to_utc().to_local().to_string() == l.to_string())} ${(time::date_from_unix(t, true).to_local().to_string() == l.to_string())}`;)",
                   "false true true true true", "one instant in both zones");
            expect(R"(var x = time::local_date(2024, 6, 15, 12, 0, 0, 250);
                var back = time::date_from_unix(x.unix());
                `${(back.to_string() == x.to_string())} ${x.utc} ${x.format("${year}-${month}-${day} ${military_hours}:${minutes}.${milliseconds}")} ${(x.to_utc() == x)}`;)",
                   "true false 2024-6-15 12:0.250 true", "a local date from fields");
            expect(R"(var l = time::date_from_unix(1700000000); l.hour = 12; l.minute = 0; var back = time::date_from_unix(l.unix()); `${back.hour} ${back.minute} ${(back.day == l.day)}`;)",
                   "12 0 true", "a local field store then unix");
            expect(R"(`${(time::local_now() == time::utc_now())} ${(time::local_now().to_utc().to_string() == time::utc_now().to_string())} ${(time::utc_now().unix_ms() == time::unix_ms())} ${time::utc_now().to_string()} ${time::local_now().utc}`;)",
                   "true true true 2023-11-14T22:13:20.123Z false", "now in both zones");
        });

        test("qualified_type_names_everywhere", [expect]() {
            expect(R"(
                function span(time::date a, time::date b) -> float { return b - a; }
                time::date later(time::date d) { return d.add_days(1); }
                class Event { time::date when; time::stopwatch clock; }
                Event ev; ev.when = time::utc_date(2024, 5, 1);
                array<time::date> list = [time::utc_date(2024, 1, 1), time::utc_date(2024, 1, 2)];
                time::date& first = list[0];
                first = first.add_days(10);
                var c = cast<time::date>(list[1]);
                shared_ptr<time::date> h = new time::date();
                time::date direct = time::utc_date(2020, 1, 1);
                string names = "";
                for (time::date x : list) { names += x.format("${day} "); }
                `${span(list[1], list[0])} ${later(ev.when).format("${month}/${day}")} ${names}${type_of(c)} ${h.year} ${direct.year} ${ev.clock.is_running()} ${type_of(time::stopwatch())}`;)",
                   "777600.0 5/2 11 2 time::date 1970 2020 false time::stopwatch", "parameters, returns, fields, arrays, references, casts, handles, loops");
        });

        // ------------------------------------------------------------ the source, sleep, parallel regions, the checker

        test("time_source_replacement_and_sleep", [this]() {
            for (bool use_vm : {false, true}) {
                virtual_clock clk;
                auto e = make_timed(use_vm, clk);
                const char* be = use_vm ? " (vm)" : " (interpreter)";
                e->execute("sleep(0.25); sleep(0.01); sleep(0); sleep(-1); sleep(0.0); sleep(-0.005);");
                check_eq((size_t)2, clk.sleeps.size(), std::string("only positive sleeps reach the source") + be);
                if (clk.sleeps.size() == 2) {
                    check_eq((int64_t)250000000, (int64_t)clk.sleeps[0].count(), std::string("sleep(seconds)") + be);
                    check_eq((int64_t)10000000, (int64_t)clk.sleeps[1].count(), std::string("a fraction of a second") + be);
                }
                check_eq(std::string("100.26"), e->execute("time::now();").to_string(), std::string("a sleep advances a host clock") + be);
                check_near(100.26, e->engine_elapsed(), 1e-9);
                check_eq((int64_t)std::chrono::nanoseconds(clk.mono).count(), (int64_t)e->time().monotonic_now().count(), std::string("monotonic_now") + be);
                check_eq((int64_t)clk.wall.count(), (int64_t)e->time().wall_now().count(), std::string("wall_now") + be);
                const int reads = clk.mono_reads;
                e->time().source({}, {}, {});
                check_true(e->execute("var a = time::now(); a >= 0.0 && a < 3600.0;").as<bool>(), std::string("the system source is back") + be);
                check_eq(reads, clk.mono_reads, std::string("the host clock is no longer read") + be);
                auto errors = e->execute(R"(var out = ""; var s = "x"; try { sleep(s); } catch (e) { out += e; } out += " | "; try { reflect::call("sleep", 1, 2); } catch (e) { out += e; } out;)").to_string();
                check_eq(std::string("sleep expects a number of seconds | No overload of 'sleep' takes (int, int)"), errors, std::string("sleep errors") + be);
            }
        });

        test("reflection_lists_time_functions_and_their_signatures", [expect]() {
            expect(R"(var args = reflect::function_arguments("time::date_from_unix")[0];
                string out = "";
                for (auto p : args) { out += `${p["name"]}:${p["type"]}:${p["default"]} `; }
                var fns = reflect::functions();
                `${out}${fns.contains("time::now")} ${fns.contains("time::utc_date")} ${reflect::function_arguments("time::now")[0].size()}`;)",
                   "arg0:float:false arg1:bool:true true true 0", "declared signatures answer reflection");
        });

        test("a_simulation_clock_drives_scripts", [this]() {
            for (bool use_vm : {false, true}) {
                auto eng = jai::foundry::make_default_engine();
                eng->set_backend(use_vm ? backend_type::vm : backend_type::interpreter);
                stdlib::register_all(eng);
                auto out = std::make_shared<std::ostringstream>();
                eng->set_output_stream(out);
                std::chrono::nanoseconds sim_now{0};
                const auto epoch = std::chrono::nanoseconds(1700000000000000000LL);
                eng->time().source(
                    [&] { return sim_now; },
                    [&] { return epoch + sim_now; },
                    [&](std::chrono::nanoseconds d) { sim_now += d; });
                sim_now += std::chrono::milliseconds(1500);
                eng->execute("print(time::now(), ' ', time::utc_now().to_string());");
                eng->execute("sleep(2.0); print(time::now());");
                check_eq(std::string("1.5 2023-11-14T22:13:21.500Z\n3.5\n"), out->str(), use_vm ? "vm" : "interpreter");
            }
        });

        test("parallel_bodies_reject_time_calls", [this]() {
            for (bool use_vm : {false, true}) {
                virtual_clock clk;
                auto e = make_timed(use_vm, clk);
                const std::string what = use_vm ? "vm" : "interpreter";
                auto r = e->execute(R"(
                    array<float> xs = [1.0, 2.0, 3.0];
                    try { parallel_for (auto& x : xs) { x = time::now(); } } catch (e) { return e; }
                    return "no-error";)").to_string();
                check_true(r.find("time:: calls are not allowed in a parallel body (engine-owned clock)") != std::string::npos, "parallel_for (" + what + "): " + r);
                auto t = e->execute(R"(
                    float stamp(float x) { return x + time::unix_time(); }
                    try { parallel_transform([1.0, 2.0], stamp); } catch (e) { return e; }
                    return "no-error";)").to_string();
                check_true(t.find("time:: calls are not allowed in a parallel body (engine-owned clock)") != std::string::npos, "parallel_transform (" + what + "): " + t);
            }
        });

        test("the_checker_knows_time_signatures_and_members", [this]() {
            virtual_clock clk;
            auto e = make_timed(false, clk);
            auto first_error = [&](const std::string& src) {
                const check_report r = e->check(src);
                for (const auto& d : r.diagnostics) {
                    if (d.severity == check_diagnostic::level::error) { return d.message; }
                }
                return std::string("clean");
            };
            check_eq(std::string("'time::now' expects 0 argument(s), got 1"), first_error("time::now(5);"), "arity");
            check_eq(std::string("'time::date_from_unix' expects 1 to 2 argument(s), got 3"), first_error("time::date_from_unix(1, true, 3);"), "an arity range");
            check_eq(std::string("argument 1 of 'sleep': cannot pass 'string' where a number is expected"), first_error("sleep(\"x\");"), "an argument type");
            check_eq(std::string("argument 2 of 'time::date_from_unix': cannot pass 'int' where a bool is expected"), first_error("time::date_from_unix(1, 1);"), "a bool argument");
            check_eq(std::string("no member 'bogus' in class 'time::stopwatch'"), first_error("time::stopwatch sw; sw.bogus();"), "a stopwatch member");
            check_eq(std::string("no member 'nope' in class 'time::date'"), first_error("auto d = time::utc_now(); d.nope;"), "a date member");
            check_true(first_error("int n = time::date_now(\"${year}\");").find("string") != std::string::npos, "a string result into an int: " + first_error("int n = time::date_now(\"${year}\");"));
            check_eq(std::string("no member 'nope' in class 'time::date'"), first_error("time::local_now().nope;"), "a result's members");
            const std::string clean = R"(
                time::stopwatch sw; sw.start(); float e1 = sw.elapsed(); float d1 = sw.delta(); bool r = sw.is_running();
                float n = time::now(); float ee = time::engine_elapsed(); float xe = time::execute_elapsed(); float ut = time::unix_time(); int um = time::unix_ms();
                string s = time::date_now("${year}"); time::date d = time::local_now(); time::date u = time::utc_now(); time::date f = time::date_from_unix(0, true);
                time::date g = time::utc_date(2024, 1, 1); time::date h = time::local_date(2024, 1, 1, 12); int y = d.year; d.month = 3; float gap = u - d;
                bool b = d.between(f, g) && d < g; string iso = d.to_string(); time::date t = d.add_days(1).add_hours(1.5).to_utc(); time::stopwatch w = time::stopwatch();
                sleep(0); sleep(0.0);)";
            const check_report r = e->check(clean);
            check_eq((size_t)0, r.diagnostics.size(), "a clean program: " + (r.diagnostics.empty() ? std::string() : r.diagnostics[0].message));
        });
    }
};

} // namespace jai::foundry::tests

using time_stdlib_tests = jai::foundry::tests::time_stdlib_tests;
FOUNDRY_REGISTER(time_stdlib_tests)
