// Gator Quest - sound and music with zero audio assets: every effect is
// synthesized into an 8-bit WAV and every realm's theme is composed into a
// MIDI file on first use, cached under the data folder. Playback is the
// runner's Sound: each effect loads once and replays, the theme loops on
// its own (no sound where the runner has none).

import "gq_render.jai";

bool AUDIO_ON;
string AUDIO_DIR;
map<string, string> SFX_PATH;
map<string, var> SFX_SOUND;     // name -> its loaded Sound, null when it will not load
map<string, int> SFX_LAST;
var MUSIC = null;               // the theme's Sound

int SFX_RATE = 11025;

string le16(int v) { return "" + to_char(v & 255) + to_char((v >> 8) & 255); }
string le32(int v) { return "" + to_char(v & 255) + to_char((v >> 8) & 255) + to_char((v >> 16) & 255) + to_char((v >> 24) & 255); }
string be16(int v) { return "" + to_char((v >> 8) & 255) + to_char(v & 255); }
string be32(int v) { return "" + to_char((v >> 24) & 255) + to_char((v >> 16) & 255) + to_char((v >> 8) & 255) + to_char(v & 255); }

string wav_wrap(string data) {
	int n = data.size();
	return "RIFF" + le32(36 + n) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(SFX_RATE) + le32(SFX_RATE) + le16(1) + le16(8) + "data" + le32(n) + data;
}

// one voice: wave 0 square, 1 saw, 2 sine, 3 noise, 4 triangle; frequency sweeps f0 -> f1;
// amplitude attacks then decays; volume 0..100
string synth(int wave, float f0, float f1, float secs, int vol, float attack, int seed) {
	int n = math::itrunc(secs * SFX_RATE);
	string out = "";
	float phase = 0.0;
	int noise = seed * 7919 + 17;
	int hold = 0;
	int held = 0;
	for (int i = 0; i < n; ++i) {
		float t = i / (n * 1.0);
		float f = f0 + (f1 - f0) * t;
		phase = phase + f / SFX_RATE;
		if (phase >= 1.0) { phase = phase - math::floor(phase); }
		float s = 0.0;
		if (wave == 0) { s = phase < 0.5 ? 1.0 : -1.0; }
		else if (wave == 1) { s = phase * 2.0 - 1.0; }
		else if (wave == 2) { s = math::sin(phase * 6.2831853); }
		else if (wave == 4) { s = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0; }
		else {
			// noise, resampled at the sweep frequency (low f = rumble, high f = hiss)
			hold = hold + 1;
			if (hold * f >= SFX_RATE / 2) {
				hold = 0;
				noise = (noise * 1103515245 + 12345) & 2147483647;
				held = (noise >> 16) % 256 - 128;
			}
			s = held / 128.0;
		}
		float env = t < attack ? t / attack : 1.0 - (t - attack) / (1.0 - attack);
		if (env < 0.0) { env = 0.0; }
		int v = 128 + math::itrunc(s * env * vol * 1.2);
		out += to_char(iclamp(v, 1, 255));
	}
	return out;
}

// mix two sample strings (same rate), clipping
string mix(string a, string b) {
	int n = imax(a.size(), b.size());
	string out = "";
	for (int i = 0; i < n; ++i) {
		int x = i < a.size() ? (a[i] + 0) - 128 : 0;
		int y = i < b.size() ? (b[i] + 0) - 128 : 0;
		out += to_char(iclamp(128 + x + y, 1, 255));
	}
	return out;
}

string silence(float secs) { return string::repeat("" + to_char(128), math::itrunc(secs * SFX_RATE)); }

