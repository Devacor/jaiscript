// HTML templates (docs/site/guide/19-templates.html, detail/template_library.hpp): the web::html value, the splice and join
// builtins a template body desugars to, template(...)'s run-time path, and scoped_template's evaluator. The evaluator walks
// the parsed body itself: every operation it runs is listed here, so an untrusted template reaches no global, no function,
// no host binding and no script code.

#include <jaiscript/detail/template_library.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/execution_backend.hpp>
#include <jaiscript/core/runtime_errors.hpp>
#include <jaiscript/detail/engine_access.hpp>
#include <jaiscript/detail/ast.hpp>
#include <jaiscript/detail/lexer.hpp>
#include <jaiscript/detail/parser.hpp>
#include <jaiscript/detail/string_symbolizer.hpp>
#include <jaiscript/detail/format_spec.hpp>
#include <jaiscript/detail/value_text.hpp>
#include <jaiscript/detail/truthiness.hpp>
#include <jaiscript/detail/builtin_methods.hpp>
#include <jaiscript/detail/scalar_ops.hpp>
#include <jaiscript/detail/char_promotion.hpp>
#include <jaiscript/detail/string_join.hpp>
#include <jaiscript/detail/utf8_operands.hpp>
#include <jaiscript/detail/math_intrinsics.hpp>
#include <jaiscript/detail/math_values.hpp>
#include <jaiscript/detail/structural_equality.hpp>
#include <jaiscript/detail/native_stack.hpp>
#include <jaiscript/detail/execution_limits.hpp>
#include <jaiscript/detail/web_kernels.hpp>
#include <jaiscript/detail/class_runtime.hpp>
#include <jaiscript/detail/in_place_result.hpp>
#include <jaiscript/core/script_namespace.hpp>
#include <jaiscript/detail/environment.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_map>

namespace jai::detail::template_library {

namespace {

	using R = checked_result<script_value>;

	// web::html: text a template made, spliced as it is
	struct html_text {
		std::string text;
		std::string to_string() const { return text; }
	};

	struct scoped_program {
		std::shared_ptr<std::vector<token>> tokens;   // the body's identifiers view the symbolizer; the tokens own nothing the tree needs
		expression_ptr body;
	};

	struct scoped_file_entry {
		std::string content;
		bool loaded = false;
		uint64_t mtime = 0, size = 0;
		int64_t next_check = 0;
		bool failed = false;
		uint64_t failed_mtime = 0, failed_size = 0;
	};

	struct dynamic_entry {
		std::optional<script_value> function;
		std::string unit;
		std::string source;
		int64_t next_check = 0;
		bool failed = false;
		uint64_t failed_mtime = 0, failed_size = 0;
	};

	struct library {
		engine* eng = nullptr;
		uint64_t html_type = 0;
		const class_definition* html_class = nullptr;
		uint64_t joiner = 0, splice = 0, raw = 0, format_value = 0, args = 0;
		std::unordered_map<std::string, std::shared_ptr<const scoped_program>> scoped_programs;
		std::unordered_map<std::string, scoped_file_entry> scoped_files;
		std::unordered_map<std::string, dynamic_entry> dynamic_templates;
		std::unique_ptr<builtin_method_registries> methods;

		const html_text* as_html(const script_value& a_value) const {
			const script_value& v = a_value.deref();
			if (v.raw_storage_index() != script_value::TYPEID_OBJECT || v.is_shared_ptr_type()) { return nullptr; }
			auto instance = const_cast<script_value&>(v).get_class_instance();
			if (!instance || instance->get_class_definition() != html_class) { return nullptr; }
			return extract_cpp_object_ptr<html_text>(v, html_type);
		}
		script_value make_html(std::string a_text) const {
			return value_converter<html_text>::to(html_text{std::move(a_text)}, eng);
		}
		R refused(std::string_view a_text) const {
			return R(make_error_code(runtime_error_code::invalid_operation), "{0}", engine_access::park_error_text(*eng, a_text));
		}
	};

	JAI_NOINLINE R refusal(const library& a_lib, std::string a_text) { return a_lib.refused(a_text); }

	// a map key reads with [], never a dot: the text the checker and both backends raise
	std::string map_member_text(std::string_view a_member) {
		return "map has no member '" + std::string(a_member) + "'; read the key as [\"" + std::string(a_member) + "\"]";
	}

	const char* kind_name(const script_value& v) {
		switch (v.raw_storage_index()) {
			case script_value::TYPEID_NULL: return "null";
			case script_value::TYPEID_INT: return "int";
			case script_value::TYPEID_FLOAT: return "float";
			case script_value::TYPEID_STRING: return "string";
			case script_value::TYPEID_UTF8_STRING: return "utf8_string";
			case script_value::TYPEID_CHAR: return "char";
			case script_value::TYPEID_BOOL: return "bool";
			case script_value::TYPEID_ARRAY: return "array";
			case script_value::TYPEID_MAP: return "map";
			case script_value::TYPEID_FUNCTION: return "function";
			default: return "object";
		}
	}

	bool stat_file(const std::string& a_path, uint64_t& a_mtime, uint64_t& a_size) {
		std::error_code ec;
		const auto when = std::filesystem::last_write_time(a_path, ec);
		if (ec) { return false; }
		const auto bytes = std::filesystem::file_size(a_path, ec);
		if (ec) { return false; }
		a_mtime = static_cast<uint64_t>(when.time_since_epoch().count());
		a_size = static_cast<uint64_t>(bytes);
		return true;
	}

	bool read_whole_file(const std::string& a_path, std::string& a_out) {
		std::ifstream file(a_path, std::ios::binary);
		if (!file) { return false; }
		a_out.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		return true;
	}

	// a lies inside dir: both canonical, dir's components a prefix of a's
	bool lies_inside(const std::filesystem::path& a_file, const std::filesystem::path& a_dir) {
		auto f = a_file.begin();
		for (auto d = a_dir.begin(); d != a_dir.end(); ++d, ++f) {
			if (d->empty()) { continue; }   // a trailing separator
			if (f == a_file.end() || *f != *d) { return false; }
		}
		return f != a_file.end();
	}

	// A path's re-check is due once the import window (engine::import_recheck_interval) has passed since the last
	bool check_due(engine& a_engine, int64_t& a_next) {
		const execution_limits& limits = engine_access::execution_limits(a_engine);
		const double interval = a_engine.import_recheck_interval();
		if (interval <= 0.0 || limits.sampled_now >= a_next) {
			a_next = limits.sampled_now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(interval)).count();
			return true;
		}
		return false;
	}

	// ------------------------------------------------------------------ splices

	// The display text a trusted splice shows: a spec formats as format_value does, anything else as to_string() shows it
	checked_result<std::string> trusted_text(const library& a_lib, const script_value& a_value, const std::string* a_spec) {
		if (!a_spec) { return a_lib.eng->text_of(a_value); }
		format_spec spec;
		if (!parse_format_spec(*a_spec, spec)) { return checked_result<std::string>(make_error_code(runtime_error_code::invalid_operation), "{0}", engine_access::park_error_text(*a_lib.eng, "unsupported format spec ':" + *a_spec + "'")); }
		if (!plain_text_value(a_value) && !format_spec_needs_number(spec)) {
			auto text = a_lib.eng->text_of(a_value);
			if (!text) { return text; }
			return format_value_with_spec(script_value(std::move(text.value()), a_lib.eng), spec, *a_spec);
		}
		return format_value_with_spec(a_value, spec, *a_spec);
	}

	// One splice appended to out: a web::html as it is (escaped too when `escape_html`), an array element by element, anything
	// else as its text, escaped
	JAI_NOINLINE checked_result<void> null_splice(const library& a_lib, const std::string& a_description) {
		return checked_result<void>(make_error_code(runtime_error_code::invalid_operation), "{0}", engine_access::park_error_text(*a_lib.eng, "a template splice is null: " + a_description));
	}

