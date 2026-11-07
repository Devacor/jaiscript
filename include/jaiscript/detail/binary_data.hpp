#pragma once

#ifndef __JAISCRIPT_DETAIL_BINARY_DATA_HPP__
#define __JAISCRIPT_DETAIL_BINARY_DATA_HPP__

// The binary:: library over byte strings (guide ch. 9 "Binary data"). binary::reader walks a string's bytes with a cursor,
// binary::writer builds bytes, the width tokens (binary::int8 ... binary::float64, binary::text(n), binary::bytes(n))
// name an encoding, and a binary::layout is a validated list of named widths that unpack, pack
// and read(Class, layout) apply in order. Little-endian until big_endian(). A read past the end, a value that does not fit its
// width, a malformed layout and a C-string scan past engine::binary_scan_limit raise catchable errors
//Engine-core: every engine registers it (engine.cpp), and string's to_bytes()/reader()
// builtins live here beside it.

#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/core/script_map.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/float_text.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <bit>
#include <limits>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace jai::binary {

	enum class kind : uint8_t { int8, int16, int32, int64, uint8, uint16, uint32, float32, float64, text, bytes };

	struct width {
		kind k = kind::int8;
		uint64_t length = 0;   // 64-bit on a 32-bit host too: a width spans the script int range
		uint64_t size() const noexcept {
			switch (k) {
				case kind::int8: case kind::uint8: return 1;
				case kind::int16: case kind::uint16: return 2;
				case kind::int32: case kind::uint32: case kind::float32: return 4;
				case kind::int64: case kind::float64: return 8;
				case kind::text: case kind::bytes: return length;
			}
			return 0;
		}
		std::string name() const {
			switch (k) {
				case kind::int8: return "int8";
				case kind::int16: return "int16";
				case kind::int32: return "int32";
				case kind::int64: return "int64";
				case kind::uint8: return "uint8";
				case kind::uint16: return "uint16";
				case kind::uint32: return "uint32";
				case kind::float32: return "float32";
				case kind::float64: return "float64";
				case kind::text: return "text(" + std::to_string(length) + ")";
				case kind::bytes: return "bytes(" + std::to_string(length) + ")";
			}
			return "?";
		}
		bool is_text() const noexcept { return k == kind::text; }
		bool is_bytes() const noexcept { return k == kind::bytes; }
		bool is_integer() const noexcept { return k != kind::float32 && k != kind::float64 && k != kind::text && k != kind::bytes; }
		bool is_float() const noexcept { return k == kind::float32 || k == kind::float64; }
	};

	inline std::string count_text(uint64_t n, const char* one, const char* many) { return std::to_string(n) + (n == 1 ? one : many); }

	// a 64-bit byte count as an allocation size: past the address space (a 32-bit host) it fails as that allocation would
	inline size_t allocation_size(uint64_t n) {
		if (n > SIZE_MAX) [[unlikely]] { throw std::bad_alloc(); }
		return static_cast<size_t>(n);
	}

	inline std::string float_text(double v) {
		if (std::isnan(v)) { return std::string(jai::detail::k_nan_text); }
		char buf[64];
		auto r = std::to_chars(buf, buf + sizeof(buf), v);
		return std::string(buf, r.ptr);
	}

	inline uint64_t load_bits(const unsigned char* p, size_t n, bool big) noexcept {
		uint64_t v = 0;
		if (big) { for (size_t i = 0; i < n; ++i) { v = (v << 8) | p[i]; } }
		else { for (size_t i = n; i-- > 0;) { v = (v << 8) | p[i]; } }
		return v;
	}

	inline void store_bits(unsigned char* p, uint64_t v, size_t n, bool big) noexcept {
		for (size_t i = 0; i < n; ++i) { p[big ? n - 1 - i : i] = static_cast<unsigned char>(v >> (8 * i)); }
	}

	inline script_int load_integer(kind k, const unsigned char* p, bool big) noexcept {
		switch (k) {
			case kind::int8: return static_cast<int8_t>(p[0]);
			case kind::uint8: return p[0];
			case kind::int16: return static_cast<int16_t>(static_cast<uint16_t>(load_bits(p, 2, big)));
			case kind::uint16: return static_cast<script_int>(load_bits(p, 2, big));
			case kind::int32: return static_cast<int32_t>(static_cast<uint32_t>(load_bits(p, 4, big)));
			case kind::uint32: return static_cast<script_int>(load_bits(p, 4, big));
			default: return std::bit_cast<int64_t>(load_bits(p, 8, big));
		}
	}

	inline script_float load_float(kind k, const unsigned char* p, bool big) noexcept {
		if (k == kind::float32) { return std::bit_cast<float>(static_cast<uint32_t>(load_bits(p, 4, big))); }
		return std::bit_cast<double>(load_bits(p, 8, big));
	}

	// a fixed-length text field: its bytes up to the first NUL (C char[n])
	inline std::string load_text(const unsigned char* p, size_t n) {
		const void* nul = std::memchr(p, 0, n);
		return std::string(reinterpret_cast<const char*>(p), nul ? static_cast<const unsigned char*>(nul) - p : n);
	}

	inline bool integer_range(kind k, script_int& lo, script_int& hi) noexcept {
		switch (k) {
			case kind::int8: lo = -128; hi = 127; return true;
			case kind::uint8: lo = 0; hi = 255; return true;
			case kind::int16: lo = -32768; hi = 32767; return true;
			case kind::uint16: lo = 0; hi = 65535; return true;
			case kind::int32: lo = std::numeric_limits<int32_t>::min(); hi = std::numeric_limits<int32_t>::max(); return true;
			case kind::uint32: lo = 0; hi = std::numeric_limits<uint32_t>::max(); return true;
			default: return false;
		}
	}

	inline bool integer_fits(kind k, script_int v) noexcept {
		script_int lo, hi;
		return !integer_range(k, lo, hi) || (v >= lo && v <= hi);
	}

	inline bool float_fits(kind k, script_float v) noexcept {
		return k != kind::float32 || !std::isfinite(v) || std::fabs(v) <= std::numeric_limits<float>::max();
	}

	// "" when v fits w, else what the error says after "Cannot write "
	inline std::string integer_misfit(kind k, script_int v, const width& w) {
		script_int lo, hi;
		if (!integer_range(k, lo, hi) || (v >= lo && v <= hi)) { return {}; }
		return std::to_string(v) + " as " + w.name() + " (" + std::to_string(lo) + " to " + std::to_string(hi) + ")";
	}

	inline std::string float_misfit(kind k, script_float v, const width& w) {
		constexpr double fmax = std::numeric_limits<float>::max();
		if (k != kind::float32 || !std::isfinite(v) || std::fabs(v) <= fmax) { return {}; }
		return float_text(v) + " as " + w.name() + " (" + float_text(-fmax) + " to " + float_text(fmax) + ")";
	}

	inline void store_integer(kind k, script_int v, unsigned char* p, bool big) noexcept {
		store_bits(p, static_cast<uint64_t>(v), static_cast<size_t>(width{k}.size()), big);
	}

	inline void store_float(kind k, script_float v, unsigned char* p, bool big) noexcept {
		if (k == kind::float32) { store_bits(p, std::bit_cast<uint32_t>(static_cast<float>(v)), 4, big); }
		else { store_bits(p, std::bit_cast<uint64_t>(v), 8, big); }
	}

	inline const std::string& value_kind_name(const script_value& v) {
		static const std::string names[] = {"null", "int", "float", "bool", "char", "string", "utf8_string", "array", "map", "function", "object"};
		const script_value& d = v.deref();
		if (d.is_null()) return names[0];
		if (d.is_int()) return names[1];
		if (d.is_float()) return names[2];
		if (d.is_bool()) return names[3];
		if (d.is_char()) return names[4];
		if (d.is_string()) return names[5];
		if (d.is_utf8_string()) return names[6];
		if (d.is_array()) return names[7];
		if (d.is_map()) return names[8];
		if (d.is_function()) return names[9];
		return names[10];
	}

	struct field {
		script_value key;   // the name as a string value: every map unpack builds shares it
		width w;
	};

	class layout {
	public:
		std::vector<field> fields;
		uint64_t bytes = 0;
		uint64_t size() const noexcept { return bytes; }
	};

	[[noreturn]] JAI_NOINLINE inline void raise(const std::string& text) { throw std::runtime_error(text); }

	// the writers' range texts, built only for the raise
	[[noreturn]] JAI_NOINLINE inline void raise_write_misfit(kind k, script_int v) { raise("Cannot write " + integer_misfit(k, v, width{k})); }
	[[noreturn]] JAI_NOINLINE inline void raise_write_misfit(kind k, script_float v) { raise("Cannot write " + float_misfit(k, v, width{k})); }

	// "" when s can be written as text (NUL-ended), else why not (its first NUL byte)
	inline std::string embedded_nul(const std::string& s) {
		const size_t at = s.find('\0');
		return at == std::string::npos ? std::string() : "the text holds a NUL at byte " + std::to_string(at);
	}

	// "" when s fits an n-byte text field (no NUL inside, at most n bytes), else why not
	inline std::string text_field_misfit(const std::string& s, uint64_t n) {
		std::string why = embedded_nul(s);
		if (why.empty() && s.size() > n) { why = "the text is " + count_text(s.size(), " byte", " bytes") + ", longer than text(" + std::to_string(n) + ")"; }
		return why;
	}

	// s (at most n bytes) in an n-byte text field: its bytes, then NULs to the end; exactly n bytes leave no terminator
	inline void store_text_field(unsigned char* p, const std::string& s, size_t n) {
		std::memcpy(p, s.data(), s.size());
		std::memset(p + s.size(), 0, n - s.size());
	}

	// n bytes as an array<uint8>, one copy
	inline script_value byte_array(engine* eng, const unsigned char* p, size_t n) {
		script_value out = script_value::make_array(detail::engine_access::get_type_info_uint8(*eng), eng);
		out.unchecked_get_array_storage()->bytes().assign(p, p + n);
		return out;
	}

	// the raw bytes a bytes() write takes: a string as it is, or an array of uint8s, ints or chars 0..255; "" and ok, or why not
	inline bool raw_bytes_of(const script_value& value, std::string& out, std::string& why) {
		const script_value& v = value.deref();
		if (const std::string* s = v.text_bytes()) { out = *s; return true; }
		if (!v.is_array()) { why = value_kind_name(v) + " is not bytes (a string or an array of 0 to 255)"; return false; }
		const script_array* node = v.unchecked_array_node();
		const size_t n = node ? node->size() : 0;
		out.assign(n, '\0');
		if (n == 0) { return true; }
		if (node->kind() == script_array::kind_t::u8 || node->kind() == script_array::kind_t::c8) { std::memcpy(out.data(), node->bytes().data(), n); return true; }
		if (node->kind() == script_array::kind_t::i64) {
			const auto& ints = node->ints();
			for (size_t i = 0; i < n; ++i) {
				if (ints[i] < 0 || ints[i] > 255) { why = "element " + std::to_string(i) + " is " + std::to_string(ints[i]) + ", not 0 to 255"; return false; }
				out[i] = static_cast<char>(ints[i]);
			}
			return true;
		}
		size_t i = 0;
		bool ok = true;
		node->for_each(node->owner, [&](const script_value& el) {
			if (!ok) { return; }
			const script_value& e = el.deref();
			if (e.is_int() && e.as_int() >= 0 && e.as_int() <= 255) { out[i] = static_cast<char>(e.as_int()); }
			else if (e.is_char()) { out[i] = e.as_char(); }
			else { why = "element " + std::to_string(i) + " is " + (e.is_int() ? std::to_string(e.as_int()) : value_kind_name(e)) + ", not 0 to 255"; ok = false; }
			++i;
		});
		return ok;
	}

	inline const width* as_width(const script_value& v, uint64_t width_type_id) {
		const script_value& d = v.deref();
		if (!d.is_object()) { return nullptr; }
		try { return jai::detail::extract_cpp_object_ptr<width>(d, width_type_id); } catch (...) { return nullptr; }
	}

	// name, width pairs -> a layout; who names the caller in the texts ("binary::layout", "unpack")
	inline layout make_layout(const script_value* args, size_t n, const char* who, engine* eng, uint64_t width_type_id) {
		if (n == 0 || n % 2 != 0) {
			raise(std::string(who) + " expects name, width pairs, got " + count_text(n, " argument", " arguments"));
		}
		layout out;
		for (size_t i = 0; i < n; i += 2) {
			const script_value& name = args[i].deref();
			if (!name.is_text()) {
				raise(std::string(who) + " argument " + std::to_string(i + 1) + " must be a field name string, got " + value_kind_name(name));
			}
			const std::string& text = name.unchecked_text();
			if (text.empty()) { raise(std::string(who) + " argument " + std::to_string(i + 1) + " is an empty field name"); }
			const width* w = as_width(args[i + 1], width_type_id);
			if (!w) {
				raise(std::string(who) + " argument " + std::to_string(i + 2) + " must be a binary width (binary::int32, binary::text(8), ...), got " +
					value_kind_name(args[i + 1]));
			}
			for (const auto& f : out.fields) {
				if (f.key.unchecked_as_string() == text) { raise(std::string(who) + " repeats the field name '" + text + "'"); }
			}
			// the total is a script int (layout.size()) and the bound every read and pack trusts: it must not wrap
			if (w->size() > static_cast<uint64_t>(std::numeric_limits<script_int>::max()) - out.bytes) {
				raise(std::string(who) + " fields total more than " + std::to_string(std::numeric_limits<script_int>::max()) + " bytes");
			}
			out.fields.push_back(field{script_value(text, eng), *w});
			out.bytes += w->size();
		}
		return out;
	}

	class reader {
	public:
		explicit reader(engine* eng) : engine_(eng), source_(std::string(), eng) {}
		reader(engine* eng, const script_value& source) : engine_(eng), source_(snapshot(eng, source)) {}

		const std::string& data() const noexcept { return source_.unchecked_text(); }
		engine* get_engine() const noexcept { return engine_; }
		size_t pos() const noexcept { return pos_; }
		size_t size() const noexcept { return data().size(); }
		size_t remaining() const noexcept { return size() - pos_; }
		bool at_end() const noexcept { return pos_ == size(); }
		bool big() const noexcept { return big_; }
		void set_big(bool b) noexcept { big_ = b; }

		void seek(script_int to) {
			if (to < 0 || static_cast<size_t>(to) > size()) {
				raise("Cannot seek to position " + std::to_string(to) + ": the data is " + count_text(size(), " byte", " bytes"));
			}
			pos_ = static_cast<size_t>(to);
		}
		void skip(script_int n) {
			const script_int to = static_cast<script_int>(pos_) + n;
			if (to < 0 || static_cast<size_t>(to) > size()) {
				raise("Cannot skip " + std::to_string(n) + " bytes at position " + std::to_string(pos_) + ": the data is " + count_text(size(), " byte", " bytes"));
			}
			pos_ = static_cast<size_t>(to);
		}

		[[noreturn]] JAI_NOINLINE void past_end(uint64_t n, std::string_view what) const {
			const size_t left = remaining();
			raise("Cannot read " + std::string(what) + " at position " + std::to_string(pos_) + ": " + count_text(n, " byte wanted, ", " bytes wanted, ") +
				std::to_string(left) + (left == 1 ? " remains" : " remain"));
		}
		[[noreturn]] JAI_NOINLINE void past_end_counted(uint64_t c, const char* what, script_int n) const {
			past_end(c, std::string(what) + "(" + std::to_string(n) + ")");
		}
		[[noreturn]] JAI_NOINLINE void past_end_field(const field& f) const {
			past_end(f.w.size(), "field '" + f.key.unchecked_as_string() + "' (" + f.w.name() + ")");
		}
		// the next n bytes, the cursor past them, once the caller knows they fit
		const unsigned char* advance(size_t n) noexcept {
			const unsigned char* p = reinterpret_cast<const unsigned char*>(data().data()) + pos_;
			pos_ += n;
			return p;
		}
		const unsigned char* take(size_t n, std::string_view what) {
			if (n > remaining()) { past_end(n, what); }
			return advance(n);
		}
		// a counted read (text, bytes): its name built only for the error
		const unsigned char* take_counted(uint64_t c, const char* what, script_int n) {
			if (c > remaining()) { past_end_counted(c, what, n); }
			return advance(static_cast<size_t>(c));
		}
		const unsigned char* take_field(const field& f) {
			if (f.w.size() > remaining()) { past_end_field(f); }
			return advance(static_cast<size_t>(f.w.size()));
		}
		uint64_t count_arg(script_int n, const char* what) const {
			if (n < 0) { raise(std::string("Cannot read ") + what + "(" + std::to_string(n) + "): a count cannot be negative"); }
			return static_cast<uint64_t>(n);
		}

		script_int integer(kind k) {
			static constexpr std::string_view names[] = {"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32"};
			const width w{k};
			return load_integer(k, take(static_cast<size_t>(w.size()), names[static_cast<size_t>(k)]), big_);
		}
		script_float floating(kind k) {
			return load_float(k, take(k == kind::float32 ? 4 : 8, k == kind::float32 ? std::string_view("float32") : std::string_view("float64")), big_);
		}
		// an n-byte text field: the cursor moves n, the text is the bytes before the first NUL (all n when there is none)
		std::string text(script_int n) {
			const uint64_t c = count_arg(n, "text");
			return load_text(take_counted(c, "text", n), static_cast<size_t>(c));
		}
		// a C string at the cursor: the bytes before the next NUL, the cursor past the NUL. The scan reads at most the
		// remaining bytes and the engine's binary_scan_limit (the host's; no script can raise it); no NUL in reach raises,
		// naming the bound that stopped it, and moves nothing
		std::string text() {
			const size_t left = remaining();
			const size_t limit = detail::engine_access::execution_limits(*get_engine()).binary_scan_limit;
			const size_t window = limit < left ? limit : left;
			const char* p = data().data() + pos_;
			const void* nul = window ? std::memchr(p, 0, window) : nullptr;
			if (!nul) {
				const std::string bound = window == left ? "in the remaining " + count_text(left, " byte", " bytes")
					: "within " + count_text(limit, " byte", " bytes") + " (the engine's binary scan limit; raise it from C++ with engine->binary_scan_limit)";
				raise("Cannot read null-terminated text at position " + std::to_string(pos_) + ": no NUL terminator " + bound);
			}
			const size_t len = static_cast<size_t>(static_cast<const char*>(nul) - p);
			pos_ += len + 1;
			return std::string(p, len);
		}
		script_value bytes(script_int n) {
			const uint64_t c = count_arg(n, "bytes");
			const unsigned char* p = take_counted(c, "bytes", n);
			return byte_array(get_engine(), p, static_cast<size_t>(c));
		}

		script_value read_field(const field& f) {
			const unsigned char* p = take_field(f);
			engine* eng = get_engine();
			if (f.w.k == kind::text) { return script_value(load_text(p, static_cast<size_t>(f.w.length)), eng); }
			if (f.w.k == kind::bytes) { return byte_array(eng, p, static_cast<size_t>(f.w.length)); }
			if (f.w.is_float()) { return script_value(load_float(f.w.k, p, big_), eng); }
			return script_value(load_integer(f.w.k, p, big_), eng);
		}
		// the fields in order into a map<string, var>; the cursor moves only when every field fits
		script_value unpack(const layout& l) {
			engine* eng = get_engine();
			if (l.bytes > remaining()) { probe(l); }
			script_value out = script_value::make_map(detail::engine_access::get_type_info_string(*eng), nullptr, eng);
			script_map& m = out.as_map();
			for (const auto& f : l.fields) { m.insert_or_assign(f.key, read_field(f)); }
			return out;
		}
		// raises the first field that does not fit, at its own position, and leaves the cursor where it was
		void probe(const layout& l) {
			const size_t start = pos_;
			try { for (const auto& f : l.fields) { take_field(f); } }
			catch (...) { pos_ = start; throw; }
			pos_ = start;
		}

	private:
		static script_value snapshot(engine* eng, const script_value& v) {
			const script_value& d = v.deref();
			const size_t k = d.raw_storage_index();
			if (k == script_value::TYPEID_STRING || k == script_value::TYPEID_UTF8_STRING) { return d; }
			if (const std::string* bytes = d.text_bytes()) { return script_value(*bytes, eng); }
			if (d.is_string()) { return script_value(d.as_string(), eng); }
			raise("binary::reader expects a string, got " + value_kind_name(d));
		}

		engine* engine_;
		script_value source_;
		size_t pos_ = 0;
		bool big_ = false;
	};

	class writer {
	public:
		const std::string& data() const noexcept { return out_; }
		size_t pos() const noexcept { return pos_; }
		size_t size() const noexcept { return out_.size(); }
		void set_big(bool b) noexcept { big_ = b; }
		bool big() const noexcept { return big_; }

		void seek(script_int to) {
			if (to < 0 || static_cast<size_t>(to) > out_.size()) {
				raise("Cannot seek to position " + std::to_string(to) + ": " + count_text(out_.size(), " byte written", " bytes written"));
			}
			pos_ = static_cast<size_t>(to);
		}
		// The engine whose memory cap the buffer's growth is charged to (each method call names it)
		writer& charged_to(engine* a_engine) noexcept {
			engine_ = a_engine;
			return *this;
		}
		unsigned char* room(size_t n) {
			if (pos_ + n > charged_) { charge_growth(pos_ + n); }
			if (pos_ == out_.size()) { out_.append(n, '\0'); }
			else if (pos_ + n > out_.size()) { out_.resize(pos_ + n); }
			unsigned char* p = reinterpret_cast<unsigned char*>(out_.data()) + pos_;
			pos_ += n;
			return p;
		}
		void put(const void* src, size_t n) { if (n) { std::memcpy(room(n), src, n); } }

		void integer(kind k, script_int v) {
			if (!integer_fits(k, v)) [[unlikely]] { raise_write_misfit(k, v); }
			store_integer(k, v, room(static_cast<size_t>(width{k}.size())), big_);
		}
		void floating(kind k, script_float v) {
			if (!float_fits(k, v)) [[unlikely]] { raise_write_misfit(k, v); }
			store_float(k, v, room(static_cast<size_t>(width{k}.size())), big_);
		}
		// a C string: the text, then a NUL
		void text(const std::string& s) {
			const std::string why = embedded_nul(s);
			if (!why.empty()) { raise("Cannot write text: " + why); }
			put(s.data(), s.size());
			put("", 1);
		}
		// an n-byte text field: the text, then NULs to n (a text of exactly n bytes fills it); a longer text raises
		void text(const std::string& s, script_int n) {
			if (n < 0) { raise("Cannot write text(s, " + std::to_string(n) + "): a count cannot be negative"); }
			const std::string why = text_field_misfit(s, static_cast<uint64_t>(n));
			if (!why.empty()) { raise("Cannot write text: " + why); }
			const size_t length = allocation_size(static_cast<uint64_t>(n));
			store_text_field(room(length), s, length);
		}
		// raw bytes as they are: a string's, or an array's (each 0 to 255)
		void bytes(const script_value& b) {
			std::string raw, why;
			if (!raw_bytes_of(b, raw, why)) { raise("Cannot write bytes: " + why); }
			put(raw.data(), raw.size());
		}

		// the value's encoding for f, or "" with the reason in why
		static bool encode(const field& f, const script_value& value, unsigned char* p, bool big, std::string& why) {
			const script_value& v = value.deref();
			if (f.w.is_text()) {
				const std::string* s = v.text_bytes();
				if (!s) { why = value_kind_name(v) + " does not fit " + f.w.name(); return false; }
				why = text_field_misfit(*s, f.w.length);
				if (!why.empty()) { return false; }
				store_text_field(p, *s, static_cast<size_t>(f.w.length));
				return true;
			}
			if (f.w.is_bytes()) {
				std::string raw;
				if (!raw_bytes_of(v, raw, why)) { return false; }
				if (raw.size() != f.w.length) {
					why = f.w.name() + " takes exactly " + count_text(f.w.length, " byte", " bytes") + ", got " + std::to_string(raw.size());
					return false;
				}
				std::memcpy(p, raw.data(), raw.size());
				return true;
			}
			if (f.w.is_float()) {
				if (!v.is_float() && !v.is_int()) { why = value_kind_name(v) + " does not fit " + f.w.name(); return false; }
				const script_float x = v.is_float() ? v.as_float() : static_cast<script_float>(v.as_int());
				if (!float_fits(f.w.k, x)) { why = "cannot write " + float_misfit(f.w.k, x, f.w); return false; }
				store_float(f.w.k, x, p, big);
				return true;
			}
			if (!v.is_int() && !v.is_char()) { why = value_kind_name(v) + " does not fit " + f.w.name(); return false; }
			const script_int x = v.is_int() ? v.as_int() : static_cast<script_int>(static_cast<unsigned char>(v.as_char()));
			if (!integer_fits(f.w.k, x)) { why = "cannot write " + integer_misfit(f.w.k, x, f.w); return false; }
			store_integer(f.w.k, x, p, big);
			return true;
		}
		// the map's values in the layout's order; nothing is written unless every field fits
		void pack(const layout& l, const script_value& values) {
			const script_value& m = values.deref();
			if (!m.is_map()) { raise("pack expects a map of field values, got " + value_kind_name(m)); }
			const script_map& map = m.as_map();
			const size_t staging = allocation_size(l.bytes);
			if (pos_ + staging > charged_) { charge_growth(pos_ + staging); }   // the staging is the growth: charged before it exists
			std::string staged(staging, '\0');
			unsigned char* p = reinterpret_cast<unsigned char*>(staged.data());
			std::string why;
			for (const auto& f : l.fields) {
				const std::string& name = f.key.unchecked_as_string();
				auto it = map.find(map_string_key_probe{name});
				if (it == map.end()) { raise("Cannot pack field '" + name + "': the map has no '" + name + "'"); }
				if (!encode(f, it->second, p, big_, why)) { raise("Cannot pack field '" + name + "': " + why); }
				p += static_cast<size_t>(f.w.size());
			}
			put(staged.data(), staged.size());
		}

	private:
		// a 4 KiB step at a time, so small writes charge rarely
		void charge_growth(size_t a_end) {
			if (!engine_) { return; }
			const size_t step = std::max<size_t>(a_end - charged_, 4096);
			auto& limits = detail::engine_access::execution_limits(*engine_);
			if (!limits.memory_charge(step)) [[unlikely]] { jai::detail::throw_memory_cap(limits); }
			charged_ += step;
		}

		std::string out_;
		size_t pos_ = 0;
		bool big_ = false;
		engine* engine_ = nullptr;
		size_t charged_ = 0;
	};

	inline const layout* as_layout(const script_value& v, uint64_t layout_type_id) {
		const script_value& d = v.deref();
		if (!d.is_object()) { return nullptr; }
		try { return jai::detail::extract_cpp_object_ptr<layout>(d, layout_type_id); } catch (...) { return nullptr; }
	}

	// r.read(Class, layout): a default-constructed instance of Class, its fields (bases first, declaration order, statics
	// skipped) taking the layout's values; the counts and each field's kind must agree
	inline script_value read_instance(reader& r, const script_value& cls, const layout& l) {
		engine* eng = r.get_engine();
		const script_value& ctor = cls.deref();
		if (!ctor.is_function()) { raise("read expects a class, got " + value_kind_name(ctor)); }
		checked_result<script_value> made = [&]() {
			detail::engine_access::construction_access_waiver waiver(eng);
			return ctor.as_function()(std::vector<script_value>{});
		}();
		if (!made) { raise("read could not construct the class: " + eng->format_error(made)); }
		script_value obj = std::move(made.value());
		strong_ptr<class_instance> instance;
		if (obj.is_object()) {
			try { instance = obj.as<strong_ptr<class_instance>>(); } catch (...) {}
		}
		class_definition* def = instance ? instance->get_class_definition() : nullptr;
		if (!def || def->is_cpp_class()) { raise("read expects a script class, got " + (def ? def->get_name() : value_kind_name(obj))); }
		std::vector<std::pair<class_definition*, uint64_t>> slots;
		std::vector<class_definition*> chain;
		for (class_definition* c = def; c;) {
			chain.push_back(c);
			const auto& parents = c->get_parent_classes();
			c = parents.empty() ? nullptr : parents.front().get();
		}
		for (size_t i = chain.size(); i-- > 0;) {
			for (uint64_t id : chain[i]->field_order()) {
				if (!chain[i]->is_static_field(id)) { slots.emplace_back(chain[i], id); }
			}
		}
		const std::string& cname = def->get_name();
		if (slots.size() != l.fields.size()) {
			raise("Cannot read " + cname + " from a layout of " + count_text(l.fields.size(), " field", " fields") + ": " + cname + " has " +
				count_text(slots.size(), " field", " fields"));
		}
		auto* sym = detail::engine_access::get_symbolizer(*eng);
		for (size_t i = 0; i < slots.size(); ++i) {
			const field& f = l.fields[i];
			type_info_ptr declared = slots[i].first->get_field_declared_type(slots[i].second);
			const script_value_type t = declared ? declared->base_type : script_value_type::jai_any_type;
			const bool ok = t == script_value_type::jai_any_type ||
				(f.w.is_integer() && t == script_value_type::jai_int_type) ||
				(f.w.is_float() && t == script_value_type::jai_float_type) ||
				(f.w.is_text() && (t == script_value_type::jai_string_type || t == script_value_type::jai_utf8_string_type)) ||
				(f.w.is_bytes() && t == script_value_type::jai_array_type);
			if (!ok) {
				raise("Cannot read field '" + f.key.unchecked_as_string() + "' (" + f.w.name() + ") into " + cname + "." +
					std::string(sym->get_string(slots[i].second)) + " (declared " + declared->canonical_name() + ")");
			}
		}
		if (l.bytes > r.remaining()) { r.probe(l); }
		for (size_t i = 0; i < slots.size(); ++i) {
			script_value v = r.read_field(l.fields[i]);
			type_info_ptr declared = slots[i].first->get_field_declared_type(slots[i].second);
			if (declared && declared->base_type == script_value_type::jai_utf8_string_type) { v = script_value::make_utf8(v.unchecked_as_string(), eng); }
			instance->set_field(slots[i].second, std::move(v));
		}
		return obj;
	}

	inline void add_lanes(engine& eng);

	inline void register_library(engine& eng) {
		dynamic_binder<width>(eng, "binary::width")
			.strict_signatures()
			.method("size", [](const width& w) -> script_int { return static_cast<script_int>(w.size()); })
			.method("name", [](const width& w) -> std::string { return w.name(); })
			.build();
		dynamic_binder<layout>(eng, "binary::layout")
			.strict_signatures()
			.method("size", [](const layout& l) -> script_int { return static_cast<script_int>(l.bytes); })
			.method("count", [](const layout& l) -> script_int { return static_cast<script_int>(l.fields.size()); })
			.build();
		const uint64_t width_id = detail::engine_access::get_class_definition(eng, "binary::width")->get_type_id();
		const uint64_t layout_id = detail::engine_access::get_class_definition(eng, "binary::layout")->get_type_id();

		dynamic_binder<reader>(eng, "binary::reader")
			.strict_signatures()
			.constructor<>()
			.constructor<script_value>()
			.method("int8", [](reader& r) -> script_int { return r.integer(kind::int8); })
			.method("int16", [](reader& r) -> script_int { return r.integer(kind::int16); })
			.method("int32", [](reader& r) -> script_int { return r.integer(kind::int32); })
			.method("int64", [](reader& r) -> script_int { return r.integer(kind::int64); })
			.method("uint8", [](reader& r) -> script_int { return r.integer(kind::uint8); })
			.method("uint16", [](reader& r) -> script_int { return r.integer(kind::uint16); })
			.method("uint32", [](reader& r) -> script_int { return r.integer(kind::uint32); })
			.method("float32", [](reader& r) -> script_float { return r.floating(kind::float32); })
			.method("float64", [](reader& r) -> script_float { return r.floating(kind::float64); })
			.method("text", [](reader& r, script_int n) -> std::string { return r.text(n); })
			.method("text", [](reader& r) -> std::string { return r.text(); })
			.method("bytes", [](reader& r, script_int n) -> script_value { return r.bytes(n); })
			.method("seek", [](reader& r, script_int to) -> reader& { r.seek(to); return r; })
			.method("skip", [](reader& r, script_int n) -> reader& { r.skip(n); return r; })
			.method("pos", [](const reader& r) -> script_int { return static_cast<script_int>(r.pos()); })
			.method("size", [](const reader& r) -> script_int { return static_cast<script_int>(r.size()); })
			.method("remaining", [](const reader& r) -> script_int { return static_cast<script_int>(r.remaining()); })
			.method("at_end", [](const reader& r) -> bool { return r.at_end(); })
			.method("big_endian", [](reader& r) -> reader& { r.set_big(true); return r; })
			.method("little_endian", [](reader& r) -> reader& { r.set_big(false); return r; })
			.method("read", [layout_id](reader& r, const script_value& cls, const script_value& l) -> script_value {
				const layout* lay = as_layout(l, layout_id);
				if (!lay) { raise("read expects a binary::layout, got " + value_kind_name(l)); }
				return read_instance(r, cls, *lay);
			})
			.build();
		auto reader_def = detail::engine_access::get_class_definition(eng, "binary::reader");
		const uint64_t reader_id = reader_def->get_type_id();
		reader_def->add_method("unpack", [reader_id, width_id, layout_id](const std::vector<script_value>& args) -> checked_result<script_value> {
			reader* r = jai::detail::extract_cpp_object_ptr<reader>(args[0], reader_id);
			if (args.size() == 2) {
				if (const layout* lay = as_layout(args[1], layout_id)) { return r->unpack(*lay); }
			}
			return r->unpack(make_layout(args.data() + 1, args.size() - 1, "unpack", r->get_engine(), width_id));
		});

		engine* const writes_for = &eng;
		dynamic_binder<writer>(eng, "binary::writer")
			.strict_signatures()
			.method("int8", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::int8, v); return w; })
			.method("int16", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::int16, v); return w; })
			.method("int32", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::int32, v); return w; })
			.method("int64", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::int64, v); return w; })
			.method("uint8", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::uint8, v); return w; })
			.method("uint16", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::uint16, v); return w; })
			.method("uint32", [writes_for](writer& w, script_int v) -> writer& { w.charged_to(writes_for).integer(kind::uint32, v); return w; })
			.method("float32", [writes_for](writer& w, script_float v) -> writer& { w.charged_to(writes_for).floating(kind::float32, v); return w; })
			.method("float64", [writes_for](writer& w, script_float v) -> writer& { w.charged_to(writes_for).floating(kind::float64, v); return w; })
			.method("text", [writes_for](writer& w, const std::string& s) -> writer& { w.charged_to(writes_for).text(s); return w; })
			.method("text", [writes_for](writer& w, const std::string& s, script_int n) -> writer& { w.charged_to(writes_for).text(s, n); return w; })
			.method("bytes", [writes_for](writer& w, const script_value& b) -> writer& { w.charged_to(writes_for).bytes(b); return w; })
			.method("pack", [layout_id, writes_for](writer& w, const script_value& l, const script_value& values) -> writer& {
				const layout* lay = as_layout(l, layout_id);
				if (!lay) { raise("pack expects a binary::layout, got " + value_kind_name(l)); }
				w.charged_to(writes_for).pack(*lay, values);
				return w;
			})
			.method("seek", [](writer& w, script_int to) -> writer& { w.seek(to); return w; })
			.method("pos", [](const writer& w) -> script_int { return static_cast<script_int>(w.pos()); })
			.method("size", [](const writer& w) -> script_int { return static_cast<script_int>(w.size()); })
			.method("big_endian", [](writer& w) -> writer& { w.set_big(true); return w; })
			.method("little_endian", [](writer& w) -> writer& { w.set_big(false); return w; })
			.method("to_string", [](const writer& w) -> std::string { return w.data(); })
			.build();

		auto& ns = detail::engine_access::script_namespaces(eng)[detail::engine_access::symbolize(eng, "binary")];
		if (!ns) { ns = std::make_shared<script_namespace_data>(); }
		engine* e = &eng;
		auto global = detail::engine_access::get_global_environment(eng);
		for (const char* cls : {"reader", "writer"}) {
			auto ctor = global->get(std::string("binary::") + cls);
			if (ctor) { ns->variables[detail::engine_access::symbolize(eng, cls)] = ctor.value(); }
		}
		auto width_value = [e](kind k, uint64_t length) { return detail::engine_access::make_value_object(*e, std::make_shared<width>(width{k, length})); };
		const std::pair<const char*, kind> widths[] = {
			{"int8", kind::int8}, {"int16", kind::int16}, {"int32", kind::int32}, {"int64", kind::int64}, {"uint8", kind::uint8},
			{"uint16", kind::uint16}, {"uint32", kind::uint32}, {"float32", kind::float32}, {"float64", kind::float64}};
		for (const auto& [name, k] : widths) { ns->variables[detail::engine_access::symbolize(eng, name)] = width_value(k, 0); }
		ns->variables[detail::engine_access::symbolize(eng, "text")] = script_value::make_function([width_value](const std::vector<script_value>& args) -> checked_result<script_value> {
			if (args.size() != 1 || !args[0].deref().is_int()) { raise("binary::text expects one int length"); }
			const script_int n = args[0].deref().as_int();
			if (n < 0) { raise("binary::text expects a length of 0 or more, got " + std::to_string(n)); }
			return width_value(kind::text, static_cast<uint64_t>(n));
		}, &eng);
		ns->variables[detail::engine_access::symbolize(eng, "bytes")] = script_value::make_function([width_value](const std::vector<script_value>& args) -> checked_result<script_value> {
			if (args.size() != 1 || !args[0].deref().is_int()) { raise("binary::bytes expects one int length"); }
			const script_int n = args[0].deref().as_int();
			if (n < 0) { raise("binary::bytes expects a length of 0 or more, got " + std::to_string(n)); }
			return width_value(kind::bytes, static_cast<uint64_t>(n));
		}, &eng);
		add_lanes(eng);
		ns->variables[detail::engine_access::symbolize(eng, "layout")] = script_value::make_function([e, width_id](const std::vector<script_value>& args) -> checked_result<script_value> {
			return detail::engine_access::make_value_object(*e, std::make_shared<layout>(make_layout(args.data(), args.size(), "binary::layout", e, width_id)));
		}, &eng);

		auto& strings = detail::engine_access::script_namespaces(eng)[detail::engine_access::symbolize(eng, "string")];
		if (!strings) { strings = std::make_shared<script_namespace_data>(); }
		strings->variables[detail::engine_access::symbolize(eng, "from_bytes")] = script_value::make_function([e](const std::vector<script_value>& args) -> checked_result<script_value> {
			if (args.size() != 1) { raise("string::from_bytes expects exactly 1 argument"); }
			const script_value& a = args[0].deref();
			if (!a.is_array()) { raise("string::from_bytes expects an array of bytes, got " + value_kind_name(a)); }
			const script_array* node = a.unchecked_array_node();
			const size_t n = node ? node->size() : 0;
			auto bad = [](size_t i, const std::string& what) { raise("string::from_bytes expects bytes (0 to 255): element " + std::to_string(i) + " is " + what); };
			if (!detail::engine_access::execution_limits(*e).memory_charge(n)) [[unlikely]] { return jai::detail::raise_memory_cap(detail::engine_access::execution_limits(*e)); }
			std::string out(n, '\0');
			if (n == 0) { return script_value(std::move(out), e); }
			if (node->kind() == script_array::kind_t::u8 || node->kind() == script_array::kind_t::c8) {
				std::memcpy(out.data(), node->bytes().data(), n);
			} else if (node->kind() == script_array::kind_t::i64) {
				const auto& ints = node->ints();
				for (size_t i = 0; i < n; ++i) {
					if (ints[i] < 0 || ints[i] > 255) { bad(i, std::to_string(ints[i])); }
					out[i] = static_cast<char>(ints[i]);
				}
			} else {
				size_t i = 0;
				node->for_each(e, [&](const script_value& el) {
					const script_value& v = el.deref();
					if (v.is_int()) {
						const script_int x = v.as_int();
						if (x < 0 || x > 255) { bad(i, std::to_string(x)); }
						out[i] = static_cast<char>(x);
					} else if (v.is_char()) {
						out[i] = v.as_char();
					} else {
						bad(i, value_kind_name(v));
					}
					++i;
				});
			}
			return script_value(std::move(out), e);
		}, &eng);
	}

	inline script_value make_reader(engine& eng, const script_value& source) { return detail::engine_access::make_value_object(eng, std::make_shared<reader>(&eng, source)); }

	// The direct lanes (class_definition::native_method) for the members hot loops call: exact int / float / string
	// arguments only, anything else declines to the bound method
	inline bool lane_int(const script_value* args, size_t argc, script_int& v) {
		if (argc != 1) { return false; }
		const script_value& a = args[0].deref();
		if (a.raw_storage_index() != script_value::TYPEID_INT) { return false; }
		v = a.unchecked_as_int();
		return true;
	}
	template <kind K> bool read_integer_lane(void* s, const script_value& recv, const script_value*, size_t argc, script_value& out) {
		if (argc != 0) { return false; }
		out = script_value(static_cast<reader*>(s)->integer(K), recv.payload_engine());
		return true;
	}
	template <kind K> bool read_float_lane(void* s, const script_value& recv, const script_value*, size_t argc, script_value& out) {
		if (argc != 0) { return false; }
		out = script_value(static_cast<reader*>(s)->floating(K), recv.payload_engine());
		return true;
	}
	template <kind K> bool write_integer_lane(void* s, const script_value& recv, const script_value* args, size_t argc, script_value& out) {
		script_int v;
		if (!lane_int(args, argc, v)) { return false; }
		static_cast<writer*>(s)->charged_to(recv.payload_engine()).integer(K, v);
		out = recv;
		return true;
	}

	inline void add_lanes(engine& eng) {
		using N = class_definition::native_method;
		auto reader_def = detail::engine_access::get_class_definition(eng, "binary::reader");
		auto writer_def = detail::engine_access::get_class_definition(eng, "binary::writer");
		auto on = [&](class_definition& def, const char* name, N fn) { def.add_native_method(detail::engine_access::symbolize(eng, name), fn); };
		class_definition& r = *reader_def;
		on(r, "int8", &read_integer_lane<kind::int8>);
		on(r, "int16", &read_integer_lane<kind::int16>);
		on(r, "int32", &read_integer_lane<kind::int32>);
		on(r, "int64", &read_integer_lane<kind::int64>);
		on(r, "uint8", &read_integer_lane<kind::uint8>);
		on(r, "uint16", &read_integer_lane<kind::uint16>);
		on(r, "uint32", &read_integer_lane<kind::uint32>);
		on(r, "float32", &read_float_lane<kind::float32>);
		on(r, "float64", &read_float_lane<kind::float64>);
		on(r, "text", [](void* s, const script_value& recv, const script_value* args, size_t argc, script_value& out) {
			if (argc == 0) {
				out = script_value(static_cast<reader*>(s)->text(), recv.payload_engine());
				return true;
			}
			script_int n;
			if (!lane_int(args, argc, n)) { return false; }
			out = script_value(static_cast<reader*>(s)->text(n), recv.payload_engine());
			return true;
		});
		on(r, "bytes", [](void* s, const script_value&, const script_value* args, size_t argc, script_value& out) {
			script_int n;
			if (!lane_int(args, argc, n)) { return false; }
			out = static_cast<reader*>(s)->bytes(n);
			return true;
		});
		on(r, "seek", [](void* s, const script_value& recv, const script_value* args, size_t argc, script_value& out) {
			script_int n;
			if (!lane_int(args, argc, n)) { return false; }
			static_cast<reader*>(s)->seek(n);
			out = recv;
			return true;
		});
		on(r, "skip", [](void* s, const script_value& recv, const script_value* args, size_t argc, script_value& out) {
			script_int n;
			if (!lane_int(args, argc, n)) { return false; }
			static_cast<reader*>(s)->skip(n);
			out = recv;
			return true;
		});
		on(r, "pos", [](void* s, const script_value& recv, const script_value*, size_t argc, script_value& out) {
			if (argc != 0) { return false; }
			out = script_value(static_cast<script_int>(static_cast<reader*>(s)->pos()), recv.payload_engine());
			return true;
		});
		on(r, "remaining", [](void* s, const script_value& recv, const script_value*, size_t argc, script_value& out) {
			if (argc != 0) { return false; }
			out = script_value(static_cast<script_int>(static_cast<reader*>(s)->remaining()), recv.payload_engine());
			return true;
		});
		on(r, "at_end", [](void* s, const script_value& recv, const script_value*, size_t argc, script_value& out) {
			if (argc != 0) { return false; }
			out = script_value(static_cast<reader*>(s)->at_end(), recv.payload_engine());
			return true;
		});
		class_definition& w = *writer_def;
		on(w, "int8", &write_integer_lane<kind::int8>);
		on(w, "int16", &write_integer_lane<kind::int16>);
		on(w, "int32", &write_integer_lane<kind::int32>);
		on(w, "int64", &write_integer_lane<kind::int64>);
		on(w, "uint8", &write_integer_lane<kind::uint8>);
		on(w, "uint16", &write_integer_lane<kind::uint16>);
		on(w, "uint32", &write_integer_lane<kind::uint32>);
		on(w, "text", [](void* s, const script_value& recv, const script_value* args, size_t argc, script_value& out) {
			if (argc != 1) { return false; }
			const script_value& a = args[0].deref();
			if (a.raw_storage_index() != script_value::TYPEID_STRING) { return false; }
			static_cast<writer*>(s)->charged_to(recv.payload_engine()).text(a.unchecked_as_string());
			out = recv;
			return true;
		});
	}

} // namespace jai::binary

