# jai_spider: a web application host for JaiScript

`jai_spider app.jai` spins a JaiScript app over HTTP/1.1. One network thread moves the bytes; a pool of workers runs
the app, each request in a fresh engine built ahead of it, so requests never wait on one another's handlers and none
sees what another left behind. Each engine is built from the app's files as they are, so an edit applies to the next
request with no restart, and an edit that does not check is reported in the console while the app keeps serving the
code it had. SQLite is built in (compiled into jai_spider), `spider::every` runs background tasks, files under the
app's `public/` folder are served as they are, and the `spider::` client reaches other servers from the same program.

Built by the `jai_spider` CMake target (on by default, `JAISCRIPT_BUILD_SPIDER`); the binary lands in the build
directory's `bin/` next to `jai_spider_lib/`, the script library apps import.

SQLite is part of the repository: `third_party/sqlite` holds the official amalgamation (3.54.1), and the build compiles
it with `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_DQS=0` and `SQLITE_THREADSAFE=1`. Nothing needs installing or downloading.
`third_party/sqlite/README.md` says how to update it.

```
jai_spider examples/guestbook/app.jai
/\(o.o)/\  jai_spider 0.6.0-beta
jai_spider: spinning examples/guestbook/app.jai on http://127.0.0.1:8080/ (files from .../guestbook/public)
jai_spider: 8 workers, a fresh engine for each request (the first build took 23.5 ms); edits apply on the next request; Ctrl+C stops
GET / 200 1.21 ms
GET /style.css 200 0.31 ms (file)
POST /sign 303 2.40 ms
jai_spider: reloaded views.jai
GET / 200 1.87 ms
```

## An app

```cpp
import "spider.jai";

database db = open_database("notes.db");       // relative to the app's folder
db.execute("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL)");

spider::router routes = spider::router();
routes.get("/", (spider::request req) => {
	int count = db.value("SELECT COUNT(*) FROM notes");   // what outlasts a request lives in the database
	return spider::html(`<h1>${count} notes</h1>`);
});
routes.get("/notes", (spider::request req) => db.query("SELECT id, text FROM notes"));   // a value answers JSON
routes.post("/notes", (spider::request req) => {
	db.execute("INSERT INTO notes (text) VALUES (?)", req.form()["text"]);
	return spider::redirect("/notes");
});
routes.get("/notes/:id", (spider::request req) => {
	var note = db.query_one("SELECT id, text FROM notes WHERE id = ?", req.param("id"));
	return note == null ? spider::not_found() : spider::json(note);
});

spider::spin(routes);
```

`spider::spin(handler[, options])` (also spelled `spider::serve`) hands the server a `spider::router` or any function
of a `spider::request`; it answers every request that is not a file under the static root. A handler may answer a
`spider::response`, a string or a `web::html` (an HTML page), `null` (204) or any other value (JSON). Options: `budget` (seconds a
request may run, over `--budget`) and `max_body` (bytes, over `--max-body`).

## Pages from templates