	template <class TextOf>
	checked_result<void> append_splice(const library& a_lib, const script_value& a_value, const std::string* a_spec, bool a_escape_html, std::string& a_out,
	                                   const std::string& a_description, TextOf&& a_text_of) {
		const script_value& v = a_value.deref();
		if (v.is_null()) { return null_splice(a_lib, a_description); }
		if (const html_text* h = a_lib.as_html(v)) {
			std::string text = h->text;
			if (a_spec) {
				format_spec spec;
				if (parse_format_spec(*a_spec, spec)) { text = format_value_with_spec(script_value(std::move(text), a_lib.eng), spec, *a_spec); }
			}
			if (a_escape_html) { stdlib::web_text_detail::append_html_escaped(text, a_out); } else { a_out += text; }
			return {};
		}
		if (v.raw_storage_index() == script_value::TYPEID_ARRAY) {
			for (const script_value& element : v.unchecked_array_node()->materialize_values(a_lib.eng)) {
				auto appended = append_splice(a_lib, element, a_spec, a_escape_html, a_out, a_description, a_text_of);
				if (!appended) { return appended; }
			}
			return {};
		}
		auto text = a_text_of(v, a_spec);
		if (!text) { return text.error_value(); }
		stdlib::web_text_detail::append_html_escaped(text.value(), a_out);
		return {};
	}

	// ------------------------------------------------------------------ the scoped rules

	// The ids the rules name
	struct library_view_ids {
		uint64_t joiner = 0, splice = 0, format_value = 0, args = 0, raw = 0;
		uint64_t string_ns = 0, array_ns = 0, map_ns = 0;
		explicit library_view_ids(string_symbolizer& a_symbols)
			: joiner(a_symbols.intern(k_joiner)), splice(a_symbols.intern(k_splice)), format_value(a_symbols.intern("format_value")),
			  args(a_symbols.intern("args")), raw(a_symbols.intern("raw")), string_ns(a_symbols.intern("string")), array_ns(a_symbols.intern("array")),
			  map_ns(a_symbols.intern("map")) {}
		// string::f, array::f, map::f: the engine's builtins over copies, the only names past args a scoped template reaches
		bool builtin_namespace(const member_expr* a_member) const {
			if (!a_member->is_static || !a_member->object || a_member->object->get_type() != node_type::identifier_expr) { return false; }
			const auto* base = static_cast<const identifier_expr*>(a_member->object.get());
			return base->global_qualified && (base->symbol_id == string_ns || base->symbol_id == array_ns || base->symbol_id == map_ns);
		}
	};

	bool internal_callee(const library_view_ids& a_ids, uint64_t a_id) {
		return a_id == a_ids.joiner || a_id == a_ids.splice || a_id == a_ids.format_value;
	}

	// A lambda's one-expression body (an arrow function's, or a block holding one return): null when the body is more
	const expression* lambda_value(const lambda_expr& a_lambda) {
		if (!a_lambda.body || a_lambda.body->get_type() != node_type::block_stmt) { return nullptr; }
		const auto& block = static_cast<const block_stmt&>(*a_lambda.body);
		if (block.declarations.size() != 1 || !block.declarations[0] || block.declarations[0]->get_type() != node_type::statement_decl) { return nullptr; }
		const auto& stmt = static_cast<const statement_decl&>(*block.declarations[0]).statement;
		if (!stmt || stmt->get_type() != node_type::return_stmt) { return nullptr; }
		return static_cast<const return_stmt&>(*stmt).value.get();
	}

	class rule_walk {
	public:
		rule_walk(string_symbolizer& a_symbols, std::vector<violation>& a_out) : ids_(a_symbols), out_(a_out) {}

		void walk(const expression* e) {
			if (!e) { return; }
			stack_.guard(e->location);
			switch (e->get_type()) {
				case node_type::literal_expr:
					return;
				case node_type::identifier_expr: {
					const auto* id = static_cast<const identifier_expr*>(e);
					if (id->global_qualified) {
						if (id->symbol_id == ids_.raw) { refuse(e, "raw() is not allowed in a scoped template: its output is always escaped"); }
						else { refuse(e, "a scoped template sees only args: '" + std::string(id->name) + "' is not available"); }
					}
					return;
				}
				case node_type::binary_expr: {
					const auto* b = static_cast<const binary_expr*>(e);
					walk(b->left.get());
					walk(b->right.get());
					return;
				}
				case node_type::unary_expr: {
					const auto* u = static_cast<const unary_expr*>(e);
					const token_type op = u->op.type;
					if (op != token_type::bang && op != token_type::minus && op != token_type::plus && op != token_type::tilde) {
						refuse(e, "a scoped template does not assign: '" + std::string(u->op.lexeme) + "' is not allowed");
						return;
					}
					walk(u->operand.get());
					return;
				}
				case node_type::ternary_expr: {
					const auto* t = static_cast<const ternary_expr*>(e);
					walk(t->condition.get());
					walk(t->then_expression.get());
					walk(t->else_expression.get());
					return;
				}
				case node_type::array_literal_expr:
					for (const auto& element : static_cast<const array_literal_expr*>(e)->elements) { walk(element.get()); }
					return;
				case node_type::map_literal_expr:
					for (const auto& [key, value] : static_cast<const map_literal_expr*>(e)->entries) { walk(key.get()); walk(value.get()); }
					return;
				case node_type::member_expr: {
					const auto* m = static_cast<const member_expr*>(e);
					if (m->is_static && ids_.builtin_namespace(m)) { return; }
					if (m->is_static) { refuse(e, "a scoped template sees only args: '::" + std::string(m->member) + "' is not available"); return; }
					if (m->object && m->object->get_type() == node_type::identifier_expr && args_shadowed_ == 0) {
						const auto* base = static_cast<const identifier_expr*>(m->object.get());
						if (!base->global_qualified && base->symbol_id == ids_.args) { refuse(e, map_member_text(m->member)); return; }
					}
					walk(m->object.get());
					return;
				}
				case node_type::call_expr: {
					const auto* c = static_cast<const call_expr*>(e);
					const expression* callee = c->callee.get();
					if (callee->get_type() == node_type::identifier_expr) {
						const auto* id = static_cast<const identifier_expr*>(callee);
						if (id->global_qualified && internal_callee(ids_, id->symbol_id)) {
							if (id->symbol_id == ids_.splice && c->arguments.size() == 3 && c->arguments[0]->get_type() == node_type::literal_expr &&
								static_cast<const literal_expr*>(c->arguments[0].get())->value.raw_storage_index() == script_value::TYPEID_NULL &&
								c->arguments[2]->get_type() == node_type::literal_expr) {
								refuse(c->arguments[0].get(), "a template splice is null: " + std::string(static_cast<const literal_expr*>(c->arguments[2].get())->value.unchecked_as_string()));
								return;
							}
							for (const auto& a : c->arguments) { walk(a.get()); }
							return;
						}
						if (id->symbol_id == ids_.raw && id->global_qualified) { refuse(e, "raw() is not allowed in a scoped template: its output is always escaped"); return; }
						if (id->global_qualified) {
							refuse(e, "a scoped template calls only built-in methods and the string::, array:: and map:: functions: '" + std::string(id->name) + "(...)' is not allowed");
							return;
						}
						refuse(e, "a scoped template does not call function values: '" + std::string(id->name) + "(...)' is not allowed");
						return;
					}
					if (callee->get_type() == node_type::member_expr && !static_cast<const member_expr*>(callee)->is_static) {
						walk(static_cast<const member_expr*>(callee)->object.get());
						for (const auto& a : c->arguments) { walk(a.get()); }
						return;
					}
					if (callee->get_type() == node_type::member_expr && ids_.builtin_namespace(static_cast<const member_expr*>(callee))) {
						for (const auto& a : c->arguments) { walk(a.get()); }
						return;
					}
					if (callee->get_type() == node_type::member_expr) {
						refuse(e, "a scoped template calls only built-in methods and the string::, array:: and map:: functions: '" + std::string(static_cast<const member_expr*>(callee)->member) + "(...)' is not allowed");
						return;
					}
					refuse(e, "a scoped template does not call function values");
					return;
				}
				case node_type::lambda_expr: {
					const auto* l = static_cast<const lambda_expr*>(e);
					for (const auto& p : l->parameters) {
						if (p.is_reference || p.default_value || (p.type && p.type->base_type != script_value_type::jai_any_type)) {
							refuse(e, "a scoped template's lambda takes plain parameter names");
							return;
						}
					}
					if (!l->captures.empty()) { refuse(e, "a scoped template's lambda takes plain parameter names"); return; }
					const expression* value = lambda_value(*l);
					if (!value) { refuse(e, "a scoped template's lambda is one expression (x => expr)"); return; }
					const bool shadows = std::any_of(l->parameters.begin(), l->parameters.end(), [](const parameter& p) { return p.name == "args"; });
					args_shadowed_ += shadows;
					walk(value);
					args_shadowed_ -= shadows;
					return;
				}
				default:
					refuse(e, std::string("a scoped template is one expression over args: ") + construct_name(e->get_type()) + " is not allowed");
					return;
			}
		}

