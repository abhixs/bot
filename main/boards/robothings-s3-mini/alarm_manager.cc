#include "alarm_manager.h"

#include "alarm_schedule.h"
#include "alarm_tone.h"
#include "device_action.h"
#include "application.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>

#define TAG "AlarmManager"

namespace {
constexpr const char* kNvsNamespace = "alarms";
constexpr const char* kNvsKey = "list";
// The beep (about 0.9 s) repeats every 3 s. The echo canceller removes most of the
// beep from the microphone signal; the quiet gaps between beeps keep "Alexa"
// recognisable even when some echo is left. The state is checked every 250 ms so a
// wake word stops the ring at once.
constexpr int kRingTickMs = 250;
constexpr int kRingPeriodMs = 3000;

std::string TwoDigitTime(int hour, int minute) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return buf;
}
}  // namespace

AlarmManager::~AlarmManager() {
    if (check_timer_ != nullptr) {
        esp_timer_stop(check_timer_);
        esp_timer_delete(check_timer_);
    }
    if (ring_timer_ != nullptr) {
        esp_timer_stop(ring_timer_);
        esp_timer_delete(ring_timer_);
    }
}

void AlarmManager::Initialize() {
    Load();
    RegisterTools();

    esp_timer_create_args_t check_args = {
        .callback = [](void* arg) { static_cast<AlarmManager*>(arg)->CheckAlarms(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_check",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&check_args, &check_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(check_timer_, 1000 * 1000));

    esp_timer_create_args_t ring_args = {
        .callback = [](void* arg) { static_cast<AlarmManager*>(arg)->RingTick(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_ring",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&ring_args, &ring_timer_));
    ESP_LOGI(TAG, "Loaded %d alarm(s)", static_cast<int>(alarms_.size()));
}

void AlarmManager::Load() {
    Settings settings(kNvsNamespace, false);
    next_id_ = std::max(1, static_cast<int>(settings.GetInt("next_id", 1)));
    std::string json = settings.GetString(kNvsKey, "[]");
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    alarms_.clear();
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        cJSON* id = cJSON_GetObjectItem(item, "i");
        cJSON* hour = cJSON_GetObjectItem(item, "h");
        cJSON* minute = cJSON_GetObjectItem(item, "m");
        cJSON* days = cJSON_GetObjectItem(item, "d");
        cJSON* fire_at = cJSON_GetObjectItem(item, "f");
        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) ||
            !cJSON_IsNumber(days) || !cJSON_IsNumber(fire_at)) {
            continue;
        }
        Alarm alarm;
        alarm.id = id->valueint;
        alarm.hour = std::clamp(hour->valueint, 0, 23);
        alarm.minute = std::clamp(minute->valueint, 0, 59);
        alarm.days = static_cast<uint8_t>(days->valueint & alarm_schedule::kEveryDay);
        alarm.fire_at = static_cast<int64_t>(fire_at->valuedouble);
        alarm.is_timer = cJSON_IsTrue(cJSON_GetObjectItem(item, "t"));
        alarm.lamp = cJSON_IsTrue(cJSON_GetObjectItem(item, "l"));
        cJSON* created = cJSON_GetObjectItem(item, "c");
        if (cJSON_IsNumber(created)) alarm.created_at = static_cast<int64_t>(created->valuedouble);
        cJSON* label = cJSON_GetObjectItem(item, "n");
        if (cJSON_IsString(label)) {
            alarm.label = label->valuestring;
        }
        if (static_cast<int>(alarms_.size()) < kMaxAlarms) {
            alarms_.push_back(alarm);
        }
    }
    cJSON_Delete(root);
}

void AlarmManager::SaveLocked() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& alarm : alarms_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "i", alarm.id);
        cJSON_AddNumberToObject(item, "h", alarm.hour);
        cJSON_AddNumberToObject(item, "m", alarm.minute);
        cJSON_AddNumberToObject(item, "d", alarm.days);
        cJSON_AddNumberToObject(item, "f", static_cast<double>(alarm.fire_at));
        cJSON_AddBoolToObject(item, "t", alarm.is_timer);
        cJSON_AddBoolToObject(item, "l", alarm.lamp);
        cJSON_AddStringToObject(item, "n", alarm.label.c_str());
        cJSON_AddNumberToObject(item, "c", static_cast<double>(alarm.created_at));
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        Settings settings(kNvsNamespace, true);
        settings.SetString(kNvsKey, json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

int AlarmManager::NextIdLocked() {
    int id = next_id_;
    for (const auto& alarm : alarms_) {
        id = std::max(id, alarm.id + 1);
    }
    if (id > 1000000) id = 1;  // practically never
    next_id_ = id + 1;
    Settings settings(kNvsNamespace, true);
    settings.SetInt("next_id", next_id_);
    return id;
}

std::string AlarmManager::ListJson() {
    time_t now = time(nullptr);
    cJSON* root = cJSON_CreateObject();
    if (alarm_schedule::IsTimeValid(now)) {
        cJSON_AddStringToObject(root, "device_time", alarm_schedule::FormatDateTime(now).c_str());
    } else {
        cJSON_AddStringToObject(root, "device_time", "unknown (clock not synced yet)");
    }
    cJSON_AddBoolToObject(root, "ringing", ringing_);
    cJSON* list = cJSON_AddArrayToObject(root, "alarms");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& alarm : alarms_) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", alarm.id);
            cJSON_AddStringToObject(item, "type", alarm.is_timer ? "timer" : "alarm");
            if (!alarm.is_timer) {
                cJSON_AddStringToObject(item, "time", TwoDigitTime(alarm.hour, alarm.minute).c_str());
                cJSON_AddStringToObject(item, "repeat",
                                        alarm_schedule::RepeatToString(alarm.days).c_str());
            }
            cJSON_AddStringToObject(
                item, "next_ring",
                alarm_schedule::FormatDateTime(static_cast<time_t>(alarm.fire_at)).c_str());
            if (alarm.is_timer && alarm_schedule::IsTimeValid(now)) {
                cJSON_AddNumberToObject(item, "seconds_left",
                                        std::max<int64_t>(0, alarm.fire_at - now));
            }
            cJSON_AddStringToObject(item, "label", alarm.label.c_str());
            cJSON_AddBoolToObject(item, "turn_on_lamp", alarm.lamp);
            cJSON_AddItemToArray(list, item);
        }
    }
    char* json = cJSON_PrintUnformatted(root);
    std::string result = json != nullptr ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

void AlarmManager::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.alarm.list",
        "List all alarms and timers on the device, and get the device's current local date and "
        "time. Call this first when the user asks what alarms are set, or before cancelling one.",
        PropertyList(), [this](const PropertyList& properties) -> ToolResult {
            return ListJson();
        });

    mcp.AddTool(
        "self.alarm.set_alarm",
        "Set an alarm on the device for a clock time (24-hour, device local time). The device "
        "rings and animates even without internet. Use for requests like 'wake me up at 7', "
        "'alarm at 6:30 every weekday'.\n"
        "repeat: 'once' (default, next occurrence of that time), 'daily', 'weekdays', "
        "'weekends', or a comma list of days such as 'mon,wed,fri'.\n"
        "turn_on_lamp: also switch the lamp relay on when the alarm rings.",
        PropertyList({
            Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 0, 59),
            Property("repeat", kPropertyTypeString, std::string("once")),
            Property("label", kPropertyTypeString, std::string("")).SetMaxLength(40),
            Property("turn_on_lamp", kPropertyTypeBoolean, false),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            time_t now = time(nullptr);
            if (!alarm_schedule::IsTimeValid(now)) {
                return std::unexpected(
                    std::string("Device clock is not synced yet; try again in a moment."));
            }
            uint8_t days = 0;
            if (!alarm_schedule::ParseRepeat(properties["repeat"].value<std::string>(), days)) {
                return std::unexpected(std::string(
                    "Invalid repeat. Use once, daily, weekdays, weekends or e.g. mon,wed,fri."));
            }
            Alarm alarm;
            alarm.hour = properties["hour"].value<int>();
            alarm.minute = properties["minute"].value<int>();
            alarm.days = days;
            alarm.label = properties["label"].value<std::string>();
            alarm.lamp = properties["turn_on_lamp"].value<bool>();
            alarm.fire_at = alarm_schedule::NextOccurrence(now, alarm.hour, alarm.minute, days);
            alarm.created_at = static_cast<int64_t>(now);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (static_cast<int>(alarms_.size()) >= kMaxAlarms) {
                    return std::unexpected(std::string(
                        "Too many alarms (max 10). Cancel one with self.alarm.cancel first."));
                }
                alarm.id = NextIdLocked();
                alarms_.push_back(alarm);
                SaveLocked();
            }
            ESP_LOGI(TAG, "Alarm %d set for %02d:%02d (%s)", alarm.id, alarm.hour, alarm.minute,
                     alarm_schedule::RepeatToString(days).c_str());
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "id", alarm.id);
            cJSON_AddStringToObject(
                result, "next_ring",
                alarm_schedule::FormatDateTime(static_cast<time_t>(alarm.fire_at)).c_str());
            cJSON_AddStringToObject(result, "repeat",
                                    alarm_schedule::RepeatToString(days).c_str());
            return result;
        });

    mcp.AddTool(
        "self.alarm.set_timer",
        "Start a countdown timer on the device, e.g. 'set a 10 minute timer', 'remind me in "
        "30 seconds'. (For a Pomodoro use self.pomodoro.start; for counting up use "
        "self.stopwatch.control.) Give the duration as minutes and/or seconds. A timer under "
        "31 minutes shows its countdown on the screen at once.",
        PropertyList({
            Property("minutes", kPropertyTypeInteger, 0, 0, 1440),
            Property("seconds", kPropertyTypeInteger, 0, 0, 3600),
            Property("label", kPropertyTypeString, std::string("")).SetMaxLength(40),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            time_t now = time(nullptr);
            if (!alarm_schedule::IsTimeValid(now)) {
                return std::unexpected(
                    std::string("Device clock is not synced yet; try again in a moment."));
            }
            int total = properties["minutes"].value<int>() * 60 + properties["seconds"].value<int>();
            if (total < 5) {
                return std::unexpected(std::string("Timer must be at least 5 seconds."));
            }
            if (total == last_added_seconds_ && esp_timer_get_time() - last_added_us_ < 15000000LL &&
                HasTimer(last_added_id_)) {
                // Already set on the device from the same request.
                return std::string("{\"success\":true,\"note\":\"timer already running\"}");
            }
            Alarm alarm;
            alarm.is_timer = true;
            alarm.fire_at = static_cast<int64_t>(now) + total;
            alarm.created_at = static_cast<int64_t>(now);
            alarm.label = properties["label"].value<std::string>();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (static_cast<int>(alarms_.size()) >= kMaxAlarms) {
                    return std::unexpected(std::string(
                        "Too many alarms (max 10). Cancel one with self.alarm.cancel first."));
                }
                alarm.id = NextIdLocked();
                alarms_.push_back(alarm);
                SaveLocked();
            }
            ESP_LOGI(TAG, "Timer %d set for %d s", alarm.id, total);
            if (on_timer_set_) {
                on_timer_set_(total);
            }
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "id", alarm.id);
            cJSON_AddNumberToObject(result, "seconds", total);
            return result;
        });

    mcp.AddTool(
        "self.alarm.cancel",
        "Cancel (delete) an alarm or timer by its id from self.alarm.list. Use id -1 to cancel "
        "all alarms and timers.",
        PropertyList({
            Property("id", kPropertyTypeInteger, -1, 100000),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            int id = properties["id"].value<int>();
            int removed = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto before = alarms_.size();
                if (id == -1) {
                    alarms_.clear();
                } else {
                    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                                                 [id](const Alarm& a) { return a.id == id; }),
                                  alarms_.end());
                }
                removed = static_cast<int>(before - alarms_.size());
                if (removed > 0) {
                    SaveLocked();
                }
            }
            if (removed == 0) {
                return std::unexpected(std::string("No alarm with that id. Call self.alarm.list."));
            }
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "removed", removed);
            return result;
        });

    mcp.AddTool(
        "self.alarm.stop",
        "Stop the alarm that is ringing right now. Set snooze_minutes (1-30) to ring again later "
        "instead of stopping for good.",
        PropertyList({
            Property("snooze_minutes", kPropertyTypeInteger, 0, 0, 30),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            if (!ringing_) {
                return std::string("{\"success\":true,\"note\":\"no alarm is ringing\"}");
            }
            int snooze = properties["snooze_minutes"].value<int>();
            Application::GetInstance().Schedule([this, snooze]() { StopRinging(snooze); });
            return true;
        });
}

