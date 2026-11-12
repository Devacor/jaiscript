// embed_quickstart: a small host that uses JaiScript the way a game would.
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --config Release
//
//   embed_quickstart               run every section and exit 0 when all checks hold
//   embed_quickstart --debug       also listen for VS Code (DAP) on 127.0.0.1:52472
//   embed_quickstart --debug-wait  listen, then wait for Enter before running game.jai
//
// The hot-reload section writes live/rules.jai under the working directory and edits it mid-run.
// VS Code: install tools/vscode-jaiscript, then an attach configuration
//   { "type": "jaiscript", "request": "attach", "name": "Attach", "host": "127.0.0.1", "port": 52472 }

#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/signals/signal.hpp>
#include <jaiscript/debug/connector.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <jaiscript/testing/unattended.hpp>

namespace fs = std::filesystem;

// the host's own permissions take the slots after the engine's
enum class GamePermission : std::int32_t { cheats = jai::permissions::count };

struct Vec2 {
	double x = 0, y = 0;
	Vec2() = default;
	Vec2(double x, double y) : x(x), y(y) {}
	Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
	double length() const { return std::sqrt(x * x + y * y); }
	template<typename Archive> void serialize(Archive& ar) { ar("x", x); ar("y", y); }
};

class Actor {
public:
	std::string name = "actor";
	Actor() = default;
	explicit Actor(std::string n) : name(std::move(n)) {}
	virtual ~Actor() = default;
	virtual std::string describe() const { return "actor " + name; }
	template<typename Archive> void serialize(Archive& ar) { ar("name", name); }
};

class Monster : public Actor {
	jai::signal_emitter<void(std::string, int64_t)> onHurtSignal;
public:
	jai::signal<void(std::string, int64_t)> on_hurt;
	int64_t hp = 10;
	Vec2 pos;

	Monster() : on_hurt(onHurtSignal) {}
	Monster(std::string n, int64_t h) : Actor(std::move(n)), on_hurt(onHurtSignal), hp(h) {}

	std::string describe() const override { return "monster " + name + " (" + std::to_string(hp) + " hp)"; }
	bool alive() const { return hp > 0; }
	void hurt(int64_t amount) {
		hp = hp > amount ? hp - amount : 0;
		onHurtSignal(name, hp);
	}
	template<typename Archive> void serialize(Archive& ar) { ar("name", name); ar("hp", hp); ar("pos", pos); }
};

struct SaveGame {
	int64_t wave = 0;
	std::string hero;
	std::vector<int64_t> scores;
	std::shared_ptr<Actor> boss;
	std::string script_state;
	template<typename Archive> void serialize(Archive& ar) {
		ar("wave", wave);
		ar("hero", hero);
		ar("scores", scores);
		ar("boss", boss);
		ar("script_state", script_state);
	}
};

// A registrar (rather than a plain dynamic_binder) also gives Monster its serialization identity,
// so a shared_ptr<Actor> holding one saves a $type and loads back as a Monster.
static jai::registrar<Monster, void, jai::bases<Actor>> monsterRegistrar("Monster", [](jai::dynamic_binder<Monster>& b) {
	b.base_class<Actor>()
		.constructor<std::string, int64_t>()
		.property("hp", &Monster::hp)
		.property("pos", &Monster::pos)
		.property("on_hurt", &Monster::on_hurt)
		.method("hurt", &Monster::hurt)
		.method("alive", &Monster::alive);
});

static int failures = 0;

static void section(const std::string& title) { std::cout << "\n== " << title << " ==\n"; }

static void expect(bool ok, const std::string& what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
	if (!ok) { ++failures; }
}

static jai::script_value call(jai::engine& eng, const std::string& name, const std::vector<jai::script_value>& args = {}) {
	jai::script_value fn = eng.get_variable(name);
	jai::engine::external_call_guard guard(&eng);
	jai::checked_result<jai::script_value> result = fn.as_function()(args);
	if (!result) { throw jai::runtime_error(eng.format_error(result)); }
	return result.value();
}

static void write_text(const fs::path& path, const std::string& text) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