	private:
		library_view_ids ids_;
		int args_shadowed_ = 0;   // inside a lambda with a parameter named args: that name is not the template's map
		std::vector<violation>& out_;
		native_stack_mark stack_;

		static const char* construct_name(node_type t) {
			switch (t) {
				case node_type::assignment_expr: return "an assignment";
				case node_type::new_expr: return "'new'";
				case node_type::this_expr: return "'this'";
				case node_type::super_expr: return "'super'";
				case node_type::throw_expr: return "'throw'";
				case node_type::yield_expr: return "'yield'";
				case node_type::include_expr: return "'include'";
				case node_type::import_expr: return "'import'";
				case node_type::cast_expr: return "a cast";
				default: return "this expression";
			}
		}
		void refuse(const expression* e, std::string a_message) { out_.push_back({e->location, std::move(a_message)}); }
	};

	// ------------------------------------------------------------------ the scoped evaluator

	struct sandbox_lambda;

	class scoped_render {
	public:
		scoped_render(library& a_lib, const std::shared_ptr<const scoped_program>& a_program)
			: lib_(a_lib), eng_(a_lib.eng), program_(a_program), limits_(engine_access::execution_limits(*a_lib.eng)),
			  ids_(*engine_access::get_symbolizer(*a_lib.eng)), limit_(limits_.scoped_template_output_limit) {}
		~scoped_render() { *alive_ = false; }
		scoped_render(const scoped_render&) = delete;
		scoped_render& operator=(const scoped_render&) = delete;
		const std::shared_ptr<bool>& alive() const { return alive_; }

		R render(script_value a_args) {
			data_has_objects_ = holds_object(a_args);
			names_.push_back({ids_.args, std::move(a_args)});
			try {
				return eval(program_->body.get());
			} catch (const nesting_exhausted&) {
				return lib_.refused(k_nesting_too_deep);
			} catch (const std::exception& e) {
				return lib_.refused(e.what());
			}
		}

		R call_lambda(const lambda_expr& a_lambda, const std::vector<std::pair<uint64_t, script_value>>& a_closure, const std::vector<script_value>& a_args) {
			auto saved = std::move(names_);
			names_ = a_closure;
			for (size_t i = 0; i < a_lambda.parameters.size(); ++i) {
				names_.push_back({a_lambda.parameters[i].symbol_id != UINT64_MAX ? a_lambda.parameters[i].symbol_id : symbols().intern(a_lambda.parameters[i].name),
				                  i < a_args.size() ? script_value(a_args[i].deref()) : script_value(std::monostate{}, eng_)});
			}
			R result = eval(lambda_value(a_lambda));
			names_ = std::move(saved);
			return result;
		}

		size_t output() const { return output_; }

	private:
		library& lib_;
		engine* eng_;
		std::shared_ptr<const scoped_program> program_;
		execution_limits& limits_;
		library_view_ids ids_;
		std::vector<std::pair<uint64_t, script_value>> names_;
		size_t output_ = 0;
		size_t limit_ = 0;   // the limit when the render started: a host change applies to the next render
		uint32_t ticks_ = 0;
		bool data_has_objects_ = false;
		native_stack_mark stack_;
		std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

		string_symbolizer& symbols() const { return *engine_access::get_symbolizer(*eng_); }
		script_value null_value() const { return script_value(std::monostate{}, eng_); }

		// Values that could run code through a built-in: class instances, host objects, handles, functions this render did not make
		static bool object_like(const script_value& v);
		static bool holds_object(const script_value& a_value) {
			std::vector<script_value> pending{a_value};
			while (!pending.empty()) {
				script_value v = pending.back().deref();
				pending.pop_back();
				if (object_like(v)) { return true; }
				if (v.raw_storage_index() == script_value::TYPEID_ARRAY) {
					for (const script_value& e : v.unchecked_array_node()->materialize_values(v.payload_engine())) { pending.push_back(e); }
				} else if (v.raw_storage_index() == script_value::TYPEID_MAP) {
					for (const auto& [k, e] : v.as_map()) { pending.push_back(k); pending.push_back(e); }
				}
			}
			return false;
		}

		checked_result<void> budget() {
			if (++ticks_ < 1024 && !limits_.memory_tripped()) [[likely]] { return {}; }
			ticks_ = 0;
			if (limits_.memory_tripped()) { return raise_memory_cap(limits_); }
			const auto now = std::chrono::steady_clock::now();
			limits_.note_clock_sample(now);
			if (limits_.step_budget_spent()) { limits_.terminal_error = true; return raise_execution_budget(limits_); }
			if (execution_backend* backend = engine_access::get_execution_backend(*eng_)) {
				const auto deadline = backend->execution_deadline();
				if (deadline != std::chrono::steady_clock::time_point{} && now >= deadline) {
					limits_.terminal_error = true;
					return raise_execution_budget(limits_);
				}
			}
			return {};
		}

		R refuse_object(std::string_view a_what) const {
			return refusal(lib_, "a scoped template does not " + std::string(a_what) + " a class instance, a host object or a function: its code could run");
		}

		R literal(const literal_expr* a_literal) const {
			const auto& storage = a_literal->value.get_storage();
			switch (storage.index()) {
				case 1: return script_value(storage.get<script_int>(), eng_);
				case 2: return script_value(storage.get<script_float>(), eng_);
				case 3: return script_value(std::string(a_literal->value.unchecked_as_string()), eng_);
				case 4: return script_value(storage.get<script_char>(), eng_);
				case 5: return script_value(storage.get<script_bool>(), eng_);
				default: return null_value();
			}
		}

		R eval(const expression* e);
		R eval_call(const call_expr* c);
		R eval_member(const member_expr* m);
		R eval_index(const script_value& a_container, const script_value& a_key);
		R eval_binary(const binary_expr* b);
		R eval_unary(const unary_expr* u);
		R join(const call_expr* c);
		R output_limit() const;
		R splice(const call_expr* c);
		R arithmetic(token_type a_op, const script_value& a_left, const script_value& a_right, bool a_joins_text);
		R compare(token_type a_op, const script_value& a_left, const script_value& a_right);
		R method(const script_value& a_receiver, const member_expr* a_callee, std::vector<script_value> a_args);
		R namespace_call(const call_expr* c, const member_expr* a_callee);
		const script_value* builtin_namespace_value(const member_expr* a_member) const;
		checked_result<std::string> plain_text(const script_value& v, const std::string* a_spec);
		bool counts(size_t a_bytes) {
			output_ += a_bytes;
			return limit_ == 0 || output_ <= limit_;
		}
	};

