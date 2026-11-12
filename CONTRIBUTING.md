# Contributing to JaiScript

Thanks for looking. This file is the whole workflow: configure, build, test, and the handful of
rules that keep a two-backend language honest. [`docs/BUILD.md`](docs/BUILD.md) has the
longer build reference — options, generators, where binaries land.

## Configure once per configuration

```bash
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B build       -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Iterate in Debug. It has no link-time code generation, so an incremental link is about five
seconds against several minutes for Release, and on MSVC the debug runtime fills freed memory
with `0xDD`, which turns a dangling reference into a loud crash instead of a quiet wrong
answer. Do a Release pass before you call a change done; Release is the configuration that
ships, and optimizer-dependent bugs only show up there.

(`cmake --preset debug` and `cmake --preset release` configure the same two trees. Visual
Studio's Open Folder mode uses the `x64-*` presets, which build under `out/build/<preset>/`.)

## Build

```bash
cmake --build build-debug --target jaiscript_tests
```

Adding a **new** test `.cpp` needs a reconfigure — the test list is a `file(GLOB_RECURSE)`
evaluated at configure time, so a fresh file is invisible until CMake runs again. Adding a test
to an existing file needs nothing.

## The parity rule

Every change is run twice:

```bash
build-debug/bin/jaiscript_tests
build-debug/bin/jaiscript_tests --backend=vm
```

Both must be green, and "green" means more than a pass count: the tree-walking interpreter and
the bytecode VM must produce the same results, the same printed output, and the same error
text, byte for byte. The interpreter is the VM's oracle and the VM is the interpreter's; that
is what makes it possible to trust either one.

The way that stays affordable is shared kernels. A rule both backends need lives once, in a
header under `include/jaiscript/detail/`, and both backends call it:

| kernel | owns |
|---|---|
| `typed_slot.hpp` | the conversion a typed slot runs — declarations, assignments, parameters, returns, field stores — one table for all of them |
| `return_conv.hpp` | which route a `return` takes through that table |
| `container_enforce.hpp` | the element boundary of `array<T>` / `map<K,V>` |
| `cast_kernel.hpp` | `cast<T>` / `cast<T&>` / `cast<shared_ptr<T>>` |
| `ref_lvalue.hpp` | lvalue reference arguments (`f(obj.field)`, `f(arr[i])`) |
| `char_promotion.hpp` | `char` entering arithmetic and bitwise operators as 0..255 |
| `builtin_methods.hpp` | every builtin container and string method |
| `type_proofs.hpp` | the runtime guards behind the parse-time type proofs |
| `null_member_diagnostic.hpp`, `arity_diagnostic.hpp` | error texts both backends raise |

If you find yourself writing the same rule twice, that is the signal to add a kernel rather
than a second implementation.

## Tests

Suites live under `source/tests/`, one directory per area: `language/`, `semantics/`,
`containers/`, `stdlib/`, `vm/`, `serialization/`, `signals/`, `properties/`, `core/`,
`integration/`, `debug/`, `foundry/` (the framework's own suite), `performance/` (built only
with `JAISCRIPT_ENABLE_BENCHMARKS`), and `fuzz/`.

**Reproduce a bug as a failing test first.** Watch it go red, then fix it. A test written after
the fix proves much less.

The framework is Foundry, which is in the tree:

```cpp
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>

namespace jai::foundry::tests {
class my_tests : public suite {
public:
    my_tests() : suite("My Suite") {}
    void forge_tests() override {
        test("two_plus_two", [this]() {
            auto e = make_engine();                                // honors --backend=vm
            check_eq((int64_t)4, e->execute("2 + 2").as_int());   // check_eq(expected, actual)
        });
    }
};
}

