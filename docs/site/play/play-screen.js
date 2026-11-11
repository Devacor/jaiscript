// The game pages' screen: a fixed grid of terminal cells drawn on a canvas. It reads what the games write (UTF-8 text,
// SGR colours in 16, 256 and 24-bit, cursor moves, erases, the alternate screen) and draws block elements, quadrants,
// sextants and light box lines as rectangles in the cell, so a half-block pixel is exactly half a cell and no glyph cache
// can fill up. Other characters are the font's. A cell is twice as tall as it is wide: half-block pixels come out square.
"use strict";

const JaiScreen = (() => {
	const DEFAULT_FG = 0xe6e3f0;
	const DEFAULT_BG = 0x05060a;
	const BASIC = [0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
		0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff];
	const PALETTE = (() => {
		const result = BASIC.slice();
		const steps = [0, 95, 135, 175, 215, 255];
		for (let i = 0; i < 216; i += 1) {
			result.push((steps[Math.floor(i / 36)] << 16) | (steps[Math.floor(i / 6) % 6] << 8) | steps[i % 6]);
		}
		for (let i = 0; i < 24; i += 1) { const v = 8 + i * 10; result.push((v << 16) | (v << 8) | v); }
		return result;
	})();
	const BOLD = 1;
	const DIM = 2;
	const INVERSE = 4;
	// light box lines: which of left, right, up, down each joins
	const BOX = { 0x2500: "lr", 0x2502: "ud", 0x250c: "rd", 0x2510: "ld", 0x2514: "ru", 0x2518: "lu", 0x251c: "udr",
		0x2524: "udl", 0x252c: "lrd", 0x2534: "lru", 0x253c: "lrud", 0x256d: "rd", 0x256e: "ld", 0x2570: "ru", 0x256f: "lu",
		0x2574: "l", 0x2575: "u", 0x2576: "r", 0x2577: "d" };

	const css = (rgb) => "#" + rgb.toString(16).padStart(6, "0");

	class Screen {
		constructor(container, cols, rows) {
			this.cols = cols;
			this.rows = rows;
			this.canvas = document.createElement("canvas");
			this.canvas.className = "play-canvas";
			container.appendChild(this.canvas);
			this.context = this.canvas.getContext("2d", { alpha: false });
			const size = cols * rows;
			this.chars = new Uint32Array(size);
			this.fgs = new Uint32Array(size);
			this.bgs = new Uint32Array(size);
			this.attrs = new Uint8Array(size);
			this.dirty = new Uint8Array(size);
			this.decoder = new TextDecoder("utf-8");
			this.cell = { w: 8, h: 16 };
			this.reset();
			this.frame = 0;
		}

		reset() {
			this.decoder = new TextDecoder("utf-8");
			this.x = 0;
			this.y = 0;
			this.wrapPending = false;
			this.fg = DEFAULT_FG;
			this.bg = DEFAULT_BG;
			this.attr = 0;
			this.state = 0;
			this.params = "";
			this.saved = { x: 0, y: 0 };
			this.erase(0, this.cols * this.rows);
		}

		// The cell size that fits the columns across width and the rows into height, in whole device pixels
		fit(width, height) {
			const ratio = window.devicePixelRatio || 1;
			const across = Math.floor((width * ratio) / this.cols);
			const down = Math.floor((height * ratio) / (this.rows * 2));
			const w = Math.max(3, Math.min(across, down, Math.round(11 * ratio)));
			if (w === this.cell.w && this.canvas.width === w * this.cols) { return; }
			this.cell = { w, h: w * 2 };
			this.canvas.width = w * this.cols;
			this.canvas.height = w * 2 * this.rows;
			this.canvas.style.width = this.canvas.width / ratio + "px";
			this.canvas.style.height = this.canvas.height / ratio + "px";
			this.dirty.fill(1);
			this.schedule();
		}

		erase(from, to) {
			for (let i = Math.max(0, from); i < Math.min(to, this.chars.length); i += 1) {
				this.chars[i] = 32;
				this.fgs[i] = this.fg;
				this.bgs[i] = this.bg;
				this.attrs[i] = 0;
				this.dirty[i] = 1;
			}
		}

		scrollUp() {
			const cols = this.cols;
			this.chars.copyWithin(0, cols);
			this.fgs.copyWithin(0, cols);
			this.bgs.copyWithin(0, cols);
			this.attrs.copyWithin(0, cols);
			this.dirty.fill(1);
			this.erase((this.rows - 1) * cols, this.rows * cols);
		}

		newline() {
			this.x = 0;
			this.wrapPending = false;
			if (this.y === this.rows - 1) { this.scrollUp(); } else { this.y += 1; }
		}

		put(code) {
			if (this.wrapPending) { this.newline(); }
			const i = this.y * this.cols + this.x;
			let fg = this.fg;
			let bg = this.bg;
			if (this.attr & INVERSE) { [fg, bg] = [bg, fg]; }
			if (this.chars[i] !== code || this.fgs[i] !== fg || this.bgs[i] !== bg || this.attrs[i] !== this.attr) {
				this.chars[i] = code;
				this.fgs[i] = fg;
				this.bgs[i] = bg;
				this.attrs[i] = this.attr;
				this.dirty[i] = 1;
			}
			if (this.x === this.cols - 1) { this.wrapPending = true; } else { this.x += 1; }
		}

		move(x, y) {
			this.x = Math.max(0, Math.min(this.cols - 1, x));
			this.y = Math.max(0, Math.min(this.rows - 1, y));
			this.wrapPending = false;
		}

		sgr(list) {
			const p = list.length ? list : [0];
			for (let i = 0; i < p.length; i += 1) {
				const n = p[i];
				if (n === 0) { this.fg = DEFAULT_FG; this.bg = DEFAULT_BG; this.attr = 0; }
				else if (n === 1) { this.attr |= BOLD; }
				else if (n === 2) { this.attr |= DIM; }
				else if (n === 22) { this.attr &= ~(BOLD | DIM); }
				else if (n === 7) { this.attr |= INVERSE; }
				else if (n === 27) { this.attr &= ~INVERSE; }
				else if (n >= 30 && n <= 37) { this.fg = BASIC[n - 30]; }
				else if (n >= 90 && n <= 97) { this.fg = BASIC[n - 82]; }
				else if (n >= 40 && n <= 47) { this.bg = BASIC[n - 40]; }
				else if (n >= 100 && n <= 107) { this.bg = BASIC[n - 92]; }
				else if (n === 39) { this.fg = DEFAULT_FG; }
				else if (n === 49) { this.bg = DEFAULT_BG; }
				else if (n === 38 || n === 48) {
					let color = null;
					if (p[i + 1] === 5) { color = PALETTE[p[i + 2] & 255]; i += 2; }
					else if (p[i + 1] === 2) { color = ((p[i + 2] & 255) << 16) | ((p[i + 3] & 255) << 8) | (p[i + 4] & 255); i += 4; }
					if (color !== null) { if (n === 38) { this.fg = color; } else { this.bg = color; } }
				}
			}
		}

		csi(final) {
			const isPrivate = this.params.startsWith("?");
			const list = (isPrivate ? this.params.slice(1) : this.params).split(";").filter((s) => s !== "").map(Number);
			const n = (d) => (list[0] === undefined || list[0] === 0 ? d : list[0]);
			const at = this.y * this.cols + this.x;
			switch (final) {
				case "m": this.sgr(list); break;
				case "H": case "f": this.move((list[1] || 1) - 1, (list[0] || 1) - 1); break;
				case "A": this.move(this.x, this.y - n(1)); break;
				case "B": this.move(this.x, this.y + n(1)); break;
				case "C": this.move(this.x + n(1), this.y); break;
				case "D": this.move(this.x - n(1), this.y); break;
				case "G": this.move(n(1) - 1, this.y); break;
				case "d": this.move(this.x, n(1) - 1); break;
				case "J": {
					const mode = list[0] || 0;
					if (mode === 0) { this.erase(at, this.cols * this.rows); }
					else if (mode === 1) { this.erase(0, at + 1); }
					else { this.erase(0, this.cols * this.rows); }
					break;
				}
				case "K": {
					const mode = list[0] || 0;
					const start = this.y * this.cols;
					if (mode === 0) { this.erase(at, start + this.cols); }
					else if (mode === 1) { this.erase(start, at + 1); }
					else { this.erase(start, start + this.cols); }
					break;
				}
				case "X": this.erase(at, at + n(1)); break;
				case "s": this.saved = { x: this.x, y: this.y }; break;
				case "u": this.move(this.saved.x, this.saved.y); break;
				case "h": case "l":
					if (isPrivate && (list.includes(1049) || list.includes(47) || list.includes(1047))) {
						this.erase(0, this.cols * this.rows);
						this.move(0, 0);
					}
					break;
				default: break;
			}
		}

		write(bytes) {
			const text = typeof bytes === "string" ? bytes : this.decoder.decode(bytes, { stream: true });
			for (const ch of text) {
				const code = ch.codePointAt(0);
				if (this.state === 0) {
					if (code === 27) { this.state = 1; }
					else if (code === 10) { this.newline(); }
					else if (code === 13) { this.x = 0; this.wrapPending = false; }
					else if (code === 8) { this.move(this.x - 1, this.y); }
					else if (code === 9) { this.move((Math.floor(this.x / 8) + 1) * 8, this.y); }
					else if (code >= 32) { this.put(code); }
				} else if (this.state === 1) {
					if (ch === "[") { this.state = 2; this.params = ""; }
					else if (ch === "]") { this.state = 3; }
					else if (ch === "7") { this.saved = { x: this.x, y: this.y }; this.state = 0; }
					else if (ch === "8") { this.move(this.saved.x, this.saved.y); this.state = 0; }
					else if (ch === "c") { this.reset(); }
					else { this.state = 0; }
				} else if (this.state === 2) {
					if (code >= 0x40 && code <= 0x7e) { this.csi(ch); this.state = 0; }
					else { this.params += ch; }
				} else if (this.state === 3) {
					// an OSC string runs to BEL or ESC \
					if (code === 7) { this.state = 0; } else if (code === 27) { this.state = 1; }
				}
			}
			this.schedule();
		}

		schedule() {
			if (!this.frame) { this.frame = requestAnimationFrame(() => { this.frame = 0; this.draw(); }); }
		}

		draw() {
			const g = this.context;
			const { w, h } = this.cell;
			const font = `${Math.round(h * 0.78)}px "JetBrains Mono", Menlo, Consolas, monospace`;
			g.textBaseline = "middle";
			g.textAlign = "center";
			for (let i = 0; i < this.chars.length; i += 1) {
				if (!this.dirty[i]) { continue; }
				this.dirty[i] = 0;
				const x = (i % this.cols) * w;
				const y = Math.floor(i / this.cols) * h;
				g.fillStyle = css(this.bgs[i]);
				g.fillRect(x, y, w, h);
				const code = this.chars[i];
				if (code === 32) { continue; }
				const attr = this.attrs[i];
				g.globalAlpha = attr & DIM ? 0.6 : 1;
				g.fillStyle = css(this.fgs[i]);
				if (!this.shape(g, code, x, y, w, h)) {
					const bold = attr & BOLD ? "bold " : "";
					if (g.font !== bold + font) { g.font = bold + font; }
					g.fillText(String.fromCodePoint(code), x + w / 2, y + h / 2 + 1);
				}
				g.globalAlpha = 1;
			}
		}

		// Block elements, quadrants, sextants, shades and light box lines in the current fill; false for anything else
		shape(g, code, x, y, w, h) {
			const half = Math.round(h / 2);
			const mid = Math.round(w / 2);
			if (code === 0x2588) { g.fillRect(x, y, w, h); return true; }
			if (code === 0x2580) { g.fillRect(x, y, w, half); return true; }
			if (code === 0x2584) { g.fillRect(x, y + half, w, h - half); return true; }
			if (code === 0x258c) { g.fillRect(x, y, mid, h); return true; }
			if (code === 0x2590) { g.fillRect(x + mid, y, w - mid, h); return true; }
			if (code >= 0x2581 && code <= 0x2587) {
				const part = Math.round((h * (code - 0x2580)) / 8);
				g.fillRect(x, y + h - part, w, part);
				return true;
			}
			if (code >= 0x2589 && code <= 0x258f) {
				g.fillRect(x, y, Math.round((w * (0x2590 - code)) / 8), h);
				return true;
			}
			if (code === 0x2594) { g.fillRect(x, y, w, Math.max(1, Math.round(h / 8))); return true; }
			if (code === 0x2595) { const part = Math.max(1, Math.round(w / 8)); g.fillRect(x + w - part, y, part, h); return true; }
			if (code >= 0x2591 && code <= 0x2593) {
				g.globalAlpha *= (code - 0x2590) / 4;
				g.fillRect(x, y, w, h);
				return true;
			}
			if (code >= 0x2596 && code <= 0x259f) {
				// quadrants: bits top-left 1, top-right 2, bottom-left 4, bottom-right 8
				const bits = [4, 8, 1, 13, 9, 7, 11, 2, 6, 14][code - 0x2596];
				return this.blocks(g, bits, 2, x, y, w, h);
			}
			if (code >= 0x1fb00 && code <= 0x1fb3b) {
				// sextants count the six-cell patterns from 1, skipping the two half blocks (21, 42)
				let pattern = code - 0x1fb00 + 1;
				if (pattern >= 21) { pattern += 1; }
				if (pattern >= 42) { pattern += 1; }
				return this.blocks(g, pattern, 3, x, y, w, h);
			}
			const box = BOX[code];
			if (box) {
				const t = Math.max(1, Math.round(w / 8));
				const cx = x + Math.floor((w - t) / 2);
				const cy = y + Math.floor((h - t) / 2);
				if (box.includes("l")) { g.fillRect(x, cy, cx - x + t, t); }
				if (box.includes("r")) { g.fillRect(cx, cy, x + w - cx, t); }
				if (box.includes("u")) { g.fillRect(cx, y, t, cy - y + t); }
				if (box.includes("d")) { g.fillRect(cx, cy, t, y + h - cy); }
				return true;
			}
			return false;
		}

		// A 2-wide grid of `rows` rows: bit 2r is the left cell of row r, bit 2r+1 the right
		blocks(g, bits, rows, x, y, w, h) {
			const mid = Math.round(w / 2);
			for (let r = 0; r < rows; r += 1) {
				const top = y + Math.round((h * r) / rows);
				const bottom = y + Math.round((h * (r + 1)) / rows);
				if (bits & (1 << (2 * r))) { g.fillRect(x, top, mid, bottom - top); }
				if (bits & (1 << (2 * r + 1))) { g.fillRect(x + mid, top, w - mid, bottom - top); }
			}
			return true;
		}
	}

	return { Screen };
})();
