// Gator Quest - the prediction test: the real host and a real client session over
// loopback, on a virtual clock (a minute of play in well under a minute), with the
// network simulator adding latency, jitter and loss. The client walks a route by
// "holding keys" through the same pred_input the keyboard uses. Every byte either
// side writes reaches the other before it next reads (the loopback takes no virtual
// time), so a run's outcome depends on the seeds alone, never on the machine's load.
//
//   jaiscript gatorquest.jai -- predtest            the sweep: 0, 60, 150, 300 ms round trip
//   jaiscript gatorquest.jai -- predtest lag 120 jitter 20 loss 2

import "gq_endgame.jai";

int PT_S;
int PT_NEXT_TICK;
int PT_SAME_FRAME_CHECKS;
int PT_SAME_FRAME_MISSES;
int PT_HOST_RX;                 // bytes the host has read from the test client
int PT_CLIENT_RX;               // bytes the test client has read from the host
int PT_HOST_TX0;                // NET_BYTES_OUT when this round's host started
bool PT_STALLED;                // a delivery did not complete within five seconds

int pt_mob() { return CHARS[NCS_SLOT[PT_S]].mob; }

bool pt_delivered() { return PT_HOST_RX >= NCS_TX[PT_S] && PT_CLIENT_RX >= NET_BYTES_OUT - PT_HOST_TX0; }

void pt_deliver() {
	for (int i = 0; i < 5000 && !pt_delivered(); ++i) {
		NET_SRV.pump();
		NCS_CH[PT_S].pump();
		if (!pt_delivered()) { sleep(0.001); }
	}
	if (!pt_delivered()) { PT_STALLED = true; }
}

// one client frame (33 ms); the host ticks at 10 Hz in between
void pt_frame(int held) {
	pt_frame_input(held);
	pt_frame_world();
}

void pt_frame_input(int held) {
	NOW_MS = NOW_MS + 33;
	// the promise under test: a free step is taken on the frame the key is held
	bool expect = held >= 0 && pred_active(CV) && NOW_MS >= PRED_READY && !CV.sliding && pred_blocked(CV, PRED_X, PRED_Y, held) == 0;
	int bx = PRED_X;
	int by = PRED_Y;
	pred_input(CV, held, NOW_MS);
	if (expect) {
		PT_SAME_FRAME_CHECKS = PT_SAME_FRAME_CHECKS + 1;
		if (PRED_X == bx && PRED_Y == by) { PT_SAME_FRAME_MISSES = PT_SAME_FRAME_MISSES + 1; }
	}
}

void pt_frame_world() {
	net_session_pump(PT_S);
	while (NOW_MS >= PT_NEXT_TICK) {
		pt_deliver();
		sim_tick();
		net_host_pump();
		host_after_tick();
		PT_NEXT_TICK = PT_NEXT_TICK + TICK_MS;
		pt_deliver();
	}
	net_session_pump(PT_S);
	pred_reconcile(CV, NOW_MS);
	pred_frame(CV, NOW_MS);
}

// the most open ground in Gatorland outside the towns: the route needs room to walk
array<int> pt_open_spot() {
	int best = -1;
	int bx = W_ARRIVE_X[0];
	int by = W_ARRIVE_Y[0];
	for (int y = 8; y < W_H[0] - 8; y += 3) {
		for (int x = 8; x < W_W[0] - 8; x += 3) {
			if (town_at(0, x, y) >= 0) { continue; }
			int open = 0;
			for (int dy = -5; dy <= 5; ++dy) {
				for (int dx = -6; dx <= 6; ++dx) {
					if (walk_at(0, x + dx, y + dy) == 1) { open = open + 1; }
				}
			}
			if (open > best) { best = open; bx = x; by = y; }
		}
	}
	return [bx - 4, by];
}

bool pt_converged() {
	int m = pt_mob();
	return PEND_SEQ.size() == 0 && PRED_X == MOB_X[m] && PRED_Y == MOB_Y[m] && PRED_W == MOB_WORLD[m];
}

