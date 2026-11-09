#!/usr/bin/env python3
"""Rewrite the shared chrome of every docs page.

The chrome is the part of a page nobody writes by hand: <head> metadata, the header,
the sidebar, the prev/next footer nav, and the site footer. Everything between
<main> and </main> is copied through byte-for-byte, except the one <nav class="prevnext">
block, which is chrome that happens to live inside the article.

Run it from anywhere:  python site_tools/apply_chrome.py [--check]
--check reports which files would change and exits 1 if any would, touching nothing.
"""

import argparse
import pathlib
import re
import sys

SITE = pathlib.Path(__file__).resolve().parent.parent / "site"

VERSION = "0.5 beta"
REPO = "https://github.com/Devacor/jaiscript"

FONTS = (
    '<link rel="preconnect" href="https://fonts.googleapis.com">\n'
    '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>\n'
    '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Atkinson+Hyperlegible:ital,wght@0,400;0,700;1,400'
    '&amp;family=Chakra+Petch:wght@500;600;700&amp;family=JetBrains+Mono:wght@400;600&amp;display=swap">'
)

THEME_BOOTSTRAP = (
    '<script>(function(){var t=null;try{t=localStorage.getItem("jai-theme")}catch(e){}\n'
    'if(!t)t=window.matchMedia&&window.matchMedia("(prefers-color-scheme: light)").matches?"light":"dark";\n'
    'document.documentElement.setAttribute("data-theme",t);}())</script>'
)

# ---------------------------------------------------------------- page tables

GUIDE = [
    ("01-hello.html", "Hello &amp; Basics",
     "Run your first script from C++, meet the values and literals, and learn print and format."),
    ("02-type-ladder.html", "The Type Ladder: typed, auto, var",
     "Three rungs of typing: int enforced, auto inferred then locked, var dynamic - plus the conversion rules typed targets apply."),
    ("03-value-semantics.html", "Value Semantics &amp; References",
     "JaiScript copies by default, everywhere. Here are the escape hatches - reference parameters, reference declarations, shared_ptr - and what each costs."),
    ("04-operators.html", "Operators &amp; Arithmetic",
     "The full C++ operator set with checked overflow, no undefined behavior, char promotion, and operator overloading on script classes."),
    ("05-control-flow.html", "Control Flow",
     "if, while, both for forms, a switch that breaks by default, and try/catch/throw that unwinds loops correctly."),
    ("06-functions.html", "Functions &amp; Lambdas",
     "Functions, lambdas with explicit captures, default arguments, variadics, and by-reference parameters."),
    ("07-classes.html", "Classes",
     "Fields in declaration order, constructor overloading and delegation, inheritance with super, statics, and hot reload with instance migration."),
    ("08-coroutines.html", "Coroutines",
     "Coroutines and yield: handles you advance with next(), drivable by range-for, and the shapes that make multi-frame behavior easy to write."),
    ("09-containers-strings.html", "Containers &amp; Strings",
     "Arrays and maps that follow their declared type, the string methods that copy and the ones that mutate, and how to tell them apart."),
    ("10-modules.html", "Modules: include vs import",
     "include reruns a file; import caches it. Path resolution, host-controlled filesystem access, and what the checker sees across units."),
    ("11-kitchen-sink.html", "Enums, Namespaces, Destructuring &amp; Friends",
     "Enums, namespaces, destructuring, and the smaller conveniences that did not need a chapter of their own."),
    ("12-reflection.html", "Reflection: reflect::",
     "The reflect:: surface: ask a value what it is, walk fields and methods by name, and build tools on top of live objects."),
    ("13-strict-checking.html", "Types That Fail Early: strict checking",
     "What fails at load and what waits for run time, how to read a diagnostic, cast&lt;T&gt; as the explicit boundary, extern, and the [[warn_mode]] / [[strict_mode]] attributes."),
    ("14-time.html", "Time: Clocks, Stopwatches &amp; Dates",
     "The time:: namespace: monotonic and wall clocks, execute_elapsed, value-type stopwatches, validated calendar dates in UTC or local time, templates with format specs, and why parallel bodies cannot read the clock."),
    ("15-math.html", "Math: Vectors &amp; Colors",
     "The math value types: vec2, vec3, vec4, ivec2, ivec3 and color, their operators and value semantics, the component-wise and geometry libraries, perceptual color mixing, and packed vector arrays."),
    ("16-parallel.html", "Parallel Loops",
     "parallel_for, parallel_transform and parallel_reduce: what a parallel body may and may not do, the zip form, captured reads, index writes, constructing objects, C++ calls, and reductions that are bit-identical at every worker count."),
    ("17-web.html", "The web:: Library",
     "The opt-in web:: library: HTML escaping and unescaping, RFC 3986 and form percent-encoding, query strings parsed and built, and URLs split into a web::url and joined back."),
    ("18-random.html", "Random Numbers &amp; Crypto",
     "The random type: a generator you own, seeded streams that replay bit for bit on every platform, copies and save files for rollback, draws in parallel loops; and crypto:: for secrets: secure random bytes, SHA-2, HMAC and PHP-compatible bcrypt passwords."),
    ("19-templates.html", "HTML Templates",
     "template(path, names...) renders an HTML file with escaped ${...} splices, seeing the names it is given by reference and the program, checked at load and reloaded on edit; scoped_template renders untrusted templates over a copy of a map."),
    ("20-terminal-games.html", "A Terminal Game in One File",
     "A real-time game run by the standalone runner: read_key and key_down polling, frame pacing with a stopwatch and sleep, frames drawn with ANSI escape codes, the terminal's size, sound, and saves."),
]

