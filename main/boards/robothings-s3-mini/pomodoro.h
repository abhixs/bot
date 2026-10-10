// Continuous Pomodoro cycle for the RoboThings S3 Mini, built on AlarmManager
// timers:
//
//   Focus (25 min) -> Break (5 min) -> Focus (25 min) -> Break -> ...
//
// Each period starts the next one by itself the moment it ends (the alarm only
// announces it), until the user stops the Pomodoro. The user can extend the
// current period ("extend 10 minutes": focus or break, whichever runs), pause and
// resume it (the remaining time and the period are kept), or stop the session.
//
// The session survives a restart (NVS namespace "pomodoro"). All methods run on
// the main task.
#pragma once

#include <cstdint>
#include <functional>

#include "alarm_manager.h"

class Pomodoro {
public:
    enum class Phase { Focus, Break };

    static constexpr int kFocusSeconds = 25 * 60;
    static constexpr int kBreakSeconds = 5 * 60;

    static Pomodoro& GetInstance() {
        static Pomodoro instance;
        return instance;
    }

    // Restores a running (or paused) session. Call after AlarmManager::Initialize().
    void Initialize();

    // Called when a period starts or resumes (to show its countdown) and when the
    // session ends (to return to the normal display).
    void OnPhaseStart(std::function<void(Phase)> cb) { on_phase_start_ = std::move(cb); }
    void OnEnd(std::function<void()> cb) { on_end_ = std::move(cb); }

    // Starts (or restarts) the cycle with a focus period.
    void Start(int focus_seconds = kFocusSeconds);
    // Ends the cycle for good.
    void Stop();
    // Adds time to the current period (focus or break), also while paused.
    bool Extend(int seconds);
    // Freezes the current period with its remaining time / continues it.
    bool Pause();
    bool Resume();

    bool active() const { return active_; }
    bool paused() const { return active_ && paused_; }
    int paused_remaining() const { return paused() ? paused_remaining_ : -1; }
    Phase phase() const { return phase_; }

    // AlarmManager ring hook (main task): the running period ended.
    void HandleRing(const AlarmManager::RingInfo& info);
    // Ends a session whose timer was cancelled from outside (call regularly).
    void CheckTimer();

private:
    Pomodoro() = default;

    void StartPhase(Phase phase, int seconds);
    void Save();

    bool active_ = false;
    bool paused_ = false;
    int paused_remaining_ = 0;
    Phase phase_ = Phase::Focus;
    int timer_id_ = 0;
    int focus_seconds_ = kFocusSeconds;
    int64_t missing_since_us_ = 0;
    std::function<void(Phase)> on_phase_start_;
    std::function<void()> on_end_;
};
