// The tweak EIGHT's FOUR slide lands live: show.jai defines live_spin() at 1.0, and
// halfway through the slide the reel imports this file, which redefines it. The
// engine swaps the function under the running scene; the star keeps its angle and
// turns six times as fast from the next frame.
function live_spin() -> float { return 6.0; }
