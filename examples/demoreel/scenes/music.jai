// MUSIC — the soundtrack is procedural synthwave, and the scenes hear it. Each scene
// has a cue: its bars of music composed at boot from the reel's seed and the scene's
// mood (a key and a progression of lush seventh and added-ninth chords, a drum style
// on a vintage drum machine, instruments, an energy curve), with an impact on each of
// the scene's own big moments: a reverse cymbal swelling over the bar before it, a
// snare roll, a beat of silence and the drop. The kick pumps the chords (a sidechain
// duck), the bass pulses in octaves, the arpeggio and the lead echo on the dotted
// eighth. The score is data: it renders to a Standard MIDI File the runner plays, and
// it answers the scene frame by frame (how hard the kick just landed, which chord is
// sounding, how much energy the section has), so every scene moves with the music it
// is played to, with no audio analysis and to the last byte the same on every run.
// Time is in sixteenth steps from the cue's first downbeat, which is the scene's
// t = 0; the reel's 124 bpm makes a step 0.121 s and a bar 1.935 s.

float MUSIC_BEAT = 0.48387097;
float MUSIC_BAR = 1.93548388;
float MUSIC_STEP = 0.12096774;

// chord qualities: the four tones a chord is voiced and arpeggiated from, semitones over its root
// (maj7, m7, add9, m add9, 7sus4, 7)
var MUSIC_CHORDS = [[0, 4, 7, 11], [0, 3, 7, 10], [0, 4, 7, 14], [0, 3, 7, 14], [0, 5, 7, 10], [0, 4, 7, 10]];
// the progressions: four chords, each [semitones over the key's tonic, quality]
var MUSIC_PROGRESSIONS = [
	[[0, 3], [8, 0], [3, 2], [10, 2]],     // night drive: i(add9) bVImaj7 bIII(add9) bVII(add9)
	[[0, 2], [7, 2], [9, 1], [5, 0]],      // anthem: I(add9) V(add9) vi7 IVmaj7
	[[5, 0], [7, 4], [4, 1], [9, 1]],      // city lights: IVmaj7 V7sus4 iii7 vi7
	[[0, 0], [2, 2], [0, 0], [2, 2]],      // lydian float: Imaj7 II(add9)
	[[0, 3], [10, 2], [8, 0], [10, 4]],    // dark drive: i(add9) bVII(add9) bVImaj7 bVII7sus4
	[[8, 0], [10, 2], [0, 2], [0, 0]],     // triumph: bVImaj7 bVII(add9) I(add9) Imaj7
	[[0, 0], [4, 1], [9, 1], [5, 0]]       // sunset: Imaj7 iii7 vi7 IVmaj7
];

// General MIDI drum kits (the GS set on channel 10): the power kit's gated snare, the
// electronic kit's pads, the 808
int KIT_POWER = 16;
int KIT_ELECTRONIC = 24;
int KIT_808 = 25;

// drum styles: 0 outrun (four on the floor, octave bass), 1 breakbeat, 2 half time, 3 ostinato, 4 driving sixteenths
class Mood {
	int prog = 0;        // into MUSIC_PROGRESSIONS
	int style = 0;
	int root = 45;       // the key's tonic in the bass register
	int kit = 16;
	int bass = 38;       // General MIDI programs
	int pad = 90;
	int arp = 81;
	int lead = 81;
	float energy = 0.6;
	int chord_bars = 2;
	bool cut_crash = true;    // a crash on the cue's first downbeat, where the picture cuts to the scene

	Mood(int prog_, int style_, int root_, int kit_, int bass_, int pad_, int arp_, int lead_, float energy_, int chord_bars_) {
		prog = prog_;
		style = style_;
		root = root_;
		kit = kit_;
		bass = bass_;
		pad = pad_;
		arp = arp_;
		lead = lead_;
		energy = energy_;
		chord_bars = chord_bars_;
	}
}

// one lane of events: steps (sorted), with a velocity and, for pitched lanes, a note and a length
class Lane {
	array<int> at = [];
	array<int> vel = [];
	array<int> note = [];
	array<int> len = [];

