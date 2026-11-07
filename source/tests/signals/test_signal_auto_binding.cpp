// Signal auto-binding: declaring a JAI_SIGNAL_PROPERTY (or exposing a plain
// signal member via .property) is the whole registration — script connects
// through the generic "Signal" view with no bind_signal_type ceremony, and
// typed per-signature bindings keep precedence when present.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/signals/signal_binding.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

using namespace jai;
using namespace jai::foundry;

namespace jai::foundry::tests {

class scoreboard : public jai::property_owner<scoreboard> {
public:
	typedef void ScoreSignature(int);

	JAI_SIGNAL_PROPERTY(ScoreSignature, on_scored);

	jai::signal_emitter<void(int)> onTickedSignal;   // plain pair, the Clickable onEnabled shape
	jai::signal<void(int)> onTicked{ onTickedSignal };

	void score(int v) { on_scoredSignal(v); }
	void tick(int v) { onTickedSignal(v); }
};

// A button shape: the signal lives on the BASE (a clickable base declares onAccept) and
// script connects through a DERIVED registration (base_class + auto_bind).
class base_widget : public jai::property_owner<base_widget> {
public:
	typedef void PressSignature(int);
	JAI_SIGNAL_PROPERTY(PressSignature, on_press);
	void press(int v) { on_pressSignal(v); }
};

class derived_widget : public jai::property_owner<derived_widget, base_widget> {
public:
	derived_widget() = default;
};

// A host whose naming convention is not nameSignal: the emitter is named explicitly.
class door : public jai::property_owner<door> {
public:
	typedef void OpenSignature(int);
	JAI_SIGNAL_PROPERTY_AS(OpenSignature, on_open, open_emitter);
	void open(int v) { open_emitter(v); }
	void add_text_receiver(const std::string& id, const std::string& script) { open_emitter.connect(id, script); }
	bool has_receiver(const std::string& id) { return open_emitter.connected(id); }
};

// A signal other threads connect to: the synchronized policy, declared the one-declaration way.
class relay : public jai::property_owner<relay> {
public:
	typedef void PingSignature(int);
	JAI_SYNCHRONIZED_SIGNAL_PROPERTY(PingSignature, on_ping);

	jai::signal_emitter<void(int), jai::signal_synchronized> onPulseSignal;
	jai::signal<void(int), jai::signal_synchronized> onPulse{ onPulseSignal };

	void ping(int v) { on_pingSignal(v); }
	void pulse(int v) { onPulseSignal(v); }
	void add_text_receiver(const std::string& id, const std::string& script) { on_pingSignal.connect(id, script); }
	bool has_receiver(const std::string& id) { return on_pingSignal.connected(id); }
};

class signal_auto_binding_tests : public suite {
public:
	signal_auto_binding_tests() : suite("Signal Auto Binding") {}

	static std::shared_ptr<engine> make_bound_engine() {
		auto eng = jai::foundry::make_engine();
		dynamic_binder<scoreboard>(*eng, "Scoreboard")
			.constructor<>()
			.method("score", &scoreboard::score)
			.method("tick", &scoreboard::tick)
			.property("onTicked", &scoreboard::onTicked)
			.auto_bind()
			.build();
		return eng;
	}

