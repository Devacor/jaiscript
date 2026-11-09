// GLOOM's sound, with no audio files: every effect is synthesized at boot into
// an 8-bit WAV from a few swept voices. Feel names the cues each frame and
// sound_frame plays them through the host's load_sound (the runner's, which
// jai_gloom registers too). Playing needs a folder to write the WAVs to
// (HOST_SOUND_DIR); --smoke --look showcase still builds the bank and counts
// the cues, so the gate checks both without a speaker.

int SFX_RATE = 11025;
array<string> SFX_NAMES = ["pistol", "scatter", "hexcast", "boom", "thud", "splat", "scrap", "pain", "pain_big", "death",
	"roar", "hiss", "whir", "bellow", "burst", "spit", "fireball", "door", "pickup", "weapon", "key", "secret", "clear",
	"start"];
// state is declared without an initializer, so a --dev reload of this file keeps it
map<string, var> SFX_SOUNDS;
map<string, int> SFX_LAST;
int SFX_FRAME;
int SFX_BANK_HASH;
int SFX_BANK_BYTES;
int CUE_COUNT;
int CUE_HASH;

// one voice: wave 0 square, 1 saw, 2 sine, 3 noise, 4 triangle, sweeping f0 to f1
// (exponentially, as pitch is heard); a quick attack, then a decay to silence
function voice(int wave, float f0, float f1, float secs, float vol, float attack, int seed) -> array<float> {
	int n = math::itrunc(secs * SFX_RATE);
	array<float> out = [];
	float phase = 0.0;
	float hold = 0.0;
	float held = 0.0;
	int noise = seed * 7919 + 17;
	float glide = math::log(f1 / f0);
	for (int i = 0; i < n; ++i) {
		float t = 1.0 * i / n;
		float f = f0 * math::exp(glide * t);
		phase += f / SFX_RATE;
		phase -= math::floor(phase);
		float s = 0.0;
		if (wave == 0) { s = phase < 0.5 ? 1.0 : -1.0; }
		else if (wave == 1) { s = phase * 2.0 - 1.0; }
		else if (wave == 2) { s = math::sin(phase * 6.2831853); }
		else if (wave == 4) { s = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0; }
		else {
			hold += f / SFX_RATE;
			if (hold >= 1.0) {
				hold -= math::floor(hold);
				noise = (noise * 1103515245 + 12345) & 2147483647;
				held = ((noise >> 16) % 256 - 128) / 128.0;
			}
			s = held;
		}
		float env = t < attack ? t / attack : (1.0 - (t - attack) / (1.0 - attack));
		out.push(s * env * env * vol);
	}
	return out;
}

// b laid over a, starting at sample offset
function overlay_voice(array<float> a, array<float> b, int offset) -> array<float> {
	array<float> out = a;
	while (out.size() < offset + b.size()) { out.push(0.0); }
	for (int i = 0; i < b.size(); ++i) { out[offset + i] += b[i]; }
	return out;
}

function then_voice(array<float> a, array<float> b) -> array<float> { return overlay_voice(a, b, a.size()); }

function at_secs(float secs) -> int { return math::itrunc(secs * SFX_RATE); }

function note_hz(int semis) -> float { return 220.0 * math::pow(2.0, semis / 12.0); }

