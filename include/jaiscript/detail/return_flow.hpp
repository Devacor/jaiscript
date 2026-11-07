#pragma once

#ifndef __JAISCRIPT_DETAIL_RETURN_FLOW_HPP__
#define __JAISCRIPT_DETAIL_RETURN_FLOW_HPP__

// How a function body completes, for the checker's required-return rule and the vm's graft eligibility: a load-time AST
// walk that adds nothing to a call or a return. A break or continue belongs to its enclosing loop or switch; a literal
// condition decides its branch the way the runtime's truthiness does.

#include <jaiscript/detail/ast.hpp>
#include <optional>

namespace jai::detail {

	// A declared return type that holds no null: falling off the end, or a bare return, leaves it without a value
	inline bool requires_return_value(const type_info* t) {
		if (!t || t->type_name.empty() || t->type_name == "auto") { return false; }
		switch (t->base_type) {
			case script_value_type::jai_int_type: case script_value_type::jai_float_type:
			case script_value_type::jai_bool_type: case script_value_type::jai_char_type:
			case script_value_type::jai_string_type: case script_value_type::jai_utf8_string_type:
			case script_value_type::jai_reference_type:
				return true;
			default:
				return false;
		}
	}

	class return_flow {
	public:
		// maybe_normal: reaches the end only through a catch of an exception the analysis cannot prove is raised
		enum : uint8_t { normal = 1, returns = 2, throws = 4, breaks = 8, continues = 16, falls_through = 32, bare_return = 64, maybe_normal = 128 };
		struct result {
			uint8_t paths = normal;
			bool may_throw = false;
		};

		explicit return_flow(const type_info* return_type = nullptr) : return_type_(return_type) {}

		template <typename Statements>
		result sequence(const Statements& statements) const {
			result r;
			for (const auto& s : statements) {
				if (!(r.paths & (normal | maybe_normal))) { break; }
				result next = analyze(s.get());
				const bool definite = (r.paths & normal) != 0;
				r.paths = static_cast<uint8_t>((r.paths & ~(normal | maybe_normal)) | (definite ? next.paths : uncertain(next.paths)));
				r.may_throw = r.may_throw || next.may_throw;
			}
			return r;
		}

