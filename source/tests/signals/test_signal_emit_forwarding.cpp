#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/signals/signal_impl.hpp>
#include <jaiscript/core/engine.hpp>
#include <memory>
#include <string>
#include <utility>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

// By-value sinking receivers: forwarding the rvalue into the first would leave later ones a moved-from shell.
class signal_emit_forwarding_tests : public suite {
public:
	signal_emit_forwarding_tests() : suite("Signal Emit Forwarding") {}

	void forge_tests() override {
		test("emit_rvalue_string_reaches_both_sinking_receivers", [this]() {
			signal_emitter<void(std::string)> sig;
			std::string first, second;
			auto r1 = sig.connect([&](std::string s) { first = std::move(s); });
			auto r2 = sig.connect([&](std::string s) { second = std::move(s); });
			sig.emit(std::string("the whole text"));
			check_eq(std::string("the whole text"), first, "first receiver sees the full text");
			check_eq(std::string("the whole text"), second, "second receiver sees the full text");
		});

		test("emit_while_rvalue_string_reaches_both_predicates", [this]() {
			signal_emitter<bool(std::string)> sig;
			std::string first, second;
			auto r1 = sig.connect([&](std::string s) { first = std::move(s); return true; });
			auto r2 = sig.connect([&](std::string s) { second = std::move(s); return true; });
			check_true(sig.emit_while(std::string("keep going")), "no predicate short-circuited");
			check_eq(std::string("keep going"), first);
			check_eq(std::string("keep going"), second);
		});

		test("call_operator_rvalue_shared_ptr_reaches_both_receivers", [this]() {
			signal_emitter<void(std::shared_ptr<int>)> sig;
			std::shared_ptr<int> first, second;
			auto r1 = sig.connect([&](std::shared_ptr<int> p) { first = std::move(p); });
			auto r2 = sig.connect([&](std::shared_ptr<int> p) { second = std::move(p); });
			sig(std::make_shared<int>(7));
			check_true(first && *first == 7, "first receiver holds the pointer");
			check_true(second && *second == 7, "second receiver holds the pointer, not null");
		});

		test("emit_rvalue_string_reaches_text_receiver_after_sinking_cpp_receiver", [this]() {
			auto eng = make_engine();
			eng->execute("var g_seen = \"\";");
			signal_emitter<void(std::string)> sig;
			sig.script_engine(eng.get());
			sig.parameter_names({"s"});
			std::string sunk;
			auto r1 = sig.connect([&](std::string s) { sunk = std::move(s); });
			auto r2 = sig.connect("g_seen = s;");
			sig.emit(std::string("script sees it too"));
			check_eq(std::string("script sees it too"), sunk);
			check_eq(std::string("script sees it too"), eng->execute("g_seen").as<std::string>(), "text receiver after a by-value C++ sink still gets the full text");
		});
	}
};

FOUNDRY_REGISTER(jai::foundry::tests::signal_emit_forwarding_tests);

} // namespace jai::foundry::tests
