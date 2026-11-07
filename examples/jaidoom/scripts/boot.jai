// JaiDOOM boot.jai - the game fetches its own data. Freedoom (BSD) from the Freedoom
// project's release and the DOOM shareware episode from a pinned GitHub mirror, each
// verified by SHA-256 before it is kept, the Freedoom archive unpacked by the runner's
// zip_extract, and every sound and music lump decoded into cache/ up front so the first
// play is silent-free. Interactive mode draws a loading screen on the terminal and asks
// before touching the shareware episode (free to redistribute, not open source);
// `jaidoom.jai -- bootstrap` does the same without a screen for scripts and CI.
//
// Host API used (runner): Download, zip_extract, write_file, file_exists; crypto::sha256_file is the stdlib's.

import "wad.jai";
import "sound.jai";
import "mus.jai";

var BOOT_FREEDOOM_VERSION = "0.13.0";
var BOOT_FREEDOOM_URL = "https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip";
var BOOT_FREEDOOM_ZIP_SHA = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59";
var BOOT_FREEDOOM1_SHA = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d";
var BOOT_FREEDOOM2_SHA = "a8772e088847032510d97ba2312406a6998f21cbab44d4ff10696faa9c0ecd4b";
// DOOM shareware 1.9: the canonical 4,196,020-byte doom1.wad, from the Doom-Utils shareware
// collection at a pinned commit so the bytes cannot drift under the checksum
var BOOT_SHAREWARE_URL = "https://raw.githubusercontent.com/Doom-Utils/shareware-collection/6591a922701e5769b32278e9c555b1bf93f5714c/Doom%201.9/doom1.wad";
var BOOT_SHAREWARE_SHA = "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771";
var BOOT_SHAREWARE_SIZE = 4196020;
function boot_zip() -> string { return wad_dir() + ".download/freedoom-" + BOOT_FREEDOOM_VERSION + ".zip"; }

var BOOT_LINES = [];          // status lines already settled (the screen's history)
var BOOT_ACTIVE = "";         // the line in progress
var BOOT_FRACTION = -1.0;     // progress of the active line; < 0 = no bar
var BOOT_INTERACTIVE = false;
var BOOT_ABORT = false;

function boot_wad_ok(string path, string sha) -> bool {
	return file_exists(path) && crypto::sha256_file(path) == sha;
}
function boot_have_freedoom() -> bool {
	return boot_wad_ok(wad_path("freedoom1"), BOOT_FREEDOOM1_SHA) && boot_wad_ok(wad_path("freedoom2"), BOOT_FREEDOOM2_SHA);
}
function boot_have_shareware() -> bool { return boot_wad_ok(wad_path("doom1"), BOOT_SHAREWARE_SHA); }
function boot_needed(string wanted) -> bool { return !file_exists(wanted); }

// ------------------------------------------------------------------ screen ---
function boot_settle(string line) -> void {
	BOOT_LINES.push(line);
	BOOT_ACTIVE = "";
	BOOT_FRACTION = -1.0;
	if (!BOOT_INTERACTIVE) { print(line); }
}
function boot_bar(int width) -> string {
	if (BOOT_FRACTION < 0.0) { return ""; }
	int filled = cast<int>(BOOT_FRACTION * width);
	string bar = "";
	for (int i = 0; i < width; i++) { bar += i < filled ? "#" : "."; }
	return "[" + bar + "] " + to_string(cast<int>(BOOT_FRACTION * 100.0)) + "%";
}
function boot_draw() -> void {
	if (!BOOT_INTERACTIVE) { return; }
	int cols = term_cols();
	if (cols < 60) { cols = 60; }
	string out = ESC + "[H";
	string pad = "";
	for (int i = 0; i < cols; i++) { pad += " "; }
	var rows = [];
	rows.push("");
	rows.push("  JAIDOOM  -  FETCHING GAME DATA");
	rows.push("  The game data is not part of the repository. JaiDOOM downloads it, verifies");
	rows.push("  every file by SHA-256, unpacks it, and decodes the sound and music itself.");
	rows.push("");
	for (auto l : BOOT_LINES) { rows.push("  " + l); }
	if (BOOT_ACTIVE != "") {
		rows.push("  " + BOOT_ACTIVE);
		string bar = boot_bar(cols - 10);
		if (bar != "") { rows.push("  " + bar); }
	}
	rows.push("");
	rows.push("  Freedoom (c) the Freedoom project, BSD 3-Clause - https://freedoom.github.io");
	rows.push("  DOOM shareware (c) 1993 id Software - freely redistributable, not open source");
	rows.push("  Mirror: github.com/Doom-Utils/shareware-collection  -  esc cancels");
	for (auto r : rows) {
		string line = r;
		if (line.size() > cols) { line = line.substr(0, cols); }
		out += line + pad.substr(0, cols - line.size()) + "\n";
	}
	out += ESC + "[J";
	print(out, skip_newline);
}

