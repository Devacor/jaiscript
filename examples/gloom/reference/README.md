# GLOOM — reference spec

This document is the **single source of truth** for GLOOM's behavior, and for
any port of it to another language.
The JaiScript implementation in `scripts/` is the reference; where prose and
script disagree, the script wins. Conformance is defined operationally:
**same seed, same tick count ⇒ same `STATE_HASH`** (§3).

---

## 1. Game spec

### 1.1 Time

- Fixed timestep `TICK = 1.0 / 30.0` seconds. All sim math uses this constant;
  there is no variable-dt anywhere in the sim.
- Interactive mode accumulates real dt into whole ticks (max 3 per frame, then
  the accumulator resets to 0 — stall recovery). `--smoke` feeds exactly one
  TICK per frame.
- `G.tick` increments at the TOP of every tick, in every mode.

### 1.2 Modes

`mode`: 0 title, 1 play, 2 tally, 3 dead, 4 victory. `mode_t` counts ticks in
mode. Transitions (all trigger on the `start` input):

- title → play: `start && mode_t > 8` → `start_episode()` (map 0, fresh player).
- tally → next: `start && mode_t > 25` → `finish_map()`: fold map tallies into
  episode tallies, `map_i++`; past the last map → mode 4 (victory), else load.
- dead → play: `start && mode_t > 25` → `deaths++`, fresh player,
  **same map reloaded and repopulated** (enemies/items reset; RNG stream
  continues — no reseed).
- victory: terminal (renders forever).

Non-play modes still run `accumulate_hash()` every tick.

### 1.3 Tick structure (play mode) — ORDER IS NORMATIVE

```
tick += 1
input = gather_input()            // pilot coroutine resume OR host keys
weapon select (if input.weapon valid and owned and different: cooldown = 8)
automap toggle on edge key "m"
if input.use:    player_use()
player_move(input)                // turn, then move, then pickups if moved
if input.fire:   player_fire()
decrement timers: cooldown muzzle gun_kick noise msg_t face_pain face_grin hurt_flash (each floor 0)
for each enemy IN ARRAY ORDER: enemy.tick()
update_shots()
particle pool update (pure step, §4.3)
map_ticks += 1
accumulate_hash()
```

Enemy array order = map-parse order (row-major over the map text), plus
warden summons appended at spawn time.

### 1.4 Player

- Position floats `px, py` (tile units, center-of-tile spawns at `x + 0.5`),
  heading `pang` radians, normalized to [0, 2π) after each turn.
- Turn rate 2.7 rad/s. Move speed 3.4 tiles/s, 5.0 when `run`. Diagonal wish
  vectors are normalized. Collision: the four corners of a square of radius
  0.26 must be on floor; x and y resolve **separately** (wall sliding), x first.
- Bob phase `bob += 0.30 (run) / 0.22` per moved tick (HUD only).
- `hp` 0..100 (soul orb overheals to 150 cap), `armor` 0..100.
- Damage intake: `absorbed = min(armor, dmg*2/3)` (integer math);
  `armor -= absorbed; hp -= dmg - absorbed`. God mode (`--god`): no damage.
  hp ≤ 0 → mode 3, `death_cause` = source string.
- Ammo pools: bullets/shells/cells, caps 200/50/120, start 48/0/0.
  Weapons owned: `have = [1,0,0]` at spawn (pistol only).

### 1.5 Weapons

| # | name | ammo | cooldown (ticks) | pellets | spread (rad, ±) | damage/pellet | type |
|---|---|---|---|---|---|---|---|
| 0 | PISTOL | bullets | 11 | 1 | 0.012 | roll(6,12) | hitscan |
| 1 | SCATTERGUN | shells | 28 | 6 | 0.11 | roll(3,7) | hitscan |
| 2 | HEXCASTER | cells | 22 | 1 | 0 | explosion | projectile, speed 9.0 |

Firing (only if cooldown == 0): if the pool is empty → message + cooldown 8,
**no shot**. Else: ammo -= 1, cooldown = weapon cd, `muzzle = 3`, `noise = 24`,
`gun_kick = 4`, muzzle particle burst, then:

- hitscan pellet: angle = `pang + (nextf()-0.5)*2*spread` per pellet; see §1.8.
- hexcaster: spawns shot kind 3 along `pang` (no spread).

### 1.6 Enemies

Common: position floats, radius, hp, `alive`, states
(0 dormant, 1 hunt, 2 attack, 3 dying, 4 dead — dying/dead are art states),
`flash` (4 ticks after damage, whitened sprite), `stun` (pain), a coroutine
brain handle. Movement = `move_toward(tx, ty)`: normalized step
`speed * TICK`, x/y resolved separately against the tile grid (radius below);
`walk` increments per axis actually moved (drives walk-frame art).

| kind | name | hp | speed | radius | pain% | melee |
|---|---|---|---|---|---|---|
| 0 | GRUNT | 30 | 3.1 | 0.34 | 55 | roll(4,10) |
| 1 | SPITTER | 45 | 2.2 | 0.36 | 40 | roll(3,7) shove |
| 2 | TURRET | 60 | 0 | 0.38 | 15 | — |
| 3 | WARDEN | 520 | 2.0 (×1.5 enraged) | 0.45 | 8 | roll(10,22) |

Damage intake (`hurt(dmg)`): `hp -= dmg; flash = 4;` blood burst of
`min(6, 2 + dmg/6)` particles. hp ≤ 0 → death: `alive = false`,
`kills += 1`, gib burst (14 particles for warden, else 7; warden also an
explosion burst + `warden_down = true`), message. Else pain roll
`chance(pain%)`: `stun = 5 + next(5)`, **brain handle discarded** (re-minted
next non-stunned tick — mid-phase interruption is the mechanic), state → hunt.

`enemy.tick()`: `anim += 1`; flash decrements; dead → return; stun decrements
→ return; brain minted if null/done; `brain.next()` (one yield = one tick).

Perception: `sees_player()` = distance < 13 AND `los_clear` (§1.8);
`hears_player()` = `noise > 0` AND distance < 12. `noise` is set by player
shots (24), hex explosions (20), door opens (≥10); decays 1/tick.

Brains (timers are loop counters in the coroutine frame; consult
`enemies.jai` for exact bodies — normative):

- **GRUNT**: dormant until sees (< 11) or hears → 7-tick roar → loop: if
  dist < 1.3: attack state, 6-tick windup, then if dist < 1.6 && LOS → melee;
  5-tick recover. Else zigzag chase: aims at the player's flank, side flips
  every 20 anim ticks (`zig` initial from `next(2)`), lean 0.9 beyond 3 tiles
  else 0.2.
- **SPITTER**: dormant until sees (< 12) or hears → loop: if LOS && 2 < d < 9.5:
  9-tick telegraph → spawn shot kind 1 at player (speed 6.5) if still LOS →
  16 ticks orbiting sideways (`orbit` dir, 40% flip after) → repeat. If d ≤ 2:
  30% shove melee when d < 1.2, retreat step. Else advance.
- **TURRET**: outer loop forever: dormant until sees && d < 11 → 8-tick wake →
  while sees && d < 12: attack state, 3 shots (each = `turret_shot`, then
  2 yields), 13-tick cooldown. LOS broken → back to dormant wait.
  `turret_shot`: muzzle fx, hit chance `max(12, 66 - trunc(d*5))`%, damage
  roll(2,6); miss → tracer wall-impact fx at `min(d+1.5, wall_dist-0.1)`.
- **WARDEN**: dormant until sees (< 13) or hears → message, 10-tick pause →
  loop: enrage check at hp < 260 (speed ×1.5, once); after enraged, summon
  once: up to 2 grunts at free tiles among (+2,0)(-2,0)(0,+2)(0,-2) offsets
  (each spawn: explosion fx; **counts toward kill_total**). If LOS && d > 2.4:
  8-tick telegraph → 1 volley (2 enraged) of shot kind 2 (speed 5.5) with
  4 ticks between → 12 ticks (6 enraged) walking at player. If d < 1.7:
  5-tick windup melee. Else walk at player.