int AlarmManager::SecondsToNextTimer() {
    time_t now = time(nullptr);
    if (!alarm_schedule::IsTimeValid(now)) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t best = -1;
    for (const auto& alarm : alarms_) {
        if (!alarm.is_timer) continue;
        int64_t left = std::max<int64_t>(0, alarm.fire_at - static_cast<int64_t>(now));
        if (best < 0 || left < best) best = left;
    }
    return static_cast<int>(best);
}

void AlarmManager::CheckAlarms() {
    time_t now = time(nullptr);
    if (!alarm_schedule::IsTimeValid(now)) {
        return;
    }

    bool changed = false;
    bool have_ring = false;
    RingInfo ring;
    int attempt = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = alarms_.begin(); it != alarms_.end();) {
            if (it->fire_at > static_cast<int64_t>(now)) {
                ++it;
                continue;
            }
            bool on_time = static_cast<int64_t>(now) - it->fire_at <= kMissedGraceSeconds;
            if (on_time && !have_ring) {
                have_ring = true;
                ring.title = it->is_timer ? std::string("Timer")
                                          : TwoDigitTime(it->hour, it->minute);
                ring.label = it->label;
                ring.lamp = it->lamp;
                ring.id = it->id;
                ring.is_timer = it->is_timer;
                const bool pomodoro = it->label.rfind("Pomodoro", 0) == 0;
                ring.snooze_ok = !pomodoro &&
                                 ((!it->is_timer && it->days != 0) ||
                                  (it->created_at == 0 && !it->is_timer) ||
                                  (it->created_at != 0 &&
                                   it->fire_at - it->created_at >= kSnoozeMinLeadSeconds));
                if (it->id == retry_timer_id_ && retry_timer_id_ != 0) {
                    // The snooze of an unanswered alarm: ring as that alarm again.
                    ring = retry_info_;
                    ring.id = it->id;
                    attempt = retry_attempt_;
                    retry_timer_id_ = 0;
                }
            } else if (!on_time) {
                ESP_LOGW(TAG, "Alarm %d missed (device was off or clock jumped)", it->id);
            }
            changed = true;
            if (!it->is_timer && it->days != 0) {
                it->fire_at = alarm_schedule::NextOccurrence(now, it->hour, it->minute, it->days);
                ++it;
            } else {
                it = alarms_.erase(it);
            }
        }
        if (changed) {
            SaveLocked();
        }
    }

    if (have_ring) {
        ESP_LOGI(TAG, "Ringing: %s %s (ring %d)", ring.title.c_str(), ring.label.c_str(), attempt);
        Application::GetInstance().Schedule(
            [this, ring, attempt]() { StartRinging(ring, attempt); });
    }
}

