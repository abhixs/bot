// Continuous Pomodoro cycle for the RoboThings S3 Mini, built on AlarmManager
// timers:
//
//   Focus (25 min) -> rings -> [extend N min of focus] -> Break (5 min) -> rings ->
//   Focus (25 min) -> ...  until the user stops the Pomodoro.
//
// When a focus timer rings, the user decides: "extend 10 minutes" adds a focus
// extension, "stop" (or no answer: the alarm stops by itself after 20 s, or any
// other conversation ends) starts the break. When a break ends the next focus
// starts at once and the alarm only announces it.
//
// The session survives a restart (NVS namespace "pomodoro"). All methods run on
// the main task.
#pragma once

#include <esp_timer.h>

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

    // Restores a running session and starts the decision watcher.
    void Initialize();

    // Called when a phase timer starts (to show its countdown) and when the
    // session ends (to return to the normal display).
    void OnPhaseStart(std::function<void(Phase)> cb) { on_phase_start_ = std::move(cb); }
    void OnEnd(std::function<void()> cb) { on_end_ = std::move(cb); }

    // Starts (or restarts) the cycle with a focus phase.
    void Start(int focus_seconds = kFocusSeconds);
    // Ends the cycle for good.
    void Stop();
    // A focus timer just ended and the user has not decided yet.
    bool AwaitingDecision() const { return awaiting_decision_; }
    // "Extend N minutes": after a focus ends, a focus extension; while a phase
    // runs, more time on it. Returns false when no session is running.
    bool Extend(int seconds);
    // "Stop" after a focus ended: start the break now. Returns false when nothing
    // was waiting for a decision.
    bool Continue();

    bool active() const { return active_; }
    Phase phase() const { return phase_; }

    // AlarmManager ring hook (main task).
    void HandleRing(const AlarmManager::RingInfo& info);

private:
    Pomodoro() = default;

    void StartPhase(Phase phase, int seconds);
    void Save();
    void Watch();  // main task, every 500 ms while a decision is pending

    bool active_ = false;
    Phase phase_ = Phase::Focus;
    int timer_id_ = 0;
    int focus_seconds_ = kFocusSeconds;
    bool awaiting_decision_ = false;
    int64_t decision_since_us_ = 0;
    int64_t idle_since_us_ = 0;
    esp_timer_handle_t watch_timer_ = nullptr;
    std::function<void(Phase)> on_phase_start_;
    std::function<void()> on_end_;
};
