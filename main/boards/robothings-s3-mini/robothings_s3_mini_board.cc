// RoboThings S3 Mini: ESP32-S3 Super Mini Zero voice assistant with animated
// robot eyes on a 128x64 OLED, a lamp relay and voice-set alarms / timers.
#include "alarm_manager.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "clock_sync.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "music_player.h"
#include "robo_eyes_display.h"
#include "wake_word_switch.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "RoboThingsS3Mini"

// Lamp relay with voice tools; the alarm can also switch it on.
class RelayLamp {
public:
    explicit RelayLamp(gpio_num_t gpio) : gpio_(gpio) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        Set(false);

        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.lamp.get_state", "Get the power state of the lamp", PropertyList(),
                    [this](const PropertyList&) -> ToolResult {
                        return std::string(power_ ? "{\"power\": true}" : "{\"power\": false}");
                    });
        mcp.AddTool("self.lamp.turn_on", "Turn on the lamp", PropertyList(),
                    [this](const PropertyList&) -> ToolResult {
                        Set(true);
                        return true;
                    });
        mcp.AddTool("self.lamp.turn_off", "Turn off the lamp", PropertyList(),
                    [this](const PropertyList&) -> ToolResult {
                        Set(false);
                        return true;
                    });
    }

    void Set(bool on) {
        power_ = on;
        gpio_set_level(gpio_, on ? 1 : 0);
    }

private:
    gpio_num_t gpio_;
    bool power_ = false;
};

namespace {

// Lower-case ASCII, drop punctuation, collapse spaces, and remove a leading or
// trailing "alexa" so "Alexa, bye!" and "bye bye." both become plain phrases.
std::string NormalizeSpeech(const std::string& text) {
    std::string out;
    bool space = false;
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        // Devanagari danda "।" (E0 A5 A4) counts as punctuation.
        if (c == 0xE0 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA5 &&
            static_cast<unsigned char>(text[i + 2]) == 0xA4) {
            i += 2;
            space = true;
            continue;
        }
        if (c < 0x80 && !isalnum(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(c < 0x80 ? tolower(c) : c));
    }
    for (const char* name : {"alexa ", "hey alexa "}) {
        if (out.rfind(name, 0) == 0) out.erase(0, strlen(name));
    }
    const std::string tail = " alexa";
    if (out.size() > tail.size() && out.compare(out.size() - tail.size(), tail.size(), tail) == 0) {
        out.erase(out.size() - tail.size());
    }
    return out;
}

bool IsOneOf(const std::string& text, std::initializer_list<const char*> phrases) {
    for (const char* p : phrases) {
        if (text == p) return true;
    }
    return false;
}

}  // namespace

