// The fixture spider_test.py drives: one route per thing the host and lib/spider.jai promise, and the routes the
// spider:: client checks (tests/client.jai) call.
import "spider.jai";
import "session.jai";
import "greeting.jai";

database db = open_database("test.db");
db.execute("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, qty INTEGER, price REAL, note TEXT)");
spider::session_store sessions = spider::session_store(db);

int counter;

spider::router routes = spider::router();

routes.get("/", (spider::request req) => "<h1>home</h1>");
routes.get("/greeting", (spider::request req) => spider::plain_text(greeting()));
routes.get("/count", (spider::request req) => {
	counter += 1;
	return spider::plain_text(to_string(counter));
});
routes.get("/posts/:id", (spider::request req) => spider::json({"id": req.param("id")}));
routes.get("/files/*rest", (spider::request req) => spider::json({"rest": req.param("rest")}));
routes.post("/only-post", (spider::request req) => "posted");
routes.get("/echo", (spider::request req) => spider::json({"method": req.method, "path": req.path, "query": req.query, "agent": req.header("X-TEST-Agent")}));
routes.any("/echo/body", (spider::request req) => spider::json({"method": req.method, "body": req.body, "length": req.body.size()}));
routes.post("/echo/form", (spider::request req) => spider::json(req.form()));
routes.post("/echo/json", (spider::request req) => spider::json({"got": req.json()}));
routes.post("/echo/files", (spider::request req) => {
	map<string, var> out = {"fields": req.form()};
	map<string, var> files;
	for (auto [name, upload] : req.files()) {
		files[name] = {"filename": upload.filename, "size": upload.size(), "type": upload.content_type, "data": upload.data};
	}
	out["files"] = files;
	return spider::json(out);
});
// an avatar upload: the file's bytes scaled and saved as a PNG
routes.post("/image/avatar", (spider::request req) => {
	try {
		image picture = image::load(req.files()["picture"].data);
		picture.crop_fill(16, 16);
		string saved = picture.encode("png");
		map<string, var> made = image::data(saved);
		return spider::json({"source": picture.format, "format": made["format"], "width": made["width"], "height": made["height"]});
	} catch (e) {
		return spider::response(422, e.message);
	}
});
routes.get("/cookies", (spider::request req) => {
	spider::response r = spider::json(req.cookies);
	r.set_cookie("flavor", "oat meal", {"max_age": 60});
	r.set_cookie("plain", "1", {"http_only": false, "same_site": ""});
	return r;
});
routes.get("/session", (spider::request req) => {
	spider::session s = sessions.load(req);
	int seen = s.get("seen", 0);
	s.set("seen", seen + 1);
	spider::response r = spider::json({"seen": seen + 1, "new": s.is_new});
	sessions.save(s, r);
	return r;
});
routes.get("/download", (spider::request req) => spider::file("public/style.css", "text/x-test"));
routes.get("/forged-sendfile", (spider::request req) => {
	spider::response r = spider::plain_text("forged");
	r.header("X-Sendfile", "app.jai");
	return r;
});
routes.get("/bad-header", (spider::request req) => {
	spider::response r = spider::plain_text("x");
	r.headers["X-Bad"] = "a\r\nInjected: 1";
	return r;
});
routes.get("/cookie-inject", (spider::request req) => {
	spider::response r = spider::plain_text("x");
	r.set_cookie("a", "1", {"path": "/; Domain=evil.example"});
	return r;
});
routes.get("/boom", (spider::request req) => 1 / 0);
routes.get("/throw", (spider::request req) => { throw "thrown from a handler"; });
routes.get("/spin", (spider::request req) => {
	int n = 0;
	while (true) { n += 1; }
	return "never";
});
routes.get("/status/:code", (spider::request req) => spider::response(req.param("code").to_int(), "custom status"));
routes.get("/redirect", (spider::request req) => spider::redirect("/target"));
routes.get("/null", (spider::request req) => null);

routes.any("/client/echo", (spider::request req) => spider::json({"method": req.method, "body": req.body, "type": req.header("content-type"),
	"agent": req.header("user-agent"), "x": req.header("x-spider"), "query": req.query}));
