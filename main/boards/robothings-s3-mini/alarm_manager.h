// Voice-controlled alarms and timers for the RoboThings S3 Mini board.
//
// The AI sets alarms through device-side MCP tools (self.alarm.*). Alarms are kept
// in NVS so they survive a reboot, and they ring from the device itself, so no
// internet connection is needed at ring time (only to set them by voice).
#pragma once

#include <esp_timer.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class AlarmManager {
public:
    struct Alarm {
        int id = 0;
        int hour = 0;
        int minute = 0;
        uint8_t days = 0;     // repeat mask, bit0 = Sunday; 0 = one-shot
        int64_t fire_at = 0;  // next ring time (device local seconds)
        bool is_timer = false;
        bool lamp = false;    // switch the lamp relay on when it rings
        std::string label;
    };

    struct RingInfo {
        std::string title;  // e.g. "07:30" or "Timer"
        std::string label;
        bool lamp = false;
    };

    static constexpr int kMaxAlarms = 10;
    static constexpr int kRingSeconds = 60;
    static constexpr int kMissedGraceSeconds = 10 * 60;

    static AlarmManager& GetInstance() {
        static AlarmManager instance;
        return instance;
    }

    // Loads saved alarms, registers the MCP tools and starts the 1 s check timer.
    void Initialize();

    // Called on the main task when an alarm starts / stops ringing.
    void OnRingStart(std::function<void(const RingInfo&)> cb) { on_ring_start_ = std::move(cb); }
    void OnRingStop(std::function<void()> cb) { on_ring_stop_ = std::move(cb); }

    bool IsRinging() const { return ringing_; }
    // Seconds until the soonest running timer ends (for the on-screen countdown),
    // or -1 when no timer is running.
    int SecondsToNextTimer();
    // Stops a ringing alarm. With snooze_minutes > 0 it rings again later.
    void StopRinging(int snooze_minutes = 0);

private:
    AlarmManager() = default;
    ~AlarmManager();
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    void RegisterTools();
    void Load();
    void SaveLocked();
    void CheckAlarms();
    void StartRinging(const RingInfo& info);
    void RingTick();
    int NextIdLocked();
    std::string ListJson();

    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    esp_timer_handle_t check_timer_ = nullptr;
    esp_timer_handle_t ring_timer_ = nullptr;
    bool ringing_ = false;
    int ring_elapsed_ms_ = 0;
    int next_tone_ms_ = 0;    // only touched on the main task
    int next_toggle_ms_ = 0;  // only touched on the main task
    bool idle_seen_while_ringing_ = false;  // only touched on the main task
    RingInfo current_ring_;
    std::function<void(const RingInfo&)> on_ring_start_;
    std::function<void()> on_ring_stop_;
};
