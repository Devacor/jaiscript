// The documentation site's C++ samples and its host-dependent script samples, run as the pages show them.
//
// A page marks a checked block with data-sample="id". A C++ block's code is held here between the markers
// "// [sample id]" and "// [/sample]" (several regions of one id join with a blank line, so a sample can declare types at
// namespace scope and run statements in a test); the page's text must equal it. A script block marked data-host is read from
// the page and run on the engine the test set up. The output block that follows the marked block on the page is what the run
// must print: exactly for a C++ sample, and line by line in order for a script sample (the runner's "== result" lines aside).
// With JAI_DOC_SAMPLES_DUMP naming a directory, each run's output is also written there as <id>.txt, for an author to paste.
// A region marked "// [context id]" ... "// [/context]" is code a sample names but its page does not show; the page's text
// never holds it. docs/site_tools/check_sample_includes.py compiles a whole-program page's samples with that context and
// only the includes the page shows (tools/ci/gates.sh headers).
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/jaiscript.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <jaiscript/core/engine_time.hpp>
#include <jaiscript/core/script_run.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// [sample quickstart-run]
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>   // section 3
#include <jaiscript/stdlib/stdlib.hpp>
#include <filesystem>                          // section 5
#include <fstream>
#include <iostream>
// [/sample]

namespace doc_quickstart_run {
// [sample quickstart-run]
int main() {
    auto eng = jai::engine::make();                    // an engine is an object: make as many as you like
    jai::stdlib::register_all(eng);                    // formatted print, to_string, JSON, random, time

    eng->execute(R"(print("hello from {}", "JaiScript");)");

    jai::held_value r = eng->execute("6 * 7");         // the value of the last expression
    std::cout << "6 * 7 = " << r.as_int() << "\n";
    std::cout << eng->execute(R"("jai" + "script")").as<std::string>() << "\n";
    std::cout << std::boolalpha << eng->execute("2.5 * 2 > 4").as<bool>() << "\n";
    return 0;
}
// [/sample]
}

// [sample vs-first-main]
#include <jaiscript/core/engine.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <iostream>
// [/sample]

namespace doc_vs_first_main {
// [sample vs-first-main]
int main() {
    auto eng = jai::engine::make();
    jai::stdlib::register_all(eng);     // print, strings, JSON, time and more

    std::cout << "2 + 2 = " << eng->execute<int>("2 + 2") << "\n";
    eng->execute(R"(print("hello from {}", "JaiScript");)");

    try {
        eng->execute(R"(int x = "not a number";)");
    } catch (const std::exception& e) {
        std::cout << "caught: " << e.what() << "\n";
    }
    return 0;
}
// [/sample]
}

namespace doc_quickstart_bind {
// [sample quickstart-bind]
struct Player {
    std::string name;
    int64_t hp = 100;
    explicit Player(std::string a_name) : name(std::move(a_name)) {}
    void hit(int64_t damage) { hp -= damage; }
};
// [/sample]
}

// [sample binder-turret]
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <iostream>
// [/sample]
// [sample binder-spawner-class]
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/signals/signal_property.hpp>
#include <jaiscript/serialization/convenience.hpp>   // to_json / from_json / to_binary below
// [/sample]
// [sample signals-observable]
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/property_serialization.hpp>   // property<T> vtable lives here
#include <jaiscript/properties/observable_property.hpp>
// [/sample]
// [sample math-marker-class]
#include <jaiscript/properties/property_manager.hpp>
#include <jaiscript/properties/observable_property.hpp>
#include <jaiscript/properties/macros.hpp>
#include <jaiscript/properties/property_serialization.hpp>
// [/sample]
#include <jaiscript/thread_pool.hpp>
// [sample serialization-savegame]
#include <jaiscript/serialization/convenience.hpp>
// [/sample]
// [sample coroutine-native-handle]
#include <jaiscript/core/coroutine.hpp>
// [/sample]

// [sample math-types]
#pragma once
#include <jaiscript/math/adapter.hpp>

namespace game {
    struct Vec3 {
        float x = 0, y = 0, z = 0;
        friend bool operator==(const Vec3&, const Vec3&) = default;
    };
    struct Cell { int col = 0, row = 0; };
    struct Rgba { float R = 1, G = 1, B = 1, A = 1; };
}

JAI_BIND_VEC3((game::Vec3), x, y, z)
JAI_BIND_IVEC2((game::Cell), col, row)
JAI_BIND_COLOR((game::Rgba), R, G, B, A)
// [/sample]

namespace doc_binder_turret {
// [sample binder-turret]
struct Turret {                                   // 1. an ordinary class
    std::string name = "turret";
    int64_t ammo = 10;
    Turret() = default;
    Turret(std::string a_name, int64_t a_ammo) : name(std::move(a_name)), ammo(a_ammo) {}
    bool fire() { if (ammo == 0) { return false; } --ammo; return true; }
    double range() const { return 12.5; }
};

int64_t total_ammo(const Turret& a, const Turret& b) { return a.ammo + b.ammo; }

int main() {
    auto eng = jai::engine::make();
    jai::stdlib::register_all(eng);

    jai::dynamic_binder<Turret>(*eng, "Turret")   // 2. bind it under a script name
        .constructor<>()
        .constructor<std::string, int64_t>()
        .property("name", &Turret::name)
        .property("ammo", &Turret::ammo)
        .method("fire", &Turret::fire)
        .method("range", &Turret::range);
    eng->add_function("total_ammo", &total_ammo);  // 3. functions that take it

    eng->execute(R"(                                // 4. scripts construct and call it
        Turret t = Turret("north", 2);
        t.fire();
        t.fire();
        print(t.name, " ammo=", t.ammo, " fired=", t.fire(), " range=", t.range());
    )");

    auto east = std::make_shared<Turret>("east", 5);  // 5. hand a live C++ object to script
    eng->add_global("east", eng->make_object(east));
    eng->execute("east.fire(); east.ammo += 10;");
    std::cout << "C++ sees east.ammo = " << east->ammo << "\n";

    eng->execute(R"(var west = new Turret("west", 3); print(total_ammo(east, west));)");
    auto west = eng->get_variable("west").as<std::shared_ptr<Turret>>();   // 6. and take one back
    west->ammo = 99;
    eng->execute("print(west.ammo);");
    return 0;
}
// [/sample]
}

namespace doc_binder_spawner {
// [sample binder-spawner-class]
class Spawner : public jai::property_owner<Spawner> {
public:
    JAI_PROPERTY((std::string), id, "north-gate");
    JAI_PROPERTY((int64_t), level, 1);
    JAI_PROPERTY((double), interval, 2.5);
    JAI_OBSERVABLE_PROPERTY((int64_t), threat, 0);     // emits on_change(old, new)
    JAI_SIGNAL_PROPERTY(void(int64_t), onWave);        // script-connectable, serializable

    void spawn_wave() { total_waves_ += 1; onWaveSignal(total_waves_); }
    int64_t total_waves() const { return total_waves_; }

private:
    int64_t total_waves_ = 0;   // not a property: private, unreflected, unsaved
};

static jai::registrar<Spawner, void> _regSpawner("Spawner",
    [](jai::dynamic_binder<Spawner>& b) {
        b.method("spawn_wave", &Spawner::spawn_wave);
        b.method("total_waves", &Spawner::total_waves);
    });
// [/sample]
}