// A key edge each frame; esc aborts, y/n answer a pending question ("" = no answer yet)
function boot_pump_keys() -> string {
	if (!BOOT_INTERACTIVE) { return ""; }
	var k = read_key();
	string answer = "";
	while (k != "") {
		if (k == "esc") { BOOT_ABORT = true; }
		else if (k == "y" || k == "n" || k == "enter") { answer = k; }
		k = read_key();
	}
	return answer;
}

// ---------------------------------------------------------------- transfers ---
function boot_fetch(string label, string url, string dest) -> bool {
	auto d = Download();
	if (!d.start(url, dest)) { boot_settle("FAILED  " + label + ": " + d.error()); return false; }
	BOOT_ACTIVE = "fetching " + label;
	BOOT_FRACTION = 0.0;
	int last_pct = -1;
	while (!d.pump()) {
		boot_pump_keys();
		if (BOOT_ABORT) { d.cancel(); }
		BOOT_FRACTION = d.fraction();
		int pct = cast<int>(BOOT_FRACTION * 100.0);
		if (!BOOT_INTERACTIVE && pct / 20 != last_pct / 20 && d.total() > 0) {
			print(`   ${d.received()} / ${d.total()} bytes (${pct}%)`);
			last_pct = pct;
		}
		boot_draw();
		sleep(0.016);
	}
	if (!d.success()) { boot_settle("FAILED  " + label + ": " + d.error()); return false; }
	boot_settle("ok      " + label + " (" + to_string(d.received()) + " bytes)");
	return true;
}
function boot_verify(string label, string path, string sha) -> bool {
	BOOT_ACTIVE = "verifying " + label;
	boot_draw();
	string got = crypto::sha256_file(path);
	if (got != sha) {
		delete_file(path);
		boot_settle("FAILED  " + label + ": checksum mismatch, file discarded");
		return false;
	}
	boot_settle("ok      " + label + " verified  sha256 " + got.substr(0, 12) + "...");
	return true;
}

function boot_freedoom() -> bool {
	if (boot_have_freedoom()) { boot_settle("ok      Freedoom " + BOOT_FREEDOOM_VERSION + " already present and verified"); return true; }
	if (!(file_exists(boot_zip()) && crypto::sha256_file(boot_zip()) == BOOT_FREEDOOM_ZIP_SHA)) {
		if (!boot_fetch("Freedoom " + BOOT_FREEDOOM_VERSION + " (freedoom.github.io, BSD 3-Clause)", BOOT_FREEDOOM_URL, boot_zip())) { return false; }
		if (!boot_verify("freedoom-" + BOOT_FREEDOOM_VERSION + ".zip", boot_zip(), BOOT_FREEDOOM_ZIP_SHA)) { return false; }
	}
	var pairs = [["freedoom1.wad", BOOT_FREEDOOM1_SHA], ["freedoom2.wad", BOOT_FREEDOOM2_SHA]];
	for (auto p : pairs) {
		string name = p[0];
		BOOT_ACTIVE = "unpacking " + name;
		boot_draw();
		if (!zip_extract(boot_zip(), "freedoom-" + BOOT_FREEDOOM_VERSION + "/" + name, wad_dir() + name)) {
			boot_settle("FAILED  could not unpack " + name);
			return false;
		}
		if (!boot_verify(name, wad_dir() + name, p[1])) { return false; }
	}
	delete_file(boot_zip());
	return true;
}

function boot_shareware(bool want) -> bool {
	if (boot_have_shareware()) { boot_settle("ok      DOOM shareware episode already present and verified"); return true; }
	if (!want) { boot_settle("--      DOOM shareware episode skipped"); return true; }
	if (BOOT_INTERACTIVE) {
		BOOT_LINES.push("        (c) 1993 id Software: free to redistribute unmodified, not open source");
		BOOT_ACTIVE = "Also fetch the DOOM shareware episode (doom1.wad)?   Y / N";
		boot_draw();
		string answer = "";
		while (answer == "" && !BOOT_ABORT) { answer = boot_pump_keys(); boot_draw(); sleep(0.016); }
		if (BOOT_ABORT) { return false; }
		if (answer == "n") { boot_settle("--      DOOM shareware episode skipped"); return true; }
	}
	string tmp = wad_dir() + ".download/doom1.wad";
	if (!boot_fetch("DOOM shareware 1.9 (github.com/Doom-Utils/shareware-collection)", BOOT_SHAREWARE_URL, tmp)) { return false; }
	if (!boot_verify("doom1.wad", tmp, BOOT_SHAREWARE_SHA)) { return false; }
	string bytes = read_file(tmp);
	if (bytes.size() != BOOT_SHAREWARE_SIZE || !write_file(wad_dir() + "doom1.wad", bytes)) { boot_settle("FAILED  could not place doom1.wad"); return false; }
	delete_file(tmp);
	return true;
}

