#include <jaiscript/stdlib/random.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/os_random.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jai {
namespace stdlib {

	random::random() {
		do {
			if (!detail::os_random_bytes(core_.words, sizeof(core_.words))) {
				throw runtime_error("random: the operating system's secure random generator failed");
			}
		} while (core_.is_zero());
	}

	random::random(int64_t a_seed) { core_.seed(static_cast<uint64_t>(a_seed)); }

	void random::seed(int64_t a_seed) {
		core_.seed(static_cast<uint64_t>(a_seed));
		region_cursors_ = nullptr;
		region_element_ = SIZE_MAX;
	}

	random::random(const random& a_other) {
		a_other.follow_region();
		core_ = a_other.core_;
	}

	random& random::operator=(const random& a_other) {
		if (this != &a_other) {
			a_other.follow_region();
			core_ = a_other.core_;
			region_cursors_ = nullptr;
			region_element_ = SIZE_MAX;
		}
		return *this;
	}

	void random::follow_region() const {
		if (!region_cursors_) { return; }
		const size_t element = region_cursors_->empty() ? SIZE_MAX : region_cursors_->front().element_cursor_index();
		if (element != region_element_) {
			region_element_ = element;
			core_.seed(detail::region_element_key(region_seed_, element));
		}
	}

	void random::begin_region(const std::vector<script_value>* a_cursors, uint64_t a_seed) noexcept {
		region_cursors_ = a_cursors;
		region_seed_ = a_seed;
		region_element_ = SIZE_MAX;
	}

	uint64_t random::next() {
		follow_region();
		return core_.next();
	}

	int64_t random::next_int(int64_t a_lo, int64_t a_hi) {
		if (a_hi < a_lo) { throw runtime_error("random::next_int range is inverted (max < min)"); }
		follow_region();
		return core_.between(a_lo, a_hi);
	}

	double random::next_float() {
		follow_region();
		return core_.unit();
	}

	double random::next_float(double a_lo, double a_hi) {
		if (!std::isfinite(a_lo) || !std::isfinite(a_hi)) { throw runtime_error("random::next_float bounds must be finite"); }
		if (a_hi < a_lo) { throw runtime_error("random::next_float range is inverted (max < min)"); }
		follow_region();
		// one rounding on every platform, never a contraction choice; a span past the float range runs halved (exactly)
		const double span = a_hi - a_lo;
		const double result = std::isfinite(span) ? std::fma(core_.unit(), span, a_lo)
		                                          : 2.0 * std::fma(core_.unit(), a_hi * 0.5 - a_lo * 0.5, a_lo * 0.5);
		return result < a_hi || a_lo == a_hi ? result : std::nextafter(a_hi, a_lo);
	}

	bool random::next_bool() {
		follow_region();
		return core_.coin();
	}

	size_t random::next_index(size_t a_count) {
		follow_region();
		return static_cast<size_t>(core_.below(a_count));
	}

	std::string random::next_bytes(int64_t a_count) {
		if (a_count < 0) { throw runtime_error("random::next_bytes count cannot be negative"); }
		follow_region();
		// ASan aborts on an allocation whose size plus bookkeeping overflows instead of throwing; length_error reads as "Out of memory" like bad_alloc
		if (static_cast<uint64_t>(a_count) > std::string().max_size() - 64) { throw std::length_error("next_bytes"); }
		std::string result(static_cast<size_t>(a_count), '\0');
		for (size_t at = 0; at < result.size(); at += 8) {
			uint64_t word = core_.next();
			for (size_t i = at; i < at + 8 && i < result.size(); ++i, word >>= 8) { result[i] = static_cast<char>(word & 0xFF); }
		}
		return result;
	}

	std::string random::state() const {
		follow_region();
		static constexpr char k_hex_digits[] = "0123456789abcdef";
		std::string result(64, '0');
		for (size_t i = 0; i < 64; ++i) { result[i] = k_hex_digits[(core_.words[i / 16] >> (60 - 4 * (i % 16))) & 15]; }
		return result;
	}

	bool random::set_state(std::string_view a_hex) {
		if (a_hex.size() != 64) { return false; }
		detail::xoshiro256 parsed;
		for (size_t i = 0; i < 64; ++i) {
			const char c = a_hex[i];
			const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (digit < 0) { return false; }
			parsed.words[i / 16] = (parsed.words[i / 16] << 4) | static_cast<uint64_t>(digit);
		}
		if (parsed.is_zero()) { return false; }
		core_ = parsed;
		region_cursors_ = nullptr;
		region_element_ = SIZE_MAX;
		return true;
	}

namespace random_detail {

