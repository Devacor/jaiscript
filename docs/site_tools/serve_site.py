"""Serves the site locally the way jaiscript.com serves it: the Play pages cross-origin isolated (as play/.htaccess makes
them on Apache), .wasm as application/wasm, and 404.html for a path that is not there.

    python docs/site_tools/serve_site.py [--port 8000] [--root docs/site] [--no-isolation]

--no-isolation leaves the headers off, so the Play pages fall back to the stepped engine.
"""
import argparse
import functools
import http.server
import pathlib

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"


class SiteHandler(http.server.SimpleHTTPRequestHandler):
    isolate = True

    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript", ".json": "application/json"}

    def end_headers(self):
        if self.isolate and self.path.startswith("/play/"):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_error(self, code, message=None, explain=None):
        page = pathlib.Path(self.directory) / "404.html"
        if code != 404 or self.command != "GET" or not page.is_file():
            super().send_error(code, message, explain)
            return
        body = page.read_bytes()
        self.send_response(404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--root", default=str(SITE))
    parser.add_argument("--no-isolation", action="store_true")
    args = parser.parse_args()
    SiteHandler.isolate = not args.no_isolation
    handler = functools.partial(SiteHandler, directory=args.root)
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print("serving %s on http://127.0.0.1:%d/" % (args.root, args.port), flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
