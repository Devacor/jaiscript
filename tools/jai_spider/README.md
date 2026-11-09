# jai_spider: a web application host for JaiScript

`jai_spider app.jai` spins a JaiScript app over HTTP/1.1. The app is a set of modules; every request reaches their
imports first, so an edit applies to the next request with no restart, and an edit that does not check is reported
in the console while the app keeps serving the code it had. SQLite is built in (compiled into jai_spider), files under the app's `public/`
folder are served as they are, and the `spider::` client reaches other servers from the same program.

Built by the `jai_spider` CMake target (on by default, `JAISCRIPT_BUILD_SPIDER`); the binary lands in the build
directory's `bin/` next to `jai_spider_lib/`, the script library apps import.

SQLite is not in the repository. The build uses a system SQLite 3.37 or newer when CMake finds one (vcpkg, apt
`libsqlite3-dev`, brew); otherwise it downloads the official amalgamation zip (3.54.1, `sqlite-amalgamation-3540100.zip`
from sqlite.org) at configure time into the build tree, checks it against the pinned SHA3-256
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` (the hash sqlite.org publishes) and compiles it with
`SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_DQS=0` and `SQLITE_THREADSAFE=1`. The configure output says which source was used.
Offline, or to pick the source yourself:

- `-DJAI_SPIDER_SQLITE_ZIP=<path>` uses a local copy of that zip (hash-checked);
- `-DJAI_SPIDER_SQLITE_DIR=<dir>` compiles an unpacked amalgamation (`sqlite3.c`, `sqlite3.h`) as it is;
- `-DJAI_SPIDER_SQLITE_FETCH=OFF` turns the download into a configure error.

A configure with no system SQLite, no network and neither path set fails with a message; it never builds a
`jai_spider` without a database. To update the pin, change the version, zip name and hash at the top of
`CMakeLists.txt`.

```
jai_spider examples/guestbook/app.jai
/\(o.o)/\  jai_spider 0.5.2-beta
jai_spider: spinning examples/guestbook/app.jai on http://127.0.0.1:8080/ (files from .../guestbook/public)
GET / 200 1.21 ms
GET /style.css 200 0.31 ms (file)
POST /sign 303 2.40 ms
jai_spider: reloaded views.jai
GET / 200 1.87 ms
```

## An app

```cpp
import "spider.jai";

Database db = open_database("notes.db");       // relative to the app's folder
db.execute("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL)");

int requests;                                  // no initializer: the value survives every reload

spider::Router routes = spider::Router();
routes.get("/", (spider::Request req) => {
	requests += 1;
	return spider::html(`<h1>${requests} requests</h1>`);
});
routes.get("/notes", (spider::Request req) => db.query("SELECT id, text FROM notes"));   // a value answers JSON
routes.post("/notes", (spider::Request req) => {
	db.execute("INSERT INTO notes (text) VALUES (?)", req.form()["text"]);
	return spider::redirect("/notes");
});
routes.get("/notes/:id", (spider::Request req) => {
	var note = db.query_one("SELECT id, text FROM notes WHERE id = ?", req.param("id"));
	return note == null ? spider::not_found() : spider::json(note);
});

