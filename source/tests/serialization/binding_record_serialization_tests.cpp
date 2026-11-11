#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/core/registrar.hpp>
#include <jaiscript/properties.hpp>
#include <jaiscript/properties/property_serialization.hpp>
#include <jaiscript/serialization/convenience.hpp>
#include <jaiscript/stdlib/stdlib.hpp>

// Classes the tests cannot edit: no serialize, save, load or property_owner, only a dynamic_binder in the engine
namespace binding_record_types {

struct Stat {
	int hp = 10;
	std::string name = "rat";
	double speed = 1.5;
};

struct Item {
	int weight = 3;
};

struct Pos {
	double x = 1;
	double y = 2;
};

enum class Mood { calm, angry };

struct Rich {
	std::vector<int> list{1, 2};
	std::map<std::string, int> bag{{"a", 1}};
	std::shared_ptr<Item> first = std::make_shared<Item>();
	std::shared_ptr<Item> second = first;
	Pos pos;
	Mood mood = Mood::angry;
	int cache = 5;
	const int fixed = 9;
	int getSecret() const { return secret; }
	void setSecret(int a_value) { secret = a_value; }
	int area() const { return 42; }
	int secret = 4;
};

struct Shape {
	virtual ~Shape() = default;
	int sides = 0;
};

struct Square : Shape {
	double edge = 2;
};

struct Circle : Shape {
	double radius = 1;
};

// an explicit route wins over the binding's properties
struct Routed {
	int value = 1;
	template<class Archive> void serialize(Archive& a_archive) { a_archive("routed", value); }
};

// a free serialize, or a free save and load pair, beside a class the tests cannot edit also wins
struct FreeRouted {
	int value = 1;
};

template<class Archive> void serialize(Archive& a_archive, FreeRouted& a_routed) { a_archive("free", a_routed.value); }

struct FreeSaved {
	int value = 1;
};

template<class Archive> void save(Archive& a_archive, const FreeSaved& a_saved) { a_archive("saved", a_saved.value); }
template<class Archive> void load(Archive& a_archive, FreeSaved& a_saved) { a_archive("saved", a_saved.value); }

struct Unbound {
	int value = 1;
};

struct Healed {
	int hp = 1;
	int doubled = 0;
};

struct Holder : jai::property_owner<Holder> {
	JAI_PROPERTY((Stat), stat);
	JAI_PROPERTY((std::vector<std::shared_ptr<Shape>>), shapes);
};

struct Registered {
	virtual ~Registered() = default;
	int level = 1;
};

struct RegisteredChild : Registered {
	int bonus = 2;
};

}

static jai::registrar<binding_record_types::RegisteredChild, void, jai::bases<binding_record_types::Registered>> bindingRecordChild("BindingRecordChild",
	[](jai::dynamic_binder<binding_record_types::RegisteredChild>& a_builder) {
	a_builder.base_class<binding_record_types::Registered>().property("bonus", &binding_record_types::RegisteredChild::bonus);
});

namespace jai::foundry::tests {

class binding_record_serialization_tests : public suite {
public:
	binding_record_serialization_tests() : suite("Binding Record Serialization") {}

