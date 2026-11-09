# JaiScript for VS Code

Language support for [JaiScript](../../), an embedded scripting language for C++ hosts.

## What it does

- **Language server** - the extension launches `jaiscript_lsp` (`tools/lsp`, the
  `jaiscript_lsp` CMake target) for every `.jai` file. It answers from the engine's own parser and
  strict checker: diagnostics as you type across the whole program (an error in an imported file is
  reported in that file, at its line), go to definition, find references, hover (declared and
  inferred types, signatures, class fields), completion (scope, members after `.`, namespaces and
  enums after `::`, builtin container and string methods, host functions), signature help, rename
  (refused with the reason when it cannot be proven complete), document and workspace symbols.
- **Syntax highlighting** - TextMate grammar for `.jai` files.
- **Attach debugging** - breakpoints, stepping, call stack and variables over the Debug Adapter
  Protocol. The DAP server lives inside the C++ host (`jai::debug::debug_connector`, a raw TCP port); this
  extension points VS Code's DAP client straight at that socket, with nothing to spawn.

The full guide is the docs page `docs/site/embedding/14-language-server.html` (and
`12-debugging.html` for the debugger).

## Install

`out/extension.js` is a single bundled file (the language client included), so the folder runs as
it is:

```
code tools/vscode-jaiscript        # F5 opens an Extension Development Host
```

or link it into your extensions folder
(`mklink /J "%USERPROFILE%\.vscode\extensions\snapjaw.jaiscript" tools\vscode-jaiscript` on
Windows), or package it: `npm install`, `npx @vscode/vsce package`, then
`code --install-extension jaiscript-0.5.2.vsix`.

Build the server like any other target, `cmake --build build --target jaiscript_lsp`; the binary
lands in `build/bin/` (see `docs/BUILD.md`). The extension looks for it in this order: the
`jaiscript.languageServer.path` setting, `server/jaiscript_lsp` inside the extension folder, the
checkout's `build/bin/` and `build-debug/bin/` (the CMake presets), the Visual Studio Open Folder build trees
(`out/build/<config>/bin/`), then `PATH`.

## Settings

| setting | default | meaning |
|---|---|---|
| `jaiscript.languageServer.enabled` | `true` | run the server |
| `jaiscript.languageServer.path` | empty | the server executable (`${workspaceFolder}` expands) |
| `jaiscript.languageServer.host` | `runner` | registrations scripts are checked against: `runner` (the jaiscript runner's), `stdlib`, `bare` |
| `jaiscript.languageServer.includePaths` | `[]` | include paths for `import` / `include` |
| `jaiscript.languageServer.log` | `false` | log requests and timings to the output channel |

*JaiScript: Restart Language Server* restarts it; a settings change does too.

## Attach debugging

Run the standalone runner with `--debug`: `jaiscript --debug game.jai` logs
`debugger: DAP listening on 127.0.0.1:52477`; without `--debug` it opens no socket. A host of
your own listens with

```cpp
auto conn = jai::debug::listen(*eng);   // port jai::debug::default_port, 52472
```

Add a `.vscode/launch.json` attach configuration with the port the process logged (the
extension's default is the runner's, 52477) and start it while the script runs:

```json
{
  "version": "0.2.0",
  "configurations": [
    { "type": "jaiscript", "request": "attach", "name": "Attach to JaiScript", "host": "127.0.0.1", "port": 52477 }
  ]
}
```

Breakpoints key on the original `.jai` source path, so they bind whether a unit was loaded from
source or from a pre-parsed `.jaibite`. The host must be built with `JAISCRIPT_ENABLE_DEBUGGER`.

## Building the extension

```
npm install
npm run compile   # type check, then bundle src/extension.ts -> out/extension.js
```
