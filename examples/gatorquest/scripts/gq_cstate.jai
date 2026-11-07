// Gator Quest - client-side live state (camera, particles, floating text, UI
// modes, networking, prediction). Like gq_state.jai, every declaration here is
// state written without an initializer, so a reload keeps it; client_state_init
// gives the starting values.

import "gq_state.jai";

// ---- camera (world pixels)
float CAM_X;
float CAM_Y;
bool CAM_SNAP;
int SHAKE_F;
int SHAKE_P;
int FRAME;
int NOW_MS;
int PRNG;
int LIGHTNING_F;
int SHADE_WORLD;
int SHADE_GHOST;
int VIEW_TX0;
int VIEW_TY0;
int TITLE_X;
int TITLE_Y;
string TITLE_LAST;             // the last gator played (the title's default name)

// ---- particles (tile-unit positions)
array<float> P_X;
array<float> P_Y;
array<float> P_VX;
array<float> P_VY;
array<int> P_LIFE;
array<int> P_MAX;
array<int> P_COL;
array<int> P_FLAGS;

// ---- floating text
array<float> FT_X;
array<float> FT_Y;
array<string> FT_S;
array<int> FT_COL;
array<int> FT_T;
array<int> FT_SCALE;

// ---- projectiles
array<float> PR_X0;
array<float> PR_Y0;
array<float> PR_X1;
array<float> PR_Y1;
array<int> PR_T;
array<int> PR_DUR;
array<int> PR_KIND;

// ---- weather flakes
array<float> WX_X;
array<float> WX_Y;

// ---- per-mobile presentation
map<int, int> FLASH;           // serial -> frame the hit flash ends
map<int, float> VIS_X;         // serial -> last drawn position (for smooth motion)
map<int, float> VIS_Y;

// ---- banner
string BANNER;
int BANNER_F;
int BANNER_COL;
string BANNER_SUB;

// ---- UI
string UI_MODE;                // play, pack, skills, journal, spells, craft, dialog, shop_buy, shop_sell, bank, chat, help, map, menu, worn
int UI_SEL;
int UI_SCROLL;
string CHAT_BUF;
string UI_TOAST;
int UI_TOAST_F;
int TILE_ZOOM;                 // 0 auto, else forced tile size
int PANE_WIDTH;
int SCREEN_COLS;
int SCREEN_ROWS;
bool MUSIC_ON;
bool SFX_ON;
int MUSIC_CUR;                 // the theme playing (-1 title, -2 death, else a world), -99 none

// ---- networking: the host side (one NetServer, clients by id)
shared_ptr<NetServer> NET_SRV;
int NET_PORT;
map<int, int> NET_SLOT_OF;     // client id -> player slot (after HELLO)
map<int, int> NET_CID_OF;      // player slot -> client id
array<int> NH_KIND;            // host inbox: 0 connect, 1 packet, 2 disconnect
array<int> NH_ID;
array<string> NH_TXT;
int NET_BYTES_OUT;
int NET_SNAPS_OUT;

// ---- delta snapshots: per player slot, the last record sent for each visible mobile and item
array<map<int, string>> SNAP_MOBS;
array<map<int, string>> SNAP_ITEMS;
array<int> SNAP_WORLD;
bool SNAP_DELTAS;

// ---- networking: client sessions (the game uses one; the net test drives several)
array<shared_ptr<NetChannel>> NCS_CH;
array<array<string>> NCS_INBOX;
array<shared_ptr<ClientView>> NCS_VIEW;
array<string> NCS_STATUS;      // connecting, hello, joined, refused: why, closed: why
array<int> NCS_SLOT;
array<int> NCS_SEED;
array<string> NCS_DROP;
array<int> NCS_TX;             // bytes the session has written to its socket
int NET_SESSION;              // the session the interactive client plays through
int NET_CLOSE_F;               // frames left before a closed client session quits

// ---- the network simulator (off by default): added round trip, jitter and TCP-style loss
int NET_SIM_LAG;               // round-trip ms added (half each way)
int NET_SIM_JITTER;            // +- ms per message
int NET_SIM_LOSS;              // percent of messages that need a retransmission (+250 ms, order kept)
int NET_SIM_SEED;
array<array<int>> NCS_OUT_DUE; // per session: delayed outgoing messages
array<array<string>> NCS_OUT_MSG;
array<array<int>> NCS_IN_DUE;  // per session: delayed incoming messages
array<array<string>> NCS_IN_MSG;
array<int> NCS_OUT_LAST;       // TCP keeps order: a message never overtakes the one before it
array<int> NCS_IN_LAST;

// ---- client-side prediction of the local gator
bool PRED_ON;
int PRED_SEQ;                  // the last step sequence number sent
array<int> PEND_SEQ;           // steps sent but not yet processed by the host
array<int> PEND_DIR;
int PRED_W;                    // the predicted tile (-1 = not started)
int PRED_X;
int PRED_Y;
int PRED_FX;                   // the tile the current step started from
int PRED_FY;
int PRED_T0;                   // when the current step started (ms)
int PRED_DUR;                  // how long it takes (ms)
int PRED_READY;                // when the next step may start (ms)
int PRED_DIR;                  // facing, updated on the key press
int PRED_LUNGE_T;              // a bump attack's lunge (ms)
int PRED_BUMP;                 // the mobile a blocked step would bump (set by pred_blocked)
int PRED_WALL_DIR;             // the direction last sent while walking into a wall, -1 none
float CORR_X;                  // visual correction offset (tiles), decays to zero
float CORR_Y;
int PRED_CORRECTIONS;          // measured: reconciliations that moved the gator
int PRED_SNAPS;                // ... of which snapped (too far to glide)
float PRED_CORR_MAX;           // the largest correction (tiles)
int PRED_STEPS;
// the smoothed server clock (ticks); others are drawn INTERP_DELAY ticks behind it
float CLOCK_TICK;
int CLOCK_MS;
bool CLOCK_SET;
int CLOCK_SEEN;
float INTERP_DELAY = 1.0;
int LAST_MOVE_SENT;

// the starting values (a reload of this file keeps the live ones)
void client_state_init() {
	CAM_SNAP = true;
	PRNG = 12345;
	SHADE_WORLD = -1;
	SHADE_GHOST = -1;
	UI_MODE = "play";
	PANE_WIDTH = 34;
	SCREEN_COLS = 120;
	SCREEN_ROWS = 34;
	MUSIC_ON = true;
	SFX_ON = true;
	MUSIC_CUR = -99;
	NET_PORT = 28777;
	SNAP_DELTAS = true;
	NET_SESSION = -1;
	NET_SIM_SEED = 777;
	PRED_ON = true;
	PRED_W = -1;
	PRED_DUR = 1;
	PRED_DIR = 4;
	PRED_LUNGE_T = -1000;
	PRED_BUMP = -1;
	PRED_WALL_DIR = -1;
	CLOCK_SEEN = -1;
	LAST_MOVE_SENT = -2;
}
