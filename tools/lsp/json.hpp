#pragma once

// The JSON the protocol needs: a value tree, a strict parser and a compact writer.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jai_lsp {

class json {
public:
	enum class kind : uint8_t { null, boolean, number, string, array, object };
	using member = std::pair<std::string, json>;

	json() = default;
	json(std::nullptr_t) {}
	json(bool b) : kind_(kind::boolean), bool_(b) {}
	json(int v) : kind_(kind::number), int_(v), is_int_(true) {}
	json(int64_t v) : kind_(kind::number), int_(v), is_int_(true) {}
	json(unsigned v) : kind_(kind::number), int_(v), is_int_(true) {}
	json(unsigned long v) : kind_(kind::number), int_(static_cast<int64_t>(v)), is_int_(true) {}
	json(unsigned long long v) : kind_(kind::number), int_(static_cast<int64_t>(v)), is_int_(true) {}
	json(double v) : kind_(kind::number), double_(v) {}
	json(const char* s) : kind_(kind::string), string_(s) {}
	json(std::string s) : kind_(kind::string), string_(std::move(s)) {}
	json(std::string_view s) : kind_(kind::string), string_(s) {}

	static json array() { json j; j.kind_ = kind::array; return j; }
	static json object() { json j; j.kind_ = kind::object; return j; }

	bool is_null() const { return kind_ == kind::null; }
	bool is_string() const { return kind_ == kind::string; }
	bool is_integer() const { return kind_ == kind::number && is_int_; }
	bool is_array() const { return kind_ == kind::array; }
	bool is_object() const { return kind_ == kind::object; }

	const std::string& str() const { static const std::string empty; return kind_ == kind::string ? string_ : empty; }
	// a fraction truncates; a number past int64 (or not a number) is the fallback
	int64_t integer(int64_t fallback = 0) const {
		if (kind_ != kind::number) { return fallback; }
		if (is_int_) { return int_; }
		return double_ >= -9223372036854775808.0 && double_ < 9223372036854775808.0 ? static_cast<int64_t>(double_) : fallback;
	}
	bool boolean(bool fallback = false) const { return kind_ == kind::boolean ? bool_ : fallback; }

	size_t size() const { return kind_ == kind::array ? items_.size() : kind_ == kind::object ? members_.size() : 0; }
	const json& operator[](size_t i) const { return i < items_.size() ? items_[i] : null_value(); }
	const std::vector<json>& items() const { return items_; }

	const json& operator[](std::string_view key) const {
		if (kind_ == kind::object) {
			for (const auto& [k, v] : members_) { if (k == key) { return v; } }
		}
		return null_value();
	}
	bool contains(std::string_view key) const {
		if (kind_ != kind::object) { return false; }
		for (const auto& [k, v] : members_) { if (k == key) { return true; } }
		return false;
	}
	json& operator[](const char* key) { return at_key(key); }
	json& operator[](const std::string& key) { return at_key(key); }
	json& set(std::string_view key, json value) { at_key(key) = std::move(value); return *this; }
	json& push(json value) {
		if (kind_ != kind::array) { *this = array(); }
		items_.push_back(std::move(value));
		return items_.back();
	}

	std::string dump() const { std::string out; write(out); return out; }

	static json parse(std::string_view text) {
		parser p{text};
		p.skip_space();
		json value = p.value(0);
		p.skip_space();
		if (p.pos != text.size()) { throw std::runtime_error("trailing characters after JSON value"); }
		return value;
	}

private:
	kind kind_ = kind::null;
	bool bool_ = false;
	bool is_int_ = false;
	int64_t int_ = 0;
	double double_ = 0.0;
	std::string string_;
	std::vector<json> items_;
	std::vector<member> members_;

	static const json& null_value() { static const json n; return n; }

	json& at_key(std::string_view key) {
		if (kind_ != kind::object) { *this = object(); }
		for (auto& [k, v] : members_) { if (k == key) { return v; } }
		members_.emplace_back(std::string(key), json());
		return members_.back().second;
	}

