// The fixture spider_test.py drives: one route per thing the host and lib/spider.jai promise, and the routes the
// spider:: client checks (tests/client.jai) call.
import "spider.jai";
import "session.jai";
import "greeting.jai";

Database db = open_database("test.db");
db.execute("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, qty INTEGER, price REAL, note TEXT)");
spider::SessionStore sessions = spider::SessionStore(db);

int counter;

spider::Router routes = spider::Router();

routes.get("/", (spider::Request req) => "<h1>home</h1>");
routes.get("/greeting", (spider::Request req) => spider::plain_text(greeting()));
routes.get("/count", (spider::Request req) => {
	counter += 1;
	return spider::plain_text(to_string(counter));
});
routes.get("/posts/:id", (spider::Request req) => spider::json({"id": req.param("id")}));
routes.get("/files/*rest", (spider::Request req) => spider::json({"rest": req.param("rest")}));
routes.post("/only-post", (spider::Request req) => "posted");
routes.get("/echo", (spider::Request req) => spider::json({"method": req.method, "path": req.path, "query": req.query, "agent": req.header("X-TEST-Agent")}));
routes.any("/echo/body", (spider::Request req) => spider::json({"method": req.method, "body": req.body, "length": req.body.size()}));
routes.post("/echo/form", (spider::Request req) => spider::json(req.form()));
routes.post("/echo/json", (spider::Request req) => spider::json({"got": req.json()}));
routes.post("/echo/files", (spider::Request req) => {
	map<string, var> out = {"fields": req.form()};
	map<string, var> files;
	for (auto [name, upload] : req.files()) {
		files[name] = {"filename": upload.filename, "size": upload.size(), "type": upload.content_type, "data": upload.data};
	}
	out["files"] = files;
	return spider::json(out);
});
routes.get("/cookies", (spider::Request req) => {
	spider::Response r = spider::json(req.cookies);
	r.set_cookie("flavor", "oat meal", {"max_age": 60});
	r.set_cookie("plain", "1", {"http_only": false, "same_site": ""});
	return r;
});
routes.get("/session", (spider::Request req) => {
	spider::Session s = sessions.load(req);
	int seen = s.get("seen", 0);
	s.set("seen", seen + 1);
	spider::Response r = spider::json({"seen": seen + 1, "new": s.is_new});
	sessions.save(s, r);
	return r;
});
routes.get("/download", (spider::Request req) => spider::file("public/style.css", "text/x-test"));
routes.get("/forged-sendfile", (spider::Request req) => {
	spider::Response r = spider::plain_text("forged");
	r.header("X-Sendfile", "app.jai");
	return r;
});
routes.get("/bad-header", (spider::Request req) => {
	spider::Response r = spider::plain_text("x");
	r.headers["X-Bad"] = "a\r\nInjected: 1";
	return r;
});
routes.get("/cookie-inject", (spider::Request req) => {
	spider::Response r = spider::plain_text("x");
	r.set_cookie("a", "1", {"path": "/; Domain=evil.example"});
	return r;
});
routes.get("/boom", (spider::Request req) => 1 / 0);
routes.get("/throw", (spider::Request req) => { throw "thrown from a handler"; });
routes.get("/spin", (spider::Request req) => {
	int n = 0;
	while (true) { n += 1; }
	return "never";
});
routes.get("/status/:code", (spider::Request req) => spider::Response(req.param("code").to_int(), "custom status"));
routes.get("/redirect", (spider::Request req) => spider::redirect("/target"));
routes.get("/null", (spider::Request req) => null);

routes.any("/client/echo", (spider::Request req) => spider::json({"method": req.method, "body": req.body, "type": req.header("content-type"),
	"agent": req.header("user-agent"), "x": req.header("x-spider"), "query": req.query}));
routes.get("/client/hop/:n", (spider::Request req) => {
	int n = req.param("n").to_int();
	if (n == 0) { return spider::plain_text("landed"); }
	return spider::redirect("/client/hop/" + to_string(n - 1), 302);
});
routes.post("/client/form", (spider::Request req) => spider::redirect("/client/echo?after=post"));
routes.post("/client/keep", (spider::Request req) => spider::redirect("/client/echo", 307));
routes.get("/client/slow", (spider::Request req) => {
	sleep(0.8);
	return spider::plain_text("slow");
});
routes.get("/client/fetch-self-headers", (spider::Request req) => {
	spider::Response r = spider::plain_text("headers");
	r.header("X-Twice", "a");
	r.headers["X-Map"] = "written through the map";
	return r;
});

routes.post("/items", (spider::Request req) => {
	var body = req.json();
	db.execute("INSERT INTO items (name, qty, price, note) VALUES (?, ?, ?, ?)", body["name"], body["qty"], body["price"], body["note"]);
	return spider::json({"id": db.last_insert_id()}, 201);
});
routes.get("/items", (spider::Request req) => db.query("SELECT id, name, qty, price, note FROM items ORDER BY id"));
routes.get("/items/:id", (spider::Request req) => {
	var row = db.query_one("SELECT id, name, qty, price, note FROM items WHERE id = :id", {"id": req.param("id").to_int()});
	return row == null ? spider::not_found("no item") : spider::json(row);
});
routes.put("/items/:id", (spider::Request req) => {
	var body = req.json();
	int changed = db.execute("UPDATE items SET qty = ? WHERE id = ?", body["qty"], req.param("id").to_int());
	return spider::json({"changed": changed});
});
routes.delete("/items/:id", (spider::Request req) => spider::json({"changed": db.execute("DELETE FROM items WHERE id = ?", req.param("id").to_int())}));
routes.get("/items-search", (spider::Request req) => db.query("SELECT name FROM items WHERE name = ?", req.query.get("name", "")));
routes.post("/items/rollback", (spider::Request req) => {
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
routes.post("/items/nested", (spider::Request req) => {
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
routes.get("/sql-error", (spider::Request req) => db.query("SELECT * FROM missing_table"));
db.execute("CREATE TABLE IF NOT EXISTS kinds (v)");
routes.get("/sql/in", (spider::Request req) => db.query("SELECT name FROM items WHERE qty IN (?) ORDER BY name", [101, 102]));
routes.get("/sql/in-empty", (spider::Request req) => db.query("SELECT name FROM items WHERE qty IN (?)", []));
routes.get("/sql/sorted", (spider::Request req) => db.query("SELECT name FROM items ORDER BY {}, id", sql_choose(req.query.get("by", "name"), ["name", "qty DESC", "price"])));
routes.get("/sql/injected", (spider::Request req) => {
	var text = "SELECT name FROM items WHERE name = '" + req.query.get("n", "") + "'";
	[[warn_mode]] return spider::json(db.query(text));
});
routes.get("/sql/reflected", (spider::Request req) => {
	var text = "DELETE FROM items";
	return spider::json(reflect::invoke(db, "execute", text));
});
routes.get("/sql/fragment-misuse", (spider::Request req) => db.query("SELECT name FROM items WHERE name = ?", sql_choose("name", ["name"])));
routes.post("/sql/types", (spider::Request req) => {
	db.execute("DELETE FROM kinds");
	for (var v : [1, 2.5, true, null, "it's \"q\" \xC3\xA9"]) { db.execute("INSERT INTO kinds (v) VALUES (?)", v); }
	return db.query("SELECT typeof(v) AS t, v FROM kinds ORDER BY rowid");
});

spider::spin(routes, {"max_body": 65536});