void AlarmManager::StartRinging(const RingInfo& info, int attempt) {
    current_ring_ = info;
    ring_attempt_ = attempt;
    if (!ringing_) {
        ringing_ = true;
        if (on_ring_start_) {
            on_ring_start_(info);
        }
    }
    ring_elapsed_ms_ = 0;
    next_tone_ms_ = 0;
    next_toggle_ms_ = 0;
    idle_seen_while_ringing_ = false;
    esp_timer_stop(ring_timer_);
    esp_timer_start_periodic(ring_timer_, kRingTickMs * 1000);
    RingTick();
}

void AlarmManager::RingTick() {
    if (!ringing_) {
        return;
    }
    if (ring_elapsed_ms_ >= kRingSeconds * 1000) {
        Application::GetInstance().Schedule([this]() { OnRingTimeout(); });
        return;
    }
    const int elapsed = ring_elapsed_ms_;
    ring_elapsed_ms_ += kRingTickMs;
    auto& app = Application::GetInstance();
    app.Schedule([this, &app, elapsed]() {
        if (!ringing_) return;
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle:
                idle_seen_while_ringing_ = true;
                if (elapsed >= next_tone_ms_) {
                    next_tone_ms_ = elapsed + kRingPeriodMs;
                    app.PlaySound(AlarmToneOgg());
                }
                break;
            case kDeviceStateListening:
            case kDeviceStateSpeaking:
            case kDeviceStateConnecting:
                if (idle_seen_while_ringing_) {
                    // The device was idle and ringing, then someone said the wake word
                    // (or pressed talk): that means "I'm awake", so stop the alarm.
                    RingInfo info = current_ring_;
                    StopRinging(0);
                    if (on_ring_answered_) {
                        on_ring_answered_(info);
                    }
                } else if (app.GetDeviceState() != kDeviceStateConnecting &&
                           elapsed >= next_toggle_ms_) {
                    // A conversation left over from setting the alarm: end it so the
                    // alarm can be heard (not too often: a late toggle on an idle
                    // device would open a new conversation).
                    next_toggle_ms_ = elapsed + 1500;
                    app.ToggleChatState();
                }
                break;
            default:
                break;
        }
    });
}

