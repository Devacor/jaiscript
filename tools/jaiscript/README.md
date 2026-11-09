# jaiscript — the standalone runner / REPL

`jaiscript.exe` is JaiScript's `python.exe`: run a script from the command line, type code
into a live session, or point it at a file and re-run on every save. It is also the
**reference embedding** — the host is deliberately thin, and every engine call in
`main.cpp` is the canonical shape from `docs/site/embedding`.

Built by the `jaiscript_runner` CMake target (on by default, `JAISCRIPT_BUILD_RUNNER`);
the binary lands in the build directory's `bin/` (`build/bin/jaiscript`; see `docs/BUILD.md`).

## Mode 1 — file runner

```
jaiscript file.jai [options] [-- args...]
```

Runs the script and exits. The exit code is the script's result if it is an int
(a trailing expression works: end your script with `some_int_expr`), otherwise 0;
script errors print message + stack trace to stderr and exit 1; usage errors (an unknown
flag, a missing or malformed value) exit 2. Only 0 to 255 survive as an exit code on every
platform: an int outside that range exits with its low byte (`300` exits 44, `-1` exits 255),
on Windows as on POSIX, so keep a result meant as an exit code in that range.

- `include`/`import` resolve relative to the script's own directory.
- Everything after `--` is the global `ARGS` (array of strings).
- **Drag-drop a `.jai` file onto the exe works** — and when the console window is
  exclusively ours (drag-drop / double-click launch) the runner pauses on
  "press Enter to close" so the output is readable. `--pause` / `--no-pause` override.
