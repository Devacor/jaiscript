#include <jaiscript/detail/regex_library.hpp>
#include <jaiscript/detail/regex.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/utf8.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jai::detail::regex_library {

	using regex::npos;
	using regex::subject;
	using R = checked_result<script_value>;

	struct library;

	struct regex_value {
		regex::compiled_ptr re;
		const library* lib;   // the engine's regex library (engine-lifetime): its matches and their class
		std::string loaded_pattern, loaded_flags;   // a load's fields, compiled by post_load
		bool loaded = false;
		regex_value(regex::compiled_ptr r, const library* l) : re(std::move(r)), lib(l) {}
	};

	// One match, a value of its own: the whole match's text, each group's span inside it, and each slot's position in the
	// subject (bytes for a string, characters for a utf8_string; -1 for a group that did not take part)
	struct regex_match {
		regex::compiled_ptr re;
		std::string text;
		std::vector<size_t> offsets;
		std::vector<script_int> positions;
		bool utf8 = false;
	};

	// The engine's compiled patterns by flags and text, so a pattern built at run time compiles once too; bounded by count
	// and by the bytes the programs take, starting over when either fills
	struct pattern_cache {
		static constexpr size_t k_limit = 256;
		static constexpr size_t k_bytes = size_t(16) << 20;
		std::unordered_map<std::string, regex::compiled_ptr> entries;
		size_t bytes = 0;
	};

	struct regex_constructor {
		std::shared_ptr<const library> lib;
		std::shared_ptr<pattern_cache> cache;
		R operator()(const std::vector<script_value>& args) const;
	};

	struct type_ids {
		uint64_t regex = 0, match = 0;
	};

	[[noreturn]] JAI_NOINLINE void raise(const std::string& text) { throw std::runtime_error(text); }

	// a parallel worker's frozen symbols park no text: it reads as every host failure there does
	JAI_NOINLINE R refused(engine* eng, const std::string& text) {
		const uint64_t id = engine_access::park_error_text(*eng, text);
		return R(make_error_code(runtime_error_code::invalid_operation), id ? std::string_view("{0}") : host_exception_slotless_text, id);
	}

	// The bytes a regex call reads, and whether they are a utf8_string's
	struct subject_text {
		const std::string* held = nullptr;   // a script string's own bytes; a C++-bound string's are copied into bound
		std::string bound;
		bool utf8 = false;
		const std::string& text() const { return held ? *held : bound; }
		subject kind() const { return utf8 ? subject::utf8 : subject::bytes; }
	};

	JAI_NOINLINE std::string kind_name(const script_value& v, engine* eng) { return detail::slot_source_type_name(v, eng); }

	inline bool text_of(const script_value& v, subject_text& out) {
		const script_value& d = v.deref();
		if (const std::string* t = d.text_bytes()) {
			out.held = t;
			out.utf8 = d.is_utf8_string();
			return true;
		}
		if (d.is_string()) {
			out.bound = d.as_string();
			return true;
		}
		return false;
	}

	inline subject_text subject_arg(const std::vector<script_value>& args, size_t i, const char* method, engine* eng) {
		subject_text s;
		if (i >= args.size() || !text_of(args[i], s)) {
			raise(std::string("regex: ") + method + " expects a string or utf8_string, got " + (i < args.size() ? kind_name(args[i], eng) : std::string("nothing")));
		}
		return s;
	}

	inline script_value text_value(engine* eng, std::string bytes, bool utf8) {
		return utf8 ? script_value::make_utf8(std::move(bytes), eng) : script_value(std::move(bytes), eng);
	}

	inline script_int character_count(const std::string& text, size_t from, size_t to) {
		script_int n = 0;
		for (size_t at = from; at < to; at += utf8::decode(text, at).length) { ++n; }
		return n;
	}

	inline size_t byte_of_character(const std::string& text, script_int index) {
		size_t at = 0;
		for (script_int k = 0; k < index && at < text.size(); ++k) { at += utf8::decode(text, at).length; }
		return at;
	}

	// Positions of successive matches, counted once through the text
	struct position_counter {
		const std::string& text;
		bool utf8;
		size_t byte = 0;
		script_int chars = 0;
		script_int at(size_t offset) {
			if (!utf8) { return static_cast<script_int>(offset); }
			if (offset < byte) { byte = 0; chars = 0; }
			chars += character_count(text, byte, offset);
			byte = offset;
			return chars;
		}
	};

	inline regex_match match_of(const regex::compiled_ptr& re, const std::string& text, bool utf8, const std::vector<size_t>& slots, position_counter& counter) {
		regex_match m;
		m.re = re;
		m.utf8 = utf8;
		const size_t begin = slots[0], end = slots[1];
		m.text.assign(text, begin, end - begin);
		m.offsets.resize(slots.size());
		m.positions.resize(slots.size());
		const script_int base = counter.at(begin);
		for (size_t k = 0; k < slots.size(); ++k) {
			const size_t s = slots[k];
			const bool open = slots[k & ~size_t(1)] == npos || slots[k | 1] == npos;
			if (s == npos || open) {
				m.offsets[k] = npos;
				m.positions[k] = -1;
				continue;
			}
			m.offsets[k] = s - begin;
			m.positions[k] = utf8 ? base + character_count(text, begin, s) : static_cast<script_int>(s);
		}
		return m;
	}

	inline regex::compiled_ptr compile_cached(engine* eng, pattern_cache* cache, const std::string& pattern, const std::string& flags, std::string& error) {
		const bool cached = cache && !engine_access::parallel_region_active(*eng);
		std::string key;
		if (cached) {
			key.reserve(flags.size() + 1 + pattern.size());
			key.append(flags).push_back('\0');
			key.append(pattern);
			if (auto it = cache->entries.find(key); it != cache->entries.end()) { return it->second; }
		}
		regex::compiled_ptr re = regex::compile(pattern, flags, error);
		if (re && cached) {
			const size_t bytes = regex::footprint(*re) + key.size();
			if (bytes <= pattern_cache::k_bytes / 4) {
				if (cache->entries.size() >= pattern_cache::k_limit || cache->bytes + bytes > pattern_cache::k_bytes) {
					cache->entries.clear();
					cache->bytes = 0;
				}
				cache->bytes += bytes;
				cache->entries.emplace(std::move(key), re);
			}
		}
		return re;
	}

	bool constant_text(const expression* e, std::string& out) {
		if (!e) { return false; }
		if (e->get_type() == node_type::literal_expr) {
			const script_value& v = static_cast<const literal_expr*>(e)->value;
			if (v.raw_storage_index() != script_value::TYPEID_STRING) { return false; }
			out = v.unchecked_as_string();
			return true;
		}
		if (e->get_type() != node_type::binary_expr) { return false; }
		// a + chain of literals, walked down its left spine
		std::vector<const expression*> rights;
		const expression* at = e;
		while (at->get_type() == node_type::binary_expr) {
			const auto* b = static_cast<const binary_expr*>(at);
			if (b->op.type != token_type::plus) { return false; }
			rights.push_back(b->right.get());
			at = b->left.get();
		}
		std::string text;
		if (!constant_text(at, text) || at->get_type() != node_type::literal_expr) { return false; }
		for (size_t k = rights.size(); k-- > 0;) {
			const expression* r = rights[k];
			if (!r || r->get_type() != node_type::literal_expr) { return false; }
			const script_value& v = static_cast<const literal_expr*>(r)->value;
			if (v.raw_storage_index() != script_value::TYPEID_STRING) { return false; }
			text += v.unchecked_as_string();
		}
		out = std::move(text);
		return true;
	}

	// ===== the replacement text =====

	struct piece {
		std::string literal;
		size_t group = npos;
	};

	JAI_NOINLINE void raise_group_number(size_t n, size_t pos, size_t groups) {
		raise("regex: the replacement names group " + std::to_string(n) + " at position " + std::to_string(pos) + ", but the pattern has " +
		      std::to_string(groups) + (groups == 1 ? " group" : " groups"));
	}

	// $0 to $99 (the longest number that names a group), ${n}, ${name} and $$
	inline std::vector<piece> parse_replacement(const std::string& t, const regex::compiled& re) {
		std::vector<piece> out(1);
		const size_t groups = regex::group_count(re);
		auto digit = [](char c) { return c >= '0' && c <= '9'; };
		for (size_t i = 0; i < t.size(); ++i) {
			if (t[i] != '$') { out.back().literal.push_back(t[i]); continue; }
			const size_t at = i;
			const char c = i + 1 < t.size() ? t[i + 1] : '\0';
			if (c == '$') { out.back().literal.push_back('$'); ++i; continue; }
			size_t group = npos;
			if (digit(c)) {
				const size_t one = static_cast<size_t>(c - '0');
				const bool two = i + 2 < t.size() && digit(t[i + 2]);
				const size_t both = two ? one * 10 + static_cast<size_t>(t[i + 2] - '0') : npos;
				if (two && both <= groups) { group = both; i += 2; }
				else if (one <= groups) { group = one; i += 1; }
				else { raise_group_number(two ? both : one, at, groups); }
			} else if (c == '{') {
				const size_t close = t.find('}', i + 2);
				if (close == std::string::npos || close == i + 2) {
					raise("regex: '${' at position " + std::to_string(at) + " of the replacement needs a group number or name and '}'");
				}
				const std::string name = t.substr(i + 2, close - i - 2);
				bool number = true;
				for (const char ch : name) { number = number && digit(ch); }
				if (number) {
					size_t n = 0;
					for (const char ch : name) { n = n > 1000 ? n : n * 10 + static_cast<size_t>(ch - '0'); }
					if (n > groups) { raise_group_number(n, at, groups); }
					group = n;
				} else {
					group = regex::group_index(re, name);
					if (group == npos) {
						raise("regex: the replacement names group '" + name + "' at position " + std::to_string(at) + ", which the pattern does not have");
					}
				}
				i = close;
			} else {
				raise("regex: a '$' at position " + std::to_string(at) + " of the replacement starts no group reference (write $$ for a dollar sign)");
			}
			out.back().group = group;
			out.emplace_back();
		}
		return out;
	}

	// ===== the methods =====

	struct library {
		engine* eng;
		type_ids ids;
		class_definition* regex_def;   // engine-lifetime, like every class definition
		class_definition* match_def;

		// a bound value the way a constructor makes one (no identity record: a regex or a match is a value)
		script_value make_bound(class_definition& def, std::shared_ptr<void> object) const {
			auto instance = detail::make_native_instance(def, def.get_name(), std::move(object), eng);
			script_value made = script_value::make_object(def.get_name(), instance, eng);
			if (engine_access::parallel_region_active(*eng)) [[unlikely]] { detail::note_region_made(made, eng); }
			return made;
		}
		script_value make_regex(const regex::compiled_ptr& re) const { return make_bound(*regex_def, std::make_shared<regex_value>(re, this)); }

		const regex_value& self(const std::vector<script_value>& args) const {
			return *detail::extract_cpp_object_ptr<regex_value>(args[0], ids.regex);
		}
		const regex_match& match(const std::vector<script_value>& args) const {
			return *detail::extract_cpp_object_ptr<regex_match>(args[0], ids.match);
		}

		static void require_subject(const regex::compiled& re, const subject_text& s) {
			if (std::string refusal = regex::subject_refusal(re, s.kind()); !refusal.empty()) { raise(refusal); }
		}

		script_value make_match(regex_match m) const { return make_bound(*match_def, std::make_shared<regex_match>(std::move(m))); }

		script_value full_match(const regex_value& rv, const subject_text& s) const {
			require_subject(*rv.re, s);
			regex::searcher searcher(*rv.re, s.kind());
			return script_value(searcher.full(s.text()), eng);
		}
		R matches(const std::vector<script_value>& args) const { return full_match(self(args), subject_arg(args, 1, "matches", eng)); }

		script_value search_from(const regex_value& rv, const subject_text& s, size_t from) const {
			const std::string& text = s.text();
			regex::searcher searcher(*rv.re, s.kind());
			std::vector<size_t> slots;
			if (!searcher.find(text, from, true, slots)) { return script_value(std::monostate{}, eng); }
			position_counter counter{text, s.utf8};
			return make_match(match_of(rv.re, text, s.utf8, slots, counter));
		}

		R search(const std::vector<script_value>& args) const {
			const regex_value& rv = self(args);
			const subject_text s = subject_arg(args, 1, "search", eng);
			require_subject(*rv.re, s);
			const std::string& text = s.text();
			size_t from = 0;
			if (args.size() == 3) {
				const script_value& p = args[2].deref();
				if (!p.is_int()) { raise("regex: search expects an int start position, got " + kind_name(p, eng)); }
				const script_int start = p.as_int();
				const script_int size = s.utf8 ? character_count(text, 0, text.size()) : static_cast<script_int>(text.size());
				if (start < 0 || start > size) {
					raise("regex: search position " + std::to_string(start) + " is outside the text (0 to " + std::to_string(size) + ")");
				}
				from = s.utf8 ? byte_of_character(text, start) : static_cast<size_t>(start);
			}
			return search_from(rv, s, from);
		}

		R find_all(const std::vector<script_value>& args) const {
			const regex_value& rv = self(args);
			const subject_text s = subject_arg(args, 1, "find_all", eng);
			require_subject(*rv.re, s);
			const std::string& text = s.text();
			script_value out = script_value::make_array(nullptr, eng);
			script_array& items = *out.unchecked_get_array_storage();
			auto& limits = engine_access::execution_limits(*eng);
			regex::searcher searcher(*rv.re, s.kind());
			position_counter counter{text, s.utf8};
			std::vector<size_t> slots;
			for (size_t from = 0; from <= text.size() && searcher.find(text, from, true, slots);) {
				const size_t begin = slots[0], end = slots[1];
				if (!limits.memory_charge(sizeof(script_value) + sizeof(regex_match) + (end - begin))) [[unlikely]] { return raise_memory_cap(limits); }
				items.push(make_match(match_of(rv.re, text, s.utf8, slots, counter)));
				from = end == begin ? regex::next_character(text, end, s.kind()) : end;
			}
			return out;
		}

		R replace(const std::vector<script_value>& args, bool every) const {
			const regex::compiled_ptr re = self(args).re;
			const char* name = every ? "replace" : "replace_first";
			const subject_text s = subject_arg(args, 1, name, eng);
			require_subject(*re, s);
			const script_value with = args[2].deref();
			subject_text template_text;
			const bool callback = with.is_function();
			if (!callback && !text_of(with, template_text)) {
				raise(std::string("regex: ") + name + " expects a string or a function as the replacement, got " + kind_name(with, eng));
			}
			// a function replacement may write the variables this call read: the search keeps its own regex and text
			std::string owned;
			if (callback) { owned = s.text(); }
			const std::string& text = callback ? owned : s.text();
			std::vector<piece> pieces;
			bool groups = callback;
			if (!callback) {
				pieces = parse_replacement(template_text.text(), *re);
				for (const piece& p : pieces) { groups = groups || (p.group != npos && p.group != 0); }
			}
			regex::searcher searcher(*re, s.kind());
			position_counter counter{text, s.utf8};
			std::vector<size_t> slots;
			std::string out;
			size_t last = 0;
			for (size_t from = 0; from <= text.size() && searcher.find(text, from, groups, slots);) {
				const size_t begin = slots[0], end = slots[1];
				out.append(text, last, begin - last);
				if (callback) {
					script_value made = make_match(match_of(re, text, s.utf8, slots, counter));
					R result = with.as_function()(std::vector<script_value>{std::move(made)});
					if (!result) { return result; }
					if (engine_access::backend_unwinding(*eng)) { return script_value(std::monostate{}, eng); }
					subject_text piece_text;
					if (!text_of(result.value(), piece_text)) {
						raise(std::string("regex: the ") + name + " function must return a string, got " + kind_name(result.value(), eng));
					}
					out += piece_text.text();
				} else {
					for (const piece& p : pieces) {
						out += p.literal;
						if (p.group != npos && slots[2 * p.group] != npos && slots[2 * p.group + 1] != npos) {
							out.append(text, slots[2 * p.group], slots[2 * p.group + 1] - slots[2 * p.group]);
						}
					}
				}
				last = end;
				if (!every) { break; }
				from = end == begin ? regex::next_character(text, end, s.kind()) : end;
			}
			out.append(text, last, std::string::npos);
			auto& limits = engine_access::execution_limits(*eng);
			if (!limits.memory_charge(out.size())) [[unlikely]] { return raise_memory_cap(limits); }
			return text_value(eng, std::move(out), s.utf8);
		}

		// ECMAScript's split: an empty match at a piece's start splits nothing, a match starting at the end never splits, and
		// each split's capture groups (an unmatched one as "") follow its piece; max caps the splits, the rest the last piece
		R split(const std::vector<script_value>& args) const {
			const regex_value& rv = self(args);
			const subject_text s = subject_arg(args, 1, "split", eng);
			require_subject(*rv.re, s);
			script_int max_splits = -1;
			if (args.size() == 3) {
				const script_value& m = args[2].deref();
				if (!m.is_int()) { raise("regex: split expects an int count of splits, got " + kind_name(m, eng)); }
				max_splits = m.as_int();
			}
			const std::string& text = s.text();
			const size_t groups = regex::group_count(*rv.re);
			script_value out = script_value::make_array(s.utf8 ? nullptr : engine_access::get_type_info_string(*eng), eng);
			script_array& items = *out.unchecked_get_array_storage();
			auto& limits = engine_access::execution_limits(*eng);
			bool denied = false;
			auto push = [&](size_t at, size_t count) {
				if (denied || !limits.memory_charge(sizeof(script_value) + count)) { denied = true; return; }
				items.push(text_value(eng, text.substr(at, count), s.utf8));
			};
			regex::searcher searcher(*rv.re, s.kind());
			std::vector<size_t> slots;
			if (text.empty()) {
				if (!searcher.find(text, 0, false, slots)) { push(0, 0); }
				if (denied) { return raise_memory_cap(limits); }
				return out;
			}
			size_t p = 0, q = 0;
			script_int splits = 0;
			while (q < text.size() && (max_splits < 0 || splits < max_splits)) {
				if (!searcher.find(text, q, groups > 0, slots) || slots[0] >= text.size()) { break; }
				const size_t begin = slots[0], end = slots[1];
				if (end == p) {
					q = regex::next_character(text, begin, s.kind());
					continue;
				}
				push(p, begin - p);
				for (size_t g = 1; g <= groups; ++g) {
					const size_t a = slots[2 * g], b = slots[2 * g + 1];
					if (a == npos || b == npos) { push(0, 0); } else { push(a, b - a); }
				}
				++splits;
				p = q = end;
			}
			push(p, text.size() - p);
			if (denied) { return raise_memory_cap(limits); }
			return out;
		}

		size_t group_arg(const regex_match& m, const script_value& g, const char* method) const {
			const script_value& d = g.deref();
			const size_t groups = m.offsets.size() / 2 - 1;
			if (d.is_int()) {
				const script_int i = d.as_int();
				if (i < 0 || static_cast<uint64_t>(i) > groups) {
					raise("regex::match has no group " + std::to_string(i) + " (it has " + std::to_string(groups) + (groups == 1 ? " group)" : " groups)"));
				}
				return static_cast<size_t>(i);
			}
			subject_text name;
			if (!text_of(d, name)) {
				raise(std::string("regex::match: ") + method + " expects a group number or name, got " + kind_name(d, eng));
			}
			const size_t index = regex::group_index(*m.re, name.text());
			if (index == npos) { raise("regex::match has no group named '" + name.text() + "'"); }
			return index;
		}

		R group_text(const std::vector<script_value>& args) const {
			const regex_match& m = match(args);
			const size_t g = group_arg(m, args[1], "[]");
			const size_t a = m.offsets[2 * g], b = m.offsets[2 * g + 1];
			if (a == npos) { return script_value(std::monostate{}, eng); }
			return text_value(eng, m.text.substr(a, b - a), m.utf8);
		}

		R bound(const std::vector<script_value>& args, bool end) const {
			const regex_match& m = match(args);
			const size_t g = args.size() > 1 ? group_arg(m, args[1], end ? "end" : "start") : 0;
			return script_value(m.positions[2 * g + (end ? 1 : 0)], eng);
		}
	};

	R regex_constructor::operator()(const std::vector<script_value>& args) const {
		engine* eng = lib->eng;
		if (args.size() > 2) {
			return refused(eng, "regex expects a pattern and optional flags, got " + std::to_string(args.size()) + " arguments");
		}
		subject_text pattern, flags;
		if (!args.empty() && !text_of(args[0], pattern)) { return refused(eng, "regex: the pattern must be a string, got " + kind_name(args[0], eng)); }
		if (args.size() == 2 && !text_of(args[1], flags)) { return refused(eng, "regex: the flags must be a string, got " + kind_name(args[1], eng)); }
		std::string error;
		regex::compiled_ptr re = compile_cached(eng, cache.get(), pattern.text(), flags.text(), error);
		if (!re) { return refused(eng, error); }
		return lib->make_regex(re);
	}

	bool is_constructor(const script_value& callee) {
		const script_value& d = callee.deref();
		return d.raw_storage_index() == script_value::TYPEID_FUNCTION && d.unchecked_as_function().target<regex_constructor>() != nullptr;
	}

	bool make_constant(const script_value& callee, const regex::compiled_ptr& re, script_value& out) {
		const script_value& d = callee.deref();
		if (d.raw_storage_index() != script_value::TYPEID_FUNCTION) { return false; }
		const regex_constructor* ctor = d.unchecked_as_function().target<regex_constructor>();
		if (!ctor) { return false; }
		out = ctor->lib->make_regex(re);
		return true;
	}

	// The direct lanes (class_definition::native_method) of the calls loops make: a text subject and the plain arity; anything
	// else declines to the bound method, which raises its text
	bool matches_lane(void* self, const script_value&, const script_value* args, size_t argc, script_value& out) {
		subject_text s;
		if (argc != 1 || !text_of(args[0], s)) { return false; }
		const regex_value& rv = *static_cast<const regex_value*>(self);
		out = rv.lib->full_match(rv, s);
		return true;
	}
	bool search_lane(void* self, const script_value&, const script_value* args, size_t argc, script_value& out) {
		subject_text s;
		if (argc != 1 || !text_of(args[0], s)) { return false; }
		const regex_value& rv = *static_cast<const regex_value*>(self);
		library::require_subject(*rv.re, s);
		out = rv.lib->search_from(rv, s, 0);
		return true;
	}
	bool match_text_lane(void* self, const script_value& receiver, const script_value*, size_t argc, script_value& out) {
		if (argc != 0) { return false; }
		const regex_match& m = *static_cast<const regex_match*>(self);
		out = text_value(receiver.payload_engine(), m.text, m.utf8);
		return true;
	}
	template <bool End>
	bool match_bound_lane(void* self, const script_value& receiver, const script_value* args, size_t argc, script_value& out) {
		if (argc > 1) { return false; }
		const regex_match& m = *static_cast<const regex_match*>(self);
		size_t g = 0;
		if (argc == 1) {
			const script_value& a = args[0].deref();
			if (a.raw_storage_index() != script_value::TYPEID_INT) { return false; }
			const script_int i = a.unchecked_as_int();
			if (i < 0 || static_cast<uint64_t>(i) >= m.offsets.size() / 2) { return false; }
			g = static_cast<size_t>(i);
		}
		out = script_value(m.positions[2 * g + (End ? 1 : 0)], receiver.payload_engine());
		return true;
	}

	template <R (library::*Method)(const std::vector<script_value>&) const>
	struct library_call {
		std::shared_ptr<const library> lib;
		R operator()(const std::vector<script_value>& args) const { return ((*lib).*Method)(args); }
	};

	void register_library(engine& eng) {
		dynamic_binder<regex_value>(eng, "regex")
			.property("pattern", [](const regex_value& r) -> std::string { return regex::pattern_of(*r.re); }, [](regex_value&, const std::string&) {})
			.property("flags", [](const regex_value& r) -> std::string { return regex::flags_of(*r.re); }, [](regex_value&, const std::string&) {})
			.post_load_hook([](regex_value& r, int) {
				if (!r.loaded) { return; }
				r.loaded = false;
				std::string error;
				regex::compiled_ptr re = regex::compile(r.loaded_pattern, r.loaded_flags, error);
				if (!re) { throw std::runtime_error(error); }
				r.re = std::move(re);
			})
			.build();
		dynamic_binder<regex_match>(eng, "regex::match").build();

		const auto regex_class = engine_access::get_class_definition(eng, "regex");
		const auto match_class = engine_access::get_class_definition(eng, "regex::match");
		auto lib = std::make_shared<library>(library{&eng, type_ids{regex_class->get_type_id(), match_class->get_type_id()}, regex_class.get(), match_class.get()});

		auto method = [&](class_definition& def, const char* name, size_t arity, script_function fn) {
			def.add_method(name, std::move(fn), arity);
			def.note_host_overload(engine_access::symbolize(eng, name), arity, true);
		};
		class_definition& rc = *regex_class;
		method(rc, "matches", 1, library_call<&library::matches>{lib});
		method(rc, "search", 1, library_call<&library::search>{lib});
		method(rc, "search", 2, library_call<&library::search>{lib});
		method(rc, "find_all", 1, library_call<&library::find_all>{lib});
		method(rc, "split", 1, library_call<&library::split>{lib});
		method(rc, "split", 2, library_call<&library::split>{lib});
		method(rc, "replace", 2, [lib](const std::vector<script_value>& a) -> R { return lib->replace(a, true); });
		method(rc, "replace_first", 2, [lib](const std::vector<script_value>& a) -> R { return lib->replace(a, false); });
		for (const char* field : {"pattern", "flags"}) {
			const bool is_pattern = field[0] == 'p';
			rc.add_method(std::string("_set_") + field, [lib, is_pattern, field](const std::vector<script_value>& a) -> R {
				engine* e = lib->eng;
				if (!e->loading()) {
					return R(make_error_code(runtime_error_code::invalid_assignment_target), "Cannot assign to '{0}': it is read-only", engine_access::symbolize(*e, field));
				}
				regex_value* r = a.size() == 2 ? detail::extract_cpp_object_ptr<regex_value>(a[0], lib->ids.regex) : nullptr;
				subject_text t;
				if (!r || !text_of(a[1], t)) { return refused(e, std::string("regex: a saved ") + field + " must be a string"); }
				(is_pattern ? r->loaded_pattern : r->loaded_flags) = t.text();
				if (!r->loaded) {
					r->loaded = true;
					if (is_pattern) { r->loaded_flags.clear(); } else { r->loaded_pattern.clear(); }
				}
				return script_value(std::monostate{}, e);
			}, 1);
		}
		rc.add_static_method("escape", [lib](const std::vector<script_value>& a) -> R {
			engine* e = lib->eng;
			subject_text t;
			if (a.size() != 1 || !text_of(a[0], t)) {
				return refused(e, "regex::escape expects one string, got " + (a.size() == 1 ? kind_name(a[0], e) : std::to_string(a.size()) + " arguments"));
			}
			return text_value(e, regex::escape(t.text()), t.utf8);
		}, 1);

		class_definition& mc = *match_class;
		method(mc, "text", 0, [lib](const std::vector<script_value>& a) -> R {
			const regex_match& m = lib->match(a);
			return text_value(lib->eng, m.text, m.utf8);
		});
		method(mc, "[]", 1, library_call<&library::group_text>{lib});
		method(mc, "start", 0, [lib](const std::vector<script_value>& a) -> R { return lib->bound(a, false); });
		method(mc, "start", 1, [lib](const std::vector<script_value>& a) -> R { return lib->bound(a, false); });
		method(mc, "end", 0, [lib](const std::vector<script_value>& a) -> R { return lib->bound(a, true); });
		method(mc, "end", 1, [lib](const std::vector<script_value>& a) -> R { return lib->bound(a, true); });
		method(mc, "size", 0, [lib](const std::vector<script_value>& a) -> R {
			return script_value(static_cast<script_int>(lib->match(a).offsets.size() / 2), lib->eng);
		});

		rc.add_native_method(engine_access::symbolize(eng, "matches"), &matches_lane);
		rc.add_native_method(engine_access::symbolize(eng, "search"), &search_lane);
		mc.add_native_method(engine_access::symbolize(eng, "text"), &match_text_lane);
		mc.add_native_method(engine_access::symbolize(eng, "start"), &match_bound_lane<false>);
		mc.add_native_method(engine_access::symbolize(eng, "end"), &match_bound_lane<true>);

		eng.add_function_with_arity("regex", regex_constructor{lib, std::make_shared<pattern_cache>()}, SIZE_MAX);
	}

} // namespace jai::detail::regex_library
