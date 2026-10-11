// Voice reminders for the RoboThings S3 Mini: "remind me to drink water after 30
// minutes", "kal subah 8 baje Rahul ko call karna yaad dilana", daily / weekly ones.
//
// The AI creates, lists, edits and cancels them through device-side MCP tools
// (self.reminder.*), which understand the user's words; the device keeps them in
// NVS (they survive restarts and power cuts) and checks them every second.
//
// A reminder that comes due is put on the "due" list (also in NVS), which the board
// announces one by one when the device is free: the notification sound, then the
// assistant says "Reminder, <message>" and listens for the answer (okay / remind me
// again in N minutes). Reminders cannot ring while the device is off: one whose time
// passed while it was off is announced once as a missed reminder when it is back on
// (a repeating one then continues with its next time).
#pragma once

#include <esp_timer.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class ReminderManager {
public:
    struct Reminder {
        int id = 0;
        std::string message;  // "drink water", "call Rahul"
        int64_t fire_at = 0;  // next time it comes due (device local seconds)
        uint8_t days = 0;     // repeat mask, bit0 = Sunday; 0 = once
        int hour = 0;
        int minute = 0;
    };

    // A reminder waiting to be announced.
    struct Due {
        int id = 0;
        std::string message;
        int64_t due_at = 0;   // when it was due (device local seconds)
        bool missed = false;  // the device was off (or busy for long) at that time
        int attempts = 0;     // announcements that could not reach the assistant
    };

    static constexpr int kMaxReminders = 20;
    static constexpr int kMaxDue = 20;
    static constexpr int kMaxMessageLength = 60;
    // Due longer ago than this when first seen: announced as missed.
    static constexpr int kMissedAfterSeconds = 3 * 60;
    // Announcements that cannot reach the assistant (no network) are tried this often.
    static constexpr int kMaxAttempts = 3;

    static ReminderManager& GetInstance() {
        static ReminderManager instance;
        return instance;
    }

    // Loads saved reminders, registers the MCP tools and starts the 1 s check.
    void Initialize();

    // The oldest reminder waiting to be announced. False when there is none.
    bool PeekDue(Due& out);
    // It was announced (the assistant started speaking it): remove it for good.
    void MarkAnnounced(int id, int64_t due_at);
    // It could not be announced (no connection): try later, or drop it after
    // kMaxAttempts. Returns true if it stays queued.
    bool MarkFailed(int id, int64_t due_at);
    // "Remind me again after N minutes" right after an announcement: a one-time
    // reminder with the same message. Returns its id, or -1.
    int Snooze(const std::string& message, int seconds);

private:
    ReminderManager() = default;
    ReminderManager(const ReminderManager&) = delete;
    ReminderManager& operator=(const ReminderManager&) = delete;

    void RegisterTools();
    void Load();
    void SaveLocked();
    void SaveDueLocked();
    void CheckDue();
    int NextIdLocked();
    std::string ListJson();
    std::string DescribeLocked(const Reminder& r) const;

    std::mutex mutex_;
    std::vector<Reminder> reminders_;
    std::vector<Due> due_;
    int next_id_ = 1;
    esp_timer_handle_t check_timer_ = nullptr;
    // A snooze set by the device a moment ago: the AI's own create call for the same
    // request ("remind me after 10 minutes") must not add a second reminder.
    int64_t last_snooze_us_ = 0;
};