// the bank, by cue name
function sfx_voices(string name) -> array<float> {
	if (name == "pistol") { return overlay_voice(voice(3, 3200.0, 500.0, 0.16, 0.9, 0.02, 1), voice(0, 180.0, 60.0, 0.12, 0.5, 0.02, 2), 0); }
	if (name == "scatter") {
		array<float> blast = overlay_voice(voice(3, 2200.0, 120.0, 0.42, 1.0, 0.01, 3), voice(2, 90.0, 35.0, 0.35, 0.9, 0.01, 4), 0);
		return overlay_voice(blast, voice(3, 900.0, 600.0, 0.08, 0.35, 0.3, 5), at_secs(0.3));
	}
	if (name == "hexcast") { return overlay_voice(voice(2, 260.0, 1500.0, 0.3, 0.7, 0.1, 6), voice(4, 520.0, 3000.0, 0.25, 0.3, 0.2, 7), 0); }
	if (name == "boom") { return overlay_voice(voice(3, 600.0, 40.0, 0.9, 1.0, 0.01, 8), voice(2, 80.0, 28.0, 0.7, 0.9, 0.01, 9), 0); }
	if (name == "thud") { return voice(3, 1100.0, 250.0, 0.07, 0.7, 0.05, 10); }
	if (name == "splat") { return overlay_voice(voice(3, 900.0, 140.0, 0.35, 0.9, 0.02, 11), voice(1, 220.0, 55.0, 0.3, 0.5, 0.05, 12), 0); }
	if (name == "scrap") { return overlay_voice(voice(3, 4000.0, 700.0, 0.5, 0.8, 0.01, 13), voice(0, 300.0, 80.0, 0.45, 0.4, 0.02, 14), 0); }
	if (name == "pain") { return voice(1, 310.0, 190.0, 0.2, 0.7, 0.1, 15); }
	if (name == "pain_big") { return overlay_voice(voice(1, 280.0, 120.0, 0.38, 0.8, 0.05, 16), voice(3, 600.0, 200.0, 0.2, 0.4, 0.05, 17), 0); }
	if (name == "death") { return overlay_voice(voice(1, 240.0, 45.0, 1.2, 0.8, 0.05, 18), voice(3, 400.0, 60.0, 1.0, 0.5, 0.1, 19), 0); }
	if (name == "roar") { return overlay_voice(voice(1, 150.0, 70.0, 0.75, 0.8, 0.15, 20), voice(3, 800.0, 220.0, 0.75, 0.5, 0.15, 21), 0); }
	if (name == "hiss") { return voice(3, 7000.0, 4000.0, 0.45, 0.6, 0.3, 22); }
	if (name == "whir") { return voice(0, 140.0, 900.0, 0.45, 0.35, 0.6, 23); }
	if (name == "bellow") { return overlay_voice(voice(1, 95.0, 48.0, 1.3, 0.9, 0.1, 24), voice(3, 420.0, 90.0, 1.2, 0.6, 0.1, 25), 0); }
	if (name == "burst") {
		array<float> pop = voice(3, 2800.0, 600.0, 0.07, 0.8, 0.02, 26);
		return overlay_voice(overlay_voice(pop, pop, at_secs(0.09)), pop, at_secs(0.18));
	}
	if (name == "spit") { return overlay_voice(voice(3, 3500.0, 1200.0, 0.14, 0.6, 0.05, 27), voice(2, 700.0, 260.0, 0.14, 0.5, 0.05, 28), 0); }
	if (name == "fireball") { return voice(3, 300.0, 900.0, 0.45, 0.8, 0.4, 29); }
	if (name == "door") { return overlay_voice(voice(0, 55.0, 70.0, 0.7, 0.35, 0.1, 30), voice(3, 260.0, 160.0, 0.7, 0.45, 0.1, 31), 0); }
	if (name == "pickup") { return then_voice(voice(4, 660.0, 660.0, 0.06, 0.6, 0.05, 32), voice(4, 990.0, 990.0, 0.1, 0.6, 0.05, 33)); }
	if (name == "weapon") {
		array<float> out = [];
		array<int> notes = [0, 7, 12, 19];
		for (auto n : notes) { out = then_voice(out, voice(0, note_hz(n), note_hz(n), 0.07, 0.35, 0.05, 34 + n)); }
		return out;
	}
	if (name == "key") {
		array<float> out = [];
		array<int> notes = [12, 16, 19];
		for (auto n : notes) { out = then_voice(out, voice(2, note_hz(n), note_hz(n), 0.1, 0.6, 0.05, 40 + n)); }
		return out;
	}
	if (name == "secret") {
		array<float> out = [];
		array<int> notes = [3, 7, 10, 15, 19, 22];
		for (auto n : notes) { out = then_voice(out, voice(2, note_hz(n + 12), note_hz(n + 12), 0.08, 0.5, 0.05, 50 + n)); }
		return out;
	}
	if (name == "clear") {
		array<float> out = [];
		array<int> notes = [0, 4, 7, 12, 7, 12];
		for (int k = 0; k < notes.size(); ++k) {
			float len = k == notes.size() - 1 ? 0.5 : 0.11;
			out = then_voice(out, overlay_voice(voice(0, note_hz(notes[k]), note_hz(notes[k]), len, 0.3, 0.05, 60 + k),
				voice(2, note_hz(notes[k] - 12), note_hz(notes[k] - 12), len, 0.4, 0.05, 70 + k), 0));
		}
		return out;
	}
	if (name == "start") {
		array<float> hit = overlay_voice(voice(2, 70.0, 40.0, 0.8, 1.0, 0.01, 80), voice(3, 300.0, 60.0, 0.5, 0.5, 0.01, 81), 0);
		return overlay_voice(hit, voice(1, note_hz(-17), note_hz(-17), 1.0, 0.3, 0.3, 82), at_secs(0.05));
	}
	return voice(2, 660.0, 660.0, 0.05, 0.3, 0.1, 99);
}

