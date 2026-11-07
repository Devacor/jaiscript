// JaiDOOM anim.jai — texture animation: switches press in (SW1* <-> SW2*
// pairs, repeatables pop back out after 35 tics) and flats cycle in lockstep
// every 8 tics (nukage and friends). Wall-texture cycles (SLADRIP-style) are
// not wired: segs cache texture ids, and no ep1 map uses them.

import "wad.jai";
import "map.jai";
import "tex.jai";
import "render.jai";
import "sound.jai";

map<int, int> SWPAIR = {};    // texture id <-> its pressed/released partner
array<int> BTN_LD = [];       // pressed repeatable switches awaiting pop-out
array<int> BTN_TICS = [];
array<int> ANIM_SLOTS = [];   // animated flat slots
array<array<string>> ANIM_FRAMES = []; // per slot: the flat-name cycle
int ANIM_PHASE = 0;
int ANIM_TICK = 0;

function anim_init() -> void {
	SWPAIR = {};
	for (var kv : TEX_NAMES) {
		string nm = kv.first;
		if (!nm.starts_with("SW1")) { continue; }
		int other = tex_id("SW2" + nm.substr(3, nm.size() - 3));
		if (other >= 0) {
			SWPAIR[kv.second] = other;
			SWPAIR[other] = kv.second;
		}
	}
	BTN_LD = [];
	BTN_TICS = [];
	ANIM_SLOTS = [];
	ANIM_FRAMES = [];
	ANIM_PHASE = 0;
	ANIM_TICK = 0;
	var sets = [
		["NUKAGE1", "NUKAGE2", "NUKAGE3"],
		["FWATER1", "FWATER2", "FWATER3", "FWATER4"],
		["SWATER1", "SWATER2", "SWATER3", "SWATER4"],
		["LAVA1", "LAVA2", "LAVA3", "LAVA4"],
		["BLOOD1", "BLOOD2", "BLOOD3"]
	];
	for (var set : sets) {
		array<string> have = [];
		for (var fn : set) {
			if (lump_num(fn) >= 0) { have.push(fn); }
		}
		if (have.size() < 2) { continue; }
		for (var fn2 : have) {
			if (FLAT_SLOT.has(fn2)) {                // only slots this map actually shows
				int slot = FLAT_SLOT[fn2];
				ANIM_SLOTS.push(slot);
				ANIM_FRAMES.push(have);
			}
		}
	}
}

// a successful use-activation presses the line's switch face (if it has one)
function switch_use(int ld) -> void {
	int nsegs = SG_LINE.size();
	bool swapped = false;
	for (int i = 0; i < nsegs; ++i) {
		if (SG_LINE[i] != ld || SG_DIR[i] != 0) { continue; }
		int mt = SEG_MIDT[i];
		if (mt >= 0 && SWPAIR.has(mt)) { SEG_MIDT[i] = SWPAIR[mt]; tex_prepare(SEG_MIDT[i]); swapped = true; }
		int tt = SEG_TOPT[i];
		if (tt >= 0 && SWPAIR.has(tt)) { SEG_TOPT[i] = SWPAIR[tt]; tex_prepare(SEG_TOPT[i]); swapped = true; }
		int bt = SEG_BOTT[i];
		if (bt >= 0 && SWPAIR.has(bt)) { SEG_BOTT[i] = SWPAIR[bt]; tex_prepare(SEG_BOTT[i]); swapped = true; }
	}
	if (!swapped) { return; }
	snd_play("DSSWTCHN");
	if (LD_SPECIAL[ld] != 0) {                       // still armed = repeatable: pop back out
		BTN_LD.push(ld);
		BTN_TICS.push(35);
	}
}

function anim_tic() -> void {
	int i = 0;
	while (i < BTN_LD.size()) {
		BTN_TICS[i] = BTN_TICS[i] - 1;
		if (BTN_TICS[i] <= 0) {
			switch_pop(BTN_LD[i]);
			BTN_LD.remove(i);
			BTN_TICS.remove(i);
		} else { i = i + 1; }
	}
	ANIM_TICK = ANIM_TICK + 1;
	if (ANIM_TICK >= 8) {
		ANIM_TICK = 0;
		ANIM_PHASE = ANIM_PHASE + 1;
		int n = ANIM_SLOTS.size();
		for (int a = 0; a < n; ++a) {
			var frames = ANIM_FRAMES[a];
			var nm = frames[ANIM_PHASE % frames.size()];
			int slot = ANIM_SLOTS[a];
			FLATS[slot] = flat_pix(nm);
		}
	}
}

// pop-out is a plain re-swap (the pair map is symmetric), no sound, no re-arm
function switch_pop(int ld) -> void {
	int nsegs = SG_LINE.size();
	for (int i = 0; i < nsegs; ++i) {
		if (SG_LINE[i] != ld || SG_DIR[i] != 0) { continue; }
		int mt = SEG_MIDT[i];
		if (mt >= 0 && SWPAIR.has(mt)) { SEG_MIDT[i] = SWPAIR[mt]; }
		int tt = SEG_TOPT[i];
		if (tt >= 0 && SWPAIR.has(tt)) { SEG_TOPT[i] = SWPAIR[tt]; }
		int bt = SEG_BOTT[i];
		if (bt >= 0 && SWPAIR.has(bt)) { SEG_BOTT[i] = SWPAIR[bt]; }
	}
}