	void add(int step, int v, int n, int l) {
		at.push(step);
		vel.push(v);
		note.push(n);
		len.push(l);
	}

	// the last event at or before step s, or -1
	int last(float s) {
		int lo = 0;
		int hi = at.size() - 1;
		int result = -1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (at[mid] <= s) {
				result = mid;
				lo = mid + 1;
			} else {
				hi = mid - 1;
			}
		}
		return result;
	}

	// 1 the moment an event lands (scaled by its velocity), decaying at `rate` per second
	float env(float t, float rate) {
		float s = t / MUSIC_STEP;
		int i = last(s + 0.0001);
		if (i < 0) { return 0.0; }
		float since = t - at[i] * MUSIC_STEP;
		return vel[i] / 127.0 * math::pow(2.718281828, 0.0 - since * rate);
	}
}

class Cue {
	int bars = 8;
	var mood = null;
	int seed = 1;
	var kick = Lane();
	var snare = Lane();
	var hat = Lane();
	var crash = Lane();
	var bass_lane = Lane();
	var pad_lane = Lane();
	var arp_lane = Lane();
	var lead_lane = Lane();
	var echo = Lane();     // the lead's and the arpeggio's dotted-eighth repeats
	var stab = Lane();     // brass on the accents
	var orch = Lane();     // the orchestra hit on the big hits
	var swell = Lane();    // a reverse cymbal into each big hit
	array<int> duck = [];  // the chords' expression per step: the kick's sidechain pump
	array<float> energy_bar = [];
	array<int> chord_root = [];    // per bar: the chord's root, a MIDI note in the bass register
	array<int> chord_q = [];       // per bar: the chord's quality, into MUSIC_CHORDS
	array<float> hit_t = [];
	array<int> hit_level = [];
	array<int> big_steps = [];

	Cue(int bars_, var mood_, int seed_, array<float> hits, array<int> levels) {
		bars = bars_;
		mood = mood_;
		seed = seed_;
		hit_t = hits;
		hit_level = levels;
		compose();
	}

	// ------------------------------------------------------------ queries ---

	float beat_phase(float t) {
		float b = t / MUSIC_BEAT;
		return b - math::floor(b);
	}

	float bar_phase(float t) {
		float b = t / MUSIC_BAR;
		return b - math::floor(b);
	}

	int bar_index(float t) { return math::ifloor(t / MUSIC_BAR); }

	// 1 on every beat, falling away before the next
	float pulse(float t) {
		float f = beat_phase(t);
		return (1.0 - f) * (1.0 - f);
	}

	float kick_env(float t) { return kick.env(t, 9.0); }
	float snare_env(float t) { return snare.env(t, 7.0); }
	float hat_env(float t) { return hat.env(t, 22.0); }
	float bass_env(float t) { return bass_lane.env(t, 5.0); }
	float lead_env(float t) { return lead_lane.env(t, 3.0); }
	float arp_env(float t) { return arp_lane.env(t, 14.0); }

	int bass_note(float t) {
		int i = bass_lane.last(t / MUSIC_STEP + 0.0001);
		return i < 0 ? mood.root : bass_lane.note[i];
	}

	int lead_note(float t) {
		int i = lead_lane.last(t / MUSIC_STEP + 0.0001);
		return i < 0 ? 0 : lead_lane.note[i];
	}

	// the sounding chord's root as a pitch class, 0..11
	int chord(float t) {
		int b = bar_index(t);
		b = b < 0 ? 0 : (b >= bars ? bars - 1 : b);
		return chord_root[b] % 12;
	}

	// the section's intensity, 0..1, eased between bars
	float energy(float t) {
		float b = t / MUSIC_BAR;
		int i = math::ifloor(b);
		if (i < 0) { return energy_bar[0]; }
		if (i >= bars - 1) { return energy_bar[bars - 1]; }
		float f = b - i;
		return energy_bar[i] + (energy_bar[i + 1] - energy_bar[i]) * f * f * (3.0 - 2.0 * f);
	}