EMBEDDING = [
    ("01-embedding-quickstart.html", "Embedding Quickstart",
     "From nothing to a scripted, hot-reloading game loop in six steps: add JaiScript to the build, run a script, bind a class, call a script function, reload an edit while the loop runs, and handle errors."),
    ("02-dynamic-binder.html", "dynamic_binder: Binding C++ Classes",
     "One declaration makes a field reflected, script-visible and serialized - then the fluent binder chain, verb by verb."),
    ("03-backends.html", "Backends: Interpreter vs VM",
     "One parser, one AST, two interchangeable execution backends. What each is good at, and how to choose per engine."),
    ("04-jaibite.html", "jaibite: Parse Once, Ship Precompiled",
     "Parse once, ship precompiled: engine-bound script handles that skip straight to execution, and what the version stamp guarantees."),
    ("05-safety-rails.html", "Safety Rails",
     "Host-controlled filesystem and custom permissions, bounded script execution, and engine isolation limits."),
    ("06-static-checking.html", "Static Checking",
     "Compiler-style diagnostics before anything runs - strict by default, warn or off when you ask - and how var opts a value out."),
    ("07-hot-reload.html", "Hot Reload",
     "Redefining a class migrates every live instance in place. What survives a reload, what resets, and how to drive it from the host."),
    ("08-serialization.html", "Serialization",
     "Script values to JSON and back, plus a C++ archive framework that writes JSON or binary through one interface."),
    ("09-coroutine-hosting.html", "Coroutine Hosting",
     "Driving script coroutines from C++: next, completion, and the shapes that fit per-entity behavior and cutscenes."),
    ("10-signals-properties.html", "Signals &amp; Observable Properties",
     "Firing and receiving signals across the boundary, the three routes a signal takes to become script-connectable, and values that announce their own changes."),
    ("11-performance.html", "Performance Notes",
     "What the VM does for you, what idiomatic-fast code looks like, the honest cost model, and real measured numbers."),
    ("12-debugging.html", "Debugging",
     "Breakpoints, stepping, and live variable reads and writes on both backends, plus a VS Code attach that costs nothing until it is used."),
    ("13-script-extensible.html", "Script-Extensible Objects: extend",
     "A script class can derive a bound C++ class, and the script decides which object becomes which class."),
    ("14-language-server.html", "Editor Support: the Language Server",
     "jaiscript_lsp answers the editor from the engine's own parser and strict checker: diagnostics as you type across a whole program, definitions, references, hover, completion, rename."),
    ("15-math-adapters.html", "Math Adapters: Host Vector Types",
     "One macro line makes a host vector, point or color struct cross every binding as math::vec3, math::ivec2 or math::color: parameters, returns, write-back, bound members, reflected properties and std::vector buffers."),
    ("16-thread-pool.html", "Thread Pool for Host Work",
     "jai::thread_pool for the host's own work: shared and pinned queues, continuations on a worker or posted to the host's thread, the run_posted drain, exceptions, spinning, and what destruction drops."),
    ("17-host-api.html", "Host API in Depth",
     "The rest of the engine's host API: functions and direct entries, live bindings to C++ variables and what writes through them, running files, per-execution variables, calling script and reading its state back, every error surface, and the time source."),
    ("18-build-integration.html", "Building JaiScript into a Host",
     "Every way into a project in full: CMake, Visual Studio and MSBuild, Xcode and an installed package, with the sources, flags and defines each needs, one file or per file, and Debug and Release notes."),
    ("19-jai-spider.html", "jai_spider: a Web Application Host",
     "jai_spider serves a JaiScript program over HTTP/1.1: the handler the app hands to spider::spin, Request and a Router in script, spider::Response shared with the spider:: client, SQLite bound as a class, cookie sessions, static files, and modules that reload on the request after they are saved."),
]

