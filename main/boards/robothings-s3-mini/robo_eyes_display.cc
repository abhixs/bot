#include "robo_eyes_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

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

constexpr const char* kSettingsNamespace = "display";
constexpr const char* kMainModeKey = "main_mode";  // "time" / "emotion"
constexpr const char* kThemeKey = "theme";         // "dark" / "light" (shared with Display)

bool IsSetupState(DeviceState state) {
    return state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
           state == kDeviceStateActivating || state == kDeviceStateUpgrading ||
           state == kDeviceStateAudioTesting || state == kDeviceStateFatalError;
}
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

    // Same fonts; "light" inverts the panel (see ApplyThemeLocked). Registering it
    // lets the common "self.screen.set_theme" tool switch light / dark too.
    auto light_theme = new LvglTheme("light");
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);
    LvglThemeManager::GetInstance().RegisterTheme("light", light_theme);

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

    LoadSettings();
    ApplyThemeLocked();
}

// Restores the saved main mode and theme (called once, at start-up).
void RoboEyesDisplay::LoadSettings() {
    Settings settings(kSettingsNamespace, false);
    main_mode_ = settings.GetString(kMainModeKey, "emotion") == "time" ? MainDisplayMode::Time
                                                                      : MainDisplayMode::Emotion;
    theme_ = settings.GetString(kThemeKey, "dark") == "light" ? DisplayTheme::Light
                                                             : DisplayTheme::Dark;
    ESP_LOGI(TAG, "Main mode: %s, theme: %s", main_mode_ == MainDisplayMode::Time ? "time" : "emotion",
             theme_ == DisplayTheme::Light ? "light" : "dark");
}

// Called with the display lock held.
void RoboEyesDisplay::ApplyThemeLocked() {
    const bool light = theme_ == DisplayTheme::Light;
    if (panel_ != nullptr) {
        // The controller's own "inverse display" mode: no pixel is redrawn.
        esp_err_t err = esp_lcd_panel_invert_color(panel_, light);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set the %s theme: %s", light ? "light" : "dark",
                     esp_err_to_name(err));
        }
    }
    if (auto theme = LvglThemeManager::GetInstance().GetTheme(light ? "light" : "dark")) {
        current_theme_ = theme;
    }
}

void RoboEyesDisplay::SetMainMode(MainDisplayMode mode) {
    {
        DisplayLockGuard lock(this);
        main_mode_ = mode;
        countdown_selected_ = false;
        stopwatch_selected_ = false;
        hold_active_ = false;  // show the chosen mode right away
        idle_ms_ = 0;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kMainModeKey, mode == MainDisplayMode::Time ? "time" : "emotion");
    ESP_LOGI(TAG, "Main mode set to %s", mode == MainDisplayMode::Time ? "time" : "emotion");
}

bool RoboEyesDisplay::ShowCountdown() {
    if (!countdown_ || countdown_() < 0) {
        return false;  // no timer running
    }
    DisplayLockGuard lock(this);
    countdown_selected_ = true;
    stopwatch_selected_ = false;
    hold_active_ = false;
    ESP_LOGI(TAG, "Showing the countdown");
    return true;
}

void RoboEyesDisplay::ClearCountdown() {
    DisplayLockGuard lock(this);
    countdown_selected_ = false;
}

bool RoboEyesDisplay::ShowStopwatch() {
    if (!stopwatch_ || stopwatch_() < 0) {
        return false;  // no stopwatch on
    }
    DisplayLockGuard lock(this);
    stopwatch_selected_ = true;
    countdown_selected_ = false;
    hold_active_ = false;
    ESP_LOGI(TAG, "Showing the stopwatch");
    return true;
}

// Stopwatch: minutes : seconds (hours : minutes from 100 minutes on). The colon is
// steady while it runs and blinks while it is paused.
void RoboEyesDisplay::UpdateStopwatchFace(int elapsed_s) {
    if (dot_clock_ == nullptr) return;
    auto two = [](int v) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", v % 100);
        return std::string(buf);
    };
    const bool running = stopwatch_running_ ? stopwatch_running_() : true;
    const bool colon = running || (lv_tick_get() / 500) % 2 == 0;
    const int minutes = elapsed_s / 60;
    if (minutes >= 100) {
        dot_clock_->Set(two(minutes / 60), two(minutes % 60), colon);
    } else {
        dot_clock_->Set(two(minutes), two(elapsed_s % 60), colon);
    }
}

void RoboEyesDisplay::SetDisplayTheme(DisplayTheme theme) {
    {
        DisplayLockGuard lock(this);
        theme_ = theme;
        ApplyThemeLocked();
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kThemeKey, theme == DisplayTheme::Light ? "light" : "dark");
    ESP_LOGI(TAG, "Theme set to %s", theme == DisplayTheme::Light ? "light" : "dark");
}

// Priority: setup screens, a ringing alarm, a requested expression, the countdown
// (when chosen, while a timer runs), then the main mode. Only the user (or a new
// short timer) changes which one is chosen.
DisplayScreen RoboEyesDisplay::ResolveScreen(DeviceState state, int countdown_s,
                                             int stopwatch_s) const {
    if (IsSetupState(state)) return DisplayScreen::Setup;
    if (alarm_active_) return DisplayScreen::Alarm;
    if (hold_active_) return DisplayScreen::Expression;
    if (countdown_selected_ && countdown_s >= 0) return DisplayScreen::Countdown;
    if (stopwatch_selected_ && stopwatch_s >= 0) return DisplayScreen::Stopwatch;
    return main_mode_ == MainDisplayMode::Time ? DisplayScreen::Clock : DisplayScreen::Eyes;
}

