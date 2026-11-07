#include <jaiscript/detail/value_text.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/script_map.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/container_enforce.hpp>
#include <jaiscript/detail/object_borrow.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <algorithm>
#include <charconv>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jai::detail {

namespace value_text_walk {

	void append_hex_escape(std::string& out, unsigned char c) {
		static const char digits[] = "0123456789abcdef";
		out += "\\x";
		out += digits[c >> 4];
		out += digits[c & 15];
	}

	// text as a literal of its quote: \n \r \t \\ and the quote escaped, other control bytes as \xNN; a string's bytes
	// past 0x7f stay as they are (UTF-8 text reads as text), a char's become \xNN (one byte is no text)
	void append_quoted(std::string& out, std::string_view text, char quote) {
		out.reserve(out.size() + text.size() + 2);
		out += quote;
		size_t run = 0;
		for (size_t i = 0; i < text.size(); ++i) {
			const unsigned char c = static_cast<unsigned char>(text[i]);
			const bool high_char = quote == '\'' && c >= 0x80;
			if (c >= 0x20 && c != 0x7f && c != '\\' && c != static_cast<unsigned char>(quote) && !high_char) { continue; }
			out.append(text.data() + run, i - run);
			run = i + 1;
			switch (c) {
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				case '\\': out += "\\\\"; break;
				default:
					if (c == static_cast<unsigned char>(quote)) {
						out += '\\';
						out += quote;
					} else {
						append_hex_escape(out, c);
					}
			}
		}
		out.append(text.data() + run, text.size() - run);
		out += quote;
	}

	void append_int(std::string& out, script_int v) {
		char digits[24];
		auto res = std::to_chars(digits, digits + sizeof(digits), v);
		out.append(digits, res.ptr);
	}

	// A scalar's text, quoted inside a container
	void append_scalar(std::string& out, const script_value& v, bool nested) {
		switch (v.raw_storage_index()) {
			case script_value::TYPEID_INT: append_int(out, v.unchecked_as_int()); return;
			case script_value::TYPEID_STRING:
				if (nested) { append_quoted(out, v.unchecked_as_string(), '"'); } else { out += v.unchecked_as_string(); }
				return;
			case script_value::TYPEID_UTF8_STRING:
				if (nested) { append_quoted(out, v.unchecked_as_utf8().bytes, '"'); } else { out += v.unchecked_as_utf8().bytes; }
				return;
			case script_value::TYPEID_CHAR: {
				const char c = v.unchecked_as_char();
				if (nested) { append_quoted(out, std::string_view(&c, 1), '\''); } else { out += c; }
				return;
			}
			case script_value::TYPEID_CPP_BOUND:
				if (nested && v.current_type() == script_value_type::jai_string_type) { append_quoted(out, v.as_string(), '"'); return; }
				if (nested && v.current_type() == script_value_type::jai_char_type) {
					const char c = v.as_char();
					append_quoted(out, std::string_view(&c, 1), '\'');
					return;
				}
				out += v.to_string();
				return;
			default: out += v.to_string(); return;
		}
	}

	enum class frame_shape : uint8_t { array, map, fields };

	struct frame {
		frame_shape shape = frame_shape::array;
		script_value held;                       // the container or object, when the walk holds it
		const script_value* in_place = nullptr;  // else where it lives
		size_t index = 0;
		bool first = true;
		bool value_next = false;                 // map: the key is written, its value comes next
		script_map::const_iterator next;
		const std::vector<uint64_t>* class_fields = nullptr;
		std::vector<uint64_t> own_fields;        // an instance with fields added at run time: its whole list
		const void* identity = nullptr;          // fields: the instance on the path

		explicit frame(engine* eng) : held(std::monostate{}, eng) {}

		const script_value& source() const { return in_place ? *in_place : held; }
		const std::vector<uint64_t>& fields() const { return class_fields ? *class_fields : own_fields; }
	};

