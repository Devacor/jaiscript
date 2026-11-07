# jai_demoreel — a demoscene tech reel written in JaiScript

Seventeen auto-advancing truecolor scenes, five kinds of transition, pixel-font
title cards, and a finale where the word JAISCRIPT ignites out of a wall of
rainbow fire, cools to neon and rolls the credits — every effect coded in `scenes/*.jai`, every frame drawn at
**double vertical resolution** with half-block pixels. A ray tracer, a sphere
tracer, a Menger-sponge raymarcher, a textured Wolfenstein raycaster, a
VoxelSpace terrain flight, Verlet cloth, metaballs, a falling-sand toy, a
murmuration, a texture tunnel, a skull-headed spider crawling a morphing gyroid
labyrinth and the classics. The C++ host (`demoreel_host.cpp`) is a
thin shell: a VT console, a monotonic clock, non-blocking keys, a seeded rng
class, and the frame pump. Everything you see is script.

The reel doubles as a determinism test: `--smoke` hashes the whole frame stream
against a reference. Six scenes trace their rows across every core with
`parallel_transform`, and the stream stays byte-identical anyway.

## How a frame is drawn

Each scene paints a `w x 2h` grid of palette indices (square pixels, two per
terminal row) and the presenter in `util.jai` packs them into cells through the
upper-half-block glyph `▀`: the **foreground** truecolor escape paints the top
pixel, the **background** escape the bottom one (JaiDoom's `vid_row_half`
technique). Escapes are emitted only when a color changes along the row, and a
cell whose two pixels agree collapses to a space (a background run) or a full
block (a foreground run), so smooth regions cost one byte per cell. Palettes
precompute both escape strings per entry; a transition simply concatenates the
two scenes' tables (the incoming scene's indices shift by an offset) and the
reel's own UI entries ride at the end. `hb_row` is written as a
`parallel_transform` body over captured globals, so the same function serves
the serial loop and, with `HB_WORKERS` set (the standalone runner does on wide
terminals), a parallel presentation.

Text stays crisp on top: the HUD, the scene handle under each title card and
the finale's credits are ordinary glyph rows; the big title letters themselves
are pixels from a 5x7 font (`FONT5`), typed in with a shine sweeping across.

## Run it

From the JaiScript source root, build the runner once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target jaiscript_runner
```

then:

```bash
build/bin/jaiscript examples/demoreel/demoreel.jai
```

(On Windows the runner is `build\bin\jaiscript.exe`.) The native host is the `jai_demoreel` target: `cmake --build build --config Release --target jai_demoreel`, then
`build/bin/jai_demoreel`.

## Build

Part of the JaiScript CMake tree (target `jai_demoreel`):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target jai_demoreel
```

The exe prefers the source `scenes/` dir (so hot reload edits the real files),
falling back to `demoreel_scenes/` copied next to the binary.

### No build: the standalone runner

`demoreel.jai` is a pure-script shim that recreates the C++ host contract (the
`ESC` global, `itrunc`/`ifloor`, and a bit-exact script port of the seeded
xorshift64* `Rng` — checked-int64-safe via 16-bit-limb multiplies) and pumps the
reel on the standalone runner — drag-drop it onto `jaiscript.exe` or:

```
jaiscript demoreel.jai                       # the reel, 30 fps, with its soundtrack (q/esc quits; h/f/n/p/space/digits work)
jaiscript demoreel.jai -- color 256          # the reel quantized to the xterm-256 cube (color 24 forces 24-bit)
jaiscript demoreel.jai -- ascii 1            # one density character per cell (no block glyphs)
jaiscript demoreel.jai -- frames 1700        # headless self-test: smoke frames, sizes + stream hash + timing
jaiscript demoreel.jai -- frames 60 dump P   # also write frames 0/N-2/N-1 to P_<i>.txt (+ P_<i>.bmp, the pixel grid)
jaiscript demoreel.jai -- bench 60           # every scene mid-flight: script ms/frame and KB/frame
jaiscript demoreel.jai -- shot 14 at 4 dump P   # one frame of scene 14 at scene time 4 -> P.txt (as presented) + P.bmp
jaiscript demoreel.jai -- midi song.mid      # write the soundtrack and exit
jaiscript demoreel.jai -- music 0            # the reel, silent
```