	static void write_string(std::string& out, const std::string& s) {
		out += '"';
		for (unsigned char c : s) {
			switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				default:
					if (c < 0x20) {
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					} else {
						out += static_cast<char>(c);
					}
			}
		}
		out += '"';
	}

	void write(std::string& out) const {
		switch (kind_) {
			case kind::null: out += "null"; break;
			case kind::boolean: out += bool_ ? "true" : "false"; break;
			case kind::number:
				if (is_int_) {
					out += std::to_string(int_);
				} else {
					char buf[32];
					std::snprintf(buf, sizeof(buf), "%.17g", double_);
					out += buf;
				}
				break;
			case kind::string: write_string(out, string_); break;
			case kind::array:
				out += '[';
				for (size_t i = 0; i < items_.size(); ++i) {
					if (i) { out += ','; }
					items_[i].write(out);
				}
				out += ']';
				break;
			case kind::object:
				out += '{';
				for (size_t i = 0; i < members_.size(); ++i) {
					if (i) { out += ','; }
					write_string(out, members_[i].first);
					out += ':';
					members_[i].second.write(out);
				}
				out += '}';
				break;
		}
	}

	struct parser {
		std::string_view text;
		size_t pos = 0;

		[[noreturn]] void fail(const char* what) const {
			throw std::runtime_error(std::string("JSON parse error at ") + std::to_string(pos) + ": " + what);
		}
		void skip_space() {
			while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) { ++pos; }
		}
		bool consume(std::string_view word) {
			if (text.substr(pos, word.size()) != word) { return false; }
			pos += word.size();
			return true;
		}
		json value(int depth) {
			if (depth > 256) { fail("nesting too deep"); }
			if (pos >= text.size()) { fail("unexpected end"); }
			char c = text[pos];
			if (c == '{') { return object_value(depth); }
			if (c == '[') { return array_value(depth); }
			if (c == '"') { return json(string_value()); }
			if (consume("true")) { return json(true); }
			if (consume("false")) { return json(false); }
			if (consume("null")) { return json(); }
			return number_value();
		}
		json object_value(int depth) {
			json out = json::object();
			++pos;
			skip_space();
			if (pos < text.size() && text[pos] == '}') { ++pos; return out; }
			for (;;) {
				skip_space();
				if (pos >= text.size() || text[pos] != '"') { fail("expected key"); }
				std::string key = string_value();
				skip_space();
				if (pos >= text.size() || text[pos] != ':') { fail("expected ':'"); }
				++pos;
				skip_space();
				out.members_.emplace_back(std::move(key), value(depth + 1));
				skip_space();
				if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
				if (pos < text.size() && text[pos] == '}') { ++pos; return out; }
				fail("expected ',' or '}'");
			}
		}
		json array_value(int depth) {
			json out = json::array();
			++pos;
			skip_space();
			if (pos < text.size() && text[pos] == ']') { ++pos; return out; }
			for (;;) {
				skip_space();
				out.items_.push_back(value(depth + 1));
				skip_space();
				if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
				if (pos < text.size() && text[pos] == ']') { ++pos; return out; }
				fail("expected ',' or ']'");
			}
		}
		static void append_utf8(std::string& out, uint32_t cp) {
			if (cp < 0x80) {
				out += static_cast<char>(cp);
			} else if (cp < 0x800) {
				out += static_cast<char>(0xC0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			} else if (cp < 0x10000) {
				out += static_cast<char>(0xE0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			} else {
				out += static_cast<char>(0xF0 | (cp >> 18));
				out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}
		uint32_t hex4() {
			if (pos + 4 > text.size()) { fail("bad \\u escape"); }
			uint32_t v = 0;
			for (int i = 0; i < 4; ++i) {
				char h = text[pos++];
				v <<= 4;
				if (h >= '0' && h <= '9') { v |= static_cast<uint32_t>(h - '0'); }
				else if (h >= 'a' && h <= 'f') { v |= static_cast<uint32_t>(h - 'a' + 10); }
				else if (h >= 'A' && h <= 'F') { v |= static_cast<uint32_t>(h - 'A' + 10); }
				else { fail("bad hex digit"); }
			}
			return v;
		}
		std::string string_value() {
			std::string out;
			++pos;
			while (pos < text.size()) {
				char c = text[pos++];
				if (c == '"') { return out; }
				if (static_cast<unsigned char>(c) < 0x20) { fail("control character in a string"); }
				if (c != '\\') { out += c; continue; }
				if (pos >= text.size()) { break; }
				char e = text[pos++];
				switch (e) {
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u': {
						uint32_t cp = hex4();
						if (cp >= 0xDC00 && cp < 0xE000) { fail("unpaired surrogate"); }
						if (cp >= 0xD800 && cp < 0xDC00) {
							if (pos + 2 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') { fail("unpaired surrogate"); }
							pos += 2;
							const uint32_t low = hex4();
							if (low < 0xDC00 || low >= 0xE000) { fail("unpaired surrogate"); }
							cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						}
						append_utf8(out, cp);
						break;
					}
					default: fail("bad escape");
				}
			}
			fail("unterminated string");
		}
		// RFC 8259: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
		json number_value() {
			const size_t start = pos;
			auto digits = [&] {
				const size_t from = pos;
				while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { ++pos; }
				return pos - from;
			};
			if (pos < text.size() && text[pos] == '-') { ++pos; }
			const size_t whole_start = pos;
			const size_t whole = digits();
			if (whole == 0) { fail(pos == start ? "unexpected character" : "bad number"); }
			if (whole > 1 && text[whole_start] == '0') { fail("bad number"); }
			bool integral = true;
			if (pos < text.size() && text[pos] == '.') {
				++pos;
				if (digits() == 0) { fail("bad number"); }
				integral = false;
			}
			if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
				++pos;
				if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) { ++pos; }
				if (digits() == 0) { fail("bad number"); }
				integral = false;
			}
			std::string spelled(text.substr(start, pos - start));
			if (integral) {
				try { return json(static_cast<int64_t>(std::stoll(spelled))); } catch (...) {}
			}
			try { return json(std::stod(spelled)); } catch (...) { fail("bad number"); }
		}
	};
};

} // namespace jai_lsp