// ------------------------------------------------------------------- audio ---
// Every DS* lump to .wav and D_* lump to .mid under the data folder's cache/<wad>/ - the files sound.jai and
// mus.jai would otherwise mint lazily on first play. Returns the count written.
function audio_extract_wad(string path) -> int {
	if (!wad_load(path)) { return -1; }
	string dir = audio_cache_dir();
	int written = 0;
	for (int i = 0; i < NUM_LUMPS; ++i) {
		string name = LUMP_NAME[i];
		if (name.starts_with("DS")) {
			string wav = snd_wav_bytes(name);
			if (wav != "" && write_file(dir + string::to_lower(name) + ".wav", wav)) { written++; }
		} else if (name.starts_with("D_")) {
			string mid = mus_midi_bytes(name);
			if (mid != "" && write_file(dir + string::to_lower(name) + ".mid", mid)) { written++; }
		}
		if ((i & 255) == 0) { BOOT_FRACTION = (i + 0.0) / NUM_LUMPS; boot_draw(); }
	}
	return written;
}
function boot_audio(bool want) -> void {
	if (!want) { boot_settle("--      audio cache skipped (lumps decode lazily on first play)"); return; }
	for (auto w : ["doom1", "freedoom1", "freedoom2"]) {
		string path = wad_path(w);
		if (!file_exists(path)) { continue; }
		BOOT_ACTIVE = "decoding sounds and music from " + w + ".wad";
		BOOT_FRACTION = 0.0;
		boot_draw();
		int n = audio_extract_wad(path);
		if (n < 0) { boot_settle("FAILED  could not read " + w + ".wad"); }
		else { boot_settle("ok      " + w + ": " + to_string(n) + " sounds and songs -> cache/" + w + "/"); }
	}
}

function boot_write_sources(bool shareware) -> void {
	string text = "JaiDOOM game data - fetched by the game's own bootstrap\n\n";
	text += "freedoom1.wad, freedoom2.wad\n";
	text += "  Freedoom " + BOOT_FREEDOOM_VERSION + ", the Freedoom project, https://freedoom.github.io\n";
	text += "  License: BSD 3-Clause (COPYING.txt in this folder)\n";
	text += "  Source:  " + BOOT_FREEDOOM_URL + "\n";
	text += "  sha256:  freedoom1.wad " + BOOT_FREEDOOM1_SHA + "\n           freedoom2.wad " + BOOT_FREEDOOM2_SHA + "\n\n";
	if (shareware) {
		text += "doom1.wad\n";
		text += "  DOOM shareware episode, version 1.9, Copyright 1993 id Software.\n";
		text += "  The shareware episode may be freely redistributed in unmodified form; it is not\n";
		text += "  open source and may not be sold or modified. The registered game data is not included.\n";
		text += "  Source:  " + BOOT_SHAREWARE_URL + "\n";
		text += "  sha256:  " + BOOT_SHAREWARE_SHA + "  (" + to_string(BOOT_SHAREWARE_SIZE) + " bytes)\n\n";
	}
	text += "cache/<wad>/*.wav and *.mid are decoded from the lumps of the WAD named by the folder\nand carry that WAD's license.\n";
	write_file(wad_dir() + "SOURCES.txt", text);
}

// The whole sweep. interactive: terminal loading screen with the shareware question;
// otherwise plain lines. Returns true when the requested WADs are in place.
function boot_run(bool interactive, bool want_shareware, bool want_audio) -> bool {
	BOOT_INTERACTIVE = interactive;
	BOOT_LINES = [];
	BOOT_ABORT = false;
	if (interactive) { print(ESC + "[?1049h" + ESC + "[?25l" + ESC + "[2J", skip_newline); }
	else { print("JaiDOOM bootstrap: fetching game data into wads/ and cache/"); }
	boot_draw();
	bool ok = boot_freedoom();
	if (ok && !BOOT_ABORT) { ok = boot_shareware(want_shareware); }
	if (ok && !BOOT_ABORT) { boot_audio(want_audio); }
	if (ok && !BOOT_ABORT) { boot_write_sources(boot_have_shareware()); boot_settle("ok      wads/SOURCES.txt written"); }
	if (BOOT_ABORT) { boot_settle("cancelled"); ok = false; }
	if (interactive) {
		BOOT_ACTIVE = ok ? "ready - press a key" : "press a key to exit";
		boot_draw();
		string k = "";
		while (k == "") { k = read_key(); sleep(0.016); }
		print(ESC + "[?25h" + ESC + "[?1049l", skip_newline);
	}
	return ok;
}
