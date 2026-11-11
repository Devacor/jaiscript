// Plays what a script's Sound does, with Web Audio: WAV and MP3 through the browser's decoder, MIDI through a General
// MIDI synth voiced for synthwave (filtered saw basses, supersaw pads, plucked and singing leads, FM bells, drum machines
// on channel 10). Each MIDI channel has a strip that follows its controllers: expression (11), brightness (74, a lowpass),
// pan (10) and reverb send (91). The MIDI mix shares one reverb, a compressor and a soft clip. One master gain carries the
// page's volume and mute, remembered between visits.
"use strict";

const JaiAudio = (() => {
	const OP = { load: 0, play: 1, stop: 2, volume: 3, release: 4, stopAll: 5 };
	const FORMAT = { wav: 1, midi: 2, mp3: 3 };
	const LOOKAHEAD = 0.6;
	const STORE = "jai-play-audio";
	const PAN = 10;
	const EXPRESSION = 11;
	const BRIGHTNESS = 74;
	const REVERB = 91;
	const STRIP_CONTROLS = [PAN, EXPRESSION, BRIGHTNESS, REVERB];

	let context = null;
	let master = null;
	let mix = null;
	let reverbIn = null;
	const settings = { volume: 0.7, muted: false };
	try { Object.assign(settings, JSON.parse(localStorage.getItem(STORE) || "{}")); } catch (error) { /* defaults */ }
	const sounds = new Map();
	let noise = null;
	let driveCurve = null;

	function save() {
		try { localStorage.setItem(STORE, JSON.stringify(settings)); } catch (error) { /* not remembered */ }
	}

	function applyMaster() {
		if (master) { master.gain.setTargetAtTime(settings.muted ? 0 : settings.volume * settings.volume, context.currentTime, 0.02); }
	}

	function tanhCurve(drive) {
		const curve = new Float32Array(1025);
		for (let i = 0; i < curve.length; i += 1) { curve[i] = Math.tanh(drive * (i / 512 - 1)) / Math.tanh(drive); }
		return curve;
	}

	// A plate's impulse response: noise that decays over `seconds` and darkens as it goes
	function plate(seconds) {
		const rate = context.sampleRate;
		const length = Math.floor(seconds * rate);
		const buffer = context.createBuffer(2, length, rate);
		const predelay = Math.floor(0.018 * rate);
		for (let channel = 0; channel < 2; channel += 1) {
			const data = buffer.getChannelData(channel);
			let held = 0;
			for (let i = predelay; i < length; i += 1) {
				const t = (i - predelay) / (length - predelay);
				held += (Math.random() * 2 - 1 - held) * (0.75 - 0.6 * t);
				data[i] = held * Math.exp(-6.2 * t);
			}
		}
		return buffer;
	}

	// MIDI sounds meet in `mix`: a compressor glues them and a soft clip catches the peaks. The reverb belongs to the
	// context, so a sound's tail rings on after the sound stops.
	function buildMix() {
		const clip = context.createWaveShaper();
		clip.curve = tanhCurve(1.5);
		clip.oversample = "2x";
		const glue = context.createDynamicsCompressor();
		glue.threshold.value = -16;
		glue.knee.value = 8;
		glue.ratio.value = 4;
		glue.attack.value = 0.004;
		glue.release.value = 0.16;
		mix = context.createGain();
		mix.connect(glue);
		glue.connect(clip);
		clip.connect(master);
		const reverb = context.createConvolver();
		reverb.buffer = plate(2.4);
		const reverbOut = context.createGain();
		reverbOut.gain.value = 0.5;
		reverbIn = context.createGain();
		reverbIn.connect(reverb);
		reverb.connect(reverbOut);
		reverbOut.connect(mix);
	}

	// Called from a click: browsers start audio only after the page's user acts
	function unlock() {
		if (!context) {
			const Context = window.AudioContext || window.webkitAudioContext;
			if (!Context) { return; }
			context = new Context();
			master = context.createGain();
			master.connect(context.destination);
			buildMix();
			applyMaster();
		}
		if (context.state === "suspended") { context.resume(); }
	}

	function noiseBuffer() {
		if (!noise) {
			noise = context.createBuffer(1, context.sampleRate * 2, context.sampleRate);
			const data = noise.getChannelData(0);
			for (let i = 0; i < data.length; i += 1) { data[i] = Math.random() * 2 - 1; }
		}
		return noise;
	}

	// ------------------------------------------------------------------------------------------------ MIDI file

	function parseMidi(bytes) {
		const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		let at = 0;
		const tag = () => String.fromCharCode(bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]);
		if (tag() === "RIFF") { at = 20; }
		if (tag() !== "MThd") { return null; }
		const headerLength = view.getUint32(at + 4);
		const division = view.getUint16(at + 12);
		at += 8 + headerLength;
		const events = [];
		while (at + 8 <= bytes.length) {
			const kind = tag();
			const length = view.getUint32(at + 4);
			let p = at + 8;
			const end = Math.min(bytes.length, p + length);
			at = end;
			if (kind !== "MTrk") { continue; }
			let tick = 0;
			let status = 0;
			const varint = () => {
				let value = 0;
				for (let i = 0; i < 4 && p < end; i += 1) {
					const b = bytes[p++];
					value = (value << 7) | (b & 0x7f);
					if (!(b & 0x80)) { break; }
				}
				return value;
			};
			while (p < end) {
				tick += varint();
				let b = bytes[p];
				if (b & 0x80) { status = b; p += 1; } else { b = status; }
				const type = status & 0xf0;
				if (status === 0xff) {
					const meta = bytes[p++];
					const size = varint();
					if (meta === 0x51 && size === 3) { events.push({ tick, tempo: (bytes[p] << 16) | (bytes[p + 1] << 8) | bytes[p + 2] }); }
					p += size;
				} else if (status === 0xf0 || status === 0xf7) {
					p += varint();
				} else if (type === 0xc0 || type === 0xd0) {
					events.push({ tick, type, channel: status & 15, a: bytes[p++] });
				} else {
					events.push({ tick, type, channel: status & 15, a: bytes[p], b: bytes[p + 1] });
					p += 2;
				}
			}
		}
		events.sort((x, y) => x.tick - y.tick);
		// ticks to seconds through the tempo map, then notes paired on and off
		let tempo = 500000;
		let lastTick = 0;
		let seconds = 0;
		const timeline = [];
		const open = new Map();
		let lastNote = 0;
		const program = new Array(16).fill(0);
		const volume = new Array(16).fill(1);
		for (const event of events) {
			seconds += ((event.tick - lastTick) * tempo) / (division * 1e6);
			lastTick = event.tick;
			if (event.tempo) { tempo = event.tempo; continue; }
			const key = event.channel * 128 + event.a;
			if (event.type === 0xc0) { program[event.channel] = event.a; }
			else if (event.type === 0xb0 && event.a === 7) { volume[event.channel] = event.b / 127; }
			else if (event.type === 0xb0 && STRIP_CONTROLS.includes(event.a)) {
				timeline.push({ time: seconds, channel: event.channel, control: event.a, value: event.b / 127 });
			} else if (event.type === 0x90 && event.b > 0) {
				const note = { time: seconds, length: 0.25, channel: event.channel, note: event.a,
					velocity: (event.b / 127) * volume[event.channel], program: program[event.channel] };
				timeline.push(note);
				lastNote = seconds;
				if (!open.has(key)) { open.set(key, []); }
				open.get(key).push(note);
			} else if (event.type === 0x80 || (event.type === 0x90 && event.b === 0)) {
				const pending = open.get(key);
				if (pending && pending.length) {
					const note = pending.shift();
					note.length = Math.max(0.02, seconds - note.time);
				}
			}
		}
		return { timeline, length: Math.max(seconds, timeline.length ? lastNote + 0.25 : 0) };
	}

	// ------------------------------------------------------------------------------------------------ synth

	const frequency = (note) => 440 * Math.pow(2, (note - 69) / 12);

	// A level that rises to `peak` over `attack`, settles toward peak * sustain while the note is held and falls away over
	// `release` once it ends. Returns the time the voice is silent.
	function contour(param, time, length, peak, attack, decay, sustain, release) {
		const top = time + attack;
		const off = Math.max(top, time + length);
		const rest = peak * sustain;
		const settle = decay / 3;
		param.setValueAtTime(0.0001, time);
		param.linearRampToValueAtTime(peak, top);
		param.setTargetAtTime(rest, top, settle);
		param.setValueAtTime(Math.max(0.0001, rest + (peak - rest) * Math.exp(-(off - top) / settle)), off);
		param.exponentialRampToValueAtTime(0.0001, off + release);
		return off + release;
	}

	function envelope(gain, time, peak, attack, hold, release) {
		gain.gain.setValueAtTime(0.0001, time);
		gain.gain.linearRampToValueAtTime(peak, time + attack);
		gain.gain.setValueAtTime(peak, time + Math.max(attack, hold));
		gain.gain.exponentialRampToValueAtTime(0.0001, time + Math.max(attack, hold) + release);
		return time + Math.max(attack, hold) + release;
	}

	function oscillator(target, time, end, type, hz, cents) {
		const osc = context.createOscillator();
		osc.type = type;
		osc.frequency.value = hz;
		osc.detune.value = cents || 0;
		osc.connect(target);
		osc.start(time);
		osc.stop(end + 0.05);
		return osc;
	}

	// A voice's own lowpass and loudness, wired to `out`; its oscillators feed the filter
	function voice(out, cutoff, resonance) {
		const filter = context.createBiquadFilter();
		filter.type = "lowpass";
		filter.frequency.value = cutoff;
		filter.Q.value = resonance;
		const gain = context.createGain();
		gain.gain.value = 0;
		filter.connect(gain);
		gain.connect(out);
		return { filter, gain };
	}

	// A place across the stereo field picked by the pitch, so a chord's notes and an arpeggio's steps fan out
	function placed(out, note, width) {
		const panner = context.createStereoPanner();
		panner.pan.value = (((note * 7) % 12) / 5.5 - 1) * width;
		panner.connect(out);
		return panner;
	}

	function vibrato(oscillators, time, end, cents) {
		const lfo = context.createOscillator();
		lfo.frequency.value = 5.6;
		const depth = context.createGain();
		depth.gain.value = 0;
		depth.gain.setValueAtTime(0, time + 0.22);
		depth.gain.linearRampToValueAtTime(cents, time + 0.5);
		lfo.connect(depth);
		for (const osc of oscillators) { depth.connect(osc.detune); }
		lfo.start(time);
		lfo.stop(end + 0.05);
	}

	// Synth bass: two saws (a saw and a square for synth bass 2) through a filter that snaps shut, over a sine the
	// filter never touches
	function bass(out, time, note, velocity, length, program) {
		const hz = frequency(note);
		const { filter, gain } = voice(out, hz * 3, 7);
		const end = contour(gain.gain, time, length, 0.17 * velocity, 0.004, 0.3, 0.65, 0.05);
		filter.frequency.setValueAtTime(Math.min(8000, hz * 14 * (0.5 + velocity)), time);
		filter.frequency.setTargetAtTime(hz * 2.5, time, 0.06);
		oscillator(filter, time, end, "sawtooth", hz, -9);
		oscillator(filter, time, end, program === 39 ? "square" : "sawtooth", hz, 9);
		const sub = context.createGain();
		sub.gain.value = 0.5;
		sub.connect(gain);
		oscillator(sub, time, end, "sine", hz);
	}

	const PADS = {
		89: { attack: 0.2, cutoff: 1900, resonance: 1, release: 0.7 },
		90: { attack: 0.03, cutoff: 4200, resonance: 1, release: 0.3 },
		91: { attack: 0.25, cutoff: 1300, resonance: 6, release: 0.7 },
		94: { attack: 0.3, cutoff: 3600, resonance: 1, release: 0.9, shimmer: true },
		95: { attack: 0.5, cutoff: 700, resonance: 4, release: 0.8, sweep: 5200 }
	};
	const STRINGS = { attack: 0.14, cutoff: 3000, resonance: 1, release: 0.5 };

	// Three saws spread a few cents apart: warm (89), polysynth (90), choir (91), halo (94, an octave of shimmer on top),
	// sweep (95, the filter opens over the note), and the string families
	function pad(out, time, note, velocity, length, program) {
		const hz = frequency(note);
		const recipe = PADS[program] || STRINGS;
		const { filter, gain } = voice(placed(out, note, 0.8), recipe.cutoff, recipe.resonance);
		const end = contour(gain.gain, time, length, 0.12 * velocity, recipe.attack, 2, 0.8, recipe.release);
		if (recipe.sweep) {
			filter.frequency.setValueAtTime(recipe.cutoff, time);
			filter.frequency.exponentialRampToValueAtTime(recipe.sweep, time + Math.max(recipe.attack, length));
		}
		for (const cents of [-12, 0, 12]) { oscillator(filter, time, end, "sawtooth", hz, cents); }
		if (recipe.shimmer) {
			const high = context.createGain();
			high.gain.value = 0.35;
			high.connect(filter);
			oscillator(high, time, end, "sawtooth", hz * 2, 7);
		}
	}

	const LEADS = {
		80: { waves: ["square", "square"] },
		81: { waves: ["sawtooth", "sawtooth"] },
		82: { waves: ["triangle", "sine"], interval: 12 },
		83: { waves: ["triangle", "square"] },
		84: { waves: ["sawtooth", "square"], driven: true },
		85: { waves: ["triangle", "sawtooth"] },
		86: { waves: ["sawtooth", "sawtooth"], interval: 7 },
		87: { waves: ["sawtooth", "square"], interval: -12 }
	};
	const PLUCKED = { waves: ["triangle", "sawtooth"], plucked: true };
	const OVERDRIVEN = { waves: ["sawtooth", "square"], driven: true };
	const LEAD_FAMILIES = {
		0: { waves: ["triangle", "sine"], plucked: true },
		2: { waves: ["square", "sine"], interval: 12 },
		3: PLUCKED,
		8: { waves: ["square", "sawtooth"] },
		9: { waves: ["triangle", "sine"] },
		13: PLUCKED
	};

	// Two oscillators a few cents apart. A short note is a pluck (the filter and the level snap down); a long one sings,
	// and its vibrato comes in late. A driven recipe goes through a waveshaper first.
	function lead(out, time, note, velocity, length, recipe) {
		const hz = frequency(note);
		const plucked = recipe.plucked || length < 0.2;
		const { filter, gain } = voice(plucked ? placed(out, note, 0.55) : out, hz * 3, plucked ? 5 : 2);
		const peak = (recipe.driven ? 0.16 : 0.13) * velocity;
		const end = plucked
			? contour(gain.gain, time, length, peak, 0.003, 0.22, 0.2, 0.14)
			: contour(gain.gain, time, length, peak, 0.012, 0.5, 0.75, 0.16);
		const open = Math.min(11000, hz * (plucked ? 12 : 9));
		filter.frequency.setValueAtTime(open, time);
		filter.frequency.setTargetAtTime(Math.min(open, hz * (plucked ? 2.5 : 4.5)), time, plucked ? 0.05 : 0.25);
		let input = filter;
		if (recipe.driven) {
			if (!driveCurve) { driveCurve = tanhCurve(5); }
			const shaper = context.createWaveShaper();
			shaper.curve = driveCurve;
			shaper.connect(filter);
			input = context.createGain();
			input.gain.value = 0.5;
			input.connect(shaper);
		}
		const second = hz * Math.pow(2, (recipe.interval || 0) / 12);
		const pair = [oscillator(input, time, end, recipe.waves[0], hz, -6), oscillator(input, time, end, recipe.waves[1], second, 6)];
		if (!plucked && length > 0.3) { vibrato(pair, time, end, 9); }
	}

	// A sine whose pitch another sine shakes, hard at the strike and less as it rings: tubular bells (14), crystal (98),
	// vibraphone and the other mallets
	function bell(out, time, note, velocity, length, program) {
		const hz = frequency(note);
		const ratio = program === 14 ? 3.5 : (program === 98 ? 7 : 4);
		const gain = context.createGain();
		gain.gain.value = 0;
		gain.connect(placed(out, note, 0.55));
		const end = contour(gain.gain, time, length, 0.22 * velocity, 0.002, program === 11 ? 0.5 : 0.9, 0, 0.25);
		const depth = context.createGain();
		depth.gain.setValueAtTime(hz * (0.8 + 1.6 * velocity), time);
		depth.gain.setTargetAtTime(hz * 0.15, time, 0.12);
		const carrier = oscillator(gain, time, end, "sine", hz);
		oscillator(depth, time, end, "sine", hz * ratio);
		depth.connect(carrier.frequency);
	}

	function brass(out, time, note, velocity, length) {
		const hz = frequency(note);
		const { filter, gain } = voice(out, hz * 1.5, 2);
		const end = contour(gain.gain, time, length, 0.14 * velocity, 0.018, 0.4, 0.8, 0.14);
		filter.frequency.setValueAtTime(hz * 1.5, time);
		filter.frequency.linearRampToValueAtTime(Math.min(9000, hz * 8), time + 0.05);
		filter.frequency.setTargetAtTime(Math.min(9000, hz * 4), time + 0.05, 0.15);
		for (const cents of [-9, 9]) { oscillator(filter, time, end, "sawtooth", hz, cents); }
	}

	function orchestraHit(out, time, note, velocity) {
		burst(out, time, 0.4, 0.25 * velocity, "lowpass", 3000);
		const { filter, gain } = voice(out, 4500, 1);
		const end = contour(gain.gain, time, 0.05, 0.09 * velocity, 0.003, 0.35, 0, 0.3);
		for (const step of [-12, 0, 7, 12, 19]) { oscillator(filter, time, end, "sawtooth", frequency(note + step), step); }
	}

	function swell(out, time, velocity, length) {
		const source = context.createBufferSource();
		source.buffer = noiseBuffer();
		source.loop = true;
		const filter = context.createBiquadFilter();
		filter.type = "highpass";
		filter.frequency.value = 4000;
		const gain = context.createGain();
		source.connect(filter);
		filter.connect(gain);
		gain.connect(out);
		const span = Math.max(0.3, length);
		gain.gain.setValueAtTime(0.0001, time);
		gain.gain.exponentialRampToValueAtTime(0.3 * velocity, time + span);
		gain.gain.linearRampToValueAtTime(0.0001, time + span + 0.05);
		source.start(time);
		source.stop(time + span + 0.1);
	}

	function tone(out, time, note, velocity, length, program) {
		if (program === 55) { orchestraHit(out, time, note, velocity); return; }
		const family = program >> 3;
		switch (family) {
			case 1: case 12: bell(out, time, note, velocity, length, program); break;
			case 4: bass(out, time, note, velocity, length, program); break;
			case 5: case 6: case 11: pad(out, time, note, velocity, length, program); break;
			case 7: brass(out, time, note, velocity, length); break;
			case 14: swell(out, time, velocity, length); break;
			default: {
				const guitar = family === 3 && program >= 29 ? OVERDRIVEN : null;
				lead(out, time, note, velocity, length, guitar || LEADS[program] || LEAD_FAMILIES[family] || LEADS[81]);
			}
		}
	}

	function noiseSource(out, type, hz, q) {
		const source = context.createBufferSource();
		source.buffer = noiseBuffer();
		const filter = context.createBiquadFilter();
		filter.type = type;
		filter.frequency.value = hz;
		filter.Q.value = q || 0.7;
		const gain = context.createGain();
		source.connect(filter);
		filter.connect(gain);
		gain.connect(out);
		return { source, gain };
	}

	function burst(out, time, length, peak, type, hz, q) {
		const { source, gain } = noiseSource(out, type, hz, q);
		const end = envelope(gain, time, peak, 0.002, 0.005, length);
		source.start(time, Math.random());
		source.stop(end + 0.05);
	}

	// Noise held near full for `hold` and then shut: a snare through a gated reverb
	function gatedBurst(out, time, hold, peak, hz) {
		const { source, gain } = noiseSource(out, "bandpass", hz, 0.5);
		gain.gain.setValueAtTime(0.0001, time);
		gain.gain.linearRampToValueAtTime(peak, time + 0.002);
		gain.gain.exponentialRampToValueAtTime(peak * 0.45, time + hold);
		gain.gain.linearRampToValueAtTime(0.0001, time + hold + 0.025);
		source.start(time, Math.random());
		source.stop(time + hold + 0.08);
	}

	function drop(out, time, from, to, length, peak) {
		const osc = context.createOscillator();
		osc.type = "sine";
		osc.frequency.setValueAtTime(from, time);
		osc.frequency.exponentialRampToValueAtTime(to, time + length * 0.6);
		const gain = context.createGain();
		osc.connect(gain);
		gain.connect(out);
		const end = envelope(gain, time, peak, 0.002, 0.01, length);
		osc.start(time);
		osc.stop(end + 0.05);
	}

	// Channel 10, by its kit: power (16) and electronic (24) have the gated snare and big toms, the 808 (25) a long boom
	// and a snappy snare. `dry` skips the reverb send, for the kick.
	function drum(out, dry, time, note, velocity, kit) {
		const v = velocity;
		const boom = kit === 25;
		const gated = kit === 16 || kit === 24;
		if (note === 35 || note === 36) {
			burst(dry, time, 0.012, 0.2 * v, "highpass", 2500);
			drop(dry, time, boom ? 130 : 170, boom ? 38 : 46, boom ? 0.6 : 0.32, 0.8 * v);
		} else if (note === 38 || note === 40) {
			drop(out, time, 240, 165, 0.1, 0.32 * v);
			if (gated) { gatedBurst(out, time, 0.13, 0.42 * v, 1900); }
			else { burst(out, time, boom ? 0.14 : 0.18, 0.45 * v, "bandpass", boom ? 3200 : 2200, 0.8); }
		} else if (note === 37) {
			burst(out, time, 0.03, 0.35 * v, "bandpass", 1700, 3);
		} else if (note === 39) {
			for (const lag of [0, 0.011, 0.023]) { burst(out, time + lag, 0.012, 0.35 * v, "bandpass", 1300, 1.1); }
			burst(out, time + 0.032, 0.18, 0.4 * v, "bandpass", 1300, 1.1);
		} else if (note === 42 || note === 44) {
			burst(out, time, 0.04, 0.28 * v, "highpass", 8500);
		} else if (note === 46) {
			burst(out, time, 0.28, 0.25 * v, "highpass", 7500);
		} else if (note === 49 || note === 57 || note === 52 || note === 55) {
			burst(out, time, 1.5, 0.22 * v, "highpass", 5000);
			burst(out, time, 0.8, 0.1 * v, "bandpass", 9500, 2);
		} else if (note === 51 || note === 59 || note === 53) {
			burst(out, time, 0.7, 0.14 * v, "bandpass", 6000, 1.5);
		} else if (note >= 41 && note <= 50) {
			const hz = 85 + (note - 41) * 20;
			drop(out, time, hz, hz * 0.55, gated ? 0.4 : 0.3, 0.6 * v);
			if (gated) { burst(out, time, 0.05, 0.12 * v, "bandpass", hz * 9, 1); }
		} else {
			burst(out, time, 0.08, 0.2 * v, "bandpass", 3000);
		}
	}

	// A channel of a playing MIDI sound: brightness lowpass, expression, pan to the sound's bus, reverb send to its wet
	// bus. `dry` joins after the send.
	function strip(strips, channel, bus, wet) {
		let result = strips.get(channel);
		if (!result) {
			const input = context.createBiquadFilter();
			input.type = "lowpass";
			input.frequency.value = Math.min(18000, context.sampleRate * 0.45);
			input.Q.value = 3;
			const level = context.createGain();
			const send = context.createGain();
			send.gain.value = 0;
			const dry = context.createStereoPanner();
			input.connect(level);
			level.connect(dry);
			level.connect(send);
			dry.connect(bus);
			send.connect(wet);
			result = { input, level, send, dry };
			strips.set(channel, result);
		}
		return result;
	}

	function steer(channel, control, value, time) {
		if (control === PAN) { channel.dry.pan.setValueAtTime(Math.max(-1, Math.min(1, (value * 127 - 64) / 63)), time); }
		else if (control === EXPRESSION) { channel.level.gain.setTargetAtTime(Math.pow(value, 1.6), time, 0.012); }
		else if (control === BRIGHTNESS) { channel.input.frequency.setTargetAtTime(Math.min(220 * Math.pow(72, value), context.sampleRate * 0.45), time, 0.03); }
		else if (control === REVERB) { channel.send.gain.setValueAtTime(value, time); }
	}

	// ------------------------------------------------------------------------------------------------ sounds

	function stopVoices(sound) {
		if (sound.source) { try { sound.source.stop(); } catch (error) { /* already ended */ } sound.source = null; }
		if (sound.timer) { clearInterval(sound.timer); sound.timer = 0; }
		for (const bus of [sound.bus, sound.wet]) {
			if (bus) {
				bus.gain.setTargetAtTime(0, context.currentTime, 0.01);
				setTimeout(() => bus.disconnect(), 200);
			}
		}
		sound.bus = null;
		sound.wet = null;
		sound.playing = false;
	}

	function newBus(sound, target) {
		const bus = context.createGain();
		bus.gain.value = sound.volume;
		bus.connect(target);
		return bus;
	}

	function finished(sound) {
		sound.playing = false;
		if (sound.released) { sounds.delete(sound.id); }
	}

	function playBuffer(sound) {
		const bus = sound.bus = newBus(sound, master);
		const source = context.createBufferSource();
		source.buffer = sound.buffer;
		source.loop = sound.loop;
		source.connect(bus);
		source.onended = () => { if (sound.source === source) { finished(sound); } };
		source.start();
		sound.source = source;
	}

	function playMidi(sound) {
		const song = sound.midi;
		if (!song.timeline.length) { finished(sound); return; }
		const bus = sound.bus = newBus(sound, mix);
		const wet = sound.wet = newBus(sound, reverbIn);
		const strips = new Map();
		let origin = context.currentTime + 0.05;
		let next = 0;
		const schedule = () => {
			const horizon = context.currentTime + LOOKAHEAD;
			while (true) {
				if (next >= song.timeline.length) {
					if (!sound.loop) {
						if (context.currentTime > origin + song.length) { stopVoices(sound); finished(sound); }
						return;
					}
					origin += song.length;
					next = 0;
				}
				const event = song.timeline[next];
				const time = origin + event.time;
				if (time > horizon) { return; }
				next += 1;
				const channel = strip(strips, event.channel, bus, wet);
				if (event.control !== undefined) { steer(channel, event.control, event.value, time); }
				else if (event.channel === 9) { drum(channel.input, channel.dry, time, event.note, event.velocity, event.program); }
				else { tone(channel.input, time, event.note, event.velocity, event.length, event.program); }
			}
		};
		schedule();
		sound.timer = setInterval(schedule, 150);
	}

	function start(sound) {
		if (!context || !sound.ready) { sound.pending = true; return; }
		stopVoices(sound);
		sound.playing = true;
		if (sound.midi) { playMidi(sound); } else if (sound.buffer) { playBuffer(sound); }
	}

	function handle(op, id, bytes, value) {
		if (op === OP.stopAll) {
			for (const sound of sounds.values()) { stopVoices(sound); sound.pending = false; }
			return;
		}
		if (op === OP.load) {
			const sound = { id, volume: 1, loop: false, ready: false, pending: false, playing: false, released: false };
			sounds.set(id, sound);
			if (!context) { unlock(); }
			if (!context || !bytes) { return; }
			if (value === FORMAT.midi) {
				sound.midi = parseMidi(bytes);
				sound.ready = !!sound.midi;
				if (sound.ready && sound.pending) { sound.pending = false; start(sound); }
			} else {
				context.decodeAudioData(bytes.buffer).then((buffer) => {
					sound.buffer = buffer;
					sound.ready = true;
					if (sound.pending) { sound.pending = false; start(sound); }
				}, () => { /* not a format this browser decodes: silent */ });
			}
			return;
		}
		const sound = sounds.get(id);
		if (!sound) { return; }
		if (op === OP.play) { sound.loop = value !== 0; start(sound); }
		else if (op === OP.stop) { sound.pending = false; stopVoices(sound); }
		else if (op === OP.volume) {
			sound.volume = value;
			for (const bus of [sound.bus, sound.wet]) {
				if (bus) { bus.gain.setTargetAtTime(value, context.currentTime, 0.02); }
			}
		} else if (op === OP.release) {
			sound.released = true;
			if (value === 0 || !sound.playing) { stopVoices(sound); sounds.delete(id); }
		}
	}

	function reset() {
		for (const sound of sounds.values()) { if (context) { stopVoices(sound); } }
		sounds.clear();
	}

	// ------------------------------------------------------------------------------------------------ controls

	// A speaker button that mutes and a slider for the volume, wired into a container on the page
	function controls(container) {
		const button = document.createElement("button");
		button.type = "button";
		button.className = "btn play-mute";
		const slider = document.createElement("input");
		slider.type = "range";
		slider.min = "0";
		slider.max = "100";
		slider.className = "play-volume";
		slider.setAttribute("aria-label", "Volume");
		const speaker = '<svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path fill="currentColor" d="M4 9h4l5-4v14l-5-4H4z"/>';
		const waves = '<path fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" d="M16 9a4 4 0 0 1 0 6M18.5 6.5a7.5 7.5 0 0 1 0 11"/></svg>';
		const cross = '<path fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" d="M16 9l5 6M21 9l-5 6"/></svg>';
		const show = () => {
			const silent = settings.muted || settings.volume === 0;
			button.innerHTML = speaker + (silent ? cross : waves);
			button.setAttribute("aria-label", settings.muted ? "Unmute" : "Mute");
			button.setAttribute("aria-pressed", settings.muted ? "true" : "false");
			button.title = settings.muted ? "Unmute" : "Mute";
			slider.value = String(Math.round(settings.volume * 100));
		};
		button.addEventListener("click", () => {
			settings.muted = !settings.muted;
			unlock();
			applyMaster();
			save();
			show();
		});
		slider.addEventListener("input", () => {
			settings.volume = Number(slider.value) / 100;
			if (settings.volume > 0) { settings.muted = false; }
			unlock();
			applyMaster();
			save();
			show();
		});
		show();
		container.append(button, slider);
	}

	return { handle, unlock, reset, controls };
})();