	// 1 at each of the scene's hits (a big one at full, an accent at half), decaying over about a second
	float hit(float t) {
		float result = 0.0;
		for (int k = 0; k < hit_t.size(); ++k) {
			float since = t - hit_t[k];
			if (since < 0.0 || since > 3.0) { continue; }
			float e = (hit_level[k] >= 2 ? 1.0 : 0.5) * math::pow(2.718281828, 0.0 - since * 2.5);
			result = e > result ? e : result;
		}
		return result;
	}

	// 0..1 over the bar leading into each big hit
	float riser(float t) {
		float result = 0.0;
		for (int k = 0; k < hit_t.size(); ++k) {
			if (hit_level[k] < 2) { continue; }
			float to = hit_t[k] - t;
			if (to < 0.0 || to > MUSIC_BAR) { continue; }
			float r = 1.0 - to / MUSIC_BAR;
			result = r > result ? r : result;
		}
		return result;
	}

	// ----------------------------------------------------------- composing ---

	int lcg_state = 1;

	int roll(int n) {
		lcg_state = (lcg_state * 1103515245 + 12345) & 2147483647;
		return (lcg_state >> 16) % n;
	}

	// tone i (any integer, octaves above and below) of bar b's chord, semitones over its root
	int tone(int b, int i) {
		var q = MUSIC_CHORDS[chord_q[b]];
		int oct = math::ifloor(i / 4.0);
		return q[i - oct * 4] + oct * 12;
	}

	// sixteenths until the next big hit at or after step s, or a long way off
	int to_big(int s) {
		int result = 9999;
		for (int k = 0; k < big_steps.size(); ++k) {
			int d = big_steps[k] - s;
			if (d > 0 && d < result) { result = d; }
		}
		return result;
	}

	bool is_big(int s) {
		for (int k = 0; k < big_steps.size(); ++k) {
			if (big_steps[k] == s) { return true; }
		}
		return false;
	}

