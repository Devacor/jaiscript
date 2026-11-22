#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace jai::foundry::tests {

// Ends the run when a step in its scope (a parse, a load) does not return, naming its input: a failure, not a hang. A step
// here takes milliseconds, and one that loops grows by hundreds of megabytes a second, so it gets seconds
class input_deadline {
public:
    explicit input_deadline(const char* a_step, int a_seconds = 10) : step_(a_step), seconds_(a_seconds), watch_([this]() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (size_t watched = taken_; !done_; watched = taken_) {
            if (!moved_.wait_for(lock, std::chrono::seconds(seconds_), [&]() { return done_ || taken_ != watched; })) {
                std::fprintf(stderr, "\n%s did not return within %d s: %s\n", step_, seconds_, input_.c_str());
                std::abort();
            }
        }
    }) {}
    ~input_deadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        moved_.notify_one();
        watch_.join();
    }
    // the step that starts now takes this input
    void taking(const std::string& a_input) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            input_ = a_input;
            ++taken_;
        }
        moved_.notify_one();
    }
private:
    const char* step_;
    int seconds_;
    std::mutex mutex_;
    std::condition_variable moved_;
    bool done_ = false;
    size_t taken_ = 0;
    std::string input_;
    std::thread watch_;
};

} // namespace jai::foundry::tests