	// A scoped template's lambda: a host function only its render runs, holding the names it closed over
	struct sandbox_lambda {
		scoped_render* render = nullptr;
		std::shared_ptr<bool> alive;
		std::shared_ptr<const scoped_program> program;
		const lambda_expr* lambda = nullptr;
		std::shared_ptr<std::vector<std::pair<uint64_t, script_value>>> closure;
		R operator()(const std::vector<script_value>& a_args) const;
	};

	R sandbox_lambda::operator()(const std::vector<script_value>& a_args) const {
		if (!*alive) { return R(make_error_code(runtime_error_code::invalid_operation), "a scoped template's lambda runs only inside its render"); }
		return render->call_lambda(*lambda, *closure, a_args);
	}

	bool scoped_render::object_like(const script_value& v) {
		switch (v.raw_storage_index()) {
			case script_value::TYPEID_OBJECT:
			case script_value::TYPEID_WEAK_PTR:
			case script_value::TYPEID_CPP_BOUND:
			case script_value::TYPEID_PARALLEL_BORROW:
				return true;
			case script_value::TYPEID_FUNCTION:
				return v.unchecked_as_function().target<sandbox_lambda>() == nullptr;
			default:
				return false;
		}
	}

	checked_result<std::string> scoped_render::plain_text(const script_value& v, const std::string* a_spec) {
		if (holds_object(v)) {
			return checked_result<std::string>(make_error_code(runtime_error_code::invalid_operation), "{0}",
				engine_access::park_error_text(*eng_, "a scoped template does not splice a class instance, a host object or a function: its code could run"));
		}
		if (a_spec) {
			format_spec spec;
			if (!parse_format_spec(*a_spec, spec)) {
				return checked_result<std::string>(make_error_code(runtime_error_code::invalid_operation), "{0}", engine_access::park_error_text(*eng_, "unsupported format spec ':" + *a_spec + "'"));
			}
			if (!plain_text_value(v) && !format_spec_needs_number(spec)) {
				auto text = value_text(v, own_text_hook{});
				if (!text) { return text; }
				return format_value_with_spec(script_value(std::move(text.value()), eng_), spec, *a_spec);
			}
			return format_value_with_spec(v, spec, *a_spec);
		}
		if (plain_text_value(v)) { return v.to_string(); }
		return value_text(v, own_text_hook{});
	}

	R scoped_render::eval(const expression* e) {
		stack_.guard(e->location);
		if (auto spent = budget(); !spent) { return spent.error_value(); }
		switch (e->get_type()) {
			case node_type::literal_expr: return literal(static_cast<const literal_expr*>(e));
			case node_type::identifier_expr: {
				const auto* id = static_cast<const identifier_expr*>(e);
				for (size_t i = names_.size(); i-- > 0;) {
					if (names_[i].first == id->symbol_id) { return names_[i].second; }
				}
				return refusal(lib_, "a scoped template sees only args: '" + std::string(id->name) + "' is not available");
			}
			case node_type::binary_expr: return eval_binary(static_cast<const binary_expr*>(e));
			case node_type::unary_expr: return eval_unary(static_cast<const unary_expr*>(e));
			case node_type::ternary_expr: {
				const auto* t = static_cast<const ternary_expr*>(e);
				auto condition = eval(t->condition.get());
				if (!condition) { return condition; }
				if (object_like(condition.value().deref())) { return refuse_object("test"); }
				const bool holds = truthy_of(condition.value(), [](const script_value&) { return true; });
				return eval(holds ? t->then_expression.get() : t->else_expression.get());
			}
			case node_type::array_literal_expr: {
				script_value result = script_value::make_array(nullptr, eng_);
				for (const auto& element : static_cast<const array_literal_expr*>(e)->elements) {
					auto v = eval(element.get());
					if (!v) { return v; }
					result.unchecked_get_array_storage()->push(v.value().deref().clone_element());
				}
				return result;
			}
			case node_type::map_literal_expr: {
				script_value result = script_value::make_map(nullptr, nullptr, eng_);
				for (const auto& [key_expr, value_expr] : static_cast<const map_literal_expr*>(e)->entries) {
					auto key = eval(key_expr.get());
					if (!key) { return key; }
					auto value = eval(value_expr.get());
					if (!value) { return value; }
					if (object_like(key.value().deref())) { return refuse_object("key a map by"); }
					result.as_map()[key.value().deref()] = value.value().deref().clone_element();
				}
				return result;
			}
			case node_type::member_expr: return eval_member(static_cast<const member_expr*>(e));
			case node_type::call_expr: return eval_call(static_cast<const call_expr*>(e));
			case node_type::lambda_expr: {
				sandbox_lambda fn;
				fn.render = this;
				fn.alive = alive_;
				fn.program = program_;
				fn.lambda = static_cast<const lambda_expr*>(e);
				fn.closure = std::make_shared<std::vector<std::pair<uint64_t, script_value>>>(names_);
				return script_value::make_function(script_function(std::move(fn)), eng_);
			}
			default:
				return refusal(lib_, "a scoped template is one expression over args");
		}
	}

	R scoped_render::eval_unary(const unary_expr* u) {
		auto operand = eval(u->operand.get());
		if (!operand) { return operand; }
		const script_value v = operand.value().deref();
		if (object_like(v)) { return refuse_object("apply an operator to"); }
		switch (u->op.type) {
			case token_type::bang: return script_value(!truthy_of(v, [](const script_value&) { return true; }), eng_);
			case token_type::plus: return v;
			case token_type::minus:
				if (v.raw_storage_index() == script_value::TYPEID_INT) {
					script_int out;
					if (!ints::try_sub(script_int(0), v.unchecked_as_int(), out)) { return R(make_error_code(runtime_error_code::invalid_numeric_operand), math::negate_overflow_text); }
					return script_value(out, eng_);
				}
				if (v.raw_storage_index() == script_value::TYPEID_FLOAT) { return script_value(-v.unchecked_as_float(), eng_); }
				return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operand for unary - operator");
			default:
				if (v.raw_storage_index() == script_value::TYPEID_INT) { return script_value(~v.unchecked_as_int(), eng_); }
				return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operand for ~ operator");
		}
	}

	R scoped_render::eval_binary(const binary_expr* b) {
		const token_type op = b->op.type;
		auto left = eval(b->left.get());
		if (!left) { return left; }
		if (op == token_type::ampersand_ampersand || op == token_type::pipe_pipe) {
			const script_value l = left.value().deref();
			if (object_like(l)) { return refuse_object("test"); }
			const bool lt = truthy_of(l, [](const script_value&) { return true; });
			if (op == token_type::ampersand_ampersand ? !lt : lt) { return script_value(lt, eng_); }
			auto right = eval(b->right.get());
			if (!right) { return right; }
			const script_value r = right.value().deref();
			if (object_like(r)) { return refuse_object("test"); }
			return script_value(truthy_of(r, [](const script_value&) { return true; }), eng_);
		}
		if (op == token_type::question_question) {
			if (!left.value().deref().is_null()) { return left; }
			return eval(b->right.get());
		}
		auto right = eval(b->right.get());
		if (!right) { return right; }
		if (op == token_type::left_bracket) { return eval_index(left.value().deref(), right.value().deref()); }
		const script_value l = left.value().deref(), r = right.value().deref();
		if (holds_object(l) || holds_object(r)) { return refuse_object("apply an operator to"); }
		switch (op) {
			case token_type::plus: case token_type::minus: case token_type::star: case token_type::slash: case token_type::percent:
				return arithmetic(op, l, r, b->op.joins_text);
			case token_type::less: case token_type::less_equal: case token_type::greater: case token_type::greater_equal:
			case token_type::equal_equal: case token_type::bang_equal:
				return compare(op, l, r);
			default:
				return refusal(lib_, "a scoped template does not apply '" + std::string(b->op.lexeme) + "'");
		}
	}

