#include "robo_eyes_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>

#define TAG "RoboEyesDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

namespace {
// esp_lvgl_port's monochrome conversion lights a pixel for dark colors, so in
// LVGL "black" is a lit OLED pixel and "white" is the dark background (the stock
// OLED UI relies on the same mapping).
const lv_color_t kPixelOn = lv_color_black();
const lv_color_t kPixelOff = lv_color_white();
constexpr int kStatusBarHeight = 16;
constexpr uint32_t kFrameMs = 40;  // 25 fps is plenty over 400 kHz I2C
}  // namespace

RoboEyesDisplay::RoboEyesDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);
    LvglThemeManager::GetInstance().RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };
    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
    }
}

RoboEyesDisplay::~RoboEyesDisplay() {
    if (animation_timer_ != nullptr) {
        lv_timer_delete(animation_timer_);
    }
    eyes_.reset();
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool RoboEyesDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void RoboEyesDisplay::Unlock() { lvgl_port_unlock(); }

void RoboEyesDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    Display::SetupUI();
    if (display_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, kPixelOff, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, kPixelOn, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Top bar: network icon on the left, mute / battery on the right.
    top_bar_ = lv_obj_create(screen);
    lv_obj_remove_style_all(top_bar_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, kStatusBarHeight);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);  // icons kept for the base class, not shown

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_remove_style_all(right_icons);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    // Text strip drawn over the top of the eyes when there is something to read.
    status_bar_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_bar_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, kStatusBarHeight);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, kPixelOff, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES - 4);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES - 4);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    // Eyes fill the whole screen.
    eyes_ = std::make_unique<RoboEyes>(screen, width_, height_, kPixelOn, kPixelOff);
    lv_obj_align(eyes_->obj(), LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_move_to_index(eyes_->obj(), 0);  // the text strip is drawn on top

    // Dot-matrix clock / countdown, drawn where the eyes are; hidden until needed.
    dot_clock_ = std::make_unique<DotClock>(screen, width_, height_, eyes_->left_cx(),
                                            eyes_->right_cx(), kPixelOn, kPixelOff);
    lv_obj_align(dot_clock_->obj(), LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_move_to_index(dot_clock_->obj(), 1);
    lv_obj_add_flag(dot_clock_->obj(), LV_OBJ_FLAG_HIDDEN);

    animation_timer_ = lv_timer_create(AnimationTimerCb, kFrameMs, this);
}

void RoboEyesDisplay::AnimationTimerCb(lv_timer_t* timer) {
    auto self = static_cast<RoboEyesDisplay*>(lv_timer_get_user_data(timer));
    // LVGL timers run inside the LVGL task with the port lock already held.
    self->Animate(kFrameMs);
}

void RoboEyesDisplay::Animate(uint32_t elapsed_ms) {
    if (eyes_ == nullptr) {
        return;
    }
    DeviceState state = Application::GetInstance().GetDeviceState();
    if (static_cast<int>(state) != last_state_) {
        last_state_ = static_cast<int>(state);
        idle_ms_ = 0;
    }
    const uint32_t now = lv_tick_get();
    if (hold_active_ && static_cast<int32_t>(hold_until_ms_ - now) <= 0) {
        hold_active_ = false;
    }
    if (hold_active_) {
        eyes_->SetMood(hold_mood_);
        idle_ms_ = 0;  // a requested expression is not replaced by sleep
    }

    // Clock mode ends when a new conversation starts (wake word / button).
    if (clock_mode_) {
        if (state == kDeviceStateIdle) {
            clock_seen_idle_ = true;
        } else if (clock_seen_idle_) {
            clock_mode_ = false;
        }
    }
    const int countdown_s = countdown_ ? countdown_() : -1;
    const bool show_countdown = countdown_s >= 0 && state == kDeviceStateIdle;
    const bool show_dots = !alarm_active_ && (show_countdown || clock_mode_);
    if (show_dots) {
        UpdateDotFace(!show_countdown, countdown_s);
        idle_ms_ = 0;
    }
    if (show_dots != dots_visible_ && dot_clock_ != nullptr) {
        dots_visible_ = show_dots;
        if (show_dots) {
            lv_obj_remove_flag(dot_clock_->obj(), LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(eyes_->obj(), LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dot_clock_->obj(), LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(eyes_->obj(), LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (state == kDeviceStateIdle && !alarm_active_ && !music_active_) {
        idle_ms_ = std::min<uint32_t>(idle_ms_ + elapsed_ms, kSleepAfterMs);
    } else {
        idle_ms_ = 0;
    }

    RoboEyes::Activity activity = RoboEyes::Activity::Idle;
    const bool music_showing = music_active_ && state == kDeviceStateIdle && !alarm_active_;

    // Text strip: only when there is something to read.
    if (status_bar_ != nullptr) {
        const bool setup_state = state == kDeviceStateStarting ||
                                 state == kDeviceStateWifiConfiguring ||
                                 state == kDeviceStateActivating ||
                                 state == kDeviceStateUpgrading ||
                                 state == kDeviceStateAudioTesting ||
                                 state == kDeviceStateFatalError;
        const bool notifying = notification_label_ != nullptr &&
                               !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        const bool timed = static_cast<int32_t>(overlay_until_ms_ - lv_tick_get()) > 0;
        const bool visible = alarm_active_ || setup_state || notifying || timed;
        if (visible != !lv_obj_has_flag(status_bar_, LV_OBJ_FLAG_HIDDEN)) {
            if (visible) {
                lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (alarm_active_) {
        activity = RoboEyes::Activity::Alarm;
    } else if (music_showing) {
        activity = RoboEyes::Activity::Music;
    } else if (power_save_ || idle_ms_ >= kSleepAfterMs) {
        activity = RoboEyes::Activity::Sleeping;
    } else {
        switch (state) {
            case kDeviceStateListening:
                activity = RoboEyes::Activity::Listening;
                break;
            case kDeviceStateSpeaking:
                activity = RoboEyes::Activity::Speaking;
                break;
            case kDeviceStateConnecting:
                activity = RoboEyes::Activity::Thinking;
                break;
            case kDeviceStateStarting:
            case kDeviceStateWifiConfiguring:
            case kDeviceStateActivating:
            case kDeviceStateUpgrading:
            case kDeviceStateAudioTesting:
                activity = RoboEyes::Activity::Setup;
                break;
            default:
                activity = RoboEyes::Activity::Idle;
                break;
        }
    }
    eyes_->SetActivity(activity);
    if (!dots_visible_) {
        eyes_->Tick(elapsed_ms);
    }
}

void RoboEyesDisplay::UpdateDotFace(bool show_clock, int countdown_s) {
    if (dot_clock_ == nullptr) return;
    auto two = [](int v) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", v % 100);
        return std::string(buf);
    };
    const bool blink_on = (lv_tick_get() / 500) % 2 == 0;
    if (!show_clock) {
        // Timer / Pomodoro: minutes | seconds (hours | minutes from 100 min up).
        int minutes = countdown_s / 60;
        if (minutes >= 100) {
            dot_clock_->Set(two(minutes / 60), two(minutes % 60), blink_on);
        } else {
            dot_clock_->Set(two(minutes), two(countdown_s % 60), true);
        }
        return;
    }
    time_t t = time(nullptr);
    if (t < 1735689600) {  // clock not synced yet
        dot_clock_->Set("--", "--", true);
        return;
    }
    struct tm local;
    gmtime_r(&t, &local);  // the clock already holds local wall time
    const bool use_24h = use_24h_ ? use_24h_() : false;
    std::string hours;
    if (use_24h) {
        hours = two(local.tm_hour);
    } else {
        int h12 = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
        hours = std::to_string(h12);
    }
    dot_clock_->Set(hours, two(local.tm_min), blink_on);
}

bool RoboEyesDisplay::HoldExpression(const std::string& name, int seconds) {
    RoboEyes::Mood mood = RoboEyes::MoodFromEmotion(name.c_str());
    if (mood == RoboEyes::Mood::Focused && name != "focused" && name != "neutral") {
        return false;
    }
    DisplayLockGuard lock(this);
    hold_mood_ = mood;
    hold_active_ = true;
    hold_until_ms_ = lv_tick_get() + static_cast<uint32_t>(std::max(1, seconds)) * 1000;
    clock_mode_ = false;
    idle_ms_ = 0;
    if (eyes_ != nullptr) eyes_->SetMood(mood);
    return true;
}

void RoboEyesDisplay::ShowClock() {
    DisplayLockGuard lock(this);
    clock_mode_ = true;
    clock_seen_idle_ = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    hold_active_ = false;
}

void RoboEyesDisplay::SetStatus(const char* status) {
    if (alarm_active_) {
        return;  // the alarm banner stays pinned until it is cleared
    }
    if (music_active_ && static_cast<int32_t>(overlay_until_ms_ - lv_tick_get()) > 0) {
        return;  // keep the song title on screen for its few seconds
    }
    LvglDisplay::SetStatus(status);
}

void RoboEyesDisplay::SetChatMessage(const char* role, const char* content) {
    if (role != nullptr && strcmp(role, "user") == 0) {
        if (content == nullptr || content[0] == '\0') return;
        {
            DisplayLockGuard lock(this);
            clock_mode_ = false;  // talking again brings the eyes back
        }
        if (on_user_speech_) on_user_speech_(content);
        return;
    }
    // The face fills the screen, so only system messages (activation code,
    // errors) are shown, scrolling through the text strip for a while.
    if (content == nullptr || content[0] == '\0' || alarm_active_ || role == nullptr ||
        strcmp(role, "system") != 0) {
        return;
    }
    std::string text = content;
    std::replace(text.begin(), text.end(), '\n', ' ');
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        return;
    }
    lv_label_set_text(status_label_, text.c_str());
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    ShowOverlayFor(15000);
}

void RoboEyesDisplay::ShowOverlayFor(uint32_t ms) { overlay_until_ms_ = lv_tick_get() + ms; }

void RoboEyesDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (hold_active_) {
        return;  // an expression the user asked for stays until its time is up
    }
    const uint32_t now = lv_tick_get();
    const bool neutral = emotion == nullptr || strcmp(emotion, "neutral") == 0;
    if (neutral && now - last_emotion_ms_ < 4000) {
        return;  // keep the last real emotion a little: no flicking between faces
    }
    if (!neutral) {
        last_emotion_ms_ = now;
    }
    if (eyes_ != nullptr) {
        eyes_->SetEmotion(emotion);
    }
    if (emotion != nullptr && strcmp(emotion, "sleepy") != 0 && strcmp(emotion, "neutral") != 0) {
        idle_ms_ = 0;  // any real emotion wakes the eyes up
    }
}

void RoboEyesDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    lv_obj_set_style_text_font(lv_screen_active(), lvgl_theme->text_font()->font(), 0);
}

void RoboEyesDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    power_save_ = on;
    idle_ms_ = 0;
}

void RoboEyesDisplay::SetAlarmBanner(const std::string& text) {
    DisplayLockGuard lock(this);
    alarm_active_ = true;
    idle_ms_ = 0;
    if (status_label_ != nullptr) {
        lv_label_set_text(status_label_, text.c_str());
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void RoboEyesDisplay::ClearAlarmBanner() {
    {
        DisplayLockGuard lock(this);
        alarm_active_ = false;
        idle_ms_ = 0;
        last_displayed_clock_min_ = -1;  // let the idle clock come back right away
        if (eyes_ != nullptr) {
            eyes_->SetMood(RoboEyes::Mood::Happy);
        }
    }
    SetStatus(Lang::Strings::STANDBY);
    UpdateStatusBar(true);
}

void RoboEyesDisplay::SetNowPlaying(const std::string& title) {
    DisplayLockGuard lock(this);
    music_active_ = true;
    idle_ms_ = 0;
    if (status_label_ != nullptr && !alarm_active_) {
        std::string text = "Playing: " + title;
        lv_label_set_text(status_label_, text.c_str());
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        if (notification_label_ != nullptr) lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        ShowOverlayFor(6000);
    }
}

void RoboEyesDisplay::ClearNowPlaying() {
    {
        DisplayLockGuard lock(this);
        if (!music_active_) return;
        music_active_ = false;
        overlay_until_ms_ = lv_tick_get();
        last_displayed_clock_min_ = -1;
    }
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        SetStatus(Lang::Strings::STANDBY);
        UpdateStatusBar(true);
    }
}
