# jai_crawler: GRUBWELL, dungeons of the unwashed

A first-person, step-based dungeon crawler in the spirit of the 1991 shareware crawlers (garish
corridors, a compass, monsters that get *bigger* as they walk at you), written entirely in
JaiScript. Ten floors, five kin, four professions, a bestiary of thirteen, affix loot,
identify-by-use potions and scrolls, timed effects, permadeath, and Vexadrach the Hollow-Flame
at the bottom, hoarding the color he stole. Hits throw damage numbers, the slain melt into sparks,
spells fly down the corridor carrying their own light, monsters announce themselves, and fine
loot sends up a beam in its rarity's colour. Save any script mid-run and the game reloads it
without losing the run.

The C++ host (`main.cpp`) is a thin console shell: raw keys, the console's size and colour depth,
an alternate screen it restores on every exit, a seeded `Rng`, file IO for saves, a fingerprint
of each script file for the reload watcher, and the command-line flags as globals. Everything else is script: the raycaster, dungeon generation,
combat, AI, items, saves and every screen. `scripts/crawler.jai` is the same host contract
written in JaiScript, so the game also runs on the standalone runner with no C++ of its own.

## Run it

From the JaiScript source root, build the runner once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target jaiscript_runner
build/bin/jaiscript examples/crawler/scripts/crawler.jai
```

or the native host (part of the normal build, `JAISCRIPT_BUILD_EXAMPLES=ON`):

```bash
cmake --build build --config Release --target jai_crawler
build/bin/jai_crawler                     # a new run, seed 1337
build/bin/jai_crawler --seed 7
build/bin/jai_crawler --load              # continue the saved run
build/bin/jai_crawler --god               # tourist mode: visit Vexadrach and live
build/bin/jai_crawler --colors 256        # the xterm 256-colour palette
build/bin/jai_crawler --ascii             # no Unicode block glyphs (the Linux text console)
```

The runner takes the same flags after `--`, leading dashes optional:

```bash
jaiscript crawler.jai -- seed 7 god load
jaiscript crawler.jai -- colors 256 ascii
jaiscript crawler.jai -- smoke turns 1500 seed 6             # headless autopilot; prints STATE_HASH
jaiscript crawler.jai -- input "n11;wwwww" quiet             # scripted keys (';' enter, '^' esc)
jaiscript crawler.jai -- view 36 layout single               # a 36-row forward view, alone
jaiscript crawler.jai -- smoke turns 250 seed 6 dump f6      # every frame, and a FRAME_HASH
jaiscript crawler.jai -- smoke turns 200 seed 6 anim dump f6 # the animations' frames too
jaiscript crawler.jai -- smoke turns 1500 seed 6 reload-every 100   # the pilot hot-reloads mid-run
```

`view N` and `layout single|multi` are runner-only; `screen CxR` (size everything as if the
console were C x R), `reload-every N` and `anim` (a headless run builds every animation frame)
are on both hosts (`--screen`, `--reload-every`, `--anim`). `dump PATH` renders the full frame after every smoke turn without touching the
game, writes them to `PATH.ans`, prints `FRAME_HASH` (the SHA-256 of that file) and the frame-build
time, leaves the last canvas as `PATH.bmp` with a text sketch beside it (`PATH.txt`), and the
death or victory pages the run ends on as `PATH_end.ans`; `shots N` adds a BMP and sketch every N
turns, and `bench N` re-renders the final view N times and prints ms/frame.

Play the Release build: every keypress casts four panes, redraws six boxes and, for a step or a
turn, two in-between frames; a Debug build is far slower.

## The terminal

- **Colour.** `COLORTERM=truecolor` or `24bit` means 24-bit colour, as does any Windows console
  that takes VT sequences; anything else gets the nearest colours of the xterm 256-colour cube
  (macOS Terminal.app and some Linux terminals). `--colors 24|256` overrides it. On the runner the
  shim reads `COLORTERM` through `env_var`.
- **Glyphs.** The view is drawn with the `▀` half block, the frames with rounded box lines.
  `--ascii` draws the view as one coloured cell per pair of pixels and the frames with `+-|`, for
  terminals whose font has no block glyphs.
- **Size.** The cockpit (below) is picked whenever the console holds a forward view of 20 rows
  (about 135x37); a smaller console gets the forward view alone, as large as fits, down to an
  80x24 terminal. A console resized mid-run re-fits on the next frame; `v` switches layouts.
- **Leaving.** Both hosts take the alternate screen and hide the cursor, and give them back (with
  the colours reset) on a normal exit, a script error and Ctrl+C. Windows output is UTF-8.

## Play

The title screen looks down the Well, embers rising out of the glow and eyes blinking in the
brickwork; `N` starts a run (pick a kin, a profession and a name),
`C` continues a saved one, `?` explains the keys.

| key | action |
|---|---|
| `w` / `up` | step forward; into a monster, attack |
| `s` / `down` | step back |
| `a` `d` / `left` `right` | turn 90 degrees |
| `q` / `e` | sidestep left, right |
| `.` | wait a turn |
| `z` | your class skill, down your facing |
| `g` | pick up |
| `i` | the pack: a letter to look closer, then `e` use or equip, `d` drop |
| `p` / `E` | quaff the first potion / equip the best upgrade in the pack |
| `>` | descend (on the stairs) |
| `m` | the overhead map; there `wasd`, `hjkl` and the arrows step by compass, and your facing follows |
| `v` | the forward view alone, or with LEFT, RIGHT and BEHIND around it |
| `c` / `?` | character sheet / keys |
| `esc` | pause: `s` save and quit, `q` quit, `r` reload the scripts |
| `R` | reload the scripts (saving one does the same) |
| `S` / `Q` | save and quit / quit (asks once more) |

On the runner, shift+letter gives the uppercase keys, TAB descends and SPACE waits. The runner
reports no shift state in a POSIX terminal, so there the pause page's lowercase keys stand in.

## The screen

The cockpit is four panes: the forward view (AHEAD) in the middle, LEFT and RIGHT at half scale
beside it on its horizon, BEHIND above it like a rear-view mirror. Each is the same caster turned
a further quarter turn, so LEFT, AHEAD and RIGHT read as one panorama, and the side and rear panes
are lit a little lower so the view ahead leads. Each pane is framed in the floor's wall tone and
labelled in its glow colour with the heading it shows (`LEFT · W` while you face north).

The text boxes fill the corners. The LOG keeps the messages since your last move in their colours
and dims the older ones, counting repeats. The MAP draws the explored floor around you in half
blocks, two tiles to a cell. The COMPASS lights your heading and gauges the depth. CHARACTER has
your meters (HP, focus, experience), stats, gear and, in a tall box, your kin's trait and class
skill spelled out. IN SIGHT and LOOT list what you can see, nearest first, with the pane it is in
and its distance (`LEFT 3`). The forward view alone keeps a panel beside it and the newest log
lines under it.

The map, the pack, the keys, the character sheet, the level-up boon and the pause page take the
forward view's place, framed and labelled like it, while the rest of the cockpit stays live.
Death washes the run's last view in blood under its epitaph, your killer looming beside it in
full colour; victory fills the screen with rainbow fire.

Floor 1 opens on a set piece: JAI's statue a few steps ahead, the bones of the last one down at
your feet with their blade (the Lastlight Fang, a rare find glowing blue), and a Grublin by the
statue that has already seen you. Each floor arrives under a banner with its name, and the stairs
down sum up what the floor gave and took.

## Effects

Every turn's events leave marks in the view, drawn from what the rules did and never from the
`Rng`. A hit puts its damage over the monster (larger and gold on a critical), a miss says MISS,
and what happens to you rises low in the view (`-4`, `+10`, `+12 GOLD`, `DODGE`). Your melee
blows slash across the target. The slain turn white-hot, drop apart texel by texel as they sink
into the floor, and burst into sparks of their colour. Class skills fly: an ember bolt or a hex
as a glowing orb, thorns as darts, each bursting where it lands, and Bulwark Slam jolts the
view. Vexadrach's hollow fire washes the view gray. Below a third of your health the rim of the
view beats.

Light moves with them. A bolt in flight, a cinderwisp and rare-or-better gear on the floor each
add their own light to the torch and the sconces, on walls, floor, ceiling and sprites alike.
Finer gear sends up a beam of its rarity's colour with a spark climbing it, and picking up a
rare, epic or legendary piece banners its name; every find says how it compares with what you
wear.

The first time a monster sees you it says so, a line in its colour (`Grublin: "Oi! Dinner's
walkin'!"`) and a `!` over its head; each breed has its own lines, and its own line in death.

