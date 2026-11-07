# embed_quickstart

A small C++ host that uses JaiScript the way a game would: it binds C++ classes, runs `scripts/game.jai`,
calls script functions from C++, emits signals into script, reads script state back, saves and loads through
the JSON and binary archives, and swaps `rules_v1.jai` for `rules_v2.jai` mid-run to hot-reload an import. It
checks every section and exits 0 when all of them hold.

## Run it

From the JaiScript source root:

```bash
cmake -S examples/embed_quickstart -B build/quickstart -DCMAKE_BUILD_TYPE=Release
cmake --build build/quickstart --config Release
cd build/quickstart && ./embed_quickstart
```

With the Visual Studio generator the program is `build/quickstart/Release/embed_quickstart.exe`. The hot-reload
section writes `live/rules.jai` under the working directory, which is why it runs from `build/quickstart`.

| flag | what it does |
|---|---|
| `--debug` | also listen for VS Code (DAP) on 127.0.0.1:52472 |
| `--debug-wait` | listen, then wait for Enter before running `game.jai` |

`CMakeLists.txt` builds JaiScript from source with `add_subdirectory`. To use an installed package instead,
configure with `-DEMBED_QUICKSTART_FIND_PACKAGE=ON -DCMAKE_PREFIX_PATH=<prefix>` (see `docs/BUILD.md`).