// Nobody answered: stop, and for an alarm (not a timer) set a 10-minute snooze
// timer (on screen like any timer) that rings as the alarm again, up to kAlarmRings
// rings in all.
void AlarmManager::OnRingTimeout() {
    if (!ringing_) {
        return;
    }
    const RingInfo info = current_ring_;
    const int attempt = ring_attempt_;
    StopRinging(0);
    if (!info.snooze_ok || attempt >= kAlarmRings) {
        ESP_LOGI(TAG, "Ring ended without an answer");
        return;
    }
    const int id = AddTimer(kAlarmRetrySeconds, "Snooze");
    if (id < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retry_timer_id_ = id;
        retry_info_ = info;
        retry_attempt_ = attempt + 1;
    }
    ESP_LOGI(TAG, "No answer: snooze %d min, then ring %d of %d", kAlarmRetrySeconds / 60,
             attempt + 1, kAlarmRings);
    if (on_timer_set_) {
        on_timer_set_(kAlarmRetrySeconds);  // shows the snooze countdown
    }
}

void AlarmManager::StopRinging(int snooze_minutes) {
    if (!ringing_) {
        return;
    }
    ringing_ = false;
    last_ring_stop_us_ = esp_timer_get_time();
    esp_timer_stop(ring_timer_);
    if (snooze_minutes > 0) {
        time_t now = time(nullptr);
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<int>(alarms_.size()) < kMaxAlarms) {
            Alarm snooze;
            snooze.id = NextIdLocked();
            snooze.is_timer = true;
            snooze.fire_at = static_cast<int64_t>(now) + snooze_minutes * 60;
            snooze.created_at = static_cast<int64_t>(now);
            snooze.label = current_ring_.label.empty() ? std::string("Snooze") : current_ring_.label;
            alarms_.push_back(snooze);
            SaveLocked();
        }
    }
    if (on_ring_stop_) {
        on_ring_stop_();
    }
}

