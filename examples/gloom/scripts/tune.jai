// The showcase look's tuning: light, fog, camera feel, sound. Every value here
// is read when the look builds its tables, or every frame, so `--dev` reloads
// this file while the game runs (save it and the next frame shows the change).
// Nothing here touches the simulation: STATE_HASH never moves.

// how much of a floor's mood light reaches a surface with no lamp over it
float AMBIENT = 0.34;

// lamps hang over every few tiles of open floor and light a pool beneath them
float LAMP_RADIUS = 3.2;
float LAMP_POWER = 1.0;
int LAMP_SPACING = 5;            // no two lamps closer than this many tiles

// the muzzle flash, projectiles and blasts carry their own light
float MUZZLE_RADIUS = 4.6;
float SHOT_RADIUS = 2.6;
float BLAST_RADIUS = 4.2;
float LIGHT_GAIN = 1.35;         // how bright a fully lit texel gets

// light channels: lamp (the floor's own mood light), fire, hex, acid, red lock, blue lock
array<math::color> LIGHT_COLORS = [
	math::color(1.0, 0.86, 0.62),
	math::color(1.0, 0.56, 0.18),
	math::color(0.36, 0.86, 1.0),
	math::color(0.46, 1.0, 0.36),
	math::color(1.0, 0.18, 0.12),
	math::color(0.22, 0.42, 1.0)
];

// camera feel
float BOB_PIXELS = 1.6;          // view bob while walking, in pixel rows
float SHAKE = 1.0;               // screen shake strength: hits, blasts, the scattergun
float VIGNETTE = 0.55;           // how dark the view's corners get

// sound effects through the runner's play_sound, when it has one
bool SOUND = true;