class RoboThingsS3MiniBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    RoboEyesDisplay* eyes_display_ = nullptr;
    RelayLamp* lamp_ = nullptr;
    esp_timer_handle_t standby_timer_ = nullptr;
    int64_t standby_deadline_us_ = 0;
    int64_t last_toggle_us_ = 0;

    // Ends the conversation right away, without the assistant replying.
    void GoStandby() {
        if (eyes_display_ != nullptr) eyes_display_->HoldExpression("sleeping", 3);
        if (standby_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback = [](void* arg) { static_cast<RoboThingsS3MiniBoard*>(arg)->StandbyTick(); },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "standby",
                .skip_unhandled_events = true,
            };
            esp_timer_create(&args, &standby_timer_);
        }
        standby_deadline_us_ = esp_timer_get_time() + 6LL * 1000000;
        last_toggle_us_ = 0;
        esp_timer_stop(standby_timer_);
        esp_timer_start_periodic(standby_timer_, 150 * 1000);
        StandbyTick();
    }

    void StandbyTick() {
        auto& app = Application::GetInstance();
        const DeviceState state = app.GetDeviceState();
        const int64_t now = esp_timer_get_time();
        if (state == kDeviceStateIdle || now > standby_deadline_us_) {
            esp_timer_stop(standby_timer_);
            return;
        }
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            app.GetAudioService().ResetDecoder();  // silence any reply right away
            if (now - last_toggle_us_ > 400 * 1000) {
                // speaking: abort the reply; listening: close the conversation
                app.ToggleChatState();
                last_toggle_us_ = now;
            }
        }
    }

    // Local voice shortcuts, handled on the device from the speech-to-text result so
    // they act at once (the AI does not get to answer).
    void HandleUserSpeech(const std::string& raw) {
        const std::string text = NormalizeSpeech(raw);
        if (IsOneOf(text, {"bye", "bye bye", "byebye", "ok bye", "okay bye", "bye bye alexa",
                           "chup raho", "chup raho ab", "ab chup raho", "chup ho jao", "chup",
                           "so jao", "soja", "so ja", "go to sleep", "shut up", "stop talking",
                           "बाय", "बाय बाय", "बाय-बाय", "चुप रहो", "चुप रहो अब", "अब चुप रहो",
                           "चुप हो जाओ", "सो जाओ", "सो जा"})) {
            ESP_LOGI(TAG, "Voice shortcut: standby (%s)", raw.c_str());
            GoStandby();
            return;
        }
        if (IsOneOf(text, {"show clock", "show the clock", "clock dikhao", "ghadi dikhao",
                           "time dikhao", "show time", "शो क्लॉक", "क्लॉक दिखाओ", "घड़ी दिखाओ",
                           "टाइम दिखाओ"})) {
            ESP_LOGI(TAG, "Voice shortcut: clock (%s)", raw.c_str());
            GoStandby();  // first: it shows sleeping eyes, which the clock then replaces
            if (eyes_display_ != nullptr) eyes_display_->ShowClock();
            return;
        }
    }
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags =
                {
                    .dc_low_on_data = 0,
                    .disable_control_phase = 0,
                },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        eyes_display_ = new RoboEyesDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = eyes_display_;
    }

    // While an alarm rings, any button press silences it instead of its normal action.
    static bool StopAlarmIfRinging() {
        auto& alarms = AlarmManager::GetInstance();
        if (!alarms.IsRinging()) {
            return false;
        }
        Application::GetInstance().Schedule([]() { AlarmManager::GetInstance().StopRinging(); });
        return true;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (StopAlarmIfRinging()) return;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            if (StopAlarmIfRinging()) return;
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() { Application::GetInstance().StopListening(); });

        volume_up_button_.OnClick([this]() {
            if (StopAlarmIfRinging()) return;
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            if (StopAlarmIfRinging()) return;
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeTools() {
        lamp_ = new RelayLamp(LAMP_GPIO);
        ClockSync::GetInstance().Initialize();  // keep the clock on IST
        RegisterWakeWordTools();

        auto& music = MusicPlayer::GetInstance();
        music.OnNowPlaying([this](const std::string& title) {
            Application::GetInstance().Schedule([this, title]() {
                if (eyes_display_ != nullptr) eyes_display_->SetNowPlaying(title);
            });
        });
        music.OnStopped([this]() {
            Application::GetInstance().Schedule([this]() {
                if (eyes_display_ != nullptr) eyes_display_->ClearNowPlaying();
            });
        });
        music.Initialize();

        auto& alarms = AlarmManager::GetInstance();
        alarms.OnRingStart([this](const AlarmManager::RingInfo& info) {
            MusicPlayer::GetInstance().Stop();  // the alarm takes over the speaker
            if (info.lamp && lamp_ != nullptr) {
                lamp_->Set(true);
            }
            if (eyes_display_ != nullptr) {
                std::string banner = info.title == "Timer" ? std::string("TIMER")
                                                           : "ALARM " + info.title;
                if (!info.label.empty()) banner += " " + info.label;
                eyes_display_->SetAlarmBanner(banner);
            }
        });
        alarms.OnRingStop([this]() {
            if (eyes_display_ != nullptr) {
                eyes_display_->ClearAlarmBanner();
            }
        });
        alarms.Initialize();

        if (eyes_display_ != nullptr) {
            eyes_display_->OnUserSpeech([this](const std::string& text) { HandleUserSpeech(text); });
            eyes_display_->SetCountdownProvider([]() { return AlarmManager::GetInstance().SecondsToNextTimer(); });
            eyes_display_->SetClockFormatProvider([]() { return ClockSync::GetInstance().use_24h(); });
        }

        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.screen.show_expression",
            "Show an expression on the device's eyes when the user asks for one, e.g. 'roo ke "
            "dikhao' -> crying, 'so jao' -> sleeping, 'gussa dikhao' -> angry. expression: one of "
            "happy, sad, crying, excited, thinking, speaking, sleeping, angry, surprised, wink, "
            "blush, loading, curious, tease, confused, in_love, shocked, annoyed, focused, "
            "happy_closed, sweet. seconds: how long to show it (default 10).",
            PropertyList({
                Property("expression", kPropertyTypeString).SetMaxLength(20),
                Property("seconds", kPropertyTypeInteger, 10, 2, 120),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                if (eyes_display_ == nullptr) return std::unexpected(std::string("No display"));
                std::string name = properties["expression"].value<std::string>();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                    return c == ' ' ? '_' : static_cast<char>(tolower(c));
                });
                if (!eyes_display_->HoldExpression(name, properties["seconds"].value<int>())) {
                    return std::unexpected(std::string("Unknown expression"));
                }
                return true;
            });
        mcp.AddTool(
            "self.screen.show_clock",
            "Replace the eyes with a big dot-matrix clock (hours | minutes) until the user talks "
            "again. Use for 'show clock', 'clock dikhao', 'ghadi dikhao'.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                if (eyes_display_ == nullptr) return std::unexpected(std::string("No display"));
                eyes_display_->ShowClock();
                return true;
            });
        mcp.AddTool(
            "self.assistant.standby",
            "End the conversation immediately and go to standby WITHOUT saying anything. Call it "
            "(and do not reply) when the user says bye, bye bye, chup raho, chup ho jao, so jao, "
            "stop talking or similar.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                Application::GetInstance().Schedule([this]() { GoStandby(); });
                return true;
            });
    }

public:
    RoboThingsS3MiniBoard()
        : boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO),
          volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                              AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(RoboThingsS3MiniBoard);
