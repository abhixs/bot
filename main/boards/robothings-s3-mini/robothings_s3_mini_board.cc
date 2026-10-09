// RoboThings S3 Mini: ESP32-S3 Super Mini Zero voice assistant with animated
// robot eyes on a 128x64 OLED, a lamp relay and voice-set alarms / timers.
#include "alarm_manager.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "clock_sync.h"
#include "config.h"
#include "device_action.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "music_player.h"
#include "pomodoro.h"
#include "popdown_sound.h"
#include "robo_audio_codec.h"
#include "robo_eyes_display.h"
#include "stopwatch.h"
#include "voice_shortcuts.h"
#include "wake_word_switch.h"
#include "wifi_board.h"

#include <cJSON.h>
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
                        NoteDeviceAction();
                        Set(true);
                        return true;
                    });
        mcp.AddTool("self.lamp.turn_off", "Turn off the lamp", PropertyList(),
                    [this](const PropertyList&) -> ToolResult {
                        NoteDeviceAction();
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

class RoboThingsS3MiniBoard;
static RoboThingsS3MiniBoard* g_board = nullptr;  // for NoteDeviceAction()

class RoboThingsS3MiniBoard : public WifiBoard {
private:
    // How GoStandby() ends a conversation.
    enum class StandbyStyle {
        Silent,     // mute at once: whatever reply is on its way is not heard
        Quiet,      // just close it (nothing to silence; lets the wake sound finish)
        EndBeep,    // Silent, then the short "done listening" beep (inactivity)
    };

    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    RoboEyesDisplay* eyes_display_ = nullptr;
    RelayLamp* lamp_ = nullptr;
    RoboAudioCodec* codec_ = nullptr;
    esp_timer_handle_t standby_timer_ = nullptr;
    int64_t idle_since_us_ = 0;
    int64_t standby_deadline_us_ = 0;
    int64_t last_toggle_us_ = 0;

    // WakeNet threshold while an alarm rings (0.4 - 0.9999, lower = more sensitive).
    static constexpr float kRingingWakeThreshold = 0.45f;
    // Timers shorter than this switch the screen to their countdown when set.
    static constexpr int kAutoCountdownSeconds = 31 * 60;
    // "Extend" without a number.
    static constexpr int kDefaultExtendSeconds = 10 * 60;
    // A "stop" / "extend" this soon after a ring stopped (e.g. by "Alexa") is about it.
    static constexpr int64_t kRecentRingUs = 60 * 1000000LL;
    // While a Pomodoro runs, each new phase shows its countdown unless the user
    // switched to another view.
    bool pomodoro_view_ = false;
    StandbyStyle standby_style_ = StandbyStyle::Silent;
    bool standby_requested_ = false;  // a standby we started (main task)

    // Conversation watcher (main task, every 50 ms): wake word sensitivity, the
    // inactivity ending and command mode.
    // WakeNet thresholds (the wn9_alexa model default is 0.64; lower = more sensitive).
    static constexpr float kSpeakingWakeThreshold = 0.45f;
    static constexpr float kStandbyWakeThreshold = 0.58f;
    esp_timer_handle_t conversation_timer_ = nullptr;
    DeviceState watched_state_ = kDeviceStateUnknown;
    float applied_wake_threshold_ = -1.0f;
    int aec_allowed_ = -1;  // last value sent (-1 = none yet)
    bool user_turn_seen_ = false;  // the user said something in this listening turn
    bool action_pending_ = false;  // a device action ran: close after its reply
    int64_t stopwatch_started_us_ = 0;
    int64_t pomodoro_started_us_ = 0;

    // Ends the conversation right away, without the assistant saying anything.
    // Silent / EndBeep keep the speaker muted until the device is back in standby.
    void GoStandby(StandbyStyle style = StandbyStyle::Silent) {
        standby_style_ = style;
        standby_requested_ = true;
        if (style != StandbyStyle::Quiet) {
            if (codec_ != nullptr) codec_->SetMuted(true);
            Application::GetInstance().GetAudioService().ResetDecoder();
        }
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
        idle_since_us_ = 0;
        esp_timer_stop(standby_timer_);
        esp_timer_start_periodic(standby_timer_, 150 * 1000);
        StandbyTick();
    }

    void StandbyTick() {
        auto& app = Application::GetInstance();
        const DeviceState state = app.GetDeviceState();
        const int64_t now = esp_timer_get_time();
        if (state == kDeviceStateIdle) {
            // Stay muted a moment longer so nothing of the cut reply slips out.
            if (idle_since_us_ == 0) idle_since_us_ = now;
            if (now - idle_since_us_ < 400 * 1000 && now <= standby_deadline_us_) return;
        }
        if (state == kDeviceStateIdle || now > standby_deadline_us_) {
            esp_timer_stop(standby_timer_);
            if (standby_style_ != StandbyStyle::Quiet) {
                app.GetAudioService().ResetDecoder();
                if (codec_ != nullptr) codec_->SetMuted(false);
            }
            if (standby_style_ == StandbyStyle::EndBeep && state == kDeviceStateIdle) {
                app.PlaySound(PopDownOgg());  // "done listening"
            }
            return;
        }
        idle_since_us_ = 0;
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            if (standby_style_ != StandbyStyle::Quiet) {
                app.GetAudioService().ResetDecoder();  // drop any reply right away
            }
            if (now - last_toggle_us_ > 300 * 1000) {
                // speaking: abort the reply; listening: close the conversation
                app.ToggleChatState();
                last_toggle_us_ = now;
            }
        }
    }

    // Local voice shortcuts (see voice_shortcuts.h), handled on the device from the
    // speech-to-text result so they act at once and the AI does not answer them.
    void HandleUserSpeech(const std::string& text) {
        user_turn_seen_ = true;
        const VoiceCommand command = ParseVoiceCommand(text);
        if (command.type == VoiceShortcut::None) return;
        if (!RunShortcut(command)) {
            return;  // does not apply right now (e.g. "stop" with nothing ringing): the AI answers
        }
        ESP_LOGI(TAG, "Voice shortcut: %s (%s)", VoiceShortcutName(command.type), text.c_str());
        // The command is done here: end the conversation silently.
        GoStandby();
    }

    // Carries out a local voice shortcut. Returns false when it does not apply now.
    bool RunShortcut(const VoiceCommand& command) {
        auto& alarms = AlarmManager::GetInstance();
        auto& pomodoro = Pomodoro::GetInstance();
        auto& stopwatch = Stopwatch::GetInstance();
        const bool ring_just_stopped =
            alarms.last_ring_stop_us() != 0 &&
            esp_timer_get_time() - alarms.last_ring_stop_us() < kRecentRingUs;
        switch (command.type) {
            case VoiceShortcut::Standby:
                return true;
            case VoiceShortcut::Sleep:
                if (eyes_display_ != nullptr) eyes_display_->HoldExpression("sleeping", 8);
                return true;
            case VoiceShortcut::TimeMode:
            case VoiceShortcut::EmotionMode:
            case VoiceShortcut::ToggleMode: {
                if (eyes_display_ == nullptr) return false;
                MainDisplayMode mode = command.type == VoiceShortcut::TimeMode ? MainDisplayMode::Time
                                       : command.type == VoiceShortcut::EmotionMode
                                           ? MainDisplayMode::Emotion
                                           : (eyes_display_->main_mode() == MainDisplayMode::Time
                                                  ? MainDisplayMode::Emotion
                                                  : MainDisplayMode::Time);
                eyes_display_->SetMainMode(mode);
                pomodoro_view_ = false;  // the user picked another view
                return true;
            }
            case VoiceShortcut::TimerMode:
                if (eyes_display_ == nullptr) return false;
                if (eyes_display_->ShowCountdown()) {
                    pomodoro_view_ = pomodoro.active();
                    return true;
                }
                return eyes_display_->ShowStopwatch();
            case VoiceShortcut::LightTheme:
            case VoiceShortcut::DarkTheme:
                if (eyes_display_ == nullptr) return false;
                eyes_display_->SetDisplayTheme(command.type == VoiceShortcut::LightTheme
                                                   ? DisplayTheme::Light
                                                   : DisplayTheme::Dark);
                return true;
            case VoiceShortcut::TimerSet:
                return StartTimer(command.seconds, "");
            case VoiceShortcut::Extend: {
                const int seconds = command.seconds > 0 ? command.seconds : kDefaultExtendSeconds;
                const bool was_ringing = alarms.IsRinging();
                if (pomodoro.active()) {
                    // The current period (focus or break), the cycle goes on.
                    if (was_ringing) alarms.StopRinging();
                    if (!pomodoro.Extend(seconds)) return false;
                    if (pomodoro_view_ && eyes_display_ != nullptr) eyes_display_->ShowCountdown();
                    return true;
                }
                if (was_ringing || ring_just_stopped) {
                    alarms.StopRinging();
                    return StartTimer(seconds, "Extension");  // like a snooze of that length
                }
                if (alarms.ExtendTimer(0, seconds)) return true;  // the running timer
                return false;
            }
            case VoiceShortcut::StopAlarm:
                if (alarms.IsRinging()) {
                    alarms.StopRinging();
                    return true;
                }
                if (alarms.CancelPendingRing()) return true;  // between the alarm's rings
                return ring_just_stopped;  // "Alexa" already silenced it: just "stop"
            case VoiceShortcut::StopTimer:
                if (stopwatch.Stop()) return true;
                if (alarms.IsRinging()) {
                    alarms.StopRinging();
                    return true;
                }
                return false;  // a running countdown: the AI cancels it
            case VoiceShortcut::PomodoroStart:
                pomodoro_view_ = true;
                pomodoro_started_us_ = esp_timer_get_time();
                pomodoro.Start(command.seconds > 0 ? command.seconds : Pomodoro::kFocusSeconds);
                return pomodoro.active();
            case VoiceShortcut::PomodoroStop:
                if (!pomodoro.active()) return false;
                pomodoro.Stop();
                return true;
            case VoiceShortcut::PomodoroPause:
                return pomodoro.Pause();
            case VoiceShortcut::PomodoroResume:
                return pomodoro.Resume();
            case VoiceShortcut::StopwatchStart:
                stopwatch.Start();
                stopwatch_started_us_ = esp_timer_get_time();
                if (eyes_display_ != nullptr) eyes_display_->ShowStopwatch();
                return true;
            case VoiceShortcut::StopwatchPause:
            case VoiceShortcut::StopwatchResume:
            case VoiceShortcut::StopwatchShow: {
                if (!stopwatch.active()) {
                    // Plain "pause" / "resume" with no stopwatch: the Pomodoro.
                    if (command.type == VoiceShortcut::StopwatchPause) return pomodoro.Pause();
                    if (command.type == VoiceShortcut::StopwatchResume) return pomodoro.Resume();
                    return false;
                }
                if (command.type == VoiceShortcut::StopwatchPause) stopwatch.Pause();
                if (command.type == VoiceShortcut::StopwatchResume) stopwatch.Resume();
                if (eyes_display_ != nullptr) eyes_display_->ShowStopwatch();
                return true;
            }
            case VoiceShortcut::StopwatchStop:
                return stopwatch.Stop();
            case VoiceShortcut::None:
                break;
        }
        return false;
    }

    // A countdown timer set on the device; a short one is shown at once.
    bool StartTimer(int seconds, const std::string& label) {
        if (seconds <= 0) return false;
        const int id = AlarmManager::GetInstance().AddTimer(seconds, label);
        if (id < 0) return false;
        if (eyes_display_ != nullptr && seconds < kAutoCountdownSeconds) {
            eyes_display_->ShowCountdown();
        }
        return true;
    }

    // A device action ran (MCP tool): end the conversation after its reply.
    friend void NoteDeviceAction();
    void MarkDeviceAction() { action_pending_ = true; }

    void StartConversationWatcher() {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<RoboThingsS3MiniBoard*>(arg);
                    Application::GetInstance().Schedule([self]() { self->WatchConversation(); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "conversation",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &conversation_timer_);
        esp_timer_start_periodic(conversation_timer_, 100 * 1000);
    }

    void WatchConversation() {
        auto& app = Application::GetInstance();
        auto& alarms = AlarmManager::GetInstance();
        const DeviceState state = app.GetDeviceState();

        // Echo cancellation only while the device itself plays something (reply, alarm,
        // music): it is the heaviest part of the audio front end, and with nothing
        // playing it only takes CPU from the wake word and the speech encoder.
        const bool playing = state == kDeviceStateSpeaking || alarms.IsRinging() ||
                             state == kDeviceStateNotifying ||
                             MusicPlayer::GetInstance().IsPlaying();
        if (static_cast<int>(playing) != aec_allowed_) {
            app.GetAudioService().AllowEchoCancellation(playing);
            aec_allowed_ = playing ? 1 : 0;
        }

        // Wake word sensitivity: highest while an alarm rings or the assistant speaks
        // (the speaker is next to the mic), a little above the model default in standby.
        const float threshold = alarms.IsRinging()               ? kRingingWakeThreshold
                                : state == kDeviceStateSpeaking ? kSpeakingWakeThreshold
                                                                : kStandbyWakeThreshold;
        if (threshold != applied_wake_threshold_) {
            app.GetAudioService().SetWakeWordThreshold(threshold);
            applied_wake_threshold_ = threshold;
        }

        if (state == watched_state_) return;
        const DeviceState previous = watched_state_;
        watched_state_ = state;
        const bool our_standby = standby_requested_;
        switch (state) {
            case kDeviceStateConnecting:
                user_turn_seen_ = false;
                action_pending_ = false;
                break;
            case kDeviceStateListening:
                if (previous == kDeviceStateSpeaking && action_pending_ && !our_standby) {
                    // Command mode: the reply to a device action is over.
                    ESP_LOGI(TAG, "Device action done: back to standby");
                    action_pending_ = false;
                    GoStandby(StandbyStyle::Quiet);
                    break;
                }
                user_turn_seen_ = false;  // a new listening turn
                break;
            case kDeviceStateSpeaking:
                if (previous == kDeviceStateListening && !user_turn_seen_ && !our_standby) {
                    // The server speaks although the user said nothing: its goodbye after
                    // the inactivity timeout. Silence it and just beep.
                    ESP_LOGI(TAG, "Listening timed out: ending with the beep");
                    GoStandby(StandbyStyle::EndBeep);
                }
                break;
            case kDeviceStateIdle:
                if (previous == kDeviceStateListening && !user_turn_seen_ && !our_standby &&
                    !alarms.IsRinging()) {
                    // The listening ended with nothing said (server timeout, no goodbye).
                    app.PlaySound(PopDownOgg());
                }
                standby_requested_ = false;
                action_pending_ = false;
                break;
            default:
                break;
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
        // Keep the words said right after the wake word (while the connection opens)
        // and send them first, so the server hears the whole sentence.
        Application::GetInstance().GetAudioService().EnableWakeWordSpeechBridge(true);
        // The wake sound plays the moment "Alexa" is heard, not after the server
        // connection is up.
        Application::GetInstance().SetWakeSoundOnDetect(true);
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
            Pomodoro::GetInstance().HandleRing(info);
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
        alarms.OnTimerSet([this](int seconds) {
            // A short timer (under 31 minutes, e.g. a Pomodoro) shows its countdown at once.
            if (eyes_display_ != nullptr && seconds < kAutoCountdownSeconds) {
                eyes_display_->ShowCountdown();
            }
        });
        alarms.OnRingAnswered([this](const AlarmManager::RingInfo& info) {
            // "Alexa" over an alarm: it stops and the device goes back to standby (a
            // timer / Pomodoro keeps listening, e.g. for "extend 10 minutes").
            if (!info.is_timer) GoStandby(StandbyStyle::Quiet);
        });
        alarms.Initialize();

        auto& pomodoro = Pomodoro::GetInstance();
        pomodoro.OnPhaseStart([this](Pomodoro::Phase) {
            if (pomodoro_view_ && eyes_display_ != nullptr) eyes_display_->ShowCountdown();
        });
        pomodoro.OnEnd([this]() {
            pomodoro_view_ = false;
            if (eyes_display_ != nullptr) eyes_display_->ClearCountdown();
        });
        pomodoro.Initialize();
        pomodoro_view_ = pomodoro.active();
        if (pomodoro_view_ && eyes_display_ != nullptr) eyes_display_->ShowCountdown();

        if (eyes_display_ != nullptr) {
            eyes_display_->OnUserSpeech([this](const std::string& text) { HandleUserSpeech(text); });
            eyes_display_->SetCountdownProvider([]() {
                const int seconds = AlarmManager::GetInstance().SecondsToNextTimer();
                return seconds >= 0 ? seconds : Pomodoro::GetInstance().paused_remaining();
            });
            eyes_display_->SetCountdownPausedProvider([]() {
                return Pomodoro::GetInstance().paused() &&
                       AlarmManager::GetInstance().SecondsToNextTimer() < 0;
            });
            eyes_display_->SetClockFormatProvider([]() { return ClockSync::GetInstance().use_24h(); });
            eyes_display_->SetStopwatchProvider(
                []() { return Stopwatch::GetInstance().ElapsedSeconds(); },
                []() { return Stopwatch::GetInstance().running(); });
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
                NoteDeviceAction();
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
            "self.screen.set_mode",
            "Choose what the screen shows. Main modes (saved, kept after a restart): 'time' = "
            "big clock, also while talking; 'emotion' = animated eyes; 'toggle' = the other "
            "main mode. 'timer' = the countdown of the shortest running timer (until another "
            "mode is chosen). Use when the user asks to show the time / clock, the face / "
            "emotions, the timer / countdown, or to change the mode. (Light / dark screen: "
            "self.screen.set_theme.)",
            PropertyList({Property("mode", kPropertyTypeString).SetMaxLength(10)}),
            [this](const PropertyList& properties) -> ToolResult {
                NoteDeviceAction();
                if (eyes_display_ == nullptr) return std::unexpected(std::string("No display"));
                std::string mode = properties["mode"].value<std::string>();
                std::transform(mode.begin(), mode.end(), mode.begin(),
                               [](unsigned char c) { return static_cast<char>(tolower(c)); });
                MainDisplayMode target;
                if (mode == "time" || mode == "clock") {
                    target = MainDisplayMode::Time;
                } else if (mode == "emotion" || mode == "emotions" || mode == "face") {
                    target = MainDisplayMode::Emotion;
                } else if (mode == "timer" || mode == "countdown") {
                    if (!eyes_display_->ShowCountdown()) {
                        return std::unexpected(std::string("No timer is running."));
                    }
                    return std::string("{\"mode\":\"timer\"}");
                } else if (mode == "toggle") {
                    target = eyes_display_->main_mode() == MainDisplayMode::Time
                                 ? MainDisplayMode::Emotion
                                 : MainDisplayMode::Time;
                } else {
                    return std::unexpected(
                        std::string("mode must be time, emotion, timer or toggle"));
                }
                eyes_display_->SetMainMode(target);
                pomodoro_view_ = false;
                return std::string(target == MainDisplayMode::Time ? "{\"mode\":\"time\"}"
                                                                   : "{\"mode\":\"emotion\"}");
            });
        mcp.AddTool(
            "self.pomodoro.start",
            "Start a continuous Pomodoro session: focus (default 25 min) -> break (5 min) -> "
            "focus -> ... until stopped. The countdown shows on the screen. Use for 'set "
            "pomodoro timer', 'start pomodoro'. Do not also call self.alarm.set_timer.",
            PropertyList({Property("focus_minutes", kPropertyTypeInteger, 25, 1, 120)}),
            [this](const PropertyList& properties) -> ToolResult {
                NoteDeviceAction();
                auto& pomodoro = Pomodoro::GetInstance();
                if (pomodoro.active() && esp_timer_get_time() - pomodoro_started_us_ < 15000000LL) {
                    return std::string("{\"success\":true,\"note\":\"already started\"}");
                }
                pomodoro_view_ = true;
                pomodoro_started_us_ = esp_timer_get_time();
                pomodoro.Start(properties["focus_minutes"].value<int>() * 60);
                return pomodoro.active();
            });
        mcp.AddTool(
            "self.pomodoro.extend",
            "Extend the current Pomodoro period (focus or break, whichever runs) by the given "
            "minutes, e.g. 'extend 10 minutes'. The cycle goes on afterwards.",
            PropertyList({Property("minutes", kPropertyTypeInteger, 10, 1, 120)}),
            [this](const PropertyList& properties) -> ToolResult {
                NoteDeviceAction();
                if (!Pomodoro::GetInstance().Extend(properties["minutes"].value<int>() * 60)) {
                    return std::unexpected(std::string("No Pomodoro is running."));
                }
                if (pomodoro_view_ && eyes_display_ != nullptr) eyes_display_->ShowCountdown();
                return true;
            });
        mcp.AddTool("self.pomodoro.pause",
                    "Pause the running Pomodoro period; it keeps its remaining time.",
                    PropertyList(), [](const PropertyList&) -> ToolResult {
                        NoteDeviceAction();
                        if (!Pomodoro::GetInstance().Pause()) {
                            return std::unexpected(std::string("No running Pomodoro to pause."));
                        }
                        return true;
                    });
        mcp.AddTool("self.pomodoro.resume",
                    "Resume (continue) a paused Pomodoro from where it was paused.",
                    PropertyList(), [](const PropertyList&) -> ToolResult {
                        NoteDeviceAction();
                        if (!Pomodoro::GetInstance().Resume()) {
                            return std::unexpected(std::string("No paused Pomodoro."));
                        }
                        return true;
                    });
        mcp.AddTool("self.pomodoro.stop", "End the Pomodoro session completely ('stop pomodoro').",
                    PropertyList(), [](const PropertyList&) -> ToolResult {
                        NoteDeviceAction();
                        if (!Pomodoro::GetInstance().active()) {
                            return std::string("{\"success\":true,\"note\":\"no pomodoro running\"}");
                        }
                        Pomodoro::GetInstance().Stop();
                        return true;
                    });
        mcp.AddTool(
            "self.stopwatch.control",
            "Stopwatch that counts UP (not a countdown timer). action: 'start' (from 00:00), "
            "'pause', 'resume', 'stop' (ends it and clears it from the screen) or 'show'. Use "
            "for 'start stopwatch', 'pause stopwatch', 'end stopwatch'.",
            PropertyList({Property("action", kPropertyTypeString).SetMaxLength(10)}),
            [this](const PropertyList& properties) -> ToolResult {
                NoteDeviceAction();
                auto& stopwatch = Stopwatch::GetInstance();
                const std::string action = properties["action"].value<std::string>();
                if (action == "start") {
                    if (!(stopwatch.running() &&
                          esp_timer_get_time() - stopwatch_started_us_ < 15000000LL)) {
                        stopwatch.Start();  // (not again if just started from the same request)
                        stopwatch_started_us_ = esp_timer_get_time();
                    }
                } else if (action == "pause") {
                    stopwatch.Pause();
                } else if (action == "resume") {
                    stopwatch.Resume();
                } else if (action == "stop") {
                    stopwatch.Stop();
                    return true;
                } else if (action != "show") {
                    return std::unexpected(std::string("action: start, pause, resume, stop or show"));
                }
                if (!stopwatch.active()) return std::unexpected(std::string("No stopwatch is on."));
                if (eyes_display_ != nullptr) eyes_display_->ShowStopwatch();
                cJSON* result = cJSON_CreateObject();
                cJSON_AddNumberToObject(result, "elapsed_seconds", stopwatch.ElapsedSeconds());
                cJSON_AddBoolToObject(result, "running", stopwatch.running());
                return result;
            });
        mcp.AddTool(
            "self.assistant.standby",
            "End the conversation immediately and go to standby WITHOUT saying anything. Call it "
            "(and do not reply at all) when the user says bye, bye bye, goodbye, chup raho, chup "
            "ho jao, so jao, stop talking or similar. Device style rule: keep replies short; if "
            "you did not understand the user, answer only 'Sorry, nahi samajh paayi.' and "
            "nothing else.",
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
        g_board = this;
        StartConversationWatcher();
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static RoboAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        codec_ = &audio_codec;
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }
};

void NoteDeviceAction() {
    if (g_board != nullptr) g_board->MarkDeviceAction();
}

DECLARE_BOARD(RoboThingsS3MiniBoard);
