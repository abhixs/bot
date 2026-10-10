#include "clock_sync.h"

#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include <cstdlib>
#include <ctime>
#include <string>

#define TAG "ClockSync"

namespace {
constexpr const char* kNvsNamespace = "clock";
constexpr const char* kNvsKey = "tz_offset_min";
constexpr int64_t kMinValidUtc = 1735689600;  // 2025-01-01
constexpr int kCheckIntervalS = 5;
constexpr int kMaxDriftS = 20;
}  // namespace

void ClockSync::Initialize() {
    Settings settings(kNvsNamespace, false);
    offset_minutes_ = settings.GetInt(kNvsKey, kDefaultOffsetMinutes);
    use_24h_ = settings.GetBool("clock_24h", false);

    McpServer::GetInstance().AddTool(
        "self.clock.set_timezone",
        "Set the device's time zone as an offset from UTC in minutes. India (IST) is 330, "
        "which is the default. Use this only if the user says the device shows the wrong time "
        "for their location.",
        PropertyList({
            Property("utc_offset_minutes", kPropertyTypeInteger, -720, 840),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            SetOffsetMinutes(properties["utc_offset_minutes"].value<int>());
            return true;
        });

    McpServer::GetInstance().AddTool(
        "self.clock.set_format",
        "Switch the clock between 12-hour and 24-hour format, e.g. when the user says 'time "
        "format 12 hours pe kar do'. hours: 12 or 24.",
        PropertyList({Property("hours", kPropertyTypeInteger, 12, 24)}),
        [this](const PropertyList& properties) -> ToolResult {
            int hours = properties["hours"].value<int>();
            if (hours != 12 && hours != 24) {
                return std::unexpected(std::string("hours must be 12 or 24"));
            }
            Set24h(hours == 24);
            return true;
        });

    McpServer::GetInstance().AddTool(
        "self.clock.get_time",
        "Get the current local date, time and weekday where the user is (India, IST by "
        "default). ALWAYS call this to answer questions about the time, date or day, and before "
        "talking about 'today' or 'tomorrow': your own clock may be in a different time zone.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            time_t now = time(nullptr);
            if (now < kMinValidUtc) {
                return std::unexpected(std::string("Device clock is not synced yet."));
            }
            struct tm local;
            gmtime_r(&now, &local);  // the clock already holds local wall time
            char buf[96];
            strftime(buf, sizeof(buf),
                     use_24h_ ? "%Y-%m-%d %H:%M (%A)" : "%Y-%m-%d %I:%M %p (%A)", &local);
            int offset = offset_minutes_;
            char zone[16];
            snprintf(zone, sizeof(zone), "UTC%+d:%02d", offset / 60, std::abs(offset % 60));
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "local_time", buf);
            cJSON_AddStringToObject(result, "time_zone", offset == 330 ? "IST (UTC+5:30)" : zone);
            cJSON_AddBoolToObject(result, "internet_time_synced", ref_utc_s_ != 0);
            return result;
        });

    esp_timer_create_args_t args = {
        .callback = [](void* arg) { static_cast<ClockSync*>(arg)->Check(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_sync",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, kCheckIntervalS * 1000000LL));
}

void ClockSync::SetOffsetMinutes(int minutes) {
    offset_minutes_ = minutes;
    Settings settings(kNvsNamespace, true);
    settings.SetInt(kNvsKey, minutes);
    Check();
}

void ClockSync::Set24h(bool use_24h) {
    use_24h_ = use_24h;
    Settings settings(kNvsNamespace, true);
    settings.SetBool("clock_24h", use_24h);
}

void ClockSync::OnSntpSync(struct timeval* tv) {
    auto& self = GetInstance();
    if (tv == nullptr || tv->tv_sec < kMinValidUtc) {
        return;
    }
    self.ref_utc_s_ = tv->tv_sec;
    self.ref_uptime_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "SNTP sync, UTC %lld", static_cast<long long>(tv->tv_sec));
    // SNTP just wrote plain UTC into the clock; shift it to local time right away.
    self.Check();
}

void ClockSync::StartSntp() {
    if (sntp_started_) {
        return;
    }
    sntp_started_ = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_time_sync_notification_cb(OnSntpSync);
    sntp_set_sync_interval(60 * 60 * 1000);  // re-sync hourly
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

void ClockSync::Check() {
    // Start SNTP once the network is clearly up: the server already set the clock
    // (activation finished) or the device has been running for a while.
    if (!sntp_started_) {
        if (time(nullptr) >= kMinValidUtc || esp_timer_get_time() > 60LL * 1000000) {
            StartSntp();
        }
        return;
    }

    int64_t ref_utc = ref_utc_s_;
    if (ref_utc == 0) {
        return;  // no SNTP answer yet; keep whatever the server set
    }
    int64_t elapsed_s = (esp_timer_get_time() - ref_uptime_us_) / 1000000;
    int64_t expected = ref_utc + elapsed_s + static_cast<int64_t>(offset_minutes_) * 60;
    int64_t now = static_cast<int64_t>(time(nullptr));
    if (std::llabs(now - expected) > kMaxDriftS) {
        struct timeval tv = {.tv_sec = static_cast<time_t>(expected), .tv_usec = 0};
        settimeofday(&tv, nullptr);
        ESP_LOGI(TAG, "Clock corrected by %lld s to UTC%+d min", static_cast<long long>(expected - now),
                 static_cast<int>(offset_minutes_));
    }
}
