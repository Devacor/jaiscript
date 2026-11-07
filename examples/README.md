# Examples

Every example runs from the JaiScript source root. Build the runner once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target jaiscript_runner
```

Then each game is one command (on Windows the runner is `build\bin\jaiscript.exe`):

| example | what it is | run it |
|---|---|---|
| [crawler](crawler/README.md) | GRUBWELL, a first-person step dungeon crawler | `build/bin/jaiscript examples/crawler/scripts/crawler.jai` |
| [demoreel](demoreel/README.md) | a seventeen-scene demoscene reel on two live engines | `build/bin/jaiscript examples/demoreel/demoreel.jai` |
| [gloom](gloom/README.md) | GLOOM, a real-time terminal raycast shooter | `build/bin/jaiscript examples/gloom/gloom.jai` |
| [gloom_idiomatic](gloom_idiomatic/gloom_idiomatic.jai) | GLOOM rewritten in class-based style, bit-exact with the original | `build/bin/jaiscript examples/gloom_idiomatic/gloom_idiomatic.jai` |
| [gatorquest](gatorquest/README.md) | Gator Quest, a terminal RPG with netplay | `build/bin/jaiscript examples/gatorquest/scripts/gatorquest.jai` |
| [jaidoom](jaidoom/README.md) | JaiDoom, a DOOM-format engine in script (the first run fetches Freedoom; a fan project, not affiliated with or endorsed by id Software) | `build/bin/jaiscript examples/jaidoom/scripts/jaidoom.jai` |
| [embed_quickstart](embed_quickstart/README.md) | a minimal C++ host: bindings, calls, signals, hot reload | see its README (a C++ build) |

The crawler, the reel and GLOOM also have native hosts (`jai_crawler`, `jai_demoreel`, `jai_gloom`), built the
same way and run from `build/bin`. Use a terminal with 24-bit color and UTF-8 (Windows Terminal on Windows), and
play the Release builds.