	class walker {
	public:
		walker(const own_text_hook& hook, size_t limit, engine* eng) : hook_(hook), limit_(limit), eng_(eng), scratch_(std::monostate{}, eng) {}

		checked_result<std::string> run(const script_value& root, bool nested_root = false) {
			auto entered = enter(root, nullptr, nested_root);
			if (!entered) { return entered.error_value(); }
			while (!frames_.empty() && !stopped_) {
				if (out_.size() >= limit_) {
					out_ += "...";
					break;
				}
				const script_value* child = next_child(frames_.back());
				if (stopped_) { break; }
				if (!child) {
					close(frames_.back());
					frames_.pop_back();
					continue;
				}
				auto step = enter(*child, child == &scratch_ ? &scratch_ : nullptr, true);
				if (!step) { return step.error_value(); }
			}
			return std::move(out_);
		}

	private:
		const own_text_hook& hook_;
		size_t limit_;
		engine* eng_;
		std::string out_;
		std::vector<frame> frames_;
		std::unordered_set<const void*> path_;
		std::unordered_map<const class_definition*, std::vector<uint64_t>> class_fields_;
		script_value scratch_;   // a typed element boxed for its visit
		bool stopped_ = false;

		// v's frame: a temporary the walk made (temp, when v is it) moves in, a pinned walk or a value reached through a
		// temporary reference holds a copy, else the frame reads v where it lives
		frame& open(frame_shape shape, const script_value& v, script_value* temp) {
			frame& f = frames_.emplace_back(eng_);
			f.shape = shape;
			if (temp && temp == &v) {
				f.held = std::move(*temp);
			} else if (temp || hook_.pin) {
				f.held = v;
			} else {
				f.in_place = &v;
			}
			return f;
		}

		checked_result<void> enter(const script_value& given, script_value* temp, bool nested) {
			const script_value& v = given.deref();
			switch (v.raw_storage_index()) {
				case script_value::TYPEID_ARRAY:
					if (v.unchecked_array_node()->size() == 0) {
						out_ += "[]";
					} else {
						out_ += '[';
						open(frame_shape::array, v, temp);
					}
					return {};
				case script_value::TYPEID_MAP:
					if (v.unchecked_as_map().size() == 0) {
						out_ += "{}";
					} else {
						out_ += '{';
						frame& f = open(frame_shape::map, v, temp);
						f.next = f.source().unchecked_as_map().begin();
					}
					return {};
				case script_value::TYPEID_OBJECT:
					return enter_object(v, temp, nested);
				case script_value::TYPEID_WEAK_PTR: {
					const auto shared = v.get_weak_ptr().lock();
					if (!shared) {
						out_ += "null";
						return {};
					}
					script_value target = script_value::make_object(shared->type_name(), shared->type_id(), shared->held(), shared->owner());
					return enter_object(target, &target, nested);
				}
				case script_value::TYPEID_FUNCTION:
					out_ += "<function>";
					return {};
				case script_value::TYPEID_PARALLEL_BORROW:
					if (v.parallel_object_borrow()) {   // a shared object shows as the worker's own copy of it
						script_value copy = parallel_by_value(v);
						return enter_object(copy, &copy, nested);
					}
					append_scalar(out_, v, nested);
					return {};
				default:
					append_scalar(out_, v, nested);
					return {};
			}
		}

