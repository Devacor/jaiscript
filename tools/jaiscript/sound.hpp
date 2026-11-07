#pragma once
// Runner-side sound: load_sound(path) -> Sound (play, play(true) to loop, stop, playing, volume, format), play_sound(path[, loop])
// to fire and forget, stop_sounds() to silence everything. The format comes from the file's first bytes (RIFF/WAVE, MThd or
// RIFF/RMID, an ID3 tag or an MPEG frame sync), else from the extension.
//
// Windows plays through winmm's MCI 'mpegvideo' device (DirectShow): one device type opens WAV, MIDI and MP3, each open device
// is an independent voice the system mixes with the others, `play from 0 repeat` loops and `setaudio volume` sets the level. An
// MCI device belongs to the thread that opened it, and opening or starting one takes milliseconds to tens of them, so one audio
// thread owns every device and a script's calls only queue commands. Elsewhere load_sound and play_sound answer null.
#include <jaiscript/jaiscript.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>   // before windows.h, as the runner's own network code requires
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace jai_runner {

enum class sound_format { none, wav, midi, mp3 };

inline const char* sound_format_name(sound_format f) {
	switch (f) {
		case sound_format::wav: return "wav";
		case sound_format::midi: return "midi";
		case sound_format::mp3: return "mp3";
		default: return "";
	}
}

// the header decides; an unrecognized header falls back to the extension; a file that cannot be read is no sound
inline sound_format sniff_sound_format(const std::string& path) {
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_regular_file(path, ec)) { return sound_format::none; }
	std::ifstream in(path, std::ios::binary);
	if (!in) { return sound_format::none; }
	char raw[12] = {0};
	in.read(raw, sizeof(raw));
	const size_t got = static_cast<size_t>(in.gcount());
	const auto* h = reinterpret_cast<const unsigned char*>(raw);
	auto tag_at = [&](size_t i, const char* tag) { return got >= i + 4 && std::equal(tag, tag + 4, raw + i); };
	if (tag_at(0, "RIFF") && tag_at(8, "WAVE")) { return sound_format::wav; }
	if (tag_at(0, "MThd") || (tag_at(0, "RIFF") && tag_at(8, "RMID"))) { return sound_format::midi; }
	if ((got >= 3 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') || (got >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0)) { return sound_format::mp3; }
	std::string ext = fs::path(path).extension().string();
	for (char& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	if (ext == ".wav") { return sound_format::wav; }
	if (ext == ".mid" || ext == ".midi" || ext == ".rmi") { return sound_format::midi; }
	if (ext == ".mp3") { return sound_format::mp3; }
	return sound_format::none;
}

// one open device: the flags the script reads and the audio thread writes
struct sound_voice {
	std::string alias;
	std::atomic<bool> playing{ false };
	std::atomic<bool> looping{ false };
	std::atomic<bool> failed{ false };   // the device would not open: every play answers false from then on
	uint64_t plays = 0;                  // under the mixer's lock: a run-out answer for an earlier play leaves a later one playing
};

#ifdef _WIN32
class sound_mixer {
public:
	static sound_mixer& get() {
		static sound_mixer mixer;
		return mixer;
	}

	~sound_mixer() {
		std::vector<held_sound> released;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			quit_ = true;
			released.swap(held_);
		}
		wake_.notify_all();
		released.clear();   // outside the lock: a released sound's device asks to close
		if (thread_.joinable()) { thread_.join(); }
	}

	std::shared_ptr<sound_voice> open(const std::string& path) {
		auto voice = std::make_shared<sound_voice>();
		std::lock_guard<std::mutex> lock(mutex_);
		voice->alias = "jaisnd" + std::to_string(next_alias_++);
		voices_.push_back(voice);
		post_locked([voice, path]() {
			if (!mci("open \"" + path + "\" type mpegvideo alias " + voice->alias)) {
				voice->failed.store(true);
				voice->playing.store(false);
			}
		});
		return voice;
	}

	bool play(const std::shared_ptr<sound_voice>& voice, bool loop) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (voice->failed.load()) { return false; }
		++voice->plays;
		voice->looping.store(loop);
		voice->playing.store(true);
		post_locked([voice, loop]() {
			if (voice->failed.load()) { return; }
			if (!mci("play " + voice->alias + " from 0" + (loop ? " repeat" : ""))) { voice->playing.store(false); }
		});
		return true;
	}

	void stop(const std::shared_ptr<sound_voice>& voice) {
		std::lock_guard<std::mutex> lock(mutex_);
		voice->playing.store(false);
		post_locked([voice]() { mci("stop " + voice->alias); });
	}

	void volume(const std::shared_ptr<sound_voice>& voice, double level) {
		const int scaled = static_cast<int>(std::clamp(level, 0.0, 1.0) * 1000.0 + 0.5);
		std::lock_guard<std::mutex> lock(mutex_);
		post_locked([voice, scaled]() { mci("setaudio " + voice->alias + " volume to " + std::to_string(scaled)); });
	}

	void close(const std::string& alias) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (quit_) { return; }   // the audio thread closes every device on its way out
		post_locked([alias]() { mci("close " + alias); });
	}

	void stop_all() {
		std::vector<held_sound> released;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			std::vector<std::string> aliases;
			for (auto& weak : voices_) {
				if (auto voice = weak.lock()) {
					voice->playing.store(false);
					aliases.push_back(voice->alias);
				}
			}
			released.swap(held_);
			post_locked([aliases]() { for (const auto& alias : aliases) { mci("stop " + alias); } });
		}
	}

	// a fire-and-forget sound lives here while it plays, so dropping play_sound's result does not cut it off
	void hold(std::shared_ptr<void> sound, std::shared_ptr<sound_voice> voice) {
		std::vector<held_sound> released;   // destroyed after the lock: a released sound's device asks to close
		std::lock_guard<std::mutex> lock(mutex_);
		auto done = std::stable_partition(held_.begin(), held_.end(), [](const held_sound& h) { return h.voice->playing.load(); });
		released.assign(std::make_move_iterator(done), std::make_move_iterator(held_.end()));
		held_.erase(done, held_.end());
		held_.push_back(held_sound{ std::move(sound), std::move(voice) });
	}