namespace doc_binder_vec2 {
// [sample binder-vec2-host]
struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x, double y) : x(x), y(y) {}
    double length() const { return std::sqrt(x * x + y * y); }
    Vec2& scale(double k) { x *= k; y *= k; return *this; }
    Vec2& translate(double dx, double dy) { x += dx; y += dy; return *this; }
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};
// [/sample]
}

namespace doc_binder_sprite {
// [sample binder-sprite-host]
struct Sprite {
    static std::vector<Sprite*>& registry() { static std::vector<Sprite*> all; return all; }
    double alpha = 1.0;
    Sprite() { registry().push_back(this); }        // every sprite joins the renderer's list
    ~Sprite() { std::erase(registry(), this); }
};
// [/sample]
}

namespace doc_binder_meter {
// [context binder-meter-host]
// what the page's Meter reaches beyond its own code: an audio manager, a log, the program's one Meter
struct audio_manager { void play(const char*) const {} };
inline const audio_manager& audio() { static const audio_manager manager; return manager; }
struct log_sink { void write(const std::string&) {} };
inline log_sink& logger() { static log_sink sink; return sink; }
// [/context]
// [sample binder-meter-host]
struct Meter {
    double level = 0.0;
    void add(double amount) { level += amount; }      // non-const: only an object the worker made
    double reading() const { return level; }          // const: any object
    void publish() const { audio().play("tick"); }    // the audio manager: serial
};
// [/sample]
// [context binder-meter-host]
inline std::shared_ptr<Meter> master_meter = [] { auto meter = std::make_shared<Meter>(); meter->level = 100.0; return meter; }();
// [/context]
}

namespace doc_binder_pickup {
// [sample binder-pickup-host]
class Pickup : public jai::property_owner<Pickup> {
public:
    JAI_PROPERTY((std::string), name, "pickup");
};

class HealthPickup : public jai::property_owner<HealthPickup, Pickup> {
public:
    JAI_PROPERTY((int64_t), healing, 25);
};
// [/sample]
}

namespace doc_binder_archer {
// [sample binder-binding-record]
struct Actor {
    virtual ~Actor() = default;
    std::string name;
};
struct Archer : Actor {
    int arrows = 12;
};
// [/sample]
}

namespace doc_signals {
// [sample signals-door]
class Door {
private:
    jai::signal_emitter<void(int)> onOpenSignal;
public:
    jai::signal<void(int)> onOpen;                    // public connect surface
    Door() : onOpen(onOpenSignal) {}
    void configure(jai::engine* eng) {
        onOpenSignal.script_engine(eng)               // enable script receivers
                    .parameter_names({"width"});      // names visible to script snippets
    }
    void open(int width) { onOpenSignal(width); }     // only the owner emits
};
// [/sample]
}

namespace doc_math {
// [sample math-body]
struct Body {
    game::Vec3 pos;
    game::Rgba tint;
    std::vector<game::Vec3> path;
    game::Vec3 moved(game::Vec3 d) const { return {pos.x + d.x, pos.y + d.y, pos.z + d.z}; }
};
// [/sample]
// [sample math-marker-class]
class Marker : public jai::property_owner<Marker> {
public:
    JAI_PROPERTY((game::Vec3), position);
    JAI_OBSERVABLE_PROPERTY((game::Vec3), velocity);
    JAI_PROPERTY((game::Rgba), tint);
};
// [/sample]
}

namespace doc_chai {
// [sample chai-monster-types]
struct Entity {
    virtual ~Entity() = default;
    std::string name = "entity";
    virtual std::string describe() const { return "entity " + name; }
};

struct Monster : Entity {
    int64_t hp = 10;
    Monster() = default;
    Monster(std::string a_name, int64_t a_hp) : hp(a_hp) { name = std::move(a_name); }
    void damage(int64_t amount) { hp -= amount; }
    void damage(int64_t amount, double multiplier) { hp -= static_cast<int64_t>(amount * multiplier); }
    int64_t health() const { return hp; }
    void setHealth(int64_t a_hp) { hp = a_hp < 0 ? 0 : a_hp; }
    bool alive() const { return hp > 0; }
    std::string describe() const override { return "monster " + name; }
    bool operator==(const Monster& o) const { return name == o.name && hp == o.hp; }
};
// [/sample]
}


namespace doc_serialization {
// [sample serialization-savegame]
struct SaveGame {
    int level = 0; double time_played = 0; std::string hero; std::vector<int> inventory;
    template<typename Archive>
    void serialize(Archive& ar) {          // one method, both directions, both formats
        ar("level", level);
        ar("time_played", time_played);
        ar("hero", hero);
        ar("inventory", inventory);
    }
};
// [/sample]
// [sample serialization-binding-record]
// a third-party header: no serialize, save, load or property_owner
struct Enemy {
    std::string name = "rat";
    int hp = 10;
    std::vector<std::string> loot;
};
// [/sample]
// [sample serialization-link]
struct Node {
    int64_t v = 1;
    template<class Archive> void serialize(Archive& ar) { ar("v", v); }
};
struct Link {
    std::shared_ptr<Node> a, b;
    std::weak_ptr<Node> w;
    template<class Archive> void serialize(Archive& ar) { ar("a", a); ar("b", b); ar("w", w); }
};
// [/sample]
}

namespace doc_host_direct {
// [sample host-api-direct-entry]
// one body for both: the function and its direct entry
static jai::checked_result<jai::script_value> label_of(const void* target, const jai::script_value* args, size_t count) {
    auto* eng = static_cast<jai::engine*>(const_cast<void*>(target));
    if (count != 1) return jai::script_value(std::string("?"), eng);
    return jai::script_value(std::string(args[0].is_int() ? "int" : args[0].is_float() ? "float" : "other"), eng);
}
// [/sample]
}

