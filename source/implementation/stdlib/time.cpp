#include <jaiscript/stdlib/time.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/format_spec.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace jai {
namespace stdlib {

    // Expands ${name} / ${name:spec} placeholders (the template-string spec mini-language); an empty result
    // means success, else the error text
    inline std::string format_date(engine* eng, const date& d, std::string_view tmpl, const char* who, std::string& out) {
        out.clear();
        size_t at = 0;
        while (at < tmpl.size()) {
            const size_t open = tmpl.find("${", at);
            if (open == std::string_view::npos) { out.append(tmpl.substr(at)); break; }
            out.append(tmpl.substr(at, open - at));
            const size_t close = tmpl.find('}', open + 2);
            if (close == std::string_view::npos) {
                return std::string(who) + ": unterminated placeholder '" + std::string(tmpl.substr(open)) + "'";
            }
            const std::string_view body = tmpl.substr(open + 2, close - open - 2);
            const size_t colon = body.find(':');
            const std::string_view name = body.substr(0, colon);
            const bool has_spec = colon != std::string_view::npos;
            const std::string_view spec = has_spec ? body.substr(colon + 1) : std::string_view{};
            int64_t number = 0;
            const char* text = nullptr;
            if (name == "year") number = d.year;
            else if (name == "month") number = d.month;
            else if (name == "month_name") text = time_calendar::month_name(d.month);
            else if (name == "month_short") text = time_calendar::month_name(d.month);
            else if (name == "day") number = d.day;
            else if (name == "weekday_name" || name == "weekday_short") text = time_calendar::weekday_name(d.weekday());
            else if (name == "hours") number = d.hour % 12 == 0 ? 12 : d.hour % 12;
            else if (name == "military_hours") number = d.hour;
            else if (name == "minutes") number = d.minute;
            else if (name == "seconds") number = d.second;
            else if (name == "milliseconds") number = d.millisecond;
            else if (name == "am_pm") text = d.hour < 12 ? "AM" : "PM";
            else return std::string(who) + ": unknown placeholder '${" + std::string(name) + "}'";
            std::string value = text ? std::string(text) : std::to_string(number);
            if (text && (name == "month_short" || name == "weekday_short")) value.resize(3);
            if (!has_spec) { out += value; }
            else {
                detail::format_spec fs;
                if (!detail::parse_format_spec(spec, fs)) {
                    return std::string(who) + ": unsupported format spec ':" + std::string(spec) + "' in '${" + std::string(body) + "}'";
                }
                try {
                    out += detail::format_value_with_spec(text ? script_value(value, eng) : script_value(number, eng), fs, spec);
                } catch (const std::exception& e) {
                    return std::string(who) + ": " + e.what();
                }
            }
            at = close + 1;
        }
        return {};
    }