LANGUAGE_BRIDGES = [
    ("01-from-javascript.html", "From JavaScript",
     "JavaScript beside JaiScript, idiom by idiom: what carries over, the habits that get a different answer (copies, ==, integer division, closures, map keys, array helpers), and a complete program."),
    ("02-from-cpp.html", "From C++",
     "C++ beside JaiScript, idiom by idiom: what carries over, what is absent (pointers, templates, const), what is guaranteed (evaluation order, checked overflow, no fallthrough), and a complete program."),
    ("03-from-csharp.html", "From C#",
     "C# beside JaiScript, idiom by idiom: classes as value types, var and auto, properties as accessors, loops for LINQ, coroutines for async, and a complete program."),
]

CODE_MIGRATIONS = [
    ("04-from-cereal.html", "From Cereal",
     "Moving a C++ codebase from Cereal or Boost.Serialization to the JaiScript archives: every construct mapped, a worked conversion, the property route that removes serialize functions, both libraries side by side, and converting existing files once."),
    ("05-from-boost-signals2.html", "From Boost.Signals2",
     "Moving C++ signals from Boost.Signals2 to jai::signal: every Boost type and call mapped, a class converted, and the behavioral differences that bite (the private emitter, connection lifetime, emits during emits, threads, combiners), with notes for Qt and sigslot code."),
    ("06-from-chaiscript.html", "From ChaiScript Bindings",
     "Moving a ChaiScript host to JaiScript: functions, classes, constructors, members, overloads, bases, globals, modules, calls into script and errors, from a real host's binding code, and the script habits that get a different answer."),
    ("07-from-sol2.html", "From sol2 (Lua) Bindings",
     "Moving a Lua host built on sol2 to JaiScript: usertypes, constructors, properties, overloads, bases, globals, protected calls and errors, from a real host's binding code, and the Lua habits that get a different answer (copies, 0-based indexing, division, truthiness)."),
    ("08-from-squirrel.html", "From Squirrel Bindings",
     "Moving a Squirrel host from the stack-based C API to JaiScript: natives, classes with type tags, globals, calls into script and errors, from a real host's binding code, and the Squirrel habits that get a different answer (shared instances, the empty string, generators)."),
    ("09-from-pybind11.html", "From pybind11 and Boost.Python",
     "Moving class_ bindings from pybind11 or Boost.Python to dynamic_binder: constructors, methods, fields, properties, overloads, operators, holders, exceptions and calls into script, with what has no counterpart (keyword arguments, trampolines, the GIL)."),
    ("10-from-nlohmann-json.html", "From nlohmann::json",
     "Moving JSON code from nlohmann::json to JaiScript: types through the archives' serialize functions, the archive record format, plain JSON documents in script with to_json and from_json, and reaching them from the host."),
    ("11-from-qt-properties.html", "From Qt Properties, RTTR and refl-cpp",
     "Moving C++ reflection from Q_PROPERTY, RTTR or refl-cpp to JAI_PROPERTY: declaring, enumerating, reading and writing by name, change notification through observable properties, script access, and what has no counterpart (metadata, compile-time iteration, dynamic properties)."),
    ("12-from-coroutines.html", "From Lua Coroutines and C++20 Schedulers",
     "Moving per-entity behaviour from Lua coroutines or a C++20 coroutine scheduler to JaiScript coroutines: yield, next, next(n) and done, a patrol converted three ways, hosting handles from C++, budgets and hot reload."),
]