// the sound bank, by name
string sfx_samples(string name) {
	if (name == "hit") { return mix(synth(3, 1800.0, 400.0, 0.09, 70, 0.05, 1), synth(0, 180.0, 90.0, 0.08, 40, 0.05, 2)); }
	if (name == "crit") { return mix(synth(3, 3000.0, 300.0, 0.16, 80, 0.03, 3), synth(0, 330.0, 110.0, 0.15, 60, 0.02, 4)); }
	if (name == "swing") { return synth(3, 5000.0, 900.0, 0.1, 30, 0.4, 5); }
	if (name == "bow") { return synth(1, 900.0, 300.0, 0.08, 40, 0.1, 6); }
	if (name == "arrowhit") { return synth(3, 2200.0, 700.0, 0.06, 55, 0.05, 7); }
	if (name == "zap") { return mix(synth(0, 1400.0, 300.0, 0.18, 45, 0.05, 8), synth(3, 6000.0, 3000.0, 0.12, 20, 0.1, 9)); }
	if (name == "fire") { return synth(3, 700.0, 150.0, 0.35, 60, 0.15, 10); }
	if (name == "thunder") { return mix(synth(3, 300.0, 60.0, 0.9, 85, 0.02, 11), synth(3, 2000.0, 200.0, 0.2, 50, 0.01, 12)); }
	if (name == "boom") { return mix(synth(3, 400.0, 40.0, 0.6, 90, 0.02, 13), synth(2, 90.0, 30.0, 0.5, 60, 0.02, 14)); }
	if (name == "heal") { return synth(2, 520.0, 1040.0, 0.3, 45, 0.3, 15); }
	if (name == "chime" || name == "quest") {
		string a = synth(2, 784.0, 784.0, 0.12, 50, 0.05, 16);
		string b = synth(2, 988.0, 988.0, 0.12, 50, 0.05, 17);
		string c = synth(2, 1175.0, 1175.0, name == "quest" ? 0.35 : 0.2, 55, 0.05, 18);
		return a + b + c;
	}
	if (name == "coin") { return synth(0, 988.0, 988.0, 0.05, 40, 0.02, 19) + synth(0, 1319.0, 1319.0, 0.12, 40, 0.02, 20); }
	if (name == "pickup") { return synth(4, 440.0, 880.0, 0.08, 45, 0.05, 21); }
	if (name == "drink") { return synth(2, 300.0, 700.0, 0.12, 40, 0.3, 22) + synth(2, 300.0, 800.0, 0.12, 40, 0.3, 23); }
	if (name == "eat") { return synth(3, 900.0, 600.0, 0.05, 40, 0.1, 24) + silence(0.04) + synth(3, 900.0, 600.0, 0.05, 40, 0.1, 25); }
	if (name == "die") { return synth(1, 400.0, 60.0, 0.3, 55, 0.05, 26); }
	if (name == "bossdie") { return mix(synth(1, 300.0, 30.0, 1.2, 70, 0.02, 27), synth(3, 500.0, 50.0, 1.2, 60, 0.02, 28)); }
	if (name == "death") {
		string a = synth(4, 392.0, 392.0, 0.3, 55, 0.05, 29);
		string b = synth(4, 370.0, 370.0, 0.3, 55, 0.05, 30);
		string c = synth(4, 349.0, 330.0, 0.7, 55, 0.05, 31);
		return a + b + c;
	}
	if (name == "res") { return synth(2, 262.0, 262.0, 0.12, 45, 0.1, 32) + synth(2, 330.0, 330.0, 0.12, 45, 0.1, 33) + synth(2, 392.0, 392.0, 0.12, 45, 0.1, 34) + synth(2, 523.0, 523.0, 0.35, 50, 0.1, 35); }
	if (name == "levelup" || name == "victory") {
		string out = "";
		array<int> notes = name == "levelup" ? [523, 659, 784, 1047] : [392, 392, 392, 523, 659, 784];
		for (int k = 0; k < notes.size(); ++k) { out += synth(0, notes[k] * 1.0, notes[k] * 1.0, k == notes.size() - 1 ? 0.35 : 0.09, 40, 0.05, 36 + k); }
		return out;
	}
	if (name == "splash") { return synth(3, 2500.0, 500.0, 0.3, 55, 0.08, 44); }
	if (name == "clink" || name == "clang") { return mix(synth(2, 1900.0, 1850.0, 0.15, 45, 0.02, 45), synth(3, 5000.0, 3000.0, 0.05, 30, 0.02, 46)); }
	if (name == "chop") { return synth(3, 1200.0, 300.0, 0.08, 60, 0.05, 47); }
	if (name == "hammer" || name == "craft") { return mix(synth(2, 1500.0, 1400.0, 0.1, 50, 0.02, 48), synth(3, 3000.0, 1000.0, 0.06, 40, 0.02, 49)); }
	if (name == "sizzle") { return synth(3, 6000.0, 5000.0, 0.4, 30, 0.1, 50); }
	if (name == "warp") { return synth(2, 200.0, 1600.0, 0.45, 45, 0.4, 51); }
	if (name == "roar") { return mix(synth(1, 140.0, 70.0, 0.8, 70, 0.1, 52), synth(3, 700.0, 200.0, 0.8, 50, 0.1, 53)); }
	if (name == "squeak") { return synth(0, 1800.0, 2400.0, 0.12, 40, 0.1, 54); }
	if (name == "kazoo") { return synth(1, 330.0, 350.0, 0.18, 45, 0.1, 55) + synth(1, 392.0, 380.0, 0.18, 45, 0.1, 56) + synth(1, 294.0, 300.0, 0.3, 45, 0.1, 57); }
	if (name == "fizzle") { return synth(3, 1500.0, 200.0, 0.25, 35, 0.1, 58); }
	if (name == "chest") { return synth(1, 150.0, 300.0, 0.25, 45, 0.2, 59) + synth(0, 784.0, 1047.0, 0.15, 35, 0.05, 60); }
	if (name == "ribbit") { return synth(0, 200.0, 350.0, 0.07, 45, 0.1, 61) + synth(0, 220.0, 380.0, 0.09, 45, 0.1, 62); }
	if (name == "buzz") { return synth(1, 420.0, 440.0, 0.4, 30, 0.2, 63); }
	if (name == "howl") { return synth(2, 400.0, 700.0, 0.3, 45, 0.3, 64) + synth(2, 700.0, 500.0, 0.5, 45, 0.1, 65); }
	if (name == "oink") { return synth(1, 260.0, 180.0, 0.15, 55, 0.1, 66); }
	if (name == "hiss") { return synth(3, 7000.0, 6000.0, 0.35, 30, 0.2, 67); }
	if (name == "snap") { return synth(3, 3000.0, 800.0, 0.06, 70, 0.02, 68); }
	if (name == "squish") { return synth(3, 600.0, 200.0, 0.15, 55, 0.1, 69); }
	if (name == "groan") { return synth(1, 120.0, 90.0, 0.6, 45, 0.3, 70); }
	if (name == "screech") { return synth(1, 2200.0, 1500.0, 0.2, 35, 0.1, 71); }
	if (name == "rumble") { return synth(3, 150.0, 60.0, 0.5, 60, 0.2, 72); }
	if (name == "heartbeat") { return synth(2, 70.0, 45.0, 0.1, 80, 0.1, 73) + silence(0.08) + synth(2, 60.0, 40.0, 0.12, 60, 0.1, 74); }
	return synth(2, 660.0, 660.0, 0.05, 30, 0.1, 99);
}

