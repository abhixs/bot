#include "pomodoro.h"

#include "application.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "Pomodoro"

namespace {
constexpr const char* kNvsNamespace = "pomodoro";
// After a focus rings and the device is back in standby (the alarm stopped by
// itself, a button, or a conversation that was not a decision), the break starts.
constexpr int64_t kIdleBeforeBreakUs = 2 * 1000000LL;
// Upper bound for a decision, whatever happens.
constexpr int64_t kMaxDecisionUs = 90 * 1000000LL;
}  // namespace

void Pomodoro::Initialize() {
    Settings settings(kNvsNamespace, false);
    active_ = settings.GetBool("active", false);
    phase_ = settings.GetInt("phase", 0) == 1 ? Phase::Break : Phase::Focus;
    timer_id_ = settings.GetInt("timer", 0);
    focus_seconds_ = settings.GetInt("focus", kFocusSeconds);
    if (active_ && !AlarmManager::GetInstance().HasTimer(timer_id_)) {
        // The phase ended while the device was off: the session is over.
        active_ = false;
        Save();
    }
    if (active_) {
        ESP_LOGI(TAG, "Session restored (%s)", phase_ == Phase::Focus ? "focus" : "break");
    }

    esp_timer_create_args_t args = {
        .callback =
            [](void* arg) {
                Application::GetInstance().Schedule(
                    [arg]() { static_cast<Pomodoro*>(arg)->Watch(); });
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pomodoro",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &watch_timer_);
}

void Pomodoro::Start(int focus_seconds) {
    auto& alarms = AlarmManager::GetInstance();
    if (active_ && timer_id_ != 0) {
        alarms.CancelTimer(timer_id_);
    }
    active_ = true;
    awaiting_decision_ = false;
    focus_seconds_ = focus_seconds > 0 ? focus_seconds : kFocusSeconds;
    ESP_LOGI(TAG, "Session started (%d min focus)", focus_seconds_ / 60);
    StartPhase(Phase::Focus, focus_seconds_);
}

void Pomodoro::Stop() {
    if (!active_) {
        return;
    }
    auto& alarms = AlarmManager::GetInstance();
    if (timer_id_ != 0) {
        alarms.CancelTimer(timer_id_);
    }
    if (awaiting_decision_ && alarms.IsRinging()) {
        alarms.StopRinging();
    }
    active_ = false;
    awaiting_decision_ = false;
    timer_id_ = 0;
    esp_timer_stop(watch_timer_);
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
    auto& alarms = AlarmManager::GetInstance();
    if (awaiting_decision_) {
        awaiting_decision_ = false;
        esp_timer_stop(watch_timer_);
        if (alarms.IsRinging()) {
            alarms.StopRinging();
        }
        ESP_LOGI(TAG, "Focus extended by %d min", seconds / 60);
        StartPhase(Phase::Focus, seconds);  // focus extension, then the break
        return true;
    }
    return timer_id_ != 0 && alarms.ExtendTimer(timer_id_, seconds);
}

bool Pomodoro::Continue() {
    if (!active_ || !awaiting_decision_) {
        return false;
    }
    awaiting_decision_ = false;
    esp_timer_stop(watch_timer_);
    auto& alarms = AlarmManager::GetInstance();
    if (alarms.IsRinging()) {
        alarms.StopRinging();
    }
    StartPhase(Phase::Break, kBreakSeconds);
    return true;
}

void Pomodoro::HandleRing(const AlarmManager::RingInfo& info) {
    if (!active_ || !info.is_timer || info.id != timer_id_) {
        return;
    }
    timer_id_ = 0;
    if (phase_ == Phase::Break) {
        // The break is over: the next focus starts at once, the alarm announces it.
        StartPhase(Phase::Focus, focus_seconds_);
        return;
    }
    // Focus is over: wait for "extend N minutes" or "stop".
    awaiting_decision_ = true;
    decision_since_us_ = esp_timer_get_time();
    idle_since_us_ = 0;
    esp_timer_stop(watch_timer_);
    esp_timer_start_periodic(watch_timer_, 500 * 1000);
    Save();
}

void Pomodoro::StartPhase(Phase phase, int seconds) {
    phase_ = phase;
    timer_id_ = AlarmManager::GetInstance().AddTimer(
        seconds, phase == Phase::Focus ? "Pomodoro focus" : "Pomodoro break");
    if (timer_id_ < 0) {
        ESP_LOGE(TAG, "Could not start the %s timer", phase == Phase::Focus ? "focus" : "break");
        timer_id_ = 0;
        active_ = false;
        Save();
        if (on_end_) on_end_();
        return;
    }
    Save();
    if (on_phase_start_) {
        on_phase_start_(phase);
    }
}

void Pomodoro::Watch() {
    if (!awaiting_decision_) {
        esp_timer_stop(watch_timer_);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - decision_since_us_ > kMaxDecisionUs) {
        Continue();
        return;
    }
    if (AlarmManager::GetInstance().IsRinging() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        idle_since_us_ = 0;  // still ringing, or the user is talking
        return;
    }
    if (idle_since_us_ == 0) {
        idle_since_us_ = now;
    } else if (now - idle_since_us_ >= kIdleBeforeBreakUs) {
        Continue();  // no extension asked for: take the break
    }
}

void Pomodoro::Save() {
    Settings settings(kNvsNamespace, true);
    settings.SetBool("active", active_);
    settings.SetInt("phase", phase_ == Phase::Break ? 1 : 0);
    settings.SetInt("timer", timer_id_);
    settings.SetInt("focus", focus_seconds_);
}