		checked_result<void> enter_object(const script_value& v, script_value* temp, bool nested) {
			const script_value::object_holder* holder = v.peek_object_holder();
			if (!holder) {
				out_ += "null";
				return {};
			}
			if (holder->is_coroutine_handle()) {
				out_ += "<coroutine>";
				return {};
			}
			script_value kept(std::monostate{}, eng_);
			const script_value* object = &v;
			if (hook_.speak) {
				if (hook_.pin && temp != &v) {
					kept = v;   // its to_string() may drop the caller's hold on it
					object = &kept;
					temp = &kept;
				}
				auto verdict = hook_.speak(hook_.backend, *object, out_);
				if (!verdict) { return verdict.error_value(); }
				if (verdict.value() == own_text_verdict::spoke) { return {}; }
				if (verdict.value() == own_text_verdict::unwinding) {
					stopped_ = true;
					return {};
				}
			}
			if (!holder->is_instance()) {
				append_opaque_name(holder->type_name().empty() ? std::string_view("object") : holder->type_name());
				return {};
			}
			const auto* instance = holder->unchecked_instance();
			const class_definition* def = instance->get_class_definition();
			if (def && def->is_transparent_wrapper()) {
				script_value wrapper = *object;
				script_value inner = def->unwrap(wrapper);
				if (!inner.is_null() && inner.peek_object_holder() != holder) {
					return enter(inner, &inner, nested);
				}
			}
			const std::string_view name = def ? std::string_view(def->get_name()) : instance->get_class_name();
			if (def && def->is_cpp_class()) {
				append_opaque_name(name);
				return {};
			}
			if (path_.count(instance)) {
				out_ += "<cycle ";
				out_ += name;
				out_ += '>';
				return {};
			}
			out_ += name;
			const std::vector<uint64_t>* declared = def ? &declared_fields(def) : nullptr;
			std::vector<uint64_t> own;
			if (!declared || instance->has_user_overflow_fields()) {
				if (declared) { own = *declared; }
				append_runtime_fields(*instance, own);
				declared = nullptr;
			}
			if (declared ? declared->empty() : own.empty()) {
				out_ += "{}";
				return {};
			}
			out_ += '{';
			frame& f = open(frame_shape::fields, *object, temp);
			f.class_fields = declared;
			f.own_fields = std::move(own);
			f.identity = instance;
			path_.insert(instance);
			return {};
		}

		void append_opaque_name(std::string_view name) {
			out_ += '<';
			out_ += name;
			out_ += '>';
		}

		// The class's script fields in declaration order, every base's before its own (a C++ base's state is not shown)
		const std::vector<uint64_t>& declared_fields(const class_definition* def) {
			auto [it, fresh] = class_fields_.try_emplace(def);
			std::vector<uint64_t>& ids = it->second;
			if (!fresh) { return ids; }
			std::unordered_set<uint64_t> seen;
			struct level { const class_definition* def; size_t parent; };
			std::vector<level> work{ { def, 0 } };
			while (!work.empty()) {
				const class_definition* current = work.back().def;
				const auto& parents = current->get_parent_classes();
				if (work.back().parent < parents.size()) {
					const class_definition* parent = parents[work.back().parent++].get();
					if (parent && !parent->is_cpp_class()) { work.push_back({ parent, 0 }); }
					continue;
				}
				current->for_each_own_field_id([&](uint64_t id) {
					if (seen.insert(id).second) { ids.push_back(id); }
				});
				work.pop_back();
			}
			return ids;
		}

		// Fields added at run time follow the declared ones, by name
		void append_runtime_fields(const class_instance& instance, std::vector<uint64_t>& ids) {
			std::unordered_set<uint64_t> seen(ids.begin(), ids.end());
			std::vector<uint64_t> added;
			instance.for_each_field([&](uint64_t id, const script_value&) {
				if (seen.insert(id).second) { added.push_back(id); }
			});
			if (string_symbolizer* names = symbolizer_of(instance)) {
				std::sort(added.begin(), added.end(), [names](uint64_t a, uint64_t b) { return names->get_string(a) < names->get_string(b); });
			}
			ids.insert(ids.end(), added.begin(), added.end());
		}

		string_symbolizer* symbolizer_of(const class_instance& instance) const {
			engine* eng = instance.get_engine() ? instance.get_engine() : eng_;
			return eng ? engine_access::get_symbolizer(*eng) : nullptr;
		}

		void stop_at_limit() {
			out_ += "...";
			stopped_ = true;
		}

