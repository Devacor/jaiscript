# JaiScript

JaiScript is an embedded scripting language for C++ engines: C++-flavored syntax, script
classes with hot reload, coroutines, a three-rung type ladder (`int` / `auto` / `var`) with a
static checker that is strict by default, value semantics with references by declaration, and
two interchangeable backends — a tree-walking interpreter and a bytecode VM — kept at
byte-for-byte parity by 3,900+ tests and a differential fuzzer. It has no dependencies and
builds as a static library plus headers.

## Get JaiScript

You need git, CMake 3.25+ and a C++20 compiler: Visual Studio 2022 or newer on Windows, the Xcode
command line tools on macOS, GCC 11 or Clang 14 on Linux. The installer clones JaiScript, builds the
runner in Release and puts it on your PATH (read [install.sh](docs/site/install.sh),
[install.ps1](docs/site/install.ps1) and [install.cmd](docs/site/install.cmd) first if you like):

```bash
curl -fsSL https://jaiscript.com/install.sh | sh                                                 # macOS, Linux, WSL
irm https://jaiscript.com/install.ps1 | iex                                                      # Windows PowerShell
curl -fsSL https://jaiscript.com/install.cmd -o install.cmd && install.cmd && del install.cmd    # Windows CMD
```

Then open a new terminal and run `jaiscript --version`. Without the installer, this one line clones the
repository, builds the runner with CMake's default generator (Visual Studio on Windows, Makefiles elsewhere) and
plays Gator Quest; it runs unchanged in bash, zsh, PowerShell 7 and CMD, with no developer prompt and no Ninja:

```bash
git clone --depth 1 https://github.com/Devacor/jaiscript.git && cmake -P jaiscript/tools/install/build_runner.cmake && cmake -E chdir jaiscript build/bin/jaiscript examples/gatorquest/scripts/gatorquest.jai
```

`CMakePresets.json` names a preset per toolchain (`windows-msvc`, `windows-clang`, `macos`, `macos-xcode`,
`linux-gcc`, `linux-clang`); [docs/BUILD.md](docs/BUILD.md) has them and everything else.