array<string> SFX_NAMES = ["hit", "crit", "swing", "bow", "arrowhit", "zap", "fire", "thunder", "boom", "heal", "chime", "quest", "coin",
	"pickup", "drink", "eat", "die", "bossdie", "death", "res", "levelup", "victory", "splash", "clink", "clang", "chop", "hammer", "craft",
	"sizzle", "warp", "roar", "squeak", "kazoo", "fizzle", "chest", "ribbit", "buzz", "howl", "oink", "hiss", "snap", "squish", "groan",
	"screech", "rumble", "heartbeat"];

// build any missing wavs; true when the cache is complete
bool audio_prepare(string dir) {
	AUDIO_DIR = dir;
	if (!writable_dir(dir)) { return false; }
	SFX_PATH = {};
	for (int i = 0; i < SFX_NAMES.size(); ++i) {
		string p = dir + "/sfx_" + SFX_NAMES[i] + ".wav";
		if (!file_exists(p)) {
			if (!write_file(p, wav_wrap(sfx_samples(SFX_NAMES[i])))) { continue; }
		}
		SFX_PATH[SFX_NAMES[i]] = p;
	}
	return true;
}

void sfx(string name) {
	if (!AUDIO_ON || !SFX_ON || !SFX_PATH.has(name)) { return; }
	int last = SFX_LAST.get(name, -1000);
	if (NOW_MS - last < 70) { return; }
	SFX_LAST[name] = NOW_MS;
	if (!SFX_SOUND.has(name)) { SFX_SOUND[name] = load_sound(SFX_PATH[name]); }
	var s = SFX_SOUND[name];
	if (s != null) { s.play(); }
}