private:
	struct held_sound {
		std::shared_ptr<void> sound;
		std::shared_ptr<sound_voice> voice;
	};

	sound_mixer() : thread_([this] { run(); }) {}

	static bool mci(const std::string& command, std::string* reply = nullptr) {
		char buffer[128] = {0};
		const bool ok = ::mciSendStringA(command.c_str(), buffer, sizeof(buffer) - 1, nullptr) == 0;
		if (reply) { *reply = buffer; }
		return ok;
	}

	// the device's mode can say "playing" for seconds after a short sound's last sample, while its position already stands at
	// the end
	static bool ran_out_of_sound(const std::string& alias) {
		std::string mode, position, length;
		if (!mci("status " + alias + " mode", &mode)) { return false; }
		if (mode != "playing") { return true; }
		if (!mci("status " + alias + " position", &position) || !mci("status " + alias + " length", &length)) { return false; }
		const unsigned long long end = std::strtoull(length.c_str(), nullptr, 10);
		return end > 0 && std::strtoull(position.c_str(), nullptr, 10) >= end;
	}

	void post_locked(std::function<void()> command) {
		commands_.push_back(std::move(command));
		wake_.notify_one();
	}

	// commands in order; between them, a sound playing once is asked whether it ran out
	void run() {
		std::unique_lock<std::mutex> lock(mutex_);
		while (true) {
			wake_.wait_for(lock, std::chrono::milliseconds(50), [this] { return quit_ || !commands_.empty(); });
			while (!commands_.empty()) {
				auto command = std::move(commands_.front());
				commands_.pop_front();
				lock.unlock();
				command();
				lock.lock();
			}
			if (quit_) { break; }
			std::vector<std::pair<std::shared_ptr<sound_voice>, uint64_t>> watch;
			std::erase_if(voices_, [](const std::weak_ptr<sound_voice>& weak) { return weak.expired(); });
			for (auto& weak : voices_) {
				if (auto voice = weak.lock(); voice && voice->playing.load() && !voice->looping.load()) { watch.emplace_back(voice, voice->plays); }
			}
			lock.unlock();
			std::vector<std::shared_ptr<sound_voice>> ran_out;
			for (auto& [voice, plays] : watch) {
				if (ran_out_of_sound(voice->alias)) { ran_out.push_back(voice); }
			}
			lock.lock();
			for (auto& [voice, plays] : watch) {
				if (std::find(ran_out.begin(), ran_out.end(), voice) != ran_out.end() && voice->plays == plays) { voice->playing.store(false); }
			}
		}
		lock.unlock();
		::mciSendStringA("close all", nullptr, 0, nullptr);
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::function<void()>> commands_;
	std::vector<std::weak_ptr<sound_voice>> voices_;
	std::vector<held_sound> held_;
	uint64_t next_alias_ = 0;
	bool quit_ = false;
	std::thread thread_;
};
#endif