namespace doc_samples {

std::filesystem::path source_root() { return std::filesystem::path(JAISCRIPT_TESTS_SOURCE_ROOT); }

std::string read_file(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	std::string result = ss.str();
	std::string unix;
	unix.reserve(result.size());
	for (char c : result) {
		if (c != '\r') { unix += c; }
	}
	return unix;
}

std::string unescape_html(const std::string& text) {
	static const std::pair<const char*, const char*> entities[] = {
		{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#x27;", "'"}, {"&#39;", "'"}, {"&mdash;", "\xE2\x80\x94"},
		{"&ndash;", "\xE2\x80\x93"}, {"&middot;", "\xC2\xB7"}, {"&rarr;", "\xE2\x86\x92"}, {"&larr;", "\xE2\x86\x90"},
		{"&times;", "\xC3\x97"}, {"&#8203;", ""}, {"&amp;", "&"}};
	std::string result;
	for (size_t i = 0; i < text.size();) {
		bool matched = false;
		if (text[i] == '&') {
			for (const auto& [entity, plain] : entities) {
				const size_t n = std::char_traits<char>::length(entity);
				if (text.compare(i, n, entity) == 0) {
					result += plain;
					i += n;
					matched = true;
					break;
				}
			}
		}
		if (!matched) { result += text[i++]; }
	}
	return result;
}

std::string strip_tags(const std::string& text) {
	std::string result;
	bool in_tag = false;
	for (char c : text) {
		if (c == '<') { in_tag = true; continue; }
		if (c == '>' && in_tag) { in_tag = false; continue; }
		if (!in_tag) { result += c; }
	}
	return result;
}

// The code of the block marked data-sample="id" and the output block right after it ("" when there is none)
struct page_block {
	std::string code;
	std::string output;
	bool found = false;
	bool has_output = false;
};

page_block find_block(const std::string& page_path, const std::string& id) {
	page_block result;
	const std::string page = read_file(source_root() / "docs" / "site" / page_path);
	result.has_output = false;
	const size_t mark = page.find("data-sample=\"" + id + "\"");
	if (mark == std::string::npos) { return result; }
	const size_t code_open = page.find("<code", mark);
	const size_t code_start = page.find('>', code_open) + 1;
	const size_t code_end = page.find("</code>", code_start);
	result.code = unescape_html(strip_tags(page.substr(code_start, code_end - code_start)));
	result.found = true;
	const size_t block_end = page.find("</div>", code_end) + 6;
	size_t next = block_end;
	while (next < page.size() && std::isspace(static_cast<unsigned char>(page[next]))) { ++next; }
	if (page.compare(next, 36, "<div class=\"codeblock cb-output\"><sp") == 0) {
		const size_t out_open = page.find("<code", next);
		const size_t out_start = page.find('>', out_open) + 1;
		const size_t out_end = page.find("</code>", out_start);
		result.output = unescape_html(strip_tags(page.substr(out_start, out_end - out_start)));
		result.has_output = true;
	}
	return result;
}

std::string dedent(const std::vector<std::string>& lines) {
	size_t indent = std::string::npos;
	for (const auto& line : lines) {
		const size_t first = line.find_first_not_of(" \t");
		if (first != std::string::npos) { indent = std::min(indent, first); }
	}
	std::string result;
	for (size_t i = 0; i < lines.size(); ++i) {
		std::string line = lines[i].size() >= indent && indent != std::string::npos ? lines[i].substr(indent) : std::string();
		while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
		result += line;
		if (i + 1 < lines.size()) { result += "\n"; }
	}
	return result;
}

// The regions of this file marked with the id, each dedented, joined with a blank line
std::string sample_source(const std::string& id) {
	const std::string self = read_file(source_root() / "source" / "tests" / "integration" / "doc_samples_tests.cpp");
	std::istringstream in(self);
	std::string line, result;
	std::vector<std::string> region;
	bool inside = false;
	const std::string open = "// [sample " + id + "]";
	while (std::getline(in, line)) {
		const size_t first = line.find_first_not_of(" \t");
		const std::string trimmed = first == std::string::npos ? std::string() : line.substr(first);
		if (!inside && trimmed == open) {
			inside = true;
			region.clear();
			continue;
		}
		if (inside && trimmed == "// [/sample]") {
			inside = false;
			if (!result.empty()) { result += "\n\n"; }
			result += dedent(region);
			continue;
		}
		if (inside) { region.push_back(line); }
	}
	return result;
}

std::string trim_lines(const std::string& text) {
	std::istringstream in(text);
	std::string line, result;
	std::vector<std::string> lines;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) { line.pop_back(); }
		lines.push_back(line);
	}
	while (!lines.empty() && lines.back().empty()) { lines.pop_back(); }
	while (!lines.empty() && lines.front().empty()) { lines.erase(lines.begin()); }
	for (size_t i = 0; i < lines.size(); ++i) { result += lines[i] + (i + 1 < lines.size() ? "\n" : ""); }
	return result;
}

// Every non-empty expected line except the runner's "==" lines appears in the output, in order
std::string missing_lines(const std::string& expected, const std::string& got) {
	std::vector<std::string> got_lines;
	{
		std::istringstream in(got);
		std::string line;
		while (std::getline(in, line)) { got_lines.push_back(trim_lines(line)); }
	}
	std::istringstream in(expected);
	std::string line, result;
	size_t pos = 0;
	while (std::getline(in, line)) {
		const std::string want = trim_lines(line);
		const size_t first = want.find_first_not_of(' ');
		if (first == std::string::npos || want.compare(first, 2, "==") == 0) { continue; }
		bool found = false;
		for (size_t j = pos; j < got_lines.size(); ++j) {
			if (got_lines[j] == want) {
				pos = j + 1;
				found = true;
				break;
			}
		}
		if (!found) { result += want + "\n"; }
	}
	return result;
}

void dump(const std::string& id, const std::string& got) {
	const char* dir = std::getenv("JAI_DOC_SAMPLES_DUMP");
	if (dir && *dir) { std::ofstream(std::filesystem::path(dir) / (id + ".txt"), std::ios::binary) << got; }
}

class cout_capture {
public:
	cout_capture() : previous(std::cout.rdbuf(buffer.rdbuf())) {}
	~cout_capture() { std::cout.rdbuf(previous); }
	std::string str() const { return buffer.str(); }
private:
	std::ostringstream buffer;
	std::streambuf* previous;
};

}  // namespace doc_samples

namespace jai::foundry::tests {

class doc_samples_tests : public suite {
public:
	doc_samples_tests() : suite("Doc Samples") {}

	// A C++ block run where the test stands: construction checks the page's code against this file's regions and starts
	// capturing std::cout; finish() compares what the region printed with the page's output block (when the page shows one)
	class cpp_sample {
	public:
		cpp_sample(doc_samples_tests& a_suite, std::string a_page, std::string a_id) :
			owner(a_suite),
			page(std::move(a_page)),
			id(std::move(a_id)),
			block(doc_samples::find_block(page, id)) {

			check_true(block.found, page + ": no block marked data-sample=\"" + id + "\"");
			check_eq(doc_samples::trim_lines(doc_samples::sample_source(id)), doc_samples::trim_lines(block.code),
			               page + " " + id + ": the page's code and the code this test runs differ");
			capture = std::make_unique<doc_samples::cout_capture>();
		}
		void finish() {
			const std::string got = doc_samples::trim_lines(capture->str());
			capture.reset();
			doc_samples::dump(id, got);
			if (block.has_output) { check_eq(doc_samples::trim_lines(block.output), got, page + " " + id + ": output"); }
		}
	private:
		doc_samples_tests& owner;
		std::string page;
		std::string id;
		doc_samples::page_block block;
		std::unique_ptr<doc_samples::cout_capture> capture;
	};