Some have tricks. A Grublin Slinger winds up one turn (a red `!!` over it) and throws harder the
next; step out of its line of sight in between and the rock cracks off the stone. A slain Gloop
bursts into two Glooplings on the tiles around it. A Grinnest still pretending to be a chest sends
up the brightest beam on the floor, gold, and lights the room like treasure.

Interactive play animates each turn's effects through four in-between frames before the turn's
final frame; `anim` keeps those frames in a headless dump.

## Live reload

`R` re-runs every script but `main.jai` and `state.jai` mid-run, and so does saving any of them:
while the game waits for a key it fingerprints the scripts four times a second, and a file that
changed and then held still for one more look reloads the group. A reload logs what it kept
(floor, turn, monsters, items, HP), banners itself and sweeps a bright scanline down every pane;
the title bar counts the reloads. A broken edit is refused: the whole group is checked before any
of it runs, so the run goes on under the old rules, and the log names the file, line and error
until the next save fixes it. The runner re-imports only the files that changed; the native host
re-runs them all.

## How the view is drawn

The caster draws into a palette-indexed pixel canvas holding every pane at twice the text rows.
Each text cell is an upper half block whose foreground is the top pixel and background the bottom
one, and a row emits an escape only where a colour changes, from escape tables built once per
colour. Wide canvases (160 columns and up) build their rows through `parallel_transform`.

