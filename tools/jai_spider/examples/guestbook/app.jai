// Guestbook: a page that lists signatures, a form that adds one, a JSON API over the same table, a cookie session
// that remembers the visitor's name. Run it with `jai_spider examples/guestbook/app.jai` and open http://127.0.0.1:8080/;
// edit this file, views.jai or a template under views/ while it runs and reload the page.
import "spider.jai";
import "session.jai";
import "views.jai";

Database db = open_database("guestbook.db");
db.execute("CREATE TABLE IF NOT EXISTS entries (id INTEGER PRIMARY KEY, name TEXT NOT NULL, message TEXT NOT NULL, created INTEGER NOT NULL)");
spider::SessionStore sessions = spider::SessionStore(db);

int page_views;                   // declared without an initializer: the count survives every reload

spider::Router routes = spider::Router();

routes.get("/", (spider::Request req) => {
	page_views += 1;
	spider::Session s = sessions.load(req);
	array<map<string, var>> entries = db.query("SELECT id, name, message, created FROM entries ORDER BY id DESC LIMIT 50");
	spider::Response r = spider::html(home_page(entries, s.get("name", ""), s.take("flash", ""), page_views));
	sessions.save(s, r);
	return r;
});

routes.post("/sign", (spider::Request req) => {
	map<string, string> form = req.form();
	string name = string::trim(form.get("name", ""));
	string message = string::trim(form.get("message", ""));
	spider::Session s = sessions.load(req);
	if (name == "" || message == "") {
		s.set("flash", "A name and a message, please.");
	} else if (name.size() > 80 || message.size() > 2000) {
		s.set("flash", "That is longer than this guestbook keeps.");
	} else {
		add_entry(name, message);
		s.set("name", name);
		s.set("flash", "Thanks for signing, " + name + ".");
	}
	spider::Response r = spider::redirect("/");
	sessions.save(s, r);
	return r;
});

routes.get("/entries/:id", (spider::Request req) => {
	var entry = db.query_one("SELECT id, name, message, created FROM entries WHERE id = ?", req.param("id"));
	if (entry == null) { return spider::not_found("No entry " + req.param("id")); }
	return spider::html(entry_page(entry));
});

routes.get("/api/entries", (spider::Request req) => db.query("SELECT id, name, message, created FROM entries ORDER BY id DESC"));

routes.post("/api/entries", (spider::Request req) => {
	var body = null;
	try {
		body = req.json();
	} catch (e) {
		return spider::json({"error": "the body is not JSON"}, 400);
	}
	if (type_of(body) != "map" || type_of(body.get("name")) != "string" || type_of(body.get("message")) != "string") {
		return spider::json({"error": "send {\"name\": string, \"message\": string}"}, 422);
	}
	int id = add_entry(body["name"], body["message"]);
	return spider::json(db.query_one("SELECT id, name, message, created FROM entries WHERE id = ?", id), 201);
});

spider::spin(routes);

int add_entry(string name, string message) {
	db.execute("INSERT INTO entries (name, message, created) VALUES (?, ?, ?)", name, message, time::unix_ms() / 1000);
	return db.last_insert_id();
}