BRIDGES = LANGUAGE_BRIDGES + CODE_MIGRATIONS

SECTION_INDEX_DESC = {
    "tour/index.html":
        "Ten minutes, one small game: value semantics, the strict checker, a swarm, coroutines, hot reload and parallel_for, every line run on both backends.",
    "bridges/index.html":
        "Language bridges for programmers arriving from JavaScript, C++ or C#, and code migrations for C++ code moving to JaiScript from Cereal, Boost.Signals2, ChaiScript, sol2, Squirrel, pybind11, nlohmann::json, Qt properties or C++20 coroutines.",
    "guide/index.html":
        "Twenty chapters covering the whole core language, the opt-in web:: library, random numbers and crypto, HTML templates and a terminal game - every standalone sample runs on both backends, with the output it prints.",
    "embedding/index.html":
        "Nineteen chapters on hosting JaiScript in a C++ engine: binding classes, choosing a backend, safety rails, hot reload, shipping, editor support, host math types, a thread pool for host work, and a web application host.",
    "examples/index.html":
        "Complete, playable programs written in JaiScript - a raycast shooter, a dungeon crawler, a demoscene reel, an online RPG and a DOOM port - and what each one proves.",
    "reference/index.html":
        "Quick-lookup tables distilled from the guide and embedding chapters: operators, types, conversions, the host API, and rules of thumb.",
    "index.html":
        "An embedded scripting language for C++ engines: C++-flavored syntax, script classes with hot reload, coroutines, and gradual typing.",
    "stability.html":
        "What is settled in the language and the embedding API, and what a 0.x release may still change before 1.0.",
}

SECTION_TITLE = {"tour": "The Ten-Minute Tour", "bridges": "Bridges",
                 "guide": "The Language Guide", "embedding": "Embedding &amp; Engine Guide",
                 "examples": "The Example Games", "reference": "Reference"}

# ---------------------------------------------------------------- chrome parts


def attr(text):
    return text.replace('"', "&quot;")


def head(title, desc, root):
    return "\n".join([
        "<head>",
        '<meta charset="utf-8">',
        '<meta name="viewport" content="width=device-width, initial-scale=1">',
        "<title>%s</title>" % title,
        '<meta name="description" content="%s">' % attr(desc),
        '<meta name="theme-color" content="#0b0d13" media="(prefers-color-scheme: dark)">',
        '<meta name="theme-color" content="#f6f5f1" media="(prefers-color-scheme: light)">',
        '<meta property="og:type" content="website">',
        '<meta property="og:site_name" content="JaiScript docs">',
        '<meta property="og:title" content="%s">' % attr(title),
        '<meta property="og:description" content="%s">' % attr(desc),
        '<meta name="twitter:card" content="summary">',
        '<link rel="icon" href="%sassets/favicon.svg" type="image/svg+xml">' % root,
        FONTS,
        '<link rel="stylesheet" href="%sassets/style.css">' % root,
        THEME_BOOTSTRAP,
        "</head>",
    ])


