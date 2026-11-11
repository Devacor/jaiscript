// Runs a script on the WebAssembly build under Node, the way the page drives it. The script's folder is mounted as the
// working directory, so its imports and files resolve as they do natively.
//   node tools/jaiscript_web/run_node.mjs [--budget <seconds>] <build>/bin/jaiscript_web.js <script.jai> [script args...]
// The Asyncify build is driven like the main-thread page: one jaiweb_tick per frame, waiting out each sleep. A build without
// Asyncify (jaiscript_web_worker.js) runs the script to its end in one call, the way the worker page does, under the
// execution budget --budget gives it (none by default).
import path from "node:path";
import { pathToFileURL } from "node:url";

const options = process.argv.slice(2);
const budget = options[0] === "--budget" ? Number(options.splice(0, 2)[1]) : 0;
const [moduleJs, script, ...scriptArgs] = options;
if (!moduleJs || !script) {
	console.error("usage: node run_node.mjs [--budget <seconds>] <jaiscript_web.js> <script.jai> [args...]");
	process.exit(2);
}

const { default: createJaiScript } = await import(pathToFileURL(path.resolve(moduleJs)).href);
const jai = await createJaiScript({ jaiwebWrite: bytes => process.stdout.write(bytes) });

const folder = path.dirname(path.resolve(script));
jai.FS.mkdir("/work");
jai.FS.mount(jai.NODEFS, { root: folder }, "/work");
jai.FS.chdir("/work");
jai.ccall("jaiweb_resize", null, ["number", "number"], [process.stdout.columns || 100, process.stdout.rows || 40]);

const name = path.basename(script);
const args = scriptArgs.join("\n");
const finish = code => process.stdout.write("", () => process.exit(code));

// Asyncify cannot carry a C++ exception back across a fiber switch: a failed stepped run throws the exception itself
const failed = error => {
	const text = error instanceof WebAssembly.Exception ? jai.getExceptionMessage(error)[1] : String(error);
	process.stderr.write(text.trimEnd() + "\n");
	finish(1);
};

if (path.basename(moduleJs).includes("worker")) {
	finish(jai.ccall("jaiweb_run_file", "number", ["string", "string", "number", "number"], [name, args, budget, 0]));
} else {
	let started = true;
	try {
		jai.ccall("jaiweb_start_file", null, ["string", "string"], [name, args]);
	} catch (error) {
		started = false;
		failed(error);
	}
	const tick = () => {
		try {
			if (jai._jaiweb_running()) { jai._jaiweb_tick(); }
		} catch (error) {
			failed(error);
			return;
		}
		if (!jai._jaiweb_running()) {
			finish(jai.ccall("jaiweb_exit_code", "number", [], []));
			return;
		}
		setTimeout(tick, jai.ccall("jaiweb_wait_ms", "number", [], []));
	};
	if (started) { tick(); }
}