One DDA ray per column over an 84-degree field of view; the perpendicular distance removes the
fisheye. Every colour is a 16-step ramp from the floor's fog (dark) to torch-lit (warm), so
shading is one table lookup, with a 4x4 Bayer dither between steps. Two lights reach a surface:
your torch, which falls off with distance and toward the edges of the view, and the iron sconces
on the walls, whose light is baked per floor at a quarter-tile resolution and stops at the walls
that block it. North and south faces sit at 0.72 of the light.

- **Walls** are 32x32 procedural textures, one per wardrobe: mossy brick, blood-streaked brick,
  violet blocks sprouting glowing mushrooms, riveted copper panels weeping orange, ash rock split
  by ember cracks, veined marble. Wall tiles pick variants from a coordinate hash (never the game
  `Rng`): sconces with a two-frame flame, and skull niches.
- **Floor and ceiling** are cast per pixel from the same rays: flagstones with puddles, bones,
  moss and the rune circle under the stairs; slabs crossed by timber beams above. Monsters that
  die leave blood on the flagstones for the rest of the floor.
- **Sprites** are pixel art written as strings of role letters (body, shade, highlight, outline,
  eyes, teeth, gold, steel, flame...) tinted by each monster's or item's colour. They scale with
  depth, stand on the floor (bats, moths and wisps hover), sort far to near and clip per column
  against the wall z-buffer. A monster flashes white on the turn it takes a hit, and a wounded
  one carries a health bar.
- **You**: a torch in your off hand and, once you wield one, a blade in the other. A step glides
  the camera through two in-between frames with the hands dipping; a turn sweeps through 30 and
  60 degrees. A turn you took damage rims the view in red.

Every two floors the Well changes wardrobe: Mossrot Warrens, Bloodbrick Vaults, Gilded Fungus
Halls, Screaming Copper Vein, The Ashen Throat, and on floor 10 The Hollow Gallery, where the
color has been swallowed like everything else.

## Determinism

`--smoke` hands the keys to an autopilot: it walks a breadth-first path to loot, the stairs or the
boss, turning until its nose lines up, bumps into what blocks it, fires its skill down corridors
and drinks when scared. The run folds its state (turn, floor, facing, position, vitals, gear
count, every live monster and the `Rng`) into `STATE_HASH` after each turn:

```bash
jai_crawler --smoke --seed 6 --turns 1500                                            # STATE_HASH: 2459230215
jai_crawler --smoke --seed 6 --turns 1500 --reload-every 100 --anim --screen 135x37  # the same hash
```

Seed 6's pilot fights down to floor 8 and dies there to a Marrowmaw on turn 839; `--god --smoke
--turns 3000 --seed 5` kills Vexadrach on turn 1008. With `--reload-every 100` the pilot reloads
every script each hundred turns (its first try asks for a missing file and is refused) and plays
the same game to the same hash. The runner's `Rng` is a bit-exact script twin of the host's, so
the shim prints the same hashes on `--backend=vm` and `--backend=interp`, and the `dump` frames
hash alike on both. The screens and effects read the game but never the `Rng`, so drawing them
leaves the hash unchanged. `tools/ci/gates.sh crawler` checks both runs on the host (the second
drawing every animation and the death page), the god-mode run down to the victory page, then three
animated frame dumps on each backend: the cockpit at 135x37 with reloads, the forward view alone
at 80x24 in the 256-colour cube with ASCII glyphs, and the cockpit at 200x60, whose rows build in
parallel.

## Files

```
main.cpp             the native host: console, keys, colour depth, Rng, file IO, flags
scripts/main.jai     imports, the live reload and its watcher, the entry point
scripts/state.jai    the persistent globals (G, RNG, the pilot), left out of the reload
scripts/util.jai     integer math, the grid, directions, weighted picks, the hashes
scripts/term.jai     colour depth, glyph sets and the named inks
scripts/data.jai     kin, professions, bestiary, gear, affixes, potions, scrolls, flavour
scripts/entities.jai Entity, Monster, Player, Item and the timed effects
scripts/dungeon.jai  rooms and corridors, the boss arena, field of view, the BFS
scripts/items.jai    loot rolls, the pack, potions and scrolls
scripts/combat.jai   to-hit and damage, death and spoils, experience, the class skills
scripts/ai.jai       monster behaviours
scripts/art.jai      the floors' themes, procedural textures and the sprite pixel art
scripts/view.jai     the palette, the panes and frames, the caster, sprites and hands
scripts/ui.jai       the boxes, the panel, the screen fit, the animations, the pages
scripts/screens.jai  the title, the hero pickers, death and victory
scripts/fx.jai       the turn's effects: numbers, slashes, deaths, bolts and their light, barks, banners
scripts/game.jai     the Game class (and the boss coroutine), movement, the turn loop, saves, the pilot
scripts/crawler.jai  the standalone-runner shim (the host contract in script)
```

## Language notes

- **Typed data.** The content tables are arrays and maps of small aggregate classes
  (`Breed{"Grublin", "g", 0x28AF5A, 1, 3, ...}`), so a typo in a field name is a load error and
  `BESTIARY["grublin"].min_floor` reads like the struct it is.
- **Saves through the archive.** `to_json(SaveFile{version, seed, rng, G})` writes the whole
  `Game` with its player, monsters, items and effects, and `from_json` rebuilds every class.
- **Coroutines.** Vexadrach's fight is `coroutine void boss_brain(int boss_id)` on `Game`: the
  phase (enraged, mid-telegraph) lives in its locals between turns, and the handle sits in a
  `Game` field. Each Grublin Slinger runs `coroutine void slinger_brain(int id)`, its wind-up a
  `yield` between two turns; the handles live in `MONSTER_BRAINS` by monster id, and a hot reload
  keeps them running.
- **References.** Everything that changes an entity takes it as `var&` (`attack(m, G.player,
  "claws")`), so the one real monster in `G.monsters` is the one that changes; `restore_stat(int&
  stat, ...)` and `spend_focus(int& pool, ...)` change a field in place.
- **Values.** Assigning an object copies it, so `FLOOR_START = G.player` on the way down is a
  snapshot that holds still while you change, and the stairs compare the two.
- **Hot reload.** `R` or a saved file re-imports every file but `main.jai` and `state.jai` mid-run;
  the group is checked before any of it runs (a broken edit raises, and `try`/`catch` keeps the
  run going), functions and classes replace in place, and globals declared without an
  initializer (the terminal's settings, the console's size, which monsters have spoken) keep
  their values across it.