    // time:: namespace (docs/site/guide/14-time.html): clocks through the engine's time source
    // (engine_time::source), stopwatches and calendar dates
    void register_time_functions(engine& eng_ref) {
        engine* eng = &eng_ref;
        using R = checked_result<script_value>;
        using args_t = std::vector<const script_value*>;

        dynamic_binder<stopwatch>(eng_ref, "time::stopwatch")
            .constructor<>()
            .method("start", [](stopwatch& s) { s.start(); })
            .method("stop", [](stopwatch& s) { return s.stop(); })
            .method("delta", [](stopwatch& s) { return s.delta(); })
            .method("elapsed", [](const stopwatch& s) { return s.elapsed(); })
            .method("pause", [](stopwatch& s) { return s.pause(); })
            .method("resume", [](stopwatch& s) { return s.resume(); })
            .method("is_running", [](const stopwatch& s) { return s.running; })
            .method("is_paused", [](const stopwatch& s) { return s.paused; })
            .build();

        auto whole = [](script_float v, const char* what) -> int64_t {
            if (!std::isfinite(v) || std::floor(v) != v) {
                throw runtime_error(std::string("time::date::") + what + " expects a whole number");
            }
            if (std::fabs(v) > 1e15) { throw runtime_error(std::string("time::date::") + what + " is out of range"); }
            return static_cast<int64_t>(v);
        };
        auto to_ms = [](script_float v, double scale, const char* what) -> int64_t {
            const double ms = v * scale;
            if (!std::isfinite(ms) || std::fabs(ms) > 1e15) throw runtime_error(std::string("time::date::") + what + " is out of range");
            return static_cast<int64_t>(std::llround(ms));
        };
        auto instant = [](const date& d) { return d.unix_ms_or_throw(); };
        // The field setters and the operators are registered below as checked functions: a refused store or a
        // non-date operand raises a catchable error (a throw from those call sites would leave the script)
        auto unused_setter = [](date&, script_int) {};
        dynamic_binder<date>(eng_ref, "time::date")
            .property("year", [](const date& d) -> script_int { return d.year; }, unused_setter)
            .property("month", [](const date& d) -> script_int { return d.month; }, unused_setter)
            .property("day", [](const date& d) -> script_int { return d.day; }, unused_setter)
            .property("hour", [](const date& d) -> script_int { return d.hour; }, unused_setter)
            .property("minute", [](const date& d) -> script_int { return d.minute; }, unused_setter)
            .property("second", [](const date& d) -> script_int { return d.second; }, unused_setter)
            .property("millisecond", [](const date& d) -> script_int { return d.millisecond; }, unused_setter)
            .property("weekday", [](const date& d) -> script_int { return d.weekday(); }, nullptr)
            .property("yearday", [](const date& d) -> script_int { return d.yearday(); }, nullptr)
            .property("utc", [](const date& d) -> bool { return d.utc; }, nullptr)
            .method("unix", [instant](const date& d) -> script_float { return static_cast<script_float>(instant(d)) / 1000.0; })
            .method("unix_ms", [instant](const date& d) -> script_int { return instant(d); })
            .method("format", [eng](const date& d, const std::string& tmpl) -> std::string {
                std::string out;
                const std::string error = format_date(eng, d, tmpl, "time::date::format", out);
                if (!error.empty()) throw runtime_error(error);
                return out;
            })
            .method("to_string", [](const date& d) { return d.to_string(); })
            .method("to_utc", [](const date& d) { return d.in_zone(true); })
            .method("to_local", [](const date& d) { return d.in_zone(false); })
            .method("add_days", [whole](const date& d, script_float n) { return d.plus_days(whole(n, "add_days")); })
            .method("add_months", [whole](const date& d, script_float n) { return d.plus_months(whole(n, "add_months")); })
            .method("add_years", [whole](const date& d, script_float n) { return d.plus_months(whole(n, "add_years") * 12); })
            .method("add_hours", [to_ms](const date& d, script_float n) { return d.plus_ms(to_ms(n, 3600000.0, "add_hours")); })
            .method("add_minutes", [to_ms](const date& d, script_float n) { return d.plus_ms(to_ms(n, 60000.0, "add_minutes")); })
            .method("add_seconds", [to_ms](const date& d, script_float n) { return d.plus_ms(to_ms(n, 1000.0, "add_seconds")); })
            .method("between", [instant](const date& d, const date& a, const date& b) {
                const int64_t t = instant(d), x = instant(a), y = instant(b);
                return x <= y ? (x <= t && t <= y) : (y <= t && t <= x);
            })
            .build();

        const auto date_class = detail::engine_access::get_class_definition(eng_ref, "time::date");
        const uint64_t date_type = date_class->get_type_id();
        auto as_date = [date_type](const script_value& v) -> date* {
            auto native = detail::checked_native_reference(v.deref(), typeid(date), date_type);
            return native ? static_cast<date*>(native.value()) : nullptr;
        };
        auto checked_text = [eng](const std::string& text) { return R(make_error_code(runtime_error_code::invalid_operation), "{0}", detail::engine_access::park_error_text(*eng, text)); };
        for (const auto& [field, set] : {std::pair<const char*, std::string (date::*)(int64_t)>{"year", &date::set_year}, {"month", &date::set_month},
                                         {"day", &date::set_day}, {"hour", &date::set_hour}, {"minute", &date::set_minute},
                                         {"second", &date::set_second}, {"millisecond", &date::set_millisecond}}) {
            date_class->add_method(std::string("_set_") + field, [=](const std::vector<script_value>& a) -> R {
                date* d = a.size() == 2 ? as_date(a[0]) : nullptr;
                if (!d) return R(make_error_code(runtime_error_code::type_mismatch), "time::date: a field store needs a date and one value");
                const script_value& v = a[1].deref();
                if (!v.is_int()) return checked_text(std::string("time::date: ") + field + " takes an int, got " + detail::slot_source_type_name(v, eng));
                const std::string error = (d->*set)(v.as_int());
                if (!error.empty()) return checked_text(error);
                return script_value(std::monostate{}, eng);
            });
        }
        struct date_operator { const char* token; int kind; };   // kind: 0 compare, 1 equality, 2 subtraction
        for (const date_operator op : {date_operator{"<", 0}, {"<=", 0}, {">", 0}, {">=", 0}, {"==", 1}, {"!=", 1}, {"-", 2}}) {
            const std::string token = op.token;
            date_class->add_method(token, [=](const std::vector<script_value>& a) -> R {
                date* x = a.size() == 2 ? as_date(a[0]) : nullptr;
                date* y = a.size() == 2 ? as_date(a[1]) : nullptr;
                if (!x || !y) {
                    if (op.kind == 1 && x) return script_value(token == "!=", eng);
                    return checked_text("time::date: '" + token + "' needs two dates, got " + (a.size() == 2 ? detail::slot_source_type_name(a[1], eng) : std::string("no operand")));
                }
                int64_t tx = 0, ty = 0;
                if (!x->unix_ms(tx) || !y->unix_ms(ty)) return checked_text("time::date: a date is outside the local time the platform can represent");
                if (op.kind == 2) return script_value(static_cast<script_float>(tx - ty) / 1000.0, eng);
                const bool r = token == "<" ? tx < ty : token == "<=" ? tx <= ty : token == ">" ? tx > ty : token == ">=" ? tx >= ty : token == "==" ? tx == ty : tx != ty;
                return script_value(r, eng);
            });
        }

        detail::engine_access::serial_namespace(eng_ref, "time");
        auto& ns = detail::engine_access::script_namespaces(eng_ref)[detail::engine_access::symbolize(eng_ref, "time")];
        if (!ns) { ns = std::make_shared<script_namespace_data>(); }
        using svt = script_value_type;
        auto sig = [](uint8_t min_args, uint8_t max_args, std::vector<svt> params, svt result, const char* result_class = "") {
            host_entry_signature s;
            s.min_args = min_args;
            s.max_args = max_args;
            s.params = std::move(params);
            s.result = result;
            s.result_class = result_class;
            return s;
        };

        auto def = [&](const char* name, host_entry_signature signature, auto fn) {
            const uint64_t id = detail::engine_access::symbolize(eng_ref, name);
            ns->variables[id] = script_value::make_function([eng, fn](const std::vector<script_value>& raw) -> R {
                args_t args;
                args.reserve(raw.size());
                for (const auto& a : raw) { args.push_back(&a.deref()); }
                return fn(eng, args);
            }, &eng_ref);
            ns->signatures[id] = std::move(signature);
        };
        auto count_error = [](const char* text) { return R(make_error_code(runtime_error_code::argument_count_mismatch), text); };   // static texts: the result holds a view
        auto type_error = [](const char* text) { return R(make_error_code(runtime_error_code::type_mismatch), text); };
        auto value_error = [](engine* e, const std::string& text) { return R(make_error_code(runtime_error_code::invalid_operation), "{0}", detail::engine_access::park_error_text(*e, text)); };
        auto number = [](const script_value* v, double& out) {
            if (v->is_int()) { out = static_cast<double>(v->as_int()); return true; }
            if (v->is_float()) { out = v->as_float(); return true; }
            return false;
        };
        auto make_date = [](engine* e, const date& d) -> R { return detail::value_converter<date>::to(d, e); };
        auto wall_ms = [](engine* e) { return time_calendar::floor_div(static_cast<int64_t>(e->time().wall_now().count()), 1000000); };
        auto zoned_now = [=](engine* e, bool in_utc) -> R {
            date d;
            if (!date::from_unix_ms(wall_ms(e), in_utc, d)) return value_error(e, "time: the current time is outside the supported range");
            return make_date(e, d);
        };

        def("now", sig(0, 0, {}, svt::jai_float_type), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::now expects no arguments");
            return script_value(e->engine_elapsed(), e);
        });
        def("engine_elapsed", sig(0, 0, {}, svt::jai_float_type), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::engine_elapsed expects no arguments");
            return script_value(e->engine_elapsed(), e);
        });
        def("execute_elapsed", sig(0, 0, {}, svt::jai_float_type), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::execute_elapsed expects no arguments");
            return script_value(e->execute_elapsed(), e);
        });
        def("unix_time", sig(0, 0, {}, svt::jai_float_type), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::unix_time expects no arguments");
            return script_value(std::chrono::duration<double>(e->time().wall_now()).count(), e);
        });
        def("unix_ms", sig(0, 0, {}, svt::jai_int_type), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::unix_ms expects no arguments");
            return script_value(static_cast<script_int>(wall_ms(e)), e);
        });
        def("date_now", sig(1, 1, {svt::jai_string_type}, svt::jai_string_type), [=](engine* e, const args_t& a) -> R {
            if (a.size() != 1) return count_error("time::date_now expects exactly 1 argument (template)");
            const std::string* tmpl = a[0]->text_bytes();
            if (!tmpl) return type_error("time::date_now expects a string template");
            date d;
            if (!date::from_unix_ms(wall_ms(e), false, d)) return value_error(e, "time::date_now: the current time is outside the supported range");
            std::string out;
            const std::string error = format_date(e, d, *tmpl, "time::date_now", out);
            if (!error.empty()) return value_error(e, error);
            return script_value(std::move(out), e);
        });
        def("local_now", sig(0, 0, {}, svt::jai_object_type, "time::date"), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::local_now expects no arguments");
            return zoned_now(e, false);
        });
        def("utc_now", sig(0, 0, {}, svt::jai_object_type, "time::date"), [=](engine* e, const args_t& a) -> R {
            if (!a.empty()) return count_error("time::utc_now expects no arguments");
            return zoned_now(e, true);
        });
        def("date_from_unix", sig(1, 2, {svt::jai_float_type, svt::jai_bool_type}, svt::jai_object_type, "time::date"), [=](engine* e, const args_t& a) -> R {
            if (a.empty() || a.size() > 2) return count_error("time::date_from_unix expects 1 or 2 arguments (seconds, [utc])");
            double seconds = 0.0;
            if (!number(a[0], seconds)) return type_error("time::date_from_unix expects a number of seconds");
            if (a.size() == 2 && !a[1]->is_bool()) return type_error("time::date_from_unix expects a bool for utc");
            const bool in_utc = a.size() == 2 && a[1]->as<bool>();
            int64_t ms = 0;
            bool in_range = false;
            if (a[0]->is_int()) {
                const script_int s = a[0]->as_int();
                in_range = s >= -100000000000000LL && s <= 100000000000000LL;
                ms = in_range ? s * 1000 : 0;
            } else {
                const double rounded = std::round(seconds * 1000.0);
                in_range = std::isfinite(rounded) && std::fabs(rounded) <= 1e17;
                ms = in_range ? static_cast<int64_t>(rounded) : 0;
            }
            date d;
            if (!in_range || !date::from_unix_ms(ms, in_utc, d)) {
                return value_error(e, "time::date_from_unix: the instant is outside the supported range (years 1 to 9999)");
            }
            return make_date(e, d);
        });
        for (const bool in_utc : {false, true}) {
            const char* name = in_utc ? "utc_date" : "local_date";
            def(name, sig(3, 7, {svt::jai_int_type, svt::jai_int_type, svt::jai_int_type, svt::jai_int_type, svt::jai_int_type, svt::jai_int_type, svt::jai_int_type}, svt::jai_object_type, "time::date"), [=](engine* e, const args_t& a) -> R {
                if (a.size() < 3 || a.size() > 7) {
                    return count_error(in_utc ? "time::utc_date expects 3 to 7 arguments (year, month, day, [hour, minute, second, millisecond])"
                                              : "time::local_date expects 3 to 7 arguments (year, month, day, [hour, minute, second, millisecond])");
                }
                for (const auto* v : a) {
                    if (!v->is_int()) return type_error(in_utc ? "time::utc_date expects integer fields" : "time::local_date expects integer fields");
                }
                date d;
                d.utc = in_utc;
                std::string (date::* const setters[])(int64_t) = {&date::set_year, &date::set_month, &date::set_day, &date::set_hour,
                                                                    &date::set_minute, &date::set_second, &date::set_millisecond};
                for (size_t i = 0; i < a.size(); ++i) {
                    const std::string error = (d.*setters[i])(a[i]->as_int());
                    if (!error.empty()) return value_error(e, error);
                }
                return make_date(e, d);
            });
        }
        for (const char* name : {"stopwatch", "date"}) {
            const uint64_t id = detail::engine_access::symbolize(eng_ref, name);
            ns->variables[id] = eng_ref.get_variable(std::string("time::") + name);
            ns->signatures[id] = sig(0, 0, {}, svt::jai_object_type, name == std::string_view("date") ? "time::date" : "time::stopwatch");
        }
    }


} // namespace stdlib
} // namespace jai

