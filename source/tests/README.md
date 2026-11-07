# JaiScript tests

Every suite here is a Foundry suite (`include/jaiscript/testing/foundry.hpp`), compiled into one
executable, `jaiscript_tests`, whose `main` is `main_test_runner.cpp`.

## Layout

| folder | what it covers |
|---|---|
| `language/` | language features: functions, classes, control flow, coroutines, hot reload, imports, privileges, regressions |
| `semantics/` | conversions, typed slots, value semantics, the lexer, parser and static checker, strong types |
| `containers/` | arrays, maps and their builtin methods |
| `stdlib/` | `math::`, strings, `time::`, `binary::`, reflection, I/O |
| `vm/` | bytecode-VM-specific behavior and lowering |
| `integration/` | C++ bindings: `dynamic_binder`, overloads, host references, re-entrant callbacks |
| `serialization/` | archives, polymorphic serialization, `.jaibite` round trips and validation |
| `signals/`, `properties/` | the signal system and observable properties |
| `core/` | `strong_ptr` |
| `debug/` | the DAP debugger |
| `foundry/` | the framework's own suite |
| `performance/` | benchmarks, compiled only with `-DJAISCRIPT_ENABLE_BENCHMARKS=ON` |
| `fuzz/` | the differential fuzzer `jaiscript_fuzz` (its own executable) and the seeded "Differential Fuzz Smoke" suite that joins `jaiscript_tests` |

## Writing a test

A suite registers itself; nothing lists it by hand:

```cpp
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>

namespace jai::foundry::tests {
class my_tests : public suite {
public:
    my_tests() : suite("My Suite") {}
    void forge_tests() override {
        test("two_plus_two", [this]() {
            auto e = make_engine();                                // honors --backend=
            check_eq((int64_t)4, e->execute("2 + 2").as_int());   // check_eq(expected, actual)
        });
    }
};
}

using my_tests = jai::foundry::tests::my_tests;   // FOUNDRY_REGISTER is at global scope
FOUNDRY_REGISTER(my_tests)
```

Create engines with `make_engine()` so the runner's backend choice reaches the test. Add tests to
an existing file when you can: the test list is a `file(GLOB_RECURSE)` evaluated at configure
time, so a **new** `.cpp` needs a CMake reconfigure before it builds.

A test never asserts an absolute time. Count the work instead (calls, migrations, parses, bytes
moved). Where speed itself is the point, `compare_speed` (`include/jaiscript/testing/timing.hpp`)
times the measured side against a reference operation, interleaved in the same process, and
prints the median ratio. `check_speed_ratio` fails only past a bound that marks a regression
(a lost complexity class, a fast path gone) with room for load. A wait for what must happen is
a hang rail: `stall_watch` trips once a progress reading stands still for `k_hang_guard`.

## Building and running

```bash
cmake --build build --target jaiscript_tests
build/bin/jaiscript_tests                  # tree-walking interpreter
build/bin/jaiscript_tests --backend=vm     # bytecode VM
```

Both runs must be green. Filtering:

```bash
jaiscript_tests "Coroutine"          # additive: every matching suite (all its tests) OR every matching test name
jaiscript_tests "Suite.Test"         # the dot form is the exact AND: suite filter AND test filter
jaiscript_tests --verbose            # per-test names and timings; use it to find a hang
```

A crash prints the suite and test it hit to stderr, verbose or not; on Windows `JAI_CRASH_DUMP_DIR=<dir>` also writes a
minidump there. `--gtest_filter=`, `--jaitest_filter=` and `--filter=` take the same patterns. Build directories,
generators and the CTest view are in [`docs/BUILD.md`](../../docs/BUILD.md); the contribution
workflow is in [`CONTRIBUTING.md`](../../CONTRIBUTING.md).