	void compose() {
		lcg_state = (seed & 65535) | 1;
		var prog = MUSIC_PROGRESSIONS[mood.prog];
		int steps = bars * 16;
		for (int k0 = 0; k0 < hit_t.size(); ++k0) {
			if (hit_level[k0] >= 2) { big_steps.push(itrunc(hit_t[k0] / MUSIC_STEP + 0.5)); }
		}
		// the energy curve: the title bar is quiet, then the mood's level, a build into each big
		// hit and a drop on it, easing back down after
		for (int b = 0; b < bars; ++b) {
			float e = b == 0 ? mood.energy * 0.55 : mood.energy + (1.0 - mood.energy) * (b / (bars * 1.0)) * 0.35;
			energy_bar.push(e);
		}
		for (int k = 0; k < hit_t.size(); ++k) {
			int hb = math::ifloor(hit_t[k] / MUSIC_BAR + 0.0001);
			if (hb < 0 || hb >= bars) { continue; }
			if (hit_level[k] >= 2) {
				energy_bar[hb] = 1.0;
				if (hb > 0) { energy_bar[hb - 1] = energy_bar[hb - 1] * 0.7 + 0.25; }
				if (hb + 1 < bars) { energy_bar[hb + 1] = energy_bar[hb + 1] > 0.9 ? energy_bar[hb + 1] : 0.9; }
			} else {
				energy_bar[hb] = energy_bar[hb] + 0.12 > 1.0 ? 1.0 : energy_bar[hb] + 0.12;
			}
		}
		for (int b2 = 0; b2 < bars; ++b2) {
			var ch = prog[(b2 / mood.chord_bars) % 4];
			int off = ch[0] > 6 ? ch[0] - 12 : ch[0];
			chord_root.push(mood.root + off);
			chord_q.push(ch[1]);
		}
		// the lead's motif: two bars of rhythm and chord tones from the seed, sung over each
		// chord and varied on its repeats, so the tune has a shape to remember
		array<int> motif_at = [];
		array<int> motif_deg = [];
		array<int> motif_len = [];
		int walk = 2;
		for (int ms = 0; ms < 32; ms += 2) {
			int r0 = roll(16);
			if (ms == 0 || ms == 16 || r0 < 8) {
				walk = walk + (r0 % 5) - 2;
				walk = walk < 0 ? 1 : (walk > 7 ? 6 : walk);
				motif_at.push(ms);
				motif_deg.push(walk);
				motif_len.push(r0 < 4 ? 4 : (r0 < 6 ? 3 : 2));
			}
		}
		for (int s = 0; s < steps; ++s) {
			int b3 = s / 16;
			int st = s % 16;
			float e = energy_bar[b3];
			int croot = chord_root[b3];
			int tb = to_big(s);
			bool gap = tb <= 4;    // the beat of silence before the drop
			drums(s, st, e, tb);
			if (e > 0.3 && !gap && !is_big(s)) { bass(s, st, e, croot); }
			// the pad holds the chord from each chord's first bar
			if (st == 0 && b3 % mood.chord_bars == 0) {
				for (int v = 0; v < 4; ++v) { pad_lane.add(s, (v == 0 ? 64 : 56) + itrunc(e * 30.0), croot + 24 + tone(b3, v), 16 * mood.chord_bars); }
			}
			// the arpeggio runs the chord up and down in sixteenths once the energy is up
			if (e > 0.5 && !gap) {
				var pat = mood.style == 4 ? [0, 2, 4, 2, 1, 3, 5, 3] : [0, 1, 2, 3, 4, 3, 2, 1];
				int n = croot + 24 + tone(b3, pat[st % 8]);
				int v2 = 60 + (st % 4 == 0 ? 24 : 0) + itrunc(e * 14.0);
				arp_lane.add(s, v2, n, 1);
				if (st % 4 == 0) { echo.add(s + 3, v2 / 2, n, 1); }
			}
			// the lead sings the motif at the peaks, a tone higher on every other pass, each note
			// repeating on the dotted eighth
			if (e > 0.72 && !gap) {
				int ms2 = s % 32;
				for (int mi = 0; mi < motif_at.size(); ++mi) {
					if (motif_at[mi] != ms2) { continue; }
					int lift = (s / 64) % 2 == 1 && mi >= motif_at.size() / 2 ? 1 : 0;
					int n2 = croot + 24 + tone(b3, motif_deg[mi] + lift);
					lead_lane.add(s, 104, n2, motif_len[mi]);
					echo.add(s + 3, 56, n2, motif_len[mi]);
					echo.add(s + 6, 32, n2, motif_len[mi]);
				}
			}
		}
		// the hits: a crash, a low boom, an orchestra hit and brass on the chord, the bass dropping
		// an octave under it, a reverse cymbal swelling over the bar before
		for (int k2 = 0; k2 < hit_t.size(); ++k2) {
			int hs = itrunc(hit_t[k2] / MUSIC_STEP + 0.5);
			if (hs < 0 || hs >= steps) { continue; }
			int hb3 = hs / 16;
			int hroot = chord_root[hb3];
			if (hit_level[k2] >= 2) {
				crash.add(hs, 127, 49, 1);
				crash.add(hs, 110, 57, 1);
				kick.add(hs, 127, 35, 1);
				bass_lane.add(hs, 127, bass_root(hroot), 8);
				for (int v3 = 0; v3 < 3; ++v3) { orch.add(hs, 120, hroot + 24 + tone(hb3, v3), 4); }
				for (int v4 = 0; v4 < 3; ++v4) { stab.add(hs, 112, hroot + 24 + tone(hb3, v4), 8); }
				if (hs >= 16) { swell.add(hs - 16, 112, 60, 16); }
			} else {
				crash.add(hs, 96, 57, 1);
				for (int v5 = 0; v5 < 3; ++v5) { stab.add(hs, 98, hroot + 24 + tone(hb3, v5), 2); }
			}
		}
		sort_lane(kick);
		sort_lane(crash);
		sort_lane(bass_lane);
		sort_lane(stab);
		sort_lane(echo);
		// the sidechain: every kick ducks the chords and they swell back over the beat
		for (int s2 = 0; s2 < steps; ++s2) { duck.push(127); }
		float depth = 52.0 + mood.energy * 24.0;
		var shape = [1.0, 0.6, 0.28, 0.08];
		for (int ki = 0; ki < kick.at.size(); ++ki) {
			for (int j = 0; j < 4; ++j) {
				int at = kick.at[ki] + j;
				if (at >= steps) { break; }
				int v6 = 127 - itrunc(depth * shape[j] * kick.vel[ki] / 127.0);
				if (v6 < duck[at]) { duck[at] = v6; }
			}
		}
	}