// FOUNDRY_REGISTER is at global scope, so it needs a using-alias first:
using my_tests = jai::foundry::tests::my_tests;
FOUNDRY_REGISTER(my_tests)
```

Assertions: `check`, `check_eq`, `check_ne`, `check_near`, `check_throws`, `check_true`,
`check_false`, `check_null`, `check_not_null`, `check_gt`, `check_lt`, `check_ge`, `check_le`.

Create engines through `foundry::make_engine()`, not `engine::make()` — it applies the
runner's `--backend=` choice (the interpreter unless `--backend=vm`), and that is how both
suites reach a test. A test that calls `engine::make()` directly runs the engine's default, the
VM, in both suites; one that needs a particular backend says so with `set_backend`.

One thing to know about parse errors: a recoverable one makes the parser synchronize, so
`execute()` may return null rather than throw. That is why "deeply nested input doesn't crash"
is tested by "execute returns and the engine is still usable", not by `check_throws`.

Filtering:

```bash
jaiscript_tests "Script Class"        # additive: matching SUITES (all their tests) OR matching test names
jaiscript_tests "Suite.Test"          # the dot form is the strict AND
jaiscript_tests --verbose             # per-test names and timings; use this to find a hang
```

## The differential fuzzer

`jaiscript_fuzz` generates programs, runs each on both backends, and reports any disagreement.

```bash
jaiscript_fuzz                         # seeds 0..999
jaiscript_fuzz --iterations 5000       # more programs
jaiscript_fuzz --seed 41337            # one specific program
jaiscript_fuzz --dump 41337            # print that program and stop
jaiscript_fuzz --seed 41337 --minimize # shrink a divergence to its smallest form
jaiscript_fuzz --isolate               # run each program in a child process (survives a crash)
jaiscript_fuzz --vm-reference          # the optimized VM against the VM in reference mode (every optimization off)
jaiscript_fuzz --no-proofs             # with parse-time type proofs disabled
jaiscript_fuzz --no-suppressions       # include already-triaged shapes
```

Exit code 0 means no *new* divergence. The already-triaged shapes are the suppressions in
`known_suppressions()` (`source/tests/fuzz/fuzz_harness.hpp`): each one has an id, a one-line
description, and needles that match the program text and the reported difference. Triaging a
divergence means adding such an entry, with a comment naming the backend at fault; fixing it
means removing the entry and re-running its seeds with `--no-suppressions`. A fixed set of seeds also runs inside `jaiscript_tests` as the "Differential Fuzz
Smoke" suite, so a regression shows up in the ordinary test run.

## Code style

Match the surrounding code. Beyond that:

- **snake_case throughout** — types, functions, members. It reads like the standard library on
  purpose.
- **Tabs in `interpreter.cpp` and `parser.cpp`**; elsewhere, follow the file.
- **No non-const `static` or `thread_local` storage.** State that outlives a call belongs to an
  engine instance. This is what makes multiple engines on multiple threads safe, and one static
  cache silently breaks it. `static const` non-pointer data is fine.
- **A value is made with its engine** — `script_value(v, eng)` stamps the engine's type; a plain value
  names no engine afterwards, so code that mints values takes the engine from its context (a heap payload's
  `payload_engine()`, the backend's engine, a parameter).
- **Integer arithmetic goes through `jai::ints::try_add` / `try_sub` / `try_mul` / `try_div` /
  `mod`**, never raw `a + b` on a `script_int`. The overflow policy is a build-time choice and
  those helpers are where it lives.
- **`operator==` and `operator<=>` must stay consistent** and form a strict weak ordering —
  `script_value` is used as a `std::map` key.
- **Never read a `script_value` after moving from it** — the move constructor nulls the source.
- **Comments explain a non-obvious *why*** — a gotcha, an invariant, a deliberate deviation —
  in one line. Not what the code does, and not what it used to do.
- **Keep heavy headers out of core headers.** `<intrin.h>`, `<format>` and the like do not
  belong on the `value.hpp` path; consumers pay for every one.

Two catalogues move with the code:

- **Error texts** are listed in the "Run-time errors" table of the reference
  ([`docs/site/reference/errors.html`](docs/site/reference/errors.html#run-time-errors)) and must
  stay byte-identical across backends. Change a text, update that table, and check the site for
  output blocks that quote it.
- **The AST format.** A change to what the serializer writes bumps `k_jaibite_version` in
  `include/jaiscript/detail/ast_serializer.hpp`, so stale `.jaibite` caches are
  rejected instead of misread.


## Documentation

The site in `docs/site/` is the specification as well as the documentation: the guide, the
embedding guide and the reference state the rules, and the tests hold the implementation to
them. When the code and the site disagree, one of them is a bug; a change to a rule changes the
site in the same commit.

Every JaiScript sample on the site must run on both backends under strict checking. If you
change a sample, run it:

```bash
jaiscript --backend=vm     sample.jai
jaiscript --backend=interp sample.jai
```

Where a page shows output, paste the real output.

A C++ sample that shows output, and a script sample that needs a host (`data-host`), is checked too. The page marks the
block `data-sample="id"`; the "Doc Samples" suite (`source/tests/integration/doc_samples_tests.cpp`) holds the C++ code
between `// [sample id]` and `// [/sample]` markers, requires the page's text to equal it, runs it and compares what it
prints with the output block after it on the page. A script sample's code is read from the page and run on the host the
test registers. `verify_site.py` runs the suite; with `JAI_DOC_SAMPLES_DUMP=<dir>` set, each run writes its output to
`<dir>/<id>.txt` for pasting.

