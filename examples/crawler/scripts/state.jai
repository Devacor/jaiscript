// The run's persistent globals. The dev reload ('R') leaves this file out, so the live run
// survives while every other file is redefined under it.
var G = null;         // the Game
var RNG = null;       // the seeded Rng, the only randomness the rules touch
var PILOT_CO = null;  // the autopilot's key coroutine (--smoke)
