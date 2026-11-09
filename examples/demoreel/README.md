# jai_demoreel — a demoscene tech reel written in JaiScript

A cold open and eleven auto-advancing truecolor scenes, six kinds of transition, pixel-font
title cards, and a finale where an eight-dimensional hypercube unfolds over
Escher's hyperbolic plane, shatters into the word JAISCRIPT and slams it into a
wall of rainbow fire before the credits roll — every effect coded in
`scenes/*.jai`, every frame drawn in **Unicode sextants**, six pixels a cell
(half blocks on request or on very large terminals). A ray tracer, a sphere
tracer, a textured Wolfenstein raycaster, a VoxelSpace terrain flight,
metaballs, a murmuration, a skull-headed spider weaving an orb web in a morphing gyroid
labyrinth, a Navier-Stokes ink tank and the classics. The C++ host (`demoreel_host.cpp`) is a
thin shell: a VT console, a monotonic clock, non-blocking keys, a seeded rng
class, and the frame pump. Everything you see is script.

The reel doubles as a determinism test: `--smoke` hashes the whole frame stream
against a reference. Ten of the eleven scenes split their work across every core
with `parallel_transform`, and the stream stays byte-identical anyway.

## How a frame is drawn

Each scene paints a grid of palette indices in square pixels, and the presenter
in `util.jai` packs them into cells one of two ways:

- **Sextants** (the default): a cell is 2x3 pixels drawn with one of the 64
  Unicode 13 sextant glyphs, fitted as JaiDoom fits them (`vid_row_sext`): the
  cell's brightest pixel is the foreground, its darkest the background, and each
  pixel takes the nearer in luminance. A sextant is 2/3 as wide as it is tall, so
  the grid is three pixels a row tall and one and a half a column wide, and each
  of a cell's two glyph columns shows the grid column nearest it.
- **Half blocks**: a cell is 1x2 pixels through the upper-half-block glyph `▀`:
  the **foreground** truecolor escape paints the top pixel, the **background**
  escape the bottom one (JaiDoom's `vid_row_half`).

`pix half|sext|auto` (`--pix` on `jai_demoreel`) picks; auto takes sextants while
the grid stays within 48,000 pixels (about 160x60 cells) and half blocks above,
and the density map always reads half blocks. Escapes are emitted only when a
color changes along the row, and a cell whose pixels agree collapses to a space
(a background run) or a full block (a foreground run), so smooth regions cost one
byte per cell. Palettes
precompute both escape strings per entry; a transition simply concatenates the
two scenes' tables (the incoming scene's indices shift by an offset) and the
reel's own UI entries ride at the end. `hb_row` is written as a
`parallel_transform` body over captured globals, so the same function serves
the serial loop and, with `HB_WORKERS` set (the standalone runner does on wide
terminals), a parallel presentation.

