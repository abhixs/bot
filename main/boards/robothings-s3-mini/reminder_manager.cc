#include "reminder_manager.h"

#include "alarm_schedule.h"
#include "device_action.h"
#include "mcp_server.h"
#include "reminder_schedule.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>

#define TAG "Reminders"

namespace {
constexpr const char* kNvsNamespace = "reminders";
constexpr const char* kListKey = "list";
constexpr const char* kDueKey = "due";

// Short single-line text: no line breaks or quotes (it ends up in a prompt).
std::string CleanMessage(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (c < 0x20 || c == '"' || c == '\\') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > static_cast<size_t>(ReminderManager::kMaxMessageLength)) {
        size_t cut = ReminderManager::kMaxMessageLength;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
        out.resize(cut);  // never in the middle of a UTF-8 character
    }
    return out;
}

std::string LowerAscii(const std::string& text) {
    std::string out = text;
    for (auto& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

void ReminderManager::Initialize() {
    Load();
    RegisterTools();
    esp_timer_create_args_t args = {
        .callback = [](void* arg) { static_cast<ReminderManager*>(arg)->CheckDue(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reminders",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &check_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(check_timer_, 1000 * 1000));
    ESP_LOGI(TAG, "Loaded %d reminder(s), %d waiting to be announced",
             static_cast<int>(reminders_.size()), static_cast<int>(due_.size()));
}

void ReminderManager::Load() {
    Settings settings(kNvsNamespace, false);
    std::lock_guard<std::mutex> lock(mutex_);
    next_id_ = std::max(1, static_cast<int>(settings.GetInt("next_id", 1)));
    reminders_.clear();
    due_.clear();

    cJSON* root = cJSON_Parse(settings.GetString(kListKey, "[]").c_str());
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            cJSON* id = cJSON_GetObjectItem(item, "i");
            cJSON* message = cJSON_GetObjectItem(item, "m");
            cJSON* fire_at = cJSON_GetObjectItem(item, "f");
            cJSON* days = cJSON_GetObjectItem(item, "d");
            cJSON* hour = cJSON_GetObjectItem(item, "h");
            cJSON* minute = cJSON_GetObjectItem(item, "n");
            if (!cJSON_IsNumber(id) || !cJSON_IsString(message) || !cJSON_IsNumber(fire_at) ||
                !cJSON_IsNumber(days) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute)) {
                continue;
            }
            Reminder r;
            r.id = id->valueint;
            r.message = message->valuestring;
            r.fire_at = static_cast<int64_t>(fire_at->valuedouble);
            r.days = static_cast<uint8_t>(days->valueint & alarm_schedule::kEveryDay);
            r.hour = std::clamp(hour->valueint, 0, 23);
            r.minute = std::clamp(minute->valueint, 0, 59);
            if (static_cast<int>(reminders_.size()) < kMaxReminders) reminders_.push_back(r);
        }
    }
    cJSON_Delete(root);

    root = cJSON_Parse(settings.GetString(kDueKey, "[]").c_str());
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            cJSON* id = cJSON_GetObjectItem(item, "i");
            cJSON* message = cJSON_GetObjectItem(item, "m");
            cJSON* due_at = cJSON_GetObjectItem(item, "t");
            if (!cJSON_IsNumber(id) || !cJSON_IsString(message) || !cJSON_IsNumber(due_at)) {
                continue;
            }
            Due d;
            d.id = id->valueint;
            d.message = message->valuestring;
            d.due_at = static_cast<int64_t>(due_at->valuedouble);
            d.missed = cJSON_IsTrue(cJSON_GetObjectItem(item, "x"));
            if (static_cast<int>(due_.size()) < kMaxDue) due_.push_back(d);
        }
    }
    cJSON_Delete(root);
}