// start a host and join it with one client under the given simulated link; the gator starts fresh
// (a character file saved by an earlier round or run would carry its place in)
bool pt_setup(int port, int lag, int jitter, int loss) {
	make_dirs(save_dir() + "/chars");
	array<string> old = list_dir(save_dir() + "/chars");
	for (int i = 0; i < old.size(); ++i) { delete_file(save_dir() + "/chars/" + old[i]); }
	sim_init(9);
	BOTS = [];
	NOW_MS = 0;
	PT_NEXT_TICK = 0;
	NET_SIM_LAG = 0;
	NET_SIM_JITTER = 0;
	NET_SIM_LOSS = 0;
	NET_SIM_SEED = 777 + lag;
	if (!net_host_start(port)) { return false; }
	PT_HOST_TX0 = NET_BYTES_OUT;
	PT_HOST_RX = 0;
	PT_CLIENT_RX = 0;
	PT_STALLED = false;
	NET_SRV.on_client_packet.connect("pt", [](int id, string bytes) { PT_HOST_RX = PT_HOST_RX + bytes.size(); });
	PT_S = net_client_open("127.0.0.1", port);
	NCS_CH[PT_S].on_packet.connect("pt", [](string bytes) { PT_CLIENT_RX = PT_CLIENT_RX + bytes.size(); });
	for (int i = 0; i < 200 && !NCS_CH[PT_S].connected(); ++i) { NET_SRV.pump(); NCS_CH[PT_S].pump(); sleep(0.001); }
	NET_SIM_LAG = lag;
	NET_SIM_JITTER = jitter;
	NET_SIM_LOSS = loss;
	net_client_hello(PT_S, "Predictor", 2);
	CV = NCS_VIEW[PT_S];
	NET_SESSION = PT_S;
	NET_CLIENT = true;
	pred_reset();
	PRED_ON = true;
	for (int i = 0; i < 300 && (NCS_STATUS[PT_S] != "joined" || !pred_active(CV)); ++i) { pt_frame(-1); }
	if (NCS_STATUS[PT_S] != "joined") { return false; }
	LOCAL_SLOT = NCS_SLOT[PT_S];
	// out of town, in the open, and nothing will fight back (the test is about movement)
	int m = pt_mob();
	MOB_FLAGS[m] = MOB_FLAGS[m] | MF_INVULN;
	array<int> open = pt_open_spot();
	array<int> spot = find_free_spot(0, open[0], open[1], 10, 2);
	travel(m, 0, spot[0], spot[1]);
	for (int i = 0; i < 60; ++i) { pt_frame(-1); }
	pred_reset();
	for (int i = 0; i < 30; ++i) { pt_frame(-1); }
	PT_SAME_FRAME_CHECKS = 0;
	PT_SAME_FRAME_MISSES = 0;
	return pred_active(CV) && pt_converged();
}

void pt_teardown() {
	net_session_bye(PT_S);
	net_host_stop("test over");
	NET_CLIENT = false;
	NET_SESSION = -1;
	NET_SIM_LAG = 0;
	NET_SIM_JITTER = 0;
	NET_SIM_LOSS = 0;
}

// a minute of walking: a loop of east, south, west, north with pauses, converging at each stop
array<int> pt_walk(int seconds) {
	array<int> route = [2, 70, -1, 15, 4, 50, -1, 15, 6, 70, -1, 15, 0, 50, -1, 45, 3, 40, 7, 40, -1, 45];
	int frames_walked = 0;
	int stops = 0;
	int converge_fails = 0;
	int budget = seconds * 30;
	while (frames_walked < budget) {
		for (int r = 0; r < route.size(); r += 2) {
			int dir = route[r];
			for (int f = 0; f < route[r + 1]; ++f) {
				pt_frame(dir);
				if (dir >= 0) { frames_walked = frames_walked + 1; }
			}
			if (dir < 0 && route[r + 1] >= 45) {
				stops = stops + 1;
				if (!pt_converged()) { converge_fails = converge_fails + 1; }
			}
		}
	}
	return [frames_walked, stops, converge_fails];
}

// the host sees a monster the client does not know about yet, right where the client steps
array<int> pt_rejected_move() {
	for (int i = 0; i < 40; ++i) { pt_frame(-1); }
	int m = pt_mob();
	int w = MOB_WORLD[m];
	int dir = -1;
	for (int d = 0; d < 8 && dir < 0; d += 2) {
		int x = MOB_X[m] + DX[d];
		int y = MOB_Y[m] + DY[d];
		if (is_land(w, x, y) && mob_at(w, x, y) < 0 && pred_blocked(CV, PRED_X, PRED_Y, d) == 0) { dir = d; }
	}
	if (dir < 0) { return [-1, 0, 0, 0, 0, 0, -1]; }
	int c0 = PRED_CORRECTIONS;
	int s0 = PRED_SNAPS;
	float max0 = PRED_CORR_MAX;
	PRED_CORR_MAX = 0.0;
	int rat = mob_spawn(mid("Swamp Rat"), w, MOB_X[m] + DX[dir], MOB_Y[m] + DY[dir]);
	MOB_NEXT[rat] = TICK + 600;
	int start_x = MOB_X[m];
	int start_y = MOB_Y[m];
	int t_press = NOW_MS + 33;
	pt_frame_input(dir);
	bool stepped = PRED_X == start_x + DX[dir] && PRED_Y == start_y + DY[dir];
	pt_frame_world();
	// the drawn gator must never jump: the largest move between two frames
	float jump = 0.0;
	float lx = pred_draw_x(NOW_MS);
	float ly = pred_draw_y(NOW_MS);
	int corrected_at = -1;
	for (int f = 0; f < 30; ++f) {
		int before = PRED_CORRECTIONS;
		pt_frame(-1);
		if (PRED_CORRECTIONS > before && corrected_at < 0) { corrected_at = NOW_MS - t_press; }
		float nx = pred_draw_x(NOW_MS);
		float ny = pred_draw_y(NOW_MS);
		float d = (nx - lx) * (nx - lx) + (ny - ly) * (ny - ly);
		if (d > jump) { jump = d; }
		lx = nx;
		ly = ny;
	}
	int corrections = PRED_CORRECTIONS - c0;
	int snaps = PRED_SNAPS - s0;
	int dist = math::itrunc(PRED_CORR_MAX + 0.5);
	if (max0 > PRED_CORR_MAX) { PRED_CORR_MAX = max0; }
	bool back = pt_converged() && PRED_X == start_x && PRED_Y == start_y;
	mob_free(rat);
	return [stepped ? 1 : 0, corrections, snaps, dist, math::itrunc(math::sqrt(jump) * 100.0), back ? 1 : 0, corrected_at];
}

