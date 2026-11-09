#!/usr/bin/env python3
"""Render the demoreel to a video with its own soundtrack (an MP4 and a WebM).

    python docs/site_tools/render_reel_video.py [--runner=PATH] [--out=DIR] [--frames=DIR] [--seconds=N]
                                                [--cols=120] [--rows=40] [--every=2] [--width=W] [--crf=33] [--jobs=N]

1. The runner plays the whole reel headless (examples/demoreel/demoreel.jai's `video` mode) at a fixed 1/30 s step,
   writing every `--every`-th frame as 24-bit terminal text, the soundtrack's cues (one MIDI file a scene) and
   reel.txt. `--frames DIR` reuses such a dump instead of running the reel again.
2. Every frame goes through capture_examples.py's VT screen and renderer (8x16 cells, sextants drawn as geometry),
   across all cores, and the raw RGB streams into ffmpeg.
3. The soundtrack is the reel's own score: the cue files back to back in reel order (cue i starts where scene i
   starts and lasts as long), played by a small numpy synthesizer (each cue's drum kit, a filtered-saw bass, a
   detuned pad and a square arpeggio pumped by the score's sidechain expression, a vibrato lead, brass stabs, an
   orchestra hit, a reverse cymbal), with a few echo taps for room and a soft clip.
4. ffmpeg muxes the frames with the audio into an H.264 MP4 and a VP9 WebM, at the renderer's own size (960x640 for
   the default 120x40 cells) or scaled by nearest neighbour to --width, so the pixels stay crisp; the native size
   keeps the full reel near 25-30 MB, a 1280 px upscale roughly doubles it.

ffmpeg must be on PATH; numpy is the only Python dependency beyond the standard library.
"""

import argparse
import math
import multiprocessing
import os
import pathlib
import struct
import subprocess
import sys
import time
import wave

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
PACKAGE = HERE.parent.parent
REEL_DIR = PACKAGE / "examples" / "demoreel"
sys.path.insert(0, str(HERE))
import capture_examples as cap  # noqa: E402

RATE = 44100

# ----------------------------------------------------------------------------------------------- frames ---

_FONT = None
_SIZE = None


def _init_worker(cols, rows):
    global _FONT, _SIZE
    _FONT = cap.load_font(cap.FONT_PATH)
    _SIZE = (cols, rows)


def render_rgb(path):
    """One frame file as raw RGB bytes."""
    text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    frames = cap.frames_of(text, *_SIZE)
    canvas = cap.render_frame(frames[-1], _FONT, set())
    palette = np.array(canvas.palette, dtype=np.uint8)
    pixels = np.frombuffer(canvas.pixels.tobytes(), dtype=np.uint32)
    return palette[pixels].tobytes()


# ------------------------------------------------------------------------------------------------- MIDI ---

def read_vlq(data, i):
    value = 0
    while True:
        c = data[i]
        i += 1
        value = (value << 7) | (c & 127)
        if c < 128:
            return value, i


def parse_midi(path):
    """The cue's notes as (start s, length s, channel, note, velocity), its length in seconds, its drum kit (channel
    10's program) and each channel's expression changes as (time s, 0..1)."""
    data = pathlib.Path(path).read_bytes()
    division = struct.unpack(">H", data[12:14])[0]
    length = struct.unpack(">I", data[18:22])[0]
    i, end = 22, 22 + length
    tick = 0
    tempo = 500000
    volume = [100] * 16
    program = [0] * 16
    expression = {}
    open_notes = {}
    notes = []
    status = 0
    while i < end:
        delta, i = read_vlq(data, i)
        tick += delta
        if data[i] & 0x80:
            status = data[i]
            i += 1
        if status == 0xFF:
            kind = data[i]
            size, i = read_vlq(data, i + 1)
            if kind == 0x51:
                tempo = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2]
            i += size
            continue
        kind, ch = status & 0xF0, status & 0x0F
        if kind in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
            a, b = data[i], data[i + 1]
            i += 2
            if kind == 0xB0 and a == 7:
                volume[ch] = b
            elif kind == 0xB0 and a == 11:
                expression.setdefault(ch, []).append((tick, b / 127.0))
            elif kind == 0x90 and b > 0:
                open_notes.setdefault((ch, a), []).append((tick, b * volume[ch] / 127.0))
            elif kind == 0x80 or (kind == 0x90 and b == 0):
                stack = open_notes.get((ch, a))
                if stack:
                    start, vel = stack.pop(0)
                    notes.append((start, tick - start, ch, a, vel))
        else:
            if kind == 0xC0:
                program[ch] = data[i]
            i += 1
    seconds_per_tick = tempo / 1e6 / division
    out = [(s * seconds_per_tick, max(l * seconds_per_tick, 0.02), ch, n, v) for s, l, ch, n, v in notes]
    expr = {ch: [(t * seconds_per_tick, v) for t, v in ev] for ch, ev in expression.items()}
    return out, tick * seconds_per_tick, program[9], expr