	// The page's code equals the regions held here, and what the run printed equals the page's output block
	void expect_cpp(const std::string& page, const std::string& id, const std::function<void()>& run) {
		const auto block = doc_samples::find_block(page, id);
		check_true(block.found, page + ": no block marked data-sample=\"" + id + "\"");
		if (!block.found) { return; }
		check_eq(doc_samples::trim_lines(doc_samples::sample_source(id)), doc_samples::trim_lines(block.code),
		         page + " " + id + ": the page's code and the code this test runs differ");
		std::string got;
		{
			doc_samples::cout_capture capture;
			try {
				run();
			} catch (const std::exception& e) {
				std::cout << "threw: " << e.what() << "\n";
			}
			got = capture.str();
		}
		doc_samples::dump(id, doc_samples::trim_lines(got));
		check_eq(doc_samples::trim_lines(block.output), doc_samples::trim_lines(got), page + " " + id + ": output");
	}

	// The page's host-dependent script block, run on an engine the test registered the host on
	void expect_script(const std::string& page, const std::string& id, const std::shared_ptr<engine>& eng) {
		const auto block = doc_samples::find_block(page, id);
		check_true(block.found, page + ": no block marked data-sample=\"" + id + "\"");
		if (!block.found) { return; }
		std::string got;
		bool threw = false;
		{
			doc_samples::cout_capture capture;
			try {
				eng->execute(block.code);
			} catch (const std::exception& e) {
				std::cout << e.what() << "\n";
				threw = true;
			}
			got = capture.str();
		}
		doc_samples::dump(id, doc_samples::trim_lines(got));
		check_eq(std::string(), doc_samples::missing_lines(block.output, got), page + " " + id + ": lines the run did not print\n" + got);
		if (!block.has_output) { check_true(!threw, page + " " + id + ": a block that shows no output raised\n" + got); }
	}

	void forge_tests() override {
		test("quickstart_run", [this]() {
			expect_cpp("embedding/01-embedding-quickstart.html", "quickstart-run", [] { doc_quickstart_run::main(); });
		});

		test("vs_first_main", [this]() {
			expect_cpp("embedding/21-visual-studio.html", "vs-first-main", [] { doc_vs_first_main::main(); });
		});

		test("quickstart_bind", [this]() {
			expect_cpp("embedding/01-embedding-quickstart.html", "quickstart-bind", [] {
				using doc_quickstart_bind::Player;
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample quickstart-bind]
				// in main, after the engine is made:
				jai::dynamic_binder<Player>(*eng, "Player")
				    .constructor<std::string>()
				    .property("name", &Player::name)
				    .property("hp", &Player::hp)
				    .method("hit", &Player::hit);
				eng->add_function("roll", [](int64_t sides) { return sides / 2 + 1; });   // any C++ callable

				auto hero = std::make_shared<Player>("Ada");     // a live C++ object the script shares
				eng->add_global("hero", eng->make_object(hero));
				eng->execute(R"(
				    hero.hit(roll(20));
				    Player rat = Player("rat");                   // scripts construct the class too
				    rat.hit(30);
				    print(hero.name, " ", hero.hp, ", ", rat.name, " ", rat.hp);
				)");
				std::cout << "C++ sees hp " << hero->hp << "\n";
				// [/sample]
			});
		});