routes.get("/client/hop/:n", (spider::request req) => {
	int n = req.param("n").to_int();
	if (n == 0) { return spider::plain_text("landed"); }
	return spider::redirect("/client/hop/" + to_string(n - 1), 302);
});
routes.post("/client/form", (spider::request req) => spider::redirect("/client/echo?after=post"));
routes.post("/client/keep", (spider::request req) => spider::redirect("/client/echo", 307));
routes.get("/client/slow", (spider::request req) => {
	sleep(0.8);
	return spider::plain_text("slow");
});
routes.get("/client/fetch-self-headers", (spider::request req) => {
	spider::response r = spider::plain_text("headers");
	r.header("X-Twice", "a");
	r.headers["X-Map"] = "written through the map";
	return r;
});

routes.post("/items", (spider::request req) => {
	var body = req.json();
	db.execute("INSERT INTO items (name, qty, price, note) VALUES (?, ?, ?, ?)", body["name"], body["qty"], body["price"], body["note"]);
	return spider::json({"id": db.last_insert_id()}, 201);
});
routes.get("/items", (spider::request req) => db.query("SELECT id, name, qty, price, note FROM items ORDER BY id"));
routes.get("/items/:id", (spider::request req) => {
	var row = db.query_one("SELECT id, name, qty, price, note FROM items WHERE id = :id", {"id": req.param("id").to_int()});
	return row == null ? spider::not_found("no item") : spider::json(row);
});
routes.put("/items/:id", (spider::request req) => {
	var body = req.json();
	int changed = db.execute("UPDATE items SET qty = ? WHERE id = ?", body["qty"], req.param("id").to_int());
	return spider::json({"changed": changed});
});
routes.delete("/items/:id", (spider::request req) => spider::json({"changed": db.execute("DELETE FROM items WHERE id = ?", req.param("id").to_int())}));
routes.get("/items-search", (spider::request req) => db.query("SELECT name FROM items WHERE name = ?", req.query.get("name", "")));
routes.post("/items/rollback", (spider::request req) => {
	try {
		db.transaction(() => {
			db.execute("INSERT INTO items (name, qty) VALUES (?, ?)", "doomed", 1);
			db.execute("INSERT INTO nowhere (x) VALUES (1)");
		});
	} catch (e) {
		return spider::json({"error": e, "count": db.value("SELECT COUNT(*) FROM items WHERE name = ?", "doomed")});
	}
	return spider::json({"error": null});
});
routes.post("/items/nested", (spider::request req) => {
	string thrown = "";
	db.transaction(() => {
		db.execute("INSERT INTO items (name, qty) VALUES (?, ?)", "outer", 1);
		try {
			db.transaction(() => {
				db.execute("INSERT INTO items (name, qty) VALUES (?, ?)", "inner", 1);
				throw "inner fails";
			});
		} catch (e) {
			thrown = e;
		}
	});
	return spider::json({"thrown": thrown, "outer": db.value("SELECT COUNT(*) FROM items WHERE name = 'outer'"),
		"inner": db.value("SELECT COUNT(*) FROM items WHERE name = 'inner'"), "open": db.in_transaction()});
});
routes.get("/sql-error", (spider::request req) => db.query("SELECT * FROM missing_table"));
db.execute("CREATE TABLE IF NOT EXISTS kinds (v)");
routes.get("/sql/in", (spider::request req) => db.query("SELECT name FROM items WHERE qty IN (?) ORDER BY name", [101, 102]));
routes.get("/sql/in-empty", (spider::request req) => db.query("SELECT name FROM items WHERE qty IN (?)", []));
routes.get("/sql/sorted", (spider::request req) => db.query("SELECT name FROM items ORDER BY {}, id", sql_choose(req.query.get("by", "name"), ["name", "qty DESC", "price"])));
routes.get("/sql/injected", (spider::request req) => {
	var text = "SELECT name FROM items WHERE name = '" + req.query.get("n", "") + "'";
	[[warn_mode]] return spider::json(db.query(text));
});
routes.get("/sql/reflected", (spider::request req) => {
	var text = "DELETE FROM items";
	return spider::json(reflect::invoke(db, "execute", text));
});
routes.get("/sql/fragment-misuse", (spider::request req) => db.query("SELECT name FROM items WHERE name = ?", sql_choose("name", ["name"])));
routes.post("/sql/types", (spider::request req) => {
	db.execute("DELETE FROM kinds");
	for (var v : [1, 2.5, true, null, "it's \"q\" \xC3\xA9"]) { db.execute("INSERT INTO kinds (v) VALUES (?)", v); }
	return db.query("SELECT typeof(v) AS t, v FROM kinds ORDER BY rowid");
});

