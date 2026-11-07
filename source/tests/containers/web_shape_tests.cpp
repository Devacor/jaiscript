#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
#include <jaiscript/serialization/json_archive.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <string>

namespace jai::foundry::tests {

// Web-shaped workloads (string-keyed map counters, JSON documents, object sorts): their results pinned, both backends
class web_shape_tests : public suite {
public:
	web_shape_tests() : suite("Web Shapes") {}

	void forge_tests() override {
		// m[k] op= v on the entries of string (lengths 0 to 36), float and int maps, its value as an expression, ++/--, the zero texts, conversion into an int value, a missing key's error, NaN and signed-zero float keys, string +=; keys found again through copies with their own nodes
		test("compound_assignments_on_map_entries_and_string_keys_of_every_length", [this]() {
			auto e = make_engine();
			e->execution_budget(0);   // a Debug build runs these past the default budget
			jai::stdlib::register_all(e);
			const std::string got = e->execute(R"JAI(
map<string, int> m = {};
array<string> words = ["", "a", "ab", "abc", "abcd", "abcde", "abcdefg", "abcdefgh", "abcdefghi", "abcdefghijklmno", "abcdefghijklmnop", "abcdefghijklmnopq", "abcdefghijklmnopqrstuvwxyz0123456789", "\xff\xfe", "a\x01b"];
for (auto& w : words) { m[w] = 0; }
for (int r = 0; r < 3; ++r) {
	for (auto& w : words) { m[w] += r + 1; }
	for (auto& w : words) { m[w + ""] -= 1; }
}
string out = "";
for (auto [k, v] : m) { out += to_string(k.size()) + ":" + to_string(v) + ","; }
for (auto& w : words) { string copy = w; copy.append("!"); copy = copy.substr(0, copy.size() - 1); out += to_string(m[copy]); }
map<string, float> f = {"x": 0.0, "y": 0.0};
f["x"] += 1.5; f["x"] *= 4; f["y"] -= 2;
out += " " + to_string(f["x"]) + " " + to_string(f["y"]);
map<int, int> im = {};
for (int i = 0; i < 37; ++i) { im[i] = 0; }
for (int i = 0; i < 1000; ++i) { im[i % 37] += i; im[i % 5] *= 1; }
int total = 0; for (auto [k, v] : im) { total = total * 31 % 1000003 + v; }
out += " " + to_string(total) + " " + to_string(im.size());
int x = (m["abc"] += 10);
int y = m["abc"]++;
int z = ++m["abc"];
out += " " + to_string(x) + " " + to_string(y) + " " + to_string(z) + " " + to_string(m["abc"]);
map<string, int> d = {"k": 7};
try { d["k"] /= 0; } catch (e) { out += " div:" + e.message; }
try { d["k"] %= 0; } catch (e) { out += " mod:" + e.message; }
d["k"] += 2.9;
out += " " + to_string(d["k"]);
var vm = {};
vm["n"] = 1; vm["n"] += 2.5;
try { vm["missing"] += 1; } catch (e) { out += " missing:" + e.message; }
map<float, int> fk = {};
fk[0.0] = 0; fk[math::sqrt(-1.0)] = 0;
fk[0.0] += 1; fk[-0.0] += 1; fk[math::sqrt(-1.0)] += 1; fk[math::sqrt(-1.0)] += 1;
out += " " + to_string(vm["n"]) + " " + to_string(fk.size()) + " " + to_string(fk[0.0]);
map<string, string> sm = {"a": ""};
sm["a"] += "x"; sm["a"] += "y";
out += " " + sm["a"];
var bad_src = {"z": "s", "b": [1], "a": true, "m": 2};
try { map<string, int> bad = bad_src; } catch (e) { out += " bad:" + e.message; }
var bad_keys = {"z": 1, 2: 2, 1.5: 3};
try { map<string, int> bk = bad_keys; } catch (e) { out += " badkey:" + e.message; }
map<string, float> widened = {"a": 1, "b": 2.5};
out += " " + to_string(widened["a"]);
out;
)JAI").to_string();
			check_eq(std::string(R"OUT(0:3,1:3,3:3,2:3,3:3,4:3,5:3,7:3,8:3,9:3,15:3,16:3,17:3,36:3,2:3,333333333333333 6.0 -2.0 727059 37 13 13 15 15 div:Division by zero mod:Modulo by zero 9 missing:Key "missing" not found in map 3.5 2 2 xy bad:Cannot store array in map<string,int> badkey:Cannot store int in map<string,int> 1.0)OUT"), got);
		});

		// to_json compact and indented over scalars, escapes, containers, tagged maps, math values, instances and handles; from_json of plain documents (duplicate keys keep the first), archive records, and documents whose maps share key strings
		test("to_json_and_from_json_texts", [this]() {
			auto e = make_engine();
			e->execution_budget(0);   // a Debug build runs these past the default budget
			jai::stdlib::register_all(e);
			const std::string got = e->execute(R"JAI(
class Pt { int x = 1; float y = 2.5; string name = "p\"q"; array<int> tags = [1, 2]; }
class Hold { shared_ptr<Pt> a; shared_ptr<Pt> b; weak_ptr<Pt> w; }
var docs = [null, true, -9223372036854775807, 1.0, 0.1, -0.0, 1e300, 1.0 / 3.0, "", "q\" b\\ / \n\r\t\x07\x01\x1f \xc3\xa9", 'c',
	[1, "two", 3.0, [4, [5, {}]]], {"b": 1, "a": 2, "c": {"z": [], "y": {}}}, {1: "one", 2: "two"}, {"$id": 2},
	math::vec2(1.5, -2.0), math::ivec3(1, 2, 3), Pt()];
string out = "";
for (auto& d : docs) { out += to_json(d) + "|" + to_json(d, 1) + "|"; }
Hold h;
h.a = new Pt();
h.b = h.a;
h.w = h.a;
out += to_json(h) + "|";
map<int, string> empty_ints = {};
array<float> fl = [1.0, 0.000001, 123456789.0];
out += to_json(empty_ints) + to_json(fl) + "|";
var texts = ["{\"a\":1,\"b\":[1,2,{\"c\":\"d\"}],\"a\":2}", " {\"k\" : \"v\\u00e9\\n\" , \"n\": -1.5e3, \"t\": true, \"z\": null} ",
	"{\"$char\": 65}", "{\"$map\": [[1, \"x\"]]}", "{\"$type\": \"math::vec2\", \"$val\": {\"x\": 1, \"y\": 2}}",
	"[{\"id\":1,\"tags\":[\"a\"]},{\"tags\":[],\"id\":2}]", "1e999", "{\"$id\": 1, \"$type\": \"Pt\", \"$val\": {\"x\": 7}}"];
for (auto& t : texts) {
	var back = from_json(t);
	out += to_json(back) + ":" + type_of(back) + "|";
}
var rows = from_json("[{\"name\":\"a\",\"v\":1},{\"name\":\"b\",\"v\":2},{\"v\":3,\"name\":\"c\"}]");
rows[0]["name"] += "x";
rows[1]["extra"] = true;
for (auto& o : rows) { for (auto [k, v] : o) { out += k + "=" + to_string(v) + ","; } }
try { from_json("{\"a\":1,}"); } catch (e) { out += "|" + e.message; }
try { from_json("[1 2]"); } catch (e) { out += "|" + e.message; }
out.replace("\xc3\xa9", "<e9>");
out;
)JAI").to_string();
			check_eq(std::string(R"OUT(null|null|true|true|-9223372036854775807|-9223372036854775807|1.0|1.0|0.1|0.1|-0.0|-0.0|1e+300|1e+300|0.3333333333333333|0.3333333333333333|""|""|"q\" b\\ / \n\r\t\u0007\u0001\u001f <e9>"|"q\" b\\ / \n\r\t\u0007\u0001\u001f <e9>"|{"$char":99}|{"$char":99}|[1,"two",3.0,[4,[5,{}]]]|[
 1,
 "two",
 3.0,
 [
  4,
  [
   5,
   {}
  ]
 ]
]|{"a":2,"b":1,"c":{"y":{},"z":[]}}|{
 "a": 2,
 "b": 1,
 "c": {
  "y": {},
  "z": []
 }
}|{"$map":[[1,"one"],[2,"two"]]}|{
 "$map": [
  [
   1,
   "one"
  ],
  [
   2,
   "two"
  ]
 ]
}|{"$id":2}|{
 "$id": 2
}|{"$type":"math::vec2","$val":{"x":1.5,"y":-2.0}}|{"$type":"math::vec2","$val":{"x":1.5,"y":-2.0}}|{"$type":"math::ivec3","$val":{"x":1,"y":2,"z":3}}|{"$type":"math::ivec3","$val":{"x":1,"y":2,"z":3}}|{"$type":"Pt","$val":{"x":1,"y":2.5,"name":"p\"q","tags":[1,2]}}|{
 "$type": "Pt",
 "$val": {
  "x": 1,
  "y": 2.5,
  "name": "p\"q",
  "tags": [
   1,
   2
  ]
 }
}|{"$type":"Hold","$val":{"a":{"$id":1,"$type":"Pt","$val":{"x":1,"y":2.5,"name":"p\"q","tags":[1,2]}},"b":{"$id":1},"w":{"$ref":1}}}|{"$map":[]}[1.0,1e-06,123456789.0]|{"a":1,"b":[1,2,{"c":"d"}]}:map|{"k":"v<e9>\n","n":-1500.0,"t":true,"z":null}:map|{"$char":65}:char|{"$map":[[1,"x"]]}:map|{"$type":"math::vec2","$val":{"x":1.0,"y":2.0}}:math::vec2|[{"id":1,"tags":["a"]},{"id":2,"tags":[]}]:array|{"$float":"inf"}:float|{"$id":1,"$type":"Pt","$val":{"x":7,"y":2.5,"name":"p\"q","tags":[1,2]}}:Pt|name=ax,v=1,extra=true,name=b,v=2,name=c,v=3,|Expected '"' in JSON|Expected ',' or ']' in JSON array at position 3, got '2', near: ...[1 2]...)OUT"), got);
		});

		// stable sorts across the block boundary (255..1500 elements): by-reference, by-value and int comparators make the same calls in the same number; a comparator that writes fields or reassigns its reference parameter
		test("sort_comparators_merge_and_call_alike", [this]() {
			auto e = make_engine();
			e->execution_budget(0);   // a Debug build runs these past the default budget
			jai::stdlib::register_all(e);
			const std::string got = e->execute(R"JAI(
class Item {
	int key = 0;
	int id = 0;
}
string out = "";
array<int> sizes = [0, 1, 2, 3, 255, 256, 257, 511, 512, 513, 700, 1500];
int seed = 11;
for (int n : sizes) {
	array<Item> items = [];
	for (int i = 0; i < n; ++i) {
		seed = (seed * 1103515245 + 12345) % 2147483648;
		Item it;
		it.key = (seed >> 16) % 10;
		it.id = i;
		items.push(it);
	}
	int calls = 0;
	array<Item> v = items;
	v.sort([&calls](Item& a, Item& b) -> bool { calls += 1; return a.key < b.key; });
	bool ok = true;
	for (int i = 1; i < v.size(); ++i) {
		if (v[i - 1].key > v[i].key || (v[i - 1].key == v[i].key && v[i - 1].id > v[i].id)) { ok = false; }
	}
	int vcalls = 0;
	array<Item> w = items;
	w.sort([&vcalls](Item a, Item b) -> bool { vcalls += 1; return a.key < b.key; });
	bool same = true;
	for (int i = 0; i < w.size(); ++i) { if (w[i].id != v[i].id) { same = false; } }
	array<int> ks = [];
	for (auto& it : items) { ks.push(it.key * 10000 + it.id); }
	int icalls = 0;
	ks.sort([&icalls](int a, int b) -> bool { icalls += 1; return a / 10000 < b / 10000; });
	bool isame = true;
	for (int i = 0; i < ks.size(); ++i) { if (ks[i] % 10000 != v[i].id) { isame = false; } }
	out += to_string(n) + ":" + to_string(calls) + ":" + to_string(vcalls) + ":" + to_string(icalls) + ":" + to_string(ok) + to_string(same) + to_string(isame) + " ";
}
array<Item> bumped = [];
for (int i = 0; i < 300; ++i) { Item it; it.key = 299 - i; it.id = i; bumped.push(it); }
bumped.sort([](Item& a, Item& b) -> bool { a.id += 1; b.id += 1; return a.key < b.key; });
int idsum = 0;
for (auto& it : bumped) { idsum += it.id; }
out += to_string(bumped[0].key) + " " + to_string(idsum);
array<Item> swapped = [];
for (int i = 0; i < 40; ++i) { Item it; it.key = i % 7; it.id = i; swapped.push(it); }
swapped.sort([](Item& a, Item& b) -> bool { if (a.key == 3) { Item t; t.key = -1; t.id = 999; a = t; } return a.key < b.key; });
out += " " + to_string(swapped[0].key) + " " + to_string(swapped[0].id) + " " + to_string(swapped.size());
out;
)JAI").to_string();
			check_eq(std::string(R"OUT(0:0:0:0:truetruetrue 1:0:0:0:truetruetrue 2:1:1:1:truetruetrue 3:3:3:3:truetruetrue 255:1652:1652:1652:truetruetrue 256:1710:1710:1710:truetruetrue 257:1787:1787:1787:truetruetrue 511:3860:3860:3860:truetruetrue 512:3859:3859:3859:truetruetrue 513:4022:4022:4022:truetruetrue 700:5746:5746:5746:truetruetrue 1500:13729:13729:13729:truetruetrue 0 47210 -1 999 40)OUT"), got);
		});

		// a host record's one-byte integers are numbers in the text (they were raw characters), and read back
		test("json_archive_one_byte_integers_are_numbers", [this]() {
			auto e = make_engine();
			serialization::json_archive_writer writer(-1, e.get());
			writer.begin_array(4);
			writer.write_int8(-5);
			writer.write_uint8(200);
			writer.write_int8(65);
			writer.write_uint64(18446744073709551615ull);
			writer.end_array();
			const std::string text = writer.str();
			check_eq(std::string("[-5,200,65,18446744073709551615]"), text);
			serialization::json_archive_reader reader(text, e.get());
			check_eq(size_t{4}, reader.begin_array());
			check_eq(int64_t{-5}, int64_t{reader.read_int8()});
			check_eq(int64_t{200}, int64_t{reader.read_uint8()});
			check_eq(int64_t{65}, int64_t{reader.read_int8()});
		});
	}
};

} // namespace jai::foundry::tests

using web_shape_tests = jai::foundry::tests::web_shape_tests;
FOUNDRY_REGISTER(web_shape_tests)