def header(section, root):
    def nav(key, label):
        mark = ' class="active" aria-current="page"' if key == section else ""
        return '<a href="%s%s/index.html"%s>%s</a>' % (root, key, mark, label)

    return "\n".join([
        '<a class="skip" href="#main">Skip to content</a>',
        '<header class="site-header">',
        '<a class="brand" href="%sindex.html"><span class="brand-mark" aria-hidden="true">{*}</span>JaiScript'
        '<span class="brand-sub">docs</span></a>' % root,
        '<nav class="site-nav" aria-label="Sections">'
        + nav("tour", "Tour") + nav("bridges", "Bridges") + nav("guide", "Guide") + nav("embedding", "Embedding")
        + nav("examples", "Examples") + nav("reference", "Reference") + "</nav>",
        '<div class="header-tools">',
        '<div class="search" role="search"><input id="search-box" type="search" '
        'placeholder="Search docs&hellip;" autocomplete="off" aria-label="Search docs" '
        'aria-controls="search-results" aria-expanded="false" aria-autocomplete="list">'
        '<div id="search-results" role="listbox" aria-label="Search results" hidden></div></div>',
        '<button id="theme-toggle" type="button" aria-label="Switch to light theme" '
        'title="Switch to light theme">&#9681;</button>',
        "</div>",
        "</header>",
    ])


def chapter_items(section, chapters, current, root):
    items = []
    for name, title, _ in chapters:
        href = "%s%s/%s" % (root, section, name)
        if name == current:
            items.append('<li class="current"><a href="%s" aria-current="page">%s</a>'
                         '<ol class="page-toc" data-page-toc></ol></li>' % (href, title))
        else:
            items.append("<li><a href=\"%s\">%s</a></li>" % (href, title))
    return "".join(items)


def chapter_box(label, items, first_number=1):
    counter = "" if first_number == 1 else ' style="counter-reset: ch %d"' % (first_number - 1)
    return ('<details class="sidebar-box" open><summary class="sidebar-title">%s</summary>'
            '<ol class="chapter-list"%s>%s</ol></details>' % (label, counter, items))


def chapter_sidebar(section, chapters, current, root):
    if section == "bridges":
        boxes = (chapter_box("Language Bridges", chapter_items(section, LANGUAGE_BRIDGES, current, root))
                 + chapter_box("Code Migrations", chapter_items(section, CODE_MIGRATIONS, current, root),
                               len(LANGUAGE_BRIDGES) + 1))
    else:
        label = {"guide": "Guide", "embedding": "Embedding"}[section]
        boxes = chapter_box(label, chapter_items(section, chapters, current, root))
    return '<aside class="sidebar" aria-label="Chapters">%s</aside>' % boxes


def page_sidebar(with_h3):
    flag = " data-toc-h3" if with_h3 else ""
    return ('<aside class="sidebar" aria-label="On this page">'
            '<details class="sidebar-box" open><summary class="sidebar-title">On this page</summary>'
            '<ol class="page-toc" data-page-toc%s></ol></details></aside>' % flag)


def footer(root):
    return "\n".join([
        '<footer class="site-footer">',
        '<p>JaiScript %s &middot; <a href="%s/blob/master/LICENSE">MIT license</a> &middot; '
        '<a href="%s">source</a> &middot; <a href="%s/issues">report a docs bug</a></p>'
        % (VERSION, REPO, REPO, REPO),
        '<p class="foot-note">every standalone script sample on this site runs on both backends, and the output shown is what they print</p>',
        "</footer>",
        '<script src="%sassets/search-index.js"></script>' % root,
        '<script src="%sassets/site.js"></script>' % root,
        "</body>",
        "</html>",
    ])


SECTION_CRUMB = {"tour": "Tour", "bridges": "Bridges", "guide": "Guide", "embedding": "Embedding",
                 "examples": "Examples", "reference": "Reference"}


def crumbs(rel, section, root):
    """Docs / Section / NN, stamped right after <main> on every page but the landing page."""
    if rel == "index.html":
        return ""
    parts = ['<a href="%sindex.html">Docs</a>' % root]
    name = rel.split("/")[-1]
    if not section:
        parts.append(name[:-len(".html")].capitalize())
    elif name == "index.html":
        parts.append(SECTION_CRUMB[section])
    else:
        parts.append('<a href="index.html">%s</a>' % SECTION_CRUMB[section])
        number = re.match(r"(\d+)-", name)
        parts.append(number.group(1) if number else "API" if name == "api.html" else name)
    return ('<nav class="crumbs" aria-label="Breadcrumb">%s</nav>'
            % '<span class="sep" aria-hidden="true">/</span>'.join(parts))