	R scoped_render::arithmetic(token_type a_op, const script_value& a_left, const script_value& a_right, bool a_joins_text) {
		script_value l = utf8_operands(a_left.raw_storage_index(), a_right.raw_storage_index()) ? utf8_unwrapped(a_left, eng_) : a_left;
		script_value r = utf8_operands(a_left.raw_storage_index(), a_right.raw_storage_index()) ? utf8_unwrapped(a_right, eng_) : a_right;
		if (char_operands_promote(l.raw_storage_index(), r.raw_storage_index())) { l = char_promoted(l, eng_); r = char_promoted(r, eng_); }
		const size_t li = l.raw_storage_index(), ri = r.raw_storage_index();
		if (li == script_value::TYPEID_INT && ri == script_value::TYPEID_INT) {
			script_int out;
			if (const auto fault = int_arith(a_op, l.unchecked_as_int(), r.unchecked_as_int(), out); fault != scalar_fault::none) { return scalar_fault_error<R>(a_op, fault); }
			return script_value(out, eng_);
		}
		const bool numeric = (li == script_value::TYPEID_INT || li == script_value::TYPEID_FLOAT) && (ri == script_value::TYPEID_INT || ri == script_value::TYPEID_FLOAT);
		if (numeric) {
			script_float out;
			const script_float lf = li == script_value::TYPEID_INT ? script_float(l.unchecked_as_int()) : l.unchecked_as_float();
			const script_float rf = ri == script_value::TYPEID_INT ? script_float(r.unchecked_as_int()) : r.unchecked_as_float();
			if (const auto fault = float_arith(a_op, lf, rf, out); fault != scalar_fault::none) { return scalar_fault_error<R>(a_op, fault); }
			return script_value(out, eng_);
		}
		if (a_op == token_type::plus && (li == script_value::TYPEID_STRING || ri == script_value::TYPEID_STRING)) {
			if (!a_joins_text && string_beside_scalar(li, ri)) { return string_plus_refused(li, ri); }
			auto lt = plain_text(l, nullptr);
			if (!lt) { return lt.error_value(); }
			auto rt = plain_text(r, nullptr);
			if (!rt) { return rt.error_value(); }
			std::string joined = std::move(lt.value());
			joined += rt.value();
			if (!limits_.memory_charge(sizeof(script_value) + joined.size())) [[unlikely]] { return raise_memory_cap(limits_); }
			script_value result(std::move(joined), eng_);
			return utf8_operands(a_left.raw_storage_index(), a_right.raw_storage_index()) ? utf8_rewrapped(R(std::move(result)), eng_) : R(std::move(result));
		}
		switch (a_op) {
			case token_type::plus: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for + operator");
			case token_type::minus: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for - operator");
			case token_type::star: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for * operator");
			case token_type::slash: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for / operator");
			default: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for % operator");
		}
	}

	R scoped_render::compare(token_type a_op, const script_value& a_left, const script_value& a_right) {
		script_value l = utf8_operands(a_left.raw_storage_index(), a_right.raw_storage_index()) ? utf8_unwrapped(a_left, eng_) : a_left;
		script_value r = utf8_operands(a_left.raw_storage_index(), a_right.raw_storage_index()) ? utf8_unwrapped(a_right, eng_) : a_right;
		if (char_operands_promote(l.raw_storage_index(), r.raw_storage_index())) { l = char_promoted(l, eng_); r = char_promoted(r, eng_); }
		if (char_beside_string(l.raw_storage_index(), r.raw_storage_index())) { l = char_as_text(l, eng_); r = char_as_text(r, eng_); }
		const size_t li = l.raw_storage_index(), ri = r.raw_storage_index();
		const bool numeric = (li == script_value::TYPEID_INT || li == script_value::TYPEID_FLOAT) && (ri == script_value::TYPEID_INT || ri == script_value::TYPEID_FLOAT);
		if (a_op == token_type::equal_equal || a_op == token_type::bang_equal) {
			bool equal = false;
			if (li == script_value::TYPEID_INT && ri == script_value::TYPEID_INT) { equal = l.unchecked_as_int() == r.unchecked_as_int(); }
			else if (numeric) { equal = (li == script_value::TYPEID_INT ? script_float(l.unchecked_as_int()) : l.unchecked_as_float()) == (ri == script_value::TYPEID_INT ? script_float(r.unchecked_as_int()) : r.unchecked_as_float()); }
			else { equal = l == r; }
			return script_value(a_op == token_type::equal_equal ? equal : !equal, eng_);
		}
		auto ordered = [&](auto a, auto b) { return scalar_compare(a_op, a, b); };
		if (li == script_value::TYPEID_INT && ri == script_value::TYPEID_INT) { return script_value(ordered(l.unchecked_as_int(), r.unchecked_as_int()), eng_); }
		if (numeric) {
			return script_value(ordered(li == script_value::TYPEID_INT ? script_float(l.unchecked_as_int()) : l.unchecked_as_float(),
			                            ri == script_value::TYPEID_INT ? script_float(r.unchecked_as_int()) : r.unchecked_as_float()), eng_);
		}
		if (li == script_value::TYPEID_STRING && ri == script_value::TYPEID_STRING) { return script_value(ordered(l.unchecked_as_string(), r.unchecked_as_string()), eng_); }
		if (li == script_value::TYPEID_BOOL && ri == script_value::TYPEID_BOOL) { return script_value(ordered(l.unchecked_as_bool(), r.unchecked_as_bool()), eng_); }
		switch (a_op) {
			case token_type::less: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for < operator");
			case token_type::less_equal: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for <= operator");
			case token_type::greater: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for > operator");
			default: return R(make_error_code(runtime_error_code::type_mismatch), "Invalid operands for >= operator");
		}
	}

	R scoped_render::eval_index(const script_value& a_container, const script_value& a_key) {
		if (object_like(a_container)) { return refuse_object("index"); }
		if (object_like(a_key)) { return refuse_object("index by"); }
		switch (a_container.raw_storage_index()) {
			case script_value::TYPEID_ARRAY: {
				if (a_key.raw_storage_index() != script_value::TYPEID_INT) { return R(make_error_code(runtime_error_code::invalid_index_type), "Array index must be an integer"); }
				const script_array* node = a_container.unchecked_array_node();
				const script_int i = a_key.unchecked_as_int();
				if (i < 0 || static_cast<uint64_t>(i) >= node->size()) {
					return refusal(lib_, "Array index " + std::to_string(i) + " out of bounds for array of size " + std::to_string(node->size()));
				}
				return node->get(static_cast<size_t>(i), eng_);
			}
			case script_value::TYPEID_MAP: {
				const auto& map = a_container.as_map();
				auto found = map.find(a_key);
				if (found == map.end()) { return map_key_missing_error(&symbols(), a_container.type_info_raw(), a_key); }
				return script_value(found->second.deref());
			}
			case script_value::TYPEID_STRING: {
				if (a_key.raw_storage_index() != script_value::TYPEID_INT) { return R(make_error_code(runtime_error_code::invalid_index_type), "String index must be an integer"); }
				const std::string& text = a_container.unchecked_as_string();
				const script_int i = a_key.unchecked_as_int();
				if (i < 0 || static_cast<uint64_t>(i) >= text.size()) {
					return refusal(lib_, "String index " + std::to_string(i) + " out of bounds for string of length " + std::to_string(text.size()));
				}
				return script_value(script_char(text[static_cast<size_t>(i)]), eng_);
			}
			default:
				return refusal(lib_, "a scoped template indexes arrays, maps and strings");
		}
	}