namespace jai::detail {

	// string's to_bytes() and reader(): observers, registered into the one builtin registry both backends share
	inline void add_binary_string_methods(std::unordered_map<uint64_t, builtin_method>& m, string_symbolizer* sym) {
		using R = checked_result<script_value>;
		m[sym->intern("to_bytes")] = [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
			if (args.size() > 2) return R(make_error_code(runtime_error_code::argument_count_mismatch), "to_bytes() takes 0 to 2 arguments ([offset], [count])");
			for (const auto& a : args) {
				if (!a.is_int()) return R(make_error_code(runtime_error_code::type_mismatch), "to_bytes() arguments must be integers");
			}
			const std::string& s = self.unchecked_text();
			const script_int size = static_cast<script_int>(s.size());
			const script_int offset = args.empty() ? 0 : args[0].unchecked_as_int();
			const script_int count = args.size() < 2 ? size - offset : args[1].unchecked_as_int();
			if (offset < 0 || offset > size || count < 0 || count > size - offset) {
				string_symbolizer* symbols = ctx.get_string_symbolizer();
				if (symbols->frozen()) return R(make_error_code(runtime_error_code::index_out_of_bounds), "to_bytes() range out of bounds");
				const std::string range = args.size() < 2 ? std::to_string(offset) : std::to_string(offset) + ", " + std::to_string(count);
				return R(make_error_code(runtime_error_code::index_out_of_bounds), "to_bytes({0}) out of bounds for string of size {1}",
					symbols->intern(range), symbols->intern(std::to_string(size)));
			}
			engine* eng = ctx.get_engine();
			if (!engine_access::execution_limits(*eng).memory_charge(static_cast<size_t>(count))) [[unlikely]] {
				return raise_memory_cap(engine_access::execution_limits(*eng));
			}
			return binary::byte_array(eng, reinterpret_cast<const unsigned char*>(s.data()) + offset, static_cast<size_t>(count));
		};
		m[sym->intern("reader")] = [](const builtin_method_context& ctx, script_value& self, const std::vector<script_value>& args) -> R {
			if (!args.empty()) return R(make_error_code(runtime_error_code::argument_count_mismatch), "reader() takes no arguments");
			return binary::make_reader(*ctx.get_engine(), self);
		};
	}

} // namespace jai::detail

#endif
