// The Play page: the sandbox tab and the game tabs, one bar of keys, sound and Stop under whichever shows
"use strict";

(() => {
	const GAMES = {
		demoreel: { name: "the demo reel", cols: 100, rows: 40, keys: "esc quits" },
		gloom: { name: "GLOOM", cols: 120, rows: 45,
			keys: "W S move, A D strafe, arrows turn, space fires, E uses, shift runs, 1 2 3 weapons, M map, esc quits" },
		grubwell: { name: "GRUBWELL", cols: 135, rows: 40,
			keys: "W S step, A D turn, Q E sidestep, Z skill, G pick up, I pack, tab descends, ? all keys, esc pauses" },
	};
	const TABS = ["sandbox", ...Object.keys(GAMES)];
	const sandboxPanel = document.getElementById("sandbox");
	const gamePanel = document.getElementById("game");
	const term = document.getElementById("play-term");
	const cover = document.getElementById("play-cover");
	const run = document.getElementById("play-run");
	const stop = document.getElementById("play-stop");
	const keys = document.getElementById("play-keys");
	const note = document.getElementById("play-note");
	const status = document.getElementById("play-status");
	const showStatus = (text) => { status.textContent = text; };
	let tab = "sandbox";
	let session = null;

	function showCover(text) {
		run.textContent = "Play " + GAMES[tab].name;
		note.textContent = text || "";
		cover.hidden = false;
	}

	function stopAll() {
		JaiSandbox.stop("stopped");
		if (session) { session.stop(); session = null; }
	}

	function pick(id, remember) {
		stopAll();
		tab = id;
		for (const button of document.querySelectorAll(".play-pick")) {
			button.setAttribute("aria-pressed", button.dataset.game === id ? "true" : "false");
		}
		if (remember) { history.replaceState(null, "", "#" + id); }
		sandboxPanel.hidden = id !== "sandbox";
		gamePanel.hidden = id === "sandbox";
		if (id === "sandbox") {
			keys.textContent = JaiSandbox.keys;
			JaiSandbox.open(showStatus);
			return;
		}
		keys.textContent = GAMES[id].keys;
		term.replaceChildren();
		session = new JaiPlay.Session(term, {
			cols: GAMES[id].cols, rows: GAMES[id].rows, canvas: true,
			onStatus: showStatus,
			onExit: (code) => showCover(code === 0 ? "" : "The game ended with exit code " + code + "."),
		});
		showCover();
	}

	for (const button of document.querySelectorAll(".play-pick")) {
		button.addEventListener("click", () => pick(button.dataset.game, true));
	}
	run.addEventListener("click", () => {
		cover.hidden = true;
		session.start({ game: "games/" + tab + ".json" });
	});
	stop.addEventListener("click", () => {
		if (tab === "sandbox") {
			JaiSandbox.stop("stopped");
			return;
		}
		session.stop();
		showCover("Stopped.");
	});
	window.addEventListener("hashchange", () => {
		const id = location.hash.slice(1);
		if (TABS.includes(id) && id !== tab) { pick(id, false); }
	});
	JaiAudio.controls(document.getElementById("play-audio"));
	const asked = location.hash.slice(1);
	pick(TABS.includes(asked) ? asked : "sandbox", false);
})();
