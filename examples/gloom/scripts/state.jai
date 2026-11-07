// Persistent globals that must exist before anything else loads.
// The world handle G lives in game.jai (typed shared_ptr<Game>, declared under
// the class); RNG is the host-bound seeded rng and the ONLY randomness allowed.
var RNG = null;
