#!/usr/bin/env python3
"""jai_spider's gate: start the host on a free port over a copy of tests/app and drive it over HTTP.

    python tools/jai_spider/tests/spider_test.py --exe build/bin/jai_spider [--runner build/bin/jaiscript]
        [--backend vm|interp|both] [--verbose]

Covers routing, query/form/multipart/JSON parsing, headers and cookies, sessions, SQLite CRUD with bound parameters
and transactions, script errors and the execution budget (500 with the process alive), hot reload between two
requests, a rejected reload that keeps serving, state kept across a reload, static files (types, ETag, 304, HEAD,
traversal), the limits (413, 431, 400), chunked bodies, keep-alive and pipelining, and the shutdown endpoint; then
serves a copy of examples/guestbook and walks its pages, form, session and JSON API. With --runner, the runner's
spider:: client (tests/client.jai, tests/client_catch.jai) calls the same server through each transport the build has:
methods, content kinds, options, the redirect policy, timeouts and errors.
"""
import argparse
import http.client
import json
import os
import pathlib
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
FIXTURE = HERE / "app"
EXAMPLE = HERE.parent / "examples" / "guestbook"
SHUTDOWN_TOKEN = "spider-test-token"
# seconds a wait for what must happen may take: a hang rail, never a speed bound, so machine load never decides a check
HANG_RAIL = 600


class Server:
	def __init__(self, exe, app_dir, backend, extra):
		self.lines = queue.Queue()
		self.log = []
		args = [exe, str(app_dir / "app.jai"), "--port", "0", "--allow-shutdown", SHUTDOWN_TOKEN, "--backend", backend] + extra
		self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=str(app_dir))
		threading.Thread(target=self._pump, daemon=True).start()
		self.port = None
		deadline = time.time() + HANG_RAIL
		while time.time() < deadline:
			try:
				line = self.lines.get(timeout=0.5)
			except queue.Empty:
				if self.process.poll() is not None:
					break
				continue
			if line.startswith("jai_spider: spinning") and " on http://" in line:
				self.port = int(line.split(" on http://", 1)[1].split("/", 1)[0].rsplit(":", 1)[1])
				break
		if self.port is None:
			raise RuntimeError("jai_spider did not start:\n" + "\n".join(self.log))

	def _pump(self):
		for raw in self.process.stdout:
			line = raw.decode("utf-8", "replace").rstrip("\r\n")
			self.log.append(line)
			self.lines.put(line)

	def log_text(self):
		return "\n".join(self.log)

	def stop(self):
		if self.process.poll() is None:
			self.process.kill()
		self.process.wait(timeout=HANG_RAIL)


class Result:
	def __init__(self, status, headers, body):
		self.status = status
		self.headers = headers
		self.body = body

	def header(self, name):
		for key, value in self.headers:
			if key.lower() == name.lower():
				return value
		return None

	def all_headers(self, name):
		return [value for key, value in self.headers if key.lower() == name.lower()]

	def text(self):
		return self.body.decode("utf-8", "replace")

	def json(self):
		return json.loads(self.body.decode("utf-8"))