### 1.7 Projectiles (`shots`: [kind, x, y, vx, vy, ttl])

Kinds: 1 spitter gob, 2 warden hollow fire, 3 player hex bolt. Spawn offset
0.4 (0.45 for dir-spawned hex) along the (normalized) direction; ttl 150.

`update_shots()` per shot, in array order: ttl -= 1 (≤ 0 → dead); then 3
substeps of `v * TICK / 3`: wall hit (tile solid at floor(x),floor(y)) →
kind 3: hex explosion, kind 2: fire explosion, kind 1: wall-impact fx, at
`pos - v*0.02`; kind 3 vs enemies: dist < (radius+0.2)² → hex explosion;
kinds 1/2 vs player: dist² < 0.14 → kind 2 fire explosion, kind 1
roll(4,9) damage + blood fx. Survivors are rebuilt into a new array (order
preserved). Then glow-trail particles: shot i spawns one particle when
`(tick + i) % 2 == 0` (kind 6 for gob/hex, 1 for fire).

Explosions:
- **hex**: explosion fx burst, noise 20; enemies within dist² < 3.24:
  `dmg = trunc(42 - sqrt(d²)*14)`, hurt if > 0 (in enemy array order);
  player within dist² < 2.25: `trunc(20 - sqrt(d²)*10)` self-damage.
- **fire**: explosion fx; player within dist² < 2.89: `trunc(26 - sqrt(d²)*9)`.

### 1.8 Rays, LOS, hitscan

- `los_clear(a, b)`: step along the normalized segment at t = 0.15, 0.30, …
  < len; solid tile at any sample → false.
- Wall ray (`gloom_ray` / `gloom_ray_chunk`): standard DDA over the tile grid
  returning perpendicular distance (min 0.02), side (0 = x-face, 1 = y-face),
  wall kind, and texture column `trunc(frac(wallx) * 64)`. Guard: 128 steps.
- `hitscan(origin, dir, lo, hi)`: wall distance from the DDA; then over all
  live enemies pick the smallest projection `t = rx*dx + ry*dy` with
  `0.2 ≤ t ≤ wd + radius` and perpendicular distance < radius + 0.12; hit →
  `hurt(roll(lo, hi))`; miss → wall-impact fx at `wd - 0.08` along the ray.

### 1.9 Tiles, doors, keycards, secrets, exit

Tile ints: 0 floor, 1-4 theme walls, 5 door, 6 exit switch, 7 secret wall,
8 red-locked door, 9 blue-locked door. Solid = nonzero. Doors/secrets OPEN by
becoming floor (tile := 0) — they never close.

`player_use()` (edge-triggered): probe at 0.5 / 0.9 / 1.3 along the facing;
first nonzero tile: door 5 → open; 8/9 → open if the matching keycard is held
else message; 7 → open, `secrets += 1`, glitter fx; 6 → mode 2 (tally);
walls → flavor message. Opening a door: puff fx, `noise = max(noise, 10)`.

Keycards are per-map (`key_r/key_b` reset on map load).

### 1.10 Items (`items`: [kind, x, y, taken])

Pickup check runs only on ticks where the player MOVED, over all items in
array order: untaken && dist² ≤ 0.36 → apply; refused pickups stay.

| kind | map char | effect (refused if useless) |
|---|---|---|
| 0 stimpack | `1` | +10 hp, cap 100 |
| 1 medikit | `2` | +25 hp, cap 100 |
| 2 flak vest | `3` | +50 armor, cap 100 |
| 3 bullet clip | `4` | +20 bullets |
| 4 shell box | `5` | +8 shells |
| 5 hex cells | `6` | +30 cells |
| 6 RED keycard | `r` | key_r = true |
| 7 BLUE keycard | `b` | key_b = true |
| 8 SCATTERGUN | `7` | have[1], +8 shells, auto-switch, grin 30 |
| 9 HEXCASTER | `8` | have[2], +40 cells, auto-switch, grin 30 |
| 10 soul orb | `9` | +40 hp, cap 150, grin 20 |

