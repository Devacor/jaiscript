# JaiDoom

A playable DOOM engine written in JaiScript: the WAD loader, BSP renderer, playsim, HUD,
sound and music decoding, savegames, demos and 16-player netplay are all script. It draws
to the terminal with half-block glyphs and 24-bit colour.

JaiDoom is a fan project, not affiliated with or endorsed by id Software; DOOM is a trademark of id Software.

## Play

1. Build the runner (see `../../docs/BUILD.md`), or drop `jaiscript.exe` next to `play.bat`.
2. Double-click `play.bat` (Windows) or run `./play.sh` (Linux, macOS, Git Bash).

The first run fetches the game data itself: Freedoom Phase 1 and 2 from the Freedoom
project's release, and, if you say yes, the DOOM shareware episode from a pinned mirror.
Every file is verified by SHA-256 before it is kept, the archive is unpacked by the runner,
and the sound and music lumps are decoded into `cache/` so play starts with audio. Nothing
game-related is committed to the repository; `wads/SOURCES.txt` records where each file
came from and under which license. The registered DOOM and DOOM II data are commercial:
put your own `doom.wad` or `doom2.wad` in `wads/` and pass it on the command line.

Headless, for scripts and CI: `bootstrap.bat` or `./bootstrap.sh`, with `--no-shareware`
to stay entirely on BSD-licensed data and `--no-audio` to skip the cache.
`delete_downloaded_bootstrap.bat` / `.sh` removes everything the bootstrap produced.

## Controls

```
move: W/S or up/down     strafe: A/D      turn: left/right arrows
run: hold shift (Windows) or R to toggle  use: E or space   fire: ctrl or F
weapons: 1-5   automap: tab or M (zoom -/=, B mark, C clear)   quicksave Z  quickload X
menu: esc      cheats: iddqd idkfa idfa idclip idclev## idbehold iddt
frame rate: U toggles uncapped (the game still ticks at 35 Hz; the camera eases between tics)
```

Start uncapped with `jaiscript jaidoom.jai -- uncapped`; the FPS counter in the top-right corner shows what your
machine and terminal sustain.

## Levels and your own WADs

The menu's LEVEL SELECT lists every map the loaded WADs contain and starts the one you
pick at the skill you choose. Further WADs named on the command line merge over the first:
a map pack's levels and any resources it replaces take effect, everything else comes from
the base. A map pack starts on its first map. Between levels the game follows DOOM's own
chain, including the secret exits (E1M3 to E1M9, MAP15 to MAP31, MAP31 to MAP32), and when
the standard next map is not in the loaded WADs it continues with the next map that is,
or ends the run when nothing follows.

## Command line

```
jaiscript jaidoom.jai                            # play.bat does this; dropping jaidoom.jai onto jaiscript.exe works too
jaiscript jaidoom.jai -- freedoom2 MAP01         # another IWAD and map (MAPxx names work)
jaiscript jaidoom.jai -- doom2 MAP07             # your own registered WAD from wads/ (a path works as well)
jaiscript jaidoom.jai -- doom2 mypack.wad        # a map pack merged over it
jaiscript jaidoom.jai -- record demo.json        # demos: record, then
jaiscript jaidoom.jai -- playdemo demo.json
jaiscript jaidoom.jai -- host [port] [coop]      # netplay: host (deathmatch or co-op)
jaiscript jaidoom.jai -- join <host> [port]      #          join
jaiscript jaidoom.jai -- bootstrap               # fetch the data without the screen
jaiscript p10_timedemo.jai                       # the deterministic benchmark (prints a state hash)
jaiscript shots.jai -- fight 105 tics 6          # the docs page's frames as .ans files (title.ans, fight.ans)
```

## Licenses

The scripts are part of JaiScript and carry its license. Freedoom is BSD 3-Clause
(`wads/COPYING.txt`, https://freedoom.github.io). The DOOM shareware episode is
Copyright 1993 id Software; it may be redistributed unmodified and is not open source.
The engine was written from the public WAD and engine format documentation; no id
Software source is transcribed.