// sounds from the world: only what is near enough to hear
void sfx_at(ClientView& v, string name, int x, int y) {
	int me = cv_me(v);
	if (me >= 0 && cheb(x, y, v.m_x[me], v.m_y[me]) > 12) { return; }
	sfx(name);
}

// ------------------------------------------------------------------ music ---
string midi_varlen(int v) {
	if (v < 0) { v = 0; }
	string out = "" + to_char(v & 127);
	v = v >> 7;
	while (v > 0) {
		out = to_char(0x80 | (v & 127)) + out;
		v = v >> 7;
	}
	return out;
}

// per theme: tempo (bpm), instrument program, scale (semitone offsets), root note, chord degrees, drums on/off
array<int> THEME_TEMPO = [118, 86, 76, 68, 84, 140, 100, 96, 112, 60];
array<int> THEME_PROG = [105, 22, 11, 19, 9, 30, 73, 58, 61, 20];
array<int> THEME_BASSPROG = [32, 32, 89, 19, 89, 33, 32, 58, 57, 19];
array<int> THEME_ROOT = [60, 57, 62, 57, 62, 52, 65, 48, 60, 57];
array<string> THEME_SCALE = ["0,2,4,7,9", "0,3,5,6,7,10", "0,2,3,5,7,9,10", "0,2,3,5,7,8,11", "0,2,3,5,7,8,10", "0,1,4,5,7,8,10", "0,2,4,6,7,9,11", "0,1,3,4,6,7,9,10", "0,2,4,5,7,9,11", "0,2,3,5,7,8,10"];
array<string> THEME_CHORDS = ["0,3,4,0", "0,0,3,4", "0,5,3,4", "0,5,6,4", "0,5,3,6", "0,1,0,6", "0,1,4,0", "0,1,0,1", "0,3,4,4", "0,5,3,4"];
array<int> THEME_DRUMS = [1, 1, 0, 0, 0, 1, 1, 1, 1, 0];