// Swaps between the eyes and the dot-matrix face (clock / countdown).
void RoboEyesDisplay::ShowScreen(DisplayScreen screen) {
    if (screen == screen_ || eyes_ == nullptr || dot_clock_ == nullptr) {
        screen_ = screen;
        return;
    }
    auto is_dots = [](DisplayScreen s) {
        return s == DisplayScreen::Clock || s == DisplayScreen::Countdown ||
               s == DisplayScreen::Stopwatch;
    };
    const bool dots = is_dots(screen);
    const bool had_dots = is_dots(screen_);
    screen_ = screen;
    if (dots == had_dots) return;
    if (dots) {
        lv_obj_remove_flag(dot_clock_->obj(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eyes_->obj(), LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dot_clock_->obj(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eyes_->obj(), LV_OBJ_FLAG_HIDDEN);
    }
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
    const DeviceState state = Application::GetInstance().GetDeviceState();
    const uint32_t now = lv_tick_get();
    if (static_cast<int>(state) != last_state_) {
        last_state_ = static_cast<int>(state);
        state_since_ms_ = now;
        idle_ms_ = 0;
    }
    if (hold_active_ && static_cast<int32_t>(hold_until_ms_ - now) <= 0) {
        hold_active_ = false;
        eyes_->SetMood(RoboEyes::Mood::Focused);  // back to the normal eyes
    }

    const int countdown_s = countdown_ ? countdown_() : -1;
    if (countdown_s < 0) {
        countdown_selected_ = false;  // no timer left: back to the main mode for good
    }
    const int stopwatch_s = stopwatch_ ? stopwatch_() : -1;
    if (stopwatch_s < 0) {
        stopwatch_selected_ = false;  // stopwatch ended: back to the main mode
    }
    const DisplayScreen screen = ResolveScreen(state, countdown_s, stopwatch_s);
    ShowScreen(screen);

    // No text on the screen, ever: the status strip stays hidden (only the clock and
    // countdown digits are shown).
    if (status_bar_ != nullptr && !lv_obj_has_flag(status_bar_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen == DisplayScreen::Clock || screen == DisplayScreen::Countdown) {
        UpdateDotFace(screen == DisplayScreen::Clock, countdown_s);
        idle_ms_ = 0;
        return;  // the eyes are hidden: no need to animate them
    }
    if (screen == DisplayScreen::Stopwatch) {
        UpdateStopwatchFace(stopwatch_s);
        idle_ms_ = 0;
        return;
    }

    // Eyes: standby (idle for a moment) shows the relaxed standby eyes.
    const bool music_showing = music_active_ && state == kDeviceStateIdle;
    if (screen == DisplayScreen::Eyes && state == kDeviceStateIdle && !music_active_) {
        idle_ms_ = std::min<uint32_t>(idle_ms_ + elapsed_ms, kSleepAfterMs);
    } else {
        idle_ms_ = 0;
    }

    RoboEyes::Activity activity = RoboEyes::Activity::Idle;
    switch (screen) {
        case DisplayScreen::Alarm:
            activity = RoboEyes::Activity::Alarm;
            break;
        case DisplayScreen::Setup:
            activity = RoboEyes::Activity::Setup;
            break;
        case DisplayScreen::Expression:
            // The requested face stays, also while the reply is spoken.
            eyes_->SetMood(hold_mood_);
            activity = RoboEyes::Activity::Idle;
            break;
        default:
            if (music_showing) {
                activity = RoboEyes::Activity::Music;
            } else if (power_save_ || idle_ms_ >= kSleepAfterMs) {
                activity = RoboEyes::Activity::Sleeping;  // standby eyes
            } else if (state == kDeviceStateConnecting) {
                // Wake word heard (the connection opens): ready for the command.
                activity = RoboEyes::Activity::WakeHeard;
            } else if (state == kDeviceStateListening) {
                // A listening turn opens with the "ready" face, then shows that it
                // is listening. Waiting for the reply keeps the listening face: there
                // is no separate thinking face.
                activity = now - state_since_ms_ < kWakeFaceMs ? RoboEyes::Activity::WakeHeard
                                                                : RoboEyes::Activity::Listening;
            } else if (state == kDeviceStateSpeaking) {
                activity = RoboEyes::Activity::Speaking;
            }
            break;
    }
    eyes_->SetActivity(activity);
    eyes_->Tick(elapsed_ms);
}

void RoboEyesDisplay::UpdateDotFace(bool show_clock, int countdown_s) {
    if (dot_clock_ == nullptr) return;
    auto two = [](int v) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", v % 100);
        return std::string(buf);
    };
    if (!show_clock) {
        const bool blink_on = (lv_tick_get() / 500) % 2 == 0;
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
    // Time mode: always two-digit hours ("09 00", "12 30") and no colon.
    const int hour = use_24h ? local.tm_hour : (local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12);
    dot_clock_->Set(two(hour), two(local.tm_min), false);
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
    idle_ms_ = 0;
    if (eyes_ != nullptr) eyes_->SetMood(mood);
    return true;
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

// Reached from the common "self.screen.set_theme" tool ("light" / "dark").
void RoboEyesDisplay::SetTheme(Theme* theme) {
    if (theme == nullptr) return;
    {
        DisplayLockGuard lock(this);
        auto lvgl_theme = static_cast<LvglTheme*>(theme);
        lv_obj_set_style_text_font(lv_screen_active(), lvgl_theme->text_font()->font(), 0);
    }
    SetDisplayTheme(theme->name() == "light" ? DisplayTheme::Light : DisplayTheme::Dark);
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