// the device closes when the last Sound naming it is gone (a copy of a Sound names the same device)
struct sound_device {
	std::shared_ptr<sound_voice> voice;
	sound_format format = sound_format::none;
	~sound_device() {
#ifdef _WIN32
		if (voice) { sound_mixer::get().close(voice->alias); }
#endif
	}
};

class sound {
public:
	explicit sound(std::shared_ptr<sound_device> device) : device_(std::move(device)) {}

	bool play() { return play_looped(false); }
	bool play_looped(bool loop) {
#ifdef _WIN32
		return sound_mixer::get().play(device_->voice, loop);
#else
		(void)loop;
		return false;
#endif
	}
	void stop() {
#ifdef _WIN32
		sound_mixer::get().stop(device_->voice);
#endif
	}
	bool playing() const { return device_->voice && device_->voice->playing.load(); }
	void volume(double level) {
#ifdef _WIN32
		sound_mixer::get().volume(device_->voice, level);
#else
		(void)level;
#endif
	}
	std::string format() const { return sound_format_name(device_->format); }
	const std::shared_ptr<sound_voice>& voice() const { return device_->voice; }

private:
	std::shared_ptr<sound_device> device_;
};

inline std::shared_ptr<sound> load_sound(const std::string& path) {
#ifdef _WIN32
	const sound_format format = sniff_sound_format(path);
	if (format == sound_format::none) { return nullptr; }
	auto device = std::make_shared<sound_device>();
	device->format = format;
	device->voice = sound_mixer::get().open(path);
	return std::make_shared<sound>(std::move(device));
#else
	(void)path;
	return nullptr;
#endif
}

inline std::shared_ptr<sound> play_sound(const std::string& path, bool loop) {
	auto loaded = load_sound(path);
#ifdef _WIN32
	if (loaded && loaded->play_looped(loop)) { sound_mixer::get().hold(loaded, loaded->voice()); }
#else
	(void)loop;
#endif
	return loaded;
}

inline void register_sound(jai::engine& eng) {
	jai::dynamic_binder<sound>(eng, "Sound")
		.method("play", &sound::play)
		.method("play", &sound::play_looped)
		.method("stop", &sound::stop)
		.method("playing", &sound::playing)
		.method("volume", &sound::volume)
		.method("format", &sound::format)
		.build();
	eng.add_function("load_sound", [](const std::string& path) { return load_sound(path); }, jai::filesystem);
	eng.add_function("play_sound", [](const std::string& path) { return play_sound(path, false); }, jai::filesystem);
	eng.add_function("play_sound", [](const std::string& path, bool loop) { return play_sound(path, loop); }, jai::filesystem);
	eng.add_function("stop_sounds", []() {
#ifdef _WIN32
		sound_mixer::get().stop_all();
#endif
	});
}

} // namespace jai_runner
