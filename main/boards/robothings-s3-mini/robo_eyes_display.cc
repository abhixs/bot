#include "robo_eyes_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#include <algorithm>
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

    // Status text overlaps the top bar, centered (clock, state, subtitles).
    status_bar_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_bar_);
    lv_obj_set_size(status_bar_, LV_HOR_RES - 32, kStatusBarHeight);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_SCROLLABLE);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES - 32);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES - 32);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    // Eyes fill the rest of the screen.
    eyes_ = std::make_unique<RoboEyes>(screen, width_, height_ - kStatusBarHeight, kPixelOn,
                                       kPixelOff);
    lv_obj_align(eyes_->obj(), LV_ALIGN_TOP_LEFT, 0, kStatusBarHeight);
    lv_obj_move_to_index(eyes_->obj(), 0);  // keep the status bar drawn on top

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
    if (state == kDeviceStateIdle && !alarm_active_) {
        idle_ms_ = std::min<uint32_t>(idle_ms_ + elapsed_ms, kSleepAfterMs);
    } else {
        idle_ms_ = 0;
    }

    RoboEyes::Activity activity = RoboEyes::Activity::Idle;
    if (alarm_active_) {
        activity = RoboEyes::Activity::Alarm;
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
    eyes_->Tick(elapsed_ms);
}

void RoboEyesDisplay::SetStatus(const char* status) {
    if (alarm_active_) {
        return;  // the alarm banner stays pinned until it is cleared
    }
    LvglDisplay::SetStatus(status);
}

void RoboEyesDisplay::SetChatMessage(const char* role, const char* content) {
    // No room for a chat area: subtitles and system messages (like the
    // activation code) scroll through the status line instead.
    if (content == nullptr || content[0] == '\0' || alarm_active_) {
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
}

void RoboEyesDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
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
