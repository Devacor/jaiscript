#include "../../include/jaiscript/detail/coroutine_snapshot.hpp"
#include "../../include/jaiscript/core/coroutine.hpp"
#include "../../include/jaiscript/core/engine.hpp"
#include "../../include/jaiscript/core/execution_backend.hpp"
#include "../../include/jaiscript/core/script_namespace.hpp"
#include "../../include/jaiscript/detail/ast_serializer.hpp"
#include "../../include/jaiscript/detail/class_runtime.hpp"
#include "../../include/jaiscript/detail/engine_access.hpp"
#include "../../include/jaiscript/detail/expr_children.hpp"
#include "../../include/jaiscript/detail/namespace_function.hpp"
#include "../../include/jaiscript/detail/native_stack.hpp"
#include "../../include/jaiscript/serialization/archive_impl.hpp"

namespace jai::detail {

namespace coroutine_snapshot_impl {

// The words a reason spells its asker with: a save's or a copy's
std::string_view capture_noun(coroutine_capture a_why) { return a_why == coroutine_capture::copy ? "copy" : "save"; }
std::string_view capture_done(coroutine_capture a_why) { return a_why == coroutine_capture::copy ? "copied" : "saved"; }

// A copy's reasons speak of the moment of the copy: they are read when the copy's coroutine is resumed
std::string refusal_text(coroutine_refusal a_refusal, coroutine_capture a_why) {
	const std::string is(a_why == coroutine_capture::copy ? "was" : "is");
	switch (a_refusal) {
		case coroutine_refusal::in_expression: return "it " + is + " paused inside an expression, and only a yield that is its own statement is " + std::string(capture_done(a_why));
		case coroutine_refusal::in_catch: return "it " + is + " paused inside a catch block";
		default: return "its body declares a function, a class, an enum or a namespace ahead of the yield it " + is + " paused at";
	}
}

// A copy's record that does not fit its function: the copy's coroutine stays without a frame
struct copy_not_rebuilt {};

constexpr std::string_view k_kind_function = "function";
constexpr std::string_view k_kind_method = "method";
constexpr std::string_view k_kind_static = "static";

bool declares_into_scope(const jai::statement* a_statement) {
	switch (a_statement->get_type()) {
		case node_type::function_decl: case node_type::class_decl: case node_type::namespace_decl: case node_type::enum_decl:
		case node_type::include_decl: case node_type::import_decl: case node_type::extern_decl:
			return true;
		default:
			return false;
	}
}

// The yields of a body in source order, each with the constructs around it and the slots in scope
struct site_walker {
	std::vector<coroutine_site>& out;
	coroutine_site live;
	coroutine_refusal refusal = coroutine_refusal::none;
	native_stack_mark stack;

	void emit(const yield_expr* a_yield, coroutine_refusal a_refusal) {
		coroutine_site site = live;
		site.yield = a_yield;
		site.refusal = a_refusal;
		out.push_back(std::move(site));
	}

	void expression_yields(const expression* a_expression) {
		if (!a_expression) { return; }
		stack.guard(a_expression->location);
		if (a_expression->get_type() == node_type::yield_expr) { emit(static_cast<const yield_expr*>(a_expression), coroutine_refusal::in_expression); }
		for_each_child_expression(a_expression, [&](const expression* a_child) { expression_yields(a_child); });
	}

	void statement_expression(const expression* a_expression) {
		if (a_expression && a_expression->get_type() == node_type::yield_expr) {
			const auto* yield = static_cast<const yield_expr*>(a_expression);
			emit(yield, refusal);
			expression_yields(yield->value.get());
			return;
		}
		expression_yields(a_expression);
	}

	void declare(const jai::statement* a_statement) {
		if (a_statement->get_type() == node_type::variable_decl) {
			const auto* decl = static_cast<const variable_decl*>(a_statement);
			if (decl->slot_index == SIZE_MAX || decl->is_static) { return; }
			coroutine_local local;
			local.slot = decl->slot_index;
			local.name = decl->name;
			local.type = decl->type;
			local.inferred = decl->inferred_type;
			local.held = decl->type && decl->type->base_type == script_value_type::jai_reference_type ? coroutine_local::kind::reference : coroutine_local::kind::value;
			local.boxed = decl->ref_escaping;
			live.locals.push_back(std::move(local));
		} else if (a_statement->get_type() == node_type::destructuring_decl) {
			const auto* decl = static_cast<const destructuring_decl*>(a_statement);
			for (size_t i = 0; i < decl->slot_indices.size(); ++i) {
				if (decl->slot_indices[i] == SIZE_MAX) { continue; }
				coroutine_local local;
				local.slot = decl->slot_indices[i];
				local.name = decl->names[i].first;
				local.boxed = i < decl->ref_escaping.size() && decl->ref_escaping[i];
				live.locals.push_back(std::move(local));
			}
		}
	}

	template <class Body>
	void step(coroutine_step::kind a_kind, const ast_node* a_node, Body&& a_body) {
		coroutine_step entered;
		entered.what = a_kind;
		entered.node = a_node;
		live.path.push_back(entered);
		a_body();
		live.path.pop_back();
	}

	void statements(const std::vector<statement_ptr>& a_body) {
		const size_t mark = live.locals.size();
		const coroutine_refusal outer = refusal;
		for (size_t i = 0; i < a_body.size(); ++i) {
			live.path.back().index = i;
			walk(a_body[i].get());
			declare(a_body[i].get());
			if (declares_into_scope(a_body[i].get()) && refusal == coroutine_refusal::none) { refusal = coroutine_refusal::after_declaration; }
		}
		live.locals.resize(mark);
		refusal = outer;
	}

	void block(const block_stmt* a_block) {
		step(coroutine_step::kind::block, a_block, [&]() {
			const size_t mark = live.locals.size();
			const coroutine_refusal outer = refusal;
			for (size_t i = 0; i < a_block->declarations.size(); ++i) {
				const jai::statement* decl = a_block->declarations[i].get();
				live.path.back().index = i;
				walk(decl);
				declare(decl);
				if (declares_into_scope(decl) && refusal == coroutine_refusal::none) { refusal = coroutine_refusal::after_declaration; }
			}
			live.locals.resize(mark);
			refusal = outer;
		});
	}