Text stays crisp on top: the HUD, the scene handle under each title card and
the finale's credits are ordinary glyph rows, and a scene can lay characters on
any cell through the text layer (`txt_put`, `txt_glyph`: a codepoint and a color
per cell, drawn over the cell's darkest pixel). The big title letters themselves
are pixels from a 5x7 font (`FONT5`), typed in with a shine sweeping across, at
the largest scale that fits.

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
jaiscript demoreel.jai -- pix half           # half blocks instead of sextants (for a font without them)
jaiscript demoreel.jai -- frames 1700        # headless self-test: smoke frames, sizes + stream hash + timing
jaiscript demoreel.jai -- frames 60 dump P   # also write frames 0/N-2/N-1 to P_<i>.txt (+ P_<i>.bmp, the pixel grid)
jaiscript demoreel.jai -- bench 60           # every scene mid-flight: script ms/frame and KB/frame
jaiscript demoreel.jai -- shot 8 at 4 dump P    # one frame of scene 8 at scene time 4 -> P.txt (as presented) + P.bmp
jaiscript demoreel.jai -- midi cues          # write the soundtrack's cues (one MIDI file a scene) and exit
jaiscript demoreel.jai -- video frames every 2   # the whole reel's frames as 24-bit text, with the cues, for a video
jaiscript demoreel.jai -- music 0            # the reel, silent
jaiscript demoreel.jai -- frames 90 w 120 scene 10 workers 1   # headless with rows drawn in parallel (the reel's default from 100 columns)
```

The soundtrack is procedural synthwave and the scenes hear it (`scenes/music.jai`).
At boot the script composes one cue per scene at 124 bpm: each scene has a mood (a
key and a progression of seventh and added-ninth chords, the keys travelling so no
two neighbours share one; a drum style on the power, electronic or 808 kit; General
MIDI instruments for bass, pad, arpeggio and lead; a level of energy). The bass
pumps in octaves, the gated snare and a clap hit two and four, every kick ducks the
pad and the arpeggio and they swell back over the beat (a sidechain written as MIDI
expression), the arpeggio and the lead echo on the dotted eighth, the lead sings a
two-bar motif from the reel's seed over each chord's tones, and electronic toms fill
into every fourth bar. The energy builds into each of the scene's declared hits and
drops on it: a reverse cymbal swells over the bar before a big hit, a snare roll
climbs through its last half bar, the band falls silent for the last beat, and the
hit lands a crash, a low boom, an orchestra hit and brass on the chord with the bass
dropping under it. Every cut lands with a crash.
The same score is data the scenes query every frame (`cue.kick_env(t)`,
`snare_env`, `hat_env`, `bass_env`, `chord`, `energy`, `hit`, `riser`), so the
pictures move with exactly the music being played, deterministically and with no
audio analysis. Every scene is a whole number of bars and starts on a bar line,
so the cues play back to back: the runner preloads them as MIDI files and
switches the frame a scene's transition starts.

`docs/site_tools/render_reel_video.py` turns the reel into a video with that
soundtrack, an H.264 MP4 and a VP9 WebM: it runs the `video` mode, renders every
kept frame through the examples page's terminal renderer across all cores, plays
the cues back to back through a small numpy synthesizer (each cue's drum kit with
a gated snare, toms and claps, a filtered-saw bass, a detuned pad and a square
arpeggio pumped by the score's sidechain, a vibrato lead, brass stabs, an
orchestra hit, a reverse cymbal), and hands both to ffmpeg, scaling by nearest neighbour so the pixels
stay crisp (`--seconds N` for a short cut, `--frames DIR` to reuse a dump).

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
jai_demoreel --pix half          # half blocks instead of sextants (--pix sext forces sextants at any size)
jai_demoreel --256color          # force the xterm-256 cube (--truecolor forces 24-bit)
jai_demoreel --smoke             # headless: frame-stream hash + perf
jai_demoreel --smoke --frames N  # smoke frame budget (default 1700)
jai_demoreel --capture I         # print scene I as plain text (ANSI stripped: block glyphs and spaces)
jai_demoreel --capture I --ascii # the same frame as a density map
jai_demoreel --reload-test       # headless hot-reload self-test
jai_demoreel --precompiled       # boot from demoreel.jaibite (saved on first run)
```

Keys while running:

| key | action |
| --- | ------ |
| `r` | hot-reload every `scenes/*.jai` from disk; a band under the HUD names the files that changed, or the file that failed to load with its error |
| `h` | step the HUD: two quiet chips on the bottom row (the scene; the VM's ms per frame, cores and fps), then the full status bar (backend, frame ms split into sim and draw, fps, pixel size) and a key bar that fills as the scene plays, then off |
| `f` | freeze the clock (render keeps running) |
| `n` / `p` / space / arrows | next / previous scene |
| `1`..`9`, `0` | jump straight to a scene (`0` = scene 11, the finale) |
| `q` / `ESC` | quit |

`jai_demoreel` runs one engine, on the VM: it links the library without the
interpreter, so the two backends are compared on the standalone runner
(`--backend=interp`, above). The HUD shows the backend and its frame cost. Ctrl+C, Ctrl+Break and SIGTERM
end the reel through the same exit as `q`: the cursor, the colors, the
main screen and (on Linux and macOS) the terminal's input mode come back.

## The scenes

| # | scene | what it is |
| - | ----- | ---------- |
| 1 | DONUT | a product shot that drops in on the beat: the donut sphere-traced per pixel as a distance field in its own frame (the dough torus, an icing layer with real thickness, a beaded lip, a wavy edge and drips, and sprinkles raised off it as capsules on a hashed grid in the torus's coordinates, catching the light and shadowing the icing), over a glossy walnut table that reflects it and takes its soft shadow, before a cyclorama with drifting bokeh; scored to its cue: the title bar is dark with a glint tracing the silhouette over the riser, on the next downbeat the donut drops in and lands as the key light slams on with a flash and a shake, then it hops on every kick with its sprinkles popping off the icing, the light pumps, the camera swings round on the bar lines and pushes in on the sprinkles before the spin-out; then it winds up over the riser (rocking back and spinning faster as the lights drop to a rim and the lens draws in), is tossed on the second big hit and flips end over end through the donut storm with the camera craning after it, is caught flat on the table on the next downbeat, rattling to rest in a splash of sprinkles while the camera steps round it on every beat, and the camera cranes over it and down into its hole for the cut (ten bars); it renders within a 26,000-pixel budget and scales up; behind it a donut storm falls through the set (the bokeh are out-of-focus donut rings), sprinkles erupt off the icing in closed-form fountains on the landing, the big accent and every beat between, and the push in is a dolly zoom |
| 2 | JULIA | the Julia set as a landscape flown low: every frame a 200x200 height field of a morphing c (orbiting the cardioid) is iterated across all cores, the smooth escape count raising ridges along the set's boundary and the inside a low lake the orbit trap ripples; each screen column is cast voxelspace-style across it (bilinear heights, slope light, fog to the horizon, a sky) from a camera that keeps clear of the ground; scored to driving sixteenths: kicks punch the terrain up and widen the lens, snares flash the sky, the chord turns the palette, the energy sets the speed; the riser climbs high over the set and the drop plunges into a canyon run between crystal ridges as c jumps to a new set, and the camera pulls up over the set; then the ridges grow and the flight sinks toward them over a second riser as the set turns into the Douady rabbit, the big hit slams it into a canyon run where the land snaps to a new Julia set on every beat (a flash, a punch and a palette turn each), and the last accent pulls it up into a climb as the stars thicken and stream past (ten bars); the set's inside is still water mirroring the ridges and the sky (rippling on the kick), a moon rides ahead laying a glitter path across the lake, stars turn overhead, the crystal ridge tops spark, and the frame banks into the turns |
| 3 | LAVALAMP | a forest of lava lamps raytraced in 3D: an endless grid of lamps on a dark floor, each a surface of revolution (a steel cone base, a tapered glass vessel, a steel cap) solved exactly as seven cone frustums once its bounding cylinder says the ray might hit, the ray walking the grid cell by cell (a 2D DDA) so the cost per pixel stays flat however many lamps are in view; glass gives a fresnel edge and a studio glint and bends the ray, and inside it a 3D metaball field (seven blobs rising off the heater and sinking as they cool, stretched by their speed, a pool on the base, a drip under the cap) is marched along the bent ray, each ray keeping only the blobs it passes near, the wax lit from the field's gradient and the liquid glowing with it; each lamp takes one of three blob simulations turned to its own quarter and its own colour family, the floor a pool of each lamp's light, all fogged with distance; the camera flies a spline with a mark on every bar line, out of one lamp's wax, up into a crane over the field on the first big hit, low between the rows, then a rolling swoop that dives into another lamp's wax on the second; the wax swells on the kick, burns with the section's energy, shifts hue with the chord and flashes on the hits, a rainbow wave rolling over the field after the first; it renders within 30,000 pixels and scales up; the floor is black glass that mirrors the near lamps (a second, shorter walk of the grid along the bounced ray), every lamp blooms into the haze as the ray passes it, each big hit sends a ring of heat racing across the floor and through the lamps it passes, and the camera sways between the rows on the beat; the dive lands on bar 7 and its close-up holds through a riser, the wax swelling and the lens creeping in, until the third big hit blasts the camera back out of the wax in a corkscrew up over the field, a lesser ring of heat leaving that lamp on every kick under a rainbow wave, swoops down into a lane on the next accent and races along it between the lamps (eleven bars) |
| 4 | PIPEDREAM | the 3D Pipes screensaver homage on a synthwave set, flown by a real camera: six chrome pipes grow one segment per tick through a 16x12x16 voxel volume, never intersecting, glossy elbow balls at every turn; every frame the whole sculpture is projected from the camera, each straight run a capsule impostor (a sphere swept between its end voxels, radius and 1/z interpolated along the screen segment, clipped to the near plane) and each elbow a ball, binned into bands of rows drawn across every core against a depth buffer; per pixel the nearest axis point gives a normal whose world-space reflection reads the set (sky, the horizon's hard bright band, the floor, a white rim wherever a grazing ray meets the sun), with specular, fog and a depth nudge so crossings intersect; the set is 3D too: a striped sun low in one direction, a sky graded by elevation, a floor whose grid lines stay one pixel wide rushing in with the energy, glossy with the pipes' mirror images and the sun's reflection thinning as the view steepens; ~2% of elbows are the jackodile's rainbow eye. Scored to its cue over eleven bars: generation one grows as the camera orbits up from the floor, swinging round on every bar line, and fills on bar 3 (a glint, a flash, a sweep); a CRT beam wipes it while the camera rises to a new high angle, generation two grows round a tunnel it may never enter as the camera cranes down, and fills on bar 7; the camera dives into the tunnel and flies through the sculpture toward the sun, lunging on every kick and banking, bursts out, swings round on bar 9, cranes up into the riser, and on bar 10 a white-hot surge races along every pipe in the order it grew while the camera sweeps round to put the sun behind the sculpture for the cut; the kick punches the lens and lights the floor grid, the snare flares the chrome, every chord recolours the sun, the growing tips burn white-hot and shower sparks, and the frame rolls, punched on the hits |
| 5 | BOIDS | a starling murmuration at sunset, flown through: 1,500 birds in 3D keep their distance from and match the heading of their nearest neighbours (a grid hashed every frame, steering in parallel chunks) while drawn to their places on a sheet that swings, folds and ripples on every kick; the camera leaves the shore, glides up to the flock, closes in to ride alongside it as near birds stream past as beating chevrons, then climbs away as the whole murmuration sweeps across the sun into a black veil, the sky panning as it turns; near birds are silhouettes, far ones specks in the haze, and wings turned broadside to the sun flash gold, so each turn of the flock sweeps a wave of light across it; late in the scene the flock gathers into a wheeling vortex; a falcon stoops through it twice, the first strike in slow motion on the downbeat of bar 4 with a shockwave of panic, a flash and a lens flare, the second shaking the frame; then the veil lifts off the water into a black sun, one dense sphere of birds wheeling over the real one, tightening and turning faster as the lens pushes in through the riser, and on bar ten's downbeat the falcon stoops through its heart and past the lens: the sphere bursts into streaks flung out from the heart, a shockwave of light rings across the sky with a flash and a shake, and the flock settles into a vast wheeling halo standing on the water round the sun, mirrored in the lake; scored to its cue (the kick punches the lens in, swells the sun's rays and ripples the fold, the snare punches lighter and sparks the lake's glitter, the bass breathes through the sky, each hit blazes the rays, the riser draws the light in); 11 bars |
| 6 | GLOBHUNTER | Wolfenstein-style DDA raycaster over an authored 24x24 keep, TEXTURED: coursed stone, mossy stone, fluted gold and a banner of animated rainbow fire (16x16 textures grown in script), floor and ceiling cast per pixel (a floor row and its ceiling row share one walk); LIT by flickering torches through a light map (each cell's steady torch sum, flickering with the torch that rules it, plus the muzzle on each shot), warm in the pools and cold blue in the gloom, under a vignette; billboard Grublins and Orglis brutes lit as spheres per pixel with a torchlit rim, eyes that glow in the dark, grins, wall-occluded per column; the camera hunts: it swings onto a mark in clear sight, bobs as it walks, and fires on cone lock — recoil kicking the view, muzzle flash lighting the hall, a rainbow tracer, a hit-flash and hit-stop, goo that bursts in 3D and bounces off the flagstones and walls, callouts (DOUBLE KILL, MULTI KILL), a pixel-font kill counter; the HUD scales with the screen It plays to its cue: the gun fires only on a beat (on the eighths through the bar after the big hit), footfalls land on the beat, the torches flare with the kick, the big hit on bar five's downbeat floods the hall with light and calls FRENZY, and the bar after the boss slows the walk and holds fire. Every burst lights the hall around it, the view quakes with each kill, the big hit drops into bullet time for two beats (the walk crawls, the goo hangs in the air), and on bar seven the garrison scatters as a BOSS GLOB twice a brute's size rises in the corridor ahead, eyes burning, swelling on every kick. The boss rises on bar seven, takes a hit on every beat for two bars, flashing, and bursts on bar nine's downbeat in bullet time with BOSS DOWN; then the keep falls: on bar ten it calls RUN, the torches gutter and strobe back up on every kick, the view trembles, stone falls from the vault on every snare, an ambush rises and the gun fires on the eighths as the camera sprints for the east wall, its cracks glowing hotter with daylight; on bar twelve's downbeat the wall blows in (stone flying at the lens, the frame whiting out, KEEP CLEARED) and the last bar walks out through the breach as daylight blooms over the frame. It runs thirteen bars. |
| 7 | CANYONRUN | the Comanche (1992) VoxelSpace algorithm flown as a golden-hour drone shot: a 512x512 wrapping heightmap by diamond-square, a U-shaped gorge carved along a wrapping meander, the land terraced into mesas and stepped cliffs; the colormap is baked with light in parallel rows (a low sun's shadows marched across the height field, diffuse from the normals, ambient occlusion from the height field against its blur) and materials (river, wet sand, a green floor, scrub, mesa tops, pale peaks, ten strata colours); 120 geometric depth steps of 16.16 fixed-point marching with a per-column y-buffer and a sub-texel jitter per column and step, cliff faces painted per pixel with the rock layer at the height each row looks at (integer inverse projection), grain on rock and ground, twelve haze banks; the river mirrors the sky about each column's horizon with ripples and the sun's glitter, the sky is a perspective cloud deck lit by the sunset; the camera skims the river, banks through the bends, climbs out over the rim for the reveal and dives back in It plays to its cue: the climb out runs through bar three to the reveal on bar four's downbeat, where the sun flares with the hit, the dive back in starts on bar six, and the camera nods on the kick and leans on the snare. Over it: dust streaks rush past the lens (longer on the kick), a flock of birds beats across the sky, and each hit bursts a fan of god rays from the sun across the sky; the drone flies faster and skims lower. After the dive on bar six the drone skims the river; from bar nine the gorge pinches into a slot canyon between towering shadowed walls, the river racing white for a lip, and on bar eleven's downbeat the drone shoots over the falls into a wide basin, drops to skim a lake mirroring the sky, then climbs out and turns into the sun as it blazes over the rim; the kick nods the nose and punches the lens in, the snare leans the drone. It runs twelve bars. |
| 8 | CHROME | the Amiga Juggler's grandchild: a ray tracer, one ray per pixel across every core — three tinted chrome spheres bounce on a checkerboard running to a hazy horizon, one reflection bounce, hard shadows, a sun glint; the ray returns WHAT IT SAW as a class and each sphere looks that up in its own tinted copy of the class table; the middle sphere is glass (Snell refraction in and out at index 1.5, the world upside down through it, a mirror rim at grazing angles), the dark tiles are polished and show the spheres standing over them, the camera cranes up from a floor-level dolly, whips around at the middle and pulls back high as the bounces fall into step, and star sparkles bloom on the brightest glints It plays to its cue: every bounce is a parabola landing on a beat (one sphere on each beat, one every other, one on the offbeats), the camera rides the riser in and whips around on bar three's downbeat, and in bar four the spheres fall into step until they land as one on bar five; the glints flare on the hits and twinkle on the snare. A ring of six small chrome satellites orbits the big sphere, spinning up with the energy and the kick and flung wide on each hit (one bounding sphere spares rays that miss the ring), a low sun rides in every mirror, and each hit sends a glowing shockwave racing across the checkerboard from under the spheres. Through bar seven the three whirl inward ever faster and crash together on bar eight's downbeat (a big hit that whites the frame out) into one giant faceted mirror ball: every tile mirrors one piece of the world, whole tiles flash on the kick, and the ball throws wheeling rings of light spots across the checkerboard that swell on the kick; the camera dives in around it and whips back on the bar-nine accent. The kick punches the lens in and the hits shake the camera's roll. It runs ten bars. |
| 9 | OSSUARY | a story in eight acts over 25 bars (48 s): a horned, three-eyed skull on a spider's body sleeps on a raymarched field that morphs between three triply periodic surfaces (gyroid, Schwarz P, Schwarz D) under a flowing domain warp, its walls dark teal and quiet; it wakes as its crown of flame catches and explores the sheet on eight three-segment IK legs in a tetrapod gait, the sheet melts away into a chamber, it climbs its dragline to the hub, casts the spokes one by one, lays the capture spiral from the rim inward and waits head-down while dew glints on the pale silk; a moth with eyespotted wings strikes on bar 15's downbeat (the cue's big hit), the strike rings out through every thread, the spider runs it down, turns and wraps it in a spinning cocoon, and the camera draws back on the web; then the hunter turns on the viewer: the camera swings round in front of its face, its eyes swell and kindle, its crown roars up and the chamber darkens as the camera creeps in through bar 22, and on bar 23's downbeat (a second big hit) it lunges at the lens, jaws wide, shaking the frame, then holds the frame glaring, its jaw snapping on every kick, and roars again with a burst of embers on bar 24; the walls breathe with the music (veins lit on the kick, a ring of light leaving the creature on every kick, the wall's hue on the chord, its light on the cue's energy); the silk is drawn over the trace and depth-tested, the moth and cocoon are distance fields in the same march, rows trace in parallel at most about 4,600 rays and scale up to the grid; a long lens keeps the creature big in frame and punches in on each hit, shafts of light fall through the chamber from high off the frame (each a line through the light, picked out by its slope) with dust drifting in them and brightening on the kick, dew glints along the spokes and the spiral, and the strike shakes the frame; from the strike through the wrap, while the creature fills the frame, fewer rays are traced, and again from the draw back through the eighth act |
| 10 | MAELSTROM | ink in water, simulated with Jos Stam's stable fluids (vorticity confinement, warm-started SOR pressure, semi-Lagrangian advection on a half-size grid, the ink traced per pixel, hue blended as a premultiplied vector) and seen in 3D: the tank lit from above becomes the texture a camera flies over, the ink's density raising it into glossy mountains of colour cast column by column (VoxelSpace style, nearer ink hiding farther, side faces shaded darker) over a floor of caustic light; the camera sways round the tilted tank, swings on the starburst and dives low over the whirlpool to hold on it; then the whirlpool winds tighter and sinks into a funnel through bar nine while the camera circles in, and on bar ten's downbeat it erupts: a geyser of ink towers out of the middle, sixteen jets fire a rainbow ring across the tank, a shock ring races over the water and the camera is thrown up and back; the geyser falls back pulsing on every kick and spouts again on bar eleven; the jets pump on the kick, the inks turn with each chord, the snare and the hits flare the light, the energy feeds the curl, the ink mountains bob on the kick, every kick from the whirlpool on sends a lit ring with a dark trough out across the surface, the kick punches the lens in and the hits shake the horizon; hits on the starburst (bar 4), the bloom (bar 6) and the geyser (bar 10), an accent on the second spout (bar 11); 12 bars |
| 11 | FINALE | JaiScript in eight dimensions, a 100-second music video on the soundtrack's 124 bpm: after a title sequence and a four-beat count-in it counts the language in, ONE (scripting), TWO (serialization), THREE (reflection), FOUR (hot reload), FIVE (parallel), SIX (signals), SEVEN (debugging), EIGHT (embedding), four bars a slide (`scenes/show.jai`): the counted word slams in on a sweep of light, a code card slides in and types a real snippet in syntax colours (every script one runs on the runner), a live demonstration plays beside it (Spark objects, an archive round trip, a reflect:: inspector, a file edited mid-loop, eight cores on one frame, a signal fanning out, a paused debugger, C++ and script trading calls), captions run along the lower third and a checklist lights; every slide opens one more axis of an 8-cube (256 vertices, 1024 edges) turning in a window above the demonstration as a world turns in Fez, in eased quarter-turns snapping on the beat, on Escher's hyperbolic plane: the {8,3} tiling in the Poincaré disk, folded per pixel by its three mirrors, three-coloured, opening ring by ring with the dimensions and mirrored through the circle at infinity to fill the screen; with all eight shown the cube takes the screen, the snaps double up every half beat and the whole scene turns, swirls and ripples as one per-pixel map while the plane warps; the cube shatters into a vortex of sparks that wind onto JAISCRIPT, which slams in with a flash, shockwaves and a shake as the fire wall erupts (the fire scene in `fire.jai`), the flames pump in rainbow on the beat with the plane behind them, then the letters cool to neon and the credits roll; for an encore on bar 49 the word reignites in rainbow fire with a second slam, its shockwaves and the plane roaring back, the credits clear to their farewell, and the frame dithers to black over the last bar; 52 bars |

Between scenes, two beats from the bar line, each scene hands over its own way:
DONUT dives through its hole into JULIA (zoom through), JULIA swirls into the
lamps, the wax ripples into the chrome pipes, the pipes iris onto the flock, the
flock melts like Doom into GLOBHUNTER, the hunt zooms out over the canyon, the
canyon ripples into CHROME, the glass shatters into OSSUARY, the web swirls into
the ink, the ink dives into FINALE and FINALE irises back to the start. The
motion transitions read the departing frame through a moving map (a turning
zoom, tiles spinning away, a ring wave, a vortex); the wipes merge two grids per
pixel into the combined palette. A title card with a demoscene handle opens
every scene for one bar, in one of three styles taking turns (a band opening
from the middle, a shutter closing from both sides, a glitch slipping on the
hats), pumping on the kick. A scene can set `pixel_budget`: past it the scene
renders smaller and the reel scales it up, so the heaviest scenes hold their
frame rate on large terminals.

The reel opens cold (`scenes/intro.jai`, four bars, played once): a dead signal
rolls and flickers over the riser and a CRT line powers on, JAISCRIPT slams in on
the first downbeat split into red and cyan ghosts with a chromatic shockwave over a
warping starfield of blue and rose stars, a violet glow breathing behind the letters
on the kick, the tagline types itself, and on the second impact the logo bursts into
sparks of its own colours behind a second shockwave, streams into the warp and
whites out into DONUT. Every cut lands with a dithered white flash on
its bar line, and every big hit lands on the whole frame: over the bar before it an
iris closes in on the picture, tightest through the score's beat of silence, and the
hit throws it open with a shake.

The reel hot-reloads itself on screen: FINALE's FOUR slide turns a star with
`live_spin()` (defined in `show.jai`), and on the slide's beat it imports
`scenes/live/spin_tweaked.jai`, which redefines the function. The engine swaps the
code under the running scene; the star keeps its angle and turns six times as fast
from the next frame. A module runs once a process, so a second pass through the
reel finds the tweak already live and says so; `r` re-executes `show.jai` and
puts it back.

## Measured numbers

The whole reel on the standalone runner (100x40 cells = 150x120 = 18,000 sextant
pixels per frame, seed 20260705, fixed dt 1/30, transitions included, smoke durations
of 3 s per scene) reports its own timing: `jaiscript demoreel.jai -- frames 1700`
prints the render cost per frame (`demo_frame` alone: update, render and
presentation), the cost including the shim's per-byte script hash of every
frame, and the mean frame size (45 KB). Measure it on a quiet Release build.

Per scene, three seconds in (`-- bench 20` on the VM, `-- bench 10` on the
interpreter, no transitions, same size):

| scene | VM ms | interp ms | KB/frame |
| ----- | ----- | --------- | -------- |
| DONUT | 13.2 | 108.7 | 56 |
| JULIA | 7.8 | 143.1 | 44 |
| LAVALAMP | 21.6 | 299.8 | 52 |
| PIPEDREAM | 5.6 | 62.2 | 51 |
| BOIDS | 9.8 | 93.9 | 38 |
| GLOBHUNTER | 4.1 | 52.9 | 42 |
| CANYONRUN | 4.1 | 58.9 | 91 |
| CHROME | 7.3 | 79.2 | 36 |
| OSSUARY | 22.9 | 297.8 | 47 |
| MAELSTROM | 12.3 | 213.4 | 37 |
| FINALE | 4.3 | 69.3 | 6 |

Every scene holds 30 fps on the VM, OSSUARY and the raytraced LAVALAMP heaviest at
about 22 ms; at 120x40 sextants with rows drawn in parallel the runner holds 29 to
30 fps through every scene's heaviest bars.

- Parity: frame streams **byte-identical** across backends over 1,700 frames,
  every one of the eleven scene entries: shim stream hash `512157162` on
  both. The C++ host's `--smoke` hash is FNV-1a 64 over the same frames:
  `c0086107f2b4bac1` (the host's HUD
  is off in smoke, so its frames are the shim's bytes).
- `--reload-test` re-executes every scene file into the engine. The
  presenter's globals live in `util.jai`, so a reload resets them and the reel
  re-binds them on the next frame; a reload mid-transition cancels the
  transition (the live scene rebuilds), so no scene reads a reset buffer.

### Scoreboard

`ossuary_bench/` keeps the original OSSUARY (the skull-spider on the morphing
gyroid, before it spun its web) as a frozen pair: `skull_shaped.jai`, tuned for
the VM, and `skull_natural.jai`, the same algorithms and the same frames written
the way a user would write them (scene hash `afd81a7d28930a94` from both at
`--pix half --workers 1`), without the spellings chosen to fit the VM's fast
paths (one `sk_world`, the warp's amplitude read left of its sine, ternary folds,
one `sk_crown` holding its flames, the row pushing `sk_trace`'s result). The
ratio of their serial OSSUARY rows, natural over shaped, measures how general
the VM's fast path is: at 1.0 plain code runs as fast as code written for the
VM. Drop either one into a copy of the scenes directory as `skull.jai`:

```
cp -r scenes /tmp/shaped_scenes && cp ossuary_bench/skull_shaped.jai /tmp/shaped_scenes/skull.jai
cp -r scenes /tmp/natural_scenes && cp ossuary_bench/skull_natural.jai /tmp/natural_scenes/skull.jai
jai_demoreel --perf 30 --scene 8 --backend vm --workers 1 --pix half --scenes /tmp/shaped_scenes   # shaped
jai_demoreel --perf 30 --scene 8 --backend vm --workers 1 --pix half --scenes /tmp/natural_scenes  # natural
```

## Console performance

Frames carry two colors per cell. At 100x40 in sextants they average about 53 KB,
from 34 KB for CHROME to 97 KB for CANYONRUN, whose shading changes nearly every
cell; FINALE runs from 7 KB in its slides to 115 KB under the dancing 8-cube.
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
  override the choice, and `--ascii` replaces the block glyphs with one density
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
2. Open `scenes/lava.jai` in an editor; change the wax colors (the
   `gfx.rainbow_rgb` phase in `build`), or the blob speeds in `update`.
3. Press `r` in the console window.

The engine re-executes every scene file (hot reload with instance migration) and
the current scene rebuilds — the change appears without restarting. `main.jai` is
deliberately never reloaded: it owns the persistent globals.

For three seconds a green band under the HUD names the files whose text changed
and the scene that was rebuilt (`RELOADED  lava.jai  LAVALAMP rebuilt at 4.2 s`).
An edit that does not parse or check loads nothing: the file keeps running its
previous code, every other file still reloads, and a red band shows the first
error with its source line and caret for eight seconds. Fix it and press `r`
again.

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

CHROME (`shot 8 at 4` — three chrome spheres over the checkerboard; the `@@`
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

GLOBHUNTER (`shot 6 at 3.1` — the kill counter top left, a Grublin with glowing
red eyes in the crosshair down a cold corridor while the goo of the last one
bursts around it, the pistol at the bottom):

```
::-====--:::::::::::::::::::::::.....::::::::::::::::::::::::::::::::::::::::::::::::::::::--====-::
::@-=##:#*@**:@:::::@:::::*****::....:*****:*****::::::::::::::::::::::::::::::::::::::::----====-::
::@*#-----@.-:@.::::@.::::****::::....@.**@.:.:**.:::::::::::::::::::::::::::::::::::::-==---====-::
::%.**=---%.==%.::::%.::::::::%.::::::%+.:%.:++.:::::::::::::::::::::::::::::::::::::-====---====-::
::*.==*:*++++-*++++:+++++:++++:::::::::+++::+++++::::::::::::::::::::::::::::::::::-======---====-::
::----=---========-::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::-========---=----::
::--------=========::-::::::::::::::::::::::::::::::::::::::::::::::::::::::::-::=========--------::
:::---------=======::---:::::::::::::::::::::::::::::::::::::::::::::..:::::---::=======---------:::
::::::-:-------====::-----::::::::::::::::::::::::::::::::-:::::::::::::::-----::====-------:-::::::
::::::::::--------=::-------::::::--::::::::::::::::::::::::::::::::::::-------::=--------::::::::::
----:::::::::------::-------:-:::::::::::...:::::::::::::::--:::::-:::-:-------::------:::::::::---=
---===---::::::::--::-------:==-:::::::::::::::::::::.*==:::::::::::-==:-------::--::::::::---===--=
---====---=::::::::::-------:====:::::::::=:::::::::::+++:::::.::::====:-------::::::::::=---====--=
---====---=:::==---::::::---:-===:--:::::.......#--.....::...:::--:===-:---::::::---==:::=---====--=
---====---=:::=======---:::::----:--:-#++:::+=::+++:::::::::::-:--:----:::::---=======:::=---====--=
---====---=:::==========:=---:::-:--:-===::::::::::::::::::--:-:--:-:::---=:==========:::=---====--=
---====---=:::==========:======:--:::::-==+-----=-::::::-===-:::::--:======:==========:::=---====--=
----------=:::==========:---===:=----:-:--+:-@....::::.-=+--:-:----=:===---:==========:::=----------
-----------:::==========:---===:=----:-===-=:#=:--%-:.::=-==--:----=:===---:==========:::-----------
-----------:::==========:------:=----:--=+-=-:+=+-#-+-:-=-+=--:----=:------:==========:::-----------
:::::::::::::::::::::::::::::::::::::::-===:::###==-***#===--:::::::::::::::::::::::::::::::::::::::
::::::::::::::::::::::::----:--==:--:-:==++:=:-*--%---:++++=-:-:--:==--:----::::::::::::::::::::::::
::::::::::------===::=======:====:--:-:=---=::::-:*:-:::=-+--:-:--:====:=======::===------::::::::::
::-====---=========::=======:----:--:::-=+-++=-++++**-++=-+=::::--:----:=======::=========---====-::
::-====---========-::=======:----:::::-----==-=====*==-====---:::::----:=======::-========---====-::
::-====---=--------::=======:-:::----:---#=#=#++#++--==-===:--:----:::-:=======::--------=---====-::
::-----------------::====--:::-:=----++:-+++++=====-----------:----=:-:::--====::-----------------::
::-----------------::--::::-===:=---:==--:::::::::::::--::::::::---=:===-::::--::-----------------::
::-----------------::::::---=--:------:--:---------------::---------:--=---::::::-----------------::
::-------------::::::-==:------:--=====-------==::=====---------==-::------:==-::::::-------------::
::----------::::::-=====:----------------------**##--------------==-::-----:=====-::::::----------::
::-----::::::::-========:------------::--------**++--------:::-------------:========-::::::::-----::
:::-::::::::::==========:-------===-::---------**++-----===-::-==========--:==========::::::::::-:::
::::::::::=:::========---------===-::----------**++------===:::-===========---========:::=::::::::::
::::::----=:::=====---=-------===-:::--------**##++++----====:::=======----===---=====:::=----::::::
:::-===---=:::===------------:::::::---------**++++++--------::::------------------===:::=---===-:::
:--====----:::=----::::--------------------**##++++++++-----------------------:::----=:::----====---
---====----:::---:---------------------%%--**++++++++++--%%-----------------===-=::---:::----====--=
---=-------:::-:---------------------%%%%--++++++++++++--%%%%---------------------:::-:::-------=--=
-----------:::-------------:::::---%%%%%%%%--++++++++--%%%%%%%%--------------------::::::----------=
```

DONUT (`shot 1 at 4` — the product shot: the iced torus hopping over the walnut table, its reflection below):

```
:::::::::::::::::::::::::--------------------------------------------------:::::::::::::::::::::::::
::::::::::::::------------------------------------------------------------------------::::::::::::::
------------------------------------------------------------=---------------------------------------
-------------------==---------------------=====----------+****+-------------------------------------
----------------=*******+----------------++++++*=-------*******=------------------------------------
----------------*****####+---------------+++++++*-------+******-------------------------------------
----------------##########---------------++++++++--------+++++--------------------------------------
----------------+#########----------------=+++++----------------------------------------------------
------------------+++++++------------------------#-#------------------------------------------------
----------------------------------------=+*-**#:####+#**#+**----------------------------------------
-----------------------------------===**#**##**%###*#******%*=--=-----------------------------------
-----------------------------------=+###%#+##**###%**#*#*+**#***=*----------------------------------
----------------------------------***#%%***+*+=+******=#####******#*--------------------------------
--------------------------------*###%***++-==+==++++**+**#%%#*+**+*+*=------------------------------
::::::::::::...:.:..........:.*:**%%%#*=++=+======++++***+*#*%#*******:........:........::::::::::::
...............:.:...........:+*#%#***==++===-----===++****%#*@#+***+=+-.:.:.:.:...........:...:....
....:.............:....:..:.:+##%%#*==+++==---::..:.:-+******%@%***+**+=.......:.........:.:..:.....
..:.....:..:..:....::.::.:::-**#%@#*+*++==--:::::::::.::*###+#%%*#****=*=...:::....:.:..:..:..:..:..
::.:..:::.:::::::::::.:::::::+++%@#*-*+++===::::::::::::+###*%#%++*=**++=:::.::::::.::::.::.::.::...
::::::::::::::::::::::::::::%%##%#%*+++++++=:::::::-::::%##*#%@#***+*=+++:::::::::::::::::::::::::::
.:::::::::::::::::::::::::::%####@%+******++=----------*####%*%*+****==++:::::::::::::::::::::::::::
::::::::::::::::::-:----:---***#+%%*#=*******++-----**###+#=@%******++=--::---:::--:::-:::::::::::::
:::::::-:-:-:---:----:------**+*#+#%%%*=+++****##########%%%%+***+++++-=--------------:---::::::::::
:::---:-:--------------------*#++*%##%*%******+*##+####*###***+*=+++===----------------------:-:--::
-----------------------====-==*+****+*++#####*#####%*##%#*+***+++=====-::::-::----------------------
---------------------====-======%*#+*+***+#**#####%#*+****+=*+=+=====----:::::::::-=----------------
-------------====-============++=*%+****+**+***:+**-*****+++++=-====------::::-:::::-=--------------
----------=============+====++++++++=*=*+*+*****++*+*++=-+=++=====---:-----::::-::::--===-----------
-------==============+++++++++++++++++**++++++=++=++++=+======--------:-----::::------======---=----
----===============+++++=++++++++++******++++===+-=+==-=====-----------:-----::----===========---==-
-================+++++++++++++++++********===-========--------------------------================---=
================+++++=+++++++++++**********+===--:-------------------------==++++=================--
==============+++++=++++++++++++*+**++********++++===----=-==-----====++++++++++++==================
===============++==+++++++++++++++*+++****+======*****++++++*++++++++++++++++++++++=================
==================++++++++++++++++++==-:::::::::::::::::===***++++++++++++++++++++++================
=================+++++++++++++++++-:::::::-::::::::::::::::::-=+++++++++++++++++++++================
=================++++++++++++++++:=----:---::-:-::-::::-:::::::::=++++++++++++++++++================
================++++++==++++++++:-----------------::::-:::::::::---=++++++++++++=++++===============
==============+++++++==+++++++++=:-==-=-----------------::::::::::--=+=+++++++++==++++==============
=============++++=====++++++++++=====-=-=-==----------------::-::----+++++++++=====+++==============
```

FINALE afterglow (`shot 11 at 84.8 ascii 1`: the fire is out, the word glows in neon and the credits roll):

```


                                                                                   .
      ********    +++++   ********    ######    %%%%%%  #######   ********  +++++++   ********
           **   ++     +      *     ##        %%        ##     #      *     ++     +      *
           **   ++     +      *     ##        %%        ##     #      *     ++     +      *
           **   ++++++++      *       #####   %%        #######       *     +++++++       *
      **   **   ++     +      *            #  %%        ##  #         *     ++            *
      **   **   ++     +      *            #  %%        ##   ##       *     ++            *
        ***     ++     +  ********  #######     %%%%%%  ##     #  ********  ++            *






                                       THE JAISCRIPT DEMOREEL

                                  eleven scenes . six transitions
                              every pixel computed by the jaiscript vm

                                              ONE  scripting
                                              TWO  serialization
                                            THREE  reflection
                                             FOUR  hot reload
                                             FIVE  parallel
                                              SIX  signals
                                            SEVEN  debugging
                                            EIGHT  embedding
```

## Idioms

- **`var&` for hot arrays and the frame grid.** Assignment copies arrays and objects, so
  `var alias = field_array;` is a copy and writes through it never reach the field.
  `var& alias = field_array;` binds a reference; the scenes bind their hot arrays this way at
  the top of `update` and `render`, and a scene paints through `render(var& grid)`.
- **Compound element stores.** `arr[i] += v` resolves the element once, where a read followed
  by a write resolves it twice; MAELSTROM's jets, whirlpool and pressure solve write the flow
  that way.
- **Two resolutions.** MAELSTROM's pressure solve is most of a fluid frame, so the flow runs on
  a grid of half the pixel resolution (a quarter of the cells) and only the ink, which carries
  the detail, moves per pixel through the bilinearly upsampled flow.
- **A scene's `replay`.** A jump (`--perf`, `--capture`, a shot) replays a scene's updates so it
  arrives mid-flight; three seconds rebuild most scenes, while MAELSTROM's water is its whole
  history, so it sets `replay` to its full length and a shot at second 13 shows second 13.
