#!/usr/bin/env python3
"""Regenerate the screenshots on the examples page (site/examples/img/*.png).

    python site_tools/capture_examples.py [--runner PATH] [--only NAME ...] [--keep DIR]
    python site_tools/capture_examples.py --render CAPTURE OUT.png [--frame N] [--size CxR] [--split home|erase]

Every shot in SHOTS runs one example on the standalone runner (build/bin/jaiscript by default) in a headless capture
mode, at a fixed seed and frame, and turns the terminal output into a PNG into site/examples/img. No shot reads the
clock or the console size, so the same sources give the same images byte for byte. JaiDoom draws from its game data,
so run it once first to fetch the WADs.

The renderer is a small VT screen. Text, cursor moves, erases and SGR colours (24-bit, the xterm-256 cube, the 16
named colours, reverse video) land on a grid of 8x16-pixel cells. Block elements and sextants are drawn as geometry,
as terminals draw them: a half block is two 8x8 squares in its foreground and background colours, a quadrant or sextant
cell is split the same way, and a shade blends its two colours. Every other character comes from unscii-16, a public
domain 8x16 bitmap font by Viznut (fonts/unscii-16.hex holds the subset the examples use). A capture holding several
frames (a stream of redraws) is cut at each cursor-home, or, when every frame is a whole redraw, split at the sequence
that ends one (--split), and --frame picks one, counting from 0 (negative counts from the end).

The PNGs are written with zlib alone: indexed when a frame has at most 256 colours, RGB otherwise, with the smallest of
a few filter choices kept. --render turns any capture into a PNG, to choose frames when adding a shot.
"""

import argparse
import array
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
PACKAGE = HERE.parent.parent
EXAMPLES = PACKAGE / "examples"
OUT_DIR = PACKAGE / "docs" / "site" / "examples" / "img"
FONT_PATH = HERE / "fonts" / "unscii-16.hex"

CELL_W, CELL_H = 8, 16
DEFAULT_FG = (204, 204, 204)
DEFAULT_BG = (12, 12, 12)
BASE16 = [(12, 12, 12), (197, 15, 31), (19, 161, 14), (193, 156, 0), (0, 55, 218), (136, 23, 152), (58, 150, 221),
          (204, 204, 204), (118, 118, 118), (231, 72, 86), (22, 198, 12), (249, 241, 165), (59, 120, 255),
          (180, 0, 158), (97, 214, 214), (242, 242, 242)]

SEPARATORS = {"home": "\x1b[H", "erase": "\x1b[J"}


class Shot:
    """One image: the runner's arguments (a .jai path is relative to the example's folder, {out} is the run's scratch
    folder, which is also its working directory), the capture the run leaves (a file in the scratch folder, or "-"
    for stdout), the frame of it, the screen of a stream (None: a single frame's own size), the separator that ends
    each frame when frames are whole redraws (cut there instead of replaying the stream)."""

    def __init__(self, name, folder, args, capture, frame=0, size=None, split=None):
        self.name, self.folder, self.args, self.capture = name, folder, args, capture
        self.frame, self.size, self.split = frame, size, split


def gloom(name, tick):
    return Shot(name, "gloom", ["gloom.jai", "--", "--smoke", "--look", "showcase", "--truecolor", "--ticks", str(tick + 1),
                                "--dump-frame", str(tick)], "gloom_frame_shim.txt")


def demoreel(name, scene, at):
    return Shot(name, "demoreel", ["demoreel.jai", "--", "shot", str(scene), "at", str(at), "dump", "{out}/shot"],
                "shot.txt")


def jaidoom(name, capture, mobj, tics):
    return Shot(name, "jaidoom", ["scripts/shots.jai", "--", "freedoom1", "fight", str(mobj), "tics", str(tics),
                                  "out", "{out}"], capture)


CRAWLER = ["scripts/crawler.jai", "--", "screen", "135x37"]
GATORQUEST_TOUR = ["scripts/gatorquest.jai", "--", "tour", "screen", "150x44", "out", "{out}"]