		// The frame's next child to enter (what precedes it written), null when the frame is done; a typed scalar element
		// is written here, any other typed element is boxed in scratch_
		const script_value* next_child(frame& f) {
			switch (f.shape) {
				case frame_shape::array: {
					const script_array* node = f.source().unchecked_array_node();
					while (f.index < node->size()) {
						if (out_.size() >= limit_) {
							stop_at_limit();
							return nullptr;
						}
						if (!f.first) { out_ += ", "; }
						f.first = false;
						const size_t i = f.index++;
						switch (node->kind()) {
							case script_array::kind_t::hetero: return &node->values()[i];
							case script_array::kind_t::i64: append_int(out_, node->ints()[i]); break;
							case script_array::kind_t::f64: out_ += script_value::float_text(node->floats()[i]); break;
							case script_array::kind_t::str:
								if (const auto& text = node->strs()[i]) { append_quoted(out_, *text, '"'); } else { out_ += "\"\""; }
								break;
							default:
								scratch_ = node->get(i, node->owner);
								return &scratch_;
						}
					}
					return nullptr;
				}
				case frame_shape::map: {
					if (f.value_next) {
						out_ += ": ";
						f.value_next = false;
						const script_value* value = &f.next->second;
						++f.next;
						return value;
					}
					if (f.next == f.source().unchecked_as_map().end()) { return nullptr; }
					if (!f.first) { out_ += ", "; }
					f.first = false;
					f.value_next = true;
					return &f.next->first;
				}
				case frame_shape::fields: {
					const auto* instance = static_cast<const class_instance*>(f.source().peek_object_holder()->object());
					const std::vector<uint64_t>& ids = f.fields();
					while (f.index < ids.size()) {
						const uint64_t id = ids[f.index++];
						const script_value* field = instance->find_script_field_value(id);
						if (!field) { continue; }
						if (!f.first) { out_ += ", "; }
						f.first = false;
						if (string_symbolizer* names = symbolizer_of(*instance)) { out_ += names->get_string(id); }
						out_ += ": ";
						return field;
					}
					return nullptr;
				}
			}
			return nullptr;
		}

		void close(frame& f) {
			switch (f.shape) {
				case frame_shape::array: out_ += ']'; break;
				case frame_shape::map: out_ += '}'; break;
				case frame_shape::fields:
					out_ += '}';
					path_.erase(f.identity);
					break;
			}
		}
	};

} // namespace value_text_walk

checked_result<std::string> value_text(const script_value& v, const own_text_hook& hook, size_t limit) {
	value_text_walk::walker walk(hook, limit, v.payload_engine());
	return walk.run(v);
}

JAI_NOINLINE error_propagator map_key_missing_error(string_symbolizer* sym, const type_info* map_tag, const script_value& key) {
	const script_value& k = key.deref();
	if (map_tag && map_tag->base_type == script_value_type::jai_map_type) {
		if (type_info_ptr key_type = map_tag->key_type(); key_type && key_type->base_type != script_value_type::jai_any_type && k.type() != key_type->base_type) {
			if (!map_key_kind_admits(k.type(), key_type->base_type)) {
				return element_type_error(sym, runtime_error_code::map_key_type_mismatch, "Cannot use '{0}' as a key of type '{1}'", k, key_type);
			}
			if (auto in_kind = map_key_in_kind(k, key_type->base_type, k.payload_engine()); !in_kind) { return in_kind.error_value(); }
		}
	}
	const auto code = make_error_code(runtime_error_code::map_key_not_found);
	value_text_walk::walker walk(own_text_hook{}, SIZE_MAX, k.payload_engine());
	auto text = walk.run(k, true);
	if (!text) { return error_propagator{code, map_key_not_found_slotless, 0, 0}; }
	if (sym->frozen()) {   // a parallel region: a key text the program already interned still names the key
		if (const auto id = sym->find(text.value())) { return error_propagator{code, map_key_not_found_text, *id, 0}; }
		return error_propagator{code, map_key_not_found_slotless, 0, 0};
	}
	return error_propagator{code, map_key_not_found_text, sym->intern(text.value()), 0};
}

} // namespace jai::detail