	void walk(const jai::statement* a_statement) {
		if (!a_statement) { return; }
		stack.guard(a_statement->location);
		switch (a_statement->get_type()) {
			case node_type::expression_stmt: statement_expression(static_cast<const expression_stmt*>(a_statement)->expression.get()); break;
			case node_type::expression_decl: statement_expression(static_cast<const expression_decl*>(a_statement)->expression.get()); break;
			case node_type::statement_decl: walk(static_cast<const statement_decl*>(a_statement)->statement.get()); break;
			case node_type::variable_decl: expression_yields(static_cast<const variable_decl*>(a_statement)->initializer.get()); break;
			case node_type::destructuring_decl: expression_yields(static_cast<const destructuring_decl*>(a_statement)->initializer.get()); break;
			case node_type::return_stmt: expression_yields(static_cast<const return_stmt*>(a_statement)->value.get()); break;
			case node_type::block_stmt: block(static_cast<const block_stmt*>(a_statement)); break;
			case node_type::if_stmt: {
				const auto* branch = static_cast<const if_stmt*>(a_statement);
				expression_yields(branch->condition.get());
				step(coroutine_step::kind::if_then, branch, [&]() { walk(branch->then_statement.get()); });
				step(coroutine_step::kind::if_else, branch, [&]() { walk(branch->else_statement.get()); });
				break;
			}
			case node_type::while_stmt: {
				const auto* loop = static_cast<const while_stmt*>(a_statement);
				expression_yields(loop->condition.get());
				step(coroutine_step::kind::while_body, loop, [&]() { walk(loop->body.get()); });
				break;
			}
			case node_type::for_stmt: {
				const auto* loop = static_cast<const for_stmt*>(a_statement);
				const size_t mark = live.locals.size();
				if (loop->initializer) {
					walk(loop->initializer.get());
					declare(loop->initializer.get());
				}
				expression_yields(loop->condition.get());
				expression_yields(loop->update.get());
				step(coroutine_step::kind::for_body, loop, [&]() { walk(loop->body.get()); });
				live.locals.resize(mark);
				break;
			}
			case node_type::range_for_stmt: {
				const auto* loop = static_cast<const range_for_stmt*>(a_statement);
				expression_yields(loop->container.get());
				const size_t mark = live.locals.size();
				live.loops.push_back(loop);
				coroutine_local variable;
				variable.slot = loop->variable_slot_index;
				variable.name = loop->variable_name;
				variable.type = loop->element_type;
				variable.held = coroutine_local::kind::loop_variable;
				variable.boxed = loop->var_ref_escaping;
				variable.by_reference = loop->is_reference;
				variable.binds_entry = loop->binds_map_entry;
				variable.loop = static_cast<uint32_t>(live.loops.size() - 1);
				live.locals.push_back(std::move(variable));
				step(coroutine_step::kind::range_body, loop, [&]() { walk(loop->body.get()); });
				live.loops.pop_back();
				live.locals.resize(mark);
				break;
			}
			case node_type::switch_stmt: {
				const auto* chosen = static_cast<const switch_stmt*>(a_statement);
				expression_yields(chosen->condition.get());
				live.switches.push_back(chosen);
				for (size_t i = 0; i < chosen->cases.size(); ++i) {
					const case_stmt* branch = chosen->cases[i].get();
					expression_yields(branch->value.get());
					step(coroutine_step::kind::case_body, branch, [&]() {
						live.path.back().branch = i;
						live.path.back().owner = chosen;
						statements(branch->body);
					});
				}
				if (chosen->default_case) {
					step(coroutine_step::kind::default_body, chosen->default_case.get(), [&]() {
						live.path.back().owner = chosen;
						statements(chosen->default_case->body);
					});
				}
				live.switches.pop_back();
				break;
			}
			case node_type::try_stmt: {
				const auto* guarded = static_cast<const try_stmt*>(a_statement);
				walk(guarded->try_block.get());
				const coroutine_refusal outer = refusal;
				refusal = coroutine_refusal::in_catch;
				walk(guarded->catch_block.get());
				refusal = outer;
				break;
			}
			case node_type::parallel_for_stmt: {
				const auto* loop = static_cast<const parallel_for_stmt*>(a_statement);
				expression_yields(loop->container.get());
				for (const auto& extra : loop->extra_containers) { expression_yields(extra.get()); }
				break;
			}
			default:
				break;
		}
	}
};

std::string hex64(uint64_t a_value) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string result(16, '0');
	for (size_t i = 0; i < 16; ++i) { result[15 - i] = digits[(a_value >> (i * 4)) & 0xF]; }
	return result;
}

// The declaration's structure as parsed, as 16 hex digits: equal for the same source on any engine and backend
std::string function_code(engine& a_engine, const function_decl& a_function) {
	std::vector<uint8_t> key;
	try {
		key = ast_writer(*engine_access::get_symbolizer(a_engine), false).as_parsed().node_key(&a_function);
	} catch (const std::exception&) {
		return {};
	}
	uint64_t hash = 14695981039346656037ull;
	for (uint8_t byte : key) {
		hash ^= byte;
		hash *= 1099511628211ull;
	}
	return hex64(hash);
}

struct coroutine_origin {
	std::shared_ptr<function_decl> declaration;
	std::shared_ptr<environment> closure;
	script_value minted{std::monostate{}, nullptr};   // a method's handle as a call mints it (declaration, scope and receiver set)
};

uint64_t name_id_of(engine& a_engine, const function_decl& a_function) {
	return a_function.name_id != UINT64_MAX ? a_function.name_id : engine_access::symbolize(a_engine, a_function.name);
}

std::shared_ptr<function_decl> overload_with_code(engine& a_engine, const std::unordered_map<uint64_t, std::vector<std::shared_ptr<function_decl>>>& a_table,
                                                  uint64_t a_name_id, const std::string& a_code) {
	auto found = a_table.find(a_name_id);
	if (found == a_table.end()) { return nullptr; }
	for (const auto& overload : found->second) {
		if (overload && overload->is_coroutine && function_code(a_engine, *overload) == a_code) { return overload; }
	}
	return nullptr;
}

// The class of `a_class`'s chain that declares the coroutine method with this code
std::shared_ptr<class_definition> declaring_class(engine& a_engine, const std::shared_ptr<class_definition>& a_class, uint64_t a_name_id, const std::string& a_code) {
	if (!a_class) { return nullptr; }
	if (overload_with_code(a_engine, a_class->script_method_overloads(), a_name_id, a_code)) { return a_class; }
	for (const auto& parent : a_class->get_parent_classes()) {
		if (auto found = declaring_class(a_engine, parent, a_name_id, a_code)) { return found; }
	}
	return nullptr;
}

std::pair<std::string_view, std::string_view> split_qualified(std::string_view a_name) {
	const size_t at = a_name.rfind("::");
	if (at == std::string_view::npos) { return {std::string_view{}, a_name}; }
	return {a_name.substr(0, at), a_name.substr(at + 2)};
}

// The current definition a saved (kind, name, code) names; empty when the program has none with that code
bool find_origin(engine& a_engine, std::string_view a_kind, std::string_view a_name, const std::string& a_code, const script_value& a_receiver,
                 coroutine_origin& a_out, bool& a_named) {
	a_named = false;
	const auto [scope, leaf] = split_qualified(a_name);
	const uint64_t leaf_id = engine_access::symbolize(a_engine, leaf);
	execution_backend* backend = engine_access::get_execution_backend(a_engine);
	if (a_kind == k_kind_method || a_kind == k_kind_static) {
		const std::shared_ptr<class_definition> owner = engine_access::get_class_definition(a_engine, std::string(scope));
		if (!owner) { return false; }
		const bool statics = a_kind == k_kind_static;
		const auto& table = statics ? owner->script_static_method_overloads() : owner->script_method_overloads();
		a_named = table.find(leaf_id) != table.end();
		const std::shared_ptr<function_decl> overload = overload_with_code(a_engine, table, leaf_id, a_code);
		if (!overload || !backend) { return false; }
		script_callable payload;
		payload.kind = statics ? script_callable::kind_type::static_method : script_callable::kind_type::method;
		payload.more().ast = overload;
		payload.more().cls = owner;
		const auto& values = statics ? owner->static_method_values() : owner->method_values();
		auto value = values.find(leaf_id);
		if (value == values.end() || !value->second.is_function()) { return false; }
		if (statics) {
			const auto* dispatch = value->second.as_function().target<script_static_method_dispatch>();
			if (!dispatch) { return false; }
			payload.more().definition_env = dispatch->definition_env;
		} else {
			const auto* dispatch = value->second.as_function().target<script_method_dispatch>();
			const class_instance* instance = a_receiver.peek_class_instance();
			const class_definition* held = instance ? instance->get_class_definition() : nullptr;
			if (!dispatch || !held || (held != owner.get() && !held->derives_from(owner.get()))) { return false; }
			payload.more().definition_env = dispatch->definition_env;
			payload.more().this_obj = a_receiver;
		}
		auto minted = backend->execute_callable(payload, {});
		if (!minted || !coroutine_of(minted.value())) { return false; }
		a_out.minted = std::move(minted.value());
		return true;
	}
	if (a_kind != k_kind_function) { return false; }
	if (!scope.empty()) {
		const uint64_t namespace_id = engine_access::symbolize(a_engine, scope);
		auto& namespaces = engine_access::script_namespaces(a_engine);
		auto found = namespaces.find(namespace_id);
		if (found == namespaces.end() || !found->second) { return false; }
		a_named = found->second->functions.find(leaf_id) != found->second->functions.end();
		a_out.declaration = overload_with_code(a_engine, found->second->functions, leaf_id, a_code);
		if (!a_out.declaration) { return false; }
		a_out.closure = namespace_body_environment(a_engine, namespace_id);
		return true;
	}
	const script_value* cell = engine_access::get_global_environment(a_engine)->get_value_ptr(leaf_id);
	if (!cell) { return false; }
	const script_value& value = cell->is_reference() ? cell->deref() : *cell;
	const coroutine_mint* mint = value.is_function() ? value.as_function().target<coroutine_mint>() : nullptr;
	if (!mint || !mint->declaration) { return false; }
	a_named = true;
	if (function_code(a_engine, *mint->declaration) != a_code) { return false; }
	a_out.declaration = mint->declaration;
	a_out.closure = mint->closure;
	return true;
}

struct coroutine_identity {
	std::string kind;
	std::string name;
	std::string code;
};

// The names a paused coroutine's function is saved under, into a_out; the reason (empty: none) when the program no longer
// defines that function
std::string identify(engine& a_engine, const coroutine_handle& a_handle, coroutine_identity& a_out) {
	const std::shared_ptr<function_decl> function = a_handle.get_function();
	coroutine_identity result;
	result.code = function_code(a_engine, *function);
	const uint64_t name_id = name_id_of(a_engine, *function);
	const std::string leaf(function->name);
	if (const class_instance* receiver = a_handle.receiver().peek_class_instance()) {
		const class_definition* held = receiver->get_class_definition();
		const std::shared_ptr<class_definition> start = held ? engine_access::get_class_definition(a_engine, held->get_name()) : nullptr;
		if (auto owner = declaring_class(a_engine, start, name_id, result.code)) {
			result.kind = k_kind_method;
			result.name = owner->get_name() + "::" + leaf;
			a_out = std::move(result);
			return {};
		}
		return "its class no longer declares the function it runs";
	}
	const std::shared_ptr<environment> closure = a_handle.get_closure_env();
	if (closure && closure->is_static_method_env()) {
		const std::shared_ptr<class_definition> owner = closure->get_class_definition();
		if (owner && overload_with_code(a_engine, owner->script_static_method_overloads(), name_id, result.code)) {
			result.kind = k_kind_static;
			result.name = owner->get_name() + "::" + leaf;
			a_out = std::move(result);
			return {};
		}
		return "its class no longer declares the function it runs";
	}
	result.kind = k_kind_function;
	string_symbolizer* symbols = engine_access::get_symbolizer(a_engine);
	for (const auto& [namespace_id, data] : engine_access::script_namespaces(a_engine)) {
		if (data && overload_with_code(a_engine, data->functions, name_id, result.code)) {
			result.name = std::string(symbols->get_string(namespace_id)) + "::" + leaf;
			a_out = std::move(result);
			return {};
		}
	}
	coroutine_origin origin;
	bool named = false;
	if (find_origin(a_engine, k_kind_function, leaf, result.code, script_value(std::monostate{}, &a_engine), origin, named)) {
		result.name = leaf;
		a_out = std::move(result);
			return {};
	}
	return named ? "its function was redefined after it started" : "its function is declared inside another function";
}

script_value text_value(engine& a_engine, std::string_view a_text) { return script_value(std::string(a_text), &a_engine); }

script_value make_list(engine& a_engine) { return script_value::make_array(nullptr, &a_engine); }
void list_push(script_value& a_list, script_value a_value) { const_cast<std::vector<script_value>&>(a_list.as_array()).push_back(std::move(a_value)); }

script_value make_record(engine& a_engine) { return script_value::make_map(engine_access::get_type_info_string(a_engine), nullptr, &a_engine); }
void record_set(engine& a_engine, script_value& a_record, std::string_view a_key, script_value a_value) {
	const_cast<script_map&>(a_record.as_map()).emplace(text_value(a_engine, a_key), std::move(a_value));
}

const script_value& plain(const script_value& a_value) { return a_value.is_reference() ? a_value.deref() : a_value; }

// The declared type a value carries that its saved form does not: an enum's, a typed container's
std::string stamp_of(const script_value& a_value) {
	const script_value& value = plain(a_value);
	const type_info_ptr type = value.get_type_info();
	if (!type) { return {}; }
	if (type->is_enum) { return type->canonical_name(); }
	const auto typed = [](const type_info_ptr& a_part) {
		return a_part && a_part->base_type != script_value_type::jai_any_type && a_part->base_type != script_value_type::jai_null_type;
	};
	if (value.is_array() && type->base_type == script_value_type::jai_array_type && typed(type->element_type())) { return type->canonical_name(); }
	if (value.is_map() && type->base_type == script_value_type::jai_map_type && (typed(type->key_type()) || typed(type->value_type()))) { return type->canonical_name(); }
	return {};
}

// A value where the record holds one: itself, or beside the type it carries; a map always the second, so the two never read alike
script_value packed(engine& a_engine, const script_value& a_value) {
	const script_value& value = plain(a_value);
	std::string stamp = stamp_of(value);
	if (stamp.empty() && !value.is_map()) { return value; }
	script_value wrapper = make_record(a_engine);
	record_set(a_engine, wrapper, "as", text_value(a_engine, stamp));
	record_set(a_engine, wrapper, "value", value);
	return wrapper;
}

// A slot whose declaration names its type: a load converts to it, so the save holds the bare value
bool declares_type(const type_info_ptr& a_declared) {
	return a_declared && a_declared->base_type != script_value_type::jai_any_type && a_declared->base_type != script_value_type::jai_reference_type;
}

const script_value* record_member(const script_map& a_record, engine& a_engine, std::string_view a_key) {
	auto found = a_record.find(text_value(a_engine, a_key));
	return found == a_record.end() ? nullptr : &found->second;
}

struct record_reader {
	engine& eng;
	std::string function;
	coroutine_capture why = coroutine_capture::save;