- A UTF-8 BOM (Notepad's default) is stripped before parsing.

### Source on the command line: `-e`

```
jaiscript -e "print(1 + 2)"
jaiscript -e "int n = 20;" -e "n * 2"      # exit code 40
```

`-e <source>` runs the text as a file run would run a file's text, then exits: the exit code
is the last source's result if it is an int, otherwise 0; a script error prints its message
and stack trace (attributed to `<script>`) and exits 1. Several `-e` run in order in one
engine, so declarations carry from one to the next, and the first error stops the run.
Nothing prints unless the source prints: `jaiscript -e "1 + 2"` prints nothing and exits 3.
The REPL is the mode that echoes values, piped or typed (`echo 1 + 2 | jaiscript` prints `3`).
`-e` combines with `--backend`, `--workers`, `--budget`, `--time` and `-- args`;
`include`/`import` resolve from the working directory. A script file or `--watch` with `-e`
is a usage error (exit 2).

## Mode 2 — REPL

```
jaiscript
```

A persistent engine: globals, functions and classes accumulate across lines.

```
JaiScript 0.5.2-beta - vm backend, stdlib loaded, budget off. :help for commands.
jai> int hp = 100
jai> hp - 58
42
jai> int add(int x, int y) {
...> return x + y;
...> }
jai> add(4, 5)
9
```

- **No `;` at the end of a line**: a line's last statement needs none, whatever its kind
  (`int hp = 100`, `hp += 1`, `import "rules.jai"`), as the last expression of an execute
  needs none; statements inside `{}` keep the language's rule.
- **Multi-line**: input keeps collecting while `()`, `[]`, `{}`, `/* */` or a string is
  open (the prompt becomes `...>`); a blank line force-submits what you have.
- **Auto-print**: a non-null result prints. Strings print quoted (`"5"` vs `5` stay
  distinguishable), chars quoted with `'`, everything else as `print` shows it
  (`[1, 2, 3]`, `{"k": 7}`, `P{x: 1}`); `print(...)` and other null-returning statements print nothing.
- **Errors don't kill the session** — the message prints and the engine stays usable.
- **Paste/drop a path**: a line that is a (optionally quoted) path to an existing `.jai`
  file runs that file into the session — this is exactly what dropping a file onto an
  open console window produces.
- **Piping works**: `echo 1+1 | jaiscript` (prompts and banner only print on a real console).
  A piped session exits 1 when any input failed (an error, a missing file), else 0.

Meta-commands (`:help` inside):

| command | effect |
|---|---|
| `:load <path>` | run a `.jai` file into this session |
| `:reset` | fresh engine (keeps backend, budget, debugger) |
| `:backend vm\|interp\|vm-reference` | switch backend — resets the engine (backends can't swap mid-engine) |
| `:time on\|off` | print wall time per execute |
| `:quit` / `:exit` | leave (Ctrl+Z+Enter also works; Ctrl+C ends the runner, even while input runs) |

## Mode 3 — live-coding watch

```
jaiscript --watch file.jai
```

Runs the file, then re-runs it every time you save — **edit in your editor, save, see
output**. The console clears between runs and a status line reports run #, duration,
ok/ERROR and the time. Ctrl+C quits (see Ctrl+C below). Each run gets a **fresh engine** (deterministic
world per save); because of that, the DAP listener is off in watch mode.

`--watch` restarts the world on every save. A program that keeps its world and swaps code in
place reaches `import` in its own loop instead, inside `try`/`catch`: a rejected reload raises
a catchable error at the import, and the program keeps its old code
(`docs/site/guide/10-modules.html`, "Reloading from a running program").

## Options

```
-e <source>           run the source text instead of a file (repeatable; see Mode 1)
--backend=vm|interp|vm-reference
                      execution backend (default vm; vm-reference is the VM with every
                      optimization off)
--budget=N            execution budget in seconds per execute (default 0 = unlimited;
                      the embedded-engine default of 5.0s is deliberately lifted —
                      long loops are the user's business in a runner)
--time                print wall time for each run
--warnings            list the static checker's warnings on stderr before a script runs
                      (errors always fail the load and print)
--benchmark-main=N    execute the file as a warm-up, then call its repeatable main() N times
--disasm=<file>       write every chunk the VM compiles, disassembled with its regions' IR, to <file>
--manifest=<file>     write the same chunks' semantic manifests (every field, exact constant bits)
                      to <file>
--coverage=<file>     write the VM's execution-coverage census (cells / typed / general, the
                      reasons and their source sites) to <file> at exit (a build with
                      JAISCRIPT_VM_COVERAGE)
--miss-every=<n>      the VM misses at every nth plain cell instruction (a build with
                      JAISCRIPT_MISS_INJECTION)
--workers=<n>         the worker count of parallel_for, parallel_transform and parallel_reduce
                      (default: the engine's own)
--no-param-alias      bind every by-value parameter with a clone (the read-only elision off)
--no-local-alias      copy every by-value local declaration (the read-only local elision off)
--no-proofs           run without type proofs and the typed instructions they enable (the
                      general form)
--debug               stand up the DAP debug listener and name its port on stderr
--debug-port=N        --debug, with N the first port the listener tries (default 52477, probing +10)
--pause / --no-pause  force / suppress the exit hold after a file run
--selftest            built-in smoke tests (balancer, file mode, scripted REPL session)
--version / --help
```

A value flag takes its value after `=` or as the next argument (`--workers=4`, `--workers 4`);
any other spelling is an unknown flag. Numbers are checked: `--workers` 0 to 4096, `--debug-port`
1 to 65535, `--budget` a finite number of seconds from 0, `--benchmark-main` and `--miss-every`
counts from 0.

## Environment the runner provides

The full JaiScript stdlib is always registered (`jai::stdlib::register_all`): formatted
`print` / `format` / `to_string` / `type_of`, math, `to_json` / `from_json`, container
helpers, the `random` type, `crypto::` (hashes such as `crypto::sha256(s)` and `crypto::sha256_file(path)`, HMAC,
secure random bytes and password hashing; `crypto::sha256_file` needs the `filesystem` privilege and raises `read_file`'s
error below), and the opt-in `web::` library (`jai::stdlib::register_web`: `web::html_escape`, `web::url_encode`, `web::form_encode`,
`web::parse_query`, `web::build_query`, `web::parse_url` and the rest), and the opt-in `spider::` HTTP client
(`jai::stdlib::register_spider`: `spider::catch(url[, options])`, also spelled `spider::get`, and
`spider::sling(content, url[, options])`, also spelled `spider::post(url, content[, options])`, each answering a
`spider::Response`) with the `network` permission granted and the transport of `tools/jaiscript/http_transport.hpp`:
WinHTTP on Windows, sockets elsewhere with HTTPS through OpenSSL when the build found it (plain HTTP otherwise);
`JAISCRIPT_HTTP_TRANSPORT=socket` picks the socket transport on Windows. (A bare engine has far less, listed in the reference's "What a bare engine has"
(`docs/site/reference/index.html#bare-engine`); the runner opts into everything.) On top, the host binds file IO so scripts can actually save things:

```
read_file(path) -> string      write_file(path, text) -> bool   (creates missing folders)
file_exists(path) -> bool      delete_file(path) -> bool
move_file(from, to) -> bool    (replaces `to`; false when `from` is not a file)
dir_exists(path) -> bool       make_dirs(path) -> bool           list_dir(path) -> array<string>
writable_dir(path) -> bool     (creates the folder if missing, writes and removes a probe)
zip_entries(zip) -> array<string>  zip_extract(zip, entry, dest) -> bool   (CRC-32 verified)
```

`move_file` is the last step of a safe save: write `save.tmp`, check it, then
`move_file("save.tmp", "save.txt")`. On one volume the destination is replaced in a single
operation (`MoveFileEx` on Windows, `rename(2)` elsewhere), so a crash leaves the old file or
the new one, never half of either; across volumes it copies, then removes the source. Every
function above requires the engine's `filesystem` privilege. A file's metadata (size, created and
modified times) is the engine-core `file_data(path)` builtin, on every engine.

`read_file` returns the file's bytes exactly (binary mode: every byte value, NULs and `\r\n` as they
are; `""` for an existing empty file). A file that is missing or cannot be read raises a catchable
error, `Cannot read file '<path>': <reason>`, the reason one of `not found`, `not a file` (a folder),
`permission denied`, `cannot be opened` or `read failed`. Ask `file_data(path) != null` (or `file_exists(path)`)
first when absence is expected, or catch the error; uncaught, it ends the run with the usual trace. `write_file` writes
the bytes exactly and answers `false` when it cannot.

Where things are is not the host's to invent: every engine carries the location builtins
(`resolve_path`, `current_script_dir`, `root_script_dir`, `executable_dir` with alias
`bin_dir`, `app_data_dir`, `app_cache_dir`, `working_dir`, `parent_dir`, `file_name` and the
`_path` twins; the guide's Modules chapter, "Where files are"). Loading walks the import
ladder through `resolve_path`; saving picks an anchor and asks `writable_dir` whether it can
take a file. A script dropped onto the executable therefore runs the same from any working
directory. JaiDoom chooses the folder above `scripts/` when it is writable and
`app_data_dir("JaiDOOM")` otherwise.

The host also binds `Download`, an asynchronous download the script polls from its own loop,
the same shape as the `NetChannel` binding (`tools/jaiscript/fetch.hpp`):

```
auto d = Download();
d.start(url, path)          // http or https; false with d.error() set when it cannot begin
while (!d.pump()) { ... }   // d.received(), d.total() (-1 until known), d.fraction()
d.success()  d.error()  d.cancel()
```

The transport is the platform's own: WinHTTP on Windows, libcurl on Linux and macOS when
the build found it, otherwise the `curl` executable at run time; redirects follow only to http
and https. `start` needs the `filesystem` and `network` privileges. A failed transfer leaves no
partial file. JaiDoom's first run uses this to fetch and verify its own game data.

For hosting, `net_host_name()` returns this machine's name and `net_local_addresses()` its IPv4
addresses (loopback left out), which is what a player hosting a game tells the others to type.

`env_var(name)` returns an environment variable's value, `""` when it is unset; a terminal game
reads `COLORTERM` (`truecolor` or `24bit`), and on Windows `WT_SESSION` (Windows Terminal), to
choose between 24-bit colour and the 256-colour palette.

The strings after `--` are the global `ARGS`.

## Ctrl+C

Ctrl+C or Ctrl+Break on Windows, and SIGINT, SIGTERM or SIGHUP on Linux and macOS, end the
runner at once, in every mode: a file run, `--watch` and the REPL alike (the REPL has no way to
stop just the input that is running, so Ctrl+C leaves it). The script stops where it is and its
destructors do not run. The runner gives the terminal back and exits with 130; on Linux and macOS
the signal itself ends the process, so the shell reports 130, 143 or 129. A signal the runner was
started with ignored (`nohup`, a background job) stays ignored. A script that must stop itself in
time has the execution budget, `--budget=N`.

Giving the terminal back means what the script's output changed: the runner reads the escape
sequences the script prints, and on a terminal (not a pipe or a file) it resets the colours
(`ESC[0m`) when a colour or attribute is still set, shows the cursor (`ESC[?25h`) when the script
hid it, and leaves the alternate screen (`ESC[?1049l`) when the script entered it. It does the
same when a script ends or fails without undoing them, before printing the error, so the error
lands on the normal screen. It also restores the terminal's input mode (the raw mode `read_key` sets on Linux
and macOS), and on Windows the console's output mode and code pages. A script that prints no
escape sequences sees none added.

## Debugging

`--debug` stands up a DAP listener (localhost only, costs nothing until a session attaches) in
file and REPL modes — you can attach VS Code to a *running REPL session* and set breakpoints
in files you `:load`. Install `tools/vscode-jaiscript`, F5 → "Attach to JaiScript" with the
port: 52477, or the next free one in steps of 10, which the runner names (on stderr in a
file run, in the REPL's banner): `debugger: DAP listening on 127.0.0.1:52477`. Without
`--debug` or `--debug-port=N` nothing listens: the listener takes no password, so any local
process could attach.
