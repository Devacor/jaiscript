#!/usr/bin/env python3
"""Protocol tests for jaiscript_lsp: drives the server over stdio like an editor does.

  python lsp_test.py [--exe PATH] [--update] [--only fixtures|gatorquest|latency|robustness] [--verbose]

fixtures    golden responses for every feature on tests/fixtures (--update rewrites golden/fixtures.json), and
            definition/hover/completion/references on keyword-named members (tests/keyword_fixture)
gatorquest  examples/gatorquest: zero diagnostics on the clean program, definition/references/hover/
            completion/rename spot checks
latency     examples/gatorquest: an edit re-checks only what it changes; the time from an edit to its published
            diagnostics is printed
robustness  malformed framing, oversized and malformed messages, edit floods and cut-off input: the server
            stays up, answers the next request and holds no memory for what it skips
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.parse import unquote

HERE = Path(__file__).resolve().parent
JAISCRIPT = HERE.parents[2]
FIXTURES = HERE / "fixtures"
GOLDEN = HERE / "golden" / "fixtures.json"
GATORQUEST = JAISCRIPT / "examples" / "gatorquest" / "scripts"
HANG_GUARD = 600   # seconds: a wait for what must happen fails only after this long without it, it never bounds speed


def default_exe():
    for config in ("x64-Release BENCHMARKS", "x64-Release", "x64-Debug"):
        candidate = JAISCRIPT / "out" / "build" / config / "bin" / ("jaiscript_lsp.exe" if os.name == "nt" else "jaiscript_lsp")
        if candidate.exists():
            return candidate
    return None


def norm_uri(value):
    """One spelling per file: percent-decoded, lower-case drive (the server answers VS Code's own form)."""
    if not isinstance(value, str) or not value.startswith("file://"):
        return value
    value = unquote(value)
    if len(value) > 9 and value[9] == ":":
        value = value[:8] + value[8].lower() + value[9:]
    return value


def uri_of(path):
    return norm_uri(Path(path).resolve().as_uri())


class Client:
    def __init__(self, exe, root, args=()):
        self.proc = subprocess.Popen([str(exe), *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.next_id = 1
        self.responses = {}
        self.diagnostics = {}
        self.diagnostic_log = []
        self.cv = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.versions = {}
        self.texts = {}
        result = self.request("initialize", {
            "processId": os.getpid(),
            "rootUri": uri_of(root),
            "workspaceFolders": [{"uri": uri_of(root), "name": Path(root).name}],
            "capabilities": {},
            "initializationOptions": {"host": "runner"},
        })
        self.capabilities = result["capabilities"]
        self.notify("initialized", {})

    def _read(self):
        stream = self.proc.stdout
        while True:
            length = None
            while True:
                line = stream.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            body = stream.read(length)
            message = json.loads(body.decode("utf-8"))
            with self.cv:
                if "id" in message and "method" not in message:
                    self.responses[message["id"]] = message
                elif message.get("method") == "textDocument/publishDiagnostics":
                    params = message["params"]
                    params["uri"] = norm_uri(params["uri"])
                    self.diagnostics[params["uri"]] = params
                    self.diagnostic_log.append((time.perf_counter(), params))
                self.cv.notify_all()

    def _send(self, message):
        body = json.dumps(message).encode("utf-8")
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def request(self, method, params, timeout=HANG_GUARD):
        rid = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        deadline = time.monotonic() + timeout
        with self.cv:
            while rid not in self.responses:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(method)
                self.cv.wait(remaining)
            message = self.responses.pop(rid)
        if "error" in message:
            return {"__error__": message["error"]["message"]}
        return message.get("result")

    def notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def open(self, path):
        text = Path(path).read_text(encoding="utf-8")
        uri = uri_of(path)
        self.versions[uri] = 1
        self.texts[uri] = text
        self.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "jaiscript", "version": 1, "text": text}})
        return uri

    def change(self, uri, start, end, text):
        self.versions[uri] += 1
        self.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": self.versions[uri]},
            "contentChanges": [{"range": {"start": start, "end": end}, "text": text}],
        })
        return self.versions[uri]

    def wait_diagnostics(self, uri, version=None, timeout=HANG_GUARD):
        deadline = time.monotonic() + timeout
        with self.cv:
            while True:
                params = self.diagnostics.get(uri)
                if params is not None and (version is None or params.get("version") == version):
                    return params
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("diagnostics for " + uri)
                self.cv.wait(remaining)

    def settle(self, timeout=HANG_GUARD):
        """the background checks drained; a hang is their pending count standing still for the guard, never a slow drain"""
        pending, moved = None, time.monotonic()
        while True:
            status = self.request("jaiscript/status", {})
            if status["pending"] == 0:
                return status
            if status["pending"] != pending:
                pending, moved = status["pending"], time.monotonic()
            elif time.monotonic() - moved >= timeout:
                raise TimeoutError("background checks")
            time.sleep(0.05)

    def close(self):
        try:
            self.request("shutdown", None, timeout=10)
            self.notify("exit", None)
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def position_of(text, needle, occurrence=1, delta=0):
    index = -1
    for _ in range(occurrence):
        index = text.index(needle, index + 1)
    index += delta
    line = text.count("\n", 0, index)
    line_start = text.rfind("\n", 0, index) + 1
    character = len(text[line_start:index].encode("utf-16-le")) // 2
    return {"line": line, "character": character}


