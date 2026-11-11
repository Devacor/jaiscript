# jai_gloom — GLOOM: a terminal DOOM-like, all JaiScript

A real-time 2.5D raycast shooter in the terminal — one tight four-map episode of
corridors, keycards, secrets, and things that roar before they charge — written
**entirely in JaiScript**. The C++ host (`gloom_host.*` + `gloom_adapter_jai.cpp`)
is the same thin shell as the crawler/demoreel: a VT console, held-key input, a
seeded `Rng`, a monotonic clock, flags as globals — split into a shared,
runtime-agnostic core and a per-language adapter. Everything else — the raycaster, the sub-cell renderer, enemy
brains, weapons, particles, the episode, the autopilot — is script.

GLOOM is the crawler's bigger sibling and a deliberate dogfood of the language's
larger features: **`parallel_transform`** drives the wall rays and the particle pool,
**coroutine handles living in fields** drive every enemy brain and the demo pilot,
`new`/`shared_ptr` holds the world and its registries, and template strings (with
`:spec` formats) build the HUD. The scripts are written as you would write them today:
subsystem classes (World, Player, Renderer, Hud, Pilot), an enemy hierarchy whose
subclasses override one coroutine brain, typed records (`Input`, `Particle`,
`WallChunk`, `Billboard`), enums and `math::ivec2` paths.

## Run it

From the JaiScript source root, build the runner once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target jaiscript_runner
```

then:

```bash
build/bin/jaiscript examples/gloom/gloom.jai
```

(On Windows the runner is `build\bin\jaiscript.exe`.) The native host is the `jai_gloom` target: `cmake --build build --config Release --target jai_gloom`, then
`build/bin/jai_gloom`.

## The look

GLOOM draws in two looks over one renderer:

- **showcase** (the default when you play): a ray per pixel column, textured floors and ceilings, and light. Each floor
  has its mood and its stone: sandstone flagstones, coolant glowing up through grating, a foundry floor split by
  cracks of embers, the throne's checkered red. Lamps hang from the ceiling over pools of light, and the muzzle flash,
  every projectile and every blast carry light of their own color (`lights.jai`): a grid of light texels, four to a
  tile edge, where light flows through open tiles only, so it rounds corners and stops at walls, and falls off in an
  ordered stipple. Every surface reads its texel and looks its color up in a table per light level, so a lit pixel
  costs what an unlit one does. Walls carry staggered bricks, riveted plates, conduits, cracked blocks, plank doors
  whose lock lights glow (and light the floor in front of them), and an exit switch that casts green. Sprites and the
  gun take the light they stand in.
  Hits land (`feel.jai`): the view shakes with blows and blasts and bobs as you walk, the scattergun kicks, a marker
  flicks at the aim point on a hit and flares red on a kill, the screen washes red when you bleed and gold on a
  pickup, the corners darken, and blood and scorch marks stay on the floor where they fell (old blood dries). A floor
  opens with an iris and its name. Feel watches the game from outside, frame by frame, and never writes to it, so
  `STATE_HASH` is the same in both looks. Every shot, roar, door and pickup has a sound, synthesized at boot into
  8-bit WAVs by `sound.jai` (no audio file ships) and played where the host has sound (the runner and `jai_gloom`, on
  Windows). Under the view sit a status bar and a panel with big numerals, bars, a face that bleeds as you do, the arms
  rack and keycards. The title burns, the floor tally counts up, and the death and victory pages are pixel art too.
- **reference** (`--look reference`): the frames of [reference/README.md](reference/README.md) section 4.2, the ones
  the Lua, LuaJIT, Squirrel and ChaiScript ports reproduce byte for byte. `--smoke` draws it unless told otherwise, so
  the published comparison times the same frames in every language.

Both looks run the same simulation. The reference look casts one ray per cell column over a prebaked floor and
ceiling; the showcase look casts one per pixel column and textures and lights every pixel, at about 7 ms a frame at
100x40 on the VM (the reference look takes under 1). The view is a pixel grid blitted through **sub-cell block glyphs**:

| mode | pixels/cell | glyphs | notes |
|---|---|---|---|
| `--pix half` | 1x2 | `▀` | square pixels, cheapest |
| `--pix quad` | 2x2 | `▘▚▟█…` | **default**; 2 colors/cell by luminosity partition |
| `--pix sext` | 2x3 | `🬀…🬻` | needs a Unicode-13 font (Cascadia Mono in Windows Terminal) |
| `--ascii` | 1x1 | ` .:-=+*#%@` | for terminals without block glyphs (the Linux text console) |