### 1.11 Maps

Authored in `maps.jai` (`MAPS` array — the four `rows` blocks are normative
content). Char legend: walls `#%=&` (kinds 1-4), `D/R/B` doors, `S` secret,
`X` exit, `.` floor; spawns replaced by floor: `@` player (facing angle 0 =
+x/east), `g` grunt, `z` spitter, `t` turret, `W` warden, items per §1.10.
Every map passes `validate_map`: uniform row width, sealed border, exactly one
`@`, doors flanked by floor on an axis, full reachability (doors/secrets
passable), reachable exit. `kill_total` = spawned enemies (+ summons when they
happen); `secret_total` = count of `S`.

Episode: E1M1 DIMLIT ANTECHAMBER (7 kills incl. none summoned, 1 secret,
scattergun), E1M2 COOLANT WARRENS (10 kills, 1 secret, red key), E1M3 THE HEX
FOUNDRY (13 kills, 1 secret, hexcaster behind blue key), E1M4 GLOOM'S THRONE
(warden + 2 turrets + 2 grunts + up to 2 summons, 1 secret). Par times (ticks):
1650 / 2100 / 2700 / 2400.

### 1.12 The autopilot (drives `--smoke`; normative in `game.jai`)

A coroutine yielding one input record per tick:

- mode ≠ play → yield `start`.
- **Combat**: nearest live enemy with distance < 12 AND LOS (euclidean; scan in
  array order, strict `<` best). Skipped ("disengage") when hp < 40 unless
  best_d ≤ 3. Turn toward target when |Δang| > 0.05; weapon sense: scatter if
  owned+ammo and d < 6, else hex if owned+ammo and d > 3.5, else pistol if
  bullets, else scatter, else hex (no switch if already current). Fire when
  |Δang| < 0.14 && cooldown == 0 && d > 1.2. Back off when d < 2.4; advance
  when d > 8.5 && aligned. Strafe: `strafe_t` counts ticks; > 18 → reset and
  `chance(60)` flips the strafe side. Always `run`.
- **Navigate**: goal is COMMITTED until stood upon (floor goals) or 240 ticks
  stale; the BFS field alone refreshes every 45 ticks (same goal). Goal
  priority: medkit-family [0,1,10] if hp < 55 → nearest live enemy (hunt,
  euclidean) → nearest ACCEPTABLE item (skips anything §1.10 would refuse and
  keys already held) → nearest unopened secret tile → the exit tile. BFS
  from the goal over floor + plain doors + keyed doors matching held keys
  (goal tile seeds the field, so wall goals work). Walk to the 4-neighbor with
  the smallest field value; turn when |Δang| > 0.08, forward when |Δang| < 0.6,
  `use` when the chosen next tile is nonzero, within dist² < 2.1, and facing.
- No goal at all → yield turn-right (a lost pilot stays deterministic).

Expected outcomes (seed 666, quad, any worker count): clears E1M1 100%
kills/secrets by ~tick 3000, zero deaths, and is deep in E1M2 at tick 16000
(the pilot is deliberate, not fast: it disengages to heal and commits to BFS
goals). Deaths respawn on the same map and the run continues
deterministically. Smoke conformance does NOT require finishing the episode —
the checkpoint hashes above are the contract.

### 1.13 RNG — the determinism contract

The host `Rng` (§2.3) is the ONLY randomness. **Ports must consume it in
identical order** — the order is defined by the reference code paths (§1.3
order + the call sites in combat/enemies/particles). No RNG is consumed at
boot, during map load, or in non-play modes. Practical conformance: compare
`STATE_HASH` (§3.2) at checkpoints; any divergence bisects to the first tick
where consumption differs.

---

## 2. Host API contract (`gloom_host.hpp/.cpp` — the shared host)