SHOTS = [
    gloom("gloom-title", 5),
    gloom("gloom-fight", 252),
    gloom("gloom-tally", 3065),
    Shot("grubwell-title", "crawler", CRAWLER + ["input", "n"], "-", 0, (135, 37)),
    Shot("grubwell-fight", "crawler", CRAWLER + ["smoke", "seed", "6", "turns", "300", "anim", "dump", "{out}/run"],
         "run.ans", 629, (135, 37), "erase"),
    Shot("grubwell-welcome", "crawler", CRAWLER + ["smoke", "seed", "6", "turns", "2", "dump", "{out}/run"],
         "run.ans", 0, (135, 37), "erase"),
    Shot("grubwell-reload", "crawler", CRAWLER + ["smoke", "seed", "6", "turns", "62", "anim", "reload-every", "60", "dump",
                                                  "{out}/run"], "run.ans", 188, (135, 37), "erase"),
    Shot("grubwell-victory", "crawler", CRAWLER + ["smoke", "god", "seed", "5", "turns", "3000", "dump", "{out}/run"],
         "run_end.ans", -1, (135, 37), "erase"),
    demoreel("demoreel-chrome", 9, 11),
    demoreel("demoreel-eight", 1, 23.8),
    demoreel("demoreel-finale", 12, 24),
    Shot("gatorquest-title", "gatorquest", ["scripts/gatorquest.jai", "--", "shot", "title", "out", "{out}"],
         "title_main.ans"),
    Shot("gatorquest-town", "gatorquest", GATORQUEST_TOUR, "01_swampton_morning.ans"),
    Shot("gatorquest-night", "gatorquest", GATORQUEST_TOUR, "10_swampton_night.ans"),
    jaidoom("jaidoom-title", "title.ans", 105, 6),
    jaidoom("jaidoom-fight", "fight.ans", 105, 6),
    jaidoom("jaidoom-courtyard", "fight.ans", 29, 24),
]