	static void bind_all(engine& a_engine) {
		using namespace binding_record_types;
		stdlib::register_all(a_engine);
		dynamic_binder<Stat>(a_engine, "Stat").property("hp", &Stat::hp).property("name", &Stat::name).property("speed", &Stat::speed).build();
		dynamic_binder<Item>(a_engine, "Item").property("weight", &Item::weight).build();
		dynamic_binder<Pos>(a_engine, "Pos").property("x", &Pos::x).property("y", &Pos::y).build();
		dynamic_binder<Rich>(a_engine, "Rich")
			.property("list", &Rich::list).property("bag", &Rich::bag).property("first", &Rich::first).property("second", &Rich::second)
			.property("pos", &Rich::pos).property("mood", &Rich::mood).transient_property("cache", &Rich::cache).property("fixed", &Rich::fixed)
			.property("secret", &Rich::getSecret, &Rich::setSecret).property("area", &Rich::area, nullptr).build();
		dynamic_binder<Shape>(a_engine, "Shape").property("sides", &Shape::sides).build();
		dynamic_binder<Square>(a_engine, "Square").base_class<Shape>().property("edge", &Square::edge).build();
		dynamic_binder<Circle>(a_engine, "Circle").base_class<Shape>().property("radius", &Circle::radius).build();
		dynamic_binder<Routed>(a_engine, "Routed").property("value", &Routed::value).build();
		dynamic_binder<FreeRouted>(a_engine, "FreeRouted").property("value", &FreeRouted::value).build();
		dynamic_binder<FreeSaved>(a_engine, "FreeSaved").property("value", &FreeSaved::value).build();
		dynamic_binder<Healed>(a_engine, "Healed").property("hp", &Healed::hp).transient_property("doubled", &Healed::doubled)
			.post_load_hook([](Healed& a_self) { a_self.doubled = a_self.hp * 2; }).build();
		dynamic_binder<Registered>(a_engine, "BindingRecordBase").property("level", &Registered::level).build();
		bind_registrar(a_engine);
	}

	static std::string failure(const std::function<void()>& a_action) {
		try {
			a_action();
		} catch (const serialization_error& e) {
			return e.what();
		}
		return "no error";
	}

	void forge_tests() override {
		using namespace binding_record_types;

		test("a_bound_class_writes_and_reads_the_properties_its_binding_declares", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			Stat stat;
			stat.hp = 33;
			stat.name = "ox";
			const std::string json = to_json(*eng, stat, 0);
			check_eq(std::string("{\"hp\": 33,\"name\": \"ox\",\"speed\": 1.5}"), json);
			const Stat fromJson = from_json<Stat>(*eng, json);
			check_eq(33, fromJson.hp);
			check_eq(std::string("ox"), fromJson.name);
			const Stat fromBinary = from_binary<Stat>(*eng, to_binary(*eng, stat));
			check_eq(33, fromBinary.hp);
			check_eq(std::string("ox"), fromBinary.name);
			const Stat partial = from_json<Stat>(*eng, "{\"hp\": 2, \"retired\": 7}");
			check_eq(2, partial.hp, "a key the record holds loads");
			check_eq(std::string("rat"), partial.name, "a key it lacks keeps the constructor's value, and an unknown key is skipped");

			auto shared = std::make_shared<Stat>();
			shared->hp = 8;
			const std::vector<std::shared_ptr<Stat>> twice{shared, shared};
			for (bool binary : {false, true}) {
				const auto loaded = binary ? from_binary<std::vector<std::shared_ptr<Stat>>>(*eng, to_binary(*eng, twice))
				                           : from_json<std::vector<std::shared_ptr<Stat>>>(*eng, to_json(*eng, twice));
				check(loaded.size() == 2 && loaded[0] == loaded[1] && loaded[0]->hp == 8, binary ? "shared identity, binary" : "shared identity, JSON");
			}
		});

		test("every_member_shape_and_accessor_pair_reaches_the_record", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			Rich rich;
			rich.list.push_back(9);
			rich.bag["b"] = 2;
			rich.first->weight = 12;
			rich.pos.x = 5;
			rich.mood = Mood::calm;
			rich.cache = 77;
			rich.setSecret(21);
			const std::string json = to_json(*eng, rich, 0);
			check_eq(std::string("{\"list\": [1,2,9],\"bag\": {\"a\": 1,\"b\": 2},\"first\": {\"$id\": 1,\"$val\": {\"weight\": 12}},\"second\": {\"$id\": 1},"
				"\"pos\": {\"x\": 5.0,\"y\": 2.0},\"mood\": 0,\"secret\": 21}"), json,
				"a transient property, a const member and a getter-only property stay out");
			for (bool binary : {false, true}) {
				const Rich loaded = binary ? from_binary<Rich>(*eng, to_binary(*eng, rich)) : from_json<Rich>(*eng, json);
				const std::string format = binary ? "binary" : "JSON";
				check_eq(size_t(3), loaded.list.size(), format);
				check_eq(2, loaded.bag.at("b"), format);
				check(loaded.first == loaded.second && loaded.first->weight == 12, "one object behind both pointers, " + format);
				check_eq(5.0, loaded.pos.x, format);
				check(loaded.mood == Mood::calm, "the enum, " + format);
				check_eq(21, loaded.getSecret(), "the setter, " + format);
				check_eq(5, loaded.cache, "the transient property keeps its constructed value, " + format);
			}
		});