Every color the game shows is interned once into a palette (pixel grids store small indices; the row builders emit
prebuilt escapes only when a cell's colors change). The escapes are 24-bit where the terminal has 24-bit color (on
Windows, and wherever `COLORTERM` is `truecolor` or `24bit`) and the nearest xterm-256 color elsewhere; `--truecolor`
and `--256color` override the guess. The `gloom.jai` shim reads the same through the runner's `env_var`, where Windows
means Windows Terminal (`WT_SESSION`): legacy conhost gets xterm-256.

## Build & run

Part of the normal JaiScript CMake build (`JAISCRIPT_BUILD_EXAMPLES=ON`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target jai_gloom   # PLAY THIS ONE
cmake --build build --config Debug --target jai_gloom     # debug iteration
```

Play the Release exe in **Windows Terminal** (truecolor + fast VT). The exe
prefers the source `scripts/` dir; a `gloom_scripts/` copy sits next to the
binary.

```bash
jai_gloom.exe                        # seed 666, VM backend, quad pixels, the showcase look
jai_gloom.exe --pix sext             # 2x3 pixels per cell (font permitting)
jai_gloom.exe --256color             # a terminal without 24-bit color (macOS Terminal.app)
jai_gloom.exe --ascii                # a terminal without block glyphs
jai_gloom.exe --look reference       # the frames the ports draw
jai_gloom.exe --seed 7
jai_gloom.exe --workers 0            # serial script loop instead of parallel_transform
jai_gloom.exe --bench 300            # autopilot plays while you read ms/frame numbers
jai_gloom.exe --dev                  # edit scripts/tune.jai while you play; the look reloads on save
jai_gloom.exe --mute                 # no sound
```

### Hot reload: `--dev`

`scripts/tune.jai` holds the showcase look's dials: the ambient light, the lamps, the light each weapon, projectile
and blast throws, the shake, the bob, the vignette, sound on or off. With `--dev`, save it while you play and the next
frame shows the change; the status bar names the file and how long the tables took to rebuild (about 100 ms). The
floor, the monsters, your health and the blood on the floor carry on. Save a mistake and the reload is rejected with
its line, and the game keeps the look it had. `look.jai`, `lights.jai`, `feel.jai` and `sound.jai` reload the same
way (`dev.jai`): each frame imports them as one group, the engine re-runs only what changed after checking every
changed file, and state declared without an initializer keeps its value. The simulation is never in the group, so no
reload can move `STATE_HASH`. The HUD shows `DEV: edit tune.jai` while it listens, and the core count when
`--workers` grants workers.

`gloom.jai -- --reload-test` checks all of that headless, in a scratch copy of the folder (it edits `tune.jai`): an
edit changes the frame and keeps `STATE_HASH`, a broken edit is rejected and changes nothing, and restoring the file
draws the first frame again byte for byte.

| key | action |
|---|---|
| `w` / `s` | move forward / back |
| `a` / `d` | strafe |
| `left` / `right` | turn |
| `space` / `ctrl` | fire |
| `e` | use — doors, suspicious walls, the exit switch |
| `shift` | run |
| `1` `2` `3` | pistol / scattergun / hexcaster |
| `m` | automap overlay |
| `esc` or Ctrl+C | quit |

On Windows the movement keys are honest held-key state (`GetAsyncKeyState`), so
strafe-running while turning and firing works like it should. A POSIX terminal
sends key presses but no releases: a key counts as held while its auto-repeat
keeps arriving, a shifted letter means run, and space fires. Quitting, by `esc`
or Ctrl+C, restores the screen, the cursor and the colors.

The frame loop starts frames on a fixed grid of deadlines and treats a frame
that arrived within a sixth of the period as exactly one period, so the 30 Hz
simulation advances one tick per 30 fps frame instead of beating against timer
jitter.

### No C++ host: the standalone runner

`gloom.jai` is a drag-drop shim for the standalone `jaiscript.exe` runner: it
supplies the host API of reference/README.md section 2 in pure script (a bit-exact
xorshift64\* `Rng` under checked overflow, `host_log`/`itrunc`/`ifloor`/`utf8`/
`ESC`, the `HOST_*` globals) on top of the runner's console IO, then loads
`scripts/` and runs the host pump. Flags go after `--`:

```bash
jaiscript.exe gloom.jai                          # play (esc quits)
jaiscript.exe gloom.jai -- --smoke --ticks 300   # headless parity self-test vs section 3.5
jaiscript.exe gloom.jai -- --bench 300           # autopilot + sim/draw split
jaiscript.exe gloom.jai -- --256color            # force xterm-256 (the shim guesses as the exe does)
jaiscript.exe gloom.jai -- --dev                 # play, reloading tune.jai and the look as you save them
jaiscript.exe gloom.jai -- --reload-test         # the hot-reload self-test (in a scratch copy: it edits tune.jai)
jaiscript.exe gloom.jai -- --respawn-test        # a death on the last floor keeps the arsenal you brought
```

`--smoke` exits 0 only when `STATE_HASH` matches the reference checkpoint
table.

## The game

Four maps: **DIMLIT ANTECHAMBER** (grunts, and a wall that isn't), **COOLANT
WARRENS** (spitters behind coolant vats, a red lock), **THE HEX FOUNDRY** (the
hexcaster lives here, behind the blue lock the turrets watch), **GLOOM'S THRONE**
(him, and a hexcaster waiting in the first room). Kills / secrets / par-time
tally after each floor; health, armor, three ammo pools, keycards; a face that
grins when you pick up a new gun. Die and the floor restarts, repopulated, with
full health and the arsenal you carried onto it, every gun you own at least
half stocked.

Enemies (each brain is a `coroutine` **method** whose handle lives in a field on
the enemy itself — phase state is coroutine-frame locals, pain interrupts by
discarding the handle mid-phase):

- **GRUNT** — dozes until it sees or hears you, roars (it commits), then zigzag
  charges into a lunge with a real windup.
- **SPITTER** — holds a range band, orbits sideways, telegraphs a green gob.
- **TURRET** — dormant metal until it has line of sight; wakes, 3-round bursts.
  Break line of sight and it goes back to wary idle — its coroutine never forgets.
- **THE WARDEN** — stalks and lobs hollow fire; under half health he goes
  double-volley and calls the family. Exactly once.

Weapons: pistol (accurate, honest), **SCATTERGUN** (6 pellets, crowd opinions),
**HEXCASTER** (a projectile that detonates into a particle bloom and area damage —
including yours, standing in your own spell is a choice).

## parallel_transform

Three systems run through `parallel_transform` every tick — two pure bodies in
`pure.jai` (element + locals only, math whitelist) and one captured-reads body
in `render.jai`:

- **Wall rays** — shared inputs ride inside every element, so elements are
  column *chunks* (16 of them), each carrying the map snapshot packed
  15-tiles-per-int64; the DDA inner loop is pure shift/mask. One ray per cell
  column in the reference look, one per pixel column in the showcase look;
  sprites and particles keep full sub-cell resolution. This is the
  hand-chunking reference shape; a body with captured reads could read a global
  snapshot instead.
- **Particles** — the 288-slot pool is a flat value array stepped by one pure
  function (fixed timestep baked in as a literal).
- **Glyph rows (quad)** — `gloom_row_quad(ry)` reads the CAPTURED pixel grid
  (borrow tier: zero-copy) and palette tables (per-worker snapshots); see
  "Captured reads in the glyph rows" below.

`--workers 0` runs the same functions in a plain serial loop — determinism
across worker counts holds by construction (`--smoke` hashes are identical at
any `--workers`).

Measured (min of 3 interleaved runs, quad, seed 666, 300 ticks, `--smoke`
headless ms/tick, on a loaded development machine; every row hash-identical):

| workers (quad) | VM ms/tick |
|---|---|
| 0 (serial script loops) | 45.3 |
| 1 | 37.3 |
| 4 | 35.3 |
| 8 | 33.0 |

Treat ±10% as machine noise: measure on a quiet machine, or measure twice. The
[performance guide](../../docs/site/embedding/11-performance.html) covers
measuring JaiScript.

At four workers what stays serial is the per-frame palette snapshots, the
barrier's all-primitive scan of the 13.6k-int grid, and the paint stages.
`--pix half` emits more bytes per frame than quad (35.6 KB vs 21.5 KB): a flat
quad cell is a 1-byte space on background, while every half-mode cell is a
3-byte half-block glyph.

### Captured reads in the glyph rows

`parallel_transform` captured reads (docs/site/guide/09-containers-strings.html, "Parallel loops") let a body
READ enclosing globals, so the quad glyph-row builder is a per-row function
over `ROWIDX` reading the pixel grid as a captured **borrow** (`PIX` is a flat
all-primitive int array touched only by subscript — zero copies, raw element
reads; the escape-string palettes `PAL_FG`/`PAL_BG` snapshot per worker per
frame). Captures resolve global names, so the grid is the global `PIX`, with
`VW`, `VH` and `ROWIDX` beside it. `--workers 0` runs the same row function in
a serial loop; frame bytes are **byte-identical** at every worker count, and
the STATE_HASH reference checkpoints hold (3000 ticks → 4080154357, seed 7 →
1696980843).

## Determinism

`--smoke` runs the seeded autopilot headless for N ticks on the VM and reports a
per-tick FNV `STATE_HASH` (player, enemies, particles, rng state) AND a hash of
every rendered frame string; both hold at any worker count:

```bash
jai_gloom.exe --smoke --ticks 6000
jai_gloom.exe --smoke --ticks 6000 --workers 8
jai_gloom.exe --smoke --ticks 300 --look showcase
```

At 300 ticks, seed 666, the reference look's frames hash to `7fecc09815bb64e0`
and the showcase look's to `1b6efa4fcae4e573`; `STATE_HASH` is 3580805725 under
both, since the look never touches the simulation. The showcase look also prints
its sound bank's hash and the cues it would have played. `tools/ci/gates.sh gloom`
checks both looks, the showcase look in every pixel mode (quad, sext, half,
ascii), the sound line, the shim's own smoke and its `--reload-test`;
`gloom-workers` repeats the quad and sext showcase runs at eight workers.

The autopilot is itself a coroutine handle in a Game field: it fights what it
sees, loots what it can absorb, hunts the secrets it "remembers", opens what its
keycards allow, pulls the exit switch, and — on the throne floor — goes and has
words with the landlord.

## Files

```
gloom_host.hpp/.cpp thin SHARED host: console, held keys, Rng, clock, flags,
                    smoke/bench harness; language-agnostic
gloom_adapter_jai.cpp  the JaiScript adapter + main() (this exe)
scripts/state.jai   RNG global
scripts/util.jai    math helpers, mix32, the truecolor palette intern
scripts/defs.jai    weapons, bestiary, pickups, ALL sprite art, weapon HUD art
scripts/maps.jai    the four maps + parser + boot-time validator + packed snapshot
scripts/pure.jai    the parallel_transform bodies: ray chunks, particle step
scripts/particles.jai  pool class (access labels), burst kit, color ramps
scripts/enemies.jai the Enemy class: coroutine brains in fields, pain, deaths
scripts/combat.jai  LOS, hitscan, projectiles, explosions, player damage
scripts/sim.jai     movement + sliding, use (doors/secrets/exit), pickups
scripts/tune.jai    the showcase look's dials, the file --dev is for
scripts/look.jai    the showcase look: floor moods, wall, floor and ceiling textures,
                    the lit tables, sprite shading
scripts/lights.jai  the showcase look's light grid: lamps, fixtures, flashes, blasts
scripts/render.jai  raycast view: chunked rays, the showcase view's per-pixel floors
                    and ceilings and decals, the reference view's strip cache,
                    billboards, particles, automap, half/quad/sext/ascii row builders
scripts/viewmodel.jai  the showcase look's gun in hand
scripts/canvas.jai  half-block canvas and pixel font for the showcase panel and pages
scripts/feel.jai    the showcase look watching the game: shake, bob, hit markers,
                    tints, vignette, decals, the floor's opening, sound cues
scripts/sound.jai   the effect synthesizer and cue player
scripts/dev.jai     --dev hot reload and --reload-test
scripts/hud.jai     the reference look's status strip, panel and pages
scripts/screens.jai the showcase look's status bar, panel and pages
scripts/game.jai    Game class (typed shared_ptr global), fixed-timestep tick,
                    the autopilot coroutine, STATE_HASH
scripts/main.jai    imports + host entry points
```
