# AGENTS.md — JaiScript

JaiScript is an embedded, C++-like scripting language with hot reload: a static library plus headers, snake_case
throughout. Every script runs on one of two backends held to identical behavior: the bytecode VM (the default, and the
only backend whose speed matters) and the tree-walking interpreter (a plain validator of what the site specifies). Same
results, same printed output, same error text, byte for byte.

`CONTRIBUTING.md` is the human workflow and `docs/BUILD.md` the build reference; this file is the short version plus
the rules a change must not break.

## The language contract is the site

`docs/site/` (guide, embedding guide, reference, stability) is the specification. Before changing behavior, read the
page that states the rule; change the page in the same change. When code and site disagree, one of them is a bug.
Error texts are catalogued in the reference's "Run-time errors" table and must match it.

## Build

```bash
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B build       -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-debug --target jaiscript_tests
```

On Windows run these from a Visual Studio developer prompt (`VsDevCmd.bat -arch=x64`). Iterate in Debug: no LTCG, so
links take seconds, and MSVC's freed-memory fill turns a dangling reference into a crash. Finish with a Release pass;
optimizer-dependent bugs show up only there. Other targets: `jaiscript` (library), `jaiscript_runner` (binary
`jaiscript`), `jaiscript_fuzz`, `jaiscript_lsp`, `jai_gloom`, `jai_crawler`, `jai_demoreel`.

The interpreter (`source/interpreter/`) is compiled only into the programs that compare the backends: they link
`jaiscript_interpreter` (option `JAISCRIPT_INTERPRETER`), which is `jaiscript` plus the interpreter. That is the suite,
the fuzzer, the runner (verify_site runs every sample on both backends through it) and jai_spider (its `--backend interp`
and its gate); every other program, a host application
included, links `jaiscript`, where `set_backend(backend_type::interpreter)` throws. Code both backends use (the builtin
method registries in `builtin_methods.cpp`, include resolution) never lives in the interpreter's files.

## Test

```bash
build-debug/bin/jaiscript_tests                 # interpreter
build-debug/bin/jaiscript_tests --backend=vm    # VM
jaiscript_tests "Script Class"     # bare pattern is ADDITIVE: matching suites (all their tests) OR matching test names
jaiscript_tests "Suite.Test"       # dot form is the strict AND
jaiscript_tests --verbose          # per-test names and timings (finds a hang)
```

A new test `.cpp` needs a CMake reconfigure (the list is a configure-time `file(GLOB_RECURSE)`); a test added to an
existing file needs nothing. Reproduce a bug as a failing test first, watch it go red, then fix it.

```cpp
#include <jaiscript/testing/foundry.hpp>
#include <jaiscript/core/engine.hpp>
namespace jai::foundry::tests {
class my_tests : public suite {
public:
	my_tests() : suite("My Suite") {}
	void forge_tests() override {
		test("two_plus_two", [this]() {
			auto e = make_engine();                               // honors --backend=
			check_eq((int64_t)4, e->execute("2 + 2").as_int());  // check_eq(expected, actual)
		});
	}
};
}
using my_tests = jai::foundry::tests::my_tests;   // FOUNDRY_REGISTER is at global scope: alias first
FOUNDRY_REGISTER(my_tests)
```

`make_engine()` applies the runner's backend and sets `import_recheck_interval(0)`; `engine::make()` gives the default
(VM) engine in both suites and keeps the 0.2 s import window. A recoverable parse error makes `execute()` return null
rather than throw, so "deep nesting does not crash" is tested as "execute returns and the engine still works".

## Gates a change must pass

- `jaiscript_tests` and `jaiscript_tests --backend=vm`, green in Debug **and** Release.
- The differential fuzzer: `jaiscript_fuzz --isolate` reports no new divergence (triage one as a suppression in
  `source/tests/fuzz/fuzz_harness.hpp`, naming the backend at fault; fixing it removes the entry).
- The example hash gates and the language-server tests, exactly as CI runs them:
  `BIN=build/bin bash tools/ci/gates.sh all` (or name gates: `tests-vm gloom crawler demoreel gatorquest lsp`).
- A library or header change: `bash tools/ci/gates.sh android` cross-compiles the library and the suites with the
  Android NDK (libc++, the standard library Apple platforms use, and 32-bit armeabi-v7a); it skips when no NDK is
  installed. It runs by hand: targeted.sh never picks it.
- `bash tools/ci/gates.sh wasm` builds the engine with Emscripten (`tools/jaiscript_web`, a 32-bit libc++ target that runs) and
  plays the GLOOM, crawler and demoreel shims under Node to the native gates' hashes; it skips when no Emscripten SDK
  is found, and targeted.sh picks it for a library change. `bash tools/ci/gates.sh headers` compiles every public header alone and
  `tools/ci/host_warnings.cpp` under `-Wall -Wextra -Werror` with each g++ and clang++ it finds, and wants each case of
  `tools/ci/must_not_compile.cpp` (a program the headers reject) to fail with its message; targeted.sh picks it for
  any `include/jaiscript` change.
