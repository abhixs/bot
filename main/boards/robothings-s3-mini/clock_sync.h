// Keeps the device clock on the user's local time zone (India by default).
//
// The Xiaozhi server sets the clock at boot with its own timezone offset, which
// is not always the user's. ClockSync gets real UTC from SNTP and keeps the
// system clock at UTC + the configured offset (the firmware treats the raw
// clock as local wall-clock time everywhere, including alarms).
#pragma once

#include <esp_timer.h>

#include <atomic>
#include <cstdint>

class ClockSync {
public:
    static constexpr int kDefaultOffsetMinutes = 330;  // IST, UTC+05:30

    static ClockSync& GetInstance() {
        static ClockSync instance;
        return instance;
    }

    void Initialize();
    int offset_minutes() const { return offset_minutes_; }
    void SetOffsetMinutes(int minutes);
    // 12-hour or 24-hour clock on the screen (and in self.clock.get_time).
    bool use_24h() const { return use_24h_; }
    void Set24h(bool use_24h);

private:
    ClockSync() = default;
    void Check();
    void StartSntp();
    static void OnSntpSync(struct timeval* tv);

    esp_timer_handle_t timer_ = nullptr;
    bool sntp_started_ = false;
    std::atomic<int> offset_minutes_{kDefaultOffsetMinutes};
    std::atomic<bool> use_24h_{false};
    // Reference point from the last SNTP sync: UTC seconds and esp_timer microseconds.
    std::atomic<int64_t> ref_utc_s_{0};
    std::atomic<int64_t> ref_uptime_us_{0};
};