	[[noreturn]] void fail(std::string_view a_reason) const {
		if (why == coroutine_capture::copy) { throw copy_not_rebuilt{}; }
		throw_coroutine_not_loaded(function, a_reason);
	}

	script_value unpacked(const script_value& a_position) const {
		if (!a_position.is_map()) { return a_position; }
		const script_map& wrapper = a_position.as_map();
		const script_value* as = record_member(wrapper, eng, "as");
		const script_value* value = record_member(wrapper, eng, "value");
		if (wrapper.size() != 2 || !as || !as->is_string() || !value) { fail("its save is malformed"); }
		if (as->as_string().empty()) { return *value; }
		type_info* declared = engine_access::get_type_info_by_name(eng, as->as_string());
		if (!declared) { fail("its save names the type '" + as->as_string() + "', which this program does not have"); }
		auto converted = field_store_convert_no_script(*value, type_info_ptr(declared), &eng);
		if (!converted) { fail("its save holds a value that is no '" + as->as_string() + "'"); }
		return std::move(*converted);
	}

	// A saved value into its local's slot, as a load stores a field: converted to the declared type, or to the type an
	// auto declaration inferred
	script_value typed(const script_value& a_position, const coroutine_local& a_local) const {
		const bool declared = declares_type(a_local.type);
		if (!declared && !a_local.inferred) { return unpacked(a_position); }
		auto converted = declared ? field_store_convert_no_script(a_position, a_local.type, &eng)
		                          : field_store_convert_no_script(unpacked(a_position), a_local.inferred, &eng);
		if (!converted) { fail("its save holds a value its local '" + std::string(a_local.name) + "' cannot take"); }
		return std::move(*converted);
	}