The soundtrack is composed by the script at boot: a Standard MIDI File (64 bars
at 124 bpm in A minor over i-VI-III-VII — pumping synth bass, warm pad, a saw
arpeggiating sixteenths, a square lead improvising on the pentatonic from the
reel's seed, a four-on-the-floor kit) written to the app cache dir and looped
through the runner's `play_sound`.

Verified: the 1700-frame smoke reel produces the same stream hash and
byte-identical dumped frames under `--backend=vm` and `--backend=interp`.
The shim picks its color depth as `jai_demoreel` does, through the runner's
`env_var`: 24-bit under `COLORTERM=truecolor` (or `24bit`) and in Windows Terminal
(`WT_SESSION`), the xterm-256 cube elsewhere; `color 24`, `color 256` and `ascii 1`
override it. Not ported
from the C++ host: its escape trimming and row-diff redraw (frames present
whole) and `r` hot reload.

On Windows the runner sets the console to UTF-8 and VT output. q, ESC and
Ctrl+C all end the reel with the cursor, the colors and the main screen
restored, on every platform, as `jai_demoreel` does.

## Run

```
jai_demoreel                     # the reel, 30 fps pacing
jai_demoreel --ascii             # one density character per cell, for terminals without block glyphs
jai_demoreel --256color          # force the xterm-256 cube (--truecolor forces 24-bit)
jai_demoreel --smoke             # headless: frame-stream hash + perf
jai_demoreel --smoke --frames N  # smoke frame budget (default 1700)
jai_demoreel --capture I         # print scene I as plain text (ANSI stripped: half blocks and spaces)
jai_demoreel --capture I --ascii # the same frame as a density map
jai_demoreel --reload-test       # headless hot-reload self-test
jai_demoreel --precompiled       # boot from demoreel.jaibite (saved on first run)
```

Keys while running:

| key | action |
| --- | ------ |
| `r` | hot-reload every `scenes/*.jai` from disk |
| `h` | toggle the HUD: a status bar (scene, backend, frame ms split into sim and draw, fps, pixel size) and a key bar that fills as the scene plays |
| `f` | freeze the clock (render keeps running) |
| `n` / `p` / space / arrows | next / previous scene |
| `1`..`9`, `0` | jump straight to a scene (`0` = scene 17, the finale) |
| `q` / `ESC` | quit |

The HUD shows the backend and its frame cost. Ctrl+C, Ctrl+Break and SIGTERM
end the reel through the same exit as `q`: the cursor, the colors, the
main screen and (on Linux and macOS) the terminal's input mode come back.

## The scenes

| # | scene | what it is |
| - | ----- | ---------- |
| 1 | PLASMA | four summed sine fields over a 256-entry cyclic palette, integer table lookups per pixel; the radial term reads a double-size distance table through a sliding window, so the ripple's center orbits at zero per-pixel cost |
| 2 | STARFIELD | a warp run: 700 stars in four color temperatures drawn as motion streaks (tail dim, head bright, near stars bloom) over a periodic value-noise nebula; the palette is ADDITIVE (nebula level x star light), the view rolls, and the ship spools up, jumps to warp and drops out in a flash |
| 3 | TUNNEL | the table-driven texture tunnel: atan2 and 1/r baked per pixel of a double-size table, a frame is four integer lookups per pixel; the vanishing point wanders (sliding window), the walls twist with depth, bevelled riveted plates with racing neon rings dissolve into the glow at the tunnel's end |
| 4 | DONUT | the torus homage grown up: sphere-traced per pixel on the torus distance field in the donut's own frame, exact normals, diffuse + Blinn specular, pink icing with a wavy drip edge and sprinkles on a hashed (phi, theta) grid; rows in parallel |
| 5 | JULIA | morphing Julia set (c orbits the cardioid) at full resolution, continuous escape-time coloring (log-log fractional count, no bands) into a cycling 256-entry palette, orbit-trap shading inside the set; rows in parallel |
| 6 | LAVALAMP | eight metaballs: one loop sums the inverse-square field, its gradient and a contribution-weighted hue, so merging blobs blend colors; the wax is lit from a normal built out of the gradient (the height of a blob under the field), with a tinted glow outside; rows in parallel |
| 7 | PIPEDREAM | the 3D Pipes screensaver homage: four pipes grow one segment per tick through a 16x12x16 voxel volume, never intersecting; every tube is a chain of lit sphere impostors a quarter voxel apart (per-pixel normal, specular, fog, depth nudge so crossings intersect), glossy elbow balls at every turn, a static camera per generation, CRT wipe, reseed from a new angle; ~2% of elbows spawn the jackodile's rainbow eye |
| 8 | POWDER | falling-sand toy at full pixel resolution, two substeps a frame: sand / water / ember / plant / smoke, each element a script class; water levels out (up to four cells of sideways flow a step), surfaces catch the light, water darkens with depth, embers glow onto the air around them |
| 9 | BOIDS | a starling murmuration at sunset: 80 boids (script class instances) and a flapping hawk as silhouettes against a baked sky (gradient, sun glow, a lit skyline); every palette entry is a sky tone darkened by a level, so birds and their fading wakes shade whatever sky they cross |
| 10 | KINSTEIN 3D | Wolfenstein-style DDA raycaster over an authored 24x24 keep, TEXTURED: coursed stone, mossy stone, fluted gold and a banner of animated rainbow fire (16x16 textures grown in script), floor and ceiling cast per pixel, distance fog; billboard Grublins and Orglis brutes lit as spheres per pixel, with eyes and grins, wall-occluded per column; the camera auto-fires on line-of-sight cone lock — recoil, muzzle flash, rainbow tracer, hit-flash, gib scatter, a pixel-font kill counter |
| 11 | CANYONRUN | the Comanche (1992) VoxelSpace algorithm: a 128x128 wrapping heightmap by diamond-square, a river canyon carved along a sine meander, four levels of sun-side slope light plus hashed grain baked into a packed color/height map; 72 geometric depth steps of 16.16 fixed-point marching with a per-column y-buffer, ten haze banks, a sun that tracks the heading, banking into turns |
| 12 | SPONGEWORKS | a raymarched Menger sponge: one ray per pixel, KIFS-fold sphere tracing (no sqrt, no mod), the exact face normal from ONE tracked re-fold, SDF-probe AO, fog, six 16-level face ramps, a 4-phase Bayer temporal update with per-pixel reprojection; rows in parallel |
| 13 | BANNERFALL | Verlet cloth: a 26x11 grid (structural + shear, Jakobsen sqrt-free relaxation, topology as index arithmetic) flying a rainbow weave with the gold JAI sigil; each quad rasterized as two triangles by edge functions at pixel centers from subpixel positions, lit by edge cross products; the hoist seam tears thread by thread, the banner tumbles and bursts into ember scraps |
| 14 | CHROME | the Amiga Juggler's grandchild: a ray tracer, one ray per pixel across every core — three tinted chrome spheres bounce on a checkerboard running to a hazy horizon, one reflection bounce, hard shadows, a sun glint; the ray returns WHAT IT SAW as a class and each sphere looks that up in its own tinted copy of the class table |
| 15 | FIRE | convection fire with the Doom fire's random sideways jitter at full resolution, 73 heat levels, sparks breaking off and drifting up; ignites into RAINBOW fire mid-scene (house signature) |
| 16 | OSSUARY | a horned, three-eyed skull on a spider's body crawls a raymarched field that morphs between three triply periodic surfaces (gyroid, Schwarz P, Schwarz D) under a flowing domain warp; the body re-projects onto the moving level set every frame, eight two-bone IK legs step in a tetrapod gait, three tendrils writhe behind, and the camera orbits with the surface normal as up; a crown of flame under a spinning ruby, embers and orbiting blood gems are depth-tested against the traced scene; rows in parallel |
| 17 | FINALE | the fire scene's subclass: a wall of fire out of which "JAISCRIPT" ignites in double-size pixel letters (9 px glyphs on a 10 px pitch) that are heat sources; each column's fuel swells and ebbs and the air above the word cools fast, so the flames rise in tongues, burning rainbow; then the wall dies, the letters cool to a neon rainbow with a glint sweeping across, and the credits roll up beneath them |

Between scenes: dissolve (hashed per pixel), slant wipe and iris (both with a
glowing seam), blinds, and the Doom screen melt — two grids merged per pixel
into the combined palette. A title card with a demoscene handle opens on every
scene.

## Measured numbers

The whole reel on the standalone runner (100x40 cells = 100x80 = 8,000 pixels
per frame, seed 20260705, fixed dt 1/30, transitions included, smoke durations
of 3 s per scene) reports its own timing: `jaiscript demoreel.jai -- frames 1700`
prints the render cost per frame (`demo_frame` alone: update, render and
presentation), the cost including the shim's per-byte script hash of every
frame, and the mean frame size (52 KB). Measure it on a quiet Release build.

Per scene, mid-flight (`-- bench 60`, no transitions, same size):

| scene | VM ms | interp ms | KB/frame |
| ----- | ----- | --------- | -------- |
| PLASMA | 0.8 | 11.3 | 139 |
| STARFIELD | 1.5 | 8.3 | 31 |
| TUNNEL | 0.7 | 8.9 | 63 |
| DONUT | 4.7 | 14.6 | 30 |
| JULIA | 5.0 | 16.9 | 123 |
| LAVALAMP | 2.3 | 18.1 | 51 |
| PIPEDREAM | 1.1 | 10.9 | 56 |
| POWDER | 4.9 | 25.3 | 33 |
| BOIDS | 4.0 | 14.2 | 23 |
| KINSTEIN 3D | 1.2 | 12.6 | 33 |
| CANYONRUN | 0.8 | 12.2 | 22 |
| SPONGEWORKS | 5.8 | 21.2 | 28 |
| BANNERFALL | 2.9 | 18.4 | 21 |
| CHROME | 4.0 | 17.5 | 27 |
| FIRE | 2.7 | 16.1 | 54 |
| FINALE | 3.1 | 24.5 | 96 |

The table covers the sixteen scenes it lists; OSSUARY is not in it. Every scene
in it holds 30 fps on the VM with room to spare (mean 2.8 ms, worst 5.8 ms); on
the interpreter each stays under 26 ms (mean 15.7 ms).

- Parity: frame streams **byte-identical** across backends over 1,700 frames,
  every one of the seventeen scene entries: shim stream hash `3306359932` on
  both. The C++ host's `--smoke` hash is FNV-1a 64 over the same frames:
  `76322bdbe2343c29` (the host's HUD
  is off in smoke, so its frames are the shim's bytes).
- `--reload-test` re-executes every scene file into the engine. The
  presenter's globals live in `util.jai`, so a reload resets them and the reel
  re-binds them on the next frame; a reload mid-transition cancels the
  transition (the live scene rebuilds), so no scene reads a reset buffer.

### Scoreboard

`scenes/skull_natural.jai` is OSSUARY written the way a user would write it:
the same algorithms and the same frames as `skull.jai` (scene hash
`1aba3d7537895828`), without the spellings chosen to fit the VM's fast paths
(one `sk_world`, the warp's amplitude read left of its sine, ternary folds,
one `sk_crown` holding its flames, the row pushing `sk_trace`'s result). The
ratio of the two files' serial OSSUARY rows, natural over shaped, measures how
general the VM's fast path is: at 1.0 plain code runs as fast as code written
for the VM. The host loads `skull.jai` only, so run the natural file from a
copy of the scenes directory:

```
cp -r scenes /tmp/natural_scenes
cp scenes/skull_natural.jai /tmp/natural_scenes/skull.jai
jai_demoreel --perf 30 --backend vm --workers 1                               # shaped
jai_demoreel --perf 30 --backend vm --workers 1 --scenes /tmp/natural_scenes  # natural
```

## Console performance

Half-block frames carry two colors per cell: 52 KB on average, 139 KB for PLASMA, whose
256-entry palette changes nearly every pixel, while KINSTEIN and CANYONRUN stay near 20-30 KB.
Legacy conhost parses VT ~16x slower than Windows Terminal, which is what the
host's post-processing in `demoreel_host.cpp` is for (scenes and the `--smoke` stream
are untouched by it, so parity hashes don't move):

- **Escape trimming.** Scripts already emit a color escape only when a palette
  index changes; the host additionally drops any SGR group that wouldn't change
  terminal state, and (default on conhost) rewrites 24-bit colors to the xterm-256
  cube — shorter escapes and coarser quanta = longer runs. `--truecolor` keeps
  24-bit with per-channel merge tolerance (`--tol N`, default 8); `--no-filter`
  is the byte-exact legacy stream. The filter tracks foreground and background
  separately, which is exactly the half-block stream's shape.
- **Terminal detection.** `COLORTERM=truecolor` (or `24bit`) and Windows
  Terminal (`WT_SESSION`) get 24-bit color with tolerance merging; every other
  terminal (macOS Terminal.app, the Linux console, legacy conhost) gets the
  xterm-256 cube. Conhost also shows a one-line tip on the title card ("Windows
  Terminal renders this reel much faster"). `--truecolor` and `--256color`
  override the choice, and `--ascii` replaces the half blocks with one density
  character per cell (its brightness the cell's, its color the cell's hue at
  full value) for terminals without the block glyphs; rows carrying text keep
  their characters.
- **One buffered write per frame**, via `WriteConsoleA` on a real console.
- **`--diff`** row-diff redraw: only rows that changed since the last frame are
  rewritten (cursor-move + row). Not the default — most scenes change every
  pixel every frame.

The HUD's headline frame cost is `sim + draw` (script update + frame string,
plus the console write), split out so you can see which side you're bound on.
`--bench N --scene I` runs N real frames and reports the same split headlessly
(`--bench-out FILE` to save it).

## Live coding

1. Run `jai_demoreel`.
2. Open `scenes/plasma.jai` in an editor; change the palette stops (the
   `pal.gradient` call), or the field frequencies in `render`.
3. Press `r` in the console window.

The engine re-executes every scene file (hot reload with instance migration) and
the current scene rebuilds — the change appears without restarting. `main.jai` is
deliberately never reloaded: it owns the persistent globals.

## Determinism / parity

`--smoke` runs N frames headless at a fixed dt, FNV-1a-hashing every returned
frame; `--expect H` fails the run unless the stream hash is H. All scene randomness
goes through the host's seeded xorshift `Rng` class (the presentation-only hashes
are integer arithmetic on pixel coordinates), and `parallel_transform` returns
rows in order, so the hash holds at every worker count. The backends' parity runs
on the standalone runner (`--backend=vm` and `--backend=interp` above). Env
helpers: `DEMOREEL_SMOKE_FULLDUR=1` (full interactive scene durations),
`DEMOREEL_DUMP_FRAME=N` (write frame N to a file for
diffing), `DEMOREEL_TRACE=1` (per-frame progress for crash localization).
Deliberately NOT part of the main test suite.

## --precompiled

`engine->jaibite()` parses the concatenated scene source once; `--precompiled`
saves the parsed AST as `demoreel.jaibite` on first boot and loads it afterwards
(`jaibite_load` re-interns symbols per engine, so one file serves every engine).

## Captures

Stripped of color a half-block frame is only `▀`, `█` and spaces, so these are
brightness maps of the pixel grid (one character per terminal cell, the two
pixels averaged; `-- shot I at T dump P` writes the full-color `P.bmp`).

CHROME (`shot 14 at 4` — three chrome spheres over the checkerboard; the `@@`
are sun glints, the reflections carry the sky and the floor):

```
##########********************************************************************************##########
########################++=====++####**************************#####################################
####################++:::::::::::::=+###############################################################
##################+-:::::::::::::::::-+#############################################################
#################*--:::::+++::::::::::--*###########################################################
################*=-----::@@@*::::::::---=*##########################################################
################+=======--------------====##########################################################
%%%%%%%%%%%%%%%%+++++++++++++++++++++++++++%%%%%%%+-:::::-+%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%%%%%%%%%%%%%%%%*********++**+++*++*+****++%%%%%*--:::+:::--*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%%%%%%%%%%%%%%%%*+***--=-==*===-*-===-.-*=+%%%%#+=::-**---===#%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%%%%%%%%%%%%%%%%%+===:===*==*======:=:-=*-%%%%%***..=++-+++***%%*++*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%%%%%%%%%%%%%%%%%%-:*=*::*=::=***::*=.=:=#%%%%%#+-+*=+==:-+==-:......:-+%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%%%%%%%%%%%%%%%%%%%+*:====**=::::***:=-+%%%%%%%%+=:::*=.:*:-:..........:-*%%%%%%%%%%%%%%%%%%%%%%%%%%
%%%%###%######%%%%#%%##==::=======.:.=%%%%#######*==:=*:=::.-::..@@..::::-*##%%%%%#%%%%######%##%%%%
%%###**%%%#########****##*++####=+=##%%####****#****##+%%=..-------------==***%%%%%##%%%%%******####
##*##%######*####+#########+####%%+######%%***++**%****++:-==-=--=-======-=+#######******#######*%%%
###+#++++###**********%%%%%%+********%*++++++*%******===#--:--=-:=::-=::=:=+++********++++*%%%######
+*%%%***********%%%%%+++++****%*****====*********==*===+++-==-:-===----=--*++****%%%%%%*+++****%****
++++++****+*****=====%%%%%%%%%**-=====++++==-----====%%****-=:---------++%**==****%%%****+++++++***%
%%%%%%%%%%==*********%========----=============*%%%%%%%%%**====:==***+**=========***%%%%%****=***%%%
****=====%%%%%%%%%%%%%=********%***+------======**%******==----=======**%%***==*****%%%****=========
========********=======%%%%%%%%%%%%%%*=******%%%%%%**======--------===---**=====**%%%%%%%%%%%%***===
*%%%%%%*===============%%%%******======%%%%%%%%%%%%%%%*=====*+++-====-+-=============**%%%%%%***====
%%%%%%%=======******%%%%================*%%%%%%%%%*****==*%%%%%%%%%%%%%%%%**=========****%***=======
%%******%%%%%%%%%%%%%%%%====------------**%**---------------*%%%%%%%%%%%%%%%%%==**%%%%%%%%%%%%%***==
=====*%%%%%%%%%%%%%%%%%%%-----*****%%%%%%%%%*------------------*%%%%%%%***--------**%%%%%%%%%%%%%%%%
```

KINSTEIN 3D (`shot 10 at 3.1` — the kill counter top left, oak planks overhead,
coursed stone walls converging on a Grublin down the corridor, flagstones
below, the pistol at the bottom):

```
--======:.....                                         :::::::::.        .::::.       .....:======--
--%:=**:*=%== % ....% ....===== .::::.=====.====+::::::.........::::.....................:======----
--%=*..===% :.% ..::% ::::+=++::::::::% ++% :.:++ :::::::::::::::::::::::::::::.   ....:=====-------
--* ==---=* ==* ....* ::::::..*.::::::*-..* :==..:::::::::::::::::::::::::::::::.....:=====---------
--= --=.=----:+---- -----.----         ---  -----                       ....  .......===------------
--------------=..+-..................   ...............................::::......-+..=--------------
---------------..++++-...............    ......................:::......::....-++++..---------------
..::-----------..=++++=-.............................   ....................-=++++=..-----------::..
......::-------..====+===-................................................-===+====..-------::......
..........::---..=========.:.....................................  .....:.=========..---::..........
*---.........::..=========.--:.. .................................  ..:--.=========..::.........---*
****...--........::===---=.----:.. ........  .................... ..:----.=---===::........--...****
****...====---.......::---.--:::=:..         ..                 ..:=:::--.---::.......---====...****
+++*...========+---......:.--:::--=.......................... ...=--:::--.:......---+========...*+++
++++...========++++++..-....:.::---.::: .                  . :::.---::.:....-..++++++========...++++
++++...========++++++..+++---...::-.::- . ................ . -::.-::...---+++..++++++========...++++
++++...==============..++++++.-:::....: .:.              .:. :....:::-.++++++..==============...++++
====...--------======..======.---==-::: ......         ..... :::-==---.======..======--------...====
====...--------======..======.:---=--:-:::... ..--=-:  ...:::-:--=---:.======..======--------...====
====...--------======..======.::-----:::::.:  ++#+*=#-: :::::::-----::.======..======--------...====
....................................... :....=+**#=*++:..:.: .......................................
........................::.--::=---.--: :::..:----%:::...::: :--.---=::--.::........................
.........------..=========.++===---.--: ...-=.......... ::.. :--.---===++.=========..------.........
-**************..=====----.====-:::.::..::.......:........::..::.:::-====.----=====..**************-
**************+..=====----.==---....::::....:...:...::.:::..::::....---==.----=====..+**************
*******++++++++..====-----.=::...:=--::.:..:....:.::::.:::::.::--=:...::=.-----====..++++++++*******
+++++++++++++++..--------:....:-----::.:.::.:::::.::::-..::--.::-----:....:--------..+++++++++++++++
++++++++++++++=..----::....-=.-:-:-..--.::............::.......---:-:-.=-....::----..=++++++++++++++
+++++++++======..::.....-====.:::.::::::::::::-=:.:----::-::---=:..:::.====-.....::..======+++++++++
++++=========--........======.:-.----==::-------:::::--==:::------::-:.======........--=========++++
==========--......-==..====--::::::-==..::::::..==**..:-==..:---------:--====..==-......--==========
======--.......-=====..==----:::::-==..:------..==--..--==-..:-------------==..=====-.......--======
==--........:========..---:------.....::------..==--..-----...-----------:---..========:........--==
.........:===========..--::::---------------....==--....-------------===-..:-..===========:.........
.......======--===---..:---------::::---::::..==**----..--------:::----===-....---===--======.......
...-...====----=----::--------------------....==------....----===========+++-..:----=----====...-...
-+++...=----------::----:::---------------..==**--------..================+++-...:----------=...+++-
++++...---------:-++++++++++++++++++++++##..==----------..##+++++++++++++++++++-...:---------...++++
++++...-------:.......................####..------------..####.......................:-------...++++
++==...---------------=++++-....----########..--------..########-....==================------...==++
```

DONUT (`shot 4 at 4` — the frosted torus, specular highlight on the icing):

```
....................................................................................................
....................................................................................................
...............................................=------*******+*****-=---............................
......................................-=--+****##%%%%%%##*#******+****%#**=--.......................
.................................--******++++##%%@@@%%#%*#**+**#***********+++=:....................
............................++#**+++==+=+=====**+*****++++++**+************+++++=:..................
.........................==***+++=+===---==-----============++:+*****+****++++++==-.................
.......................=+**++=+===---::::::::::::==-------=+=+********+*++++++++==--................
.....................***%**+**+===--::...............--=-*##***********++*+++++==+--:...............
...................=*********+*++++=------------##***********++******+++++**++====+-:...............
..................**************%%*%%***********%%**********+******+++++++++++===---:...............
.................*************%%********++**+++**@*********#**+++=+++++++++===+=---::...............
................-******######%#############################%###**+++++++==-=-=----::. ..............
................++***#####%%%%%%%%%%%%%%##%@@@@%%%%%%%%%#######**+++++======-----::.. ..............
................++***####%%%%@@@@@@@@@@@@@@@@@@@@@@%%%%%%##*##*++++=======---:::::.. ...............
................=+++**@%##%%%@@@@@@@@@@@@@@@@@@@@%%%%%#%#***+**=========--:-:::::.  ................
................-++++++*####%%%%%%@@@@@@@@@@@@@%%%###***#*+==========---:--::::..  .................
.................==*+*++++**#######%%%%%%%#%###*****++===+=======-------:::::..  ...................
..................-==**+++++++***++*******++++++=========++==-------::::::...   ....................
...................--============--====-================-==------::::::...    ......................
....................:----=========-====::========------------:::::::...      .......................
.......................::----::------------------------:--:::::.....      ..........................
...........................:::::::::------::::::::::::::::.....        .............................
................................::::::::::::::..:........          .................................
```

FINALE afterglow (`shot 17 at 17 ascii 1`: the fire is out, the word glows in neon and the credits roll):

```


     #########   %%%%%   #########   *******   +++++++ *******   ######### %%%%%%%   #########
          ##   %%     %%    ##     **        ++        **     **    ##     %%     %%    ##
          ##   %%     %%    ##     **        ++        **     **    ##     %%     %%    ##
          ##   %%%%%%%%%    ##       *****   ++        *******      ##     %%%%%%%      ##
     ##   ##   %%     %%    ##            ** ++        ** **        ##     %%           ##
     ##   ##   %%     %%    ##            ** ++        **   **      ##     %%           ##
       ###     %%     %% ######### *******     +++++++ **     ** ######### %%           ##


                                seventeen scenes . five transitions
                              every pixel computed by the jaiscript vm

                                           PLASMA  four sine fields
                                        STARFIELD  a warp run through a nebula
                                           TUNNEL  a table-driven texture tunnel
                                            DONUT  a sphere-traced torus
                                            JULIA  a morphing julia set
                                         LAVALAMP  lit metaballs
                                        PIPEDREAM  the 3d pipes screensaver
                                           POWDER  sand, water, ember and plant
                                            BOIDS  a murmuration and a hawk
```

## Idioms

- **`var&` for hot arrays and the frame grid.** Assignment copies arrays and objects, so
  `var alias = field_array;` is a copy and writes through it never reach the field.
  `var& alias = field_array;` binds a reference; the scenes bind their hot arrays this way at
  the top of `update` and `render`, and a scene paints through `render(var& grid)`.
- **Compound element stores.** `arr[i] += v` resolves the element once, where a read followed
  by a write resolves it twice; BANNERFALL's Verlet relaxation writes its endpoint corrections
  that way.
- **Temporal reprojection.** SPONGEWORKS keeps each pixel's last hit distance and starts the
  next march 15% short of it, so most surface rays converge in a few steps. Step counts stop
  tracking occlusion under warm starts, so its ambient occlusion is one SDF probe along the
  normal.