static std::string read_text(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static void bind_game_api(jai::engine& eng) {
	jai::dynamic_binder<Vec2>(eng, "Vec2")
		.constructor<>()
		.constructor<double, double>()
		.property("x", &Vec2::x)
		.property("y", &Vec2::y)
		.method("length", &Vec2::length)
		.method("+", &Vec2::operator+)
		.build();

	jai::dynamic_binder<Actor>(eng, "Actor")
		.constructor<std::string>()
		.property("name", &Actor::name)
		.method("describe", &Actor::describe)
		.build();

	jai::bind_registrar(eng);

	eng.add_function("spawn_offset", []() { return Vec2(0.5, -1.0); });
	eng.add_function("host_log", [](const std::string& line) { std::cout << "  [script] " << line << "\n"; });
}

static int run(int argc, char** argv) {
	bool debug = false, debug_wait = false;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--debug") { debug = true; }
		if (a == "--debug-wait") { debug = debug_wait = true; }
	}

	const fs::path scripts = fs::canonical(fs::path(EMBED_QUICKSTART_SCRIPTS));
	const fs::path live = fs::current_path() / "live";
	fs::create_directories(live);
	write_text(live / "rules.jai", read_text(scripts / "rules_v1.jai"));

	section("engine");
	auto eng = jai::engine::make();
	jai::stdlib::register_all(eng);
	std::cout << "  backend: " << eng->get_backend_name() << "\n";
	eng->add_include_path(live.string());
	bind_game_api(*eng);
	expect(eng->execute("Vec2(3.0, 4.0).length()").as_float() == 5.0, "bound Vec2 answers length() == 5");

	std::shared_ptr<jai::debug::debug_connector> connector;
	if (debug) {
		connector = jai::debug::listen(*eng);
		if (!connector->listening()) {
			std::cout << "  " << connector->error() << "\n";
		} else {
			std::cout << "  debugger listening on 127.0.0.1:" << connector->port() << "\n";
		}
		if (debug_wait && connector->listening()) {
			std::cout << "  attach from VS Code, set breakpoints in scripts/game.jai, then press Enter\n";
			std::string line;
			std::getline(std::cin, line);
		}
	}

	section("run a script file");
	eng->execute_file((scripts / "game.jai").string());
	expect(eng->has_function("tick"), "game.jai defined tick()");

	section("call script functions from C++");
	int64_t dmg = call(*eng, "tick", {eng->make_value(int64_t{1})}).as_int();
	expect(dmg == 2, "tick(1) == 2 under rules v1 (got " + std::to_string(dmg) + ")");
	int64_t crit = call(*eng, "tick", {eng->make_value(int64_t{5})}).as_int();
	expect(crit == 12, "tick(5) adds tuning.crit (got " + std::to_string(crit) + ")");
	try {
		call(*eng, "explode", {eng->make_value("kaboom")});
		expect(false, "a script throw inside a called function reaches the host");
	} catch (const jai::runtime_error& e) {   // a checked_result call reports the throw in its result; call() rethrows that error
		expect(std::string(e.what()) == "kaboom", std::string("a script throw arrives as the call's error: ") + e.what());
	}
	auto typed_tick = jai::convert_script_function<int64_t(int64_t)>(eng->get_variable("tick"));
	expect(typed_tick(1) == 2, "convert_script_function gives a typed std::function");

	section("C++ class from script, signal into script");
	jai::script_value spawned = call(*eng, "spawn", {eng->make_value("Ogre"), eng->make_value(int64_t{30})});
	std::shared_ptr<Monster> ogre = spawned.as<std::shared_ptr<Monster>>();
	expect(ogre && ogre->pos.x == 1.5 && ogre->pos.y == 1.0, "script built Monster with Vec2 + Vec2 position");
	ogre->hurt(12);
	ogre->hurt(7);
	auto journal = eng->get_variable("journal").as<std::vector<std::string>>();
	expect(journal.size() == 2, "C++ emit reached the script receiver twice");
	for (const auto& line : journal) { std::cout << "  journal: " << line << "\n"; }

	auto troll = std::make_shared<Monster>("Troll", 40);
	eng->add_global("troll", eng->make_object(troll));
	eng->execute("troll.hurt(15); troll.name = \"Cave Troll\";");
	expect(troll->hp == 25 && troll->name == "Cave Troll", "script writes land in the C++ object");
	expect(eng->execute("troll.describe()").as_string() == "monster Cave Troll (25 hp)", "base-class method dispatches virtually");

	section("read script state back");
	auto kills = eng->get_variable("kills").as<std::map<std::string, int64_t>>();
	expect(kills.size() == 2 && kills["orc"] == 3, "map<string, int> copies out as std::map<std::string, int64_t>");
	jai::script_value loot = eng->get_variable("loot");
	const std::vector<jai::script_value>& items = loot.as_array();
	expect(items.size() == 3 && items[1].as_string() == "rope", "array<var> reads through as_array()");
	jai::script_value journal_value = eng->get_variable("journal");
	size_t visited = 0;
	journal_value.unchecked_array_node()->for_each(eng.get(), [&](const jai::script_value& line) { visited += !line.as_string().empty(); });
	expect(visited == 2, "script_array::for_each walks a typed array without copying it out");
	std::string view_error;
	try { (void)journal_value.as_array(); } catch (const std::exception& e) { view_error = e.what(); }
	expect(view_error.find("array<string>") != std::string::npos, "as_array() on a typed array names its element type");

	section("save and load through the archives");
	SaveGame save;
	save.wave = 3;
	save.hero = "Alice";
	save.scores = {10, 20, 30};
	save.boss = troll;
	const int64_t frames_saved = eng->execute("frames_run").as_int();
	save.script_state = call(*eng, "snapshot").as_string();

	std::string json = jai::to_json(*eng, save);
	std::cout << json << "\n";
	SaveGame fromJson = jai::from_json<SaveGame>(*eng, json);
	auto jsonBoss = std::dynamic_pointer_cast<Monster>(fromJson.boss);
	expect(fromJson.hero == "Alice" && fromJson.scores.size() == 3, "JSON round-trip of plain fields");
	expect(jsonBoss && jsonBoss->hp == 25, "JSON round-trip rebuilt the Monster behind shared_ptr<Actor>");

	std::vector<uint8_t> bytes = jai::to_binary(*eng, save);
	SaveGame fromBinary = jai::from_binary<SaveGame>(*eng, bytes);
	auto binaryBoss = std::dynamic_pointer_cast<Monster>(fromBinary.boss);
	expect(binaryBoss && binaryBoss->name == "Cave Troll", "binary round-trip (" + std::to_string(bytes.size()) + " bytes)");

	eng->execute("frames_run = 0; journal = [];");
	call(*eng, "restore", {eng->make_value(fromBinary.script_state)});
	expect(eng->execute("frames_run").as_int() == frames_saved && eng->execute("journal.size()").as_int() == 2, "script state restored from the save");

	section("hot reload through import");
	int64_t before = eng->execute("rules_version()").as_int();
	write_text(live / "rules.jai", read_text(scripts / "rules_v2.jai"));
	int frames = 0;
	while (eng->execute("rules_version()").as_int() == before && frames < 100) {
		call(*eng, "tick", {eng->make_value(int64_t{frames})});
		++frames;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	expect(eng->execute("rules_version()").as_int() == 2, "rules.jai v2 picked up by a later frame");
	expect(eng->execute("tuning.crit").as_int() == 10 && eng->execute("tuning.bonus").as_int() == 3,
		"live Tuning migrated: crit kept 10, bonus took its initializer");
	expect(eng->execute("hits_dealt").as_int() >= 3, "hits_dealt survived the re-run");
	expect(call(*eng, "tick", {eng->make_value(int64_t{7})}).as_int() == 6, "tick(7) runs the new damage_for");

	eng->import_recheck_interval(0);
	write_text(live / "rules.jai", "int hits_dealt;\nint rules_version() { return \"three\"; }\n");
	try {
		call(*eng, "tick", {eng->make_value(int64_t{8})});
		expect(false, "a broken rules.jai should raise at the import");
	} catch (const std::exception& e) {
		expect(eng->execute("rules_version()").as_int() == 2, "a broken edit raises and keeps the running rules:");
		std::cout << e.what() << "\n";
	}

	section("safety rails");
	eng->execution_budget(0.1);
	try {
		eng->execute("int spins = 0; try { while (true) { spins += 1; } } catch (e) { print(\"never\"); }");
		expect(false, "an endless loop should exceed the budget");
	} catch (const jai::runtime_error& e) {
		expect(true, std::string("budget stopped the loop: ") + e.what());
	}
	eng->execution_budget(5.0);
	expect(eng->execute("40 + 2").as_int() == 42, "engine still usable after the budget");

	auto sandbox = jai::engine::make();
	sandbox->set_backend(eng->get_backend_type());
	jai::stdlib::register_all(sandbox);
	sandbox->disable(jai::permissions::filesystem);
	sandbox->register_permission(GamePermission::cheats, "game.cheats");
	sandbox->add_function("give_gold", [](int64_t n) { return n * 2; }, GamePermission::cheats);
	try {
		sandbox->execute("import \"rules.jai\";");
		expect(false, "import should be refused without the filesystem permission");
	} catch (const std::exception& e) {
		expect(std::string(e.what()).find("Insufficient permission: filesystem.") != std::string::npos,
			std::string("sandbox import refused: ") + e.what());
	}
	try {
		sandbox->execute("give_gold(5)");
		expect(false, "give_gold should need game.cheats");
	} catch (const std::exception& e) {
		expect(true, std::string("custom permission enforced: ") + e.what());
	}
	sandbox->enable(GamePermission::cheats);
	expect(sandbox->execute("give_gold(5)").as_int() == 10, "give_gold works once game.cheats is enabled");

	section("script errors in C++");
	try {
		eng->execute("int x = undefined_thing + 1;");
		expect(false, "undeclared name should fail the load");
	} catch (const jai::static_check_error& e) {
		expect(true, "static_check_error:");
		std::cout << e.what() << "\n";
	}
	try {
		eng->execute("var x = ;");
		expect(false, "bad syntax should fail to parse");
	} catch (const jai::parse_error& e) {
		expect(true, std::string("parse_error: ") + e.what());
	}
	try {
		call(*eng, "average", {eng->execute("array<int> empty; empty")});
		expect(false, "average([]) should divide by zero");
	} catch (const jai::runtime_error& e) {
		expect(true, std::string("runtime_error: ") + e.what());
		std::cout << eng->format_stack_trace() << "\n";
	}
	try {
		eng->execute("throw \"from script\";");
	} catch (const jai::script_exception& e) {
		expect(std::string(e.what()).find("from script") == 0, std::string("script_exception: ") + e.what());
	}

	eng->execute("troll.on_hurt.connect(\"broken\", [=](string who, int left) -> void { int z = left / 0; });");
	try {
		troll->hurt(1);   // a script receiver's failure throws out of the emit; the receivers after it do not run
		expect(false, "a failing signal receiver should throw out of the emit");
	} catch (const jai::runtime_error& e) {
		expect(std::string(e.what()).find("Division by zero") != std::string::npos, std::string("a failing signal receiver throws to the emitter: ") + e.what());
	}
	expect(eng->execute("string caught = \"\"; try { troll.hurt(1); } catch (e) { caught = e.message; } caught").as_string().find("Division by zero") != std::string::npos,
		"a script that fires the signal catches the receiver's failure");
	eng->execute("troll.on_hurt.disconnect(\"broken\");");

	std::cout << "\n" << (failures == 0 ? "all checks passed" : std::to_string(failures) + " check(s) failed") << "\n";
	return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
	jai::foundry::route_faults_to_stderr();
	std::cout << std::unitbuf;
	try {
		return run(argc, argv);
	} catch (const std::exception& e) {
		std::cout << "\nuncaught: " << e.what() << "\n";
		return 2;
	}
}
