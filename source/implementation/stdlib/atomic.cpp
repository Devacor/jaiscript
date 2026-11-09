#include <jaiscript/stdlib/atomic.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/integer_ops.hpp>
#include <atomic>
#include <cstdint>
#include <string>

namespace jai {
namespace stdlib {

namespace atomic_detail {

	using atomic_int = std::atomic<script_int>;
	using atomic_float = std::atomic<script_float>;
	using atomic_bool = std::atomic<bool>;

	[[noreturn]] JAI_NOINLINE void raise_overflow(const char* a_text) { throw runtime_error(a_text); }

	// The add and subtract steps keep the integer overflow policy (jai::ints): a compare-exchange loop, so an overflowing step
	// raises and leaves the value as it was. Returns the value before the step, or after it when a_fetch is false.
	template <bool Add>
	script_int int_step(atomic_int& a_atomic, script_int a_by, bool a_fetch, const char* a_overflow) {
		script_int seen = a_atomic.load();
		script_int next = 0;
		do {
			if (Add ? !ints::try_add(seen, a_by, next) : !ints::try_sub(seen, a_by, next)) { raise_overflow(a_overflow); }
		} while (!a_atomic.compare_exchange_weak(seen, next));
		return a_fetch ? seen : next;
	}

	// std::atomic<double>'s fetch_add and fetch_sub as the compare-exchange loop they are (C++20 adds them; not every standard
	// library has them yet)
	script_float float_step(atomic_float& a_atomic, script_float a_by, bool a_fetch) {
		script_float seen = a_atomic.load();
		while (!a_atomic.compare_exchange_weak(seen, seen + a_by)) {}
		return a_fetch ? seen : seen + a_by;
	}

	void bind_atomic_int(engine& a_engine) {
		dynamic_binder<atomic_int>(a_engine, "atomic<int>")
			.strict_signatures()
			.constructor<>()
			.constructor<script_int>()
			.method("load", [](const atomic_int& a_self) -> script_int { return a_self.load(); })
			.method("store", [](atomic_int& a_self, script_int a_value) { a_self.store(a_value); })
			.method("exchange", [](atomic_int& a_self, script_int a_value) -> script_int { return a_self.exchange(a_value); })
			.method("compare_exchange", [](atomic_int& a_self, script_int& a_expected, script_int a_desired) -> bool {
				return a_self.compare_exchange_strong(a_expected, a_desired);
			})
			.method("fetch_add", [](atomic_int& a_self, script_int a_by) -> script_int { return int_step<true>(a_self, a_by, true, "Integer overflow in '+'"); })
			.method("fetch_sub", [](atomic_int& a_self, script_int a_by) -> script_int { return int_step<false>(a_self, a_by, true, "Integer overflow in '-'"); })
			.method("fetch_and", [](atomic_int& a_self, script_int a_bits) -> script_int { return a_self.fetch_and(a_bits); })
			.method("fetch_or", [](atomic_int& a_self, script_int a_bits) -> script_int { return a_self.fetch_or(a_bits); })
			.method("fetch_xor", [](atomic_int& a_self, script_int a_bits) -> script_int { return a_self.fetch_xor(a_bits); })
			.method("++", [](atomic_int& a_self) -> script_int { return int_step<true>(a_self, 1, false, "Integer overflow in '++'"); })
			.method("++", [](atomic_int& a_self, script_int) -> script_int { return int_step<true>(a_self, 1, true, "Integer overflow in '++'"); })
			.method("--", [](atomic_int& a_self) -> script_int { return int_step<false>(a_self, 1, false, "Integer overflow in '--'"); })
			.method("--", [](atomic_int& a_self, script_int) -> script_int { return int_step<false>(a_self, 1, true, "Integer overflow in '--'"); })
			.method("+=", [](atomic_int& a_self, script_int a_by) -> script_int { return int_step<true>(a_self, a_by, false, "Integer overflow in '+='"); })
			.method("-=", [](atomic_int& a_self, script_int a_by) -> script_int { return int_step<false>(a_self, a_by, false, "Integer overflow in '-='"); })
			.method("&=", [](atomic_int& a_self, script_int a_bits) -> script_int { return a_self &= a_bits; })
			.method("|=", [](atomic_int& a_self, script_int a_bits) -> script_int { return a_self |= a_bits; })
			.method("^=", [](atomic_int& a_self, script_int a_bits) -> script_int { return a_self ^= a_bits; })
			.method("=", [](atomic_int& a_self, script_int a_value) -> script_int { return a_self = a_value; })
			.method("==", [](const atomic_int& a_self, script_int a_value) -> bool { return a_self.load() == a_value; })
			.method("to_int", [](const atomic_int& a_self) -> script_int { return a_self.load(); })
			.method("to_bool", [](const atomic_int& a_self) -> bool { return a_self.load() != 0; })
			.method("to_string", [](const atomic_int& a_self) -> std::string { return std::to_string(a_self.load()); })
			.build();
	}