	// the chord's root an octave down, kept at E1 or above where a synth bass still speaks
	int bass_root(int croot) { return croot - 12 < 28 ? croot : croot - 12; }

	void bass(int s, int st, float e, int croot) {
		int lo = bass_root(croot);
		int style = mood.style;
		if (style == 0) {
			// outrun: the root pumping in eighths, every other one an octave up, sixteenth ghosts at the peaks
			if (st % 2 == 0) { bass_lane.add(s, st % 4 == 0 ? 114 : 96, lo + (st % 4 == 2 ? 12 : 0), 1); }
			else if (e > 0.85) { bass_lane.add(s, 66, lo, 1); }
		} else if (style == 4) {
			// driving: a rolling sixteenth on the root, the octave on the last of each beat
			bass_lane.add(s, st % 4 == 0 ? 116 : (st % 2 == 0 ? 92 : 74), lo + (st % 4 == 3 && e > 0.7 ? 12 : 0), 1);
		} else if (style == 1) {
			if (st == 0 || st == 3 || st == 6 || st == 10 || st == 12 || st == 14) { bass_lane.add(s, st == 0 ? 116 : 94, lo + (st == 6 || st == 14 ? 12 : 0), st == 0 ? 3 : 1); }
		} else if (style == 2) {
			if (st == 0 || st == 6 || st == 8 || st == 14) { bass_lane.add(s, st == 0 || st == 8 ? 112 : 88, lo + (st == 14 ? 12 : 0), st == 0 || st == 8 ? 5 : 2); }
		} else {
			// ostinato: eighths on the root, the octave on the last of each half bar
			if (st % 2 == 0) { bass_lane.add(s, 70 + itrunc(e * 36.0) + (st % 8 == 0 ? 10 : 0), lo + (st % 8 == 6 ? 12 : 0), 1); }
		}
	}

	void drums(int s, int st, float e, int tb) {
		int style = mood.style;
		bool four = style == 0 || style == 4;
		bool gap = tb <= 4;
		if ((e > 0.4 || st == 0) && !gap) {
			bool k = false;
			if (four) { k = st % 4 == 0 || (style == 4 && st == 14 && e > 0.8); }
			else if (style == 1) { k = st == 0 || st == 6 || st == 10; }
			else if (style == 2) { k = st == 0 || st == 10; }
			else { k = (st == 0 && s % 32 == 0) || (st == 8 && e > 0.6); }
			if (k) { kick.add(s, 118, 36, 1); }
		}
		// the backbeat: a gated snare and a clap together
		if (e > 0.45 && !gap) {
			bool sn = false;
			if (four || style == 1) { sn = st == 4 || st == 12; }
			else if (style == 2) { sn = st == 8; }
			else { sn = st == 8 && (s / 16) % 2 == 1; }
			if (sn) {
				snare.add(s, 112, 38, 1);
				if (e > 0.55) { snare.add(s, 96, 39, 1); }
			}
			if (style == 1 && (st == 7 || st == 15) && e > 0.6) { snare.add(s, 52, 38, 1); }
		}
		// into a big hit: a snare roll climbing through the last half bar, through the silence
		if (tb <= 8) { snare.add(s, 64 + (8 - tb) * 8, 38, 1); }
		// a fill into every fourth bar: the toms falling through the last beat
		bool fill = tb > 16 && e > 0.5 && (s / 16) % 4 == 3 && st >= 12;
		if (fill) {
			var toms = [50, 48, 47, 45];
			snare.add(s, 96 + (st - 12) * 7, toms[st - 12], 1);
		}
		if (e > 0.3 && !gap) {
			bool sixteen = style == 4 || e > 0.85;
			bool open = e > 0.6 && st % 4 == 2 && style != 3;
			if (open) { hat.add(s, 70, 46, 1); }
			else if (sixteen || st % 2 == 0) { hat.add(s, st % 4 == 0 ? 74 : 50, 42, 1); }
		}
		// a crash on the cut, on the scene's first full bar and on every fourth bar at the peaks
		if (s == 0 && mood.cut_crash) { crash.add(s, 108, 49, 1); }
		if (st == 0 && s > 0 && (s == 16 || (s % 64 == 0 && e > 0.7)) && !gap) { crash.add(s, s == 16 ? 116 : 100, 49, 1); }
	}