class Suite:
	def __init__(self, server, app_dir, verbose):
		self.server = server
		self.app_dir = app_dir
		self.verbose = verbose
		self.failures = []
		self.passed = 0
		self.connection = None

	def request(self, method, path, body=None, headers=None, fresh=False):
		if fresh or self.connection is None:
			if self.connection is not None:
				self.connection.close()
			self.connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=HANG_RAIL)
		if isinstance(body, str):
			body = body.encode("utf-8")
		try:
			self.connection.request(method, path, body=body, headers=headers or {})
			response = self.connection.getresponse()
		except (http.client.HTTPException, OSError):
			self.connection.close()
			self.connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=HANG_RAIL)
			self.connection.request(method, path, body=body, headers=headers or {})
			response = self.connection.getresponse()
		data = response.read()
		result = Result(response.status, response.getheaders(), data)
		if response.will_close:
			self.connection.close()
			self.connection = None
		return result

	# read_until_close=False reads up to the end of the first header block (an interim 100 Continue)
	def raw(self, payload, read_until_close=True):
		with socket.create_connection(("127.0.0.1", self.server.port), timeout=HANG_RAIL) as s:
			s.sendall(payload)
			chunks = []
			try:
				while True:
					chunk = s.recv(65536)
					if not chunk:
						break
					chunks.append(chunk)
					if not read_until_close and b"\r\n\r\n" in b"".join(chunks):
						break
			except socket.timeout:
				pass
			return b"".join(chunks)

	def check(self, name, condition, detail=""):
		if condition:
			self.passed += 1
			if self.verbose:
				print("  ok   " + name)
		else:
			self.failures.append(name + (": " + detail if detail else ""))
			print("  FAIL " + name + (": " + detail if detail else ""))

	def wait_for_log(self, needle, timeout=HANG_RAIL):
		deadline = time.time() + timeout
		while time.time() < deadline:
			if needle in self.server.log_text():
				return True
			time.sleep(0.05)
		return False

	def write_module(self, name, text):
		path = self.app_dir / name
		path.write_text(text, encoding="utf-8")
		# a stamp the reload check cannot miss, however coarse the file system's clock
		stamp = time.time() + 2 + len(text)
		os.utime(path, (stamp, stamp))

	def run(self):
		self.routing()
		self.parsing()
		self.cookies_and_sessions()
		self.database()
		self.sql_safety()
		self.errors()
		self.hot_reload()
		self.static_files()
		self.limits_and_framing()

	def routing(self):
		r = self.request("GET", "/")
		self.check("GET / answers the handler's page", r.status == 200 and r.text() == "<h1>home</h1>", r.text())
		self.check("a string answer is HTML", (r.header("Content-Type") or "").startswith("text/html"))
		r = self.request("GET", "/posts/42")
		self.check(":id captures a segment", r.status == 200 and r.json() == {"id": "42"}, r.text())
		r = self.request("GET", "/files/a/b/c.txt")
		self.check("*rest captures the rest of the path", r.json() == {"rest": "a/b/c.txt"}, r.text())
		r = self.request("GET", "/nowhere")
		self.check("an unmatched path is 404", r.status == 404)
		r = self.request("GET", "/only-post")
		self.check("a path of another method is 405 with Allow", r.status == 405 and r.header("Allow") == "POST", str(r.status))
		r = self.request("HEAD", "/")
		self.check("HEAD takes the GET route and sends no body", r.status == 200 and r.body == b"" and r.header("Content-Length") == "13")
		r = self.request("GET", "/redirect")
		self.check("redirect is 303 with Location", r.status == 303 and r.header("Location") == "/target")
		r = self.request("GET", "/null")
		self.check("a null answer is 204", r.status == 204 and r.body == b"")
		r = self.request("GET", "/status/418")
		self.check("Response(status, body) keeps its status", r.status == 418 and r.text() == "custom status")

	def parsing(self):
		r = self.request("GET", "/echo?a=1&b=two%20words&c=x+y&empty&d=%26%3D", headers={"x-test-agent": "probe"})
		got = r.json()
		self.check("query parses: %XX, + as space, a bare key", got["query"] == {"a": "1", "b": "two words", "c": "x y", "empty": "", "d": "&="}, r.text())
		self.check("header() reads any spelling of a name", got["agent"] == "probe", r.text())
		r = self.request("GET", "/echo/caf%C3%A9?x=1")
		self.check("the path is percent-decoded", r.status == 404)
		r = self.request("POST", "/echo/form", "name=Ada+L&msg=%3Cb%3E&n=1", {"Content-Type": "application/x-www-form-urlencoded"})
		self.check("form() parses an urlencoded body", r.json() == {"name": "Ada L", "msg": "<b>", "n": "1"}, r.text())
		r = self.request("POST", "/echo/json", json.dumps({"a": [1, 2], "b": {"c": "d"}}), {"Content-Type": "application/json"})
		self.check("json() reads the body", r.json() == {"got": {"a": [1, 2], "b": {"c": "d"}}}, r.text())
		boundary = "----spiderBoundary7"
		body = (
			"--" + boundary + "\r\n"
			"Content-Disposition: form-data; name=\"title\"\r\n\r\n"
			"Hello there\r\n"
			"--" + boundary + "\r\n"
			"Content-Disposition: form-data; name=\"upload\"; filename=\"notes.txt\"\r\n"
			"Content-Type: text/plain\r\n\r\n"
			"line one\r\nline two\r\n"
			"--" + boundary + "--\r\n")
		r = self.request("POST", "/echo/files", body, {"Content-Type": "multipart/form-data; boundary=" + boundary})
		got = r.json() if r.status == 200 else {}
		self.check("multipart fields land in form()", got.get("fields") == {"title": "Hello there"}, r.text())
		self.check("multipart files land in files()", got.get("files") == {"upload": {"filename": "notes.txt", "size": 18, "type": "text/plain", "data": "line one\r\nline two"}}, r.text())
		r = self.request("POST", "/echo/body", "x" * 1000)
		self.check("a Content-Length body arrives whole", r.json() == {"method": "POST", "body": "x" * 1000, "length": 1000})
		chunked = b"POST /echo/body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n"
		raw = self.raw(chunked)
		self.check("a chunked body is decoded", b'"body":"hello world"' in raw, raw[:200].decode("latin-1"))

	def cookies_and_sessions(self):
		r = self.request("GET", "/cookies", headers={"Cookie": "a=1; b=two%20words; quoted=\"q\""})
		self.check("cookies parse from the Cookie header", r.json() == {"a": "1", "b": "two words", "quoted": "q"}, r.text())
		cookies = r.all_headers("Set-Cookie")
		self.check("set_cookie writes one Set-Cookie per cookie", len(cookies) == 2, str(cookies))
		self.check("set_cookie encodes the value and defaults HttpOnly, SameSite=Lax, Path=/",
			"flavor=oat%20meal; Path=/; Max-Age=60; HttpOnly; SameSite=Lax" in cookies, str(cookies))
		self.check("set_cookie options turn the defaults off", "plain=1; Path=/" in cookies, str(cookies))
		r = self.request("GET", "/session")
		session_cookie = (r.header("Set-Cookie") or "").split(";", 1)[0]
		self.check("a new session starts at one", r.json() == {"seen": 1, "new": True} and session_cookie.startswith("session="), r.text())
		self.check("the session id is 64 hex characters", len(session_cookie.split("=", 1)[1]) == 64, session_cookie)
		r = self.request("GET", "/session", headers={"Cookie": session_cookie})
		self.check("the session cookie brings the data back", r.json() == {"seen": 2, "new": False}, r.text())
		r = self.request("GET", "/session", headers={"Cookie": "session=forged"})
		self.check("an unknown session id starts a new session", r.json() == {"seen": 1, "new": True}, r.text())

	def database(self):
		hostile = "Robert'); DROP TABLE items;--"
		r = self.request("POST", "/items", json.dumps({"name": hostile, "qty": 3, "price": 2.5, "note": None}), {"Content-Type": "application/json"})
		self.check("INSERT through bound parameters", r.status == 201 and r.json().get("id") == 1, r.text())
		r = self.request("POST", "/items", json.dumps({"name": "widget", "qty": 10, "price": 1, "note": "x"}), {"Content-Type": "application/json"})
		r = self.request("GET", "/items")
		rows = r.json()
		self.check("a hostile value is stored as data", len(rows) == 2 and rows[0]["name"] == hostile, r.text())
		self.check("columns come back typed (int, float, string, null)", rows[0]["qty"] == 3 and rows[0]["price"] == 2.5 and rows[0]["note"] is None, r.text())
		r = self.request("GET", "/items/2")
		self.check("query_one with a named parameter", r.json().get("name") == "widget", r.text())
		r = self.request("PUT", "/items/2", json.dumps({"qty": 11}), {"Content-Type": "application/json"})
		self.check("UPDATE reports the rows changed", r.json() == {"changed": 1}, r.text())
		r = self.request("GET", "/items-search?name=" + "widget")
		self.check("a parameter from the query string", r.json() == [{"name": "widget"}], r.text())
		r = self.request("DELETE", "/items/2")
		self.check("DELETE reports the rows changed", r.json() == {"changed": 1}, r.text())
		r = self.request("GET", "/items/2")
		self.check("a missing row is null", r.status == 404)
		r = self.request("POST", "/items/rollback")
		got = r.json()
		self.check("a failing transaction rolls back and raises SQLite's message",
			got.get("count") == 0 and "no such table: nowhere" in str(got.get("error")), r.text())
		r = self.request("POST", "/items/nested")
		self.check("a throw in a nested transaction rolls back to its savepoint only",
			r.json() == {"thrown": "inner fails", "outer": 1, "inner": 0, "open": False}, r.text())
		r = self.request("GET", "/sql-error")
		self.check("an SQL error uncaught is a 500", r.status == 500 and b"no such table: missing_table" in r.body, r.text()[:200])

	def sql_safety(self):
		for name, qty in (("alpha", 101), ("beta", 102), ("gamma", 105)):
			self.request("POST", "/items", json.dumps({"name": name, "qty": qty, "price": qty * 1.5, "note": None}), {"Content-Type": "application/json"})
		r = self.request("GET", "/sql/in")
		self.check("an array argument expands IN (?) to one placeholder per element", r.json() == [{"name": "alpha"}, {"name": "beta"}], r.text())
		r = self.request("GET", "/sql/in-empty")
		self.check("an empty array expands IN (?) to IN ()", r.status == 200 and r.json() == [], r.text())
		names = sorted(row["name"] for row in self.request("GET", "/items").json())
		r = self.request("GET", "/sql/sorted?by=name")
		self.check("sql_choose fills a {} slot with one of its constant choices", [row["name"] for row in r.json()] == names[:len(r.json())] and len(r.json()) > 0, r.text())
		r = self.request("GET", "/sql/sorted?by=qty+DESC")
		self.check("a choice may be a fragment (qty DESC)", r.status == 200 and r.json()[0]["name"] == "gamma", r.text())
		r = self.request("GET", "/sql/sorted?by=name%3B+DROP+TABLE+items")
		self.check("a value outside the choices is refused", r.status == 500 and b"sql_choose: the value is not one of: name, qty DESC, price" in r.body, r.text()[:200])
		count = len(self.request("GET", "/items").json())
		r = self.request("GET", "/sql/injected?n=x%27+OR+%271%27%3D%271")
		self.check("a text built at run time is refused before SQLite sees it", r.status == 500 and b"SQL text must be a constant; pass values as parameters (?)" in r.body, r.text()[:200])
		r = self.request("GET", "/sql/reflected")
		self.check("reflect::invoke cannot pass a text the program does not spell", r.status == 500 and b"SQL text must be a constant" in r.body, r.text()[:200])
		self.check("the refused statements changed nothing", len(self.request("GET", "/items").json()) == count)
		r = self.request("GET", "/sql/fragment-misuse")
		self.check("a sql_choose choice cannot fill a ? value slot", r.status == 500 and b"fills a {} slot, not a ?" in r.body, r.text()[:200])
		r = self.request("POST", "/sql/types")
		self.check("int, float, bool, null and text bind as themselves", r.json() == [
			{"t": "integer", "v": 1}, {"t": "real", "v": 2.5}, {"t": "integer", "v": 1}, {"t": "null", "v": None}, {"t": "text", "v": "it's \"q\" é"}], r.text())

	def errors(self):
		r = self.request("GET", "/boom")
		self.check("a run-time error is a 500 with the error under --dev", r.status == 500 and b"Division by zero" in r.body, r.text()[:200])
		self.check("the error is logged with its trace", self.wait_for_log("Division by zero") and "at <lambda>" in self.server.log_text())
		r = self.request("GET", "/throw")
		self.check("an uncaught throw is a 500", r.status == 500 and b"thrown from a handler" in r.body)
		r = self.request("GET", "/spin")
		self.check("a runaway handler meets the budget", r.status == 500 and b"budget" in r.body, r.text()[:200])
		for code in ("101", "999"):
			r = self.request("GET", "/status/" + code)
			self.check("status " + code + " is refused with a 500", r.status == 500 and b"is not a final HTTP status (200 to 599)" in r.body, r.text()[:200])
		r = self.request("GET", "/bad-header")
		self.check("a header value with a line break is a 500, never a split header", r.status == 500 and r.header("Injected") is None and b"Response header &#39;X-Bad&#39; is not a header name" in r.body, r.text()[:200])
		r = self.request("GET", "/cookie-inject")
		self.check("a cookie attribute holding ';' is refused", r.status == 500 and b"option &#39;path&#39; holds a &#39;;&#39; or a control character" in r.body, r.text()[:200])
		r = self.request("GET", "/")
		self.check("the process serves on after errors", r.status == 200)

	def hot_reload(self):
		r = self.request("GET", "/count")
		r = self.request("GET", "/count")
		self.check("state counts before a reload", r.text() == "2", r.text())
		self.check("the module answers its first version", self.request("GET", "/greeting").text() == "version one")
		self.write_module("greeting.jai", 'string greeting() { return "version two, edited"; }\n')
		r = self.request("GET", "/greeting")
		self.check("an edit applies to the next request", r.text() == "version two, edited", r.text())
		self.check("the reload is logged", self.wait_for_log("reloaded greeting.jai"), self.server.log_text()[-400:])
		self.write_module("greeting.jai", 'string greeting() { int n = "not an int"; return "broken"; }\n')
		r = self.request("GET", "/greeting")
		self.check("a rejected edit keeps serving the code the app had", r.status == 200 and r.text() == "version two, edited", r.text())
		self.check("the rejected edit's listing is logged", self.wait_for_log("cannot initialize 'n' declared 'int'"), self.server.log_text()[-400:])
		r = self.request("GET", "/greeting")
		self.check("the app keeps serving while the edit stays broken", r.text() == "version two, edited", r.text())
		self.write_module("greeting.jai", 'string greeting() { Database d = open_database(":memory:"); string who = "x"; return d.value("SELECT \'" + who + "\'"); }\n')
		r = self.request("GET", "/greeting")
		self.check("an edit that builds SQL text is rejected at load", r.text() == "version two, edited" and self.wait_for_log("argument 1 of 'value' is not a constant: SQL text must be a constant; pass values as parameters (?)"), self.server.log_text()[-400:])
		self.write_module("greeting.jai", 'string greeting() { return "version three after the fix"; }\n')
		r = self.request("GET", "/greeting")
		self.check("the fixed edit applies", r.text() == "version three after the fix", r.text())
		app_text = (self.app_dir / "app.jai").read_text(encoding="utf-8")
		self.write_module("app.jai", app_text.replace('"<h1>home</h1>"', '"<h1>home, edited</h1>"'))
		r = self.request("GET", "/")
		self.check("an edit to the app itself applies", r.text() == "<h1>home, edited</h1>", r.text())
		r = self.request("GET", "/count")
		self.check("a variable declared without an initializer keeps its value across the app's reload", r.text() == "3", r.text())

	def static_files(self):
		css = (self.app_dir / "public" / "style.css").read_bytes()
		r = self.request("GET", "/style.css")
		self.check("a file under the root is served with its type", r.status == 200 and r.header("Content-Type") == "text/css; charset=utf-8" and r.body == css, str(r.headers))
		etag = r.header("ETag")
		self.check("a file carries ETag, Last-Modified and Cache-Control", etag is not None and r.header("Last-Modified") is not None and r.header("Cache-Control") == "no-cache")
		r = self.request("GET", "/style.css", headers={"If-None-Match": etag})
		self.check("If-None-Match answers 304", r.status == 304 and r.body == b"")
		r = self.request("HEAD", "/style.css")
		self.check("HEAD of a file sends its length and no body", r.status == 200 and r.body == b"" and r.header("Content-Length") == str(len(css)))
		r = self.request("GET", "/docs/")
		self.check("a folder serves its index.html", r.status == 200 and b"docs index" in r.body)
		(self.app_dir / "public" / ".hidden").write_text("secret", encoding="utf-8")
		r = self.request("GET", "/.hidden")
		self.check("a dot-file is never served", r.status == 404)
		raw = self.raw(b"GET /../app.jai HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
		self.check("a path with a .. segment is 400", raw.startswith(b"HTTP/1.1 400") and b"import" not in raw, raw[:80].decode("latin-1"))
		raw = self.raw(b"GET /%2e%2e/app.jai HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
		self.check("an encoded .. segment is 400 too", raw.startswith(b"HTTP/1.1 400") and b"import" not in raw, raw[:80].decode("latin-1"))
		raw = self.raw(b"GET /files/..%2fapp.jai HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
		self.check("an encoded / inside a segment is 400, so a *rest capture never climbs", raw.startswith(b"HTTP/1.1 400"), raw[:80].decode("latin-1"))
		if os.name == "nt":
			import ctypes
			(self.app_dir / "public" / ".environment").write_text("secret", encoding="utf-8")
			buffer = ctypes.create_unicode_buffer(1024)
			ctypes.windll.kernel32.GetShortPathNameW(str(self.app_dir / "public" / ".environment"), buffer, 1024)
			short = pathlib.Path(buffer.value).name
			if short and short != ".environment":
				r = self.request("GET", "/" + short)
				self.check("a dot-file is not served by its 8.3 short name (" + short + ")", r.status == 404 and b"secret" not in r.body, str(r.status))
		r = self.request("GET", "/forged-sendfile")
		self.check("an X-Sendfile header spider::file did not write serves no file", r.status == 200 and r.text() == "forged", r.text()[:80])
		self.check("X-Sendfile never reaches the client", r.header("X-Sendfile") is None, str(r.headers))
		r = self.request("GET", "/download")
		self.check("file(path) streams a file with the type it was given", r.status == 200 and r.header("Content-Type") == "text/x-test" and r.body == css, str(r.headers))
		big = self.app_dir / "public" / "big.bin"
		big.write_bytes(bytes(range(256)) * 8192)
		r = self.request("GET", "/big.bin")
		self.check("a large file streams whole", r.status == 200 and len(r.body) == 256 * 8192 and r.body == big.read_bytes())

	def limits_and_framing(self):
		raw = self.raw(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\nGET /posts/7 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
		self.check("pipelined requests are answered in order", raw.count(b"HTTP/1.1 200") == 2 and raw.index(b"home") < raw.index(b'"7"'), raw.decode("latin-1")[:300])
		c = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=HANG_RAIL)
		c.request("GET", "/")
		first = c.getresponse()
		first.read()
		c.request("GET", "/posts/1")
		second = c.getresponse()
		self.check("keep-alive serves two requests on one connection", first.status == 200 and second.status == 200 and second.read() == b'{"id":"1"}')
		c.close()
		raw = self.raw(b"POST /echo/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 70000\r\n\r\n" + b"x" * 1000)
		self.check("a body over spin's max_body is 413", raw.startswith(b"HTTP/1.1 413"), raw[:80].decode("latin-1"))
		chunk = b"%x\r\n" % 40000 + b"y" * 40000 + b"\r\n"
		raw = self.raw(b"POST /echo/body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk + chunk)
		self.check("a chunked body over spin's max_body is 413", raw.startswith(b"HTTP/1.1 413"), raw[:80].decode("latin-1"))
		raw = self.raw(b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Big: " + b"z" * 70000 + b"\r\n\r\n")
		self.check("a head over the limit is 431", raw.startswith(b"HTTP/1.1 431"), raw[:80].decode("latin-1"))
		raw = self.raw(b"GET / HTTP/1.1\r\nHost: localhost\r\nX-A: b\nInjected: c\r\nConnection: close\r\n\r\n")
		self.check("a bare LF inside a header field is 400", raw.startswith(b"HTTP/1.1 400"), raw[:80].decode("latin-1"))
		raw = self.raw(b"GET / HTTP/1.1\r\nHost: rebound.example:8080\r\nConnection: close\r\n\r\n")
		self.check("a Host that is not this machine is 421 on a loopback listener", raw.startswith(b"HTTP/1.1 421"), raw[:80].decode("latin-1"))
		raw = self.raw(b"GET / HTTP/1.1\r\nHost: [::1]:80\r\nConnection: close\r\n\r\n")
		self.check("Host [::1] is this machine", raw.startswith(b"HTTP/1.1 200"), raw[:80].decode("latin-1"))
		idle = [socket.create_connection(("127.0.0.1", self.server.port), timeout=HANG_RAIL) for _ in range(128)]
		try:
			raw = self.raw(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
			self.check("one address holds at most 128 connections", raw.startswith(b"HTTP/1.1 503") and b"from one address" in raw, raw[:120].decode("latin-1"))
		finally:
			for s in idle:
				s.close()
		served = False
		deadline = time.time() + HANG_RAIL
		while time.time() < deadline:
			if self.raw(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").startswith(b"HTTP/1.1 200"):
				served = True
				break
			time.sleep(0.1)
		self.check("closed connections free their address's share", served)
		raw = self.raw(b"NOT A REQUEST\r\n\r\n")
		self.check("a malformed request line is 400", raw.startswith(b"HTTP/1.1 400"), raw[:80].decode("latin-1"))
		raw = self.raw(b"GET / HTTP/2.0\r\n\r\n")
		self.check("an unknown version is 505", raw.startswith(b"HTTP/1.1 505"), raw[:80].decode("latin-1"))
		raw = self.raw(b"POST /echo/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc")
		self.check("Content-Length with Transfer-Encoding is 400", raw.startswith(b"HTTP/1.1 400"), raw[:80].decode("latin-1"))
		raw = self.raw(b"POST /echo/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n", read_until_close=False)
		self.check("Expect: 100-continue is answered", raw.startswith(b"HTTP/1.1 100 Continue"), raw[:80].decode("latin-1"))
		r = self.request("GET", "/", fresh=True)
		self.check("the server serves on after malformed traffic", r.status == 200)


def client_transports():
	# the transport a build installs by default, and on Windows the socket transport too (JAISCRIPT_HTTP_TRANSPORT=socket)
	return [None, "socket"] if os.name == "nt" else [None]


def run_client(suite, runner, backend, transport):
	env = dict(os.environ)
	label = "default transport"
	if transport:
		env["JAISCRIPT_HTTP_TRANSPORT"] = transport
		label = transport + " transport"
	base = "http://127.0.0.1:" + str(suite.server.port)
	result = subprocess.run([runner, "--backend=" + backend, str(HERE / "client.jai"), "--", base], capture_output=True, timeout=HANG_RAIL, env=env)
	out = result.stdout.decode("utf-8", "replace")
	if suite.verbose:
		print(out)
	for line in out.splitlines():
		if line.startswith("FAIL "):
			suite.check("client (" + label + "): " + line[5:], False)
	summary = [line for line in out.splitlines() if line.startswith("client: ")]
	suite.check("client (" + label + "): every check ran and held", result.returncode == 0 and summary and summary[-1].endswith(" 0 failed") and not summary[-1].startswith("client: 0 passed"),
		(summary[-1] if summary else "no summary") + " " + result.stderr.decode("utf-8", "replace")[-600:])
	result = subprocess.run([runner, "--backend=" + backend, str(HERE / "client_catch.jai"), "--", base], capture_output=True, timeout=HANG_RAIL, env=env)
	out = (result.stdout + result.stderr).decode("utf-8", "replace")
	suite.check("client (" + label + "): spider::catch is spider::get", "catch: ok" in out, out[-600:])


def run_backend(exe, runner, backend, verbose):
	work = pathlib.Path(tempfile.mkdtemp(prefix="spider_test_"))
	app_dir = work / "app"
	shutil.copytree(FIXTURE, app_dir)
	server = Server(exe, app_dir, backend, ["--max-body", "1M", "--budget", "20", "--timeout", str(HANG_RAIL), "--dev"])
	suite = Suite(server, app_dir, verbose)
	try:
		suite.run()
		if runner:
			for transport in client_transports():
				run_client(suite, runner, backend, transport)
		r = suite.request("POST", "/__spider/shutdown", fresh=True)
		suite.check("POST /__spider/shutdown without the token is 403", r.status == 403)
		r = suite.request("POST", "/__spider/shutdown", headers={"X-Spider-Shutdown": SHUTDOWN_TOKEN}, fresh=True)
		suite.check("POST /__spider/shutdown with the token stops the server", r.status == 200)
		try:
			code = server.process.wait(timeout=HANG_RAIL)
			suite.check("the process exits cleanly after the shutdown request", code == 0, "exit code " + str(code))
		except subprocess.TimeoutExpired:
			suite.check("the process exits cleanly after the shutdown request", False, "still running")
	finally:
		server.stop()
		if suite.failures or verbose:
			print("---- jai_spider log (" + backend + ")")
			print(server.log_text())
		shutil.rmtree(work, ignore_errors=True)
	print("jai_spider " + backend + ": " + str(suite.passed) + " passed, " + str(len(suite.failures)) + " failed")
	return not suite.failures


def run_broken_start(exe, backend, verbose):
	work = pathlib.Path(tempfile.mkdtemp(prefix="spider_broken_"))
	app_dir = work / "app"
	shutil.copytree(FIXTURE, app_dir)
	(app_dir / "greeting.jai").write_text('string greeting() { return missing_name; }\n', encoding="utf-8")
	server = Server(exe, app_dir, backend, [])
	suite = Suite(server, app_dir, verbose)
	try:
		suite.check("an app that does not load is reported at start", suite.wait_for_log("jai_spider: the app did not load:") and suite.wait_for_log("missing_name"))
		r = suite.request("GET", "/greeting")
		suite.check("an app that never loaded answers 500, its listing only in the log without --dev", r.status == 500 and b"missing_name" not in r.body, r.text()[:300])
		suite.write_module("greeting.jai", 'string greeting() { return "fixed"; }\n')
		r = suite.request("GET", "/greeting")
		suite.check("the fixed app serves on the next request", r.status == 200 and r.text() == "fixed", r.text()[:300])
		suite.check("the recovery is logged", suite.wait_for_log("jai_spider: the app loads"))
	finally:
		server.stop()
		if suite.failures or verbose:
			print("---- broken start log (" + backend + ")")
			print(server.log_text())
		shutil.rmtree(work, ignore_errors=True)
	print("broken start " + backend + ": " + str(suite.passed) + " passed, " + str(len(suite.failures)) + " failed")
	return not suite.failures


def run_guestbook(exe, backend, verbose):
	work = pathlib.Path(tempfile.mkdtemp(prefix="spider_guestbook_"))
	app_dir = work / "guestbook"
	shutil.copytree(EXAMPLE, app_dir, ignore=shutil.ignore_patterns("*.db", "*.db-*", ".cache"))
	server = Server(exe, app_dir, backend, [])
	suite = Suite(server, app_dir, verbose)
	try:
		r = suite.request("GET", "/")
		suite.check("guestbook: the home page renders", r.status == 200 and b"No one has signed yet." in r.body, r.text()[:200])
		r = suite.request("GET", "/style.css")
		suite.check("guestbook: the stylesheet is served", r.status == 200 and r.header("Content-Type") == "text/css; charset=utf-8")
		r = suite.request("POST", "/sign", "name=Ada&message=%3Cb%3Ehi%3C%2Fb%3E", {"Content-Type": "application/x-www-form-urlencoded"})
		cookie = (r.header("Set-Cookie") or "").split(";", 1)[0]
		suite.check("guestbook: signing redirects home with a session cookie", r.status == 303 and cookie.startswith("session="), str(r.headers))
		r = suite.request("GET", "/", headers={"Cookie": cookie})
		page = r.text()
		suite.check("guestbook: the entry shows, escaped", "&lt;b&gt;hi&lt;/b&gt;" in page and "<b>hi</b>" not in page, page[:400])
		suite.check("guestbook: the session remembers the name and the flash", 'value="Ada"' in page and "Thanks for signing, Ada." in page, page[:400])
		r = suite.request("POST", "/api/entries", json.dumps({"name": "Bot", "message": "via the API"}), {"Content-Type": "application/json"})
		suite.check("guestbook: the JSON API adds an entry", r.status == 201 and r.json().get("name") == "Bot", r.text())
		r = suite.request("GET", "/api/entries")
		suite.check("guestbook: the JSON API lists the entries", [e["name"] for e in r.json()] == ["Bot", "Ada"], r.text())
		r = suite.request("GET", "/entries/1")
		suite.check("guestbook: an entry has its own page", r.status == 200 and b"Ada signed the guestbook" in r.body)
		card = app_dir / "views" / "card.spider"
		original = card.read_text(encoding="utf-8")
		card.write_text(original.replace("<article>", '<article class="edited">'), encoding="utf-8")
		r = suite.request("GET", "/")
		suite.check("guestbook: an edited template applies on the next request", b'<article class="edited">' in r.body, r.text()[:400])
		card.write_text(original.replace('${entry["message"]}', "${missing_name}"), encoding="utf-8")
		r = suite.request("GET", "/")
		suite.check("guestbook: a template edit that does not check leaves the last version serving",
			r.status == 200 and b'<article class="edited">' in r.body and b"&lt;b&gt;hi&lt;/b&gt;" in r.body, r.text()[:400])
		suite.check("guestbook: the rejected template edit is reported", "missing_name" in server.log_text(), server.log_text()[-400:])
	finally:
		server.stop()
		if suite.failures or verbose:
			print("---- guestbook log (" + backend + ")")
			print(server.log_text())
		shutil.rmtree(work, ignore_errors=True)
	print("guestbook " + backend + ": " + str(suite.passed) + " passed, " + str(len(suite.failures)) + " failed")
	return not suite.failures


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--exe", required=True)
	parser.add_argument("--runner", default="")
	parser.add_argument("--backend", default="both", choices=["vm", "interp", "both"])
	parser.add_argument("--verbose", action="store_true")
	args = parser.parse_args()
	exe = args.exe
	if not os.path.isfile(exe) and os.path.isfile(exe + ".exe"):
		exe += ".exe"
	runner = args.runner
	if runner and not os.path.isfile(runner) and os.path.isfile(runner + ".exe"):
		runner += ".exe"
	backends = ["vm", "interp"] if args.backend == "both" else [args.backend]
	results = []
	for backend in backends:
		results += [run_backend(exe, runner, backend, args.verbose), run_broken_start(exe, backend, args.verbose), run_guestbook(exe, backend, args.verbose)]
	ok = all(results)
	print("jai_spider: " + ("every check holds" if ok else "FAILED"))
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