# ------------------------------------------------------------------------------------------------ synth ---

def freq(note):
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


def additive(f, t, harmonics, odd=False, tilt=1.0):
    """A band-limited saw (or square) built from its first harmonics, softened by `tilt`."""
    out = np.zeros_like(t)
    for k in range(1, harmonics + 1):
        if odd and k % 2 == 0:
            continue
        if f * k > RATE * 0.45:
            break
        out += np.sin(2.0 * np.pi * f * k * t) / (k ** tilt)
    return out


def env_adsr(n, attack, decay, sustain, release, hold):
    t = np.arange(n) / RATE
    e = np.where(t < attack, t / max(attack, 1e-4), sustain + (1.0 - sustain) * np.exp(-(t - attack) / max(decay, 1e-4)))
    after = t - hold
    e = np.where(after > 0.0, e * np.exp(-after / max(release, 1e-4)), e)
    return e


RNG = np.random.default_rng(20260705)


def noise(n):
    return RNG.uniform(-1.0, 1.0, n)


def highpass(x, amount=0.9):
    y = np.empty_like(x)
    y[0] = x[0]
    y[1:] = x[1:] - amount * x[:-1]
    return y


KIT_POWER, KIT_ELECTRONIC, KIT_808 = 16, 24, 25


def voice(ch, note, vel, length, kit=0):
    """One note's samples (mono) and its channel's pan."""
    if ch == 9:
        if note in (35, 36):
            boom = kit == KIT_808 or note == 35
            n = int((0.7 if boom else 0.45) * RATE)
            t = np.arange(n) / RATE
            sweep = (42.0 if boom else 45.0) + (130.0 if boom else 110.0) * np.exp(-t / 0.035)
            phase = 2.0 * np.pi * np.cumsum(sweep) / RATE
            s = np.sin(phase) * np.exp(-t / (0.32 if boom else 0.16)) + 0.25 * noise(n) * np.exp(-t / 0.004)
            return np.tanh(s * 1.6) * vel * 1.2, 0.0
        if note in (38, 40):
            # the eighties snare: a crack, a tone, and a big room cut off by a gate
            n = int(0.34 * RATE)
            t = np.arange(n) / RATE
            gate = np.where(t < 0.2, 1.0, np.exp(-(t - 0.2) / 0.012))
            room = highpass(noise(n), 0.5) * (0.35 + 0.65 * np.exp(-t / 0.05)) * gate
            tone = 200.0 if kit == KIT_ELECTRONIC else 185.0
            body = np.sin(2 * np.pi * tone * t) * np.exp(-t / (0.09 if kit == KIT_ELECTRONIC else 0.05))
            s = room * (0.75 if kit == KIT_POWER else 0.55) + body * 0.5
            return s * vel * 0.85, 0.05
        if note == 39:
            # a clap: three quick bursts of filtered noise and a short tail
            n = int(0.25 * RATE)
            t = np.arange(n) / RATE
            bursts = sum(np.where((t >= d) & (t < d + 0.006), 1.0, 0.0) for d in (0.0, 0.011, 0.022))
            s = highpass(noise(n), 0.8) * (bursts * 0.8 + np.exp(-np.maximum(t - 0.022, 0.0) / 0.06) * (t >= 0.022))
            return s * vel * 0.55, -0.1
        if note in (41, 43, 45, 47, 48, 50):
            # toms: pitched decaying sines that bend down a little, lower notes deeper
            n = int(0.5 * RATE)
            t = np.arange(n) / RATE
            base = {41: 82.0, 43: 98.0, 45: 116.0, 47: 138.0, 48: 155.0, 50: 185.0}[note]
            sweep = base * (1.0 + 0.35 * np.exp(-t / 0.04))
            phase = 2.0 * np.pi * np.cumsum(sweep) / RATE
            s = np.sin(phase) * np.exp(-t / 0.22) + 0.15 * noise(n) * np.exp(-t / 0.01)
            return s * vel * 0.75, (note - 46) * 0.06
        if note in (49, 57):
            n = int(1.8 * RATE)
            t = np.arange(n) / RATE
            return highpass(noise(n), 0.95) * np.exp(-t / 0.55) * vel * 0.45, -0.25
        long_hat = note == 46
        n = int((0.3 if long_hat else 0.06) * RATE)
        t = np.arange(n) / RATE
        return highpass(noise(n), 0.97) * np.exp(-t / (0.09 if long_hat else 0.018)) * vel * 0.32, 0.3
    f = freq(note)
    hold = length
    if ch == 0:
        n = int((hold + 0.08) * RATE)
        t = np.arange(n) / RATE
        s = additive(f, t, 9, tilt=1.25) * env_adsr(n, 0.004, 0.18, 0.55, 0.05, hold)
        return s * vel * 0.55, 0.0
    if ch == 1:
        n = int((hold + 0.6) * RATE)
        t = np.arange(n) / RATE
        s = sum(additive(f * d, t, 10, tilt=1.4) for d in (1.0, 1.006, 0.994)) / 3.0
        return s * env_adsr(n, 0.35, 0.8, 0.8, 0.45, hold) * vel * 0.22, -0.2 if note % 2 else 0.2
    if ch == 2:
        n = int((hold + 0.18) * RATE)
        t = np.arange(n) / RATE
        s = additive(f, t, 9, odd=True, tilt=1.1) * np.exp(-t / 0.13)
        return s * vel * 0.22, 0.35
    if ch == 3:
        n = int((hold + 0.25) * RATE)
        t = np.arange(n) / RATE
        vib = 1.0 + 0.004 * np.sin(2 * np.pi * 5.5 * t) * np.minimum(t / 0.25, 1.0)
        phase_t = np.cumsum(vib) / RATE
        s = additive(f, phase_t, 10, tilt=1.2) * env_adsr(n, 0.02, 0.3, 0.7, 0.15, hold)
        return s * vel * 0.3, -0.3
    if ch == 5:
        # a reverse cymbal: noise swelling to the end of its note and stopping dead
        n = int(hold * RATE)
        t = np.arange(n) / RATE
        return highpass(noise(n), 0.9) * (t / max(hold, 1e-3)) ** 2.6 * vel * 0.5, -0.15
    if ch == 6:
        # an orchestra hit: a bright stack and a burst of noise, gone in a third of a second
        n = int(0.45 * RATE)
        t = np.arange(n) / RATE
        s = (additive(f, t, 14, tilt=0.9) + 0.6 * additive(f * 2.0, t, 8, tilt=1.0)) * np.exp(-t / 0.12)
        s += highpass(noise(n), 0.7) * np.exp(-t / 0.03) * 0.6
        return s * vel * 0.32, 0.0
    n = int((hold + 0.4) * RATE)
    t = np.arange(n) / RATE
    s = (additive(f, t, 4, tilt=1.0) + 0.4 * additive(f, t, 12, tilt=1.0) * np.exp(-t / 0.08)) * env_adsr(n, 0.01, 0.25, 0.35, 0.2, hold)
    return s * vel * 0.3, 0.1