spider::spin(routes);
```

`spider::spin(handler[, options])` (also spelled `spider::serve`) hands the server a `spider::Router` or any function
of a `spider::Request`; it answers every request that is not a file under the static root. A handler may answer a
`spider::Response`, a string or a `web::html` (an HTML page), `null` (204) or any other value (JSON). Options: `budget` (seconds a
request may run, over `--budget`) and `max_body` (bytes, over `--max-body`).

## Pages from templates

A page is a template file, HTML with `${...}` splices (docs/site/guide/19-templates.html). `template(path, a, b, ...)`
renders it, seeing `a`, `b`, ... by name (the caller's variables, by reference) and the program's functions and globals;
every splice is HTML-escaped, and what it returns is a `web::html`, which splices into another template as it is. The
guestbook's views are `examples/guestbook/views/*.spider`:

```
<article>
<p class="message">${entry["message"]}</p>
<p class="byline"><a href="/entries/${entry["id"]}">${entry["name"]}, ${when(entry["created"])}</a></p>
</article>
```

```cpp
web::html home_page(array<map<string, var>> entries, string name, string flash, int views) {
	string title = "Guestbook";
	web::html body = template("views/home.spider", entries, name, flash, views);
	return template("views/layout.spider", title, body);
}
```

A template with a literal path is checked when the module that calls it loads, at the template's own line, and it is part
of that module: an edit to it re-runs the module on the next request, and an edit that does not check is reported in the
console while the app keeps serving the version that did. A list is `${array::map(entries, entry => template("card.spider", entry))}`.
`scoped_template(path, map)` renders a template a visitor or a customer wrote: it sees only a copy of the map and calls only
built-in methods, so it cannot reach the app.

## Options

```
jai_spider app.jai [--port N] [--host ADDR] [--root DIR] [--max-body N] [--timeout S] [--budget S]
                   [--backend vm|interp] [--dev] [--allow-shutdown TOKEN] [--quiet]
```

| option | meaning |
|---|---|
| `--port N` | port to listen on (default 8080; 0 picks a free one, printed in the spinning line) |
| `--host ADDR` | IPv4 address to listen on (default 127.0.0.1; 0.0.0.0 for every interface) |
| `--root DIR` | serve the files under DIR for GET and HEAD before the app sees the request (default: the app's `public/` folder when it exists) |
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

**Request**: `method`, `path` (decoded), `target` (as sent), `query_string`, `query` (`map<string, string>`),
`headers` (names lowercased), `cookies`, `params` (route captures), `body`, `remote_addr`; `header(name)` in any
spelling, `cookie(name)`, `param(name)`, `form()` (urlencoded or multipart fields), `files()`
(`map<string, spider::Upload>`: `filename`, `content_type`, `data`, `size()`, `save(path)`), `json()`.

**Response** (the library's `spider::Response`, the same class the client returns): `status`, `headers`, `cookies`,
`body`; `spider::Response(status, body)`, `header(name, value)`, `set_cookie(name, value, options)` (`max_age`,
`expires`, `path`, `domain`, `secure`, `http_only` (true), `same_site` ("Lax")), `clear_cookie(name)`. Helpers:
`spider::html(text, status)`, `spider::plain_text(text, status)`, `spider::json(value, status)`,
`spider::redirect(url, status = 303)`, `spider::file(path, content_type)`, `spider::not_found(text)`.

**Router**: `get`, `post`, `put`, `patch`, `delete`, `any`, `add(method, ...)`; patterns `/posts/:id` and
`/files/*rest`; `before(filter)` runs ahead of every route (null continues, anything else answers); a HEAD request
takes the GET route; a path that only other methods match answers 405 with `Allow`.

**Client**: `spider::catch(url[, options])` fetches (also spelled `spider::get`), `spider::sling(content, url[,
options])` sends (also spelled `spider::post(url, content[, options])`); options `method`, `headers`, `timeout`,
`type`, `redirects` (5 followed by default). WinHTTP on Windows, sockets elsewhere (HTTPS when the build found
OpenSSL); `JAISCRIPT_HTTP_TRANSPORT=socket` picks the socket transport on Windows. A call blocks the script thread,
and the requests behind it, until its answer arrives.

**Text**: the opt-in `web::` library (`jai::stdlib::register_web`, registered here): `web::html_escape`,
`web::html_unescape`, `web::url_encode` / `url_decode` (RFC 3986), `web::form_encode` / `form_decode` (space as
`+`), `web::parse_query`, `web::parse_query_all`, `web::build_query`, `web::parse_url` (a `web::url` with
`to_string()`). The host adds `parse_cookies`, `parse_multipart`, `http_date(unix_seconds)` and
`content_type_for(path)`; session ids are `crypto::random_hex(32)`, the system's secure random source.

**Database** (SQLite 3.37 or newer: the system's, else the pinned 3.54.1 amalgamation fetched at configure time), injection-proof by construction:

```cpp
Database db = open_database("app.db");          // ":memory:" for a scratch database
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
```

The SQL text must be one of the program's own constants: a string literal, a `const` holding one, a ternary or
`+` of them. Anything else fails the load (`argument 1 of 'query' is not a constant: SQL text must be a constant;
pass values as parameters (?)`), and at run time a text the program does not spell is refused before SQLite sees it,
whatever path it took (`engine::constant_argument`, gathered from every unit as it loads). Values only bind as
parameters; there is no escape or quote function. `sql_choose(value, choices)` answers the choice equal to `value`
for a `{}` slot (a sort column, a direction) and raises for anything else; its choices are constants too. Statements
are prepared once per text and kept until the database closes. An SQLite failure is a catchable error carrying
SQLite's message (`SQLite error: no such table: posts`). Files open in WAL mode.

**Sessions** (`lib/session.jai`): `spider::SessionStore sessions = spider::SessionStore(db);`, then
`spider::Session s = sessions.load(req)`, `s.get/set/erase/take`, `sessions.save(s, response)`,
`sessions.rotate(s)` after a sign-in, `sessions.destroy(s, response)`. The cookie carries a 256-bit random id; the
data stays in the `spider_sessions` table.

Every file function of the runner (`read_file`, `write_file`, `list_dir`, ...), the full standard library and the
`web::` library are there too.

## Hot reload

The host runs `import("app.jai")` before each request, with the engine's import check on every reach. A module
whose file changed (or a template it renders) is parsed and checked together with what it imports before any of it runs; then its functions
and classes replace the old ones and its top-level statements run again: a `spider::Router` built with an
initializer is rebuilt from the new code and the closing `spider::spin` hands it over, and a variable declared
without an initializer (`int requests;`, `map<string, var> cache;`) keeps its value. A rejected edit prints its
listing once, and the app answers with the code it had until the file is saved again. Editing `lib/spider.jai`
itself applies the same way.

## Model and limits

One thread. The network loop keeps every connection moving (reads, writes, files streamed in 256 KiB slices) and
hands complete requests to the script one at a time, so a slow handler (or a slow `spider::catch`) delays the
requests behind it. Requests are HTTP/1.1 or 1.0 with keep-alive and pipelining, bodies by Content-Length or
chunked, `Expect: 100-continue`. The request head is capped at 64 KiB (431), the body at `--max-body` (413), each
connection at `--timeout`, and 512 connections are open at once (503 beyond). A script error is a 500 whose page
shows the error and trace under `--dev` and nothing otherwise; the error is logged either way and the server goes on.

Static files get their Content-Type from the extension, a weak ETag and Last-Modified, and `Cache-Control: no-cache`,
so a browser revalidates and an edited file shows on reload (304 otherwise). A file or folder whose real name starts
with a dot (an 8.3 short name such as `ENV~1` included) and paths that leave the root are never served; a request path
with a `.` or `..` segment, or one decoding to a `/`, `\` or NUL inside a segment, is 400.

The server speaks no TLS: serve behind a reverse proxy (Caddy, nginx) for HTTPS.

## Files

```
main.cpp            options, the engine, hot reload, static files, the 500 page
http_server.hpp     HTTP/1.1 over the runner's sockets (../jaiscript/net_socket.hpp)
http_text.hpp       percent-decoding, cookies, multipart, dates, content types
database.hpp        the Database binding
lib/spider.jai      Request, Upload, the response helpers, Router, spin
lib/session.jai     SessionStore and Session
examples/guestbook  a guestbook: .spider templates, a form, a session, SQLite, a JSON API, a stylesheet
tests/              spider_test.py, the app it drives and the client checks (tools/ci/gates.sh spider)
```
The client's transport is `../jaiscript/http_transport.hpp`, shared with the runner.
