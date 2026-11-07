// Cookie sessions kept in SQLite: the cookie carries only a random id (crypto::random_hex, the system's secure source),
// the data stays on the server as JSON in the spider_sessions table.
//
//   spider::SessionStore sessions = spider::SessionStore(db);
//   spider::Session s = sessions.load(req);      // the visitor's session, or a new empty one
//   s.set("name", "Ada");
//   spider::Response r = spider::redirect("/");
//   sessions.save(s, r);                         // writes the row and the cookie when the session changed
import "spider.jai";

namespace spider {

class Session {
	string id;
	map<string, var> data;
	bool changed;
	bool is_new;

	var get(string key, var fallback = null) { return data.contains(key) ? data[key] : fallback; }
	bool has(string key) { return data.contains(key); }
	void set(string key, var value) {
		data[key] = value;
		changed = true;
	}
	void erase(string key) {
		if (data.contains(key)) {
			data.erase(key);
			changed = true;
		}
	}
	// a value read once: a message for the next page after a redirect
	var take(string key, var fallback = null) {
		var value = get(key, fallback);
		erase(key);
		return value;
	}
}

class SessionStore {
	Database db;
	string cookie_name = "session";
	int max_age = 1209600;            // two weeks, renewed whenever the session is saved
	bool secure_cookie = false;       // true once the site is served over HTTPS

	SessionStore() {}
	SessionStore(Database database) {
		db = database;
		db.execute("CREATE TABLE IF NOT EXISTS spider_sessions (id TEXT PRIMARY KEY, data TEXT NOT NULL, expires INTEGER NOT NULL)");
	}

	spider::Session load(spider::Request req) {
		spider::Session s;
		string id = req.cookie(cookie_name);
		if (id != "") {
			var row = db.query_one("SELECT data FROM spider_sessions WHERE id = ? AND expires > ?", id, now_seconds());
			if (row != null) {
				s.id = id;
				s.data = from_json(row["data"]);
				return s;
			}
		}
		s.id = crypto::random_hex(32);
		s.is_new = true;
		return s;
	}

	void save(spider::Session& s, spider::Response& resp) {
		if (!s.changed || (s.is_new && s.data.size() == 0)) { return; }
		db.execute("INSERT INTO spider_sessions (id, data, expires) VALUES (?, ?, ?) "
			+ "ON CONFLICT(id) DO UPDATE SET data = excluded.data, expires = excluded.expires",
			s.id, to_json(s.data), now_seconds() + max_age);
		resp.set_cookie(cookie_name, s.id, {"max_age": max_age, "secure": secure_cookie});
		s.changed = false;
		s.is_new = false;
	}

	// a new id for the same data: call it when the visitor signs in, so an id seen before the sign-in is worth nothing
	void rotate(spider::Session& s) {
		db.execute("DELETE FROM spider_sessions WHERE id = ?", s.id);
		s.id = crypto::random_hex(32);
		s.changed = true;
	}

	void destroy(spider::Session& s, spider::Response& resp) {
		db.execute("DELETE FROM spider_sessions WHERE id = ?", s.id);
		resp.clear_cookie(cookie_name);
		s.data = {};
		s.changed = false;
	}

	int purge_expired() { return db.execute("DELETE FROM spider_sessions WHERE expires <= ?", now_seconds()); }

	int now_seconds() { return time::unix_ms() / 1000; }
}

}