def synthesize(cue_paths, seconds_limit=None):
    """The cues back to back as a stereo float array."""
    cues = [parse_midi(p) for p in cue_paths]
    total = sum(cue[1] for cue in cues)
    if seconds_limit is not None:
        total = min(total, seconds_limit)
    n_total = int(total * RATE) + RATE * 2
    left = np.zeros(n_total)
    right = np.zeros(n_total)
    # the channels the score's expression pumps get buses of their own, gained after the notes are laid down
    pumped = {1: (np.zeros(n_total), np.zeros(n_total)), 2: (np.zeros(n_total), np.zeros(n_total))}
    gain = {ch: np.ones(n_total) for ch in pumped}
    offset = 0.0
    cache = {}
    for notes, length, kit, expr in cues:
        if seconds_limit is not None and offset >= seconds_limit:
            break
        for start, dur, ch, note, vel in notes:
            at = int((offset + start) * RATE)
            if at >= n_total:
                continue
            key = (ch, note, round(vel * 32), round(dur * 50), kit if ch == 9 else 0)
            if key not in cache:
                cache[key] = voice(ch, note, vel, dur, kit)
            s, pan = cache[key]
            n = min(len(s), n_total - at)
            bus_l, bus_r = pumped.get(ch, (left, right))
            bus_l[at:at + n] += s[:n] * (1.0 - pan) * 0.5
            bus_r[at:at + n] += s[:n] * (1.0 + pan) * 0.5
        cue_end = min(int((offset + length) * RATE), n_total)
        for ch, events in expr.items():
            if ch not in gain:
                continue
            for k, (t, v) in enumerate(events):
                a = int((offset + t) * RATE)
                b = int((offset + events[k + 1][0]) * RATE) if k + 1 < len(events) else cue_end
                gain[ch][max(a, 0):min(b, n_total)] = v
        offset += length
    # a few milliseconds of smoothing so the pump never clicks
    window = np.ones(int(0.008 * RATE)) / int(0.008 * RATE)
    for ch, (bus_l, bus_r) in pumped.items():
        smooth = np.convolve(gain[ch], window, mode="same")
        left += bus_l * smooth
        right += bus_r * smooth
    # room: a few echo taps, darker and quieter as they go
    wet_l = np.zeros(n_total)
    wet_r = np.zeros(n_total)
    taps = [(0.023, 0.30), (0.037, 0.26), (0.053, 0.22), (0.071, 0.19), (0.097, 0.15), (0.131, 0.12), (0.173, 0.09),
            (0.229, 0.07), (0.311, 0.05)]
    for k, (delay, gain) in enumerate(taps):
        d = int(delay * RATE)
        src_l, src_r = (left, right) if k % 2 == 0 else (right, left)
        wet_l[d:] += src_l[:-d] * gain
        wet_r[d:] += src_r[:-d] * gain
    mix_l = left + 0.35 * wet_l
    mix_r = right + 0.35 * wet_r
    peak = max(np.percentile(np.abs(mix_l), 99.9), np.percentile(np.abs(mix_r), 99.9), 1e-6)
    drive = 1.6
    out = np.stack([np.tanh(mix_l / peak * drive), np.tanh(mix_r / peak * drive)], axis=1) / math.tanh(drive) * 0.89
    return out[:int(total * RATE)]


