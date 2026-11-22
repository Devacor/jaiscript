// The fixture spider_test.py's worker checks drive: requests that take their time, a global a request changes, a
// marker file a request writes when it starts, so a check can wait for it rather than for a clock, and a transaction
// that reads and then writes, which only holds when the transactions of several workers take turns.
import "spider.jai";

database db = open_database("workers.db");
db.execute("CREATE TABLE IF NOT EXISTS hits (id INTEGER PRIMARY KEY)");

int touched;

spider::router routes = spider::router();

routes.get("/touch", (spider::request req) => {
	touched += 1;
	return spider::plain_text(to_string(touched));
});
routes.get("/sleep/:ms", (spider::request req) => {
	int started = time::unix_ms();
	file::write("started-" + req.param("ms") + ".txt", to_string(started));
	sleep(req.param("ms").to_int() / 1000.0);
	return spider::json({"ms": req.param("ms"), "started": started, "ended": time::unix_ms()});
});
// the wait between the read and the write lets every request at once read the same count
routes.get("/hit", (spider::request req) => {
	int hit = db.transaction(() => {
		int seen = db.value("SELECT COUNT(*) FROM hits");
		sleep(0.02);
		db.execute("INSERT INTO hits (id) VALUES (?)", seen + 1);
		return seen + 1;
	});
	return spider::plain_text(to_string(hit));
});
routes.get("/hits", (spider::request req) => spider::plain_text(to_string(db.value("SELECT COUNT(*) FROM hits"))));

spider::spin(routes);