		test("quickstart_call", [this]() {
			expect_cpp("embedding/01-embedding-quickstart.html", "quickstart-call", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample quickstart-call]
				eng->execute("int damage_for(int level) { return level * 3; }");

				jai::held_value fn = eng->get_variable("damage_for");   // held: safe to keep and call later
				jai::checked_result<jai::script_value> r = fn.as_function()({eng->make_value(int64_t{4})});
				if (r) { std::cout << "damage_for(4) = " << r.value().as_int() << "\n"; }

				auto damage = jai::convert_script_function<int64_t(int64_t)>(fn);   // or a typed std::function
				std::cout << "damage(10) = " << damage(10) << "\n";
				// [/sample]
			});
		});

		test("quickstart_reload", [this]() {
			expect_cpp("embedding/01-embedding-quickstart.html", "quickstart-reload", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample quickstart-reload]
				namespace fs = std::filesystem;
				const fs::path dir = fs::temp_directory_path() / "jai_quickstart";
				fs::create_directories(dir);
				auto save = [&](const char* name, const char* text) { std::ofstream(dir / name) << text; };
				save("rules.jai", "int damage_for(int frame) { return frame * 2; }");
				save("game.jai", "import \"rules.jai\";\n"
				                 "int tick(int frame) { import(\"rules.jai\"); return damage_for(frame); }");

				eng->import_recheck_interval(0);                  // see a save at once (the default looks every 0.2 s)
				eng->execute_file((dir / "game.jai").string());
				jai::held_value tick = eng->get_variable("tick");   // kept across frames
				for (int64_t frame = 1; frame <= 4; ++frame) {
				    if (frame == 3) { save("rules.jai", "int damage_for(int frame) { return frame * 100; }"); }   // an edit lands
				    auto r = tick.as_function()({eng->make_value(frame)});
				    if (!r) { std::cout << "frame " << frame << ": " << eng->format_error(r) << "\n"; continue; }
				    std::cout << "frame " << frame << ": " << r.value().as_int() << "\n";
				}
				// [/sample]
			});
		});

		test("guide_variadic_sum", [this]() {
			expect_cpp("guide/06-functions.html", "guide-variadic-sum", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample guide-variadic-sum]
				eng->add_variadic_function("sum_all", [](jai::engine&, std::span<const jai::script_value> args) {
				    jai::script_int total = 0;
				    for (const auto& a : args) total += a.as_int();
				    return total;
				});
				eng->execute("print(sum_all(1, 2, 3, 4));");
				// [/sample]
			});
		});

		test("host_api_register", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-register", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-register]
				// Typed: the signature drives conversion and overload resolution
				eng->add_function("square", [](jai::script_int n) { return n * n; });
				std::cout << eng->execute("square(12)").as_int() << "\n";

				// Variadic: the engine and every argument of the call
				eng->add_variadic_function("sum_all", [](jai::engine&, std::span<const jai::script_value> args) {
				    jai::script_int total = 0;
				    for (const auto& a : args) total += a.as_int();
				    return total;
				});
				std::cout << eng->execute("sum_all(1, 2, 3, 4)").as_int() << "\n";
				// [/sample]
			});
		});

		test("host_api_direct_variadic", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-direct-variadic", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-direct-variadic]
				eng->add_variadic_function("label", [](jai::engine&, std::span<const jai::script_value> args) -> std::string {
				    if (args.size() != 1) return "?";
				    return args[0].is_int() ? "int" : args[0].is_float() ? "float" : "other";
				});
				std::cout << eng->execute("label(2.5)").as_string() << "\n";
				// [/sample]
			});
		});

		test("host_api_direct_entry", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-direct-entry", [] {
				using doc_host_direct::label_of;
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-direct-entry]
				eng->add_variadic_function("label", [e = eng.get()](const std::vector<jai::script_value>& args) {
				    return label_of(e, args.data(), args.size());
				});
				eng->add_host_direct("label", 1, &label_of, eng.get());   // after the function: it keys on the function "label" binds now
				std::cout << eng->execute("label(2.5)").as_string() << "\n";
				// [/sample]
			});
		});

		test("host_api_live_global", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-live-global", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-live-global]
				eng->add_global("PI", jai::script_value(3.14159, eng.get()));   // copied in

				int hp = 100;
				eng->add_global_ref("hp", hp);                    // a live binding: no copy
				eng->execute("hp = hp - 25;");
				std::cout << "hp after script damage: " << hp << "\n";
				hp = 1;                                           // and the script sees C++ writes
				std::cout << "script sees: " << eng->execute("hp").as_int() << "\n";
				// [/sample]
			});
		});

		test("host_api_exact_width", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-exact-width", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-exact-width]
				jai::script_int gold = 10;
				eng->add_global_ref("gold", gold);
				auto v = eng->execute("gold");                    // a bound alias, not a snapshot
				v.as<jai::script_int&>() += 5;
				std::cout << "gold after as<script_int&> += 5: " << gold << "\n";

				int32_t narrow = 5;
				eng->add_global_ref("narrow", narrow);
				try {
				    eng->execute("narrow").as<jai::script_int&>();   // an int32_t is not a script_int
				} catch (const jai::runtime_error& e) {
				    std::cout << "int32 as<script_int&>: " << e.what() << "\n";
				}
				eng->execute("narrow += 7;");                     // by name every width works
				std::cout << "narrow after script += 7: " << narrow << "\n";
				// [/sample]
			});
		});

		test("host_api_check_listing", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-check-listing", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-check-listing]
				try {
				    eng->execute("int a = missing_one; int b = missing_two;");
				} catch (const jai::static_check_error& e) {
				    std::cout << e.what();                        // every error in the source, compiler style
				}
				// [/sample]
			});
		});

		test("host_api_stack_trace", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-stack-trace", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-stack-trace]
				try {
				    eng->execute("function inner() { throw \"boom\"; }\n"
				                 "function outer() { inner(); }\n"
				                 "outer();");
				} catch (const jai::script_exception& e) {
				    std::cout << e.what() << "\n" << eng->format_stack_trace();
				}
				// [/sample]
			});
		});

		test("host_api_time_source", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-time-source", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-time-source]
				std::chrono::nanoseconds sim_now{0};
				const auto epoch = std::chrono::nanoseconds(1700000000000000000LL);   // 2023-11-14T22:13:20Z
				eng->time().source(
				    [&] { return sim_now; },                                          // monotonic
				    [&] { return epoch + sim_now; },                                  // wall
				    [&](std::chrono::nanoseconds d) { sim_now += d; });              // sleep advances the simulation
				sim_now += std::chrono::milliseconds(1500);
				eng->execute("print(time::now(), ' ', time::utc_now().to_string());");
				// [/sample]
			});
		});

		test("host_api_suspendable_runs", [this]() {
			expect_cpp("embedding/17-host-api.html", "host-api-suspendable-runs", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample host-api-suspendable-runs]
				std::chrono::nanoseconds now{0};
				eng->time().source([&] { return now; }, {}, {});
				jai::script_run run = eng->start(R"(
				    for (int wave = 1; wave <= 3; ++wave) {
				        print("wave {} at {}", wave, time::now());
				        sleep(2.0);
				    }
				    "cleared";
				)");
				while (!run.done()) {
				    now = run.wake_at();   // a game moves its clock each frame and resumes; this jumps to the wake time
				    run.resume();
				}
				std::cout << run.result().value().to_string() << "\n";
				// [/sample]
			});
		});

		test("binder_turret", [this]() {
			cpp_sample run(*this, "embedding/02-dynamic-binder.html", "binder-turret");
			doc_binder_turret::main();
			run.finish();
		});

		// The binder page's Spawner session: every block runs on one engine, in page order
		test("binder_spawner_session", [this]() {
			using doc_binder_spawner::Spawner;
			const std::string page = "embedding/02-dynamic-binder.html";
			cpp_sample class_block(*this, page, "binder-spawner-class");
			class_block.finish();
			cpp_sample engine_block(*this, page, "binder-spawner-engine");
			// [sample binder-spawner-engine]
			auto eng = jai::engine::make();
			jai::stdlib::register_all(eng);   // formatted print, containers, math, json
			jai::bind_registrar(*eng);         // every jai::registrar in the binary, one call
			// [/sample]
			engine_block.finish();
			expect_script(page, "binder-spawner", eng);

			cpp_sample gate_block(*this, page, "binder-gate");
			// [sample binder-gate]
			auto gate = std::make_shared<Spawner>();
			eng->add_global("gate", eng->make_object(gate));
			eng->execute("gate.id = \"east-gate\"; gate.level = 7;");
			// gate->id.get() == "east-gate", gate->level.get() == 7

			eng->execute(R"JAI(
			    gate.onWave.connect("hud", [=](int wave) -> void {
			        print("gate wave {}", wave);
			    });
			)JAI");
			gate->spawn_wave();   // C++ emit -> the script receiver runs
			// [/sample]
			gate_block.finish();

			expect_script(page, "binder-threat-receiver", eng);
			cpp_sample threat_block(*this, page, "binder-threat");
			// [sample binder-threat]
			gate->threat = 5;                                    // C++ write
			eng->execute("gate.threat = gate.threat + 2;");      // script write; reads unwrap in expressions
			// [/sample]
			threat_block.finish();

			cpp_sample reflection_block(*this, page, "binder-reflection");
			// [sample binder-reflection]
			gate->visit_reflected_properties([](const std::string& name, jai::property_base* p) {
			    std::cout << "prop: " << name << "\n";
			});
			if (auto* lvl = gate->property_value<int64_t>("level")) {
			    std::cout << "level via reflection: " << *lvl << "\n";
			}
			gate->set_property_value<int64_t>("threat", int64_t{9});   // on_change fires
			// [/sample]
			reflection_block.finish();

			cpp_sample to_json_block(*this, page, "binder-to-json");
			// [sample binder-to-json]
			eng->execute(R"JAI(
			    gate.onWave.connect("logger", "print(\"wave logged\");");   // named script-text receiver
			)JAI");
			auto json = jai::to_json(*eng, gate);
			std::cout << json << "\n";
			// [/sample]
			to_json_block.finish();

			cpp_sample from_json_block(*this, page, "binder-from-json");
			// [sample binder-from-json]
			auto loaded = jai::from_json<std::shared_ptr<Spawner>>(*eng, json, *eng);
			// loaded->id.get() == "east-gate", level 7, threat 9
			loaded->spawn_wave();   // the receiver came back from the save file
			// [/sample]
			from_json_block.finish();

			cpp_sample to_binary_block(*this, page, "binder-to-binary");
			// [sample binder-to-binary]
			auto bytes  = jai::to_binary(*eng, gate);                                   // std::vector<uint8_t>
			auto again  = jai::from_binary<std::shared_ptr<Spawner>>(*eng, bytes, *eng);
			again->spawn_wave();   // receivers persist in binary too
			// [/sample]
			to_binary_block.finish();

			Spawner byValue = jai::from_json<Spawner>(*eng, jai::to_json(*eng, *gate));
			check_eq((int64_t)7, byValue.level.get(), "the page's by-value load");
			check_eq(std::string("east-gate"), byValue.id.get());

			expect_script(page, "binder-smart", eng);
			expect_script(page, "binder-smart-reload", eng);
			expect_script(page, "binder-connect", eng);
		});

		test("binder_binding_record", [this]() {
			using doc_binder_archer::Actor;
			using doc_binder_archer::Archer;
			auto eng = make_engine();
			cpp_sample block(*this, "embedding/02-dynamic-binder.html", "binder-binding-record");
			// [sample binder-binding-record]
			jai::dynamic_binder<Actor>(*eng, "Actor").property("name", &Actor::name);
			jai::dynamic_binder<Archer>(*eng, "Archer").base_class<Actor>().property("arrows", &Archer::arrows);

			std::shared_ptr<Actor> hero = std::make_shared<Archer>();
			hero->name = "robin";
			std::string json = jai::to_json(*eng, hero, 0);
			auto back = jai::from_json<std::shared_ptr<Actor>>(*eng, json);
			std::cout << json << "\n";
			std::cout << back->name << " has " << std::dynamic_pointer_cast<Archer>(back)->arrows << " arrows\n";
			// [/sample]
			block.finish();
		});

		test("binder_vec2", [this]() {
			using doc_binder_vec2::Vec2;
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample host(*this, "embedding/02-dynamic-binder.html", "binder-vec2-host");
			// [sample binder-vec2-host]
			jai::dynamic_binder<Vec2>(*eng, "Vec2")
			    .constructor<>()
			    .constructor<double, double>()             // overloaded ctors resolve by arity + type
			    .method("length", &Vec2::length)
			    .method("scale", &Vec2::scale)             // Vec2& scale(double) -> chainable
			    .method("translate", &Vec2::translate)
			    .method("+", &Vec2::operator+)             // operators are methods named by token
			    .method("==", &Vec2::operator==)
			    .static_method("axes", []() { return 2; })
			    .static_method("dot", [](const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; })
			    .property("x", &Vec2::x)
			    .property("y", &Vec2::y);
			// [/sample]
			host.finish();
			expect_script("embedding/02-dynamic-binder.html", "binder-vec2", eng);
		});

		test("binder_sprite", [this]() {
			using doc_binder_sprite::Sprite;
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample host(*this, "embedding/02-dynamic-binder.html", "binder-sprite-host");
			// [sample binder-sprite-host]
			jai::dynamic_binder<Sprite>(*eng, "Sprite")
			    .constructor<>()
			    .serial_only_constructor()
			    .property("alpha", &Sprite::alpha);
			// [/sample]
			host.finish();
			expect_script("embedding/02-dynamic-binder.html", "binder-sprite", eng);
		});

		test("binder_atomics", [this]() {
			expect_cpp("embedding/02-dynamic-binder.html", "binder-atomics", [] {
				// [sample binder-atomics]
				std::atomic<int64_t> page_views{0};                // the host's: it outlives both engines

				auto front = jai::engine::make();
				auto back = jai::engine::make();
				for (jai::engine* eng : {front.get(), back.get()}) {
				    jai::stdlib::register_all(eng);
				    jai::stdlib::register_atomics(*eng);
				    eng->add_global_ref("page_views", page_views);    // a reference: no engine copies it
				}
				front->execute("page_views += 1; ++page_views;");
				back->execute(R"(
				    int before = page_views.fetch_add(10);
				    print("back saw {} and left {}", before, page_views.load());
				)");
				std::cout << "the host reads " << page_views.load() << "\n";
				// [/sample]
			});
		});

		test("binder_meter", [this]() {
			using namespace doc_binder_meter;
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample host(*this, "embedding/02-dynamic-binder.html", "binder-meter-host");
			// [sample binder-meter-host]
			jai::dynamic_binder<Meter>(*eng, "Meter")
			    .constructor<>()
			    .method("add", &Meter::add)
			    .method("reading", &Meter::reading)
			    .method("publish", &Meter::publish, jai::serial);  // const, but it reaches a shared manager
			eng->add_function("master", []() { return master_meter; });        // the program's one Meter (level 100)
			eng->add_function("log_line", [](const std::string& line) { logger().write(line); }, jai::serial);
			// [/sample]
			host.finish();
			expect_script("embedding/02-dynamic-binder.html", "binder-meter", eng);
		});

		test("binder_pickup", [this]() {
			using doc_binder_pickup::Pickup;
			using doc_binder_pickup::HealthPickup;
			const std::string page = "embedding/02-dynamic-binder.html";
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample host(*this, page, "binder-pickup-host");
			// [sample binder-pickup-host]
			// Registration order does not matter — either class may register first, or the base
			// not at all: a derived registration materializes an unregistered property_owner base.
			jai::dynamic_binder<Pickup>(*eng, "Pickup")
			    .auto_bind()
			    .method("kind", [](Pickup& p) { return std::string("pickup:") + p.name.get(); });
			jai::dynamic_binder<HealthPickup>(*eng, "HealthPickup")
			    .auto_bind();     // derives the Pickup base edge: healing and the inherited name both bind
			// [/sample]
			host.finish();
			cpp_sample json_block(*this, page, "binder-pickup-json");
			// [sample binder-pickup-json]
			std::shared_ptr<Pickup> loot = std::make_shared<HealthPickup>();
			loot->name = std::string("elixir");
			std::static_pointer_cast<HealthPickup>(loot)->healing = 40;

			auto lootJson = jai::to_json(*eng, loot);          // saved through the BASE pointer
			std::cout << lootJson << "\n";
			auto back = jai::from_json<std::shared_ptr<Pickup>>(*eng, lootJson);
			auto* hp = dynamic_cast<HealthPickup*>(back.get());   // != nullptr, healing == 40
			// [/sample]
			json_block.finish();
			cpp_sample describe_block(*this, page, "binder-describe-host");
			// [sample binder-describe-host]
			eng->add_function("describe", [](std::shared_ptr<Pickup> p) { return p->name.get(); });
			// [/sample]
			describe_block.finish();
			expect_script(page, "binder-pickup", eng);
		});

		test("signals_door_and_observable", [this]() {
			using doc_signals::Door;
			const std::string page = "embedding/10-signals-properties.html";
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample door_block(*this, page, "signals-door");
			// [sample signals-door]
			Door door;
			door.configure(eng.get());

			auto cppConn    = door.onOpen.connect([](int w) { std::cout << "C++ receiver: opened to " << w << "\n"; });
			auto scriptConn = door.onOpen.connect(
			    "print(\"script receiver: opened to \" + to_string(width));");

			door.open(42);          // both receivers fire
			scriptConn = nullptr;   // dropping the receiver disconnects
			door.open(7);           // only the C++ receiver remains
			// [/sample]
			door_block.finish();
			cpp_sample observable_block(*this, page, "signals-observable");
			// [sample signals-observable]
			jai::property_manager mgr;
			jai::observable_property<int> hp(mgr, "hp", 100);

			auto watch = hp.on_change.connect([](const int& oldV, const int& newV) {
			    std::cout << "hp: " << oldV << " -> " << newV << "\n";
			});
			hp = 75;              // emits (100 -> 75)
			hp += 10;             // arithmetic ops emit too (75 -> 85)
			hp.set_silent(50);    // NO signal — the loading/initialization path
			std::cout << "final hp: " << hp.get() << "\n";   // 50
			// [/sample]
			observable_block.finish();
			cpp_sample binder_block(*this, page, "signals-door-binder");
			// [sample signals-door-binder]
			jai::dynamic_binder<Door>(*eng, "Door")
			    .constructor<>()
			    .method("open", &Door::open)
			    .property("onOpen", &Door::onOpen);    // the whole script exposure
			// [/sample]
			binder_block.finish();
			expect_script(page, "signals-door-script", eng);
		});

		test("signals_spawner", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			jai::bind_registrar(*eng);
			expect_script("embedding/10-signals-properties.html", "signals-spawner", eng);
		});

		test("math_adapters", [this]() {
			using doc_math::Body;
			using doc_math::Marker;
			const std::string page = "embedding/15-math-adapters.html";
			for (const char* declarations : {"math-types", "math-body", "math-marker-class"}) {
				cpp_sample block(*this, page, declarations);
				block.finish();
			}
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample host(*this, page, "math-host");
			// [sample math-host]
			using game::Vec3;
			using game::Cell;
			using game::Rgba;

			eng->add_function("scale", [](Vec3 p, float k) { return Vec3{p.x * k, p.y * k, p.z * k}; });
			eng->add_function("nudge", [](Vec3& p) { p.x += 100; });          // written back into the script's vector
			eng->add_function("neighbor", [](Cell c) { return Cell{c.col + 1, c.row}; });
			eng->add_function("faded", [](Rgba c) { return Rgba{c.R, c.G, c.B, c.A * 0.5f}; });
			eng->add_function("make_path", [](int n) {
			    std::vector<Vec3> out;
			    for (int i = 0; i < n; ++i) { out.push_back(Vec3{float(i), float(i) * 2, 1}); }
			    return out;                                                     // arrives as array<math::vec3>
			});
			eng->add_function("path_length", [](std::vector<Vec3> path) { return static_cast<int64_t>(path.size()); });

			jai::dynamic_binder<Body>(*eng, "Body")
			    .property("pos", &Body::pos)
			    .property("tint", &Body::tint)
			    .property("path", &Body::path)
			    .method("moved", &Body::moved);
			// [/sample]
			host.finish();
			expect_script(page, "math-functions", eng);
			expect_script(page, "math-body-script", eng);
			cpp_sample marker_block(*this, page, "math-marker");
			// [sample math-marker]
			jai::dynamic_binder<Marker>(*eng, "Marker").auto_bind();

			auto marker = std::make_shared<Marker>();
			int changes = 0;
			auto watch = marker->velocity.on_change.connect([&](const game::Vec3&, const game::Vec3&) { ++changes; });
			eng->add_global("marker", eng->make_object(marker));

			eng->execute(R"(
			    print(type_of(marker.position), " ", marker.tint);
			    marker.position = math::vec3(1, 2, 3);
			    marker.position.x += 4;
			    marker.velocity.y = 2;
			    marker.velocity = math::vec3(1, 1, 1);
			    marker.tint.a = 0.5;
			)");
			std::cout << "position " << marker->position->x << " " << marker->position->y << " " << marker->position->z << "\n";
			std::cout << "velocity changes " << changes << ", tint alpha " << marker->tint->A << "\n";
			// [/sample]
			marker_block.finish();
			cpp_sample value_block(*this, page, "math-value");
			// [sample math-value]
			jai::script_value v = eng->make_value(game::Vec3{1, 2, 3});         // a math::vec3 value
			eng->add_global("spawn_point", v);
			game::Vec3 back = eng->execute("spawn_point * 2").as<game::Vec3>();  // and back to the host type
			std::cout << v.to_string() << " -> " << back.x << " " << back.y << " " << back.z << "\n";
			// [/sample]
			value_block.finish();
		});

		test("pool_partial_sums", [this]() {
			cpp_sample run(*this, "embedding/16-thread-pool.html", "pool-partial-sums");
			// [sample pool-partial-sums]
			jai::thread_pool pool(4);
			const size_t workers = pool.worker_count();
			std::vector<int64_t> partial(workers);
			for (size_t w = 0; w < workers; ++w) {
			    pool.submit_to(w, [&partial, w, workers] {
			        for (int64_t i = int64_t(w); i < 1000; i += int64_t(workers)) { partial[w] += i; }
			    });
			}
			pool.wait_idle();
			int64_t sum = 0;
			for (int64_t p : partial) { sum += p; }
			std::cout << sum << "\n";
			// [/sample]
			run.finish();
		});

		test("pool_on_exception", [this]() {
			cpp_sample run(*this, "embedding/16-thread-pool.html", "pool-on-exception");
			// [sample pool-on-exception]
			jai::thread_pool pool(1);
			pool.on_exception([](std::exception_ptr error) {
			    try {
			        std::rethrow_exception(error);
			    } catch (const std::exception& e) {
			        std::cout << "on_exception: " << e.what() << "\n";
			    }
			});
			pool.submit([] { throw std::runtime_error("decode failed"); },
			            [] { std::cout << "then runs anyway\n"; });
			pool.wait_idle();
			// [/sample]
			run.finish();
		});

		test("pool_posted", [this]() {
			cpp_sample run(*this, "embedding/16-thread-pool.html", "pool-posted");
			// [sample pool-posted]
			jai::thread_pool pool;
			std::vector<std::string> levels(3);
			std::vector<jai::thread_pool::task_type> loads;
			for (size_t i = 0; i < levels.size(); ++i) {
			    loads.push_back([&levels, i] { levels[i] = "level_" + std::to_string(i); });
			}
			bool ready = false;
			pool.submit_batch(std::move(loads), [&] { ready = true; }, jai::thread_pool::then_on::posted);

			while (!ready) {
			    // one frame: input, update, render, then the work the pool handed back
			    pool.run_posted();
			}
			std::cout << levels[0] << " " << levels[1] << " " << levels[2] << "\n";
			// [/sample]
			run.finish();
		});

		test("chai_bridge_monster", [this]() {
			using doc_chai::Entity;
			using doc_chai::Monster;
			const std::string page = "bridges/06-from-chaiscript.html";
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample host(*this, page, "chai-monster-binding");
			// [sample chai-monster-binding]
			jai::dynamic_binder<Entity>(*eng, "Entity")
			    .property("name", &Entity::name)
			    .method("describe", &Entity::describe);

			jai::dynamic_binder<Monster>(*eng, "Monster")
			    .base_class<Entity>()
			    .constructor<>()
			    .constructor<std::string, int64_t>()
			    .method("damage", static_cast<void (Monster::*)(int64_t)>(&Monster::damage))
			    .method("damage", static_cast<void (Monster::*)(int64_t, double)>(&Monster::damage))
			    .method("alive", &Monster::alive)
			    .method("==", &Monster::operator==)
			    .property("hp", &Monster::health, &Monster::setHealth)
			    .property("maxHp", [](const Monster&) { return int64_t{100}; }, nullptr);

			eng->add_function("describe", [](std::shared_ptr<Entity> e) { return e->describe(); });
			// [/sample]
			host.finish();
			cpp_sample types(*this, page, "chai-monster-types");
			types.finish();
			expect_script(page, "chai-monster", eng);
		});


		test("serialization_convenience", [this]() {
			using doc_serialization::SaveGame;
			using doc_serialization::Node;
			using doc_serialization::Link;
			const std::string page = "embedding/08-serialization.html";
			auto eng = make_engine();
			cpp_sample save_block(*this, page, "serialization-savegame");
			// [sample serialization-savegame]
			SaveGame save{7, 123.5, "Alice", {1, 2, 3}};
			std::string json = jai::to_json(*eng, save);                  // human-readable, indented 2 spaces
			SaveGame loaded  = jai::from_json<SaveGame>(*eng, json);
			std::cout << json << "\n";
			std::cout << jai::to_json(*eng, save, 0) << "\n";              // indent 0: one line
			std::cout << "loaded: level=" << loaded.level << " hero=" << loaded.hero << " items=" << loaded.inventory.size() << "\n";

			std::vector<uint8_t> blob = jai::to_binary(*eng, save);       // compact little-endian
			SaveGame fromBin = jai::from_binary<SaveGame>(*eng, blob);
			std::cout << "binary: " << blob.size() << " bytes, hero=" << fromBin.hero << "\n";
			// [/sample]
			save_block.finish();
			cpp_sample link_block(*this, page, "serialization-link");
			// [sample serialization-link]
			Link link;
			link.a = std::make_shared<Node>();
			link.b = link.a;
			link.w = link.a;
			std::cout << jai::to_json(*eng, link, 0) << "\n";
			// [/sample]
			link_block.finish();
			using doc_serialization::Enemy;
			cpp_sample enemy_block(*this, page, "serialization-binding-record");
			// [sample serialization-binding-record]
			jai::dynamic_binder<Enemy>(*eng, "Enemy")
			    .property("name", &Enemy::name)
			    .property("hp", &Enemy::hp)
			    .property("loot", &Enemy::loot);

			Enemy boss{"troll", 40, {"club", "tooth"}};
			std::string record = jai::to_json(*eng, boss, 0);
			Enemy back = jai::from_json<Enemy>(*eng, record);
			std::cout << record << "\n";
			std::cout << "loaded: " << back.name << " hp=" << back.hp << " loot=" << back.loot.size() << "\n";
			// [/sample]
			enemy_block.finish();
		});

		test("coroutine_driving", [this]() {
			const std::string page = "embedding/09-coroutine-hosting.html";
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample script_block(*this, page, "coroutine-drive-script");
			// [sample coroutine-drive-script]
			eng->execute(R"( coroutine int spawner(int n) {
			                     for (int i = 1; i <= n; ++i) { yield i * 10; }
			                     return -1;
			                 }
			                 var c = spawner(3); )");

			for (int i = 0; i < 4; ++i) {
			    auto v = eng->execute("c.next();");                   // 10, 20, 30, -1
			    bool finished = eng->execute("c.done()").as<bool>();  // false ×3, then true
			}
			// [/sample]
			script_block.finish();
			cpp_sample native_block(*this, page, "coroutine-native-handle");
			// [sample coroutine-native-handle]
			eng->execute("var c2 = spawner(2);");
			auto holder = eng->execute("c2").get_object_holder();
			auto handle = std::static_pointer_cast<jai::coroutine_handle>(holder->host_object());

			while (!handle->done()) {
			    jai::checked_result<jai::script_value> step = handle->next(eng.get());
			    if (!step) break;                     // error channel (checked_result)
			    std::cout << "C++ next -> " << step.value().as_int() << "\n";   // 10, 20, then -1 (the return value)
			}
			// [/sample]
			native_block.finish();
		});

		test("safety_rails_budgets", [this]() {
			const std::string page = "embedding/05-safety-rails.html";
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample wall_clock(*this, page, "safety-execution-budget");
			// [sample safety-execution-budget]
			eng->execution_budget(0.05);
			try {
			    eng->execute("var caught = 0; try { while (true) { } } catch (e) { caught = 1; } caught;");
			} catch (const std::exception& e) {
			    std::cout << e.what() << "\n";          // the script's catch never saw it
			}
			std::cout << eng->execute<int>("2 + 2") << "\n";   // the engine survives; each execute gets a fresh deadline
			// [/sample]
			wall_clock.finish();
			eng->execution_budget(5.0);
			cpp_sample steps(*this, page, "safety-step-budget");
			// [sample safety-step-budget]
			eng->execution_step_budget(4);       // about 4 * 1024 back-edges and calls per execute
			try {
			    eng->execute("int n = 0; try { while (true) { n++; } } catch (e) { n = -1; } n;");
			} catch (const std::exception& e) {
			    std::cout << e.what() << "\n";
			}
			std::cout << eng->execute("int k = 0; for (int i = 0; i < 3; i++) { k++; } k;").as_int() << "\n";   // each entry starts afresh
			eng->execution_step_budget(0);       // off
			// [/sample]
			steps.finish();
		});

		test("hot_reload_fields", [this]() {
			auto eng = make_engine();
			jai::stdlib::register_all(eng);
			cpp_sample run(*this, "embedding/07-hot-reload.html", "hot-reload-fields");
			// [sample hot-reload-fields]
			eng->execute(R"( class Player { int hp = 100; float speed = 1.0; string name = "Unnamed"; }
			                 var p = Player(); p.hp = 62; p.name = "Alice"; )");

			eng->execute(R"( class Player {
			                     string hp = "0";          // retyped
			                     string name = "Unnamed";  // kept
			                     int level = 1;            // new
			                     // speed removed
			                     void hot_reload_migrate() { level = 5; }
			                 } )");

			eng->execute(R"( print("hp: ", type_of(p.hp), " ", p.hp);
			                 print("name: ", p.name);
			                 print("level: ", p.level);
			                 try { print(p.speed); } catch (e) { print("speed: ", e); } )");
			// [/sample]
			run.finish();
		});

		test("quickstart_errors", [this]() {
			expect_cpp("embedding/01-embedding-quickstart.html", "quickstart-errors", [] {
				auto eng = jai::engine::make();
				jai::stdlib::register_all(eng);
				// [sample quickstart-errors]
				try {
				    eng->execute("undefined_thing + 1");           // rejected before anything runs
				} catch (const std::exception& e) {
				    std::cout << e.what() << "\n";
				}
				try {
				    eng->execute(R"(throw "out of mana";)");       // a throw nothing caught
				} catch (const std::exception& e) {
				    std::cout << "script threw: " << e.what() << "\n";
				}
				eng->execute("int risky(int x) { return 10 / x; }");
				auto r = eng->get_variable("risky").as_function()({eng->make_value(int64_t{0})});
				if (!r) { std::cout << "risky(0) failed: " << eng->format_error(r) << "\n"; }   // a call reports, never throws
				std::cout << "the engine carries on: " << eng->execute("40 + 2").as_int() << "\n";
				// [/sample]
			});
		});
	}
};

}  // namespace jai::foundry::tests

using doc_samples_tests = jai::foundry::tests::doc_samples_tests;
FOUNDRY_REGISTER(doc_samples_tests)