	// insertion order is time order for every lane but the ones the hits and echoes add to afterwards
	void sort_lane(var& lane) {
		int n = lane.at.size();
		for (int i = 1; i < n; ++i) {
			int j = i;
			while (j > 0 && lane.at[j - 1] > lane.at[j]) {
				int ta = lane.at[j]; lane.at[j] = lane.at[j - 1]; lane.at[j - 1] = ta;
				int tv = lane.vel[j]; lane.vel[j] = lane.vel[j - 1]; lane.vel[j - 1] = tv;
				int tn = lane.note[j]; lane.note[j] = lane.note[j - 1]; lane.note[j - 1] = tn;
				int tl = lane.len[j]; lane.len[j] = lane.len[j - 1]; lane.len[j - 1] = tl;
				--j;
			}
		}
	}

	// ---------------------------------------------------------------- MIDI ---

	// the score as a Standard MIDI File: one track, 96 ticks a quarter, events in time order
	string midi() {
		var m = MidiTrack();
		int q = 96;
		int s16 = q / 4;
		m.ev(0, "" + to_char(255) + to_char(81) + to_char(3) + to_char(7) + to_char(98) + to_char(31));
		// channels: 0 bass, 1 pad, 2 arpeggio, 3 lead and the echoes, 4 brass, 5 reverse cymbal, 6 orchestra hit, 9 drums
		var chans = [0, 1, 2, 3, 4, 5, 6, 9];
		var programs = [mood.bass, mood.pad, mood.arp, mood.lead, 62, 119, 55, mood.kit];
		var volumes = [112, 84, 70, 96, 90, 100, 100, 116];
		var reverb = [16, 84, 72, 76, 64, 90, 80, 64];
		var chorus = [24, 72, 40, 56, 30, 0, 20, 0];
		for (int c = 0; c < chans.size(); ++c) {
			int ch = chans[c];
			m.ev(0, "" + to_char(192 | ch) + to_char(programs[c]));
			m.ev(0, b3(176 | ch, 7, volumes[c]));
			m.ev(0, b3(176 | ch, 91, reverb[c]));
			m.ev(0, b3(176 | ch, 93, chorus[c]));
		}
		var lanes = [kick, snare, hat, crash, bass_lane, pad_lane, arp_lane, lead_lane, echo, stab, swell, orch];
		var channels = [9, 9, 9, 9, 0, 1, 2, 3, 3, 4, 5, 6];
		array<int> cursor = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
		int steps = bars * 16;
		int last_duck = 127;
		for (int s = 0; s < steps; ++s) {
			int tick = s * s16;
			m.flush(tick);
			if (duck[s] != last_duck) {
				m.ev(tick, b3(176 | 1, 11, duck[s]));
				m.ev(tick, b3(176 | 2, 11, duck[s]));
				last_duck = duck[s];
			}
			for (int li = 0; li < lanes.size(); ++li) {
				var& lane = lanes[li];
				while (cursor[li] < lane.at.size() && lane.at[cursor[li]] == s) {
					int c2 = cursor[li];
					m.note(tick, channels[li], lane.note[c2], lane.vel[c2], lane.len[c2] * s16 - 2);
					cursor[li] = c2 + 1;
				}
			}
		}
		m.flush(steps * s16);
		m.ev(steps * s16, "" + to_char(255) + to_char(47) + to_char(0));
		string head = "MThd" + be32(6) + "" + to_char(0) + to_char(0) + to_char(0) + to_char(1) + to_char(0) + to_char(q);
		return head + "MTrk" + be32(m.trk.size()) + m.trk;
	}
}

// ---------------------------------------------------------- MIDI writing ---