A page is a template file, HTML with `${...}` splices (docs/site/guide/19-templates.html). `template(path, a, b, ...)`
renders it, seeing `a`, `b`, ... by name (the caller's variables, by reference) and the program's functions and globals;
every splice is HTML-escaped, and what it returns is a `web::html`, which splices into another template as it is. The
guestbook's views are `examples/guestbook/views/*.spider`:

```
<article>
<p class="message">${entry.message}</p>
<p class="byline"><a href="/entries/${entry.id}">${entry.name}, ${when(entry.created)}</a></p>
</article>
```

```cpp
web::html home_page(array<Entry> entries, string name, string flash, int views) {
	string title = "Guestbook";
	web::html body = template("views/home.spider", entries, name, flash, views);
	return template("views/layout.spider", title, body);
}
```

A template with a literal path is checked when the module that calls it loads, at the template's own line, and it is part
of that module: an edit to it applies on the next request, and an edit that does not check is reported in the console
while the app keeps serving the version that did. A list is `${array::map(entries, entry => template("card.spider", entry))}`.
`untrusted_template(path, map)` renders a template a visitor or a customer wrote: it sees only a copy of the map and calls only
built-in methods, so it cannot reach the app.

## Options

```
jai_spider app.jai [--port N] [--host ADDR] [--root DIR] [--workers N] [--queue N] [--max-body N] [--timeout S]
                   [--budget S] [--backend vm|interp] [--dev] [--allow-shutdown TOKEN] [--quiet]
```

| option | meaning |
|---|---|
| `--port N` | port to listen on (default 8080; 0 picks a free one, printed in the spinning line) |
| `--host ADDR` | IPv4 address to listen on (default 127.0.0.1; 0.0.0.0 for every interface) |
| `--root DIR` | serve the files under DIR for GET and HEAD before the app sees the request (default: the app's `public/` folder when it exists) |
| `--workers N` | worker threads running the app, each with its next engine built ahead (default: one per hardware thread; 1 to 1024) |
| `--queue N` | requests that may wait for a free worker (default 1024); past it 503 with `Retry-After: 1` |
| `--max-body N` | largest request body, `K`/`M`/`G` suffixes allowed (default 2M); a larger one is answered 413 |
| `--timeout S` | seconds a request may take to arrive and a response to leave (default 30) |
| `--budget S` | execution budget per request (default 30; 0 = unlimited); a runaway handler is a 500 |
| `--backend` | the engine's backend (vm), as for `jaiscript` |
| `--dev` | development: error pages show the error and its trace, and `lib/` is imported from the source checkout (an edit to `spider.jai` applies on the next request) |
| `--allow-shutdown TOKEN` | `POST /__spider/shutdown` from this machine with the header `X-Spider-Shutdown: TOKEN` stops the server (for scripts and agents driving it headless); without the token it is 403 |
| `--quiet` | no banner and no request log |

Ctrl+C stops the server; open connections are closed and the app's databases are closed with the engine.

A server listening on a loopback address answers 421 to a Host header that is not `localhost`, a `127.x` address or
`[::1]`, so a page whose own name resolves to 127.0.0.1 (DNS rebinding) cannot reach it. One address holds at most 128
of the 512 connections.

## The script surface (`lib/spider.jai`, all under `spider::`)

**`spider::request`**: `method`, `path` (decoded), `target` (as sent), `query_string`, `query` (`map<string, string>`),
`headers` (names lowercased), `cookies`, `params` (route captures), `body`, `remote_addr`; `header(name)` in any
spelling, `cookie(name)`, `param(name)`, `form()` (urlencoded or multipart fields), `files()`
(`map<string, spider::upload>`: `filename`, `content_type`, `data`, `size()`, `save(path)`), `json()`.

**`spider::response`** (the library's, the same class the client returns): `status`, `headers`, `cookies`,
`body`; `spider::response(status, body)`, `header(name, value)`, `set_cookie(name, value, options)` (`max_age`,
`expires`, `path`, `domain`, `secure`, `http_only` (true), `same_site` ("Lax")), `clear_cookie(name)`. Helpers:
`spider::html(text, status)`, `spider::plain_text(text, status)`, `spider::json(value, status)`,
`spider::redirect(url, status = 303)`, `spider::file(path, content_type)`, `spider::not_found(text)`.

**`spider::router`**: `get`, `post`, `put`, `patch`, `delete`, `any`, `add(method, ...)`; patterns `/posts/:id` and
`/files/*rest`; `before(filter)` runs ahead of every route (null continues, anything else answers); a HEAD request
takes the GET route; a path that only other methods match answers 405 with `Allow`.

**Client**: `spider::catch(url[, options])` fetches (also spelled `spider::get`), `spider::sling(content, url[,
options])` sends (also spelled `spider::post(url, content[, options])`); options `method`, `headers`, `timeout`,
`type`, `redirects` (5 followed by default). WinHTTP on Windows, sockets elsewhere (HTTPS when the build found
OpenSSL); `JAISCRIPT_HTTP_TRANSPORT=socket` picks the socket transport on Windows. A call blocks its own worker until
its answer arrives; the other workers serve on. One transport serves the whole process.

**Text**: the opt-in `web::` library (`jai::stdlib::register_web`, registered here): `web::html_escape`,
`web::html_unescape`, `web::url_encode` / `url_decode` (RFC 3986), `web::form_encode` / `form_decode` (space as
`+`), `web::parse_query`, `web::parse_query_all`, `web::build_query`, `web::parse_url` (a `web::url` with
`to_string()`). The host adds `parse_cookies`, `parse_multipart`, `http_date(unix_seconds)` and
`content_type_for(path)`; session ids are `crypto::random_hex(32)`, the system's secure random source.

**`database`** (SQLite 3.37 or newer: the system's, else the pinned 3.54.1 amalgamation fetched at configure time), injection-proof by construction:

```cpp
database db = open_database("app.db");          // ":memory:" for a scratch database
const string FIND = "SELECT * FROM posts WHERE id = ?";
db.query_one(FIND, id)                          // one row or null
db.query(sql, a, b, ...)                        // array<map<string, var>>: int, float, string, null
db.execute(sql, ...)                            // rows changed; a constant text of several statements runs them all
db.value(sql, ...)                              // the first column of the first row, or null
db.query("SELECT * FROM t WHERE id IN (?)", [1, 2, 3]);                 // an array fills IN (?)
db.query("SELECT * FROM t ORDER BY {}", sql_choose(sort, ["created", "title"]));   // {} takes a constant choice
db.query("SELECT * FROM t WHERE id = :id", {"id": 3});                  // one map binds by name
db.transaction(() => { ... });                  // commits, or rolls back and raises again
db.begin(); db.commit(); db.rollback(); db.last_insert_id(); db.close();

class User { int id; string handle; bool admin; array<string> tags; }
array<User> fans = db.query<User>(FANS, artist.id);    // each row a User, by column name
shared_ptr<User> one = db.query_one<User>(FIND, id);    // a handle, or null when no row
int id = db.insert("users", user);                      // the fields that are columns; returns the new rowid
db.update("users", user);                               // the other columns, WHERE the primary key matches; rows changed
```

The SQL text must be one of the program's own constants: a string literal, a `const` holding one, a ternary or
`+` of them. Anything else fails the load (`argument 1 of 'query' is not a constant: SQL text must be a constant;
pass values as parameters (?)`), and at run time a text the program does not spell is refused before SQLite sees it,
whatever path it took (`engine::constant_argument`, gathered from every unit as it loads). Values only bind as
parameters; there is no escape or quote function. `sql_choose(value, choices)` answers the choice equal to `value`
for a `{}` slot (a sort column, a direction) and raises for anything else; its choices are constants too. Statements
are prepared once per text and kept until the database closes. An SQLite failure is a catchable error carrying
SQLite's message (`SQLite error: no such table: posts`). Files open in WAL mode. A transaction (`transaction(fn)`,
`begin()`) takes the database's write lock when it begins (`BEGIN IMMEDIATE`), so transactions on several connections
queue, each waiting up to the busy timeout for its turn, instead of failing; inside one, `transaction(fn)` is a
savepoint.

With a class as a type argument, `query<T>` and `query_one<T>` answer records (`jai::make_record`): each column fills
the field of its name, a column the class lacks is skipped (a `SELECT *` survives a migration), an INTEGER 0/1 fills
a `bool`, an INTEGER a `float` and a TEXT of JSON an array, map or class field. `insert(table, record)` and
`update(table, record)` take the table's name as a constant too and the columns from its schema (`PRAGMA
table_info`, read again after a schema change), so every identifier comes from the schema, quoted, and every value
binds. An `INTEGER PRIMARY KEY` holding 0 or null is left for SQLite to assign; an array, map or class field is
stored as JSON; a record may also be a `map<string, var>`.

**Sessions** (`lib/session.jai`): `spider::session_store sessions = spider::session_store(db);`, then
`spider::session s = sessions.load(req)`, `s.get/set/erase/take`, `sessions.save(s, r)` with the `spider::response r` being sent,
`sessions.rotate(s)` after a sign-in, `sessions.destroy(s, r)`. The cookie carries a 256-bit random id; the
data stays in the `spider_sessions` table.

Every file function of the runner (`file::read`, `file::write`, `file::list_directory`, ...), the full standard library and the
`web::` library are there too.

## Workers and request isolation

One network thread accepts, reads, serves static files and writes. Each complete request goes to a pool of workers
(`--workers N`, one per hardware thread by default), and each request runs in an engine of its own, like a PHP
request in a fresh process:

- **Requests never wait on one another's handlers.** A handler that sleeps, waits on `spider::catch` or runs a long
  query holds its own worker only.
- **Every request starts fresh.** The engine that answers ran the app's top level for that request alone (imports,
  `open_database`, the router, `spider::spin`) and is destroyed once the answer has gone, its databases with it. A
  global a handler changes is gone by the next request: a count, a cache or a signed-in user lives in the database or
  the session, never in a script global.
- **No request waits for a build while a worker has one ready.** Each worker keeps its next engine built ahead and
  builds the following one right after it answers. Every request still costs a build's work: the server keeps up while
  requests arrive slower than the workers can build, and past that rate they queue. The startup line names how long
  the first (cold) build took; a small app builds in a few milliseconds, one of thousands of lines and dozens of
  templates in tens.
- **Each engine opens its own SQLite connection** (WAL, a 5 s busy timeout), so engines share no connection. A
  transaction takes the write lock when it begins, so the transactions of several workers take turns: what one reads
  is still so when it writes.
- **Answers on one connection keep their order**: a connection holds one request at a time, so pipelined requests are
  answered in the order they came, while requests on different connections run at once.
- **A full house is 503**: past `--queue N` waiting requests (1024 by default) the answer is 503 with `Retry-After: 1`.

What the host shares between threads it serializes itself: the console writes each line whole (an engine's `print`
included), the HTTP client's transport is one per process and takes concurrent calls, and the app's source stamps
and last good texts sit behind a lock. An engine is built, used and destroyed on one worker's thread.

A websocket connection (not served yet) would keep one engine for its life; nothing here rules that out.

## Background tasks

`spider::every(seconds, fn)` at the app's top level declares a background task: purge expired sessions, expire
unpaid quotes, retry webhooks.

```cpp
spider::every(3600, () => sessions.purge_expired());
spider::every(60, () => db.execute("UPDATE quotes SET state = 'expired' WHERE state = 'sent' AND expires < ?", time::unix_ms() / 1000));
```

A run goes to a worker like a request and runs in a fresh engine under the same budget. The first run comes a period
after the server starts, then a period after each run starts; a task never overlaps itself, so a run that outlasts its
period delays the next until it ends. A failure is logged (`jai_spider: spider::every task 2 (every 60 s) failed:`, the
error, its trace) and the schedule goes on. Each build reads the declarations again, so an edited task runs its new
code on its next turn, and a task keeps its schedule while its place and period stay. A period not above 0 raises
`spider::every: the period must be above 0 seconds, got 0.0`.

## Hot reload

Every engine is built from the app's files, so an edit applies to the next request with nothing reloaded in place. The
host records the files each build read (modules, includes, templates): the engine hands it every file it loads with
the bytes it parsed and the (mtime, size) stamp of exactly those bytes (`source_file_observer`). Before a worker
hands a request to the engine it built ahead, it checks those stamps, one per file; a moved one discards the engine
and a new one is built from the files as they are. So a save that lands while an engine is being built is either part
of that build or makes it stale. Only a build that is not stale when it ends is judged, as the last good build or as a
failure: one that read an older version and finished late, or caught a file between two writes of a save, judges
nothing, and the next build does. Editing `lib/spider.jai` under `--dev` applies the same way.

A rejected edit prints its listing once, and the app answers with the code it had until the file is saved again: the
host keeps the texts of the last build that checked, and while the edit stays broken it builds each engine from those
texts, written once into a folder under the app's `.cache` folder and laid out there as they were around the app, so
imports and templates resolve there as they did beside the app (a trace from such an engine names that copy). The copy goes when the server stops; a killed server leaves it there.

## Model and limits

The network loop keeps every connection moving (reads, writes, files streamed in 256 KiB slices) and hands complete
requests to the workers. Requests are HTTP/1.1 or 1.0 with keep-alive and pipelining, bodies by Content-Length or
chunked, `Expect: 100-continue`. The request head is capped at 64 KiB (431), the body at `--max-body` (413), each
connection at `--timeout`, 512 connections are open at once (503 beyond), and `--queue` requests wait for a worker
(503 beyond). A script error is a 500 whose page shows the error and trace under `--dev` and nothing otherwise; the
error is logged either way, the engine is discarded like any other and the server goes on.

Static files get their Content-Type from the extension, a weak ETag and Last-Modified, and `Cache-Control: no-cache`,
so a browser revalidates and an edited file shows on reload (304 otherwise). A file or folder whose real name starts
with a dot (an 8.3 short name such as `ENV~1` included) and paths that leave the root are never served; a request path
with a `.` or `..` segment, or one decoding to a `/`, `\` or NUL inside a segment, is 400.

The server speaks no TLS: serve behind a reverse proxy (Caddy, nginx) for HTTPS.

## Files

```
main.cpp            options, the workers and their engines, the task schedule, static files, the 500 page
http_server.hpp     HTTP/1.1 over the runner's sockets (../jaiscript/net_socket.hpp), answers made on other threads
http_text.hpp       percent-decoding, cookies, multipart, dates, content types
console.hpp         the console every thread and every engine's print writes through, a line at a time
sources.hpp         the files each build read, by stamp; the last good build's texts
database.hpp        the database and sql_fragment bindings
lib/spider.jai      request, upload, the response helpers, router, spin, every
lib/session.jai     session_store and session
examples/guestbook  a guestbook: .spider templates, a form, a session, SQLite, a task, a JSON API, a stylesheet
tests/              spider_test.py, the apps it drives (app/, workers/) and the client checks (tools/ci/gates.sh spider)
```
The client's transport is `../jaiscript/http_transport.hpp`, shared with the runner.
