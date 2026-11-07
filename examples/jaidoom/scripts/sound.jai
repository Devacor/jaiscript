// JaiDOOM sound.jai — the WAD's own voice, zero audio libraries. DS* lumps
// are DMX: u16 format(3), u16 sample rate, u32 length (incl. 16+16 pad
// bytes), then 8-bit unsigned PCM — which is exactly WAV's 8-bit encoding,
// so the samples pass through VERBATIM behind a minted 44-byte RIFF header.
// The runner's Sound does the playback (each loaded once, replays restart it,
// silent off Windows). Lumps convert lazily to cached .wav files on first play.

import "wad.jai";
import "vid.jai";

// game.jai's player position (importing it here would cycle): extern-declared so programs that never import game.jai still check
extern float P_X;
extern float P_Y;
extern float P_ANG;

bool SND_ON = false;          // play() flips it on; gates and benches stay silent
var SND_READY = {};           // lump name -> its Sound (null = missing/undecodable)

function snd_wav_bytes(string name) -> string {
	int lu = lump_num(name);
	if (lu < 0) { return ""; }
	int pos = LUMP_POS[lu];
	binary::reader r = lump_reader(lu);
	if (LUMP_SIZE[lu] < 10 || r.uint16() != 3) { return ""; }
	int rate = r.uint16();
	int count = r.int32();
	int ofs = 8;
	if (count > 32) { ofs = 24; count = count - 32; }   // strip the DMX lead/tail pads
	if (count > LUMP_SIZE[lu] - ofs) { count = LUMP_SIZE[lu] - ofs; }
	if (count <= 0) { return ""; }
	binary::writer wav = wav_header(1, rate, count);
	wav.bytes(WAD.substr(pos + ofs, count));
	return wav.to_string();
}

// a RIFF/WAVE header for 8-bit PCM: channels, sample rate, then count data bytes to follow
function wav_header(int channels, int rate, int count) -> binary::writer {
	binary::writer w;
	w.bytes("RIFF").uint32(36 + count).bytes("WAVEfmt ").uint32(16).uint16(1).uint16(channels);
	w.uint32(rate).uint32(rate * channels).uint16(channels).uint16(8).bytes("data").uint32(count);
	return w;
}

function snd_play(string name) -> void {
	if (!SND_ON) { return; }
	if (!SND_READY.has(name)) {
		var path = audio_cache_dir() + string::to_lower(name) + ".wav";
		if (!file_exists(path)) {                                        // bootstrap (or an earlier run) already decoded it
			var wav = snd_wav_bytes(name);
			if (wav == "" || !write_file(path, wav)) { path = ""; }
		}
		SND_READY[name] = path != "" ? load_sound(path) : null;
	}
	var s = SND_READY[name];
	if (s != null) { s.play(); }
}

// ---------------------------------------------------------- positional audio ---
// Pure-script 3D-ish sound with zero host changes: distance picks an
// attenuation tier, bearing picks a pan, and each (name, tier, pan) mints a
// cached STEREO wav variant whose per-ear gains were baked into the samples
// at generation time. ~25 sounds x at most 9 variants, lazily created.

var SND_POS_READY = {};       // "NAME_tN_pP" -> its Sound (null = missing/undecodable)
int SPA_TIER = 0;             // 0 full <320, 1 mid <704, 2 far <1200
int SPA_PAN = 0;              // 0 center, 1 left, 2 right (bearing beyond ±0.5 rad)

function snd_pos_pick(float dist, float rel) -> bool {
	if (dist >= 1200.0) { return false; }
	SPA_TIER = dist < 320.0 ? 0 : (dist < 704.0 ? 1 : 2);
	SPA_PAN = rel > 0.5 ? 1 : (rel < -0.5 ? 2 : 0);
	return true;
}

// the mono decode's twin: same DMX header walk, but every sample lands twice
// (interleaved LR) scaled 128 + (s-128)*num/den per ear — 8-bit PCM stays 8-bit
function snd_wav_stereo(string name, int lnum, int rnum, int den) -> string {
	int lu = lump_num(name);
	if (lu < 0) { return ""; }
	int pos = LUMP_POS[lu];
	binary::reader r = lump_reader(lu);
	if (LUMP_SIZE[lu] < 10 || r.uint16() != 3) { return ""; }
	int rate = r.uint16();
	int count = r.int32();
	int ofs = 8;
	if (count > 32) { ofs = 24; count = count - 32; }   // strip the DMX lead/tail pads
	if (count > LUMP_SIZE[lu] - ofs) { count = LUMP_SIZE[lu] - ofs; }
	if (count <= 0) { return ""; }
	binary::writer wav = wav_header(2, rate, count * 2);
	array<int> samples = WAD.to_bytes(pos + ofs, count);
	for (int i = 0; i < count; ++i) {
		int s = samples[i] - 128;
		wav.uint8(128 + s * lnum / den).uint8(128 + s * rnum / den);
	}
	return wav.to_string();
}

function snd_play_at(string name, float x, float y) -> void {
	if (!SND_ON) { return; }
	float dx = x - P_X;
	float dy = y - P_Y;
	float rel = math::atan2(dy, dx) - P_ANG;
	while (rel > math::PI) { rel = rel - math::TAU; }
	while (rel < 0.0 - math::PI) { rel = rel + math::TAU; }
	if (!snd_pos_pick(math::sqrt(dx * dx + dy * dy), rel)) { return; }
	if (SPA_TIER == 0 && SPA_PAN == 0) { snd_play(name); return; }   // close and ahead: the plain lump
	var key = `${name}_t${SPA_TIER}_p${SPA_PAN}`;
	if (!SND_POS_READY.has(key)) {
		int g = SPA_TIER == 0 ? 16 : (SPA_TIER == 1 ? 8 : 4);
		int ln = SPA_PAN == 2 ? g / 2 : g;
		int rn = SPA_PAN == 1 ? g / 2 : g;
		var wav = snd_wav_stereo(name, ln, rn, 16);
		var path = wav == "" ? "" : audio_cache_dir() + string::to_lower(key) + ".wav";
		if (path != "" && !write_file(path, wav)) { path = ""; }
		SND_POS_READY[key] = path != "" ? load_sound(path) : null;
	}
	var s = SND_POS_READY[key];
	if (s != null) { s.play(); }
}