def relativize(value, root):
    prefix = uri_of(root) + "/"
    if isinstance(value, str):
        return norm_uri(value).replace(prefix, "")
    if isinstance(value, list):
        return [relativize(v, root) for v in value]
    if isinstance(value, dict):
        return {relativize(k, root): relativize(v, root) for k, v in value.items()}
    return value


class Checks:
    def __init__(self, verbose):
        self.failures = []
        self.passed = 0
        self.verbose = verbose

    def check(self, condition, what, detail=""):
        if condition:
            self.passed += 1
            if self.verbose:
                print("  ok   " + what)
        else:
            self.failures.append(what + (": " + detail if detail else ""))
            print("  FAIL " + what + (": " + str(detail)[:400] if detail else ""))


def fixture_words():
    words = set()
    for path in FIXTURES.rglob("*.jai"):
        token = ""
        for ch in path.read_text(encoding="utf-8") + " ":
            if ch.isalnum() or ch == "_":
                token += ch
            else:
                if token:
                    words.add(token)
                token = ""
    return words


def run_fixtures(exe, update, checks):
    print("fixtures")
    client = Client(exe, FIXTURES)
    results = {}
    try:
        main = FIXTURES / "program" / "main.jai"
        lib = FIXTURES / "program" / "lib.jai"
        main_uri = client.open(main)
        main_text = client.texts[main_uri]
        lib_text = lib.read_text(encoding="utf-8")
        client.settle()
        results["diagnostics.main"] = client.wait_diagnostics(main_uri)["diagnostics"]
        status = client.request("jaiscript/status", {})
        results["status.program"] = sorted((relativize(u["uri"], FIXTURES), u["diagnostics"]) for u in status["units"])

        def at(text, needle, occurrence=1, delta=0):
            return position_of(text, needle, occurrence, delta)

        def ask(name, method, uri, text, needle, occurrence=1, delta=0, extra=None):
            params = {"textDocument": {"uri": uri}, "position": at(text, needle, occurrence, delta)}
            if extra:
                params.update(extra)
            results[name] = client.request(method, params)

        lib_uri = uri_of(lib)
        ask("hover.field", "textDocument/hover", main_uri, main_text, "h.hp", 1, 2)
        ask("hover.method_override", "textDocument/hover", main_uri, main_text, "h.heal", 1, 2)
        ask("hover.local_auto", "textDocument/hover", main_uri, main_text, "party =", 1, 0)
        ask("hover.class", "textDocument/hover", main_uri, main_text, "Hero h", 1, 0)
        ask("hover.namespace_fn", "textDocument/hover", main_uri, main_text, "shout", 1, 0)
        ask("hover.doc_comment", "textDocument/hover", main_uri, main_text, "twice", 1, 0)
        ask("hover.enum_value", "textDocument/hover", main_uri, main_text, "green", 1, 0)
        ask("hover.host_fn", "textDocument/hover", main_uri, main_text, "to_string", 1, 0)
        ask("hover.param", "textDocument/hover", main_uri, main_text, "xs)", 2, 0)
        ask("hover.base_ref_method", "textDocument/hover", main_uri, main_text, "a.power", 1, 2)
        ask("definition.global", "textDocument/definition", main_uri, main_text, "LIMIT", 1, 0)
        ask("definition.class", "textDocument/definition", main_uri, main_text, "Hero(", 1, 0)
        ask("definition.member_via_element", "textDocument/definition", main_uri, main_text, "party[0].heal", 1, 9)
        ask("definition.namespace_fn", "textDocument/definition", main_uri, main_text, "twice", 1, 0)
        ask("definition.local", "textDocument/definition", main_uri, main_text, "sum;", 1, 0)
        ask("definition.import_path", "textDocument/definition", main_uri, main_text, "lib.jai", 1, 1)
        ask("definition.enum", "textDocument/definition", main_uri, main_text, "Color", 1, 0)
        ask("references.field_hp", "textDocument/references", main_uri, main_text, "h.hp", 1, 2, {"context": {"includeDeclaration": True}})
        ask("references.class_Hero", "textDocument/references", lib_uri, lib_text, "class Hero", 1, 6, {"context": {"includeDeclaration": True}})
        ask("references.local_x", "textDocument/references", main_uri, main_text, "auto x", 1, 5, {"context": {"includeDeclaration": True}})
        ask("references.method_heal_base", "textDocument/references", lib_uri, lib_text, "void heal", 1, 5, {"context": {"includeDeclaration": False}})
        ask("references.method_power", "textDocument/references", lib_uri, lib_text, "int power", 1, 4, {"context": {"includeDeclaration": True}})
        ask("references.namespace", "textDocument/references", main_uri, main_text, "util::", 1, 0, {"context": {"includeDeclaration": True}})
        ask("highlight.local_sum", "textDocument/documentHighlight", main_uri, main_text, "sum = 0", 1, 0)
        ask("signature.ctor", "textDocument/signatureHelp", main_uri, main_text, "\"gator\"", 1, 0)
        ask("signature.method", "textDocument/signatureHelp", main_uri, main_text, "h.heal(5", 1, 7)
        ask("signature.namespace_fn", "textDocument/signatureHelp", main_uri, main_text, "util::twice(1", 1, 12)
        ask("rename.prepare_local", "textDocument/prepareRename", main_uri, main_text, "sum = 0", 1, 0)
        ask("rename.local", "textDocument/rename", main_uri, main_text, "sum = 0", 1, 0, {"newName": "acc"})
        ask("rename.field", "textDocument/rename", main_uri, main_text, "h.hp", 1, 2, {"newName": "health"})
        ask("rename.method_hierarchy", "textDocument/rename", lib_uri, lib_text, "void heal", 1, 5, {"newName": "mend"})
        ask("rename.refuse_host", "textDocument/rename", main_uri, main_text, "print", 1, 0, {"newName": "say"})
        ask("rename.refuse_dynamic_receiver", "textDocument/rename", lib_uri, lib_text, "int level", 1, 4, {"newName": "rank"})
        ask("rename.refuse_bad_name", "textDocument/rename", main_uri, main_text, "sum = 0", 1, 0, {"newName": "while"})
        ask("rename.refuse_collision", "textDocument/rename", main_uri, main_text, "total(", 1, 0, {"newName": "run"})
        results["symbols.document.lib"] = client.request("textDocument/documentSymbol", {"textDocument": {"uri": lib_uri}})
        results["symbols.workspace.her"] = sorted(
            (s["name"], relativize(s["location"]["uri"], FIXTURES)) for s in client.request("workspace/symbol", {"query": "her"}))

        words = fixture_words()

        def completion(name, text_edit_line, needle, delta):
            # type the edit, ask for completion at its end, then undo it
            pos = at(main_text, needle, 1, delta)
            version = client.change(main_uri, pos, pos, text_edit_line)
            end = {"line": pos["line"], "character": pos["character"] + len(text_edit_line)}
            result = client.request("textDocument/completion", {"textDocument": {"uri": main_uri}, "position": end})
            client.change(main_uri, pos, end, "")
            labels = sorted({item["label"] for item in result["items"] if item["label"] in words})
            results[name] = labels
            return version

        completion("completion.member_object", "h.", "h.heal(5);", 0)
        completion("completion.member_base_ref", "a.", "print(", 0)
        completion("completion.member_partial", "h.he", "h.heal(5);", 0)
        completion("completion.namespace", "util::", "string s", 0)
        completion("completion.enum", "Color.", "string s", 0)
        completion("completion.scope_locals", "", "print(", 0)
        completion("completion.static_less_call", "party[0].", "print(", 0)
        client.settle()

        errors_root = FIXTURES / "errors" / "root.jai"
        root_uri = client.open(errors_root)
        client.settle()
        results["diagnostics.errors.root"] = client.wait_diagnostics(root_uri)["diagnostics"]
        for leaf in ("typed_leaf.jai", "broken_leaf.jai"):
            leaf_uri = uri_of(FIXTURES / "errors" / leaf)
            results["diagnostics.errors." + leaf] = client.diagnostics.get(leaf_uri, {}).get("diagnostics")

        # an edit that breaks and then fixes the syntax: the error appears at the edited line and goes away
        pos = at(main_text, "int hp = h.hp;", 1, 0)
        version = client.change(main_uri, pos, pos, "int broken = ;\n\t")
        broken = client.wait_diagnostics(main_uri, version)["diagnostics"]
        end = {"line": pos["line"] + 1, "character": 1}
        version = client.change(main_uri, pos, end, "")
        fixed = client.wait_diagnostics(main_uri, version)["diagnostics"]
        results["diagnostics.edit_breaks"] = broken
        results["diagnostics.edit_fixes"] = fixed
    finally:
        client.close()

    results = relativize(results, FIXTURES)
    if update or not GOLDEN.exists():
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_text(json.dumps(results, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print("  wrote " + str(GOLDEN))
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    actual = json.loads(json.dumps(results, sort_keys=True))
    for name in sorted(set(golden) | set(actual)):
        checks.check(golden.get(name) == actual.get(name), "golden " + name,
                     "expected " + json.dumps(golden.get(name))[:300] + "\n       got " + json.dumps(actual.get(name))[:300])


def run_keyword_members(exe, checks):
    print("keyword members")
    root = HERE / "keyword_fixture"
    client = Client(exe, root)
    try:
        uri = client.open(root / "keywords.jai")
        client.settle()
        checks.check(not client.wait_diagnostics(uri)["diagnostics"], "keyword members: no diagnostics", str(client.diagnostics.get(uri)))
        text = client.texts[uri]
        lines = text.split("\n")

        def line_of(pattern):
            return next(i for i, line in enumerate(lines) if re.search(pattern, line))

        def at(needle, occurrence=1, delta=0):
            return {"textDocument": {"uri": uri}, "position": position_of(text, needle, occurrence, delta)}

        def definition(needle, occurrence=1, delta=0):
            r = client.request("textDocument/definition", at(needle, occurrence, delta))
            return r["range"]["start"]["line"] if r else None

        def hover(needle, occurrence=1, delta=0):
            r = client.request("textDocument/hover", at(needle, occurrence, delta))
            return r["contents"]["value"] if r else ""

        def completion(typed, needle):
            pos = position_of(text, needle)
            client.change(uri, pos, pos, typed)
            end = {"line": pos["line"], "character": pos["character"] + len(typed)}
            result = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": end})
            client.change(uri, pos, end, "")
            return {item["label"]: item.get("kind") for item in result["items"]}

        field = line_of(r"^\s+int default = 7;")
        method = line_of(r"^\s+int catch\(int code\) \{")
        override = line_of(r"^\s+override int catch")
        ns_fn = line_of(r"^\s+string catch\(string url\)")
        checks.check(definition("p.catch", 1, 2) == method, "keyword members: definition p.catch -> the method", str(definition("p.catch", 1, 2)))
        checks.check(definition("p.default", 1, 2) == field, "keyword members: definition p.default -> the field", str(definition("p.default", 1, 2)))
        checks.check(definition("this.default", 1, 5) == field, "keyword members: definition this.default -> the field")
        checks.check(definition("r.catch", 1, 2) == override, "keyword members: definition r.catch -> the override", str(definition("r.catch", 1, 2)))
        checks.check(definition("super::catch", 1, 7) == method, "keyword members: definition super::catch -> the base method", str(definition("super::catch", 1, 7)))
        checks.check(definition("spider::catch(\"", 1, 8) == ns_fn, "keyword members: definition spider::catch -> the namespace function",
                     str(definition("spider::catch(\"", 1, 8)))
        checks.check(definition("spider::import", 1, 8) == line_of(r"^\s+int import = 3;"), "keyword members: definition spider::import")
        checks.check(definition("Mode::for", 1, 6) == line_of(r"^enum Mode"), "keyword members: definition Mode::for -> the enum",
                     str(definition("Mode::for", 1, 6)))
        h = hover("p.catch", 1, 2)
        checks.check("int catch(int code)" in h and "Promise" in h, "keyword members: hover p.catch shows the method", h)
        h = hover("p.default", 1, 2)
        checks.check("default" in h and "field" in h, "keyword members: hover p.default shows the field", h)
        h = hover("spider::catch(\"", 1, 8)
        checks.check("catch(string url)" in h, "keyword members: hover spider::catch shows the function", h)
        sig = client.request("textDocument/signatureHelp", at("p.catch(1", 1, 8))
        labels = [s["label"] for s in (sig or {}).get("signatures", [])]
        checks.check(any("catch(int code)" in l for l in labels), "keyword members: signature help in p.catch(", str(labels))
        members = completion("p.", "print(settled)")
        checks.check(all(n in members for n in ("default", "catch", "new")), "keyword members: completion after p. lists them", str(sorted(members)))
        scoped = completion("spider::", "print(settled)")
        checks.check(all(n in scoped for n in ("catch", "import")), "keyword members: completion after spider:: lists them", str(sorted(scoped)))
        refs = client.request("textDocument/references", dict(at("p.catch", 1, 2), context={"includeDeclaration": True}))
        checks.check(sorted(r["range"]["start"]["line"] for r in refs or []).count(method) == 1 and len(refs or []) >= 2,
                     "keyword members: references to Promise.catch", str(refs)[:300])
    finally:
        client.close()


def run_gatorquest(exe, checks):
    print("gatorquest")
    client = Client(exe, GATORQUEST.parent)
    try:
        root = GATORQUEST / "gatorquest.jai"
        root_uri = client.open(root)
        start = time.perf_counter()
        client.wait_diagnostics(root_uri)
        status = client.settle()
        print("  program checked in %.0f ms (%d units)" % ((time.perf_counter() - start) * 1000, len(status["units"])))
        noisy = [(relativize(u["uri"], GATORQUEST.parent), u["diagnostics"]) for u in status["units"] if u["diagnostics"]]
        checks.check(len(status["units"]) >= 33, "every unit of the program is checked", str(len(status["units"])))
        checks.check(not noisy, "zero diagnostics on the clean program", str(noisy))
        for uri, params in client.diagnostics.items():
            checks.check(not params["diagnostics"], "no published diagnostics: " + relativize(uri, GATORQUEST.parent), str(params["diagnostics"])[:300])

        text = client.texts[root_uri]

        def at(needle, occurrence=1, delta=0):
            return position_of(text, needle, occurrence, delta)

        def definition(needle, occurrence=1, delta=0):
            r = client.request("textDocument/definition", {"textDocument": {"uri": root_uri}, "position": at(needle, occurrence, delta)})
            return (relativize(r["uri"], GATORQUEST.parent), r["range"]["start"]["line"] + 1) if r else None

        def declared(file, pattern, after=None):
            # the example keeps changing, so the expected line is read from the file
            lines = (GATORQUEST / file).read_text(encoding="utf-8").split("\n")
            start = 0 if after is None else declared(file, after)[1]
            line = next(i for i in range(start, len(lines)) if re.search(pattern, lines[i])) + 1
            return ("scripts/" + file, line)

        chars = declared("gq_state.jai", r"^array<Character> CHARS;")
        character = declared("gq_types.jai", r"^class Character\b")
        mob_live = declared("gq_mobs.jai", r"^bool mob_live\(")
        name = declared("gq_types.jai", r"^\s+string name\b", r"^class Character\b")
        pack = declared("gq_types.jai", r"^\s+array<Item> pack;", r"^class Character\b")
        checks.check(definition("CHARS[slot]") == chars, "definition CHARS -> gq_state.jai:%d" % chars[1], str(definition("CHARS[slot]")))
        checks.check(definition("Character& c") == character, "definition Character -> gq_types.jai:%d" % character[1], str(definition("Character& c")))
        checks.check(definition("mob_live(m)") == mob_live, "definition mob_live -> gq_mobs.jai:%d" % mob_live[1], str(definition("mob_live(m)")))
        checks.check(definition("c.name", 1, 2) == name, "definition c.name -> Character.name", str(definition("c.name", 1, 2)))
        checks.check(definition("c.pack.size", 1, 2) == pack, "definition c.pack -> Character.pack", str(definition("c.pack.size", 1, 2)))

        hover = client.request("textDocument/hover", {"textDocument": {"uri": root_uri}, "position": at("mob_live(m)")})
        checks.check(hover and "bool mob_live(int m)" in hover["contents"]["value"], "hover mob_live shows its signature", str(hover))
        hover = client.request("textDocument/hover", {"textDocument": {"uri": root_uri}, "position": at("Character& c", 1, 11)})
        checks.check(hover and "Character& c" in hover["contents"]["value"], "hover c shows Character&", str(hover))
        hover = client.request("textDocument/hover", {"textDocument": {"uri": root_uri}, "position": at("Character& c")})
        checks.check(hover and "class Character" in hover["contents"]["value"] and "array<Item> pack;" in hover["contents"]["value"], "hover Character lists its fields", str(hover)[:200])

        refs = client.request("textDocument/references", {"textDocument": {"uri": root_uri}, "position": at("mob_live(m)"), "context": {"includeDeclaration": True}})
        files = {}
        for r in refs:
            files[relativize(r["uri"], GATORQUEST.parent)] = files.get(relativize(r["uri"], GATORQUEST.parent), 0) + 1
        expected = 0
        for path in GATORQUEST.glob("*.jai"):
            expected += len(re.findall(r"\bmob_live\b", path.read_text(encoding="utf-8")))
        checks.check(len(refs) == expected, "references mob_live: every call site and the declaration across files (%d)" % expected, "%d %s" % (len(refs), files))

        pos = at("string skills")
        version = client.change(root_uri, pos, pos, "c.\n\t")
        result = client.request("textDocument/completion", {"textDocument": {"uri": root_uri}, "position": {"line": pos["line"], "character": pos["character"] + 2}})
        client.change(root_uri, pos, {"line": pos["line"] + 1, "character": 1}, "")
        labels = {item["label"] for item in result["items"]}
        checks.check({"pack", "gold", "skill", "name", "fame"} <= labels, "completion c. lists Character fields", str(sorted(labels))[:300])

        # mid-edit: an unclosed call and a half-typed name, the way the text looks while typing
        version = client.change(root_uri, pos, pos, "mob_live(\n\t")
        sig = client.request("textDocument/signatureHelp", {"textDocument": {"uri": root_uri}, "position": {"line": pos["line"], "character": pos["character"] + 9}})
        checks.check(sig and sig["signatures"] and sig["signatures"][0]["label"] == "bool mob_live(int m)", "signature help inside an unclosed call", str(sig)[:300])
        client.change(root_uri, pos, {"line": pos["line"] + 1, "character": 1}, "")
        version = client.change(root_uri, pos, pos, "mob_al\n\t")
        result = client.request("textDocument/completion", {"textDocument": {"uri": root_uri}, "position": {"line": pos["line"], "character": pos["character"] + 6}})
        client.change(root_uri, pos, {"line": pos["line"] + 1, "character": 1}, "")
        labels = {item["label"] for item in result["items"]}
        checks.check("mob_alive" in labels and "mob_live" not in labels, "completion of a half-typed name offers the program's functions", str(sorted(labels))[:300])

        combat = GATORQUEST / "gq_combat.jai"
        combat_uri = client.open(combat)
        client.wait_diagnostics(combat_uri)
        rename = client.request("textDocument/rename", {"textDocument": {"uri": root_uri}, "position": at("mob_live(m)"), "newName": "mob_exists"})
        edits = sum(len(v) for v in rename.get("changes", {}).values()) if isinstance(rename, dict) and "changes" in rename else 0
        checks.check(edits == expected, "rename mob_live edits every occurrence across files", str(rename)[:300])
        client.settle()
    finally:
        client.close()


def edit_latency(client, uri, rounds=30):
    lines = client.texts[uri].split("\n")
    line = next(i for i, l in enumerate(lines) if l.startswith("\t") and l.strip().endswith(";") and "return" not in l)
    samples = []
    for i in range(rounds):
        pos = {"line": line, "character": 1}
        probe = "int lsp_probe_%d = %d;\n\t" % (i, i)
        t0 = time.perf_counter()
        version = client.change(uri, pos, pos, probe)
        client.wait_diagnostics(uri, version)
        samples.append((time.perf_counter() - t0) * 1000)
        version = client.change(uri, pos, {"line": line + 1, "character": 1}, "")
        client.wait_diagnostics(uri, version)
    samples.sort()
    return statistics.median(samples), samples[int(len(samples) * 0.95) - 1], samples[-1], line


def unit_checks(client):
    return {u["uri"]: u["checks"] for u in client.request("jaiscript/status", {})["units"]}


def rechecked(before, after):
    return {uri: after[uri] - before.get(uri, 0) for uri in after if after[uri] != before.get(uri, 0)}


def run_latency(exe, checks):
    print("latency")
    client = Client(exe, GATORQUEST.parent)
    try:
        client.open(GATORQUEST / "gatorquest.jai")
        combat_uri = client.open(GATORQUEST / "gq_combat.jai")
        actions_uri = client.open(GATORQUEST / "gq_actions.jai")
        client.wait_diagnostics(actions_uri)
        client.settle()
        for name, uri in (("gq_actions.jai", actions_uri), ("gq_combat.jai", combat_uri)):
            before = unit_checks(client)
            median, p95, worst, line = edit_latency(client, uri)
            print("  edit -> diagnostics on %s: median %.1f ms, p95 %.1f ms, max %.1f ms" % (name, median, p95, worst))
            edits = rechecked(before, unit_checks(client))
            checks.check(edits == {norm_uri(uri): 60}, "60 edits inside a function of %s check that unit once each, no other" % name,
                         str({relativize(k, GATORQUEST.parent): v for k, v in edits.items()}))
        root_uri = uri_of(GATORQUEST / "gatorquest.jai")
        root_text = client.texts[root_uri]
        t0 = time.perf_counter()
        client.request("textDocument/references", {"textDocument": {"uri": root_uri}, "position": position_of(root_text, "mob_live(m)"), "context": {"includeDeclaration": True}})
        first_refs = (time.perf_counter() - t0) * 1000
        t0 = time.perf_counter()
        client.request("textDocument/references", {"textDocument": {"uri": root_uri}, "position": position_of(root_text, "CHARS[slot]"), "context": {"includeDeclaration": True}})
        warm_refs = (time.perf_counter() - t0) * 1000
        print("  references across the program: %.0f ms cold (every unit's model built), %.1f ms warm" % (first_refs, warm_refs))
        combat_text = client.texts[combat_uri]
        t0 = time.perf_counter()
        client.request("textDocument/hover", {"textDocument": {"uri": combat_uri}, "position": position_of(combat_text, "mob_live(", 1, 0)})
        print("  hover: %.1f ms" % ((time.perf_counter() - t0) * 1000))
        pos = {"line": line, "character": 1}
        version = client.change(combat_uri, pos, pos, "CHARS[0].\n\t")
        t0 = time.perf_counter()
        items = client.request("textDocument/completion", {"textDocument": {"uri": combat_uri}, "position": {"line": line, "character": 10}})
        print("  completion after CHARS[0].: %.1f ms (%d items)" % ((time.perf_counter() - t0) * 1000, len(items["items"])))
        client.change(combat_uri, pos, {"line": line + 1, "character": 1}, "")
        t0 = time.perf_counter()
        pos = {"line": line, "character": 1}
        version = client.change(combat_uri, pos, pos, "int typo_probe = \"text\";\n\t")
        params = client.wait_diagnostics(combat_uri, version)
        error_ms = (time.perf_counter() - t0) * 1000
        checks.check(any("typo_probe" in d["message"] for d in params["diagnostics"]), "a type error typed into gq_combat.jai is reported", str(params["diagnostics"])[:300])
        print("  edit introducing a type error -> its diagnostic: %.1f ms" % error_ms)
        client.change(combat_uri, pos, {"line": line + 1, "character": 1}, "")
        client.wait_diagnostics(combat_uri, version + 1)
        client.settle()
        before = unit_checks(client)
        t0 = time.perf_counter()
        version = client.change(combat_uri, {"line": 0, "character": 0}, {"line": 0, "character": 0}, "int lsp_new_global = 1;\n")
        client.wait_diagnostics(combat_uri, version)
        client.settle()
        print("  edit changing the program surface -> whole program rechecked: %.0f ms" % ((time.perf_counter() - t0) * 1000))
        program = rechecked(before, unit_checks(client))
        checks.check(len(program) == len(before) and set(program.values()) == {1},
                     "an edit to the program surface checks every unit of the program once", "%d of %d units: %s" % (len(program), len(before), sorted(set(program.values()))))
    finally:
        client.close()


class RawServer:
    """The server over raw stdio bytes, for input no well-behaved client sends."""

    def __init__(self, exe, args=()):
        if os.name == "nt":
            import ctypes
            ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)   # a crash is an exit code here, never a dialog
        self.proc = subprocess.Popen([str(exe), "--host=bare", *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)
        self.out = bytearray()
        self.cv = threading.Condition()
        self.reading = threading.Event()
        self.reading.set()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            self.reading.wait()
            chunk = self.proc.stdout.read1(65536)
            if not chunk:
                return
            with self.cv:
                self.out += chunk
                self.cv.notify_all()

    def write(self, raw):
        try:
            self.proc.stdin.write(raw)
            self.proc.stdin.flush()
            return True
        except OSError:
            return False

    @staticmethod
    def frame(message):
        body = message if isinstance(message, bytes) else json.dumps(message).encode("utf-8")
        return b"Content-Length: %d\r\n\r\n" % len(body) + body

    def send(self, message):
        return self.write(self.frame(message))

    def hold_replies(self):
        """stop reading the server's output: a reply larger than the pipe then holds its one handler thread"""
        self.reading.clear()

    def take_replies(self):
        self.reading.set()

    def barrier(self):
        """once this is written the server has read every message before it: the padding outgrows any pipe buffer"""
        return self.send({"jsonrpc": "2.0", "method": "$/padding", "params": {"padding": "x" * (1 << 20)}})

    def answered(self, rid, timeout=HANG_GUARD):
        needle = b'"id":%d,' % rid
        deadline = time.monotonic() + timeout
        with self.cv:
            while needle not in self.out:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.proc.poll() is not None:
                    return needle in self.out
                self.cv.wait(min(remaining, 0.2))
            return True

    def response(self, rid, timeout=HANG_GUARD):
        if not self.answered(rid, timeout):
            return None
        deadline = time.monotonic() + timeout
        with self.cv:
            while True:
                data = bytes(self.out)
                start = data.rindex(b"Content-Length:", 0, data.index(b'"id":%d,' % rid))
                length = int(data[start:data.index(b"\r\n", start)].split(b":")[1])
                body_start = data.index(b"\r\n\r\n", start) + 4
                if len(data) >= body_start + length:
                    return json.loads(data[body_start:body_start + length].decode("utf-8"))
                if time.monotonic() > deadline:
                    return None
                self.cv.wait(0.2)

    def initialize(self):
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        return self.answered(1)

    def reply_with(self, needle, timeout=HANG_GUARD):
        """the first message whose body holds needle, decoded"""
        deadline = time.monotonic() + timeout
        with self.cv:
            while True:
                data = bytes(self.out)
                at = data.find(needle)
                if at >= 0:
                    start = data.rindex(b"Content-Length:", 0, at)
                    length = int(data[start:data.index(b"\r\n", start)].split(b":")[1])
                    body_start = data.index(b"\r\n\r\n", start) + 4
                    if len(data) >= body_start + length:
                        return json.loads(data[body_start:body_start + length].decode("utf-8"))
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.proc.poll() is not None:
                    return None
                self.cv.wait(min(remaining, 0.2))

    def ping(self, rid, timeout=HANG_GUARD):
        self.send({"jsonrpc": "2.0", "id": rid, "method": "jaiscript/status", "params": {}})
        return self.answered(rid, timeout)

    def private_mb(self):
        if os.name == "nt":
            out = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-Process -Id %d).PrivateMemorySize64" % self.proc.pid],
                                 capture_output=True, text=True, timeout=HANG_GUARD).stdout.strip()
            return int(out) / 1e6 if out.isdigit() else None
        try:
            import psutil
            return psutil.Process(self.proc.pid).memory_info().rss / 1e6
        except ImportError:
            return None

    def end_input(self, timeout=HANG_GUARD):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def run_robustness(exe, checks):
    print("robustness")
    clean_exits = (0, 1)

    def framing_case(name, header):
        server = RawServer(exe)
        try:
            server.initialize()
            server.write(header + b"{}")
            checks.check(server.ping(2), name + ": the next request is answered", "no answer, exit %s" % server.proc.poll())
        finally:
            server.kill()

    framing_case("a non-numeric Content-Length", b"Content-Length: abc\r\n\r\n")
    framing_case("a negative Content-Length", b"Content-Length: -5\r\n\r\n")
    framing_case("a Content-Length past size_t", b"Content-Length: 99999999999999999999999999\r\n\r\n")
    framing_case("a header line with no colon", b"garbage\r\n\r\n")

    server = RawServer(exe)
    try:
        server.initialize()
        server.write(b"Content-Length: 1099511627776\r\n\r\n")
        server.write(b"x" * (1 << 20))   # written once the server has read the header and is reading past it
        checks.check(server.proc.poll() is None, "a Content-Length of 2^40: the server stays up")
        ws = server.private_mb()
        checks.check(ws is None or ws < 200, "a Content-Length of 2^40: no body is allocated", "%s MB" % ws)
        code = server.end_input()
        checks.check(code in clean_exits, "a Content-Length of 2^40: end of input exits cleanly", "exit %s" % code)
    finally:
        server.kill()

    server = RawServer(exe, ["--max-message=65536"])
    try:
        server.initialize()
        server.send({"jsonrpc": "2.0", "id": 50, "method": "jaiscript/status", "params": {"padding": "x" * 70000}})
        checks.check(server.ping(51), "a body over --max-message: the next request is answered")
        checks.check(not server.answered(50, 0), "a body over --max-message is skipped, not answered")   # replies leave in order
    finally:
        server.kill()

    server = RawServer(exe)
    try:
        server.initialize()
        for _ in range(25):
            server.write(b"A" * 2000000)   # each write returns once the server has read all but a pipe buffer of it
        ws = server.private_mb()
        checks.check(ws is None or ws < 40, "a 50 MB header line is not held", "%s MB" % ws)
        server.write(b"\r\n\r\n")
        checks.check(server.ping(2), "a 50 MB header line: the next request is answered")
    finally:
        server.kill()

    server = RawServer(exe)
    try:
        server.initialize()
        for raw in [b"{not json", b"[1,2]", b"42", b"null", b'"text"', b"[" * 100000, b'{"a":' * 100000]:
            server.write(server.frame(raw))
        rid = 100
        for method in ["textDocument/hover", "textDocument/didOpen", "textDocument/didChange", "textDocument/completion",
                       "textDocument/rename", "workspace/didChangeWatchedFiles", "x/unknown"]:
            for params in ["text", 5, None, [], {"textDocument": "x"}, {"textDocument": {"uri": 5}},
                           {"textDocument": {"uri": "file:///none.jai"}, "position": {"line": -1, "character": 2 ** 62}},
                           {"changes": [5, None, {"uri": 7}]}, {"contentChanges": [{"range": 5, "text": 1}]}]:
                for rid_value in (rid, None, "s", {"o": 1}):
                    server.send({"jsonrpc": "2.0", "id": rid_value, "method": method, "params": params})
                rid += 1
        checks.check(server.ping(2), "after malformed and wrong-typed messages the next request is answered")
    finally:
        server.kill()

    server = RawServer(exe)
    try:
        server.initialize()
        for rid in range(1000, 3000):
            server.send({"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": rid}})
        status = server.response(2) if server.ping(2) else None
        checks.check(status is not None and status.get("result", {}).get("cancelled") == 0,
                     "a cancel for a request not queued keeps nothing", str(status)[:200])
    finally:
        server.kill()

    server = RawServer(exe)
    try:
        server.initialize()
        uri = uri_of(FIXTURES / "flood_unsaved.jai")
        server.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "jaiscript", "version": 1, "text": "int x = 1;\n" + "int z = 2;\n" * 2000}}})
        edits = 10000
        burst = bytearray()
        for version in range(2, edits + 2):
            burst += server.frame({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"range": {"start": {"line": 0, "character": 4}, "end": {"line": 0, "character": 4}}, "text": "y"}]}})
        status = server.response(3) if server.ping(3) else None
        parses = (status or {}).get("result", {}).get("parses", 0)
        # the burst queues whole behind a request whose reply is held, so how it is read never decides how often it parses
        server.hold_replies()
        server.send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}})
        server.write(bytes(burst))
        server.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}})
        server.barrier()
        server.take_replies()
        symbols = server.response(2)
        status = server.response(5) if server.ping(5) else None
        parsed = (status or {}).get("result", {}).get("parses", 0) - parses
        checks.check(parsed == 1, "a queued burst of %d edits is parsed once" % edits, "%d parses" % parsed)
        names = [s["name"] for s in (symbols or {}).get("result") or []]
        checks.check(bool(names) and names[0] == "y" * edits + "x", "a burst of edits applies every edit in order", str(names[:1])[:80])
    finally:
        server.kill()

    for args in (["--max-message=1000"], ["--max-message=lots"], ["--host=nope"]):
        proc = subprocess.run([str(exe), *args], stdin=subprocess.DEVNULL, capture_output=True, timeout=HANG_GUARD)
        checks.check(proc.returncode == 2, "a bad %s is a usage error" % args[0], "exit %s" % proc.returncode)

    def error_code(reply):
        return (reply or {}).get("error", {}).get("code")

    server = RawServer(exe)
    try:
        server.send({"jsonrpc": "2.0", "id": 3, "method": "shutdown"})
        checks.check(error_code(server.response(3)) == -32002, "shutdown before initialize is -32002")
        server.initialize()
        server.write(server.frame(b'{"jsonrpc":"2.0","id":4,"method":"jaiscript/status","params":{"n":0099}}'))
        reply = server.reply_with(b'"code":-32700')
        checks.check(reply is not None and reply.get("id") is None, "a number with a leading zero is a parse error (-32700, id null)", str(reply)[:200])
        server.write(server.frame(b'{"jsonrpc":"2.0","id":5,"method":"jaiscript/status","params":{"s":"\\ud800\\u0041"}}'))
        for message in ({"jsonrpc": "2.0", "id": 1.5, "method": "jaiscript/status"}, {"jsonrpc": "2.0", "id": {"o": 1}, "method": "jaiscript/status"},
                        {"jsonrpc": "2.0", "id": 6, "method": ""}, {"jsonrpc": "2.0", "params": {}}):
            server.send(message)
        checks.check(server.ping(7), "after invalid requests the next request is answered")
        checks.check(not server.answered(5, 0), "an unpaired surrogate escape is a parse error, not a request")   # replies leave in order
        invalid = bytes(server.out).count(b'"code":-32600')
        checks.check(invalid == 4, "a fractional or object id, an empty method and a message with neither id nor method are -32600", "%d replies" % invalid)
        server.send({"jsonrpc": "2.0", "id": 8, "method": "initialize", "params": {"capabilities": {}}})
        checks.check(error_code(server.response(8)) == -32600, "a second initialize is -32600")

        uri = uri_of(FIXTURES / "cancel_unsaved.jai")
        server.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "jaiscript", "version": 1, "text": "int z = 2;\n" * 40000}}})
        burst = bytearray()
        for message in ({"jsonrpc": "2.0", "id": 9, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}},
                        {"jsonrpc": "2.0", "id": "abc", "method": "jaiscript/status"},
                        {"jsonrpc": "2.0", "id": 10, "method": "jaiscript/status"},
                        {"jsonrpc": "2.0", "id": 77, "result": None},
                        {"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": -2}},
                        {"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {}},
                        {"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 77}},
                        {"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": "abc"}},
                        {"jsonrpc": "2.0", "id": 77, "method": "jaiscript/status"}):
            burst += server.frame(message)
        # request 9's reply is held, so every message behind it is still queued when its cancel is read
        server.hold_replies()
        server.write(bytes(burst))
        server.barrier()
        server.take_replies()
        checks.check(error_code(server.reply_with(b'"id":"abc"')) == -32800, "a cancel by string id cancels that queued request")
        checks.check("result" in (server.response(10) or {}), "a cancel of another id, or of none, leaves a queued request alone")
        checks.check("result" in (server.response(77) or {}), "a stray response's id is never marked cancelled")

        server.send({"jsonrpc": "2.0", "id": 11, "method": "shutdown"})
        server.answered(11)
        server.send({"jsonrpc": "2.0", "id": 12, "method": "textDocument/hover", "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 4}}})
        checks.check(error_code(server.response(12)) == -32600, "a request after shutdown is -32600")
        server.send({"jsonrpc": "2.0", "method": "exit"})
        checks.check(server.end_input() == 0, "exit after shutdown exits 0")
    finally:
        server.kill()

    server = RawServer(exe)
    try:
        server.initialize()
        server.write(b'Content-Length: 100\r\n\r\n{"jsonrpc":"2.0","id":9,')
        code = server.end_input()
        checks.check(code in clean_exits, "input cut off mid-body exits cleanly", "exit %s" % code)
    finally:
        server.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe")
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--only", choices=["fixtures", "gatorquest", "latency", "robustness"])
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    exe = Path(args.exe) if args.exe else default_exe()
    if not exe or not exe.exists():
        print("jaiscript_lsp not found; build the jaiscript_lsp target or pass --exe")
        return 2
    print("server: " + str(exe))
    checks = Checks(args.verbose)
    if args.only in (None, "fixtures"):
        run_fixtures(exe, args.update, checks)
        run_keyword_members(exe, checks)
    if args.only in (None, "gatorquest"):
        run_gatorquest(exe, checks)
    if args.only in (None, "latency"):
        run_latency(exe, checks)
    if args.only in (None, "robustness"):
        run_robustness(exe, checks)
    print("%d passed, %d failed" % (checks.passed, len(checks.failures)))
    return 1 if checks.failures else 0


if __name__ == "__main__":
    sys.exit(main())