function le_bytes(int v, int n) -> string {
	string out = "";
	for (int i = 0; i < n; ++i) { out += to_char((v >> (i * 8)) & 255); }
	return out;
}

// mono 8-bit PCM: the oldest WAV every player opens
function wav_bytes(array<float> samples) -> string {
	string data = "";
	for (auto s : samples) {
		int v = 128 + math::itrunc(s * 100.0);
		data += to_char(iclamp(v, 1, 255));
	}
	int n = data.size();
	return "RIFF" + le_bytes(36 + n, 4) + "WAVEfmt " + le_bytes(16, 4) + le_bytes(1, 2) + le_bytes(1, 2) + le_bytes(SFX_RATE, 4)
		+ le_bytes(SFX_RATE, 4) + le_bytes(1, 2) + le_bytes(8, 2) + "data" + le_bytes(n, 4) + data;
}

function text_hash(int h, string s) -> int {
	for (int i = 0; i < s.size(); ++i) { h = mix32(h, s[i] + 0); }
	return h;
}

// build the bank: written and loaded when the host has a sound folder, hashed under --smoke
function sound_boot() -> void {
	if (!SHOWCASE || !SOUND) { return; }
	bool playing = HOST_SOUND_DIR != "";
	if (!playing && !HOST_SMOKE) { return; }
	SFX_BANK_HASH = 2166136261;
	SFX_BANK_BYTES = 0;
	for (auto& name : SFX_NAMES) {
		string bytes = wav_bytes(sfx_voices(name));
		SFX_BANK_HASH = text_hash(SFX_BANK_HASH, bytes);
		SFX_BANK_BYTES += bytes.size();
		if (!playing) { continue; }
		string path = HOST_SOUND_DIR + "/" + name + ".wav";
		if (write_file(path, bytes)) {
			var s = load_sound(path);
			if (s != null) { SFX_SOUNDS[name] = s; }
		}
	}
}

// this frame's cues, each sound at most once every few frames
function sound_frame() -> void {
	SFX_FRAME = SFX_FRAME + 1;
	for (auto& cue : GLOOM_CUES) {
		CUE_COUNT = CUE_COUNT + 1;
		CUE_HASH = text_hash(CUE_HASH, cue);
		if (!SOUND || !SFX_SOUNDS.has(cue)) { continue; }
		if (SFX_FRAME - SFX_LAST.get(cue, -100) < 3) { continue; }
		SFX_LAST[cue] = SFX_FRAME;
		SFX_SOUNDS[cue].play();
	}
}