	const script_value& member(const script_map& a_record, std::string_view a_key) const {
		const script_value* found = record_member(a_record, eng, a_key);
		if (!found) { fail("its save is malformed"); }
		return *found;
	}

	std::vector<script_value> list(const script_map& a_record, std::string_view a_key) const {
		const script_value& found = member(a_record, a_key);
		if (!found.is_array()) { fail("its save is malformed"); }
		std::vector<script_value> result;
		const script_array* node = found.unchecked_array_node();
		result.reserve(node->size());
		for (size_t i = 0; i < node->size(); ++i) { result.push_back(node->get(i, &eng)); }
		return result;
	}
};

const script_array* array_node(const script_value& a_value) {
	const script_value& value = plain(a_value);
	return value.is_array() ? value.unchecked_array_node() : nullptr;
}

const script_map* map_node(const script_value& a_value) {
	const script_value& value = plain(a_value);
	return value.is_map() ? &value.as_map() : nullptr;
}

bool same_container(const script_value& a_left, const script_value& a_right) {
	if (const script_array* left = array_node(a_left)) { return left == array_node(a_right); }
	if (const script_map* left = map_node(a_left)) { return left == map_node(a_right); }
	return false;
}

size_t container_holders(const script_value& a_container) {
	const script_value& value = plain(a_container);
	if (value.is_array()) { return const_cast<script_value&>(value).get_array_storage().use_count(); }
	if (value.is_map()) { return const_cast<script_value&>(value).get_map_storage().use_count(); }
	return 0;
}

} // namespace coroutine_snapshot_impl

std::vector<coroutine_site> coroutine_sites(const std::vector<parameter>& a_parameters, const block_stmt& a_body) {
	using namespace coroutine_snapshot_impl;
	std::vector<coroutine_site> result;
	site_walker walker{result, {}, coroutine_refusal::none, {}};
	for (const parameter& bound : a_parameters) {
		if (bound.slot_index == SIZE_MAX) { continue; }
		coroutine_local local;
		local.slot = bound.slot_index;
		local.name = bound.name;
		local.type = bound.type;
		local.boxed = bound.ref_escaping;
		walker.live.locals.push_back(std::move(local));
	}
	walker.block(&a_body);
	return result;
}

bool inert_case_value(const expression* a_value) {
	if (!a_value) { return false; }
	switch (a_value->get_type()) {
		case node_type::literal_expr: return true;
		case node_type::identifier_expr: return true;
		case node_type::unary_expr: {
			const auto* unary = static_cast<const unary_expr*>(a_value);
			return (unary->op.type == token_type::minus || unary->op.type == token_type::plus) && unary->operand && unary->operand->get_type() == node_type::literal_expr;
		}
		default: return false;
	}
}

bool switch_subject_held(const switch_stmt& a_switch) {
	if (!a_switch.condition || a_switch.condition->get_type() != node_type::identifier_expr) { return true; }
	for (const auto& branch : a_switch.cases) {
		if (!inert_case_value(branch->value.get())) { return true; }
	}
	return false;
}

std::shared_ptr<coroutine_handle> coroutine_of(const script_value& a_value) {
	using namespace coroutine_snapshot_impl;
	const script_value& value = plain(a_value);
	if (!value.is_object()) { return nullptr; }
	const auto holder = value.get_object_holder();
	if (!holder || !holder->is_coroutine_handle()) { return nullptr; }
	return std::static_pointer_cast<coroutine_handle>(holder->host_object());
}