	R scoped_render::eval_member(const member_expr* m) {
		if (m->is_static) {
			const script_value* held = builtin_namespace_value(m);
			if (!held || held->is_function()) { return refusal(lib_, "a scoped template sees only args: '::" + std::string(m->member) + "' is not available"); }
			return script_value(held->deref());
		}
		auto object = eval(m->object.get());
		if (!object) { return object; }
		script_value v = object.value().deref();
		if (m->null_safe && v.is_null()) { return null_value(); }
		const std::string_view name = m->member;
		if (v.raw_storage_index() == script_value::TYPEID_MAP) {
			const auto& map = v.as_map();
			auto found = map.find(script_value(std::string(name), eng_));
			return refusal(lib_, map_member_text(name));
		}
		if (v.is_math()) {
			const int i = math::component_of(math::kind_of(v), name);
			if (i >= 0) { return math::component_value(v, static_cast<size_t>(i), eng_); }
			return refusal(lib_, std::string(math_type_name(v.type())) + " has no member '" + std::string(name) + "'");
		}
		if (v.raw_storage_index() == script_value::TYPEID_OBJECT) {
			auto instance = v.get_class_instance();
			const uint64_t id = m->member_id != UINT64_MAX ? m->member_id : symbols().intern(name);
			if (instance && !instance->has_cpp_object() && !v.is_shared_ptr_type()) {
				const class_definition* def = instance->get_class_definition();
				if (def && def->is_script_class() && !def->user_accessor(symbols().intern("_get_" + std::string(name)))) {
					if (const script_value* field = instance->find_field_value(id)) { return script_value(field->deref()); }
				}
			}
			return refusal(lib_, "a scoped template reads the plain fields of a script class instance: '" + std::string(name) + "' is not one");
		}
		return refusal(lib_, std::string(kind_name(v)) + " has no member '" + std::string(name) + "'");
	}

	// A splice counts the bytes it writes and a body its own text, so a nested template's text counts each time it is spliced:
	// the limit bounds the work a render does
	R scoped_render::join(const call_expr* c) {
		std::string out;
		for (const auto& part : c->arguments) {
			auto v = eval(part.get());
			if (!v) { return v; }
			const script_value& text = v.value().deref();
			if (text.raw_storage_index() != script_value::TYPEID_STRING) { return refusal(lib_, "a template part is not text"); }
			if (part->get_type() == node_type::literal_expr && !counts(text.unchecked_as_string().size())) { return output_limit(); }
			out += text.unchecked_as_string();
		}
		return lib_.make_html(std::move(out));
	}

	JAI_NOINLINE R scoped_render::output_limit() const {
		return refusal(lib_, "a scoped template's output passed its " + std::to_string(limit_) + "-byte limit: the host raises it with engine::scoped_template_output_limit");
	}

	JAI_NOINLINE R trusted_output_limit(const library& a_lib, size_t a_limit) {
		return refusal(a_lib, "template()'s output passed its " + std::to_string(a_limit) + "-byte limit: the host raises it with engine::template_output_limit");
	}

	R scoped_render::splice(const call_expr* c) {
		auto v = eval(c->arguments[0].get());
		if (!v) { return v; }
		std::string spec_text, description;
		const std::string* spec = nullptr;
		if (c->arguments.size() > 1 && c->arguments[1]->get_type() == node_type::literal_expr) {
			spec_text = static_cast<const literal_expr*>(c->arguments[1].get())->value.unchecked_as_string();
			if (!spec_text.empty()) { spec = &spec_text; }
		}
		if (c->arguments.size() > 2 && c->arguments[2]->get_type() == node_type::literal_expr) {
			description = static_cast<const literal_expr*>(c->arguments[2].get())->value.unchecked_as_string();
		}
		std::string out;
		auto appended = append_splice(lib_, v.value(), spec, true, out, description, [this](const script_value& a_value, const std::string* a_spec) { return plain_text(a_value, a_spec); });
		if (!appended) { return appended.error_value(); }
		if (!counts(out.size())) { return output_limit(); }
		if (!limits_.memory_charge(out.size())) [[unlikely]] { return raise_memory_cap(limits_); }
		return script_value(std::move(out), eng_);
	}

	R scoped_render::eval_call(const call_expr* c) {
		const expression* callee = c->callee.get();
		if (callee->get_type() == node_type::identifier_expr) {
			const uint64_t id = static_cast<const identifier_expr*>(callee)->symbol_id;
			if (id == ids_.joiner) {
				auto result = join(c);
				return result;
			}
			if (id == ids_.splice) { return splice(c); }
			if (id == ids_.format_value && c->arguments.size() == 2) {
				auto v = eval(c->arguments[0].get());
				if (!v) { return v; }
				auto s = eval(c->arguments[1].get());
				if (!s) { return s; }
				if (!s.value().deref().is_string()) { return refusal(lib_, "format_value expects (value, spec_string)"); }
				const std::string spec = s.value().deref().as_string();
				auto text = plain_text(v.value().deref(), &spec);
				if (!text) { return text.error_value(); }
				return script_value(std::move(text.value()), eng_);
			}
			return refusal(lib_, "a scoped template calls only built-in methods and the string::, array:: and map:: functions");
		}
		if (callee->get_type() == node_type::member_expr && static_cast<const member_expr*>(callee)->is_static) {
			return namespace_call(c, static_cast<const member_expr*>(callee));
		}
		if (callee->get_type() != node_type::member_expr) {
			return refusal(lib_, "a scoped template does not call function values");
		}
		const auto* m = static_cast<const member_expr*>(callee);
		auto receiver = eval(m->object.get());
		if (!receiver) { return receiver; }
		std::vector<script_value> args;
		args.reserve(c->arguments.size());
		for (const auto& a : c->arguments) {
			auto v = eval(a.get());
			if (!v) { return v; }
			args.push_back(v.value().deref());
		}
		if (m->null_safe && receiver.value().deref().is_null()) { return null_value(); }
		return method(receiver.value().deref(), m, std::move(args));
	}

	// The engine's own function or value in string::, array:: or map::; null for anything else, a script's own function there included
	const script_value* scoped_render::builtin_namespace_value(const member_expr* a_member) const {
		if (!ids_.builtin_namespace(a_member)) { return nullptr; }
		const auto* base = static_cast<const identifier_expr*>(a_member->object.get());
		auto& spaces = engine_access::script_namespaces(*eng_);
		auto space = spaces.find(base->symbol_id);
		if (space == spaces.end() || !space->second) { return nullptr; }
		const uint64_t id = a_member->member_id != UINT64_MAX ? a_member->member_id : symbols().intern(a_member->member);
		auto held = space->second->variables.find(id);
		if (held == space->second->variables.end()) { return nullptr; }
		const script_value& v = held->second;
		if (v.is_function()) {
			const script_function& fn = v.unchecked_as_function();
			if (fn.target<script_callable_thunk>() || fn.target<script_method_dispatch>() || fn.target<script_static_method_dispatch>() || fn.target<bound_method_fn>()) { return nullptr; }
		}
		return &v;
	}

	R scoped_render::namespace_call(const call_expr* c, const member_expr* a_callee) {
		const script_value* held = builtin_namespace_value(a_callee);
		if (!held || !held->is_function()) {
			return refusal(lib_, "a scoped template calls only built-in methods and the string::, array:: and map:: functions: '" + std::string(a_callee->member) + "(...)' is not allowed");
		}
		const script_value fn = *held;
		std::vector<script_value> args;
		args.reserve(c->arguments.size());
		for (const auto& a : c->arguments) {
			auto v = eval(a.get());
			if (!v) { return v; }
			args.push_back(v.value().deref());
		}
		for (const auto& a : args) {
			if (object_like(a) || (data_has_objects_ && holds_object(a))) { return refuse_object("pass a built-in function"); }
		}
		R result = fn.unchecked_as_function()(args);
		if (result && limit_ != 0) {
			const script_value& made = result.value().deref();
			if (made.raw_storage_index() == script_value::TYPEID_STRING && made.unchecked_as_string().size() > limit_) { return output_limit(); }
		}
		return result;
	}

	checked_result<bool> sandbox_equal(void*, const script_value& a, const script_value& b) {
		return a.deref() == b.deref();
	}
	bool sandbox_unwinding(void*) { return false; }