function vlq(int v) -> string {
	string out = "" + to_char(v & 127);
	int r = v >> 7;
	while (r > 0) {
		out = "" + to_char(128 | (r & 127)) + out;
		r = r >> 7;
	}
	return out;
}

function b3(int a, int b, int c) -> string { return "" + to_char(a) + to_char(b) + to_char(c); }
function be32(int v) -> string {
	return "" + to_char((v >> 24) & 255) + to_char((v >> 16) & 255) + to_char((v >> 8) & 255) + to_char(v & 255);
}

class MidiTrack {
	string trk = "";
	int last = 0;
	array<int> off_t = [];
	array<int> off_c = [];
	array<int> off_n = [];

	void ev(int tick, string bytes) {
		trk += vlq(tick - last) + bytes;
		last = tick;
	}

	void note(int tick, int ch, int n, int vel, int dur) {
		ev(tick, b3(144 | ch, n, vel));
		off_t.push(tick + (dur < 1 ? 1 : dur));
		off_c.push(ch);
		off_n.push(n);
	}

	// note-offs due by `tick`, emitted in time order (a delta is never negative)
	void flush(int tick) {
		while (true) {
			int pick = -1;
			for (int i = 0; i < off_t.size(); ++i) {
				if (off_t[i] <= tick && (pick < 0 || off_t[i] < off_t[pick])) { pick = i; }
			}
			if (pick < 0) { return; }
			ev(off_t[pick], b3(128 | off_c[pick], off_n[pick], 0));
			int last_i = off_t.size() - 1;
			off_t[pick] = off_t[last_i];
			off_c[pick] = off_c[last_i];
			off_n[pick] = off_n[last_i];
			off_t.pop();
			off_c.pop();
			off_n.pop();
		}
	}
}

// each scene's mood, by its place in the reel: the keys travel, so no two neighbours share one
// [progression, drum style, tonic, kit, bass, pad, arpeggio, lead, base energy, bars a chord]
function music_mood(int idx) -> var {
	switch (idx) {
		case 0: return Mood(1, 0, 38, KIT_POWER, 39, 90, 80, 62, 0.74, 2);         // DONUT: D anthem, outrun, synth brass lead
		case 1: return Mood(0, 4, 40, KIT_ELECTRONIC, 38, 95, 81, 81, 0.85, 1);    // JULIA: E night drive, driving sixteenths
		case 2: return Mood(3, 2, 41, KIT_808, 38, 89, 98, 82, 0.64, 2);           // LAVALAMP: F lydian float, half time, crystal
		case 3: return Mood(2, 0, 43, KIT_POWER, 39, 90, 81, 81, 0.76, 2);         // PIPEDREAM: G city lights, outrun saw
		case 4: return Mood(6, 2, 38, KIT_808, 38, 89, 98, 73, 0.56, 2);           // BOIDS: D sunset, half time, flute
		case 5: return Mood(4, 4, 36, KIT_ELECTRONIC, 39, 95, 87, 30, 0.82, 1);    // GLOBHUNTER: C dark drive, distortion lead
		case 6: return Mood(1, 0, 46, KIT_POWER, 39, 50, 81, 62, 0.72, 2);         // CANYONRUN: B flat anthem, synth strings
		case 7: return Mood(6, 0, 40, KIT_ELECTRONIC, 38, 89, 11, 81, 0.72, 2);    // CHROME: E sunset, vibes arpeggio
		case 8: return Mood(0, 3, 42, KIT_808, 38, 91, 14, 81, 0.5, 2);            // OSSUARY: F sharp night drive, an ostinato under choir and bells
		case 9: return Mood(2, 1, 44, KIT_808, 38, 94, 81, 80, 0.72, 2);           // MAELSTROM: A flat city lights, breakbeat
		case 10: return Mood(5, 0, 45, KIT_POWER, 39, 90, 81, 81, 0.74, 2);        // FINALE: A major triumph, the reel's anthem
	}
	// the cold open: F dark drive, driving sixteenths, starting from silence
	var result = Mood(4, 4, 41, KIT_ELECTRONIC, 39, 95, 81, 81, 0.82, 1);
	result.cut_crash = false;
	return result;
}