void ReminderManager::SaveLocked() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& r : reminders_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "i", r.id);
        cJSON_AddStringToObject(item, "m", r.message.c_str());
        cJSON_AddNumberToObject(item, "f", static_cast<double>(r.fire_at));
        cJSON_AddNumberToObject(item, "d", r.days);
        cJSON_AddNumberToObject(item, "h", r.hour);
        cJSON_AddNumberToObject(item, "n", r.minute);
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        Settings settings(kNvsNamespace, true);
        settings.SetString(kListKey, json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void ReminderManager::SaveDueLocked() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& d : due_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "i", d.id);
        cJSON_AddStringToObject(item, "m", d.message.c_str());
        cJSON_AddNumberToObject(item, "t", static_cast<double>(d.due_at));
        cJSON_AddBoolToObject(item, "x", d.missed);
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    if (json != nullptr) {
        Settings settings(kNvsNamespace, true);
        settings.SetString(kDueKey, json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

int ReminderManager::NextIdLocked() {
    int id = next_id_;
    for (const auto& r : reminders_) id = std::max(id, r.id + 1);
    if (id > 1000000) id = 1;
    next_id_ = id + 1;
    Settings settings(kNvsNamespace, true);
    settings.SetInt("next_id", next_id_);
    return id;
}

// Every second (esp_timer task): reminders whose time has come move to the due list.
void ReminderManager::CheckDue() {
    const time_t now = time(nullptr);
    if (!alarm_schedule::IsTimeValid(now)) return;  // clock not synced yet
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (auto it = reminders_.begin(); it != reminders_.end();) {
        if (it->fire_at > static_cast<int64_t>(now)) {
            ++it;
            continue;
        }
        changed = true;
        const bool already = std::any_of(due_.begin(), due_.end(), [&](const Due& d) {
            return d.id == it->id && d.due_at == it->fire_at;
        });
        if (!already) {
            Due d;
            d.id = it->id;
            d.message = it->message;
            d.due_at = it->fire_at;
            d.missed = static_cast<int64_t>(now) - it->fire_at > kMissedAfterSeconds;
            if (static_cast<int>(due_.size()) >= kMaxDue) {
                ESP_LOGW(TAG, "Too many reminders waiting: dropping the oldest");
                due_.erase(due_.begin());
            }
            due_.push_back(d);
            ESP_LOGI(TAG, "Reminder %d due%s: %s", d.id, d.missed ? " (missed)" : "",
                     d.message.c_str());
        }
        if (it->days != 0) {
            // Repeating: on to its next time (after now: missed days are not replayed).
            it->fire_at = alarm_schedule::NextOccurrence(now, it->hour, it->minute, it->days);
            ++it;
        } else {
            it = reminders_.erase(it);
        }
    }
    if (changed) {
        SaveLocked();
        SaveDueLocked();
    }
}

bool ReminderManager::PeekDue(Due& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (due_.empty()) return false;
    out = due_.front();
    return true;
}

void ReminderManager::MarkAnnounced(int id, int64_t due_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(due_.begin(), due_.end(),
                           [&](const Due& d) { return d.id == id && d.due_at == due_at; });
    if (it == due_.end()) return;
    due_.erase(it);
    SaveDueLocked();
}

bool ReminderManager::MarkFailed(int id, int64_t due_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(due_.begin(), due_.end(),
                           [&](const Due& d) { return d.id == id && d.due_at == due_at; });
    if (it == due_.end()) return false;
    if (++it->attempts < kMaxAttempts) return true;
    ESP_LOGW(TAG, "Reminder %d could not be announced (no connection): dropped", id);
    due_.erase(it);
    SaveDueLocked();
    return false;
}

int ReminderManager::AddIn(const std::string& message, int seconds) {
    const time_t now = time(nullptr);
    if (!alarm_schedule::IsTimeValid(now) || seconds <= 0) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(reminders_.size()) >= kMaxReminders) return -1;
    Reminder r;
    r.id = NextIdLocked();
    r.message = CleanMessage(message);
    r.fire_at = static_cast<int64_t>(now) + seconds;
    int wday;
    int64_t day_index;
    alarm_schedule::SplitTime(static_cast<time_t>(r.fire_at), wday, r.hour, r.minute, day_index);
    reminders_.push_back(r);
    SaveLocked();
    last_snooze_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "Reminder %d set by the device for %d s: %s", r.id, seconds, r.message.c_str());
    return r.id;
}

std::string ReminderManager::DescribeLocked(const Reminder& r) const {
    return alarm_schedule::FormatDateTime(static_cast<time_t>(r.fire_at));
}

std::string ReminderManager::ListJson() {
    const time_t now = time(nullptr);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_time",
                            alarm_schedule::IsTimeValid(now)
                                ? alarm_schedule::FormatDateTime(now).c_str()
                                : "unknown (clock not synced yet)");
    cJSON* list = cJSON_AddArrayToObject(root, "reminders");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Reminder> sorted = reminders_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Reminder& a, const Reminder& b) { return a.fire_at < b.fire_at; });
        for (const auto& r : sorted) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", r.id);
            cJSON_AddStringToObject(item, "message", r.message.c_str());
            cJSON_AddStringToObject(item, "next", DescribeLocked(r).c_str());
            cJSON_AddStringToObject(item, "repeat", alarm_schedule::RepeatToString(r.days).c_str());
            cJSON_AddItemToArray(list, item);
        }
    }
    char* json = cJSON_PrintUnformatted(root);
    std::string result = json != nullptr ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

