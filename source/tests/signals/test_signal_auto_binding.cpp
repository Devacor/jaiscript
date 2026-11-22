// Signal auto-binding: declaring a JAI_SIGNAL (or exposing a plain
// signal member via .property) is the whole registration — script connects
// through the generic "signal" view with no bind_signal_type ceremony, and
// typed per-signature bindings keep precedence when present.

#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/properties.hpp>
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

	JAI_SIGNAL(ScoreSignature, on_scored);

	jai::signal_emitter<void(int)> onTickedSignal;   // plain pair, the Clickable onEnabled shape
	jai::signal<void(int)> onTicked{ onTickedSignal };

	void score(int v) { on_scoredSignal(v); }
	void tick(int v) { onTickedSignal(v); }
};

// A button shape: the signal lives on the BASE (a clickable base declares onAccept) and
// script connects through a DERIVED registration.
class base_widget : public jai::property_owner<base_widget> {
public:
	typedef void PressSignature(int);
	JAI_SIGNAL(PressSignature, on_press);
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
	JAI_SIGNAL_AS(OpenSignature, on_open, open_emitter);
	void open(int v) { open_emitter(v); }
	void add_text_receiver(const std::string& id, const std::string& script) { open_emitter.connect(id, script); }
	bool has_receiver(const std::string& id) { return open_emitter.connected(id); }
};

// A signal other threads connect to: the synchronized policy, declared the one-declaration way.
class relay : public jai::property_owner<relay> {
public:
	typedef void PingSignature(int);
	JAI_SYNCHRONIZED_SIGNAL(PingSignature, on_ping);

	jai::signal_emitter<void(int), jai::signal_synchronized> onPulseSignal;
	jai::signal<void(int), jai::signal_synchronized> onPulse{ onPulseSignal };

	void ping(int v) { on_pingSignal(v); }
	void pulse(int v) { onPulseSignal(v); }
	void add_text_receiver(const std::string& id, const std::string& script) { on_pingSignal.connect(id, script); }
	bool has_receiver(const std::string& id) { return on_pingSignal.connected(id); }
};

// A saved object whose receivers name functions, its own or another object's methods, and source
class beacon : public jai::property_owner<beacon> {
public:
	typedef void FlashSignature(int);
	JAI_SIGNAL(FlashSignature, on_flash);
	JAI_PROPERTY((int64_t), total, 0);

	void flash(int v) { on_flashSignal(v); }
	void note(int v) { total = total + v; }
	void add_text_receiver(const std::string& id, const std::string& script) { on_flashSignal.connect(id, script); }
};

class signal_auto_binding_tests : public suite {
public:
	signal_auto_binding_tests() : suite("Signal Auto Binding") {}

	static std::shared_ptr<engine> make_beacon_engine() {
		auto eng = jai::foundry::make_engine();
		jai::stdlib::register_all(*eng);
		dynamic_binder<beacon>(*eng, "Beacon")
			.constructor<>()
			.method("flash", &beacon::flash)
			.method("note", &beacon::note)
			.build();
		return eng;
	}

	static std::string error_text(const std::function<void()>& a_run) {
		try {
			a_run();
		} catch (const std::exception& e) {
			return e.what();
		}
		return "no error";
	}

	static void check_has(const std::string& a_text, const std::string& a_part, const std::string& a_label) {
		check_true(a_text.find(a_part) != std::string::npos, a_label + ": no " + a_part + " in " + a_text);
	}

	// JSON without the whitespace between its tokens, so an expectation reads on one line
	static std::string squeezed(const std::string& a_json) {
		std::string result;
		bool quoted = false;
		for (size_t i = 0; i < a_json.size(); ++i) {
			const char c = a_json[i];
			if (quoted && c == '\\' && i + 1 < a_json.size()) {
				result += c;
				result += a_json[++i];
				continue;
			}
			if (c == '"') { quoted = !quoted; }
			if (quoted || (c != ' ' && c != '\n' && c != '\r')) { result += c; }
		}
		return result;
	}

	static std::shared_ptr<engine> make_bound_engine() {
		auto eng = jai::foundry::make_engine();
		dynamic_binder<scoreboard>(*eng, "Scoreboard")
			.constructor<>()
			.method("score", &scoreboard::score)
			.method("tick", &scoreboard::tick)
			.property("onTicked", &scoreboard::onTicked)
			.build();
		return eng;
	}

