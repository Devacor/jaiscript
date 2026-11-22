// The fixture for spider::every: a task that outlasts its period and records each run, so overlapping runs would show,
// one that fails every time, and one that returns false on its second run.
import "spider.jai";

database db = open_database("tasks.db");
db.execute("CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY, started INTEGER NOT NULL, ended INTEGER)");

spider::router routes = spider::router();
routes.get("/runs", (spider::request req) => db.query("SELECT started, ended FROM runs ORDER BY id"));

spider::every(0.2, () => {
	db.execute("INSERT INTO runs (started) VALUES (?)", time::unix_ms());
	int id = db.last_insert_id();
	sleep(0.5);
	db.execute("UPDATE runs SET ended = ? WHERE id = ?", time::unix_ms(), id);
});
spider::every(1, () => { throw "thrown by a task"; });

db.execute("CREATE TABLE IF NOT EXISTS stopper (id INTEGER PRIMARY KEY)");
routes.get("/stopper", (spider::request req) => db.query("SELECT id FROM stopper"));
// returns false on its second run: a stopped task is never run again
spider::every(0.2, () => {
	db.execute("INSERT INTO stopper DEFAULT VALUES");
	return db.query("SELECT id FROM stopper").size() < 2;
});

spider::spin(routes);