The host is shared by every language: each supplies a thin adapter
(`gloom_adapter_<lang>.cpp`; JaiScript's is `gloom_adapter_jai.cpp`) implementing
the interface in `gloom_host.hpp`.

The host owns: console, input, timing, rng, flags. Script owns everything else.
Entry points the host calls (script must export):

| call | when | signature |
|---|---|---|
| `gloom_boot(w, h)` | once after scripts load | ints: console cols, rows |
| `gloom_frame(dt, key, fps, ms_sim, ms_draw)` | per frame | floats + edge-key string → returns the FULL frame string (rows joined by `\n`) |
| `gloom_state_hash()` | smoke end | → int |
| `gloom_wants_quit()` | per frame (interactive) | → bool |
| `gloom_summary()` | process end | prints via host_log |
| `gloom_force_autopilot()` | before boot (`--bench`) | enables the pilot |

Bindings the script consumes:

- `host_log(string)` — line to stdout (never hashed).
- `key_down(string) -> bool` — held-key state; names: single chars `a-z 0-9`,
  `left right up down space shift ctrl`. Always false in smoke/bench.
  (Windows: `GetAsyncKeyState`, only while the console window is foreground.)
- `itrunc(float) -> int` — C cast truncation. `ifloor(float) -> int` — floor.
- `utf8(int) -> string` — UTF-8 encoding of a codepoint (glyph tables).
- `ESC` — the escape char as a 1-char string (lexers without `\x1b`).
- Globals: `HOST_SEED HOST_SMOKE HOST_TICKS HOST_WORKERS HOST_GOD
  HOST_BACKEND HOST_PIX` (`--pix`: 0 half, 1 quad, 2 sext).
- Presentation globals the JaiScript adapter also binds, which a port may
  leave out: `HOST_LOOK` (`"reference"` under `--smoke`, else `"showcase"`;
  `--look` chooses), `HOST_TRUECOLOR` (24-bit escapes, else xterm-256) and
  `HOST_ASCII` (`--ascii`). The reference look is the one this document
  specifies; every hash in it is the reference look's.
- Frame keys (edge events, one per frame): lowercased chars, `esc tab enter
  up down left right`; consumed by the FIRST tick of the frame.

### 2.3 Rng (bind identically in every port)

xorshift64\*: state `s` = seed (0 → 0x9E3779B97F4A7C15), warmed by 4 discarded
steps at construction. Step: `s ^= s>>12; s ^= s<<25; s ^= s>>27;
return s * 0x2545F4914F6CDD1D` (all uint64).

- `next(n)`: n ≤ 0 → 0 else `step() % n` (unsigned mod).
- `roll(lo, hi)`: hi ≤ lo → lo else `lo + next(hi - lo + 1)`.
- `chance(p)`: `next(100) < p`.
- `nextf()`: `(step() >> 11) * (1.0 / 9007199254740992.0)` — [0,1) double.
- `state()`: `s & 0x3FFFFFFFFFFFFFFF` (fits a script int64).

---

## 3. Instrumentation spec

### 3.1 Flags

```
--smoke            headless: BOTH backends (ports: run once), fixed dt=TICK,
                   autopilot; prints per-backend total/ms-per-tick, the frame
                   stream hash, STATE_HASH; exit 0 iff parity
--ticks N          smoke tick budget (default 2000)
--workers N        parallel width; 0 = serial loops (ports: always serial)
--dump-frame N     smoke: write frame N to gloom_frame_<backend>.txt
--bench N          interactive loop + autopilot for N frames; prints
                   sim/draw ms per frame split
--seed N  --god  --pix half|quad|sext  --w/--h  --fps
--look reference|showcase   --truecolor  --256color  --ascii   (presentation)
```

### 3.2 STATE_HASH (sim conformance)

`mix32(h, v)` = FNV-ish fold on the low 32 bits of an int64 lane:
`x = h ^ (v & 0xFFFFFFFF); x ^= (v >> 32) & 0xFFFFFFFF;
return ((x & 0xFFFFFFFF) * 16777619) & 0xFFFFFFFF` (int64 math).

`hash` starts 2166136261 at boot and folds EVERY tick, in this exact order:
tick; `mode*31 + map_i`; `trunc(px*256)*4096 + trunc(py*256)`;
`trunc(pang*1024)`; `hp*512 + armor`; `ammo[0]*65536 + ammo[1]*256 + ammo[2]`;
`weapon*64 + key_r*2 + key_b`; live-enemy count; enemy accumulator
(`acc = (acc + trunc(e.x*64)*977 + trunc(e.y*64)*331 + e.hp*7) & 0xFFFFFFFF`
over live enemies in array order); particle accumulator
(`pacc = (pacc + kind*131 + (trunc(x*16)*61 + trunc(y*16))*17 + life)
& 0xFFFFFFFF` over live pool slots in slot order); 
`shots.size()*8191 + kills*127 + secrets*31`; `RNG.state()`.

Floats must be IEEE-754 doubles evaluated in source order — the quantization
(`trunc`) plus xorshift RNG makes the hash bit-exact across correct ports.

### 3.3 Frame hash (render conformance, optional but recommended)

FNV-1a 64 (offset 14695981039346656037, prime 1099511628211) over the raw
bytes of every frame string in tick order. Frame bytes are deterministic given
§4.2's palette/glyph rules; smoke passes `fps = ms_sim = ms_draw = 0` so the
HUD perf readout is blank.

### 3.4 Perf reporting

Report ms/tick (smoke, headless) and the `--bench` sim/draw split, plus the
subsystem ablation if you want parity with the JaiScript numbers (sim-only /
rays / paint / rows — measured by early-returning the render pipeline).

### 3.5 Reference checkpoints (seed 666, quad pixels, 100x40)

Identical on both JaiScript backends and at every `--workers` value; a port
that matches these is conformant. (Regenerate with
`jai_gloom.exe --smoke --ticks N`; sim hashes are independent of `--pix`.)

| ticks | STATE_HASH |
|---|---|
| 300 | 3580805725 |
| 2000 | 319812559 |
| 3000 | 4080154357 |
| 16000 | 3497451110 |

Other seeds at 3000 ticks: seed 7 -> 1696980843, seed 99 -> 1855347375,
seed 4242 -> 2848371116; god-mode seed 5 at 12000 ticks -> 576425398.

---

## 4. Architecture notes for porters

### 4.1 Script/host split

Everything below `gloom_frame` is script: sim, raycast, sprite/particle
rendering, row-string building, HUD, screens, pilot. The host only blits the
returned string (`\x1b[H` + frame + reset) and supplies §2.

### 4.2 The renderer (serial-equivalent semantics)

- View = truecolor pixel grid `vw × vh` (`vw = cols*PIXW`,
  `vh = (rows-6)*PIXH`; PIXW×PIXH = 1×2 half / 2×2 quad / 2×3 sext).
- Colors are interned: `rgb_idx(r,g,b)` clamps to 0..255 and returns a stable
  small index; pixels store indices; the row builder emits prebuilt
  `ESC[38;2;r;g;bm` / `48;2` escapes ONLY when the cell's fg/bg changes from
  the previous cell in that row; every row ends with `ESC[0m`; rows join with
  `\n`. Frame BYTES do not depend on intern order (escapes carry raw rgb).
- One ray per CELL column (`ncols = vw/PIXW`), camera plane 0.78,
  `camx = 2*col/(ncols-1) - 1`. Wall slice: `lineh = trunc(vh/dist)` (min 1),
  centered; 16 distance shades `clamp(15 - trunc(dist*1.55) + muzzle_boost)`
  (boost 2 while `muzzle > 0`), side 1 faces at 0.70 brightness; texture from
  a 32-tall tone strip per (kind, side, shade, texcol/2) — tone patterns in
  `render.jai:tex_tone` (normative); fixed-point walk `tacc += 65536/lineh`,
  index `tacc >> 11` (strip has a 33rd duplicate entry to absorb rounding).
  Ceiling/floor: per-pixel-row gradient quantized to cell rows, prebaked into
  a background grid copied whole each frame. Wall colors: theme base +
  accent = base×1.35 (clamped), mortar = base×0.42; shade luminance
  `0.13 + 0.87*s/15`; doors/exit/red/blue kinds use fixed palettes
  (`render.jai:build_render_tables`).
- Billboards (enemies, items, projectiles): camera transform
  `inv = 1/(planex*diry - dirx*planey)`, `depth = inv*(planex*ry - planey*rx)`,
  cull depth outside [0.18, 15]; screen x `= (vw/2)*(1 + ttx/depth)`;
  `ph = trunc(span*size)` with `span = vh/depth`, aspect
  `pw = ph*aw*2*PIXW/(ah*PIXH)`; feet on `vh/2 + span/2` minus `zlift*span`;
  far-to-near (insertion sort on depth, stable), clipped per pixel column by
  `depth < zbuf[x]`; art sampled nearest; 8 shade bands
  `clamp(trunc(depth*0.85) - muzzle_boost1)`, damage flash whitens
  (+170/+150/+140 pre-shade). Art bitmaps + palettes: `defs.jai:ART_SRC`
  (normative pixels).
- Particles: project like billboards; size `psz = min(3, trunc(0.07*span)+1)`
  tall × `psz*2*PIXW/PIXH` wide; color = per-kind 4-phase ramp
  (`phase = 3 - min(3, life/5)`); z-clipped per column.
- Overlays: automap (tile blocks + blips), hurt-flash red border.
- Cell → glyph: half mode fg=top/bg=bottom `▀`; quad/sext: all-equal → space
  on bg; vertical-halves / horizontal-halves two-color fast paths (`▌`, `▀`);
  else luminosity partition: threshold `(lmin+lmax+1)/2` over
  `lum = 2r+3g+b`, fg = brightest pixel's color, bg = darkest, glyph from the
  bit pattern (bits: TL=1, TR=2, BL=4, BR=8; sext adds ML=4/MR=8, BL=16,
  BR=32 with the standard U+1FB00 mapping, ▌/▐/█/space specials).
- **Parallelism is an implementation detail**: the reference runs the ray fn
  over 16 column-range chunks via `parallel_transform` (and the particle step
  over the pool). `--workers 0` runs the SAME functions in a serial loop with
  identical results — ports should implement the serial form; outputs are
  defined to be identical.
- The map snapshot for rays is packed 15 tiles per int64, 4-bit render kinds
  (secret → kind 1; red/blue doors → 7/8), rebuilt only when a tile changes.
  Ports without the value-isolation constraint may simply read their tile
  array — the packing exists for the parallel contract, not the game.

### 4.3 Particle pool

288 fixed slots `[kind, x, y, z, vx, vy, vz, life]`, round-robin cursor
overwrite. Kinds: 0 dead, 1 spark, 2 blood, 3 gib, 4 smoke, 5 flash,
6 hexmote. The step function (`pure.jai:gloom_particle`, normative: gravity/
drag constants, floor bounce for gibs, life decrement, floor clamp 0.02) uses
TICK as a literal. Spawn bursts (`particles.jai`) consume RNG — order matters.

### 4.4 Traps a porter might hit

- `player_move` applies TURN before MOVE, and moves x before y.
- Pickups only run on ticks where the player actually moved.
- The dying/dead enemy stays in the array forever (corpse sprite, hash-inert
  except the live filter). Kill counting happens at death, not sweep.
- `enemy_art` frame thresholds: walk frame flips every 6 `walk` units; death
  frames at anim < 6 / < 12 / corpse.
- Warden summons add to `kill_total` mid-map (100% kills means the family).
- The exit is usable regardless of remaining enemies.
- Title/tally/dead screens tick the hash too (mode/mode_t both fold via
  `mode*31+map_i` and tick).
- Integer division in damage/geometry is TRUNCATION toward zero (C semantics).

---

## 5. JaiScript language-feel notes (for the comparison rubric)

Happy-path experience implementing GLOOM in JaiScript, engine-bug detours
excluded. LOC (script total ~3560 + 530 host):

| file | LOC | subsystem |
|---|---|---|
| render.jai | 694 | raycast view, strips, billboards, row builders |
| defs.jai | 618 | data + all pixel art (art is most of it) |
| game.jai | 529 | Game class, tick, autopilot, hash |
| maps.jai | 347 | maps + parser + validator |
| enemies.jai | 331 | Enemy class + 4 coroutine brains |
| hud.jai | 233 | HUD + full-screen states |
| combat.jai | 229 | LOS/hitscan/projectiles/damage |
| pure.jai | 164 | the two parallel bodies |
| sim.jai | 153 | movement/use/pickups |
| particles.jai | 130 | pool + burst kit |
| util/main/state | 133 | glue |

Rough wall-time per milestone: host + skeleton + data ~2h; all systems first
pass ~3h; first clean smoke (incl. map fixes via the validator) ~2h; perf
pass (measure → chunked rays → strip cache → bg prefill → row fast paths)
~2h; pilot debugging + balance ~1.5h.

**What felt good:**

- **Coroutine methods as enemy brains, handles in fields** — the single best
  fit of language to problem in the project. Phase state lives in the
  coroutine frame (`for (int i = 0; i < 7; ++i) { yield; }` IS the telegraph),
  and pain-interruption = discard the handle; `tick()` re-mints it. The
  warden's enrage/summon-once logic is just locals. The autopilot is the same
  pattern at greater length.
- **Typed fields and typed locals.** `int lineh = 1.0 * vh / dist;` being the
  truncation you want, everywhere, removes a whole class of casts. Typed
  fields caught several transposed-argument bugs at the write site.
- **Template strings** (`${expr}` and the new `:.1f` specs) make the HUD and
  the validator's error messages effortless.
- **`new` + typed `shared_ptr` globals/registries**: `G` as
  `shared_ptr<Game>` and `G.enemies` as a registry of handles reads exactly
  like the design in the aliasing guide; chained mutation
  (`G.enemies[i].hurt(...)`) just works.
- **The boot-time map validator** (throwing template strings) caught five
  authoring bugs in minutes; script-side validation of script-side data is a
  genuinely pleasant workflow.
- **jaibite caching + include-path imports**: boot is instant, and script
  edits during development need no rebuild — edit, rerun the exe, observe.

**What felt awkward (genuine friction, not bugs):**

- **Value semantics demands constant vigilance at scale.** The rules are
  documented and consistent, but a 3.5k-line game touches every edge:
  `var e = arr[i]` deep-copies even shared_ptr elements (mutations silently
  lost — use `auto&` or chained calls), `var& x = G.field` doesn't bind
  (member exprs are fine as ref ARGUMENTS, not ref DECLARATIONS — hence the
  `render_view()` → `render_view_impl(G.pix, ...)` entry-point pattern).
  Each rule is fine; the asymmetries between them are the tax.
- **No string subscript** (`s[i]` errors; `at()` returns a 1-char string).
  Fine for text, painful for bytes-as-data; the map snapshot ended up
  bit-packed in int64s (which was faster anyway, but the language pushed me
  there).
- **The pure-value `parallel_transform` contract makes shared inputs
  expensive** — the natural per-column element wants the map snapshot; at 200
  columns that's ~9 ms/frame of input building, so you hand-chunk into 16
  column ranges, reimplementing the scheduler's own chunking. First-class
  read-only shared inputs (or `parallel_for`'s pads) is the missing feature.
- **Serial per-element cost is the frame budget.** ~0.5 µs per array element
  read means "loop over 27k sub-pixels" stages dominate everything; you write
  renderer code counting statements-per-pixel like it's 1985 (that's half the
  charm, half a warning).
- Small ones: ternaries over two container reads mis-type into typed locals
  (if/else works); method declarations require the `function` keyword form for
  `var` returns; remembering which loops need `auto&` is muscle memory that
  the compiler doesn't check for you.

**Net:** the object/coroutine/typing layer feels like writing gameplay code in
a small C++, and it held 60 ticks/sec of full game sim in ~2 ms. The renderer
is where a scripting language earns its per-op price; JaiScript's answer
(parallel_transform + the VM) covers the compute but not yet the serial
grid-consumption stages.