		test("base_class_levels_and_polymorphic_pointers_load_their_dynamic_type", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			auto square = std::make_shared<Square>();
			square->sides = 4;
			square->edge = 6;
			auto circle = std::make_shared<Circle>();
			circle->radius = 3;
			const std::vector<std::shared_ptr<Shape>> shapes{square, circle, square};
			const std::string json = to_json(*eng, shapes, 0);
			check(json.find("\"$type\": \"Square\",\"$val\": {\"sides\": 4,\"edge\": 6.0}") != std::string::npos, "the base level, then the class's own: " + json);
			for (bool binary : {false, true}) {
				const auto loaded = binary ? from_binary<std::vector<std::shared_ptr<Shape>>>(*eng, to_binary(*eng, shapes))
				                           : from_json<std::vector<std::shared_ptr<Shape>>>(*eng, json);
				const auto* asSquare = dynamic_cast<const Square*>(loaded.at(0).get());
				const auto* asCircle = dynamic_cast<const Circle*>(loaded.at(1).get());
				check(asSquare && asSquare->sides == 4 && asSquare->edge == 6.0, binary ? "Square, binary" : "Square, JSON");
				check(asCircle && asCircle->radius == 3.0, binary ? "Circle, binary" : "Circle, JSON");
				check(loaded.at(2) == loaded.at(0), "identity through the base pointer");
			}

			std::shared_ptr<Registered> child = std::make_shared<RegisteredChild>();
			const std::string childJson = to_json(*eng, child, 0);
			check(childJson.find("\"$type\": \"BindingRecordChild\",\"$val\": {\"level\": 1,\"bonus\": 2}") != std::string::npos, "a registrar's binding: " + childJson);
			check(dynamic_cast<RegisteredChild*>(from_json<std::shared_ptr<Registered>>(*eng, childJson).get()) != nullptr, "loads its registered type");
		});

		test("a_binding_names_its_type_in_each_engine", [this]() {
			auto first = make_engine();
			bind_all(*first);
			auto second = make_engine();
			stdlib::register_all(*second);
			dynamic_binder<Shape>(*second, "Polygon").property("sides", &Shape::sides).build();
			dynamic_binder<Square>(*second, "Tile").base_class<Shape>().property("edge", &Square::edge).build();
			const std::shared_ptr<Shape> square = std::make_shared<Square>();
			check(to_json(*first, square, 0).find("\"Square\"") != std::string::npos, "the first engine's name");
			const std::string tile = to_json(*second, square, 0);
			check(tile.find("\"Tile\"") != std::string::npos, "the second engine's: " + tile);
			check(dynamic_cast<Square*>(from_json<std::shared_ptr<Shape>>(*second, tile).get()) != nullptr, "each loads its own name");
			check_eq(std::string("Unknown polymorphic type: Tile"), failure([&]() { from_json<std::shared_ptr<Shape>>(*first, tile); }), "and no other engine's");
		});

		test("an_explicit_route_wins_and_an_unbound_class_throws_where_the_archive_reaches_it", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			check_eq(std::string("{\"routed\": 1}"), to_json(*eng, Routed{}, 0));
			check_eq(std::string("{\"free\": 1}"), to_json(*eng, FreeRouted{}, 0));
			check_eq(7, from_json<FreeRouted>(*eng, "{\"free\": 7}").value);
			check_eq(std::string("{\"saved\": 1}"), to_json(*eng, FreeSaved{}, 0));
			check_eq(8, from_json<FreeSaved>(*eng, "{\"saved\": 8}").value);
			const std::string unwritable = failure([&]() { to_json(*eng, Unbound{}); });
			check(unwritable.find("Unbound' is not serializable. Add save/load, serialize, property_owner, or ADL functions, bind it in the archive's engine") != std::string::npos,
				unwritable);
			const std::string unreadable = failure([&]() { from_json<Unbound>(*eng, "{\"value\": 2}"); });
			check(unreadable.find("Unbound' is not deserializable.") != std::string::npos, unreadable);
			auto other = make_engine();
			check(failure([&]() { to_json(*other, Stat{}); }).find("is not serializable.") != std::string::npos, "an engine that does not bind the class");
		});

		test("a_property_owner_holds_binding_records", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			Holder holder;
			holder.stat->hp = 5;
			holder.shapes->push_back(std::make_shared<Square>());
			const std::string json = to_json(*eng, holder, 0);
			const Holder back = from_json<Holder>(*eng, json);
			check_eq(5, back.stat->hp, json);
			check(back.shapes->size() == 1 && dynamic_cast<Square*>(back.shapes->at(0).get()) != nullptr, json);
		});

		test("the_bindings_post_load_hook_runs_after_a_load", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			check_eq(14, from_json<Healed>(*eng, "{\"hp\": 7}").doubled);
			check_eq(6, from_binary<Healed>(*eng, to_binary(*eng, Healed{3, 0})).doubled);
			check_eq((int64_t)8, eng->execute("var h = Healed(); h.hp = 4; from_json(to_json(h)).doubled").as_int(), "a hook without the version, in script");
		});

		test("script_values_holding_bound_objects_round_trip", [this]() {
			auto eng = make_engine();
			bind_all(*eng);
			eng->execute(R"(
				class Box { Rich rich; Stat stat; }
				var item = new Item();
				item.weight = 8;
				var box = Box();
				box.rich.list.push_back(5);
				box.rich.pos.x = 7;
				box.rich.secret = 11;
				box.rich.mood = 0;
				box.rich.first = item;
				box.rich.second = item;
				box.stat.hp = 9;
				var other = Rich();
				other.first = item;
				var all = [box, other];
			)");
			check_eq(std::string("{\"$type\":\"Rich\",\"$val\":{\"list\":[1,2,5],\"bag\":{\"a\":1},\"first\":{\"$id\":1,\"$type\":\"Item\",\"$val\":{\"weight\":8}},"
				"\"second\":{\"$id\":1},\"pos\":{\"$type\":\"Pos\",\"$val\":{\"x\":7.0,\"y\":2.0}},\"mood\":0,\"cache\":5,\"fixed\":9,\"secret\":11,\"area\":42}}"),
				eng->execute("to_json(box.rich)").to_string());
			for (const char* load : {"from_json(to_json(all))", "from_binary(to_binary(all))"}) {
				eng->execute(std::string("var back = ") + load + ";");
				const std::string state = eng->execute(R"(
					var b = back[0];
					b.rich.second.weight = 30;
					to_string(b.rich.list.size()) + " " + to_string(b.rich.pos.x) + " " + to_string(b.rich.secret) + " " + to_string(b.rich.mood) + " " +
						to_string(b.rich.first.weight) + " " + to_string(back[1].first.weight) + " " + to_string(b.stat.hp)
				)").to_string();
				check_eq(std::string("3 7.0 11 0 30 30 9"), state, std::string("one Item behind three pointers across two objects: ") + load);
			}
		});
	}
};

}

using binding_record_serialization_tests = jai::foundry::tests::binding_record_serialization_tests;
FOUNDRY_REGISTER(binding_record_serialization_tests)
