// 128x64 OLED display for the RoboThings S3 Mini: full-screen RoboEyes, with a
// 16 px text strip on top that only appears for alarms, notifications, setup
// messages (Wi-Fi, activation code) and the song title.
#pragma once

#include "dot_clock.h"
#include "lvgl_display.h"
#include "robo_eyes.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <functional>
#include <memory>
#include <string>

class RoboEyesDisplay : public LvglDisplay {
public:
    // In standby (idle) the eyes fall asleep after this short pause (cosmetic only:
    // the wake word stays on).
    static constexpr uint32_t kSleepAfterMs = 3 * 1000;

    RoboEyesDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, bool mirror_x, bool mirror_y);
    ~RoboEyesDisplay();

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void SetTheme(Theme* theme) override;
    void SetPowerSaveMode(bool on) override;
    bool IsMonochrome() const override { return true; }

    // Shows a pinned banner (e.g. "ALARM 07:00") and the alarm eyes until cleared.
    void SetAlarmBanner(const std::string& text);
    void ClearAlarmBanner();

    // While music plays and the device is idle the status line shows the song and
    // the eyes dance; a conversation still shows its normal status.
    void SetNowPlaying(const std::string& title);
    void ClearNowPlaying();

    // Shows an expression on demand ("roo ke dikhao" -> crying) for `seconds`,
    // overriding automatic emotions and sleep. Returns false for unknown names.
    bool HoldExpression(const std::string& name, int seconds);

    // Dot-matrix clock instead of the eyes until the user talks again.
    void ShowClock();

    // Everything the user says (speech-to-text), for local voice shortcuts.
    void OnUserSpeech(std::function<void(const std::string&)> cb) { on_user_speech_ = std::move(cb); }

    // Seconds left on the running timer, or -1 (the countdown replaces the eyes
    // while the device is idle).
    void SetCountdownProvider(std::function<int()> provider) { countdown_ = std::move(provider); }
    // Returns true if the clock should use 24-hour format.
    void SetClockFormatProvider(std::function<bool()> provider) { use_24h_ = std::move(provider); }

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    static void AnimationTimerCb(lv_timer_t* timer);
    void Animate(uint32_t elapsed_ms);

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    std::unique_ptr<RoboEyes> eyes_;
    std::unique_ptr<DotClock> dot_clock_;
    void UpdateDotFace(bool show_clock, int countdown_s);
    lv_timer_t* animation_timer_ = nullptr;

    bool alarm_active_ = false;
    bool music_active_ = false;
    // The status strip covers the top of the eyes, so it only appears when it has
    // something worth reading: until this tick (lv_tick), or while pinned below.
    uint32_t overlay_until_ms_ = 0;
    void ShowOverlayFor(uint32_t ms);
    bool power_save_ = false;
    uint32_t idle_ms_ = 0;
    int last_state_ = -1;

    // Expression held on request
    bool hold_active_ = false;
    uint32_t hold_until_ms_ = 0;
    RoboEyes::Mood hold_mood_ = RoboEyes::Mood::Focused;
    // Keep a real emotion for a moment instead of flicking back to neutral.
    uint32_t last_emotion_ms_ = 0;

    // Clock mode: on until the next conversation starts.
    bool clock_mode_ = false;
    bool clock_seen_idle_ = false;
    bool dots_visible_ = false;

    std::function<void(const std::string&)> on_user_speech_;
    std::function<int()> countdown_;
    std::function<bool()> use_24h_;
};