	uint64_t random_type_id(engine* a_engine) {
		const auto definition = detail::engine_access::get_class_definition(*a_engine, "random");
		return definition ? definition->get_type_id() : UINT64_MAX;
	}

	random* as_random(const script_value& a_value, uint64_t a_type_id) {
		if (a_type_id == UINT64_MAX) { return nullptr; }
		auto native = detail::checked_native_reference(a_value.deref(), typeid(random), a_type_id);
		return native ? static_cast<random*>(native.value()) : nullptr;
	}

	bool is_random(const script_value& a_value) {
		engine* owner = a_value.deref().payload_engine();
		return owner && as_random(a_value, random_type_id(owner)) != nullptr;
	}

	uint64_t draw_region_seed(const script_value& a_value) {
		random* source = as_random(a_value, random_type_id(a_value.deref().payload_engine()));
		return source ? source->next() : 0;
	}

	script_value region_copy(const script_value& a_value, engine* a_engine, const std::vector<script_value>* a_cursors, uint64_t a_seed) {
		const random* source = as_random(a_value, random_type_id(a_engine));
		random copy = source ? *source : random(0);
		copy.begin_region(a_cursors, a_seed);
		return detail::value_converter<random>::to(copy, a_engine);
	}

	void begin_region(const script_value& a_copy, const std::vector<script_value>* a_cursors, uint64_t a_seed) {
		if (random* copy = as_random(a_copy, random_type_id(a_copy.deref().payload_engine()))) { copy->begin_region(a_cursors, a_seed); }
	}

	JAI_NOINLINE checked_result<script_value> shuffle_needs_array() {
		return checked_result<script_value>(make_error_code(runtime_error_code::type_mismatch), "random::shuffle expects an array");
	}

} // namespace random_detail

	void register_random_functions(engine& eng_ref) {
		using namespace random_detail;
		detail::engine_access::library_registration library(eng_ref);
		engine* eng = &eng_ref;
		using R = checked_result<script_value>;

		auto unused_setter = [](random&, const std::string&) {};
		dynamic_binder<random>(eng_ref, "random")
			.strict_signatures()
			.constructor<>()
			.constructor<int64_t>()
			.method("next_int", [](random& a_random, script_int a_lo, script_int a_hi) -> script_int { return a_random.next_int(a_lo, a_hi); })
			.method("next_float", [](random& a_random) -> script_float { return a_random.next_float(); })
			.method("next_float", [](random& a_random, script_float a_lo, script_float a_hi) -> script_float { return a_random.next_float(a_lo, a_hi); })
			.method("seed", [](random& a_random, script_int a_seed) { a_random.seed(a_seed); })
			.method("next_bool", [](random& a_random) -> bool { return a_random.next_bool(); })
			.method("next_bytes", [](random& a_random, script_int a_count) -> std::string { return a_random.next_bytes(a_count); })
			.method("pick", [eng](random& a_random, const script_value& a_array) -> script_value {
				const script_value& array = a_array.deref();
				if (!array.is_array() || array.unchecked_array_node()->empty()) { throw runtime_error("random::pick expects a non-empty array"); }
				const script_array& elements = *array.unchecked_array_node();
				return elements.get(a_random.next_index(elements.size()), eng);
			})
			.property("state", [](const random& a_random) -> std::string { return a_random.state(); }, unused_setter)
			.build();

		const auto random_class = detail::engine_access::get_class_definition(eng_ref, "random");
		const uint64_t type_id = random_class->get_type_id();
		// shuffle writes its argument in place, and a refused state store raises a catchable error: both are checked methods
		random_class->add_method("shuffle", host_writeback_function{ [type_id, eng](const std::vector<script_value>& a_args) -> R {
			random* generator = a_args.size() == 2 ? as_random(a_args[0], type_id) : nullptr;
			if (!generator || !a_args[1].deref().is_array()) { return shuffle_needs_array(); }
			script_value& target = const_cast<script_value&>(a_args[1].deref());
			script_array& elements = *target.unchecked_get_array_storage();
			for (size_t i = elements.size(); i > 1; --i) { elements.swap_elements(i - 1, generator->next_index(i)); }
			return script_value(std::monostate{}, eng);
		}, 1 }, 1);
		random_class->add_method("_set_state", [type_id, eng](const std::vector<script_value>& a_args) -> R {
			random* generator = a_args.size() == 2 ? as_random(a_args[0], type_id) : nullptr;
			const std::string* hex = generator ? a_args[1].deref().text_bytes() : nullptr;
			if (!hex || !generator->set_state(*hex)) { return R(make_error_code(runtime_error_code::invalid_operation), random::k_state_text); }
			return script_value(std::monostate{}, eng);
		});
	}

} // namespace stdlib
} // namespace jai
