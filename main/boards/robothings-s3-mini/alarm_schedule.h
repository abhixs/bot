// Pure scheduling helpers for the RoboThings alarm feature.
// No ESP-IDF dependencies, so this file can be unit-tested on a PC.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace alarm_schedule {

// Bit 0 = Sunday ... bit 6 = Saturday (same order as struct tm::tm_wday).
constexpr uint8_t kEveryDay = 0x7F;
constexpr uint8_t kWeekdays = 0x3E;  // Mon..Fri
constexpr uint8_t kWeekends = 0x41;  // Sun + Sat

// Earliest wall-clock time the device can trust (the server has synced the clock).
constexpr time_t kMinValidTime = 1735689600;  // 2025-01-01 00:00:00

inline bool IsTimeValid(time_t now) { return now >= kMinValidTime; }

// Parses "once", "daily", "weekdays", "weekends" or a comma list such as "mon,wed,fri".
// Returns false for text it does not understand. days == 0 means a one-shot alarm.
inline bool ParseRepeat(const std::string& text, uint8_t& days) {
    std::string s;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (s.empty() || s == "once" || s == "none" || s == "no" || s == "onetime" || s == "one-time") {
        days = 0;
        return true;
    }
    if (s == "daily" || s == "everyday" || s == "always" || s == "all") {
        days = kEveryDay;
        return true;
    }
    if (s == "weekdays" || s == "weekday" || s == "workdays") {
        days = kWeekdays;
        return true;
    }
    if (s == "weekends" || s == "weekend") {
        days = kWeekends;
        return true;
    }
    static const char* kNames[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    uint8_t mask = 0;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string token = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty()) {
            bool found = false;
            for (int d = 0; d < 7; d++) {
                if (token.compare(0, 3, kNames[d]) == 0) {
                    mask |= static_cast<uint8_t>(1u << d);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (mask == 0) {
        return false;
    }
    days = mask;
    return true;
}

inline std::string RepeatToString(uint8_t days) {
    if (days == 0) return "once";
    if (days == kEveryDay) return "daily";
    if (days == kWeekdays) return "weekdays";
    if (days == kWeekends) return "weekends";
    static const char* kNames[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    std::string out;
    for (int d = 0; d < 7; d++) {
        if (days & (1u << d)) {
            if (!out.empty()) out += ",";
            out += kNames[d];
        }
    }
    return out;
}

// The device clock already holds local time (the server adds the timezone
// offset before setting it), so plain gmtime/timegm-style arithmetic on the
// raw seconds gives local wall-clock values.
inline void SplitTime(time_t t, int& wday, int& hour, int& minute, int64_t& day_index) {
    int64_t secs = static_cast<int64_t>(t);
    day_index = secs / 86400;
    int64_t sod = secs % 86400;
    hour = static_cast<int>(sod / 3600);
    minute = static_cast<int>((sod % 3600) / 60);
    // 1970-01-01 was a Thursday (wday 4).
    wday = static_cast<int>((day_index + 4) % 7);
}

// Next time (strictly after `now`) at which hour:minute falls on an allowed day.
// For one-shot alarms (days == 0) any day is allowed.
inline time_t NextOccurrence(time_t now, int hour, int minute, uint8_t days) {
    int wday, h, m;
    int64_t day_index;
    SplitTime(now, wday, h, m, day_index);
    uint8_t allowed = days == 0 ? kEveryDay : days;
    for (int offset = 0; offset <= 7; offset++) {
        int d = (wday + offset) % 7;
        if (!(allowed & (1u << d))) continue;
        int64_t candidate = (day_index + offset) * 86400 + hour * 3600 + minute * 60;
        if (candidate > static_cast<int64_t>(now)) {
            return static_cast<time_t>(candidate);
        }
    }
    return 0;
}

inline std::string FormatClock(time_t t) {
    int wday, h, m;
    int64_t day_index;
    SplitTime(t, wday, h, m, day_index);
    static const char* kNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %02d:%02d", kNames[wday], h, m);
    return buf;
}

inline std::string FormatDateTime(time_t t) {
    // Civil-from-days (Howard Hinnant) to avoid relying on gmtime_r on the host.
    int64_t z = static_cast<int64_t>(t) / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned mo = mp < 10 ? mp + 3 : mp - 9;
    if (mo <= 2) y++;
    char buf[48];
    snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %s", static_cast<long long>(y), mo, d,
             FormatClock(t).c_str());
    return buf;
}

}  // namespace alarm_schedule