def prevnext(prev, nxt):
    """prev/nxt are (href, label, title) or None."""
    if not prev and not nxt:
        return ""
    parts = ['<nav class="prevnext" aria-label="Chapter navigation">']
    if prev:
        parts.append('<a class="prev" rel="prev" href="%s"><span>&larr; %s</span>%s</a>'
                     % (prev[0], prev[1], prev[2]))
    if nxt:
        parts.append('<a class="next" rel="next" href="%s"><span>%s &rarr;</span>%s</a>'
                     % (nxt[0], nxt[1], nxt[2]))
    parts.append("</nav>")
    return "".join(parts)


# ---------------------------------------------------------------- the chain

def build_pages():
    """Every page: (relative path, section, root prefix, description, sidebar, prevnext)."""
    pages = []

    pages.append(("index.html", None, "", SECTION_INDEX_DESC["index.html"], "", ""))
    pages.append(("stability.html", None, "", SECTION_INDEX_DESC["stability.html"], page_sidebar(True), ""))

    pages.append(("tour/index.html", "tour", "../", SECTION_INDEX_DESC["tour/index.html"], page_sidebar(True),
                  prevnext(None, ("../guide/01-hello.html", "next &middot; the guide", GUIDE[0][1]))))

    bridges_index = ("../bridges/index.html", "bridges", SECTION_TITLE["bridges"])
    pages.append(("bridges/index.html", "bridges", "../", SECTION_INDEX_DESC["bridges/index.html"],
                  chapter_sidebar("bridges", BRIDGES, None, "../"), ""))
    for name, title, desc in LANGUAGE_BRIDGES:
        pages.append(("bridges/" + name, "bridges", "../", desc,
                      chapter_sidebar("bridges", BRIDGES, name, "../"),
                      prevnext(bridges_index, ("../guide/01-hello.html", "next &middot; the guide", GUIDE[0][1]))))
    for name, title, desc in CODE_MIGRATIONS:
        pages.append(("bridges/" + name, "bridges", "../", desc,
                      chapter_sidebar("bridges", BRIDGES, name, "../"),
                      prevnext(bridges_index, ("../embedding/index.html", "next &middot; embedding",
                                               SECTION_TITLE["embedding"]))))

    guide_index = ("../guide/index.html", "guide", SECTION_TITLE["guide"])
    embed_index = ("../embedding/index.html", "embedding", SECTION_TITLE["embedding"])

    pages.append(("guide/index.html", "guide", "../", SECTION_INDEX_DESC["guide/index.html"],
                  chapter_sidebar("guide", GUIDE, None, "../"), ""))
    for i, (name, title, desc) in enumerate(GUIDE):
        prev = guide_index if i == 0 else \
            ("../guide/" + GUIDE[i - 1][0], "previous", GUIDE[i - 1][1])
        nxt = ("../embedding/" + EMBEDDING[0][0], "next &middot; embedding", EMBEDDING[0][1]) \
            if i == len(GUIDE) - 1 else ("../guide/" + GUIDE[i + 1][0], "next", GUIDE[i + 1][1])
        pages.append(("guide/" + name, "guide", "../", desc,
                      chapter_sidebar("guide", GUIDE, name, "../"), prevnext(prev, nxt)))

    pages.append(("embedding/index.html", "embedding", "../",
                  SECTION_INDEX_DESC["embedding/index.html"],
                  chapter_sidebar("embedding", EMBEDDING, None, "../"), ""))
    for i, (name, title, desc) in enumerate(EMBEDDING):
        prev = embed_index if i == 0 else \
            ("../embedding/" + EMBEDDING[i - 1][0], "previous", EMBEDDING[i - 1][1])
        nxt = ("../examples/index.html", "next &middot; examples", SECTION_TITLE["examples"]) \
            if i == len(EMBEDDING) - 1 else \
            ("../embedding/" + EMBEDDING[i + 1][0], "next", EMBEDDING[i + 1][1])
        pages.append(("embedding/" + name, "embedding", "../", desc,
                      chapter_sidebar("embedding", EMBEDDING, name, "../"), prevnext(prev, nxt)))

    pages.append(("examples/index.html", "examples", "../",
                  SECTION_INDEX_DESC["examples/index.html"], page_sidebar(False),
                  prevnext(("../embedding/" + EMBEDDING[-1][0], "previous &middot; embedding",
                            EMBEDDING[-1][1]),
                           ("../reference/index.html", "next &middot; reference",
                            SECTION_TITLE["reference"]))))

    pages.append(("reference/index.html", "reference", "../",
                  SECTION_INDEX_DESC["reference/index.html"], page_sidebar(True),
                  prevnext(("../examples/index.html", "previous &middot; examples",
                            SECTION_TITLE["examples"]), None)))
    pages.append(("reference/api.html", "reference", "../",
                  "Full callable signatures, defaults, return types, effects and availability for builtins, the standard library and standalone runner.",
                  page_sidebar(False), ""))
    return pages