	R scoped_render::method(const script_value& a_receiver, const member_expr* a_callee, std::vector<script_value> a_args) {
		const std::string_view name = a_callee->member;
		const uint64_t id = a_callee->member_id != UINT64_MAX ? a_callee->member_id : symbols().intern(name);
		if (object_like(a_receiver)) { return refuse_object("call a method of"); }
		if (data_has_objects_) {
			if (holds_object(a_receiver)) { return refuse_object("call a built-in method over"); }
			for (const auto& a : a_args) { if (holds_object(a)) { return refuse_object("pass a built-in method"); } }
		}
		if (a_receiver.is_math()) {
			const math_fn fn = vec_method_fn(a_receiver.type(), name);
			if (fn == math_fn::none) { return refusal(lib_, std::string(math_type_name(a_receiver.type())) + " has no member '" + std::string(name) + "'"); }
			return call_vec_method(fn, a_receiver, a_args, eng_);
		}
		if (!lib_.methods) {
			lib_.methods = std::make_unique<builtin_method_registries>();
			init_builtin_method_registries(&symbols(), *lib_.methods);
		}
		// a template uses every value it computes, and an in-place builtin returns nothing: the language's error, with its copy twin
		if (auto twin = in_place_builtin(lib_.methods->in_place, a_receiver, id)) { return in_place_result_error(&symbols(), a_callee, *twin); }
		const std::unordered_map<uint64_t, builtin_method>* table = nullptr;
		switch (a_receiver.raw_storage_index()) {
			case script_value::TYPEID_STRING: table = &lib_.methods->string_methods; break;
			case script_value::TYPEID_UTF8_STRING: table = &lib_.methods->utf8_methods; break;
			case script_value::TYPEID_ARRAY: table = &lib_.methods->array_methods; break;
			case script_value::TYPEID_MAP: table = &lib_.methods->map_methods; break;
			default: break;
		}
		auto found = table ? table->find(id) : decltype(table->end()){};
		if (!table || found == table->end()) {
			if (a_receiver.raw_storage_index() == script_value::TYPEID_MAP) { return refusal(lib_, map_member_text(name)); }
			return refusal(lib_, std::string(kind_name(a_receiver)) + " has no member '" + std::string(name) + "'");
		}
		builtin_method_context ctx;
		ctx.engine_ref = eng_;
		ctx.symbolizer = &symbols();
		ctx.equal_fn = &sandbox_equal;
		ctx.unwinding_fn = &sandbox_unwinding;
		ctx.backend_self = this;
		script_value self = a_receiver;
		R result = found->second(ctx, self, a_args);
		if (result && limit_ != 0) {
			const script_value& made = result.value().deref();
			if (made.raw_storage_index() == script_value::TYPEID_STRING && made.unchecked_as_string().size() > limit_) { return output_limit(); }
		}
		return result;
	}

	std::shared_ptr<const scoped_program> compile_scoped(library& a_lib, const std::string& a_content, const std::string& a_label, std::string& a_error) {
		const std::string key = a_label + '\n' + a_content;
		if (auto it = a_lib.scoped_programs.find(key); it != a_lib.scoped_programs.end()) { return it->second; }
		string_symbolizer* symbols = engine_access::get_symbolizer(*a_lib.eng);
		auto program = std::make_shared<scoped_program>();
		lexer lex(a_content, symbols, a_label);
		program->tokens = std::make_shared<std::vector<token>>(lex.tokenize_template(a_lib.joiner, a_lib.splice));
		static const std::unordered_set<std::string> no_template_types;
		parser prs(*program->tokens, symbols, a_lib.eng, no_template_types, a_label);
		prs.source_text(a_content);
		auto body = prs.parse_scoped_template_body();
		if (!body) {
			a_error = format_error(body, *symbols);
			return nullptr;
		}
		program->body = std::move(body.value());
		if (a_lib.scoped_programs.size() >= 256) { a_lib.scoped_programs.clear(); }
		a_lib.scoped_programs.emplace(key, program);
		return program;
	}

	R render_scoped(library& a_lib, const std::shared_ptr<const scoped_program>& a_program, const script_value& a_data) {
		const script_value& data = a_data.deref();
		if (data.raw_storage_index() != script_value::TYPEID_MAP) { return a_lib.refused("scoped_template's second argument is a map"); }
		scoped_render render(a_lib, a_program);
		R result = render.render(data.clone());
		return result;
	}

} // namespace

void scoped_violations(const expression& body, string_symbolizer& symbols, std::vector<violation>& out) {
	rule_walk walk(symbols, out);
	try {
		walk.walk(&body);
	} catch (const nesting_exhausted& e) {
		out.push_back({e.location(), std::string(k_nesting_too_deep_listing)});
	}
}

scoped_file resolve_scoped_file(engine& eng, const std::string& path, const std::string& from) {
	namespace fs = std::filesystem;
	scoped_file result;
	const fs::path asked = fs::path(path);
	// a drive prefix refuses on every platform, where only Windows' fs::path gives it a root name
	const bool names_a_drive = path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0]));
	if (path.empty() || path.front() == '/' || path.front() == '\\' || names_a_drive || asked.has_root_name() || asked.has_root_directory()) {
		result.error = "scoped_template: '" + path + "' is not a path relative to its template directories";
		return result;
	}
	size_t start = 0;
	while (start <= path.size()) {
		const size_t end = path.find_first_of("/\\", start);
		const std::string segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (segment == "..") {
			result.error = "scoped_template: '" + path + "' names a parent directory (..)";
			return result;
		}
		if (end == std::string::npos) { break; }
		start = end + 1;
	}
	std::error_code ec;
	bool escaped = false;
	for (const std::string& listed : eng.scoped_template_dirs()) {
		auto located = resolve_on_import_ladder(listed, &eng, from);   // a relative folder resolves as an import's file does
		if (!located) { continue; }
		const fs::path folder = fs::weakly_canonical(fs::path(located.value()), ec);
		if (ec || !fs::is_directory(folder, ec)) { ec.clear(); continue; }
		const fs::path candidate = fs::weakly_canonical(folder / asked, ec);
		if (ec || !fs::is_regular_file(candidate, ec)) { ec.clear(); continue; }
		if (!lies_inside(candidate, folder)) { escaped = true; continue; }
		result.resolved = candidate.string();
		return result;
	}
	result.error = escaped ? "scoped_template: '" + path + "' leaves its template directory through a link"
	                       : "template file '" + path + "' not found in the scoped template directories";
	return result;
}