void ReminderManager::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.reminder.create",
        "ALWAYS use for 'remind me ...' / '... yaad dilana' (never a timer or alarm for these): "
        "at the time the device says 'Reminder, <message>'. message: 2-6 words in the user's "
        "language ('drink water', 'Rahul ko call'). When: minutes_from_now and/or "
        "seconds_from_now ('30 sec me' = seconds 30), OR time HH:MM 24h ('6 PM' = 18:00) with "
        "optional day (today/tomorrow/weekday/YYYY-MM-DD) and repeat (once/daily/weekdays/"
        "weekends/weekly/'mon,thu'). If the message or time is missing or unclear ('kal 8 "
        "baje' - morning or evening?), ask first; never guess. Confirm in one short sentence.",
        PropertyList({
            Property("message", kPropertyTypeString, std::string("")).SetMaxLength(80),
            Property("minutes_from_now", kPropertyTypeInteger, 0, 0, 10080),
            Property("seconds_from_now", kPropertyTypeInteger, 0, 0, 3600),
            Property("time", kPropertyTypeString, std::string("")).SetMaxLength(12),
            Property("day", kPropertyTypeString, std::string("")).SetMaxLength(20),
            Property("repeat", kPropertyTypeString, std::string("once")).SetMaxLength(30),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            const time_t now = time(nullptr);
            if (!alarm_schedule::IsTimeValid(now)) {
                return std::unexpected(
                    std::string("Device clock is not synced yet; try again in a moment."));
            }
            if (esp_timer_get_time() - last_snooze_us_ < 30 * 1000000LL) {
                // The user just asked to be reminded again; the device has done it.
                return std::string(
                    "{\"success\":true,\"note\":\"Already rescheduled by the device. Just "
                    "confirm in one short sentence.\"}");
            }
            const std::string message = CleanMessage(properties["message"].value<std::string>());
            if (message.empty()) {
                return std::unexpected(std::string(
                    "No message. Ask the user what the reminder is for, then call again."));
            }
            reminder_schedule::When when;
            std::string error;
            const int from_now = properties["minutes_from_now"].value<int>() * 60 +
                                 properties["seconds_from_now"].value<int>();
            if (!reminder_schedule::Resolve(now, from_now,
                                            properties["time"].value<std::string>(),
                                            properties["day"].value<std::string>(),
                                            properties["repeat"].value<std::string>(), when,
                                            error)) {
                return std::unexpected(error);
            }
            Reminder r;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const std::string key = LowerAscii(message);
                for (const auto& existing : reminders_) {
                    if (LowerAscii(existing.message) == key && existing.fire_at == when.fire_at &&
                        existing.days == when.days) {
                        return std::string("{\"success\":true,\"note\":\"already set\"}");
                    }
                }
                if (static_cast<int>(reminders_.size()) >= kMaxReminders) {
                    return std::unexpected(std::string(
                        "Too many reminders (max 20). Cancel one with self.reminder.cancel."));
                }
                r.id = NextIdLocked();
                r.message = message;
                r.fire_at = when.fire_at;
                r.days = when.days;
                r.hour = when.hour;
                r.minute = when.minute;
                reminders_.push_back(r);
                SaveLocked();
            }
            ESP_LOGI(TAG, "Reminder %d set for %s (%s): %s", r.id,
                     alarm_schedule::FormatDateTime(static_cast<time_t>(r.fire_at)).c_str(),
                     alarm_schedule::RepeatToString(r.days).c_str(), r.message.c_str());
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "id", r.id);
            cJSON_AddStringToObject(result, "message", r.message.c_str());
            cJSON_AddStringToObject(
                result, "when", alarm_schedule::FormatDateTime(static_cast<time_t>(r.fire_at)).c_str());
            cJSON_AddStringToObject(result, "repeat",
                                    alarm_schedule::RepeatToString(r.days).c_str());
            cJSON_AddStringToObject(result, "device_time", alarm_schedule::FormatDateTime(now).c_str());
            return result;
        });

    mcp.AddTool(
        "self.reminder.list",
        "List reminders (id, message, next time, repeat) and the device time. 'What reminders "
        "do I have'; also before editing / cancelling one.",
        PropertyList(), [this](const PropertyList&) -> ToolResult { return ListJson(); });

    mcp.AddTool(
        "self.reminder.cancel",
        "Cancel (delete) reminders: by id (from self.reminder.list), or by match = a word of "
        "the message ('cancel my water reminder' -> match 'water'). id -1 cancels all. If "
        "several reminders match and the user did not say 'all', ask which one.",
        PropertyList({
            Property("id", kPropertyTypeInteger, 0, -1, 1000000),
            Property("match", kPropertyTypeString, std::string("")).SetMaxLength(40),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            const int id = properties["id"].value<int>();
            const std::string match = LowerAscii(properties["match"].value<std::string>());
            std::vector<std::string> removed;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (id == 0 && !match.empty()) {
                    int hits = 0;
                    for (const auto& r : reminders_) {
                        if (LowerAscii(r.message).find(match) != std::string::npos) hits++;
                    }
                    if (hits > 1) {
                        return std::unexpected(std::string(
                            "Several reminders match. Call self.reminder.list and ask the user "
                            "which one, then cancel it by id."));
                    }
                }
                for (auto it = reminders_.begin(); it != reminders_.end();) {
                    const bool hit = id == -1 || (id > 0 && it->id == id) ||
                                     (id == 0 && !match.empty() &&
                                      LowerAscii(it->message).find(match) != std::string::npos);
                    if (hit) {
                        removed.push_back(it->message);
                        it = reminders_.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!removed.empty()) SaveLocked();
            }
            if (removed.empty()) {
                return std::unexpected(
                    std::string("No such reminder. Call self.reminder.list to see them."));
            }
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON* names = cJSON_AddArrayToObject(result, "cancelled");
            for (const auto& m : removed) cJSON_AddItemToArray(names, cJSON_CreateString(m.c_str()));
            return result;
        });

    mcp.AddTool(
        "self.reminder.edit",
        "Change a reminder (id from self.reminder.list): its message and/or its time. Leave a "
        "field empty / 0 to keep it. For a new time give minutes_from_now OR time (24-hour "
        "HH:MM) with optional day; repeat as in self.reminder.create. 'Change my reminder to "
        "9 AM' -> time '09:00'. If the user has several reminders and did not say which, ask.",
        PropertyList({
            Property("id", kPropertyTypeInteger, 1, 1, 1000000),
            Property("message", kPropertyTypeString, std::string("")).SetMaxLength(80),
            Property("minutes_from_now", kPropertyTypeInteger, 0, 0, 10080),
            Property("time", kPropertyTypeString, std::string("")).SetMaxLength(12),
            Property("day", kPropertyTypeString, std::string("")).SetMaxLength(20),
            Property("repeat", kPropertyTypeString, std::string("")).SetMaxLength(30),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            NoteDeviceAction();
            const time_t now = time(nullptr);
            if (!alarm_schedule::IsTimeValid(now)) {
                return std::unexpected(
                    std::string("Device clock is not synced yet; try again in a moment."));
            }
            const int id = properties["id"].value<int>();
            const std::string message = CleanMessage(properties["message"].value<std::string>());
            const int minutes = properties["minutes_from_now"].value<int>();
            const int from_now = minutes * 60;
            const std::string clock = properties["time"].value<std::string>();
            const std::string day = properties["day"].value<std::string>();
            const std::string repeat = properties["repeat"].value<std::string>();
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = std::find_if(reminders_.begin(), reminders_.end(),
                                   [id](const Reminder& r) { return r.id == id; });
            if (it == reminders_.end()) {
                return std::unexpected(
                    std::string("No reminder with that id. Call self.reminder.list."));
            }
            Reminder updated = *it;
            if (!message.empty()) updated.message = message;
            if (minutes > 0 || !clock.empty() || !day.empty() || !repeat.empty()) {
                // A new time; a repeat or day change alone keeps the old clock time.
                char hhmm[8];
                snprintf(hhmm, sizeof(hhmm), "%02d:%02d", it->hour, it->minute);
                const std::string use_clock = minutes > 0 ? "" : (clock.empty() ? hhmm : clock);
                const std::string use_repeat =
                    !repeat.empty() ? repeat
                                    : (minutes > 0 ? "once"
                                                   : alarm_schedule::RepeatToString(it->days));
                reminder_schedule::When when;
                std::string error;
                if (!reminder_schedule::Resolve(now, from_now, use_clock, day, use_repeat, when,
                                                error)) {
                    return std::unexpected(error);
                }
                updated.fire_at = when.fire_at;
                updated.days = when.days;
                updated.hour = when.hour;
                updated.minute = when.minute;
            }
            *it = updated;
            SaveLocked();
            ESP_LOGI(TAG, "Reminder %d changed: %s at %s", id, updated.message.c_str(),
                     alarm_schedule::FormatDateTime(static_cast<time_t>(updated.fire_at)).c_str());
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "id", id);
            cJSON_AddStringToObject(result, "message", updated.message.c_str());
            cJSON_AddStringToObject(
                result, "when",
                alarm_schedule::FormatDateTime(static_cast<time_t>(updated.fire_at)).c_str());
            cJSON_AddStringToObject(result, "repeat",
                                    alarm_schedule::RepeatToString(updated.days).c_str());
            return result;
        });
}
