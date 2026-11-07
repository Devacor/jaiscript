#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/jaibite.hpp>
#include <jaiscript/detail/ast_serializer.hpp>

#include "foundry/small_stack.hpp"

namespace jai::foundry::tests {

class jaibite_stack_tests : public suite {
public:
	jaibite_stack_tests() : suite("Jaibite Native Stack") {}

	struct nested_tree {
		std::vector<expression_ptr> pins;
		std::vector<declaration_ptr> declarations;

		explicit nested_tree(size_t depth) {
			source_location location;
			location.filename = "saved-depth-fixture";
			token plus;
			plus.type = token_type::plus;
			plus.lexeme = "+";
			plus.location = location;
			auto literal = [&]() {
				return std::make_shared<literal_expr>(location,
					script_value(script_value::ast_literal_tag{}, script_int(1)));
			};
			pins.push_back(literal());
			for (size_t i = 0; i < depth; ++i) {
				pins.push_back(std::make_shared<binary_expr>(location, pins.back(), plus, literal()));
			}
			declarations.push_back(std::make_shared<expression_decl>(location, pins.back(), true));
		}

		~nested_tree() {
			declarations.clear();
			while (!pins.empty()) { pins.pop_back(); }
		}
	};

	void forge_tests() override {
		// past the depth a level count once stopped at: the walks measure the stack, and 16 MB holds the tree on every build
		test("valid_deep_tree_keeps_its_bytes_on_large_stack", [this]() {
			on_small_stack([this]() {
				auto eng = make_engine();
				nested_tree tree(1500);
				// stamped checked-clean: the load is the structural reader and the proof pass, and the bite saves back the same stamp
				const auto bytes = detail::serialize_jaibite(tree.declarations,
					*detail::engine_access::get_symbolizer(*eng), eng->registration_fingerprint(), detail::jaibite_checked_clean_flags());
				{
					auto loaded = eng->jaibite_load_bytes(bytes);
					check_eq(bytes, loaded.save_bytes());
				}
				check_eq(script_int(42), eng->execute("40 + 2;").as_int());
			}, size_t(16) << 20);
		});

		test("reader_and_writer_recover_on_one_megabyte_stack", [this]() {
			for (size_t depth : {size_t(250), size_t(800), size_t(100000)}) {
				nested_tree tree(depth);
				on_small_stack([&]() {
					const std::string nesting = "jaibite: AST nesting too deep";
					auto eng = make_engine();
					bool saved = false;
					std::vector<uint8_t> bytes;
					try {
						bytes = detail::serialize_jaibite(tree.declarations, *detail::engine_access::get_symbolizer(*eng),
							eng->registration_fingerprint(), detail::jaibite_checked_clean_flags());
						saved = true;
					} catch (const jai::runtime_error& error) {
						check_eq(nesting, std::string(error.what()));
					}
					check_true(!saved || depth <= 800, "100,000 levels pass a 1 MB stack");
					if (saved) {
						try {
							auto loaded = eng->jaibite_load_bytes(bytes);
							check_eq(bytes, loaded.save_bytes());
						} catch (const jai::runtime_error& error) {
							// the reader, or the check a load runs over what it read
							const std::string text = error.what();
							check_true(text == nesting || text == "Nesting too deep for the host thread's stack", text);
						}
					}
					check_eq(script_int(42), eng->execute("40 + 2;").as_int());
				});
			}
		});
	}
};

} // namespace jai::foundry::tests

using jaibite_stack_tests = jai::foundry::tests::jaibite_stack_tests;
FOUNDRY_REGISTER(jaibite_stack_tests)