void register_library(engine& eng) {
	dynamic_binder<html_text>(eng, "web::html")
		.constructor<>()
		.method("to_string", &html_text::to_string)
		.build();
	const auto html_class = engine_access::get_class_definition(eng, "web::html");
	auto lib = std::make_shared<library>();
	lib->eng = &eng;
	lib->html_class = html_class.get();
	lib->html_type = html_class->get_type_id();
	lib->joiner = engine_access::symbolize(eng, k_joiner);
	lib->splice = engine_access::symbolize(eng, k_splice);
	lib->raw = engine_access::symbolize(eng, k_raw);
	lib->format_value = engine_access::symbolize(eng, "format_value");
	lib->args = engine_access::symbolize(eng, "args");

	auto& reflect_space = engine_access::script_namespaces(eng)[engine_access::symbolize(eng, "reflect")];
	if (!reflect_space) { reflect_space = std::make_shared<script_namespace_data>(); }
	reflect_space->variables[engine_access::symbolize(eng, "scoped_template_dirs")] = script_value::make_function([lib](const std::vector<script_value>& a_args) -> R {
		if (!a_args.empty()) { return R(make_error_code(runtime_error_code::argument_count_mismatch), "reflect::scoped_template_dirs takes no arguments"); }
		script_value dirs = script_value::make_array(engine_access::get_type_info_string(*lib->eng), lib->eng);
		for (const auto& dir : lib->eng->scoped_template_dirs()) { dirs.unchecked_get_array_storage()->push(script_value(dir, lib->eng)); }
		return dirs;
	}, &eng);

	eng.add_variadic_function(std::string(k_joiner), [lib](const std::vector<script_value>& a_args) -> R {
		size_t total = 0;
		for (const auto& part : a_args) {
			const script_value& text = part.deref();
			if (text.raw_storage_index() != script_value::TYPEID_STRING) { return lib->refused("a template part is not text"); }
			total += text.unchecked_as_string().size();
		}
		if (const size_t limit = engine_access::execution_limits(*lib->eng).template_output_limit; limit != 0 && total > limit) { return trusted_output_limit(*lib, limit); }
		if (!builtin_memory_charge(lib->eng, total)) [[unlikely]] { return builtin_memory_cap(lib->eng); }
		std::string out;
		out.reserve(total);
		for (const auto& part : a_args) { out += part.deref().unchecked_as_string(); }
		return lib->make_html(std::move(out));
	}, serial);

	eng.add_variadic_function(std::string(k_splice), [lib](const std::vector<script_value>& a_args) -> R {
		if (a_args.size() != 3 || !a_args[1].deref().is_string() || !a_args[2].deref().is_string()) { return lib->refused("a template splice takes a value, a format spec and its spelling"); }
		const std::string spec_text = a_args[1].deref().as_string();
		const std::string* spec = spec_text.empty() ? nullptr : &spec_text;
		std::string out;
		auto appended = append_splice(*lib, a_args[0], spec, false, out, a_args[2].deref().as_string(),
			[&lib](const script_value& a_value, const std::string* a_spec) { return trusted_text(*lib, a_value, a_spec); });
		if (!appended) { return appended.error_value(); }
		if (engine_access::backend_unwinding(*lib->eng)) { return script_value(std::monostate{}, lib->eng); }
		if (const size_t limit = engine_access::execution_limits(*lib->eng).template_output_limit; limit != 0 && out.size() > limit) { return trusted_output_limit(*lib, limit); }
		if (!builtin_memory_charge(lib->eng, out.size())) [[unlikely]] { return builtin_memory_cap(lib->eng); }
		return script_value(std::move(out), lib->eng);
	}, serial);

	eng.add_variadic_function(std::string(k_raw), [lib](const std::vector<script_value>& a_args) -> R {
		if (a_args.size() != 1) { return R(make_error_code(runtime_error_code::argument_count_mismatch), "raw() takes 1 argument"); }
		if (const html_text* h = lib->as_html(a_args[0])) { return lib->make_html(h->text); }
		auto text = lib->eng->text_of(a_args[0]);
		if (!text) { return text.error_value(); }
		return lib->make_html(std::move(text.value()));
	}, serial);

	eng.add_variadic_function(std::string(k_scoped), [lib](const std::vector<script_value>& a_args) -> R {
		if (a_args.size() != 3 || !a_args[0].deref().is_string() || !a_args[1].deref().is_string()) { return lib->refused("scoped_template_text(text, map) takes a text and a map"); }
		if (auto allowed = lib->eng->require_permission(permissions::scoped_template); !allowed) { return allowed.error_value(); }
		std::string error;
		auto program = compile_scoped(*lib, a_args[0].deref().as_string(), a_args[1].deref().as_string(), error);
		if (!program) { return lib->refused(error); }
		return render_scoped(*lib, program, a_args[2]);
	}, serial);

	eng.add_variadic_function(std::string(k_scoped_file), [lib](const std::vector<script_value>& a_args) -> R {
		if (a_args.size() != 3 || !a_args[0].deref().is_string() || !a_args[1].deref().is_string()) { return lib->refused("scoped_template(path, map) takes a path and a map"); }
		engine& e = *lib->eng;
		if (auto allowed = e.require_permission(permissions::scoped_template); !allowed) { return allowed.error_value(); }
		const scoped_file found = resolve_scoped_file(e, a_args[0].deref().as_string(), a_args[1].deref().as_string());
		if (!found.error.empty()) { return lib->refused(found.error); }
		const std::string& resolved_path = found.resolved;
		scoped_file_entry& file = lib->scoped_files[resolved_path];
		if (!file.loaded || check_due(e, file.next_check)) {
			uint64_t mtime = 0, size = 0;
			const bool stamped = stat_file(resolved_path, mtime, size);
			const bool moved = !file.loaded || !stamped || mtime != file.mtime || size != file.size;
			const bool held = file.loaded && file.failed && stamped && mtime == file.failed_mtime && size == file.failed_size;
			if (moved && !held) {
				std::string content;
				if (!read_whole_file(resolved_path, content)) {
					if (!file.loaded) { return lib->refused("template file '" + a_args[0].deref().as_string() + "' cannot be read"); }
				} else {
					std::string error;
					if (!compile_scoped(*lib, content, resolved_path, error)) {
						if (!file.loaded) { return lib->refused(error); }
						file.failed = true;
						file.failed_mtime = mtime;
						file.failed_size = size;
						return lib->refused(error);
					}
					file.content = std::move(content);
					file.loaded = true;
					file.failed = false;
					file.mtime = mtime;
					file.size = size;
				}
			}
		}
		std::string error;
		auto program = compile_scoped(*lib, file.content, resolved_path, error);
		if (!program) { return lib->refused(error); }
		return render_scoped(*lib, program, a_args[2]);
	}, serial);

	// template(path, a, b, ...) with a path known at run time: the unit `(auto& a, auto& b) => template("<file>", a, b);` renders
	// the file with its literal path, so it expands, checks and reloads as a literal path does; the function it made renders
	// until the file (or a template it reads) changes and checks again
	eng.add_variadic_function(std::string(k_dynamic), [lib](const std::vector<script_value>& a_args) -> R {
		if (a_args.size() != 3 || !a_args[1].deref().is_string() || !a_args[2].deref().is_string()) { return lib->refused("template() takes a path, then the names the template sees"); }
		engine& e = *lib->eng;
		if (!a_args[0].deref().is_string()) { return R(make_error_code(runtime_error_code::type_mismatch), "{0}", engine_access::park_error_text(e, "template() path is not a string")); }
		const std::string path = a_args[0].deref().as_string();
		if (auto allowed = e.require_permission(permissions::filesystem); !allowed) { return allowed.error_value(); }
		auto resolved = resolve_include_path(path, &e, a_args[2].deref().as_string());
		if (!resolved) { return lib->refused("template file '" + path + "' not found"); }
		const std::string names = a_args[1].deref().as_string();
		dynamic_entry& entry = lib->dynamic_templates[resolved.value() + '\n' + names];
		if (entry.function && !check_due(e, entry.next_check)) { return *entry.function; }
		if (entry.unit.empty()) {
			std::string params, passed;
			size_t start = 0;
			while (start <= names.size() && !names.empty()) {
				size_t end = names.find(',', start);
				if (end == std::string::npos) { end = names.size(); }
				const std::string one = names.substr(start, end - start);
				if (!params.empty()) { params += ", "; passed += ", "; }
				params += "auto& " + one;
				passed += one;
				start = end + 1;
			}
			entry.unit = resolved.value() + "#template(" + names + ")";
			entry.source = "(" + params + ") => template(R\"jai_template(" + resolved.value() + ")jai_template\"" + (passed.empty() ? "" : ", " + passed) + ");\n";
		}
		if (entry.function) {
			if (engine_access::template_unit_current(e, entry.unit, entry.source)) { return *entry.function; }
			uint64_t mtime = 0, size = 0;
			if (entry.failed && stat_file(resolved.value(), mtime, size) && mtime == entry.failed_mtime && size == entry.failed_size) { return *entry.function; }
		}
		R made = engine_access::run_template_unit(e, entry.unit, entry.source);
		if (!made) {
			if (entry.function) {
				entry.failed = true;
				(void)stat_file(resolved.value(), entry.failed_mtime, entry.failed_size);
			}
			return made;
		}
		entry.function = std::move(made.value());
		entry.failed = false;
		return *entry.function;
	}, serial);
}

} // namespace jai::detail::template_library