std::string coroutine_display_name(const coroutine_handle& a_handle) {
	if (const coroutine_handle::uncopied_state* uncopied = a_handle.uncopied()) { return uncopied->name; }
	std::string result;
	if (const class_instance* receiver = a_handle.receiver().peek_class_instance()) {
		if (const class_definition* owner = receiver->get_class_definition()) { result = owner->get_name() + "::"; }
	}
	if (const auto function = a_handle.get_function()) { result += function->name; }
	return result;
}

JAI_NOINLINE void throw_coroutine_not_saved(const coroutine_handle& a_handle, std::string_view a_reason) {
	throw serialization_error("Cannot save coroutine '" + coroutine_display_name(a_handle) + "': " + std::string(a_reason));
}

JAI_NOINLINE void throw_coroutine_not_loaded(std::string_view a_function, std::string_view a_reason) {
	throw serialization_error("Cannot load coroutine '" + std::string(a_function) + "': " + std::string(a_reason));
}

std::string coroutine_record_into(engine& a_engine, coroutine_handle& a_handle, coroutine_capture a_why, coroutine_record& record,
                                  std::vector<coroutine_site>* a_sites) {
	using namespace coroutine_snapshot_impl;
	if (const coroutine_handle::uncopied_state* uncopied = a_handle.uncopied()) { return "it was not copied with its object: " + uncopied->reason; }
	const std::string_view is = a_why == coroutine_capture::copy ? "was" : "is";
	const std::shared_ptr<function_decl> function = a_handle.get_function();
	if (!function || !function->body) { return "it has no function"; }
	const coroutine_handle::status status = a_handle.get_status();
	if (status == coroutine_handle::status::running) { return "it " + std::string(is) + " running"; }
	if (a_why == coroutine_capture::save) {
		coroutine_identity identity;
		if (std::string unknown = identify(a_engine, a_handle, identity); !unknown.empty()) { return unknown; }
		record.emplace_back("function", text_value(a_engine, identity.name));
		record.emplace_back("kind", text_value(a_engine, identity.kind));
		record.emplace_back("code", text_value(a_engine, identity.code));
	}
	if (status == coroutine_handle::status::created) {
		record.emplace_back("state", text_value(a_engine, "created"));
		script_value arguments = make_list(a_engine);
		for (const script_value& argument : a_handle.get_args()) { list_push(arguments, packed(a_engine, argument)); }
		record.emplace_back("args", std::move(arguments));
		return {};
	}
	if (status == coroutine_handle::status::failed) {
		record.emplace_back("state", text_value(a_engine, "failed"));
		return {};
	}
	if (status == coroutine_handle::status::completed) {
		record.emplace_back("state", text_value(a_engine, "completed"));
		auto result = a_handle.result_from_script(0);
		record.emplace_back("result", packed(a_engine, result ? result.value() : script_value(std::monostate{}, &a_engine)));
		return {};
	}

	std::vector<coroutine_site> owned;
	std::vector<coroutine_site>& sites = a_sites ? *a_sites : owned;
	sites = coroutine_sites(function->parameters, *function->body);
	execution_backend* backend = engine_access::get_execution_backend(a_engine);
	uint32_t at = UINT32_MAX;
	coroutine_frame_view frame;
	if (!backend || !backend->paused_coroutine_frame(a_handle, sites, at, frame) || at >= sites.size()) {
		return "its paused frame cannot be read";
	}
	const coroutine_site& site = sites[at];
	if (site.refusal != coroutine_refusal::none) { return refusal_text(site.refusal, a_why); }
	if (frame.loops.size() != site.loops.size() || frame.subjects.size() != site.switches.size()) {
		return "its paused frame cannot be read";
	}
	const auto unnamed = [&]() { return std::string(is) + " a reference the " + std::string(capture_noun(a_why)) + " cannot name"; };
	const auto slot_value = [&](size_t a_slot) -> const script_value* { return a_slot < frame.slot_count ? frame.slots + a_slot : nullptr; };
	const auto local_reason = [&](const coroutine_local& a_local, std::string_view a_reason) {
		return "its local '" + std::string(a_local.name) + "' " + std::string(a_reason);
	};

	// a cell every holder of which is a slot of this frame is the frame's own
	const auto frame_holders = [&](const script_value& a_cell) {
		size_t count = 0;
		for (size_t slot = 0; slot < frame.slot_count; ++slot) {
			if (frame.slots[slot].get_reference_holder() == a_cell.get_reference_holder()) { ++count; }
		}
		return count;
	};

	record.emplace_back("state", text_value(a_engine, "suspended"));
	record.emplace_back("at", script_value(static_cast<script_int>(at), &a_engine));
	script_value locals = make_list(a_engine);
	for (size_t i = 0; i < site.locals.size(); ++i) {
		const coroutine_local& local = site.locals[i];
		const script_value* held = slot_value(local.slot);
		const script_value null(std::monostate{}, &a_engine);
		if (!held) {
			list_push(locals, null);
			continue;
		}
		const auto* holder = held->get_reference_holder();
		if (local.held == coroutine_local::kind::loop_variable && holder && !holder->plain_cell()) {
			const coroutine_loop_state& loop = frame.loops[local.loop];
			const script_array* elements = array_node(loop.range);
			const script_map* entries = map_node(loop.range);
			const bool element_cursor = elements && holder->container.get() == elements && !holder->has_map_key && holder->container_index == loop.at;
			const bool entry_cursor = entries && local.binds_entry && holder->has_map_key && holder->container_map.get() == entries;
			if (!element_cursor && !entry_cursor) { return local_reason(local, unnamed()); }
			list_push(locals, null);
			continue;
		}
		if (local.held == coroutine_local::kind::reference) {
			if (!holder || !holder->plain_cell()) { return local_reason(local, unnamed()); }
			if (script_value::reference_holders(*held) > frame_holders(*held)) { return local_reason(local, unnamed()); }
			size_t target = SIZE_MAX;
			for (size_t j = 0; j < site.locals.size(); ++j) {
				const script_value* other = slot_value(site.locals[j].slot);
				if (j != i && site.locals[j].held != coroutine_local::kind::reference && other && other->get_reference_holder() == holder) { target = j; break; }
			}
			if (target != SIZE_MAX) {
				list_push(locals, script_value(static_cast<script_int>(target), &a_engine));
			} else {
				script_value owned = make_list(a_engine);
				list_push(owned, packed(a_engine, *held));
				list_push(locals, std::move(owned));
			}
			continue;
		}
		if (holder) {
			if (!holder->plain_cell()) { return local_reason(local, unnamed()); }
			if (script_value::reference_holders(*held) > frame_holders(*held)) { return local_reason(local, std::string(is) + " shared with a function value"); }
		}
		const script_value& value = plain(*held);
		if (value.is_function() && a_why == coroutine_capture::save) { return local_reason(local, "holds a function value"); }
		if (local.held == coroutine_local::kind::loop_variable && local.by_reference && map_node(frame.loops[local.loop].range)) {
			return local_reason(local, std::string(is) + " a by-reference loop variable over a map");
		}
		list_push(locals, declares_type(local.type) ? value : packed(a_engine, value));
	}
	record.emplace_back("locals", std::move(locals));

	script_value loops = make_list(a_engine);
	for (size_t k = 0; k < site.loops.size(); ++k) {
		const range_for_stmt& loop = *site.loops[k];
		const coroutine_loop_state& state = frame.loops[k];
		script_value entry = make_record(a_engine);
		const script_value& range = plain(state.range);
		if (coroutine_of(range)) {
			record_set(a_engine, entry, "range", range);
		} else if (range.is_string() || range.is_utf8_string()) {
			record_set(a_engine, entry, "range", range);
			record_set(a_engine, entry, "at", script_value(static_cast<script_int>(state.at), &a_engine));
		} else if (range.is_array() || range.is_map()) {
			// the storage the loop iterates: a local's, a field's of the receiver, a program variable's, else the loop's own
			size_t local_owner = SIZE_MAX;
			const identifier_expr* named = loop.container && loop.container->get_type() == node_type::identifier_expr ? static_cast<const identifier_expr*>(loop.container.get()) : nullptr;
			const auto owns = [&](size_t a_index) {
				const coroutine_local& candidate = site.locals[a_index];
				const script_value* held = slot_value(candidate.slot);
				return (candidate.held != coroutine_local::kind::loop_variable || candidate.loop < k) && held && same_container(*held, range);
			};
			for (size_t j = 0; named && j < site.locals.size(); ++j) {
				if (site.locals[j].slot == named->slot_index && owns(j)) { local_owner = j; break; }
			}
			for (size_t j = 0; local_owner == SIZE_MAX && j < site.locals.size(); ++j) {
				if (owns(j)) { local_owner = j; }
			}
			std::string field_owner;
			if (local_owner == SIZE_MAX) {
				if (const class_instance* receiver = a_handle.receiver().peek_class_instance()) {
					string_symbolizer* symbols = engine_access::get_symbolizer(a_engine);
					receiver->for_each_field([&](uint64_t a_field, const script_value& a_held) {
						if (field_owner.empty() && same_container(a_held, range)) { field_owner = std::string(symbols->get_string(a_field)); }
					});
				}
			}
			std::string global_owner;
			if (local_owner == SIZE_MAX && field_owner.empty() && named && named->slot_index == SIZE_MAX) {
				const uint64_t symbol = named->symbol_id != UINT64_MAX ? named->symbol_id : engine_access::symbolize(a_engine, named->name);
				const script_value* cell = engine_access::get_global_environment(a_engine)->get_value_ptr(symbol);
				if (cell && same_container(*cell, range)) { global_owner = std::string(named->name); }
			}
			if (local_owner != SIZE_MAX) {
				record_set(a_engine, entry, "local", script_value(static_cast<script_int>(local_owner), &a_engine));
			} else if (!field_owner.empty()) {
				record_set(a_engine, entry, "field", text_value(a_engine, field_owner));
			} else if (!global_owner.empty()) {
				record_set(a_engine, entry, "global", text_value(a_engine, global_owner));
			} else {
				// the backend's own handle, this view's copy and a by-reference variable's cursor hold a range nothing else iterates
				size_t expected = 2;
				for (const coroutine_local& local : site.locals) {
					const script_value* held = slot_value(local.slot);
					if (local.held == coroutine_local::kind::loop_variable && local.loop == k && held && held->get_reference_holder()) { ++expected; }
				}
				if (container_holders(range) > expected) {
					return "its range-for over '" + std::string(loop.variable_name) + "' walks storage the " + std::string(capture_noun(a_why)) + " cannot name";
				}
				record_set(a_engine, entry, "range", packed(a_engine, range));
			}
			if (range.is_array()) {
				record_set(a_engine, entry, "at", script_value(static_cast<script_int>(state.at), &a_engine));
			} else {
				record_set(a_engine, entry, "key", packed(a_engine, state.key));
			}
		} else {
			return "its paused frame cannot be read";
		}
		list_push(loops, std::move(entry));
	}
	record.emplace_back("loops", std::move(loops));

	script_value subjects = make_list(a_engine);
	for (size_t k = 0; k < site.switches.size(); ++k) {
		if (switch_subject_held(*site.switches[k])) { list_push(subjects, packed(a_engine, frame.subjects[k])); }
	}
	record.emplace_back("switches", std::move(subjects));
	return {};
}

