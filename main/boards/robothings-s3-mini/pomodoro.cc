#include "pomodoro.h"

#include "settings.h"

#include <esp_log.h>

#include <algorithm>

#define TAG "Pomodoro"

namespace {
constexpr const char* kNvsNamespace = "pomodoro";
}  // namespace

void Pomodoro::Initialize() {
    Settings settings(kNvsNamespace, false);
    active_ = settings.GetBool("active", false);
    paused_ = settings.GetBool("paused", false);
    paused_remaining_ = settings.GetInt("remain", 0);
    phase_ = settings.GetInt("phase", 0) == 1 ? Phase::Break : Phase::Focus;
    timer_id_ = settings.GetInt("timer", 0);
    focus_seconds_ = settings.GetInt("focus", kFocusSeconds);
    if (active_ && !paused_ && !AlarmManager::GetInstance().HasTimer(timer_id_)) {
        // The period ended while the device was off: the session is over.
        active_ = false;
        Save();
    }
    if (active_) {
        ESP_LOGI(TAG, "Session restored (%s%s)", phase_ == Phase::Focus ? "focus" : "break",
                 paused_ ? ", paused" : "");
    }
}

void Pomodoro::Start(int focus_seconds) {
    if (active_ && !paused_ && timer_id_ != 0) {
        AlarmManager::GetInstance().CancelTimer(timer_id_);
    }
    active_ = true;
    paused_ = false;
    focus_seconds_ = focus_seconds > 0 ? focus_seconds : kFocusSeconds;
    ESP_LOGI(TAG, "Session started (%d min focus)", focus_seconds_ / 60);
    StartPhase(Phase::Focus, focus_seconds_);
}

void Pomodoro::Stop() {
    if (!active_) {
        return;
    }
    if (!paused_ && timer_id_ != 0) {
        AlarmManager::GetInstance().CancelTimer(timer_id_);
    }
    active_ = false;
    paused_ = false;
    timer_id_ = 0;
    Save();
    ESP_LOGI(TAG, "Session ended");
    if (on_end_) {
        on_end_();
    }
}

bool Pomodoro::Extend(int seconds) {
    if (!active_ || seconds <= 0) {
        return false;
    }
    if (paused_) {
        paused_remaining_ += seconds;
        Save();
        return true;
    }
    if (!AlarmManager::GetInstance().ExtendTimer(timer_id_, seconds)) {
        return false;
    }
    ESP_LOGI(TAG, "%s extended by %d s", phase_ == Phase::Focus ? "Focus" : "Break", seconds);
    return true;
}

bool Pomodoro::Pause() {
    if (!active_ || paused_) {
        return false;
    }
    auto& alarms = AlarmManager::GetInstance();
    const int remaining = alarms.TimerRemaining(timer_id_);
    if (remaining < 0) {
        return false;
    }
    alarms.CancelTimer(timer_id_);
    timer_id_ = 0;
    paused_ = true;
    paused_remaining_ = std::max(1, remaining);
    Save();
    ESP_LOGI(TAG, "Paused (%s, %d s left)", phase_ == Phase::Focus ? "focus" : "break",
             paused_remaining_);
    return true;
}

bool Pomodoro::Resume() {
    if (!active_ || !paused_) {
        return false;
    }
    paused_ = false;
    ESP_LOGI(TAG, "Resumed (%d s left)", paused_remaining_);
    StartPhase(phase_, paused_remaining_);
    return true;
}

void Pomodoro::HandleRing(const AlarmManager::RingInfo& info) {
    if (!active_ || paused_ || !info.is_timer || info.id != timer_id_) {
        return;
    }
    timer_id_ = 0;
    // The next period starts at once; the alarm only announces it.
    if (phase_ == Phase::Focus) {
        StartPhase(Phase::Break, kBreakSeconds);
    } else {
        StartPhase(Phase::Focus, focus_seconds_);
    }
}

void Pomodoro::StartPhase(Phase phase, int seconds) {
    phase_ = phase;
    timer_id_ = AlarmManager::GetInstance().AddTimer(
        seconds, phase == Phase::Focus ? "Pomodoro focus" : "Pomodoro break");
    if (timer_id_ < 0) {
        ESP_LOGE(TAG, "Could not start the %s timer", phase == Phase::Focus ? "focus" : "break");
        timer_id_ = 0;
        active_ = false;
        paused_ = false;
        Save();
        if (on_end_) on_end_();
        return;
    }
    Save();
    if (on_phase_start_) {
        on_phase_start_(phase);
    }
}

void Pomodoro::Save() {
    Settings settings(kNvsNamespace, true);
    settings.SetBool("active", active_);
    settings.SetBool("paused", paused_);
    settings.SetInt("remain", paused_remaining_);
    settings.SetInt("phase", phase_ == Phase::Break ? 1 : 0);
    settings.SetInt("timer", timer_id_);
    settings.SetInt("focus", focus_seconds_);
}
