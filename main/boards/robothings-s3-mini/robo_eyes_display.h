// 128x64 SSD1306 display for the RoboThings S3 Mini: a 16 px status bar on top
// (clock / status / subtitles, Wi-Fi, mute) and animated RoboEyes below it.
#pragma once

#include "lvgl_display.h"
#include "robo_eyes.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <memory>
#include <string>

class RoboEyesDisplay : public LvglDisplay {
public:
    // Idle this long and the eyes fall asleep (cosmetic only: wake word stays on).
    static constexpr uint32_t kSleepAfterMs = 120 * 1000;

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
    lv_timer_t* animation_timer_ = nullptr;

    bool alarm_active_ = false;
    bool power_save_ = false;
    uint32_t idle_ms_ = 0;
    int last_state_ = -1;
};
