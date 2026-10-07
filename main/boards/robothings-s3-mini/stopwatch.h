// Stopwatch (counts up) for the RoboThings S3 Mini. Not to be confused with the
// countdown timers in AlarmManager. Controlled from the main task, read by the
// display task, hence the atomics.
#pragma once

#include <esp_timer.h>

#include <atomic>
#include <cstdint>

class Stopwatch {
public:
    static Stopwatch& GetInstance() {
        static Stopwatch instance;
        return instance;
    }

    // Starts from 00:00 (also when it was already running).
    void Start() {
        accumulated_us_.store(0);
        started_us_.store(esp_timer_get_time());
        running_.store(true);
        active_.store(true);
    }
    bool Pause() {
        if (!active_.load() || !running_.load()) return false;
        accumulated_us_.store(accumulated_us_.load() + esp_timer_get_time() - started_us_.load());
        running_.store(false);
        return true;
    }
    bool Resume() {
        if (!active_.load() || running_.load()) return false;
        started_us_.store(esp_timer_get_time());
        running_.store(true);
        return true;
    }
    // Ends it and clears it from the screen.
    bool Stop() {
        if (!active_.load()) return false;
        running_.store(false);
        active_.store(false);
        accumulated_us_.store(0);
        return true;
    }

    bool active() const { return active_.load(); }
    bool running() const { return running_.load(); }
    // Elapsed whole seconds, or -1 when no stopwatch is on.
    int ElapsedSeconds() const {
        if (!active_.load()) return -1;
        int64_t us = accumulated_us_.load();
        if (running_.load()) us += esp_timer_get_time() - started_us_.load();
        return static_cast<int>(us / 1000000);
    }

private:
    Stopwatch() = default;
    std::atomic<bool> active_{false};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> started_us_{0};
    std::atomic<int64_t> accumulated_us_{0};
};