		result analyze(const statement* s) const {
			if (!s) { return {}; }
			switch (s->get_type()) {
				case node_type::return_stmt: {
					const auto* ret = static_cast<const return_stmt*>(s);
					if (!ret->value) { return {static_cast<uint8_t>(returns | bare_return), requires_return_value(return_type_)}; }
					bool converts_without_error = false;
					if (!ret->binds_reference && ret->value->get_type() == node_type::literal_expr) {
						const auto kind = static_cast<const literal_expr*>(ret->value.get())->value.current_type();
						converts_without_error = (kind == script_value_type::jai_int_type || kind == script_value_type::jai_float_type ||
							kind == script_value_type::jai_bool_type || kind == script_value_type::jai_char_type) &&
							(!return_type_ || return_type_->base_type == kind);
					}
					return {returns, !converts_without_error};
				}
				case node_type::break_stmt: return {breaks, false};
				case node_type::continue_stmt: return {continues, false};
				case node_type::fallthrough_stmt: return {falls_through, false};
				case node_type::block_stmt: return sequence(static_cast<const block_stmt*>(s)->declarations);
				case node_type::statement_decl: return analyze(static_cast<const statement_decl*>(s)->statement.get());
				case node_type::expression_stmt:
				case node_type::expression_decl: {
					const expression* e = s->get_type() == node_type::expression_stmt ? static_cast<const expression_stmt*>(s)->expression.get()
					                                                                   : static_cast<const expression_decl*>(s)->expression.get();
					if (e && e->get_type() == node_type::throw_expr) { return {throws, true}; }
					return {normal, e && e->get_type() != node_type::literal_expr};
				}
				case node_type::if_stmt: {
					const auto* branch = static_cast<const if_stmt*>(s);
					if (auto taken = literal_truth(branch->condition.get())) {
						return analyze(*taken ? branch->then_statement.get() : branch->else_statement.get());
					}
					result then_r = analyze(branch->then_statement.get());
					result else_r = analyze(branch->else_statement.get());
					return {static_cast<uint8_t>(then_r.paths | else_r.paths), true};
				}
				case node_type::while_stmt: {
					const auto* loop = static_cast<const while_stmt*>(s);
					return loop_result(loop->condition.get(), analyze(loop->body.get()));
				}
				case node_type::for_stmt: {
					const auto* loop = static_cast<const for_stmt*>(s);
					result r = analyze(loop->initializer.get());
					if (!(r.paths & (normal | maybe_normal))) { return r; }
					result body = loop_result(loop->condition.get(), analyze(loop->body.get()));
					r.paths = static_cast<uint8_t>((r.paths & ~(normal | maybe_normal)) | body.paths);
					r.may_throw = r.may_throw || body.may_throw || loop->update != nullptr;
					return r;
				}
				case node_type::range_for_stmt: {
					result body = analyze(static_cast<const range_for_stmt*>(s)->body.get());
					body.paths = static_cast<uint8_t>(normal | (body.paths & (returns | throws | bare_return)));
					body.may_throw = true;
					return body;
				}
				case node_type::try_stmt: {
					const auto* attempt = static_cast<const try_stmt*>(s);
					result r = analyze(attempt->try_block.get());
					if (r.may_throw || (r.paths & throws)) {
						result caught = analyze(attempt->catch_block.get());
						r.paths = static_cast<uint8_t>((r.paths & ~throws) | ((r.paths & throws) ? caught.paths : uncertain(caught.paths)));
						r.may_throw = caught.may_throw;
					}
					return r;
				}
				case node_type::switch_stmt: {
					const auto* choice = static_cast<const switch_stmt*>(s);
					result next = choice->default_case ? sequence(choice->default_case->body) : result{};
					result r = next;
					for (size_t i = choice->cases.size(); i-- > 0;) {
						result current = sequence(choice->cases[i]->body);
						uint8_t paths = current.paths;
						if ((paths & falls_through) || (choice->cases[i]->has_fallthrough && (paths & normal))) {
							paths = static_cast<uint8_t>((paths & ~(falls_through | normal)) | next.paths);
							current.may_throw = current.may_throw || next.may_throw;
						}
						if (paths & breaks) { paths = static_cast<uint8_t>((paths & ~breaks) | normal); }
						current.paths = paths;
						r.paths = static_cast<uint8_t>(r.paths | paths);
						r.may_throw = r.may_throw || current.may_throw;
						next = current;
					}
					if (r.paths & breaks) { r.paths = static_cast<uint8_t>((r.paths & ~breaks) | normal); }
					r.may_throw = true;
					return r;
				}
				default: return {normal, true};
			}
		}

	private:
		const type_info* return_type_;

		static uint8_t uncertain(uint8_t paths) {
			return static_cast<uint8_t>((paths & ~normal) | ((paths & normal) ? maybe_normal : 0));
		}

		// a char is truthy whatever its value (the runtime's is_truthy)
		static std::optional<bool> literal_truth(const expression* e) {
			if (!e) { return true; }
			if (e->get_type() != node_type::literal_expr) { return std::nullopt; }
			const auto& v = static_cast<const literal_expr*>(e)->value;
			if (v.is_bool()) { return v.unchecked_as_bool(); }
			if (v.is_int()) { return v.unchecked_as_int() != 0; }
			if (v.is_float()) { return v.unchecked_as_float() != 0.0; }
			if (v.is_char()) { return true; }
			if (v.is_string()) { return !v.unchecked_as_string().empty(); }
			if (v.is_null()) { return false; }
			return std::nullopt;
		}

		result loop_result(const expression* condition, result body) const {
			auto always = literal_truth(condition);
			if (always && !*always) { return {}; }
			uint8_t paths = static_cast<uint8_t>(body.paths & (returns | throws | bare_return));
			if (!always || (body.paths & breaks)) { paths = static_cast<uint8_t>(paths | normal); }
			return {paths, body.may_throw || !always};
		}
	};

	inline bool has_complete_return(const function_decl& fn) {
		if (!fn.body) { return false; }
		const auto r = return_flow(fn.return_type.get()).analyze(fn.body.get());
		return !(r.paths & (return_flow::normal | return_flow::maybe_normal | return_flow::bare_return));
	}

}

#endif