	void forge_tests() override {
		test("signal_property_as_names_its_emitter", [this]() {
			auto eng = jai::foundry::make_engine();
			dynamic_binder<door>(*eng, "Door")
				.constructor<>()
				.method("open", &door::open)
				.auto_bind()
				.build();
			eng->execute("var got = -1; var d = new Door();");
			eng->execute("d.on_open.connect(\"t\", [](var v) { got = v; });");
			eng->execute("d.open(12);");
			check_eq((int64_t)12, eng->execute("got").as_int());
		});

		test("signal_property_as_serializes_under_its_name", [this]() {
			auto eng = jai::foundry::make_engine();
			int64_t fired = 0;
			eng->add_function("mark", [counter = &fired](int v) { *counter += v; });
			auto original = std::make_shared<door>();
			original->add_text_receiver("bump", "mark(arg_0);");
			std::string json = jai::to_json(*eng, original);
			check_true(json.find("\"on_open\"") != std::string::npos, "the property key is the signal's name: " + json);
			auto restored = jai::from_json<std::shared_ptr<door>>(*eng, json);
			check(restored != nullptr, "loaded not null");
			check_true(restored->has_receiver("bump"), "text receiver persisted");
			restored->open(4);
			check_eq((int64_t)4, fired, "persisted text receiver runs through the named emitter");
		});

		test("signal_property_connects_via_auto_bind_only", [this]() {
			auto eng = make_bound_engine();
			eng->execute("var got = -1; var s = new Scoreboard();");
			eng->execute("s.on_scored.connect(\"t\", [](var v) { got = v; });");
			eng->execute("s.score(41);");
			check_eq((int64_t)41, eng->execute("got").as_int());
		});

		test("signal_view_reports_and_drops_connections", [this]() {
			auto eng = make_bound_engine();
			eng->execute("var got = 0; var s = new Scoreboard();");
			eng->execute("s.on_scored.connect(\"t\", [](var v) { got = got + v; });");
			check_eq(true, eng->execute("s.on_scored.connected(\"t\")").as<bool>());
			eng->execute("s.score(5);");
			eng->execute("s.on_scored.disconnect(\"t\");");
			check_eq(false, eng->execute("s.on_scored.connected(\"t\")").as<bool>());
			eng->execute("s.score(5);");
			check_eq((int64_t)5, eng->execute("got").as_int());
		});

		test("plain_signal_member_binds_without_ceremony", [this]() {
			auto eng = make_bound_engine();
			eng->execute("var got = -1; var s = new Scoreboard();");
			eng->execute("s.onTicked.connect(\"t\", [](var v) { got = v; });");
			eng->execute("s.tick(7);");
			check_eq((int64_t)7, eng->execute("got").as_int());
		});

		test("typed_bind_signal_type_keeps_precedence", [this]() {
			auto eng = jai::foundry::make_engine();
			bind_signal_type<void(int)>(*eng, "SignalInt");
			dynamic_binder<scoreboard>(*eng, "Scoreboard")
				.constructor<>()
				.method("tick", &scoreboard::tick)
				.property("onTicked", &scoreboard::onTicked)
				.build();
			eng->execute("var got = -1; var s = new Scoreboard();");
			eng->execute("s.onTicked.connect(\"t\", [](var v) { got = v; });");
			eng->execute("s.tick(9);");
			check_eq((int64_t)9, eng->execute("got").as_int());
		});

		test("view_reads_share_one_emitter", [this]() {
			auto eng = make_bound_engine();
			eng->execute("var count = 0; var s = new Scoreboard();");
			eng->execute("var a = s.on_scored; var b = s.on_scored;");
			eng->execute("a.connect(\"one\", [](var v) { count = count + 1; });");
			eng->execute("b.connect(\"two\", [](var v) { count = count + 1; });");
			eng->execute("s.score(1);");
			check_eq((int64_t)2, eng->execute("count").as_int());
		});

		test("text_receiver_fires_through_generic_view", [this]() {
			auto eng = make_bound_engine();
			eng->execute("var txtgot = 0; var s = new Scoreboard();");
			eng->execute("s.on_scored.connect(\"txt\", \"txtgot = 5;\");");
			eng->execute("s.score(1);");
			check_eq((int64_t)5, eng->execute("txtgot").as_int());
		});

		// Red-locked from an editor panel host: loadButton.onAccept threw "Object has
		// no member 'onAccept'" — onAccept is Clickable's, Button registers with
		// base_class + auto_bind. Both registration orders (static registrar order is
		// arbitrary in a host).
		test("inherited_signal_property_through_derived_registration", [this]() {
			auto bindBase = [](engine& a_engine) {
				dynamic_binder<base_widget>(a_engine, "BaseWidget")
					.constructor<>()
					.method("press", &base_widget::press)
					.auto_bind()
					.build();
			};
			auto bindDerived = [](engine& a_engine) {
				dynamic_binder<derived_widget>(a_engine, "DerivedWidget")
					.constructor<>()
					.base_class<base_widget>()
					.auto_bind()
					.build();
			};
			auto run = [&](bool a_baseFirst) {
				auto eng = jai::foundry::make_engine();
				if (a_baseFirst) { bindBase(*eng); bindDerived(*eng); }
				else { bindDerived(*eng); bindBase(*eng); }
				const char* order = a_baseFirst ? "base-first" : "derived-first";
				try {
					eng->execute("var got = -1; var w = new DerivedWidget();");
					eng->execute("w.press(3);");
					check_eq((int64_t)-1, eng->execute("got").as_int());
					eng->execute("w.on_press.connect(\"t\", [](var v) { got = v; });");
					eng->execute("w.press(4);");
					check_eq((int64_t)4, eng->execute("got").as_int());
				} catch (const std::exception& a_error) {
					throw runtime_error(std::string(order) + ": " + a_error.what());
				}
			};
			run(true);
			run(false);
		});

		test("a_script_connect_during_an_emit_joins_from_the_next_emit", [this]() {
			auto eng = make_bound_engine();
			auto r = eng->execute(R"(
				string log = "";
				var s = new Scoreboard();
				s.on_scored.connect("a", [](var v) {
					log += "a";
					if (!s.on_scored.connected("b")) { s.on_scored.connect("b", [](var w) { log += "b"; }); }
				});
				s.score(1);
				log += "|";
				s.score(2);
				log;
			)");
			check_eq(std::string("a|ab"), r.as<std::string>(), "script gets the same default as C++: a connect joins from the next emit");
		});

		test("a_synchronized_signal_property_binds_saves_and_loads", [this]() {
			auto eng = jai::foundry::make_engine();
			dynamic_binder<relay>(*eng, "Relay")
				.constructor<>()
				.method("ping", &relay::ping)
				.method("pulse", &relay::pulse)
				.property("onPulse", &relay::onPulse)
				.auto_bind()
				.build();
			eng->execute("var got = 0; var r = new Relay();");
			eng->execute("r.on_ping.connect(\"t\", [](var v) { got = got + v; });");
			eng->execute("r.onPulse.connect(\"p\", [](var v) { got = got + 100 * v; });");
			eng->execute("r.ping(3); r.pulse(1);");
			check_eq((int64_t)103, eng->execute("got").as_int());

			int64_t fired = 0;
			eng->add_function("mark", [counter = &fired](int v) { *counter += v; });
			auto original = std::make_shared<relay>();
			original->add_text_receiver("bump", "mark(arg_0);");
			auto restored = jai::from_json<std::shared_ptr<relay>>(*eng, jai::to_json(*eng, original));
			check(restored != nullptr, "loaded not null");
			check_true(restored->has_receiver("bump"), "text receiver persisted");
			restored->ping(4);
			check_eq((int64_t)4, fired);

			auto typed = jai::foundry::make_engine();
			bind_signal_type<void(int), jai::signal_synchronized>(*typed, "SyncSignalInt");
			dynamic_binder<relay>(*typed, "Relay")
				.constructor<>()
				.method("pulse", &relay::pulse)
				.property("onPulse", &relay::onPulse)
				.property("onPulseEmitter", &relay::onPulseSignal)
				.build();
			typed->execute("var got = 0; var r = new Relay();");
			typed->execute("r.onPulse.connect(\"p\", [](var v) { got = v; });");
			typed->execute("r.onPulseEmitter.emit(6);");
			check_eq((int64_t)6, typed->execute("got").as_int(), "bind_signal_type binds the synchronized pair");
		});

		// A receiver redefines the functions and the class its siblings reach, mid-emit: receivers still to run see the new
		// bodies, the instance migrates under a receiver bound to its method, and a script class deriving the emitting C++
		// object is redefined while its C++ method emits
		test("a_reload_mid_emit_reaches_the_receivers_still_to_run", [this]() {
			auto eng = make_bound_engine();
			jai::engine* raw = eng.get();
			eng->add_function("reload_now", [raw]() {
				raw->execute(R"(
					function on_b(int v) { log += "B" + to_string(v); }
					class Mon { string pad = "p"; float hp = 0.0; void hit(int v) { hp = hp - v; log += "H" + to_string(v); } }
					class Board : Scoreboard { int seen = 0; string tag = "v2"; }
					function on_a(int v) { log += "A" + to_string(v); }
				)");
			});
			jai::stdlib::register_all(*eng);
			auto r = eng->execute(R"(
				string log = "";
				function on_a(int v) { log += "a" + to_string(v); reload_now(); }
				function on_b(int v) { log += "b" + to_string(v); }
				class Mon { int hp = 10; void hit(int v) { hp = hp - v; log += "h" + to_string(v); } }
				class Board : Scoreboard { int seen = 0; }
				var mon = new Mon();
				var s = new Board();
				s.on_scored.connect("a", [](var v) { on_a(v); });
				s.on_scored.connect("b", [](var v) { on_b(v); });
				s.on_scored.connect("c", reflect::bind(mon, "hit"));
				s.on_scored.connect("d", [](var v) { s.seen = s.seen + v; });
				s.on_scored.connect("t", "log += \"t\";");
				s.score(1);
				s.score(2);
				log + " | " + to_string(mon.hp) + " " + type_of(mon.hp) + " " + mon.pad + " | " + to_string(s.seen) + " " + s.tag;
			)");
			check_eq(std::string("a1B1H1tA2B2H2t | 7.0 float p | 3 v2"), r.as<std::string>());
		});
	}
};

} // namespace jai::foundry::tests

using signal_auto_binding_tests = jai::foundry::tests::signal_auto_binding_tests;
FOUNDRY_REGISTER(signal_auto_binding_tests)