	void forge_tests() override {
		test("signal_property_as_names_its_emitter", [this]() {
			auto eng = jai::foundry::make_engine();
			dynamic_binder<door>(*eng, "Door")
				.constructor<>()
				.method("open", &door::open)
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
		// no member 'onAccept'" — onAccept is Clickable's, Button registers its
		// base. Both registration orders (static registrar order is
		// arbitrary in a host).
		test("inherited_signal_property_through_derived_registration", [this]() {
			auto bindBase = [](engine& a_engine) {
				dynamic_binder<base_widget>(a_engine, "BaseWidget")
					.constructor<>()
					.method("press", &base_widget::press)
					.build();
			};
			auto bindDerived = [](engine& a_engine) {
				dynamic_binder<derived_widget>(a_engine, "DerivedWidget")
					.constructor<>()
					.base_class<base_widget>()
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
				log + " | " + to_string(mon.hp) + " " + reflect::type_of(mon.hp) + " " + mon.pad + " | " + to_string(s.seen) + " " + s.tag;
			)");
			check_eq(std::string("a1B1H1tA2B2H2t | 7.0 float p | 3 v2"), r.as<std::string>());
		});

		// A function receiver saves the name its function has in the program, and a load connects whatever the loading program
		// calls that; a lambda stays runtime-only
		test("a_function_receiver_saves_by_name_and_a_load_rearms_it", [this]() {
			auto eng = make_beacon_engine();
			eng->execute(R"(
				int got = 0;
				void on_flash(int n) { got = got + n; }
				namespace hud { void tally(int n) { got = got + 10 * n; } }
				class Panel { static void tally(int n) { got = got + 100 * n; } }
				var b = new Beacon();
				b.on_flash.connect("plain", on_flash);
				b.on_flash.connect("spaced", hud::tally);
				b.on_flash.connect("static", Panel::tally);
				b.on_flash.connect("lambda", [](int n) { got = got + 1000 * n; });
			)");
			auto original = eng->execute<std::shared_ptr<beacon>>("b");
			const std::string json = jai::to_json(*eng, original);
			check_has(squeezed(json), R"("on_flash":{"receivers":{"plain":{"function":"on_flash"},"spaced":{"function":"hud::tally"},"static":{"function":"Panel::tally"}}})", "the named functions");
			check_true(json.find("lambda") == std::string::npos, "a lambda is not saved: " + json);
			const auto from_text = jai::from_json<std::shared_ptr<beacon>>(*eng, json);
			const auto from_bytes = jai::from_binary<std::shared_ptr<beacon>>(*eng, jai::to_binary(*eng, original));
			for (const auto& copy : { from_text, from_bytes }) {
				check_eq((size_t)3, copy->on_flash.observer_count(), "the three named functions came back");
				eng->execute("got = 0;");
				copy->flash(1);
				check_eq((int64_t)111, eng->execute("got").as_int());
			}
			check_eq(json, jai::to_json(*eng, from_text), "a loaded function receiver saves what it loaded");
		});

		// A method receiver saves its object as an archive reference: the object the payload holds before the signal (the owner
		// itself) or after it, and the load connects that object's method
		test("a_method_receiver_saves_its_object_by_reference", [this]() {
			auto eng = make_beacon_engine();
			eng->execute(R"(
				var a = new Beacon();
				var b = new Beacon();
				a.on_flash.connect("self", reflect::bind(a, "note"));
				a.on_flash.connect("peer", reflect::bind(b, "note"));
				b.on_flash.connect("back", reflect::bind(a, "note"));
			)");
			std::vector<std::shared_ptr<beacon>> pair { eng->execute<std::shared_ptr<beacon>>("a"), eng->execute<std::shared_ptr<beacon>>("b") };
			const std::string json = jai::to_json(*eng, pair);
			check_has(squeezed(json), R"("receivers":{"peer":{"object":{"$ref":2},"method":"note"},"self":{"object":{"$ref":1},"method":"note"}})", "the owner's own method and a later object's");
			check_has(squeezed(json), R"("$id":2,"$val":{"on_flash":{"receivers":{"back":{"object":{"$ref":1},"method":"note"}}})", "an earlier object's method");
			const auto from_text = jai::from_json<std::vector<std::shared_ptr<beacon>>>(*eng, json);
			const auto from_bytes = jai::from_binary<std::vector<std::shared_ptr<beacon>>>(*eng, jai::to_binary(*eng, pair));
			for (const auto& copy : { from_text, from_bytes }) {
				check_eq((size_t)2, copy.size());
				copy[0]->flash(3);
				copy[1]->flash(4);
				check_eq((int64_t)7, copy[0]->total.get(), "the loaded first object's method ran for its own flash and the second's");
				check_eq((int64_t)3, copy[1]->total.get(), "the loaded second object's method ran for the first's flash");
			}
			check_eq((int64_t)0, pair[0]->total.get(), "the saved objects stay untouched");
		});

		// A receiver the load cannot connect is a loud error naming it; skip_unknown_receivers loads the rest without it
		test("an_unknown_receiver_fails_the_load_unless_the_engine_skips_it", [this]() {
			auto eng = make_beacon_engine();
			eng->execute(R"(
				void on_flash(int n) { }
				var a = new Beacon();
				var outside = new Beacon();
				a.on_flash.connect("fn", on_flash);
				a.on_flash.connect("peer", reflect::bind(outside, "note"));
			)");
			auto original = eng->execute<std::shared_ptr<beacon>>("a");
			const std::string json = jai::to_json(*eng, original);
			const auto bytes = jai::to_binary(*eng, original);
			check_eq(std::string("Signal receiver 'peer' names an object the archive does not hold ({\"$ref\": 2})"),
				error_text([&]() { jai::from_json<std::shared_ptr<beacon>>(*eng, json); }));
			check_eq(std::string("Signal receiver 'peer' names an object the archive does not hold ({\"$ref\": 2})"),
				error_text([&]() { jai::from_binary<std::shared_ptr<beacon>>(*eng, bytes); }));

			auto other = make_beacon_engine();
			check_eq(std::string("Signal receiver 'fn' names no function 'on_flash' in the program"),
				error_text([&]() { jai::from_json<std::shared_ptr<beacon>>(*other, json); }));
			other->execute("int on_flash = 3;");
			check_eq(std::string("Signal receiver 'fn' names no function 'on_flash' in the program"),
				error_text([&]() { jai::from_json<std::shared_ptr<beacon>>(*other, json); }), "a variable of the name is no function");
			check_eq(std::string("Signal receiver 'peer' names no method 'mute' on 'Beacon'"),
				error_text([&]() { jai::from_json<std::vector<std::shared_ptr<beacon>>>(*eng, std::string("{\"value0\": [") +
					"{\"$id\": 2, \"$val\": {\"on_flash\": {}, \"total\": 0}}, " +
					"{\"$id\": 1, \"$val\": {\"on_flash\": {\"receivers\": {\"peer\": {\"object\": {\"$ref\": 2}, \"method\": \"mute\"}}}, \"total\": 0}}]}"); }));

			other->skip_unknown_receivers(true);
			check_true(other->skip_unknown_receivers());
			for (const auto& copy : { jai::from_json<std::shared_ptr<beacon>>(*other, json), jai::from_binary<std::shared_ptr<beacon>>(*other, bytes) }) {
				check_false(copy->on_flash.connected("fn"), "the unknown function is skipped");
				check_false(copy->on_flash.connected("peer"), "the object outside the payload is skipped");
				check_eq((size_t)0, copy->on_flash.observer_count());
			}
			eng->skip_unknown_receivers(true);
			auto kept = jai::from_json<std::shared_ptr<beacon>>(*eng, json);
			check_true(kept->on_flash.connected("fn"), "a skipping load still connects what it can");
			check_false(kept->on_flash.connected("peer"));
		});

		// connect_source runs the lambda itself and saves its body, its parameter names and its value captures; a load runs the
		// body as source with the saved captures
		test("connect_source_saves_the_lambda_body_parameters_and_captures", [this]() {
			auto eng = make_beacon_engine();
			eng->execute(R"(
				int got = 0;
				int bonus = 5;
				var b = new Beacon();
				b.on_flash.connect_source("add", [bonus, scale = 2](int n) { got = got + n * scale + bonus; });
				bonus = 50;
				b.flash(1);
			)");
			check_eq((int64_t)7, eng->execute("got").as_int(), "the lambda itself runs while connected");
			auto original = eng->execute<std::shared_ptr<beacon>>("b");
			const std::string json = jai::to_json(*eng, original);
			check_has(squeezed(json), R"("add":{"source":"{ got = got + n * scale + bonus; }","parameter_names":["n"],"captures":"{\"bonus\":5,\"scale\":2}"})", "the saved source receiver");
			const auto from_text = jai::from_json<std::shared_ptr<beacon>>(*eng, json);
			const auto from_bytes = jai::from_binary<std::shared_ptr<beacon>>(*eng, jai::to_binary(*eng, original));
			for (const auto& copy : { from_text, from_bytes }) {
				eng->execute("got = 0;");
				copy->flash(3);
				check_eq((int64_t)11, eng->execute("got").as_int(), "the loaded body runs with the saved captures, not the globals");
			}
			check_eq(json, jai::to_json(*eng, from_text), "a loaded source receiver saves what it loaded");

			signal_emitter<void(int)> host;
			host.connect_source("cpp", eng->execute("[bonus](int n) { got = n + bonus; };"));
			host.emit(1);
			check_eq((int64_t)51, eng->execute("got").as_int(), "the C++ connect_source connects the lambda");
		});

		test("connect_source_refuses_what_it_cannot_save", [this]() {
			auto eng = make_beacon_engine();
			eng->execute("int got = 0; int bonus = 1; void named(int n) { } var keep = [](int x) { }; var b = new Beacon();");
			const std::string form = "connect_source takes a lambda written [captures](parameters) { body } that captures values";
			check_has(error_text([&]() { eng->execute("b.on_flash.connect_source(\"r\", [&bonus](int n) { got = bonus; });"); }), form, "a reference capture");
			check_has(error_text([&]() { eng->execute("b.on_flash.connect_source(\"r\", [=](int n) { got = bonus; });"); }), form, "a default capture");
			check_has(error_text([&]() { eng->execute("b.on_flash.connect_source(\"r\", named);"); }), form, "a named function");
			check_has(error_text([&]() { eng->execute("b.on_flash.connect_source(\"r\", (n) => n);"); }), form, "an arrow function");
			check_has(error_text([&]() { eng->execute("b.on_flash.connect_source(\"r\", [keep](int n) { keep(n); });"); }),
				"connect_source cannot save the capture 'keep': a function", "a function capture");
			check_false(eng->execute("b.on_flash.connected(\"r\")").as<bool>(), "a refused lambda is not connected");
		});

		// jai::permissions::source_receivers off: script connects no source, and a load arms none
		test("source_receivers_turn_off_with_their_permission", [this]() {
			auto eng = make_beacon_engine();
			eng->execute("int got = 0; var b = new Beacon(); b.on_flash.connect_source(\"lambda\", [](int n) { got = n; });");
			auto original = eng->execute<std::shared_ptr<beacon>>("b");
			original->add_text_receiver("text", "got = arg_0;");
			const std::string json = jai::to_json(*eng, original);
			check_true(eng->is_enabled(jai::permissions::source_receivers), "on by default");

			auto locked = make_beacon_engine();
			locked->disable(jai::permissions::source_receivers);
			locked->execute("int got = 0; var b = new Beacon();");
			check_has(error_text([&]() { locked->execute("b.on_flash.connect_source(\"s\", [](int n) { got = n; });"); }),
				"Insufficient permission: source_receivers.", "connect_source");
			check_has(error_text([&]() { locked->execute("b.on_flash.connect(\"s\", \"got = 1;\");"); }),
				"Insufficient permission: source_receivers.", "a text connect from script");
			check_eq(std::string("Signal receiver 'lambda' is source, and the engine's source receivers are disabled"),
				error_text([&]() { jai::from_json<std::shared_ptr<beacon>>(*locked, json); }));
			locked->skip_unknown_receivers(true);
			auto loaded = jai::from_json<std::shared_ptr<beacon>>(*locked, json);
			check_eq((size_t)0, loaded->on_flash.observer_count(), "a skipping load arms no source");
			loaded->add_text_receiver("host", "got = 9;");
			loaded->flash(1);
			check_eq((int64_t)9, locked->execute("got").as_int(), "the host's own text connect stays its choice");
		});
	}
};

} // namespace jai::foundry::tests

using signal_auto_binding_tests = jai::foundry::tests::signal_auto_binding_tests;
FOUNDRY_REGISTER(signal_auto_binding_tests)