coroutine_record coroutine_record_of(engine& a_engine, coroutine_handle& a_handle) {
	coroutine_record record;
	if (const std::string refused = coroutine_record_into(a_engine, a_handle, coroutine_capture::save, record, nullptr); !refused.empty()) {
		throw_coroutine_not_saved(a_handle, refused);
	}
	return record;
}

script_value make_unloaded_coroutine(engine& a_engine) {
	auto handle = std::make_shared<coroutine_handle>(&a_engine);
	handle->set_status(coroutine_handle::status::failed);
	return script_value::make_coroutine_handle(engine_access::symbolize(a_engine, "coroutine"), std::static_pointer_cast<void>(handle), &a_engine);
}

script_value coroutine_record_map(engine& a_engine, coroutine_record a_record) {
	using namespace coroutine_snapshot_impl;
	script_value result = make_record(a_engine);
	for (auto& [key, value] : a_record) { record_set(a_engine, result, key, std::move(value)); }
	return result;
}

namespace coroutine_snapshot_impl {

// The frame a record describes, once its function is known: read and checked whole before the handle changes
void rebuild_from_record(engine& a_engine, coroutine_handle& a_handle, const script_map& a_record, record_reader& reader,
                         std::shared_ptr<function_decl> declaration, std::shared_ptr<environment> closure, script_value receiver,
                         std::vector<coroutine_site> sites);

} // namespace coroutine_snapshot_impl