int run_predtest(string base_dir) {
	SAVE_DIR = base_dir + "/predtest";
	array<int> rtts = [0, 60, 150, 300];
	if (arg_value("lag", "") != "") { rtts = [NET_SIM_LAG]; }
	int jitter_arg = NET_SIM_JITTER;
	int loss_arg = NET_SIM_LOSS;
	bool custom = arg_value("lag", "") != "";
	int seconds = arg_value("seconds", "60").to_int(60);
	int fails = 0;
	print("prediction: one client walking under a simulated link (virtual clock, the real host and sockets)");
	print("  rtt  jitter loss | walked  steps | corrections /min  max  snaps | same-frame misses | stops converged");
	for (int k = 0; k < rtts.size(); ++k) {
		int rtt = rtts[k];
		int jitter = custom ? jitter_arg : rtt / 6;
		int loss = custom ? loss_arg : (rtt >= 150 ? 1 : 0);
		if (!pt_setup(28930 + k, rtt, jitter, loss)) {
			print(`  ${rtt:>4}: setup failed (${NCS_STATUS[PT_S]})`);
			fails = fails + 1;
			pt_teardown();
			continue;
		}
		array<int> w = pt_walk(seconds);
		float minutes = w[0] / 1800.0;
		float per_min = PRED_CORRECTIONS / minutes;
		print(`  ${rtt:>4} ${jitter:>6} ${loss:>3}% | ${w[0] / 30:>5}s ${PRED_STEPS:>6} | ${PRED_CORRECTIONS:>11} ${per_min:>5.1f} ${PRED_CORR_MAX:>4.0f} ${PRED_SNAPS:>6} | ${PT_SAME_FRAME_MISSES:>5} of ${PT_SAME_FRAME_CHECKS:>5} | ${w[1] - w[2]:>2} of ${w[1]}`);
		if (PT_SAME_FRAME_MISSES != 0) { fails = fails + 1; print("  FAIL  a free step was not taken on the frame the key was held"); }
		if (w[2] != 0) { fails = fails + 1; print("  FAIL  the prediction did not converge to the host at a stop"); }
		if (rtt == 0 && (per_min > 3.0 || PRED_CORR_MAX > 1.0)) { fails = fails + 1; print("  FAIL  corrections on a clean link should be rare and at most one tile"); }
		if (PRED_SNAPS != 0 || PRED_CORR_MAX > 2.0) { fails = fails + 1; print("  FAIL  corrections should glide (no snaps, at most two tiles)"); }
		if (rtt == 150 || (custom && k == 0)) {
			array<int> r = pt_rejected_move();
			print(`  rejected move at ${rtt} ms: step taken on the key frame ${r[0] == 1 ? "yes" : "NO"}, ${r[1]} correction of ${r[3]} tile ${r[6]} ms after the press, ${r[2]} snaps, largest frame-to-frame move of the drawn gator ${r[4] / 100.0:.2f} tiles, back in agreement ${r[5] == 1 ? "yes" : "no"}`);
			if (r[0] != 1 || r[1] != 1 || r[2] != 0 || r[3] != 1 || r[4] > 40 || r[5] != 1) {
				fails = fails + 1;
				print("  FAIL  the rejected move should correct once, by one tile, gliding");
			}
		}
		if (PT_STALLED) { fails = fails + 1; print("  FAIL  the loopback stalled: a delivery did not complete within five seconds"); }
		pt_teardown();
	}
	print(fails == 0 ? "predtest: all checks passed" : `predtest: ${fails} problems`);
	return fails;
}