def xterm256(n):
    if n < 16:
        return BASE16[n]
    if n < 232:
        n -= 16
        levels = (0, 95, 135, 175, 215, 255)
        return (levels[n // 36], levels[(n // 6) % 6], levels[n % 6])
    g = 8 + (n - 232) * 10
    return (g, g, g)


class Screen:
    """The part of a VT terminal the examples drive: a grid of (char, fg, bg) cells."""

    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.fg, self.bg, self.reverse = DEFAULT_FG, DEFAULT_BG, False
        self.x = self.y = 0
        self.wrap_pending = False
        self.saved = (0, 0)
        self.clear()

    def clear(self):
        self.cells = [[(" ", DEFAULT_FG, DEFAULT_BG)] * self.cols for _ in range(self.rows)]

    def colours(self):
        return (self.bg, self.fg) if self.reverse else (self.fg, self.bg)

    def blank(self):
        fg, bg = self.colours()
        return (" ", fg, bg)

    def line_feed(self):
        if self.y + 1 < self.rows:
            self.y += 1
        else:
            self.cells.pop(0)
            self.cells.append([self.blank()] * self.cols)

    def put(self, ch):
        if self.wrap_pending:
            self.x = 0
            self.line_feed()
            self.wrap_pending = False
        fg, bg = self.colours()
        self.cells[self.y][self.x] = (ch, fg, bg)
        if self.x + 1 < self.cols:
            self.x += 1
        else:
            self.wrap_pending = True

    def move(self, row, col):
        self.y = min(max(row, 0), self.rows - 1)
        self.x = min(max(col, 0), self.cols - 1)
        self.wrap_pending = False

    def erase_line(self, mode):
        row = self.cells[self.y]
        lo, hi = {0: (self.x, self.cols), 1: (0, self.x + 1)}.get(mode, (0, self.cols))
        for x in range(lo, hi):
            row[x] = self.blank()

    def erase_display(self, mode):
        if mode in (2, 3):
            for y in range(self.rows):
                self.cells[y] = [self.blank()] * self.cols
            return
        self.erase_line(mode)
        ys = range(self.y + 1, self.rows) if mode == 0 else range(0, self.y)
        for y in ys:
            self.cells[y] = [self.blank()] * self.cols

    def sgr(self, params):
        ps = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else [0]
        k = 0
        while k < len(ps):
            p = ps[k]
            if p == 0:
                self.fg, self.bg, self.reverse = DEFAULT_FG, DEFAULT_BG, False
            elif p in (38, 48) and k + 1 < len(ps) and ps[k + 1] == 2 and k + 4 < len(ps):
                col = tuple(ps[k + 2:k + 5])
                k += 4
                self.fg, self.bg = (col, self.bg) if p == 38 else (self.fg, col)
            elif p in (38, 48) and k + 2 < len(ps) and ps[k + 1] == 5:
                col = xterm256(ps[k + 2])
                k += 2
                self.fg, self.bg = (col, self.bg) if p == 38 else (self.fg, col)
            elif 30 <= p <= 37:
                self.fg = BASE16[p - 30]
            elif 90 <= p <= 97:
                self.fg = BASE16[p - 90 + 8]
            elif 40 <= p <= 47:
                self.bg = BASE16[p - 40]
            elif 100 <= p <= 107:
                self.bg = BASE16[p - 100 + 8]
            elif p == 39:
                self.fg = DEFAULT_FG
            elif p == 49:
                self.bg = DEFAULT_BG
            elif p == 7:
                self.reverse = True
            elif p == 27:
                self.reverse = False
            k += 1

    def csi(self, params, final):
        private = params.startswith("?")
        nums = [int(p) if p.isdigit() else 0 for p in params.lstrip("?").split(";")] if params.lstrip("?") else []
        first = nums[0] if nums else 0
        if final == "m":
            self.sgr(params)
        elif private:
            if final in "hl" and first in (47, 1047, 1049):
                self.erase_display(2)
        elif final in "Hf":
            self.move((nums[0] if len(nums) > 0 and nums[0] else 1) - 1, (nums[1] if len(nums) > 1 and nums[1] else 1) - 1)
        elif final == "J":
            self.erase_display(first)
        elif final == "K":
            self.erase_line(first)
        elif final == "A":
            self.move(self.y - max(first, 1), self.x)
        elif final == "B":
            self.move(self.y + max(first, 1), self.x)
        elif final == "C":
            self.move(self.y, self.x + max(first, 1))
        elif final == "D":
            self.move(self.y, self.x - max(first, 1))
        elif final == "G":
            self.move(self.y, max(first, 1) - 1)
        elif final == "d":
            self.move(max(first, 1) - 1, self.x)
        elif final == "s":
            self.saved = (self.y, self.x)
        elif final == "u":
            self.move(*self.saved)

    def snapshot(self):
        return [list(row) for row in self.cells]


TOKEN = re.compile(r"\x1b\[([0-9;?]*)([@-~])|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?|\x1b(.)|([\n\r\b])"
                   r"|([^\x00-\x1f]+)|[\x00-\x1f]", re.S)


def frames_of(text, cols, rows, want=None):
    """The frames of a capture: the screen before each cursor-home that follows drawing, and the screen at the end.
    With want (a frame number from 0), stop once that frame is complete and return the frames so far."""
    screen = Screen(cols, rows)
    frames = []
    drawn = False
    for m in TOKEN.finditer(text):
        run = m.group(5)
        if run is not None:
            for ch in run:
                screen.put(ch)
            drawn = True
            continue
        final = m.group(2)
        if final is not None:
            params = m.group(1)
            if final in "Hf" and params in ("", "1;1", "1") and drawn:
                frames.append(screen.snapshot())
                drawn = False
                if want is not None and len(frames) > want:
                    return frames
            screen.csi(params, final)
            continue
        esc = m.group(3)
        if esc == "7":
            screen.saved = (screen.y, screen.x)
        elif esc == "8":
            screen.move(*screen.saved)
        control = m.group(4)
        if control == "\n":
            screen.x = 0
            screen.wrap_pending = False
            screen.line_feed()
        elif control == "\r":
            screen.x = 0
            screen.wrap_pending = False
        elif control == "\b":
            screen.move(screen.y, screen.x - 1)
    if drawn or not frames:
        frames.append(screen.snapshot())
    return frames


def plain_size(text):
    """The cols x rows a single frame of newline-separated rows needs."""
    rows = text.rstrip("\n").split("\n")
    width = 0
    for row in rows:
        visible, i = 0, 0
        while i < len(row):
            if row[i] == "\x1b" and i + 1 < len(row) and row[i + 1] == "[":
                j = i + 2
                while j < len(row) and not ("@" <= row[j] <= "~"):
                    j += 1
                i = j + 1
                continue
            if row[i] >= " ":
                visible += 1
            i += 1
        width = max(width, visible)
    return max(width, 1), max(len(rows), 1)


def load_font(path):
    font = {}
    for line in path.read_text(encoding="ascii").splitlines():
        if not line or line.startswith("#"):
            continue
        code, bits = line.split(":")
        if len(bits) == 32:
            font[int(code, 16)] = [int(bits[r * 2:r * 2 + 2], 16) for r in range(16)]
    return font


QUADRANTS = {0x2598: 1, 0x259D: 2, 0x2580: 3, 0x2596: 4, 0x258C: 5, 0x259E: 6, 0x259B: 7, 0x2597: 8, 0x259A: 9,
             0x2590: 10, 0x259C: 11, 0x2584: 12, 0x2599: 13, 0x259F: 14, 0x2588: 15}
SHADES = {0x2591: 0.25, 0x2592: 0.5, 0x2593: 0.75}


def sextant_bits(cp):
    """U+1FB00..1FB3B are the 2x3 sextants minus the four that already exist as blocks (left, right, empty, full)."""
    if 0x1FB00 <= cp <= 0x1FB3B:
        bits = cp - 0x1FB00 + 1
        if bits >= 21:
            bits += 1
        if bits >= 42:
            bits += 1
        return bits
    return None


class Canvas:
    """An image as one palette index per pixel; the palette grows as colours are painted."""

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.palette = []
        self.index = {}
        self.pixels = array.array("I", bytes(4 * width * height))

    def colour(self, rgb):
        found = self.index.get(rgb)
        if found is None:
            found = self.index[rgb] = len(self.palette)
            self.palette.append(rgb)
        return found

    def fill(self, x0, y0, w, h, rgb):
        c = self.colour(rgb)
        run = array.array("I", [c]) * w
        for y in range(y0, y0 + h):
            at = y * self.width + x0
            self.pixels[at:at + w] = run


def render_frame(cells, font, missing):
    rows = len(cells)
    cols = max(len(r) for r in cells)
    canvas = Canvas(cols * CELL_W, rows * CELL_H)
    for cy, row in enumerate(cells):
        for cx, (ch, fg, bg) in enumerate(row):
            x0, y0 = cx * CELL_W, cy * CELL_H
            cp = ord(ch)
            canvas.fill(x0, y0, CELL_W, CELL_H, bg)
            if cp == 32:
                continue
            if cp in QUADRANTS:
                bits = QUADRANTS[cp]
                for q in range(4):
                    if bits & (1 << q):
                        canvas.fill(x0 + (q % 2) * CELL_W // 2, y0 + (q // 2) * CELL_H // 2, CELL_W // 2, CELL_H // 2, fg)
                continue
            bits = sextant_bits(cp)
            if bits is not None:
                tops = (0, 5, 11, 16)
                for q in range(6):
                    if bits & (1 << q):
                        top, bottom = tops[q // 2], tops[q // 2 + 1]
                        canvas.fill(x0 + (q % 2) * CELL_W // 2, y0 + top, CELL_W // 2, bottom - top, fg)
                continue
            if 0x2581 <= cp <= 0x2587:
                h = (cp - 0x2580) * CELL_H // 8
                canvas.fill(x0, y0 + CELL_H - h, CELL_W, h, fg)
                continue
            if 0x2589 <= cp <= 0x258F:
                canvas.fill(x0, y0, (0x2590 - cp) * CELL_W // 8, CELL_H, fg)
                continue
            if cp == 0x2594:
                canvas.fill(x0, y0, CELL_W, CELL_H // 8, fg)
                continue
            if cp == 0x2595:
                canvas.fill(x0 + CELL_W - CELL_W // 8, y0, CELL_W // 8, CELL_H, fg)
                continue
            if cp in SHADES:
                a = SHADES[cp]
                canvas.fill(x0, y0, CELL_W, CELL_H, tuple(round(bg[i] + (fg[i] - bg[i]) * a) for i in range(3)))
                continue
            glyph = font.get(cp)
            if glyph is None:
                missing.add(cp)
                glyph = [0xFF] + [0x81] * 14 + [0xFF]
            ink = canvas.colour(fg)
            for gy, bits in enumerate(glyph):
                if bits:
                    at = (y0 + gy) * canvas.width + x0
                    for gx in range(8):
                        if bits & (0x80 >> gx):
                            canvas.pixels[at + gx] = ink
    return canvas


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def filtered(raw_rows, bpp, mode):
    out = bytearray()
    prev = bytes(len(raw_rows[0]))
    for row in raw_rows:
        if mode == 0:
            out += b"\x00" + row
        elif mode == 1:
            out += b"\x01" + row[:bpp] + bytes((row[i] - row[i - bpp]) & 255 for i in range(bpp, len(row)))
        else:
            out += b"\x02" + bytes((row[i] - prev[i]) & 255 for i in range(len(row)))
        prev = row
    return bytes(out)


def png_bytes(canvas):
    w, h = canvas.width, canvas.height
    px = canvas.pixels
    if len(canvas.palette) <= 256:
        rows = [array.array("B", px[y * w:(y + 1) * w]).tobytes() for y in range(h)]
        header = struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)
        extra = png_chunk(b"PLTE", b"".join(bytes(c) for c in canvas.palette))
        bpp, modes = 1, (0,)
    else:
        rgb = [bytes(c) for c in canvas.palette]
        rows = [b"".join(rgb[i] for i in px[y * w:(y + 1) * w]) for y in range(h)]
        header = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        extra = b""
        bpp, modes = 3, (0, 1, 2)
    best = None
    for mode in modes:
        data = zlib.compress(filtered(rows, bpp, mode), 9)
        if best is None or len(data) < len(best):
            best = data
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + extra + png_chunk(b"IDAT", best) + png_chunk(b"IEND", b"")


def render_capture(text, frame, size, font, split=None):
    """The PNG canvas of one frame of a capture, how many frames the capture holds, and the code points it lacked."""
    if split:
        separator = SEPARATORS[split]
        chunks = [c for c in text.split(separator) if c.strip()]
        count = len(chunks)
        text = separator + chunks[frame] if split == "home" else chunks[frame] + separator
        frame = 0
        frames = frames_of(text, *(size or plain_size(text)), want=0)
    else:
        frames = frames_of(text, *(size or plain_size(text)), want=frame if frame >= 0 else None)
        count = len(frames)
    missing = set()
    return render_frame(frames[frame], font, missing), count, missing


def default_runner():
    for name in ("jaiscript.exe", "jaiscript"):
        candidate = PACKAGE / "build" / "bin" / name
        if candidate.exists():
            return candidate
    return PACKAGE / "build" / "bin" / "jaiscript"


def missing_note(missing):
    return f"  missing glyphs: {' '.join(f'U+{c:04X}' for c in sorted(missing))}" if missing else ""


def run_shot(shot, runner, font, keep):
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="capture_" + shot.name + "_"))
    try:
        argv = [str(runner)]
        for a in shot.args:
            argv.append(str(EXAMPLES / shot.folder / a) if a.endswith(".jai") else a.replace("{out}", str(scratch)))
        run = subprocess.run(argv, cwd=scratch, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=900)
        if shot.capture == "-":
            text = run.stdout.decode("utf-8", errors="replace")
        else:
            path = scratch / shot.capture
            if not path.exists():
                sys.stderr.write(run.stdout.decode("utf-8", errors="replace")[-2000:])
                sys.stderr.write(run.stderr.decode("utf-8", errors="replace")[-2000:])
                raise SystemExit(f"{shot.name}: the run left no {shot.capture} (exit {run.returncode})")
            text = path.read_text(encoding="utf-8", errors="replace")
        if keep:
            keep.mkdir(parents=True, exist_ok=True)
            (keep / (shot.name + ".ans")).write_text(text, encoding="utf-8")
        canvas, count, missing = render_capture(text, shot.frame, shot.size, font, shot.split)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    data = png_bytes(canvas)
    (OUT_DIR / (shot.name + ".png")).write_bytes(data)
    print(f"{shot.name}.png  {canvas.width}x{canvas.height}  {len(canvas.palette)} colours  {len(data) // 1024} KB"
          f"  (frame {shot.frame} of {count}){missing_note(missing)}")


def parse_size(text):
    cols, rows = text.lower().split("x")
    return int(cols), int(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--runner", type=pathlib.Path, default=None, help="the jaiscript runner (default build/bin)")
    ap.add_argument("--only", nargs="*", default=None, help="shot names to regenerate (default all)")
    ap.add_argument("--keep", type=pathlib.Path, default=None, help="also save each raw capture here")
    ap.add_argument("--render", nargs=2, metavar=("CAPTURE", "OUT"), help="render one capture file to a PNG")
    ap.add_argument("--frame", type=int, default=-1, help="with --render: the frame of a stream (default the last)")
    ap.add_argument("--size", type=parse_size, default=None, help="with --render: the screen of a stream, CxR")
    ap.add_argument("--split", choices=sorted(SEPARATORS), default=None,
                    help="with --render: frames are whole redraws ending at this sequence")
    opts = ap.parse_args()
    font = load_font(FONT_PATH)
    if opts.render:
        text = pathlib.Path(opts.render[0]).read_text(encoding="utf-8", errors="replace")
        canvas, count, missing = render_capture(text, opts.frame, opts.size, font, opts.split)
        data = png_bytes(canvas)
        pathlib.Path(opts.render[1]).write_bytes(data)
        print(f"{opts.render[1]}: {canvas.width}x{canvas.height}, {count} frames, {len(data) // 1024} KB"
              f"{missing_note(missing)}")
        return 0
    runner = opts.runner or default_runner()
    if not runner.exists():
        raise SystemExit(f"no runner at {runner}: build jaiscript_runner or pass --runner")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for shot in SHOTS:
        if opts.only is None or shot.name in opts.only:
            run_shot(shot, runner, font, opts.keep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