int AlarmManager::AddTimer(int seconds, const std::string& label) {
    time_t now = time(nullptr);
    if (!alarm_schedule::IsTimeValid(now) || seconds < 1) {
        return -1;
    }
    Alarm alarm;
    alarm.is_timer = true;
    alarm.fire_at = static_cast<int64_t>(now) + seconds;
    alarm.created_at = static_cast<int64_t>(now);
    alarm.label = label;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<int>(alarms_.size()) >= kMaxAlarms) {
            return -1;
        }
        alarm.id = NextIdLocked();
        alarms_.push_back(alarm);
        SaveLocked();
    }
    last_added_us_ = esp_timer_get_time();
    last_added_seconds_ = seconds;
    last_added_id_ = alarm.id;
    ESP_LOGI(TAG, "Timer %d set for %d s (%s)", alarm.id, seconds, label.c_str());
    return alarm.id;
}

bool AlarmManager::CancelTimer(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(alarms_.begin(), alarms_.end(),
                           [id](const Alarm& a) { return a.id == id && a.is_timer; });
    if (it == alarms_.end()) {
        return false;
    }
    alarms_.erase(it);
    SaveLocked();
    return true;
}

bool AlarmManager::HasTimer(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(alarms_.begin(), alarms_.end(),
                       [id](const Alarm& a) { return a.id == id && a.is_timer; });
}

bool AlarmManager::ExtendTimer(int id, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Alarm* target = nullptr;
    for (auto& alarm : alarms_) {
        if (!alarm.is_timer) continue;
        if (id != 0 ? alarm.id == id : (target == nullptr || alarm.fire_at < target->fire_at)) {
            target = &alarm;
        }
    }
    if (target == nullptr) {
        return false;
    }
    target->fire_at += seconds;
    SaveLocked();
    ESP_LOGI(TAG, "Timer %d extended by %d s", target->id, seconds);
    return true;
}

int AlarmManager::TimerRemaining(int id) {
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& alarm : alarms_) {
        if (alarm.is_timer && alarm.id == id) {
            return static_cast<int>(std::max<int64_t>(0, alarm.fire_at - static_cast<int64_t>(now)));
        }
    }
    return -1;
}

bool AlarmManager::CancelPendingRing() {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = retry_timer_id_;
        retry_timer_id_ = 0;
    }
    return id != 0 && CancelTimer(id);
}