bool copy_coroutine_record(engine& a_engine, const script_value& a_copy, const script_map& a_record, const coroutine_handle& a_source,
                           const script_value& a_receiver, std::vector<coroutine_site> a_sites) {
	using namespace coroutine_snapshot_impl;
	const std::shared_ptr<coroutine_handle> handle = coroutine_of(a_copy);
	const std::shared_ptr<environment>& scope = a_source.get_closure_env();
	if (!handle || !scope || !scope->is_method_env()) { return false; }
	string_symbolizer* symbols = engine_access::get_symbolizer(a_engine);
	auto closure = method_coroutine_scope(scope->get_parent(), symbols, symbols->get_this_id(), a_receiver, nullptr, scope->get_access_context());
	record_reader reader{a_engine, coroutine_display_name(a_source), coroutine_capture::copy};
	try {
		rebuild_from_record(a_engine, *handle, a_record, reader, a_source.get_function(), std::move(closure), a_receiver, std::move(a_sites));
	} catch (const copy_not_rebuilt&) {
		return false;
	}
	return true;
}

void load_coroutine_record(engine& a_engine, const script_value& a_handle, const script_map& a_record, const script_value& a_receiver) {
	using namespace coroutine_snapshot_impl;
	const std::shared_ptr<coroutine_handle> handle = coroutine_of(a_handle);
	record_reader reader{a_engine, "?"};
	if (!handle) { reader.fail("its save is malformed"); }
	const script_value* function = record_member(a_record, a_engine, "function");
	if (function && function->is_string()) { reader.function = function->as_string(); }
	const script_value& kind = reader.member(a_record, "kind");
	const script_value& code = reader.member(a_record, "code");
	if (!function || !function->is_string() || !kind.is_string() || !code.is_string()) { reader.fail("its save is malformed"); }

	coroutine_origin origin;
	bool named = false;
	if (!find_origin(a_engine, kind.as_string(), reader.function, code.as_string(), a_receiver, origin, named)) {
		reader.fail(named ? "the program's function differs from the one that was saved" : "the program has no such coroutine function");
	}
	std::shared_ptr<function_decl> declaration = origin.declaration;
	std::shared_ptr<environment> closure = origin.closure;
	script_value receiver(std::monostate{}, &a_engine);
	if (const std::shared_ptr<coroutine_handle> minted = coroutine_of(origin.minted)) {
		declaration = minted->get_function();
		closure = minted->get_closure_env();
		receiver = minted->receiver();
	}
	if (!declaration || !declaration->body) { reader.fail("the program has no such coroutine function"); }
	std::vector<coroutine_site> sites;
	if (const script_value* state = record_member(a_record, a_engine, "state"); state && state->is_string() && state->as_string() == "suspended") {
		sites = coroutine_sites(declaration->parameters, *declaration->body);
	}
	rebuild_from_record(a_engine, *handle, a_record, reader, std::move(declaration), std::move(closure), std::move(receiver), std::move(sites));
}