	void bind_atomic_float(engine& a_engine) {
		dynamic_binder<atomic_float>(a_engine, "atomic<float>")
			.strict_signatures()
			.constructor<>()
			.constructor<script_float>()
			.method("load", [](const atomic_float& a_self) -> script_float { return a_self.load(); })
			.method("store", [](atomic_float& a_self, script_float a_value) { a_self.store(a_value); })
			.method("exchange", [](atomic_float& a_self, script_float a_value) -> script_float { return a_self.exchange(a_value); })
			.method("compare_exchange", [](atomic_float& a_self, script_float& a_expected, script_float a_desired) -> bool {
				return a_self.compare_exchange_strong(a_expected, a_desired);
			})
			.method("fetch_add", [](atomic_float& a_self, script_float a_by) -> script_float { return float_step(a_self, a_by, true); })
			.method("fetch_sub", [](atomic_float& a_self, script_float a_by) -> script_float { return float_step(a_self, -a_by, true); })
			.method("+=", [](atomic_float& a_self, script_float a_by) -> script_float { return float_step(a_self, a_by, false); })
			.method("-=", [](atomic_float& a_self, script_float a_by) -> script_float { return float_step(a_self, -a_by, false); })
			.method("=", [](atomic_float& a_self, script_float a_value) -> script_float { return a_self = a_value; })
			.method("==", [](const atomic_float& a_self, script_float a_value) -> bool { return a_self.load() == a_value; })
			.method("to_float", [](const atomic_float& a_self) -> script_float { return a_self.load(); })
			.method("to_bool", [](const atomic_float& a_self) -> bool { return a_self.load() != 0.0; })
			.method("to_string", [](const atomic_float& a_self) -> std::string { return script_value::float_text(a_self.load()); })
			.build();
	}

	void bind_atomic_bool(engine& a_engine) {
		dynamic_binder<atomic_bool>(a_engine, "atomic<bool>")
			.strict_signatures()
			.constructor<>()
			.constructor<bool>()
			.method("load", [](const atomic_bool& a_self) -> bool { return a_self.load(); })
			.method("store", [](atomic_bool& a_self, bool a_value) { a_self.store(a_value); })
			.method("exchange", [](atomic_bool& a_self, bool a_value) -> bool { return a_self.exchange(a_value); })
			.method("compare_exchange", [](atomic_bool& a_self, bool& a_expected, bool a_desired) -> bool {
				return a_self.compare_exchange_strong(a_expected, a_desired);
			})
			.method("=", [](atomic_bool& a_self, bool a_value) -> bool { return a_self = a_value; })
			.method("==", [](const atomic_bool& a_self, bool a_value) -> bool { return a_self.load() == a_value; })
			.method("to_bool", [](const atomic_bool& a_self) -> bool { return a_self.load(); })
			.method("to_string", [](const atomic_bool& a_self) -> std::string { return a_self.load() ? "true" : "false"; })
			.build();
	}

} // namespace atomic_detail

	void register_atomics(engine& eng_ref) {
		atomic_detail::bind_atomic_int(eng_ref);
		atomic_detail::bind_atomic_float(eng_ref);
		atomic_detail::bind_atomic_bool(eng_ref);
	}

} // namespace stdlib
} // namespace jai