- A compiler change: dump `jaiscript --manifest=<file> <script>` before and after over the affected programs; the
  manifests (every chunk field, exact constant bits) change only where the change intends.
- A change to what the AST serializer writes bumps `k_jaibite_version` (`include/jaiscript/detail/ast_serializer.hpp`).
- `tools/ci/targeted.sh --build <base>..HEAD` on a committed tree writes a stamp for each gate it passes, keyed by the
  tree, the gate, its configuration and the toolchain (`tools/ci/ci_lib.sh`). A landing runs `targeted.sh --landing`,
  which skips every gate stamped for the tree it lands and runs the suites on the Release build only;
  `targeted.sh --build --landing <base>..HEAD` in a worktree stamps those Release suites as well.
- Docs: `python docs/site_tools/verify_site.py --runner <bin>/jaiscript` (links, structure, leakage, retired error texts,
  and every sample run on both backends under strict). A sample shows its real output.

## Code rules

- No non-const `static` or `thread_local` storage. State that outlives a call belongs to an engine; one static cache
  breaks multiple engines on multiple threads. `static const` non-pointer data is fine.
- A value is made with its engine (`script_value(v, eng)` stamps the engine's type) and names none afterwards: code that mints
  values takes the engine from context (a heap payload's `payload_engine()`, the backend's `engine_`, a parameter).
- `operator==`, `operator<=>` and `map_key_hash` stay consistent: `<=>` is a strict weak ordering (map iteration and
  serialization order) and equivalent keys hash alike. Float keys order totally (a NaN key is found again; `0.0` and
  `-0.0` are one key); complex values order and hash by held-pointer identity, except a class value map key, which keys
  by its class's `<`/`<=>`, else its `hash()` with `==`, else by identity (`core/script_map.hpp`).
- `jai::engine`'s public members are the host API the site documents. Library code, the tools and the tests reach every
  other member through `detail::engine_access` (`detail/engine_access.hpp`): a new internal member is private and gets
  its inline forwarder there; a new public one gets its page.
- Never read a `script_value` after moving from it (the move constructor nulls the source).
- Integer arithmetic goes through `jai::ints::try_add/try_sub/try_mul/try_div/mod`, never raw `a + b` on `script_int`:
  the overflow policy (checked by default, `JAISCRIPT_WRAP_ON_OVERFLOW` wraps) lives there, and arithmetic is never UB.
- Error text is built only on the failing path (a cold, non-inlined helper), never eagerly on a hot path, with bytes
  identical to the catalogued text.
- Code marked `KEEP BYTE-PARALLEL` exists in both the interpreter and the VM: change both sides together. A rule both
  backends need lives once, in a shared kernel under `include/jaiscript/detail/`; writing the same rule twice is the
  signal to add a kernel.
- Comments state a non-obvious *why* in one line: no narration, no history.
- Match the file: `interpreter.cpp`, `parser.cpp`, `lexer.cpp` and `engine.cpp` indent with spaces, the VM's files with tabs;
  otherwise follow the surrounding code.

### Header hygiene (consumers pay for every include)

- Binding instantiations, not header parsing, dominate consumer compile time: keep binder thunks and container
  conversions out of headers that do not need them (schema-driven primitive property binders live out of line in
  `schema_binding.cpp`).
- A binding pays only for what is particular to it. Work that does not depend on the bound type (class and instance
  creation, `class_definition` registration, the `Signal` view, the stdlib) is defined in the library, not in a header.
  A stored callable is one erased type over a static function (`detail::host_method_call`, registry
  `conversion_entry`), never a fresh lambda inside a `std::function`; a bound type's `std::vector`/`std::map` shapes
  register eagerly and compile only where a caller names them. Measure a change by object size per bound item.
- What a host only calls into keeps its containers in the library: a template's state that does not depend on its
  parameter lives in a non-template base defined in a `.cpp` (`archive_writer_core`, `archive_reader_core`), and the
  process-wide registries (`type_registry`, `type_name_registry`, the registrar order) define their members out of line.
  A header member body a host would compile once per file is a candidate to move; count it by the functions a host
  object file defines (`dumpbin /symbols`), not by reading the header.
- `<chrono>` reaches a host only through `core/engine_time.hpp`; iostreams and `<charconv>` stay in the library's `.cpp`
  files. `stdlib/stdlib.hpp` declares the registrations and includes no module's internals.
- Hosts never parse the runtime. `core/class_definition.hpp` declares the class runtime; `detail/class_runtime.hpp`
  defines its member bodies and slot/assignment kernels inline for the library's own translation units (and tests), and
  `jaiscript.hpp` is the core, the binders and the archives, never the parser or interpreter. A member a host-visible
  template calls stays defined in `class_definition.hpp` (or out of line in the library); every other runtime member is
  declared `inline` there and defined in `class_runtime.hpp`, so GCC and Clang report a host-side use as an inline
  function used but never defined. Check a header change by compiling a host file (an example) with GCC.
- No lambdas in a constructor template's member initializers or a class template's default member initializers: MSVC
  numbers those per translation unit, so two units can give one lambda name different bodies and Release calls the wrong
  one. Use named static functions (`archive_reader_thunks` / `reader_operations_of`).
- `signals/signal_decl.hpp` does not include `signal_impl.hpp`; headers that only declare signal members include
  `signal_decl.hpp`, and every `.cpp` that emits includes `signal.hpp` (or `signal_impl.hpp`): an emit instantiates the
  script-receiver path, so `signal_decl.hpp` alone leaves it unresolved at link time.
- `checked_result.hpp` does not include `<format>`; error messages use plain `{0}`/`{1}` substitution.
- `detail/integer_ops.hpp` (and its `<intrin.h>`) stays out of `interpreter.hpp`. Keep `<intrin.h>`, heavy standard
  headers and implementation headers out of widely included core headers.
- Walks over script data (JSON parsing, `from_json`/`from_binary`, `to_json`/`to_binary`, deep copy, the parallel detach,
  memberwise `==` and `<=>`) run on explicit work stacks, so data nests as deep as memory allows; no depth cap stops a
  legitimate payload, and a deep copy's steps run in the order the native walk takes them. A walk that recurses through host
  code (a host type's `serialize`/`save`/`load`) keeps a guard on save and on load: `detail::native_stack_low()` plus the
  `JAI_MAX_HOST_RECORD_DEPTH` backstop (1024). Every recursive walk over source (the parser, the checker, the slot and
  marking passes, the VM compiler, the interpreter's evaluation) asks its `detail::native_stack_mark` at each level and stops
  with the one nesting error before the native stack overflows, in a Debug build too; no count of levels stands in for it.

## Design principles

- C++ semantics are the contract: lifetimes, copies, references, evaluation order, conversions. The exception is a
  static-typing artifact such as slicing; there the dynamic choice wins (a subclass stored into a base slot keeps its
  class).
- Both backends stay at parity on what the site specifies. A change that satisfies one backend is not done.
- The VM may take any shortcut nobody can observe. The interpreter is a plain evaluator: a change shrinks its fast paths
  rather than adding to them, and never holds a VM win back.
- A simplification lands only when it is behavior- and error-text-identical, and no slower.
- Uplift the general path rather than special-casing a shape: a fast path that only one spelling reaches is suspect,
  and operator overloads and dynamic values must keep working on every path.
- Move work to compile time and cut accidental copies before anything else.
- JaiScript is a package; host applications and the examples are consumers. Consumer-shaped code in the library is
  suspect.

## Architecture

```
include/jaiscript/  core/ (value, engine, dynamic_binder, class_definition)  detail/ (lexer, parser, AST, shared kernels)
                    vm/ (opcodes, compiler, backend, disassembler)  stdlib/  signals/  properties/  serialization/  testing/
source/implementation/   library .cpp files (one TU each)       source/tests/   Foundry suites, fuzz/
source/interpreter/      the tree-walking interpreter (jaiscript_interpreter only)
tools/   jaiscript (runner), lsp, vscode-jaiscript, ci/gates.sh
examples/   GLOOM, GRUBWELL (crawler), demoreel, Gator Quest, JaiDOOM, embed_quickstart
docs/    BUILD.md, site/ (the specification), site_tools/
```

Pipeline: lexer → parser (recursive descent, AST) → the static checker (always strict, `[[warn_mode]]` the per-region
opt-out; it also stamps type proofs both backends consume behind runtime guards) → one `execution_backend`: `jai::vm` or
the interpreter, chosen per engine with `set_backend` before the first execute. Builtin container and string methods live in one registry both backends
share. `script_value` is a 16-byte type-erased value with `strong_ptr`-wrapped heavy types; `clone()` is a deep copy,
copying is shallow. Interned symbol IDs are engine-bound: never share or serialize them across engines.

## Implementation notes

A new opcode joins both env safe-lists in `vm_compiler.cpp` and, if it names a slot, the debugger's locals walk.
The code and the site are the reference for everything else.

## Commits

A commit message is a title of at most 256 characters; a change that needs more adds a description after a blank line
(what changed, not why). No trailers.
