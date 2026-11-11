// The Play page's sandbox tab: the program in the editor runs on the engine in this page, its output in the terminal above it.
// The first sample is a game in a file of its own, and it starts by itself the first time the tab shows.
// A program that declares bool frame() runs live: sandbox/live.jai calls it frame after frame, and the editor's text is
// saved as the file it reloads while the program runs.
"use strict";

const JaiSandbox = (() => {
	const BUDGET_SECONDS = 20;
	const SAMPLES = [
		{ id: "cakewalk", name: "Cakewalk: a game to walk around in", file: "sandbox/cakewalk.jai" },
		{ id: "hello", name: "Hello", source: "// Edit me, then press Run (or Ctrl+Enter)\nstring greet(string name) {\n\treturn `Hello, ${name}!`;\n}\n\narray<string> names = [\"gator\", \"cake\", \"world\"];\nfor (auto name : names) {\n\tprint(greet(name));\n}\nprint(`${names.size()} greetings, all checked before anything ran`);\n" },
		{ id: "values", name: "Copies and references", source: "// Assignment copies, as in C++: a T& or a shared_ptr<T> shares\nclass Point {\n\tint x = 0;\n\tint y = 0;\n\tPoint(int x_, int y_) { x = x_; y = y_; }\n\tstring to_string() { return `(${x}, ${y})`; }\n}\n\nPoint a = Point(1, 2);\nPoint b = a;            // a copy\nb.x = 10;\nprint(`a ${a.to_string()}  b ${b.to_string()}`);\n\nPoint& alias = a;       // a reference\nalias.y = 20;\nprint(`a ${a.to_string()} after writing through the reference`);\n\nshared_ptr<Point> shared = new Point(3, 4);\nshared_ptr<Point> same = shared;\nsame.x = 30;\nprint(`shared ${shared.to_string()}: one object, two handles`);\n" },
		{ id: "coroutines", name: "Coroutines", source: "// A coroutine yields values one at a time and keeps its place between them\ncoroutine int countdown(int from) {\n\tfor (int i = from; i > 0; --i) {\n\t\tyield i;\n\t}\n}\n\nfor (auto n : countdown(5)) {\n\tprint(\"{}...\", n);\n\tsleep(0.3);\n}\nprint(\"liftoff\");\n" },
		{ id: "animate", name: "Escape codes and sleep", source: "// The terminal takes escape codes: draw, sleep, draw again\nstring ESC = \"\\x1b\";\nprint(ESC + \"[?25l\", skip_newline);\nfor (int frame = 0; frame < 60; ++frame) {\n\tstring line = \"\";\n\tfor (int x = 0; x < 48; ++x) {\n\t\tfloat wave = math::sin((x + frame) * 0.25);\n\t\tint shade = 90 + cast<int>((wave + 1.0) * 80.0);\n\t\tline += `${ESC}[48;2;20;${shade};${255 - shade}m `;\n\t}\n\tprint(ESC + \"[2;1H\" + line + ESC + \"[0m\", skip_newline);\n\tsleep(1.0 / 30.0);\n}\nprint(ESC + \"[?25h\");\nprint(\"done\");\n" },
		{ id: "strict", name: "The strict checker", source: "// The checker reads the whole program before anything runs:\n// this one never prints, because line 6 cannot work\nprint(\"you will not see this\");\n\nint count = 3;\nstring label = count;   // int is not a string\nprint(label);\n" },
	];
	const editor = document.getElementById("sandbox-editor");
	const picker = document.getElementById("sandbox-sample");
	const run = document.getElementById("sandbox-run");
	const panel = document.getElementById("sandbox");
	const launcher = fetch("sandbox/live.jai").then((reply) => reply.ok ? reply.text() : "print(\"sandbox/live.jai did not load\");\n");
	const declaresFrame = (text) => /\bbool\s+frame\s*\(\s*\)/.test(text);
	let session = null;
	let live = false;
	let pendingEdit = 0;
	let onStatus = () => {};
	const note = (text) => session.write("\r\n\x1b[2m[" + text + "]\x1b[0m\r\n");

	for (const sample of SAMPLES) {
		const option = document.createElement("option");
		option.value = sample.id;
		option.textContent = sample.name;
		picker.appendChild(option);
	}
	async function load(id) {
		const sample = SAMPLES.find((s) => s.id === id) || SAMPLES[0];
		if (sample.source === undefined) {
			const reply = await fetch(sample.file).catch(() => null);
			sample.source = reply && reply.ok ? await reply.text() : "// " + sample.file + " did not load\n";
		}
		if (picker.value === sample.id) { editor.value = sample.source; }
	}
	// xterm.js measures its cells when it opens, so the terminal opens the first time the tab shows
	function open(a_onStatus) {
		onStatus = a_onStatus;
		if (session) { return; }
		session = new JaiPlay.Session(document.getElementById("sandbox-term"), {
			cols: 100, rows: 18, scrollback: 2000, fitHeight: false,
			onStatus: (text) => onStatus(text),
			onExit: (code) => {
				note(code === 0 ? "finished" : "exit code " + code);
				run.disabled = false;
			},
		});
		firstSample.then(() => {
			if (picker.value === SAMPLES[0].id && !session.running && !panel.hidden) { start(false); }
		});
	}
	async function start(attended) {
		run.disabled = true;
		const text = editor.value;
		live = declaresFrame(text);
		if (live) {
			session.start({ source: await launcher, live: text, budget: BUDGET_SECONDS, attended });
		} else {
			session.start({ source: text, budget: BUDGET_SECONDS, attended });
		}
	}
	function stop(reason) {
		if (!session || !session.running) { return; }
		session.stop();
		note(reason);
		run.disabled = false;
	}

	picker.addEventListener("change", () => load(picker.value));
	editor.addEventListener("input", () => {
		clearTimeout(pendingEdit);
		pendingEdit = setTimeout(() => { if (live && session && session.running) { session.edit(editor.value); } }, 250);
	});
	run.addEventListener("click", () => start(true));
	editor.addEventListener("keydown", (event) => {
		if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
			event.preventDefault();
			start(true);
		} else if (event.key === "Tab" && !event.shiftKey) {
			event.preventDefault();
			editor.setRangeText("\t", editor.selectionStart, editor.selectionEnd, "end");
		}
	});
	const firstSample = load(SAMPLES[0].id);
	return { open, stop, keys: "Ctrl+Enter runs the editor's program; click the output and it takes your keys" };
})();