string midi_theme(int theme) {
	int tempo = THEME_TEMPO[theme];
	array<int> scale = dec_ints(THEME_SCALE[theme]);
	array<int> chords = dec_ints(THEME_CHORDS[theme]);
	int root = THEME_ROOT[theme];
	int tpq = 96;
	string trk = "";
	int us = 60000000 / tempo;
	trk += midi_varlen(0) + to_char(0xFF) + to_char(0x51) + to_char(3) + to_char((us >> 16) & 255) + to_char((us >> 8) & 255) + to_char(us & 255);
	trk += midi_varlen(0) + to_char(0xC0) + to_char(THEME_PROG[theme]);
	trk += midi_varlen(0) + to_char(0xC1) + to_char(THEME_BASSPROG[theme]);
	trk += midi_varlen(0) + to_char(0xB0) + to_char(7) + to_char(80);
	trk += midi_varlen(0) + to_char(0xB1) + to_char(7) + to_char(70);
	// compose on a 16th-note grid into timed 3-byte events, then emit them in time order
	array<int> at;
	array<string> msg;
	int seed = theme * 7919 + 31;
	int bars = 16;
	int step = tpq / 4;
	int melody = 2 + scale.size();
	for (int bar = 0; bar < bars; ++bar) {
		int deg = chords[bar % chords.size()];
		int chord_root = root - 12 + scale[deg % scale.size()] + (deg >= scale.size() ? 12 : 0);
		for (int s = 0; s < 16; ++s) {
			int t = (bar * 16 + s) * step;
			// bass: root on the beat, a fifth on beat three
			if (s % 4 == 0) {
				int bn = iclamp(chord_root - 12 + (s == 8 ? 7 : 0), 20, 100);
				at.push(t);
				msg.push("" + to_char(0x91) + to_char(bn) + to_char(90));
				at.push(t + step * 3);
				msg.push("" + to_char(0x81) + to_char(bn) + to_char(0));
			}
			// melody: a hashed random walk on the scale, resting now and then, a cadence every fourth bar
			seed = (seed * 1103515245 + 12345) & 2147483647;
			int r = (seed >> 8) % 100;
			bool play = (s % 2 == 0 && r < 70) || (s % 2 == 1 && r < 25);
			if (bar % 4 == 3 && s > 8) { play = s == 12; }
			if (play) {
				seed = (seed * 1103515245 + 12345) & 2147483647;
				melody = iclamp(melody + (seed >> 8) % 5 - 2, 0, scale.size() * 2);
				int note = iclamp(root + scale[melody % scale.size()] + (melody / scale.size()) * 12, 30, 100);
				at.push(t);
				msg.push("" + to_char(0x90) + to_char(note) + to_char(96));
				at.push(t + (bar % 4 == 3 ? step * 4 : step));
				msg.push("" + to_char(0x80) + to_char(note) + to_char(0));
			}
			if (THEME_DRUMS[theme] != 0) {
				if (s % 8 == 0) { at.push(t); msg.push("" + to_char(0x99) + to_char(36) + to_char(100)); }
				if (s % 8 == 4) { at.push(t); msg.push("" + to_char(0x99) + to_char(38) + to_char(90)); }
				if (s % 2 == 0) { at.push(t); msg.push("" + to_char(0x99) + to_char(42) + to_char(55)); }
			}
		}
	}
	// stable insertion sort by time (a few hundred events, composed once)
	for (int i = 1; i < at.size(); ++i) {
		int ti = at[i];
		string mi = msg[i];
		int j = i - 1;
		while (j >= 0 && at[j] > ti) {
			at[j + 1] = at[j];
			msg[j + 1] = msg[j];
			j = j - 1;
		}
		at[j + 1] = ti;
		msg[j + 1] = mi;
	}
	int last = 0;
	for (int i = 0; i < at.size(); ++i) {
		trk += midi_varlen(at[i] - last) + msg[i];
		last = at[i];
	}
	int end = bars * 16 * step;
	trk += midi_varlen(imax(0, end - last)) + to_char(0xFF) + to_char(0x2F) + to_char(0);
	return "MThd" + be32(6) + be16(0) + be16(1) + be16(tpq) + "MTrk" + be32(trk.size()) + trk;
}

// -1 title, -2 death, else a world's theme
void music_play(int which) {
	if (!AUDIO_ON || which == MUSIC_CUR) { return; }
	MUSIC_CUR = which;
	music_stop();
	if (!MUSIC_ON) { return; }
	int theme = which == -1 ? 8 : (which == -2 ? 9 : WORLDS[which].music);
	string p = AUDIO_DIR + "/theme" + to_string(theme) + ".mid";
	if (!file_exists(p)) { write_file(p, midi_theme(theme)); }
	MUSIC = load_sound(p);
	if (MUSIC != null) { MUSIC.play(true); }
}

void music_stop() {
	if (MUSIC != null) { MUSIC.stop(); }
	MUSIC = null;
}

void music_toggle() {
	MUSIC_ON = !MUSIC_ON;
	if (!MUSIC_ON) { music_stop(); }
	else { int cur = MUSIC_CUR; MUSIC_CUR = -99; music_play(cur); }
}
