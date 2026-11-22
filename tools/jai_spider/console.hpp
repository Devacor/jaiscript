#pragma once
// The process's console, shared by the network loop, every worker and every engine's print: each write goes out whole,
// under one lock, so lines from different threads never interleave mid-line.
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>

namespace spider_host {

class console {
public:
	void out(const std::string& a_text) { write(std::cout, a_text); }
	void err(const std::string& a_text) { write(std::cerr, a_text); }

private:
	void write(std::ostream& a_stream, const std::string& a_text) {
		std::lock_guard<std::mutex> hold(lock_);
		a_stream << a_text;
		a_stream.flush();
	}

	std::mutex lock_;
};

// What one engine prints, a line at a time: an engine belongs to one thread, the console to all of them
class console_line_buffer : public std::streambuf {
public:
	explicit console_line_buffer(console& a_console) : console_(a_console) {}
	console_line_buffer(const console_line_buffer&) = delete;
	console_line_buffer& operator=(const console_line_buffer&) = delete;
	~console_line_buffer() override { sync(); }

protected:
	int_type overflow(int_type a_character) override {
		if (traits_type::eq_int_type(a_character, traits_type::eof())) { return traits_type::not_eof(a_character); }
		pending_ += traits_type::to_char_type(a_character);
		if (a_character == '\n') { emit(); }
		return a_character;
	}

	std::streamsize xsputn(const char* a_text, std::streamsize a_count) override {
		pending_.append(a_text, static_cast<size_t>(a_count));
		if (pending_.find('\n') != std::string::npos) { emit(); }
		return a_count;
	}

	int sync() override {
		if (!pending_.empty()) {
			console_.out(pending_);
			pending_.clear();
		}
		return 0;
	}

private:
	// whole lines out, a partial last line kept for the next write
	void emit() {
		const size_t end = pending_.rfind('\n') + 1;
		console_.out(pending_.substr(0, end));
		pending_.erase(0, end);
	}

	console& console_;
	std::string pending_;
};

class console_stream : public std::ostream {
public:
	explicit console_stream(console& a_console) :
		std::ostream(nullptr),
		buffer_(a_console) {
		rdbuf(&buffer_);
	}

private:
	console_line_buffer buffer_;
};

} // namespace spider_host
