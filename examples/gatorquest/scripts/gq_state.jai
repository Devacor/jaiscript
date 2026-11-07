// Gator Quest - the live world. Every declaration here is state, written without
// an initializer, so a reload of this file keeps what it holds (sim_reset and
// world generation give it its starting values).

import "gq_types.jai";

// ---- run
int SEED;
int TICK;
int RNG_S;
int NEXT_SER;
bool WORLD_READY;

// ---- terrain: every world's tiles in one flat array, W_OFF[w] = first index
array<int> W_OFF;
array<int> W_W;
array<int> W_H;
array<int> TILES;
array<int> TVAR;
array<int> REGROW;            // tick a depleted tile (felled tree, mined vein) comes back; 0 = fine
array<int> OCC;               // mobile index + 1 standing on each tile, 0 = empty
array<int> W_WEATHER;
array<int> W_WEATHER_T;
array<int> W_ARRIVE_X;
array<int> W_ARRIVE_Y;
array<int> W_BOSS_T;          // tick the world's boss may respawn

// ---- places (worldgen output; regenerated from the seed)
array<int> GATE_W;
array<int> GATE_X;
array<int> GATE_Y;
array<int> GATE_DEST;
array<int> TOWN_W;
array<int> TOWN_X0;
array<int> TOWN_Y0;
array<int> TOWN_X1;
array<int> TOWN_Y1;
array<string> TOWN_NAME;
array<int> CHEST_W;
array<int> CHEST_X;
array<int> CHEST_Y;
array<int> CHEST_TIER;
array<int> CHEST_T;           // tick the chest refills (0 = full)

// ---- mobiles (players, npcs, monsters): parallel typed arrays, kind < 0 = free slot
array<int> MOB_KIND;
array<int> MOB_SER;
array<int> MOB_WORLD;
array<int> MOB_X;
array<int> MOB_Y;
array<int> MOB_PX;
array<int> MOB_PY;
array<int> MOB_MOVE_T;
array<int> MOB_MOVE_D;
array<int> MOB_DIR;
array<int> MOB_HP;
array<int> MOB_HPMAX;
array<int> MOB_MP;
array<int> MOB_MPMAX;
array<int> MOB_TARGET;
array<int> MOB_AI;
array<int> MOB_NEXT;
array<int> MOB_ATK;
array<int> MOB_CAST;
array<int> MOB_FLAGS;
array<int> MOB_PLAYER;
array<int> MOB_SPAWNER;
array<int> MOB_HOMEX;
array<int> MOB_HOMEY;
array<int> MOB_POISON;
array<int> MOB_BURN;
array<int> MOB_SLOW;
array<int> MOB_BUFF;
array<int> MOB_HIT_T;
array<int> MOB_OWNER;
array<int> MOB_AUX;
array<int> MOB_ROLE;
array<int> MOB_LEVEL;
int MOB_LIVE;

// ---- spawners
array<int> SP_W;
array<int> SP_X;
array<int> SP_Y;
array<int> SP_KIND;
array<int> SP_MAX;
array<int> SP_RAD;
array<int> SP_DELAY;
array<int> SP_NEXT;
array<int> SP_ALIVE;

// ---- items on the ground
array<Item> GI_ITEM;
array<int> GI_W;
array<int> GI_X;
array<int> GI_Y;
array<int> GI_SER;
array<int> GI_EXPIRE;
array<int> GI_OWNER;          // player slot with first dibs (loot rights), -1 = anyone

// ---- players (slot-indexed, 16 slots)
array<Character> CHARS;
array<int> IN_MOVE;           // held movement direction per slot, -1 = none
array<array<string>> IN_QUEUE; // discrete actions per slot, consumed by the next tick
array<string> CHAR_ARCHIVE;   // characters of players who left, as save text (name-keyed on rejoin)
array<array<int>> IN_STEP_SEQ; // predicted steps from a client: sequence numbers and directions, applied at the player's pace
array<array<int>> IN_STEP_DIR;
array<int> IN_ACK;            // the last step sequence each player's host has processed (echoed in snapshots)
array<int> IN_DROPPED;        // the newest sequence dropped because the queue was full (acknowledged once it drains)

// ---- events produced by the current tick (routed into snapshots, then cleared)
array<int> EV_KIND;
array<int> EV_W;
array<int> EV_X;
array<int> EV_Y;
array<int> EV_A;
array<int> EV_B;
array<int> EV_C;
array<int> EV_TO;             // player slot, -1 = everyone near (EV_W, EV_X, EV_Y)
array<string> EV_TEXT;

// ---- a spectator camera keeps its surroundings awake (title screen); -1 = none
int CAMERA_W;
int CAMERA_X;
int CAMERA_Y;

// ---- autopilots
array<Bot> BOTS;

// ---- snapshots and the client side
array<int> NET_SHEET_SENT;     // host: sheet version last sent to each slot
shared_ptr<ClientView> CV;          // the view the renderer draws

// ---- dev/stat counters (not hashed)
int STAT_KILLS;
int STAT_SPAWNS;