The documentation — a 20-chapter language guide, a 21-chapter embedding guide, language bridges
and code migrations, the examples and a reference — is a static site in [`docs/site/`](docs/site/), published at
[jaiscript.com](https://jaiscript.com). Open `docs/site/index.html` in a browser; a clone is all you need.

## A taste

```jaiscript
class Creature {
    string name;
    int hp = 10;

    Creature(string n, int h) { name = n; hp = h; }

    void hurt(int amount) { hp -= amount; }
    bool alive() { return hp > 0; }
}

coroutine int countdown(int from) {
    for (int i = from; i > 0; i--) { yield i; }
}

auto squad = [Creature("scout", 3), Creature("brute", 9)];
for (auto& c : squad) { c.hurt(4); }              // auto& mutates the elements in place
for (auto c : squad) { print("{} hp={} alive={}", c.name, c.hp, c.alive()); }

for (auto n : countdown(3)) { print("countdown {}", n); }   // a coroutine drives a range-for

var loose = 1;
loose = "now a string";                           // var is the opt-out from typing
print("{} {}", type_of(loose), cast<int>('a'));   // char converts only through a cast

try { int x = loose; print("{}", x); } catch (e) { print("caught: {}", e); }
```

```
scout hp=-1 alive=false
brute hp=5 alive=true
countdown 3
countdown 2
countdown 1
string 97
caught: Cannot initialize 'x' (declared int) with string
```

That last line is the type ladder in one breath. `int x = "text"` never reaches the runtime —
the checker rejects it before a statement executes. `int x = loose` *does* reach the runtime,
because the value came through `var` and nothing could know its type ahead of time, so it
raises where `try`/`catch` can see it.

## Embedding it

An engine is an object. There is no global interpreter state anywhere: every value carries its
engine, so two engines on two threads need no locks.

```cpp
#include <jaiscript/core/engine.hpp>
#include <jaiscript/core/dynamic_binder.hpp>
#include <jaiscript/stdlib/stdlib.hpp>
#include <cmath>

struct Vec2 {
    double x = 0, y = 0;
    Vec2(double x, double y) : x(x), y(y) {}
    double length() const { return std::sqrt(x * x + y * y); }
};

auto e = jai::engine::make();          // always a shared_ptr; the constructor is private
jai::stdlib::register_all(e);          // formatted print, to_string, math, JSON, containers

e->add_function("sqrt", [](jai::script_float x) { return std::sqrt(x); });

jai::dynamic_binder<Vec2>(*e, "Vec2")
    .constructor<jai::script_float, jai::script_float>()
    .property("x", &Vec2::x)
    .property("y", &Vec2::y)
    .method("length", &Vec2::length)
    .build();

double len = e->execute<double>("Vec2(3, 4).length()");      // 5.0
```

One knob is worth knowing on day one:

```cpp
e->set_backend(jai::backend_type::interpreter);   // tree-walking interpreter instead of the VM; must come
                                                  // before the first execute, in a program that links
                                                  // jaiscript_interpreter
```

The embedding guide covers the rest: binding inheritance and operators, `shared_ptr` handles,
hot reload, precompiled `.jaibite` scripts, execution budgets, signals, and the debugger.

## What's in the box

**Types.** `int`, `float`, `string`, `char`, `bool`, arrays and maps (typed or dynamic),
`shared_ptr<T>` handles, `weak_ptr<T>`, and `T&` references. The ladder is `int` (enforced),
`auto` (inferred, then enforced), `var` (dynamic, forever).

**The checker.** Always on and strict: typed-to-typed mistakes and undeclared identifiers are
load errors, thrown as `jai::static_check_error` before anything runs; only a value sourced
from `var` can still fail at run time. A program's imports are checked as one closure, so a
name declared in one file is known in every other whatever the import order. `extern`
declares names the host provides, and `[[warn_mode]]` reports a unit's, a block's or a single
statement's errors as warnings and loads it (`[[strict_mode]]` re-arms a region inside one).

**Value semantics.** Assignment copies, like C++. You get aliasing when you ask for it — `T&`
references, `shared_ptr<T>` handles — and `cast<T>` / `cast<T&>` / `cast<shared_ptr<T>>` are
the explicit spellings at a typed boundary.

**Script classes.** Inheritance with `super()`, constructor overloading and `: this(...)`
delegation, static members (`C::s`), operator overloading, user `_get_x` / `_set_x`
accessors, and hot reload that migrates live instances field by field.

**Coroutines.** `coroutine` functions *and* methods, `yield`, drivable from a range-`for` or
advanced with `next()` by the host a frame at a time.

**Everything else you reach for.** Lambdas with C++ capture lists, default arguments,
host functions that take any number of arguments, template strings, `include` / `import` with a `.jaibite` disk cache, namespaces,
enums, destructuring, `parallel_transform`, JSON and binary serialization, a `reflect::` API,
a DAP debugger with a VS Code extension, an execution budget, a memory cap, and checked
integer overflow.

## Build

You need a C++20 compiler and CMake 3.20+ (3.25+ for the presets and `tools/install/build_runner.cmake`). Verified — the full suite on
both backends — with MSVC 19.51 (Visual Studio 18, toolset 14.51), Clang 20 and GCC 16; the
CI workflow targets MSVC and clang-cl on Windows, GCC 14 and Clang on Ubuntu 24.04, and AppleClang on
macOS with Makefiles and Xcode. Older compilers may work and are
untested.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jaiscript_tests jaiscript_runner
```

Binaries land in `build/bin/`. With the Visual Studio generator, drop `-G Ninja` and pass
`--config Release` to the build step; the binaries land in `build/bin/` too.

Debug links in seconds. On MSVC a top-level Release build links with LTCG and takes minutes. Iterate
in Debug, then do a Release pass.

### Fast builds in your own project

Whole-program optimization is what makes a Release link slow: the linker generates code again for
every object compiled with `/GL`, on every link. It speeds the VM up a few percent, so keep JaiScript
out of it while you develop and turn it on for the build you ship.

- **CMake.** `add_subdirectory(JaiScript)` already does this: `JAISCRIPT_LTCG` is OFF in a
  subproject, the library is a static library of plain objects, and your Release link stays
  incremental. A top-level or installed build takes `-DJAISCRIPT_LTCG=OFF`.
- **A Visual Studio project.** Build JaiScript as a static library project of its own with
  Whole Program Optimization set to No, and reference it from your program. With `jaiscript.cpp`
  compiled inside your own project instead, set that one property to No on the file. Either way
  a one-line edit to your code links in seconds, where a whole-program link takes a minute or more.
  [Visual Studio, Step by Step](docs/site/embedding/21-visual-studio.html) walks through both setups.

| Option | Default | What it does |
|---|---|---|
| `JAISCRIPT_BUILD_TESTS` | top-level | The Foundry test suite (`jaiscript_tests`) and the fuzzer (`jaiscript_fuzz`) |
| `JAISCRIPT_BUILD_RUNNER` | top-level | The standalone runner / REPL (`jaiscript`) |
| `JAISCRIPT_BUILD_LSP` | top-level | The language server (`jaiscript_lsp`) |
| `JAISCRIPT_BUILD_SPIDER` | top-level | The web application host `jai_spider` and its SQLite (the system's, else a pinned download) |
| `JAISCRIPT_BUILD_EXAMPLES` | top-level | `jai_gloom`, `jai_crawler`, `jai_demoreel`, and in a top-level build `embed_quickstart` (the embedding quickstart's host) |
| `JAISCRIPT_INTERPRETER` | top-level | `jaiscript_interpreter`, the library plus the tree-walking interpreter (the suite, the fuzzer, the runner and `jai_spider` link it) |
| `JAISCRIPT_INSTALL` | top-level | Install rules and the `find_package(JaiScript)` package |
| `JAISCRIPT_ENABLE_DEBUGGER` | ON | The DAP-over-TCP debug listener; turn it off for shipping builds |
| `JAISCRIPT_ENABLE_BENCHMARKS` | OFF | Performance suites inside `jaiscript_tests` |
| `JAISCRIPT_NATIVE_ARCH` | OFF | `-march=native`; faster, non-portable binaries |
| `JAISCRIPT_ENABLE_LTO` | OFF | `-flto` for GCC/Clang Release builds |
| `JAISCRIPT_LTCG` | top-level | `/GL` + `/LTCG` for MSVC Release builds (OFF as a subproject, so a consumer's Release link stays incremental) |

"top-level" means ON when JaiScript is the top-level CMake project and OFF when another project
pulls it in with `add_subdirectory`; [`docs/BUILD.md`](docs/BUILD.md) has every option and both
consumption routes (`add_subdirectory` and `find_package(JaiScript)`).

Two third-party sources ship: [minicoro](source/third_party/minicoro) (public domain or
MIT-0), which the library compiles, with its AddressSanitizer fiber-switch annotations corrected, and the SQLite amalgamation
(public domain), which `jai_spider` uses from the system or downloads at configure time, hash-pinned (see [`tools/jai_spider/README.md`](tools/jai_spider/README.md)). The cross-language comparison benchmarks are a
separate harness, not part of this repository. Integer
arithmetic raises a catchable error on overflow by default; define `JAISCRIPT_WRAP_ON_OVERFLOW`
for the whole build to opt into silent two's-complement wrap instead.

## Tests

```bash
build/bin/jaiscript_tests                 # tree-walking interpreter
build/bin/jaiscript_tests --backend=vm    # bytecode VM
```

Both must be green. That is the project's central rule, and the reason two backends can be
maintained at all.

Filter with `jaiscript_tests "Coroutine"` (additive: matching suites *or* matching test names)
or `jaiscript_tests "Suite.Test"` (the strict AND form). `jaiscript_fuzz --iterations 1000`
runs the differential fuzzer, which generates programs and fails if the two backends disagree
on output or on error text.

## The runner

`jaiscript` is this language's `python`: a file runner, a REPL, and a live-coding watcher.

```bash
jaiscript hello.jai            # run a file and exit
jaiscript                      # REPL — globals, functions and classes accumulate
jaiscript --watch hello.jai    # re-run on every save
```

It defaults to the VM backend; `--backend=interp` and `--budget=N` change that, and every
script loads through the strict checker. Details in [`tools/jaiscript/README.md`](tools/jaiscript/README.md).

## Examples

Five complete programs live in [`examples/`](examples): the four below, and JaiDoom after them.
Three have a C++ host that is a thin console shell — a VT terminal, key input, a clock — and *all*
of the actual program in script; Gator Quest and JaiDoom run on the stock runner alone.

- **GLOOM** (`examples/gloom`) — a real-time raycast shooter: four maps, keycards, secrets,
  enemy brains as coroutines, particles through `parallel_transform`.
- **GRUBWELL** (`examples/crawler`) — a step-based first-person dungeon crawler, and the hot
  reload showpiece: run it with `--dev`, edit a script, press `R`, and the dungeon changes
  under you.
- **demoreel** (`examples/demoreel`) — eleven truecolor demoscene scenes, every pixel computed
  by script on the VM; press `r` after editing a scene file and the change appears mid-scene.
- **Gator Quest** (`examples/gatorquest`) — a terminal RPG across six realms, with combat, spells,
  crafting, bots and netplay; the world, the art, the sound effects and the music are generated
  by the scripts. It needs nothing but the runner:
  `build/bin/jaiscript examples/gatorquest/scripts/gatorquest.jai`.

Each runs from a `.jai` entry script on the runner, and each ships a self-check that plays itself
headlessly and prints a deterministic state hash (`--smoke` for the hosted three, `-- smoke` after
Gator Quest's script). The interpreter and the VM have to produce the same one.

A larger demo, **JaiDoom** (`examples/jaidoom`), is a DOOM-format engine written from scratch
in script: WAD loader, BSP renderer, playsim, savegames, demo playback and 16-player netplay.
The scripts ship here; no game data does. The first run fetches Freedoom (and, if you agree,
the DOOM shareware episode), verifies each file by SHA-256 and keeps it outside the repository.
It is a fan project, not affiliated with or endorsed by id Software.

## Documentation

- [jaiscript.com](https://jaiscript.com) ([`docs/site/index.html`](docs/site/index.html)) — the documentation site: language guide,
  embedding guide, examples, reference.
- [`docs/site/guide/index.html`](docs/site/guide/index.html) — the language guide, from the
  first script to modules, reflection and strict checking.
- [`docs/site/bridges/index.html`](docs/site/bridges/index.html) — language bridges from
  JavaScript, C++ and C#, and code migrations from Cereal, Boost.Serialization, Boost.Signals2,
  ChaiScript, sol2, Squirrel, pybind11, nlohmann::json, Qt properties and coroutine schedulers.
- [`docs/site/reference/`](docs/site/reference/index.html) — the lookup pages: language rules
  (operators, conversions), the library, the engine API, and every error text in
  [`errors.html`](docs/site/reference/errors.html) (byte-identical on both backends).
- [`docs/site/embedding/06-static-checking.html`](docs/site/embedding/06-static-checking.html) —
  the checker's contract: modes, the conversion table, escape hatches.
- [`docs/site/embedding/21-visual-studio.html`](docs/site/embedding/21-visual-studio.html) —
  Visual Studio, step by step: from a fresh install to a C++ program that runs a script.
- [`tools/vscode-jaiscript/`](tools/vscode-jaiscript/) — syntax highlighting and the debugger
  client for VS Code.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to build, test, and land a change.

Bugs and questions: [github.com/Devacor/jaiscript/issues](https://github.com/Devacor/jaiscript/issues).

## Status

JaiScript 0.5 beta, released 12 November 2026. The language, both backends, the embedding API and
the `.jaibite` format are complete and covered by 3,900+ tests run on both backends and a
differential fuzzer. Beta means the C++ API may still change between minor versions;
[`docs/site/stability.html`](docs/site/stability.html) lists what is settled and what may change
before 1.0.

## License

MIT — see [LICENSE](LICENSE).

## Credits

JaiScript was designed and created by Devacor (mike@m2tm.net) in honor of his love, Jai.

If you ship a game or a tool with JaiScript inside, a small line in its credits, such as "Scripted with
JaiScript", would be lovely to see. It is a request, not a condition: the MIT license is the only term that
applies.