## Commits and pull requests

A commit message is a title of at most 256 characters. If the change needs more, add a description after a blank line
that says what changed.

Before you open a PR:

- [ ] `jaiscript_tests` and `jaiscript_tests --backend=vm` green in Debug **and** Release
- [ ] `jaiscript_tests --backend=vm-reference` green
- [ ] `jaiscript_fuzz` and `jaiscript_fuzz --vm-reference` green (or the divergence triaged as a suppression in `fuzz_harness.hpp`)
- [ ] the reference's "Run-time errors" table updated if any error text changed
- [ ] docs updated, and any sample you touched re-run on both backends
- [ ] no new `static` / `thread_local` mutable state

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push and pull request.
It builds Debug and Release with three compilers — MSVC on Windows, GCC 14 and Clang on
Ubuntu 24.04 — and each of the six builds runs:

- `jaiscript_tests` and `jaiscript_tests --backend=vm`
- a 300-program differential-fuzz run (`jaiscript_fuzz --isolate`)
- the example hash gates: GLOOM and GRUBWELL (and, in Release, demoreel and Gator Quest) must
  reproduce their reference hashes; the C++ hosts link the library without the interpreter and run the VM, and
  Gator Quest runs on both backends through the runner
- the embedding quickstart's host (`embed_quickstart`), whose checks must all hold
- the language server's protocol tests (`tools/lsp/tests/lsp_test.py`: golden fixtures and the
  Gator Quest checks, plus its edit-latency budget in Release)
- the web host against the runner's HTTP client (`tools/jai_spider/tests/spider_test.py`)
- the runner's own smoke tests (`jaiscript --selftest`) and, in Release, the documentation site
  (`docs/site_tools/verify_site.py`: every sample on both backends)

A separate job runs a cheap `-fsyntax-only` sweep of every implementation file, which catches
MSVC-isms and missing standard includes without waiting for the matrix. The gates live in
[`tools/ci/gates.sh`](tools/ci/gates.sh), so any of them runs locally the way CI runs it:

```bash
BIN=build/bin bash tools/ci/gates.sh tests-vm gloom
```

CI is a safety net, not a substitute for the checklist above.

### Local tiers

Two local tiers wrap the gates. Both read `JAI_DEBUG_DIR` and `JAI_RELEASE_DIR` (default `build-debug` and `build`),
build what they run with `--build`, and end on a one-line verdict.

- **Targeted, for every change.** `tools/ci/targeted.sh <rev-range>` picks the checks from the paths the range touches
  and prints each choice and why; `--plan` stops there. A documentation-site change runs `verify_site`; a test runs the
  Debug suites on the VM and the VM reference; any library change runs the VM tier (those suites, the VM against
  VM-reference fuzz and the example hashes on the VM); a change to the interpreter, the VM's general path or compiler
  front end, a shared `detail/` kernel or the parser/checker adds the interpreter tier (its suites, the interpreter
  against VM fuzz, the hashes on the interpreter). `--lanes` declares a change confined to the VM's optimized lanes and
  `--interp` forces the interpreter tier; the script's header has the rules. An example runs its own gate; prose and design notes run nothing. A
  change to the property system or the archives also prints a note for hosts, whose asset round-trips exercise them.
  A `--build` run on a committed tree leaves a stamp for each gate it passes (`tools/ci/ci_lib.sh` has the format), and
  `--landing`, the mode a landing uses, runs the suites on the Release build and skips any gate stamped for the same tree,
  configuration and toolchain. `LAND_NO_STAMPS=1` runs every gate.
- **Battery, at milestones.** `tools/ci/battery.sh` runs everything at depth: the Debug suites on the interpreter, the
  VM and the VM reference, then on the Release build the suites, a 3000-program run of each differential, every example's hashes (GLOOM also with workers and at 600 ticks,
  and JaiDOOM's timedemo where the checkout has it), the embedding quickstart, the language server and the one-file
  build's include list. Run it before a release and after a large batch of changes, not per change.

A measurement takes the machine-wide timing lock, so builds and suites elsewhere cannot skew it; nothing else needs it:

```bash
bash tools/ci/timing_lock.sh run "me: GLOOM A/B" -- ./my_benchmark.sh
bash tools/ci/timing_lock.sh status
```