void coroutine_snapshot_impl::rebuild_from_record(engine& a_engine, coroutine_handle& a_handle, const script_map& a_record, record_reader& reader,
                                                  std::shared_ptr<function_decl> declaration, std::shared_ptr<environment> closure, script_value receiver,
                                                  std::vector<coroutine_site> sites) {
	coroutine_handle* const handle = &a_handle;
	const bool functions_refused = reader.why == coroutine_capture::save;
	const script_value& state = reader.member(a_record, "state");
	if (!state.is_string()) { reader.fail("its save is malformed"); }

	const std::string& phase = state.as_string();
	std::vector<script_value> arguments;
	if (phase == "created") {
		for (const script_value& argument : reader.list(a_record, "args")) { arguments.push_back(reader.unpacked(argument)); }
	}
	if (phase != "created" && phase != "suspended" && phase != "completed" && phase != "failed") { reader.fail("its save is malformed"); }

	// everything the record holds is read and checked before the handle changes: a refused load leaves it as it was made
	script_value result(std::monostate{}, &a_engine);
	if (phase == "completed") { result = reader.unpacked(reader.member(a_record, "result")); }
	coroutine_frame frame;
	script_int at = 0;
	if (phase == "suspended") {
		const script_value& saved_at = reader.member(a_record, "at");
		if (!saved_at.is_int() || saved_at.as_int() < 0 || static_cast<size_t>(saved_at.as_int()) >= sites.size()) { reader.fail("its save names no yield of the function"); }
		at = saved_at.as_int();
		const coroutine_site& site = sites[static_cast<size_t>(at)];
		if (site.refusal != coroutine_refusal::none) { reader.fail("its save names a yield no coroutine is saved at"); }
		const std::vector<script_value> locals = reader.list(a_record, "locals");
		const std::vector<script_value> loops = reader.list(a_record, "loops");
		const std::vector<script_value> subjects = reader.list(a_record, "switches");
		if (locals.size() != site.locals.size() || loops.size() != site.loops.size()) { reader.fail("its save does not fit the function's locals"); }
		size_t live = 0;   // the frame's slots end with its last local in scope, as the declarations that ran left them
		for (const coroutine_local& local : site.locals) { live = std::max(live, local.slot + 1); }
		if (live > declaration->local_count) { reader.fail("its save does not fit the function's locals"); }
		frame.slots.assign(live, script_value(std::monostate{}, &a_engine));
		const auto slot_of = [&](const coroutine_local& a_local) -> script_value& {
			if (a_local.slot >= frame.slots.size()) { reader.fail("its save does not fit the function's locals"); }
			return frame.slots[a_local.slot];
		};

		for (size_t i = 0; i < site.locals.size(); ++i) {
			const coroutine_local& local = site.locals[i];
			if (local.held != coroutine_local::kind::value) { continue; }
			script_value value = reader.typed(locals[i], local);
			if ((functions_refused && value.is_function()) || coroutine_of(value).get() == handle) { reader.fail("its save holds a value its local '" + std::string(local.name) + "' cannot take"); }
			slot_of(local) = local.boxed ? script_value::make_cell_reference(std::move(value), &a_engine) : std::move(value);
		}
		for (size_t i = 0; i < site.locals.size(); ++i) {
			const coroutine_local& local = site.locals[i];
			if (local.held != coroutine_local::kind::reference) { continue; }
			const script_value& saved = locals[i];
			if (saved.is_int()) {
				const script_int target = saved.as_int();
				if (target < 0 || static_cast<size_t>(target) >= site.locals.size() || site.locals[static_cast<size_t>(target)].held == coroutine_local::kind::reference) {
					reader.fail("its save does not fit the function's locals");
				}
				script_value& storage = slot_of(site.locals[static_cast<size_t>(target)]);
				if (!storage.is_reference()) {
					script_value inner = std::move(storage);
					storage = script_value::make_cell_reference(std::move(inner), &a_engine);
				}
				slot_of(local) = script_value(storage);
			} else if (saved.is_array() && saved.unchecked_array_node()->size() == 1) {
				slot_of(local) = script_value::make_cell_reference(reader.unpacked(saved.unchecked_array_node()->get(0, &a_engine)), &a_engine);
			} else {
				reader.fail("its save does not fit the function's locals");
			}
		}

		frame.loops.resize(site.loops.size());
		// a loop's variable once its range is known: the cursor a by-reference variable is, else its saved value
		const auto place_loop_variable = [&](size_t a_loop) {
		for (size_t i = 0; i < site.locals.size(); ++i) {
			const coroutine_local& local = site.locals[i];
			if (local.held != coroutine_local::kind::loop_variable || local.loop != a_loop) { continue; }
			const coroutine_loop_state& loop = frame.loops[local.loop];
			const script_value& saved = locals[i];
			const script_value& range = loop.range;
			if (saved.is_null() && (local.by_reference || local.binds_entry) && range.is_array()) {
				type_info_ptr constraint = nullptr;
				if (auto tag = range.get_type_info(); tag && tag->base_type == script_value_type::jai_array_type) {
					type_info_ptr element = tag->element_type();
					if (element && element->base_type != script_value_type::jai_any_type && element->base_type != script_value_type::jai_null_type) { constraint = element; }
				}
				slot_of(local) = script_value::make_element_reference(const_cast<script_value&>(range).get_array_storage(), loop.at, &a_engine, constraint);
				script_value::bind_view(slot_of(local));
				continue;
			}
			if (saved.is_null() && local.binds_entry && range.is_map()) {
				auto& entries = const_cast<script_value&>(range).get_map_storage();
				auto found = entries->find(loop.key);
				if (found == entries->end()) { reader.fail("its save holds a loop at an entry its map does not have"); }
				const type_info* tag = range.get_type_info();
				slot_of(local) = script_value::make_map_entry_reference(entries, found->first, &found->second, &a_engine, tag ? tag->value_type() : nullptr);
				script_value::bind_view(slot_of(local));
				continue;
			}
			if (local.by_reference && !coroutine_of(range)) { reader.fail("its save does not fit the function's locals"); }
			script_value value = reader.typed(saved, local);
			if ((functions_refused && value.is_function()) || coroutine_of(value).get() == handle) { reader.fail("its save holds a value its local '" + std::string(local.name) + "' cannot take"); }
			slot_of(local) = local.boxed ? script_value::make_cell_reference(std::move(value), &a_engine) : std::move(value);
		}
		};

		for (size_t k = 0; k < site.loops.size(); ++k) {
			if (!loops[k].is_map()) { reader.fail("its save is malformed"); }
			const script_map& entry = loops[k].as_map();
			coroutine_loop_state& loop = frame.loops[k];
			script_value range(std::monostate{}, &a_engine);
			if (const script_value* saved = record_member(entry, a_engine, "range")) {
				range = coroutine_of(*saved) || saved->is_string() || saved->is_utf8_string() ? *saved : reader.unpacked(*saved);
			} else if (const script_value* owner = record_member(entry, a_engine, "local")) {
				// a local declared ahead of the loop: an outer loop's variable is placed by now, this loop's and an inner one's are not
				const coroutine_local* owning = owner->is_int() && owner->as_int() >= 0 && static_cast<size_t>(owner->as_int()) < site.locals.size() ? &site.locals[static_cast<size_t>(owner->as_int())] : nullptr;
				if (!owning || (owning->held == coroutine_local::kind::loop_variable && owning->loop >= k)) { reader.fail("its save does not fit the function's locals"); }
				range = plain(slot_of(*owning));
			} else if (const script_value* field = record_member(entry, a_engine, "field")) {
				const class_instance* instance = receiver.peek_class_instance();
				const script_value* held = instance && field->is_string() ? instance->find_script_field_value(engine_access::symbolize(a_engine, field->as_string())) : nullptr;
				if (!held) { reader.fail("its receiver has no field '" + (field->is_string() ? field->as_string() : std::string()) + "' for its loop to walk"); }
				range = plain(*held);
			} else if (const script_value* global = record_member(entry, a_engine, "global")) {
				const script_value* cell = global->is_string() ? engine_access::get_global_environment(a_engine)->get_value_ptr(engine_access::symbolize(a_engine, global->as_string())) : nullptr;
				if (!cell) { reader.fail("the program has no variable '" + (global->is_string() ? global->as_string() : std::string()) + "' for its loop to walk"); }
				range = plain(*cell);
			} else {
				reader.fail("its save is malformed");
			}
			const bool walks_coroutine = coroutine_of(range) != nullptr;
			if (walks_coroutine && coroutine_of(range).get() == handle) { reader.fail("its save is malformed"); }
			if (!walks_coroutine && !range.is_array() && !range.is_map() && !range.is_string() && !range.is_utf8_string()) { reader.fail("its save holds a loop over a value no loop walks"); }
			if (range.is_array() || range.is_string() || range.is_utf8_string()) {
				const script_value* position = record_member(entry, a_engine, "at");
				if (!position || !position->is_int() || position->as_int() < 0) { reader.fail("its save is malformed"); }
				loop.at = static_cast<size_t>(position->as_int());
				if (range.is_array() && loop.at >= range.unchecked_array_node()->size()) { reader.fail("its save holds a loop past the end of what it walks"); }
			} else if (range.is_map()) {
				loop.key = reader.unpacked(reader.member(entry, "key"));
			}
			loop.range = std::move(range);
			place_loop_variable(k);
		}

		frame.subjects.assign(site.switches.size(), script_value(std::monostate{}, &a_engine));
		size_t held = 0;
		for (size_t k = 0; k < site.switches.size(); ++k) {
			if (!switch_subject_held(*site.switches[k])) { continue; }
			if (held >= subjects.size()) { reader.fail("its save does not fit the function's locals"); }
			frame.subjects[k] = reader.unpacked(subjects[held++]);
		}
		if (held != subjects.size()) { reader.fail("its save does not fit the function's locals"); }
	}

	handle->set_function(declaration, std::move(arguments), closure);
	handle->set_receiver(std::move(receiver));
	if (phase == "created") {
		handle->set_status(coroutine_handle::status::created);
	} else if (phase == "completed") {
		handle->complete(result);
	} else if (phase == "failed") {
		handle->set_status(coroutine_handle::status::failed);
	} else {
		execution_backend* backend = engine_access::get_execution_backend(a_engine);
		handle->set_status(coroutine_handle::status::created);
		struct unpaused { coroutine_handle& handle; bool armed = true; ~unpaused() { if (armed) { handle.set_status(coroutine_handle::status::failed); } } } guard{*handle};
		if (!backend || !backend->pause_coroutine_at(*handle, sites, static_cast<uint32_t>(at), std::move(frame))) { reader.fail("its frame cannot be rebuilt"); }
		guard.armed = false;
	}
}

} // namespace jai::detail
