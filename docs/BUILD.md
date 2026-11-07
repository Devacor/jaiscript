# Building JaiScript

JaiScript builds with CMake and has no dependencies. The whole thing is a static library plus
headers, and everything else in this repository — the test suite, the runner, the example games
— is a target you can turn off.

## Requirements

- **A C++20 compiler.** Verified — the full suite on both backends — with MSVC 19.51 (Visual
  Studio 18, toolset 14.51), Clang 20 and GCC 16 (MinGW-w64); the CI workflow in
  `.github/workflows/ci.yml` targets MSVC and clang-cl on Windows, GCC 14 and Clang on Ubuntu 24.04, and
  AppleClang on macOS with Makefiles and Xcode. Older compilers may work; they are untested. The library uses `std::to_chars` for doubles and
  `std::strong_order`, so a standard library without those will not get far.
- **libc++ and 32-bit hosts.** The Android NDK (r27c: Clang 18 and libc++, the standard library Apple
  platforms use too) builds every target for arm64-v8a, armeabi-v7a, x86_64 and x86, and a 32-bit
  MSVC build runs the full suite on both backends. `tools/ci/gates.sh android` cross-compiles the
  library and the suite with the NDK it finds (`ANDROID_NDK_ROOT`, or the newest under the SDK).
  An NDK under a path with spaces needs `-DCMAKE_CXX_FLAGS=--driver-mode=g++`: CMake hands
  clang++ its short 8.3 name, which no longer selects the C++ driver, and libc++ goes unlinked.
- **CMake 3.20+**, or 3.25+ for the presets in `CMakePresets.json` and for `tools/install/build_runner.cmake`.
- **Ninja**, optional. CMake's default generator (Visual Studio on Windows, Makefiles elsewhere) builds
  everything; Ninja gives fast incremental single-configuration trees, and the `windows-clang` preset uses it.

Two platform differences are worth knowing:

- **The native-stack guard is Windows-only.** On Windows the engine reads the current thread's
  stack bounds and raises the catchable `Native stack exhausted before the call-depth cap: the host
  thread's stack bounds this recursion` while 256 KiB of headroom remain. Elsewhere only the numeric call-depth caps apply (interpreter 256, VM 250000
  by default; `engine::max_call_depth(backend, depth)`), so a host with a small thread stack
  should lower them.
- **`.jaibite` files are portable.** The format is little-endian on disk whatever the host's
  byte order, with IEEE-754 doubles and 64-bit ints, so a file written on one platform loads on
  any other.

## The short version

The installer clones JaiScript into a folder of its own (`~/.jaiscript`, or `%LOCALAPPDATA%\jaiscript` on
Windows), builds the runner in Release and puts the folder's `bin` on your PATH. Running it again updates and
rebuilds. The scripts are `docs/site/install.sh`, `install.ps1` and `install.cmd`, served from the site:

```bash
curl -fsSL https://jaiscript.com/install.sh | sh                                                 # macOS, Linux, WSL
irm https://jaiscript.com/install.ps1 | iex                                                      # Windows PowerShell
curl -fsSL https://jaiscript.com/install.cmd -o install.cmd && install.cmd && del install.cmd    # Windows CMD
```

Their options, each with an environment variable: `--dir` (`JAISCRIPT_HOME`), `--ref` (`JAISCRIPT_REF`, a tag
or branch; the newest release tag by default), `--repo` (`JAISCRIPT_REPO`), `--generator` (`CMAKE_GENERATOR`),
`--cxx` (`CXX`) and `--no-modify-path` (`JAISCRIPT_NO_MODIFY_PATH=1`). After `| sh`, pass them as
`sh -s -- --no-modify-path`; in PowerShell, as `& ([scriptblock]::Create((irm https://jaiscript.com/install.ps1))) --no-modify-path`.

In a clone, one command configures a Release tree in `build/` with CMake's default generator and builds the
runner, in parallel, with no developer prompt and no Ninja; it runs the same in bash, zsh, PowerShell 7 and CMD:

```bash
cmake -P tools/install/build_runner.cmake
cmake -E chdir . build/bin/jaiscript examples/gatorquest/scripts/gatorquest.jai
```

`cmake -E chdir` runs the runner the same way in every shell; in bash or zsh, `build/bin/jaiscript ...` does too. The
script's `-D` options (given before `-P`) name another tree, a generator, a compiler or a job count; its header
lists them.

Everything, by hand:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/bin/jaiscript_tests
```

That builds everything: the library, the test suite, the differential fuzzer, the runner, the
language server, the web application host `jai_spider` (which uses a system SQLite or downloads a pinned one at configure time), the gate
watchdog `jai_gate_run`, the three example games and the embedding quickstart's host
(`embed_quickstart`). Binaries land in `build/bin/`, the library in `build/lib/`.

The library comes in two links over one set of compiled files. `jaiscript` is the VM alone: every host,
the example games, the quickstart and the language server link it. `jaiscript_interpreter`
adds the tree-walking interpreter (`source/interpreter/`), the reference the VM is checked against; only the
programs that compare the backends link it: the test suite, the fuzzer, the runner, whose
`--backend=interp` runs every documentation sample on both, and `jai_spider`, whose `--backend interp` its gate runs. In a program without it,
`set_backend(backend_type::interpreter)` throws.

`CMakePresets.json` names the same two trees, both with Ninja: `cmake --preset release` / `cmake --build --preset release`
configure and build `build/`, and `debug` does the same in `build-debug/`. Visual Studio's Open Folder lists the
`x64-*` presets, which build under `out/build/<name>/`.

## Toolchain presets

One preset per toolchain configures a Release tree in `build-<preset>/` and builds everything, with the
binaries in `build-<preset>/bin/`:

```bash
cmake --preset linux-gcc
cmake --build --preset linux-gcc -j 8
build-linux-gcc/bin/jaiscript_tests
```

| Preset | Toolchain | Needs |
|---|---|---|
| `windows-msvc` | MSVC, CMake's default Visual Studio generator | Visual Studio 2022 or newer with the C++ workload; any shell |
| `windows-clang` | clang-cl against the MSVC STL, Ninja | A Visual Studio developer prompt with the C++ Clang tools component (or LLVM) installed |
| `macos` | AppleClang, Makefiles | The Xcode command line tools (`xcode-select --install`) |
| `macos-xcode` | AppleClang, the Xcode generator | Xcode |
| `linux-gcc` | GCC, Makefiles | g++ 11 or newer and make |
| `linux-clang` | Clang, Makefiles | clang++ 14 or newer and make |

Makefiles build one file at a time unless the build gets `-j <jobs>`; Visual Studio, Xcode and Ninja run in
parallel by themselves. `--target jaiscript_runner` builds the runner alone.

## Picking targets

Building everything takes a while; usually you want one or two things.

```bash
cmake --build build --target jaiscript          # the library alone
cmake --build build --target jaiscript_interpreter   # the library plus the interpreter
cmake --build build --target jaiscript_tests    # the Foundry test suite
cmake --build build --target jaiscript_runner   # the runner/REPL — the binary is `jaiscript`
cmake --build build --target jaiscript_fuzz     # the differential fuzzer
cmake --build build --target jai_gloom          # GLOOM, the raycast shooter
cmake --build build --target jai_crawler        # GRUBWELL, the dungeon crawler
cmake --build build --target jai_demoreel       # the demoscene reel
```

Note that the runner's CMake target is `jaiscript_runner` (the library already owns the name
`jaiscript`), while the executable it produces is `jaiscript`.

## Where the binaries end up

With a single-configuration generator (Ninja, Make) they are in `build/bin/`:

```bash
build/bin/jaiscript_tests
build/bin/jaiscript hello.jai
build/bin/jai_gloom
```

With a multi-configuration generator (Visual Studio, Xcode) the configuration is chosen at
build time, not configure time; the binaries land in the same `build/bin/`:

```bash
cmake -S . -B build
cmake --build build --config Release --target jaiscript_tests
build/bin/jaiscript_tests
```

The example games copy their scripts next to the binary (`gloom_scripts/`, `crawler_scripts/`,
`demoreel_scenes/`) but prefer the source directory in the tree (`examples/gloom/scripts/`,
`examples/crawler/scripts/`, `examples/demoreel/scenes/`) when they can find it, so editing a
script in the tree is what hot reload picks up. Build the games in Release: they render every
frame in script, and a Debug frame takes tens of times longer. Each game's `README.md` has the
flags.

## Visual Studio: Open Folder

Visual Studio can open this directory directly (**File → Open → Folder**). It detects
`CMakeLists.txt`, configures with Ninja, and puts build directories under
`out/build/<config>/`, so the test executable is `out/build/x64-Debug/bin/jaiscript_tests.exe`.
The configurations it offers are the `x64-*` presets in `CMakePresets.json`.

Debug is where you want to live day to day: it has no link-time code generation, so an
incremental link is seconds rather than minutes. Release links with LTCG (`JAISCRIPT_LTCG`, ON
when JaiScript is the top-level project); `-DJAISCRIPT_LTCG=OFF` gives a fast-linking Release.

## Options

The build targets (tests, runner, language server, examples) and the install rules default to
ON when JaiScript is the top-level project and OFF when it is added to another project with
`add_subdirectory`, so a consumer builds the library alone unless it asks for more.

| Option | Default | What it does |
|---|---|---|
| `JAISCRIPT_BUILD_TESTS` | top-level | The Foundry test suite (`jaiscript_tests`) and the fuzzer (`jaiscript_fuzz`) |
| `JAISCRIPT_BUILD_RUNNER` | top-level | The standalone runner / REPL (`jaiscript_runner`, binary `jaiscript`) |
| `JAISCRIPT_BUILD_LSP` | top-level | The language server `jaiscript_lsp` the VS Code extension launches |
| `JAISCRIPT_BUILD_SPIDER` | top-level | `jai_spider`, the web application host, and its SQLite: a system SQLite 3.37+ if CMake finds one, else the pinned amalgamation downloaded at configure time (`JAI_SPIDER_SQLITE_ZIP` / `JAI_SPIDER_SQLITE_DIR` for offline builds; `tools/jai_spider/README.md`) |
| `JAISCRIPT_BUILD_EXAMPLES` | top-level | `jai_gloom`, `jai_crawler`, `jai_demoreel`, and in a top-level build `embed_quickstart` (the embedding quickstart's host) |
| `JAISCRIPT_INTERPRETER` | top-level | `jaiscript_interpreter`, the library plus the tree-walking interpreter, which the suite, the fuzzer, the runner and `jai_spider` link. OFF: they link `jaiscript`, and the interpreter is not compiled at all |
| `JAISCRIPT_INSTALL` | top-level | The install rules and the `JaiScript` CMake package (`find_package(JaiScript)`) |
| `JAISCRIPT_ENABLE_DEBUGGER` | ON | Compiles the DAP-over-TCP debug listener. Turn it OFF for shipping or mobile builds and no socket code is compiled in at all |
| `JAISCRIPT_ENABLE_BENCHMARKS` | OFF | Adds the performance suites to `jaiscript_tests` (see below) |
| `JAISCRIPT_NATIVE_ARCH` | OFF | `-march=native` for Release. Faster; the binary stops being portable |
| `JAISCRIPT_ENABLE_LTO` | OFF | `-flto` for GCC/Clang Release builds |
| `JAISCRIPT_LTCG` | top-level | Whole-program optimization for MSVC Release (`/GL`, `/LTCG`). The measured performance numbers use it. OFF as a subproject: a `/GL` library would turn every consumer's Release link into a whole-program link (about a minute per edit instead of seconds) |

```bash
cmake -S . -B build -DJAISCRIPT_BUILD_EXAMPLES=OFF -DJAISCRIPT_ENABLE_DEBUGGER=OFF
```

### Diagnostic builds

These build the engine differently for measuring or testing it. None belongs in a shipping build.

| Option | Default | What it does |
|---|---|---|
| `JAISCRIPT_VM_COVERAGE` | OFF | The VM counts every instruction where it runs and why, and every opcode that ran (`engine::vm_coverage_report`, `jaiscript --coverage`) |
| `JAISCRIPT_VM_COVERAGE_TIMING` | OFF | Over `JAISCRIPT_VM_COVERAGE`: the report adds each instruction's own clock, each script function's exclusive and inclusive time, and the parallel regions' phase timers |
| `JAISCRIPT_MISS_INJECTION` | OFF | The VM can force a cell-block miss at every nth plain cell instruction (`engine::cells_miss_every`, `jaiscript --miss-every`) |
| `JAISCRIPT_EXPERIMENT_VM_NO_GS` | OFF | `/GS-` on `vm_backend.cpp` (MSVC), to measure the stack-check cost |
| `JAISCRIPT_RUNNER_PADDING_CONTROL` | OFF | A code-placement control build of the runner, for benchmarking |

### Integer overflow policy

By default, an overflowing `+`, `-`, `*`, unary `-`, or `INT64_MIN / -1` raises a catchable
script error. Define `JAISCRIPT_WRAP_ON_OVERFLOW` for the whole build to opt into silent
two's-complement wraparound instead:

```bash
cmake -S . -B build -DCMAKE_CXX_FLAGS=-DJAISCRIPT_WRAP_ON_OVERFLOW
```

The unused policy's code is eliminated at compile time, so neither choice costs anything at run
time. Arithmetic is never undefined behavior either way.

## Running the tests

```bash
build/bin/jaiscript_tests                 # tree-walking interpreter
build/bin/jaiscript_tests --backend=vm    # bytecode VM
build/bin/jaiscript_tests --backend=vm-reference   # the VM in reference mode
```

**All must pass.** The reference mode (`engine::vm_reference_mode(true)`, the runner's `--backend=vm-reference`) runs
the VM on its plain general path: no superinstruction, typed or dynamic row, closed region, graft, counted loop, return
fusion, tail call, builtin dispatch index, inline cache or const fold, and the shared switches (`type_proofs`,
`ir_passes`, `parameter_alias`, `local_alias`, `aggregate_lowering`) off. It is the VM's differential partner:
`jaiscript_fuzz --vm-reference` runs every program on the optimized VM and on the reference VM, and a divergence is a
bug of an optimized lane. Every choice is made at compile or backend construction, so it costs the optimized VM nothing. The two backends are held to identical results, output, and error text; a
change that only satisfies one of them is not done. See
[../CONTRIBUTING.md](../CONTRIBUTING.md).

Filtering:

```bash
jaiscript_tests "Coroutine"          # ADDITIVE: matching suites (all their tests) OR matching test names
jaiscript_tests "Language.operators" # the dot form is the strict AND: suite filter AND test filter
jaiscript_tests --verbose            # per-test names and timings
```

A grouped CTest view is registered too, in the tests subdirectory:

```bash
ctest --test-dir build/source/tests --output-on-failure
```

Adding a **new** test `.cpp` requires re-running CMake — the test list is a
`file(GLOB_RECURSE)` evaluated at configure time.

## Comparison benchmarks

`JAISCRIPT_ENABLE_BENCHMARKS=ON` adds the performance suites, which time JaiScript alone. The cross-language
comparisons (Lua, Squirrel, ChaiScript and the game ports) are a separate harness, not part of this package.

## Layout

```
JaiScript/
├── CMakeLists.txt
├── include/jaiscript/      Public headers
│   ├── core/                 value, engine, dynamic_binder, class_definition, ...
│   ├── detail/               lexer, parser, AST, and the kernels both backends share
│   ├── vm/                   opcodes, compiler, bytecode backend, disassembler
│   ├── stdlib/  signals/  properties/  serialization/  testing/
├── source/
│   ├── implementation/     Library .cpp files
│   ├── interpreter/        The tree-walking interpreter (jaiscript_interpreter only)
│   └── tests/              Foundry suites, and the differential fuzzer under fuzz/
├── tools/
│   ├── jaiscript/          The runner / REPL
│   ├── lsp/                The language server
│   ├── ci/                 The CI gates (gates.sh), the targeted and battery tiers, the timing lock
│   └── vscode-jaiscript/   The VS Code extension (syntax, debugging)
├── examples/               GLOOM and its idiomatic rewrite, GRUBWELL, the demoreel, Gator Quest, JaiDoom, embed_quickstart
└── docs/                   This directory, and the documentation site under docs/site/
```

## Consuming it from another project

The library target is `jaiscript`, also spelled `JaiScript::jaiscript`; link that name and the
include path, the C++20 requirement and the platform libraries (`ws2_32` and `bcrypt` on Windows, for the
debugger transport and `crypto::`'s random bytes) come with it. There are two CMake routes, below. Visual Studio (MSBuild) and Xcode projects compile
the sources directly; [Building JaiScript into a Host](site/embedding/18-build-integration.html) gives the sources, include
path, flags and defines for each, with a property sheet for MSBuild and the Xcode build settings by name.

**From source, as a subproject.** Nothing but the library builds by default:

```cmake
add_subdirectory(JaiScript)
target_link_libraries(my_game PRIVATE JaiScript::jaiscript)
```

`FetchContent` works the same way. Options set before the `add_subdirectory` (or with `-D`)
turn the tools back on if you want them.

**Installed, through `find_package`.** Build and install the library once per configuration;
Debug and Release share a prefix (the Debug library is `jaiscriptd`). A top-level Release build
compiles the library with `/GL` on MSVC; this command turns that off (`-DJAISCRIPT_LTCG=OFF`), so your Release
edits link against the install quickly, and builds the library alone:

```bash
cmake -S JaiScript -B build-jai -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/jaiscript -DJAISCRIPT_LTCG=OFF -DJAISCRIPT_BUILD_TESTS=OFF -DJAISCRIPT_BUILD_EXAMPLES=OFF -DJAISCRIPT_BUILD_RUNNER=OFF -DJAISCRIPT_BUILD_SPIDER=OFF -DJAISCRIPT_BUILD_LSP=OFF
cmake --build build-jai
cmake --install build-jai
```

```cmake
find_package(JaiScript 0.5 CONFIG REQUIRED)          # -DCMAKE_PREFIX_PATH=/opt/jaiscript
target_link_libraries(my_game PRIVATE JaiScript::jaiscript)
```

On MSVC the library follows the runtime of the project that builds it: as a subproject it takes
the parent's `CMAKE_MSVC_RUNTIME_LIBRARY` (CMake's default is the DLL runtime), and the in-tree
build defaults to the static runtime (`/MT`, `/MTd`) unless you pass `CMAKE_MSVC_RUNTIME_LIBRARY`
yourself. An installed package records the runtime its library was built with in
`JaiScript_MSVC_RUNTIME_LIBRARY` (empty for CMake's default), so a consumer matches it with one
line:

```cmake
set_property(TARGET my_game PROPERTY MSVC_RUNTIME_LIBRARY "${JaiScript_MSVC_RUNTIME_LIBRARY}")
```

The target carries its own interface flags: `/bigobj` on MSVC (the binder headers overflow the
default section limit in a Debug translation unit) and `-Wa,-mbig-obj` on MinGW GCC. Warning
flags stay private to the library.

`examples/embed_quickstart/` is a complete consumer: `add_subdirectory` by default,
`-DEMBED_QUICKSTART_FIND_PACKAGE=ON -DCMAKE_PREFIX_PATH=<prefix>` for the installed package.
