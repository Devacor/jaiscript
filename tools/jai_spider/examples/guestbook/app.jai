// Guestbook: a page that lists signatures, a form that adds one, a JSON API over the same table, a cookie session
// that remembers the visitor's name. Run it with `jai_spider examples/guestbook/app.jai` and open http://127.0.0.1:8080/;
// edit this file, views.jai or a template under views/ while it runs and reload the page.
//
// Each request runs in a fresh engine, so this file runs again for every one: what must outlast a request (the
// entries, the sessions, the page count) lives in the database.
import "spider.jai";
import "session.jai";
import "views.jai";

database db = open_database("guestbook.db");
db.execute("CREATE TABLE IF NOT EXISTS entries (id INTEGER PRIMARY KEY, name TEXT NOT NULL, message TEXT NOT NULL, created INTEGER NOT NULL)");
db.execute("CREATE TABLE IF NOT EXISTS counters (name TEXT PRIMARY KEY, value INTEGER NOT NULL)");
spider::session_store sessions = spider::session_store(db);

spider::every(3600, () => sessions.purge_expired());   // hourly, in the background

// one row of entries: query<Entry> fills it by column name, insert writes it back
class Entry {
	int id;
	string name;
	string message;
	int created;
}
const string NEWEST = "SELECT * FROM entries ORDER BY id DESC LIMIT 50";
const string ENTRY = "SELECT * FROM entries WHERE id = ?";

spider::router routes = spider::router();

routes.get("/", (spider::request req) => {
	db.execute("INSERT INTO counters (name, value) VALUES ('views', 1) ON CONFLICT(name) DO UPDATE SET value = value + 1");
	int views = db.value("SELECT value FROM counters WHERE name = 'views'");
	spider::session s = sessions.load(req);
	array<Entry> entries = db.query<Entry>(NEWEST);
	spider::response r = spider::html(home_page(entries, s.get("name", ""), s.take("flash", ""), views));
	sessions.save(s, r);
	return r;
});

routes.post("/sign", (spider::request req) => {
	map<string, string> form = req.form();
	string name = string::trim(form.get("name", ""));
	string message = string::trim(form.get("message", ""));
	spider::session s = sessions.load(req);
	if (name == "" || message == "") {
		s.set("flash", "A name and a message, please.");
	} else if (name.size() > 80 || message.size() > 2000) {
		s.set("flash", "That is longer than this guestbook keeps.");
	} else {
		add_entry(name, message);
		s.set("name", name);
		s.set("flash", "Thanks for signing, " + name + ".");
	}
	spider::response r = spider::redirect("/");
	sessions.save(s, r);
	return r;
});

routes.get("/entries/:id", (spider::request req) => {
	shared_ptr<Entry> entry = db.query_one<Entry>(ENTRY, req.param("id"));
	if (entry == null) { return spider::not_found("No entry " + req.param("id")); }
	return spider::html(entry_page(entry));
});

routes.get("/api/entries", (spider::request req) => db.query("SELECT id, name, message, created FROM entries ORDER BY id DESC"));

routes.post("/api/entries", (spider::request req) => {
	var body = null;
	try {
		body = req.json();
	} catch (e) {
		return spider::json({"error": "the body is not JSON"}, 400);
	}
	if (reflect::type_of(body) != "map" || reflect::type_of(body.get("name")) != "string" || reflect::type_of(body.get("message")) != "string") {
		return spider::json({"error": "send {\"name\": string, \"message\": string}"}, 422);
	}
	int id = add_entry(body["name"], body["message"]);
	return spider::json(db.query_one("SELECT id, name, message, created FROM entries WHERE id = ?", id), 201);
});

spider::spin(routes);

int add_entry(string name, string message) {
	Entry entry;
	entry.name = name;
	entry.message = message;
	entry.created = time::unix_ms() / 1000;
	return db.insert("entries", entry);   // id 0 is left out, so SQLite assigns it
}