// typed rows: a class's fields are the columns it reads and writes
db.execute("CREATE TABLE IF NOT EXISTS members (id INTEGER PRIMARY KEY, handle TEXT NOT NULL, admin INTEGER NOT NULL DEFAULT 0, score REAL NOT NULL DEFAULT 0, tags TEXT NOT NULL DEFAULT '[]')");
db.execute("CREATE TABLE IF NOT EXISTS keyless (v TEXT)");
class Member {
	int id;
	string handle;
	bool admin;
	float score;
	array<string> tags;
	string shown() { return handle + (admin ? " (admin)" : "") + " " + to_string(score); }
}
const string ALL_MEMBERS = "SELECT * FROM members ORDER BY id";
const string FIND_MEMBER = "SELECT * FROM members WHERE id = ?";
routes.post("/members", (spider::request req) => {
	Member m = from_json<Member>(req.body);
	return spider::json({"id": db.insert("members", m)}, 201);
});
routes.get("/members", (spider::request req) => {
	array<string> shown;
	auto all = db.query<Member>(ALL_MEMBERS);
	for (auto& m : all) { shown.push(m.shown()); }
	return {"shown": shown, "tags": all.size() > 0 ? all[0].tags : array<string>()};
});
// a generic's instance is a row type like any class, and a generic function wraps the rows a typed query answers
class Labeled<T> { int id; T handle; }
class Page<T> { array<T> rows; int total = 0; }
Page<T> page_of<T>(array<T> rows) { Page<T> page; page.rows = rows; page.total = rows.size(); return page; }
routes.get("/members/generic", (spider::request req) => {
	array<string> labels;
	for (auto& row : db.query<Labeled<string>>(ALL_MEMBERS)) { labels.push(row.handle); }
	array<Member> all = db.query<Member>(ALL_MEMBERS);
	auto page = page_of(all);
	shared_ptr<Labeled<string>> one = db.query_one<Labeled<string>>(FIND_MEMBER, 1);
	return {"labels": labels, "total": page.total, "first": page.rows[0].handle, "type": reflect::type_of(page), "one": one->handle + " " + reflect::type_of(one)};
});
routes.get("/members/:id", (spider::request req) => {
	shared_ptr<Member> m = db.query_one<Member>(FIND_MEMBER, req.param("id").to_int());
	return m == null ? spider::not_found("no member") : spider::json({"handle": m->handle, "admin": m->admin, "tags": m->tags});
});
routes.put("/members/:id", (spider::request req) => {
	shared_ptr<Member> m = db.query_one<Member>(FIND_MEMBER, req.param("id").to_int());
	m->score = 9.5;
	m->tags.push("edited");
	return spider::json({"changed": db.update("members", m)});
});
routes.post("/members/from-map", (spider::request req) => spider::json({"id": db.insert("members", {"handle": "mapped", "admin": true, "unknown": 1})}));
class WrongMember { int id; int handle; }
routes.get("/member-errors/wrong-type", (spider::request req) => db.query<WrongMember>(ALL_MEMBERS));
routes.get("/member-errors/run-time-table", (spider::request req) => {
	var table = req.query.get("t", "members");
	[[warn_mode]] return spider::json({"id": db.insert(table, {"handle": "x"})});
});
routes.get("/member-errors/no-table", (spider::request req) => spider::json({"id": db.insert("nowhere_at_all", {"v": 1})}));
routes.get("/member-errors/no-key", (spider::request req) => spider::json({"changed": db.update("keyless", {"v": "x"})}));
routes.get("/member-errors/missing-key", (spider::request req) => spider::json({"changed": db.update("members", {"handle": "x"})}));

spider::spin(routes, {"max_body": 65536});
