// Pure date / time helpers for reminders (no ESP-IDF, unit-tested on a PC).
// Works on device-local seconds like alarm_schedule.h: the device clock already
// holds local time, so day / hour arithmetic on the raw seconds is local.
#pragma once

#include "alarm_schedule.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

namespace reminder_schedule {

inline std::string Lower(const std::string& text) {
    std::string s;
    for (char c : text) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // trim
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Days since 1970-01-01 for a civil date (Howard Hinnant).
inline int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Day index (days since 1970-01-01, local) for "today", "tomorrow", "day after
// tomorrow", a weekday name (its next occurrence, today included) or YYYY-MM-DD.
// Empty text = -1 (not given). Returns -2 for text it does not understand.
inline int64_t ResolveDay(time_t now, const std::string& text) {
    const std::string s = Lower(text);
    if (s.empty()) return -1;
    const int64_t today = static_cast<int64_t>(now) / 86400;
    if (s == "today" || s == "aaj" || s == "aj") return today;
    if (s == "tomorrow" || s == "kal") return today + 1;
    if (s == "day after tomorrow" || s == "parso" || s == "parson") return today + 2;
    static const char* kNames[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    for (int d = 0; d < 7; d++) {
        if (s.compare(0, 3, kNames[d]) == 0) {
            const int wday = static_cast<int>((today + 4) % 7);
            return today + (d - wday + 7) % 7;
        }
    }
    int y = 0, m = 0, d = 0;
    if (sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3 && y >= 2024 && m >= 1 && m <= 12 &&
        d >= 1 && d <= 31) {
        return DaysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
    }
    return -2;
}

// "18:00", "6:30 pm", "6 PM", "6pm", "18". Returns false for anything else.
inline bool ParseClock(const std::string& text, int& hour, int& minute) {
    const std::string s = Lower(text);
    if (s.empty()) return false;
    int h = -1, m = 0;
    size_t i = 0;
    auto digits = [&](int& out) {
        size_t start = i;
        out = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 2) {
            out = out * 10 + (s[i] - '0');
            i++;
        }
        return i > start;
    };
    if (!digits(h)) return false;
    if (i < s.size() && (s[i] == ':' || s[i] == '.')) {
        i++;
        if (!digits(m)) return false;
    }
    while (i < s.size() && s[i] == ' ') i++;
    const std::string suffix = s.substr(i);
    if (suffix == "am" || suffix == "a.m." || suffix == "a.m") {
        if (h < 1 || h > 12) return false;
        if (h == 12) h = 0;
    } else if (suffix == "pm" || suffix == "p.m." || suffix == "p.m") {
        if (h < 1 || h > 12) return false;
        if (h != 12) h += 12;
    } else if (!suffix.empty()) {
        return false;
    }
    if (h > 23 || m > 59) return false;
    hour = h;
    minute = m;
    return true;
}

// "once", "daily", "weekdays", "weekends", "mon,wed" (alarm_schedule::ParseRepeat)
// plus "weekly" = every week on the reminder's own weekday (`weekday`, 0 = Sunday).
inline bool ParseRepeat(const std::string& text, int weekday, uint8_t& days) {
    const std::string s = Lower(text);
    if (s == "weekly" || s == "every week" || s == "har hafte") {
        days = static_cast<uint8_t>(1u << weekday);
        return true;
    }
    return alarm_schedule::ParseRepeat(s, days);
}

struct When {
    int64_t fire_at = 0;  // first ring (device local seconds)
    uint8_t days = 0;     // repeat mask (0 = once)
    int hour = 0;
    int minute = 0;
};

// Works out when a reminder rings. Exactly one of `seconds_from_now` (> 0) or
// `clock` must be given; `day` and `repeat` only go with `clock`. On failure
// returns false with a short reason for the AI in `error`.
inline bool Resolve(time_t now, int seconds_from_now, const std::string& clock,
                    const std::string& day, const std::string& repeat, When& out,
                    std::string& error) {
    const bool has_clock = !Lower(clock).empty();
    if (seconds_from_now > 0 && has_clock) {
        error = "Give either a time from now or a clock time, not both.";
        return false;
    }
    if (seconds_from_now > 0) {
        if (!Lower(day).empty() || (!Lower(repeat).empty() && Lower(repeat) != "once")) {
            error = "A time from now cannot have a day or a repeat; use time instead.";
            return false;
        }
        out.fire_at = static_cast<int64_t>(now) + static_cast<int64_t>(seconds_from_now);
        out.days = 0;
        int wday;
        int64_t day_index;
        alarm_schedule::SplitTime(static_cast<time_t>(out.fire_at), wday, out.hour, out.minute,
                                  day_index);
        return true;
    }
    if (!has_clock) {
        error = "When? Ask the user for the time (or how long from now).";
        return false;
    }
    int hour, minute;
    if (!ParseClock(clock, hour, minute)) {
        error = "Invalid time. Use 24-hour HH:MM, e.g. 18:00.";
        return false;
    }
    const int64_t day_index = ResolveDay(now, day);
    if (day_index == -2) {
        error = "Invalid day. Use today, tomorrow, a weekday name or YYYY-MM-DD.";
        return false;
    }
    const int64_t today = static_cast<int64_t>(now) / 86400;
    const int64_t first_day = day_index >= 0 ? day_index : today;
    uint8_t days = 0;
    if (!ParseRepeat(repeat, static_cast<int>((first_day + 4) % 7), days)) {
        error = "Invalid repeat. Use once, daily, weekdays, weekends, weekly or e.g. mon,wed.";
        return false;
    }
    int64_t fire_at;
    if (day_index >= 0) {
        fire_at = day_index * 86400 + hour * 3600 + minute * 60;
        if (fire_at <= static_cast<int64_t>(now)) {
            if (days == 0) {
                error = "That time has already passed. Ask the user for a later time.";
                return false;
            }
            fire_at = alarm_schedule::NextOccurrence(now, hour, minute, days);
        } else if (days != 0 && !(days & (1u << ((day_index + 4) % 7)))) {
            fire_at = alarm_schedule::NextOccurrence(static_cast<time_t>(fire_at - 1), hour,
                                                     minute, days);
        }
    } else {
        // No day: the next time it comes round (today if still ahead, else tomorrow).
        fire_at = alarm_schedule::NextOccurrence(now, hour, minute, days);
    }
    if (fire_at <= 0) {
        error = "Could not work out the time.";
        return false;
    }
    out.fire_at = fire_at;
    out.days = days;
    out.hour = hour;
    out.minute = minute;
    return true;
}

}  // namespace reminder_schedule