# ---------------------------------------------------------------- rewriting

TITLE_RE = re.compile(r"<title>(.*?)</title>", re.S)
MAIN_OPEN_RE = re.compile(r"<main class=\"content\"[^>]*>")
PREVNEXT_RE = re.compile(r'<nav class="prevnext".*?</nav>', re.S)
CRUMBS_RE = re.compile(r'^<nav class="crumbs"[^>]*>.*?</nav>', re.S)


def read_raw(path):
    """Read without newline translation so the file's own line endings survive."""
    with path.open("r", encoding="utf-8", newline="") as handle:
        return handle.read()


def rewrite(src, rel, section, root, desc, sidebar, nav):
    title = TITLE_RE.search(src).group(1)
    prologue = src[:src.index("<head>")]

    open_tag = MAIN_OPEN_RE.search(src)
    body = CRUMBS_RE.sub("", src[open_tag.end():src.index("</main>")], count=1)

    if nav:
        if PREVNEXT_RE.search(body):
            body = PREVNEXT_RE.sub(lambda m: nav, body, count=1)
        else:
            cut = body.rindex("</article>")
            body = body[:cut] + nav + body[cut:]
    else:
        body = PREVNEXT_RE.sub("", body)

    out = (prologue + head(title, desc, root) + "\n<body>\n" + header(section, root)
           + '\n<div class="layout">\n' + sidebar
           + '\n<main class="content" id="main" tabindex="-1">' + crumbs(rel, section, root) + body
           + "</main>\n</div>\n"
           + footer(root) + "\n")
    newline = "\r\n" if "\r\n" in src else "\n"
    return out.replace("\r\n", "\n").replace("\n", newline)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report drift, write nothing")
    args = ap.parse_args()

    drifted = []
    for rel, section, root, desc, sidebar, nav in build_pages():
        path = SITE / rel
        if not path.exists():
            print("missing: %s" % rel)
            drifted.append(rel)
            continue
        src = read_raw(path)
        out = rewrite(src, rel, section, root, desc, sidebar, nav)
        if rel == "index.html":
            out = out.replace('<script src="assets/site.js"></script>',
                              '<script src="assets/site.js"></script>\n<script src="assets/hero.js"></script>')
        if rel == "reference/api.html":
            out = out.replace("</body>", '<script src="../assets/api-reference.js"></script>\n</body>')
            out = re.sub(r'<p class="foot-note">.*?</p>', '<p class="foot-note">Source-audited API reference</p>', out)
        newline = "\r\n" if "\r\n" in src else "\n"   # the page edits above add bare newlines; a CRLF checkout keeps CRLF
        out = out.replace("\r\n", "\n").replace("\n", newline)
        if out != src:
            drifted.append(rel)
            if not args.check:
                with path.open("w", encoding="utf-8", newline="") as handle:
                    handle.write(out)

    if args.check:
        print("\n".join(drifted) if drifted else "chrome up to date")
        return 1 if drifted else 0
    print("chrome applied: %d of %d pages rewritten" % (len(drifted), len(build_pages())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