def write_wav(path, stereo):
    data = (np.clip(stereo, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())


# ------------------------------------------------------------------------------------------------- main ---

def read_meta(folder):
    meta = {}
    for line in (folder / "reel.txt").read_text(encoding="utf-8").splitlines():
        if " " in line:
            k, v = line.split(" ", 1)
            meta[k] = v
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runner", default=str(PACKAGE / "build" / "bin" / ("jaiscript.exe" if os.name == "nt" else "jaiscript")))
    ap.add_argument("--out", default=str(PACKAGE / "build" / "reel_video"), help="folder for the dump, the WAV and the videos")
    ap.add_argument("--frames", default=None, help="reuse an existing dump instead of running the reel")
    ap.add_argument("--seconds", type=float, default=None, help="render only the first N seconds")
    ap.add_argument("--cols", type=int, default=120)
    ap.add_argument("--rows", type=int, default=40)
    ap.add_argument("--every", type=int, default=2, help="keep every Nth frame of the 30 fps reel")
    ap.add_argument("--width", type=int, default=0, help="scale to this width by nearest neighbour (0: the renderer's own 8x16 cells)")
    ap.add_argument("--crf", type=int, default=33, help="the H.264 quality (lower is better and bigger)")
    ap.add_argument("--webm-crf", type=int, default=48, help="the VP9 quality")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--no-webm", action="store_true", help="skip the VP9 encode")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    began = time.time()
    if args.frames:
        dump = pathlib.Path(args.frames)
    else:
        dump = out / "frames"
        dump.mkdir(parents=True, exist_ok=True)
        cmd = [args.runner, "demoreel.jai", "--", "video", str(dump), "every", str(args.every), "w", str(args.cols),
               "h", str(args.rows), "pix", "sext", "color", "24"]
        print("reel:", " ".join(cmd), flush=True)
        subprocess.run(cmd, cwd=REEL_DIR, check=True)
    meta = read_meta(dump)
    fps = float(meta["fps"])
    cols, rows = int(meta["cols"]), int(meta["rows"])
    frame_paths = sorted(dump.glob("frame_*.txt"))
    if args.seconds is not None:
        frame_paths = frame_paths[:int(args.seconds * fps)]
    seconds = len(frame_paths) / fps
    print(f"frames: {len(frame_paths)} at {fps} fps ({seconds:.1f} s), dump took {time.time() - began:.0f} s", flush=True)

    t_audio = time.time()
    wav = out / "soundtrack.wav"
    write_wav(wav, synthesize(sorted(dump.glob("cue_*.mid")), seconds))
    print(f"audio: {wav} in {time.time() - t_audio:.0f} s", flush=True)

    width, height = cols * cap.CELL_W, rows * cap.CELL_H
    scale = f"scale={args.width}:-2:flags=neighbor" if args.width else "null"
    mp4 = out / "demoreel.mp4"
    webm = out / "demoreel.webm"
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{width}x{height}",
           "-r", str(fps), "-i", "pipe:0", "-i", str(wav),
           "-map", "0:v", "-map", "1:a", "-vf", scale, "-c:v", "libx264", "-preset", "slow", "-tune", "animation",
           "-crf", str(args.crf), "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "160k", "-movflags", "+faststart", "-shortest", str(mp4)]
    if not args.no_webm:
        cmd += ["-map", "0:v", "-map", "1:a", "-vf", scale, "-c:v", "libvpx-vp9", "-crf", str(args.webm_crf), "-b:v", "0", "-row-mt", "1",
                "-deadline", "good", "-cpu-used", "4", "-pix_fmt", "yuv420p", "-c:a", "libopus", "-b:a", "128k", "-shortest", str(webm)]
    t_video = time.time()
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    with multiprocessing.Pool(args.jobs, initializer=_init_worker, initargs=(cols, rows)) as pool:
        for k, rgb in enumerate(pool.imap(render_rgb, [str(p) for p in frame_paths], chunksize=4)):
            enc.stdin.write(rgb)
            if k % 300 == 0:
                print(f"  frame {k}/{len(frame_paths)}", flush=True)
    enc.stdin.close()
    if enc.wait() != 0:
        sys.exit("ffmpeg failed")
    print(f"video: {time.time() - t_video:.0f} s", flush=True)
    for f in (mp4, webm):
        if f.exists():
            print(f"  {f} {f.stat().st_size / 1e6:.1f} MB")
    print(f"total {time.time() - began:.0f} s")


if __name__ == "__main__":
    main()
