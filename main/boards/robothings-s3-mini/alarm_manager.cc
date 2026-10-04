#include "alarm_manager.h"

#include "alarm_schedule.h"
#include "alarm_tone.h"
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
constexpr int kRingIntervalMs = 1500;

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
    int id = 1;
    for (const auto& alarm : alarms_) {
        id = std::max(id, alarm.id + 1);
    }
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
        "Start a countdown timer on the device, e.g. 'set a 10 minute timer' or 'remind me in "
        "30 seconds'. Give the duration as minutes and/or seconds.",
        PropertyList({
            Property("minutes", kPropertyTypeInteger, 0, 0, 1440),
            Property("seconds", kPropertyTypeInteger, 0, 0, 3600),
            Property("label", kPropertyTypeString, std::string("")).SetMaxLength(40),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            time_t now = time(nullptr);
            if (!alarm_schedule::IsTimeValid(now)) {
                return std::unexpected(
                    std::string("Device clock is not synced yet; try again in a moment."));
            }
            int total = properties["minutes"].value<int>() * 60 + properties["seconds"].value<int>();
            if (total < 5) {
                return std::unexpected(std::string("Timer must be at least 5 seconds."));
            }
            Alarm alarm;
            alarm.is_timer = true;
            alarm.fire_at = static_cast<int64_t>(now) + total;
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
            if (!ringing_) {
                return std::string("{\"success\":true,\"note\":\"no alarm is ringing\"}");
            }
            int snooze = properties["snooze_minutes"].value<int>();
            Application::GetInstance().Schedule([this, snooze]() { StopRinging(snooze); });
            return true;
        });
}

void AlarmManager::CheckAlarms() {
    time_t now = time(nullptr);
    if (!alarm_schedule::IsTimeValid(now)) {
        return;
    }

    bool changed = false;
    bool have_ring = false;
    RingInfo ring;
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
        ESP_LOGI(TAG, "Ringing: %s %s", ring.title.c_str(), ring.label.c_str());
        Application::GetInstance().Schedule([this, ring]() { StartRinging(ring); });
    }
}

void AlarmManager::StartRinging(const RingInfo& info) {
    current_ring_ = info;
    if (!ringing_) {
        ringing_ = true;
        if (on_ring_start_) {
            on_ring_start_(info);
        }
    }
    ring_elapsed_ms_ = 0;
    idle_seen_while_ringing_ = false;
    esp_timer_stop(ring_timer_);
    esp_timer_start_periodic(ring_timer_, kRingIntervalMs * 1000);
    RingTick();
}

void AlarmManager::RingTick() {
    if (!ringing_) {
        return;
    }
    if (ring_elapsed_ms_ >= kRingSeconds * 1000) {
        Application::GetInstance().Schedule([this]() { StopRinging(0); });
        return;
    }
    ring_elapsed_ms_ += kRingIntervalMs;
    auto& app = Application::GetInstance();
    app.Schedule([this, &app]() {
        if (!ringing_) return;
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle:
                idle_seen_while_ringing_ = true;
                app.PlaySound(AlarmToneOgg());
                break;
            case kDeviceStateListening:
            case kDeviceStateSpeaking:
            case kDeviceStateConnecting:
                if (idle_seen_while_ringing_) {
                    // The device was idle and ringing, then someone said the wake word
                    // (or pressed talk): that means "I'm awake", so stop the alarm and
                    // let the conversation go on.
                    StopRinging(0);
                } else if (app.GetDeviceState() != kDeviceStateConnecting) {
                    // A conversation left over from setting the alarm: end it so the
                    // alarm can be heard.
                    app.ToggleChatState();
                }
                break;
            default:
                break;
        }
    });
}

void AlarmManager::StopRinging(int snooze_minutes) {
    if (!ringing_) {
        return;
    }
    ringing_ = false;
    esp_timer_stop(ring_timer_);
    if (snooze_minutes > 0) {
        time_t now = time(nullptr);
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<int>(alarms_.size()) < kMaxAlarms) {
            Alarm snooze;
            snooze.id = NextIdLocked();
            snooze.is_timer = true;
            snooze.fire_at = static_cast<int64_t>(now) + snooze_minutes * 60;
            snooze.label = current_ring_.label.empty() ? std::string("Snooze") : current_ring_.label;
            alarms_.push_back(snooze);
            SaveLocked();
        }
    }
    if (on_ring_stop_) {
        on_ring_stop_();
    }
}
