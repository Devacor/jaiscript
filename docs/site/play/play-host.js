// Runs JaiScript programs on a page: a screen (play-screen.js's canvas for the games, xterm.js for the sandbox's scrolling
// text) plus the engine as WebAssembly in a worker (play-worker.js), so a slow frame never stalls the page. Where the page is
// cross-origin isolated and its worker can start workers of its own, the threaded engine shares memory with the page and
// sleeps for real; elsewhere the worker runs the Asyncify build, which returns to the worker's event loop at every sleep.
// Output's bare newlines become CRLF, as a tty's output processing makes them.
"use strict";

const JaiPlay = (() => {
	const RING = 1024;
	const EDIT_BYTES = 1 << 18;
	const NAMED = { " ": 128, ArrowLeft: 129, ArrowRight: 130, ArrowUp: 131, ArrowDown: 132, Shift: 133, Control: 134,
		Alt: 135, Escape: 136, Enter: 137, Tab: 138, Backspace: 139 };
	const GAME_KEYS = new Set([" ", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Tab", "Backspace", "Enter"]);
	const isolated = typeof SharedArrayBuffer !== "undefined" && self.crossOriginIsolated === true;
	// the threaded engine also needs its worker to start workers of its own; asked once, before the first run
	const threaded = !isolated ? Promise.resolve(false) : new Promise((resolve) => {
		const probe = new Worker("play-worker.js");
		const done = (ok) => { probe.terminate(); resolve(ok); };
		const timer = setTimeout(() => done(false), 3000);
		probe.onmessage = (message) => { if (message.data.threads !== undefined) { clearTimeout(timer); done(message.data.threads); } };
		probe.onerror = () => { clearTimeout(timer); done(false); };
		probe.postMessage({ probe: true });
	});

	function keyCode(event) {
		if (event.key in NAMED) { return NAMED[event.key]; }
		if (event.key.length === 1) {
			const code = event.key.toLowerCase().charCodeAt(0);
			if (code > 32 && code < 127) { return code; }
		}
		return -1;
	}

	class Session {
		// options: cols, rows, canvas (a game screen), scrollback, fitHeight, onExit(code), onStatus(text)
		constructor(element, options) {
			this.element = element;
			this.options = options;
			this.held = new Map();
			if (options.canvas) {
				this.screen = new JaiScreen.Screen(element, options.cols, options.rows);
			} else {
				this.terminal = new Terminal({
					cols: options.cols, rows: options.rows, scrollback: options.scrollback || 0, convertEol: true,
					disableStdin: true, cursorBlink: false, fontFamily: '"JetBrains Mono", Menlo, Consolas, monospace',
					fontSize: 14, theme: { background: "#05060a" },
				});
				this.terminal.open(element);
				try { this.terminal.loadAddon(new WebglAddon.WebglAddon()); } catch (error) { /* the DOM renderer draws instead */ }
			}
			this.fit();
			if (document.fonts) { document.fonts.ready.then(() => this.fit()); }
			window.addEventListener("resize", () => this.fit());
			element.tabIndex = 0;
			// capture: xterm's own key handler stops the event before it bubbles
			element.addEventListener("keydown", (event) => this.key(event, true), true);
			element.addEventListener("keyup", (event) => this.key(event, false), true);
			element.addEventListener("blur", () => this.releaseKeys());
			threaded.then((ok) => this.status(ok ? "engine: worker, threads"
				: isolated ? "engine: worker, stepped (slower: this browser cannot start the engine's threads)"
				: "engine: worker, stepped (slower: this page is not cross-origin isolated)"));
		}

		status(text) { if (this.options.onStatus) { this.options.onStatus(text); } }

		// The font size that fits the columns across the stage and the rows into most of the window
		fit() {
			const width = this.element.parentElement.clientWidth - 16;
			const height = window.innerHeight * 0.82;
			if (this.screen) { this.screen.fit(width, height); return; }
			const across = width / (this.options.cols * 0.6);
			const down = this.options.fitHeight === false ? across : height / (this.options.rows * 1.2);
			const size = Math.max(5, Math.min(18, Math.floor(Math.min(across, down))));
			if (this.terminal.options.fontSize !== size) { this.terminal.options.fontSize = size; }
		}

		write(bytes) { (this.screen || this.terminal).write(bytes); }

		key(event, down) {
			if (!this.running) { return; }
			const code = down ? keyCode(event) : this.held.get(event.code);
			if (code === undefined || code < 0) { return; }
			if (GAME_KEYS.has(event.key) || event.key.length === 1) { event.preventDefault(); }
			if (down) { this.held.set(event.code, code); } else { this.held.delete(event.code); }
			this.send(code, down);
		}

		releaseKeys() {
			for (const code of this.held.values()) { this.send(code, false); }
			this.held.clear();
		}

		send(code, down) {
			if (this.words) {
				const n = Atomics.load(this.words, 3);
				Atomics.store(this.words, 16 + (n % RING), code | ((down ? 1 : 0) << 16));
				Atomics.store(this.words, 3, n + 1);
			} else if (this.worker) {
				this.worker.postMessage({ key: code, down });
			}
		}

		finished(code) {
			this.running = false;
			if (this.options.onExit) { this.options.onExit(code); }
		}

		// game: a bundle URL ({entry, args, files}); source: program text run under budget seconds; live: text saved as
		// the file program.jai beside the source, which edit() rewrites while the run goes on; attended: false for a run
		// no click began, which leaves the page its focus and the sound locked
		async start({ game, source, live, budget = 0, attended = true }) {
			const useThreads = await threaded;
			this.stop();
			if (attended) { JaiAudio.unlock(); }
			(this.screen || this.terminal).reset();
			this.running = true;
			if (attended) { this.element.focus(); }
			const { cols, rows } = this.options;
			this.worker = new Worker("play-worker.js");
			this.worker.onmessage = (message) => {
				const data = message.data;
				if (data.out) { this.write(data.out); }
				if (data.sound) { JaiAudio.handle(data.sound[0], data.sound[1], data.bytes, data.sound[2]); }
				if (data.exit !== undefined) { this.finished(data.exit); }
				if (data.failed !== undefined) { this.write("\r\n" + data.failed + "\r\n"); this.finished(1); }
			};
			if (useThreads) {
				this.words = new Int32Array(new SharedArrayBuffer(4 * (16 + RING)));
				this.words[1] = cols;
				this.words[2] = rows;
				const edits = new SharedArrayBuffer(8 + EDIT_BYTES);
				this.edits = { words: new Int32Array(edits, 0, 2), bytes: new Uint8Array(edits, 8) };
				this.worker.postMessage({ shared: this.words.buffer, edits, game, source, live, budget });
			} else {
				this.worker.postMessage({ game, source, live, budget, cols, rows });
			}
		}

		// Rewrites the live file of the run under way. The threaded engine's worker never returns to its event loop, so
		// the text crosses in shared memory: [0] a count that is odd while the page writes, [1] the text's length
		edit(text) {
			if (!this.running) { return; }
			if (!this.edits) { this.worker.postMessage({ edit: text }); return; }
			const bytes = new TextEncoder().encode(text);
			if (bytes.length > EDIT_BYTES) { return; }
			Atomics.add(this.edits.words, 0, 1);
			this.edits.bytes.set(bytes);
			Atomics.store(this.edits.words, 1, bytes.length);
			Atomics.add(this.edits.words, 0, 1);
		}

		stop() {
			if (this.worker) { this.worker.terminate(); this.worker = null; }
			JaiAudio.reset();
			this.words = null;
			this.edits = null;
			this.held.clear();
			this.running = false;
		}
	}

	return { Session, isolated };
})();
