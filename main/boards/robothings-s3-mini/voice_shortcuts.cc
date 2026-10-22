#include "voice_shortcuts.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace {

using Words = std::initializer_list<const char*>;

// Longer sentences are questions or requests for the AI, not device commands.
constexpr size_t kMaxShortcutWords = 10;

// Goodbye words, and the filler that may come with them ("ok bye alexa").
constexpr Words kByeWords = {"bye", "byebye", "goodbye", "tata", "alvida", "बाय", "बाई",
                             "बायबाय", "टाटा", "अलविदा"};
constexpr Words kByeFiller = {"ok", "okay", "good", "alexa", "now", "then", "see", "you",
                              "later", "ji", "thanks", "thank", "chalo", "phir", "milte",
                              "hain", "ab", "dost", "ओके", "चलो", "फिर", "मिलते", "हैं",
                              "अब", "जी", "थैंक", "यू"};
// Speech-to-text sometimes writes "bye" in Chinese.
constexpr Words kByeChinese = {"拜拜", "再见", "拜"};

constexpr Words kQuietWords = {"chup", "चुप"};
constexpr Words kQuietPhrases = {"shut up", "stop talking", "be quiet", "keep quiet",
                                 "quiet please", "silence"};
constexpr Words kSleepPhrases = {"so jao", "so ja", "soja", "so jaao", "go to sleep",
                                 "sleep now", "सो जाओ", "सो जा"};

// Screen theme
constexpr Words kScreenWords = {"mode", "screen", "theme", "display", "स्क्रीन", "मोड", "थीम",
                                "डिस्प्ले"};
constexpr Words kLightWords = {"light", "white", "bright", "safed", "लाइट", "व्हाइट", "सफेद"};
constexpr Words kDarkWords = {"dark", "black", "kala", "डार्क", "ब्लैक", "काला"};
constexpr Words kInvertWords = {"invert", "inverted", "inverse", "इनवर्ट"};
constexpr Words kOffWords = {"off", "band", "बंद"};
// "light on karo" is the lamp relay, not the screen.
constexpr Words kLampWords = {"lamp", "bulb", "lampa", "लैंप", "बल्ब"};

// Screen mode
constexpr Words kTimeWords = {"time", "clock", "ghadi", "ghari", "gadi", "samay", "टाइम", "क्लॉक",
                              "घड़ी", "घडी", "समय"};
constexpr Words kEmotionWords = {"face", "faces", "emotion", "emotions", "eye", "eyes", "aankhen",
                                 "aankh", "ankhen", "animation", "animations", "chehra",
                                 "expressions", "फेस", "इमोशन", "इमोशंस", "आंखें", "आँखें",
                                 "चेहरा", "एनिमेशन"};
constexpr Words kShowWords = {"show", "display", "dikhao", "dikha", "dikhado", "dikhaao",
                              "dikhaiye", "dikhaye", "dikhana", "screen", "mode", "दिखाओ",
                              "दिखा", "दिखाइए", "दिखादो", "स्क्रीन", "मोड"};
constexpr Words kChangeWords = {"change", "switch", "toggle", "swap", "badlo", "badal", "badle",
                                "चेंज", "बदलो", "बदल"};
// Showing the running timer ("show timer", "countdown dikhao")
constexpr Words kTimerWords = {"timer", "timers", "countdown", "pomodoro", "टाइमर", "काउंटडाउन",
                               "पोमोडोरो"};
// ...but setting, cancelling or asking about one is for the AI.
constexpr Words kTimerActionWords = {"set", "start", "laga", "lagao", "lagado", "minute",
                                     "minutes", "min", "second", "seconds", "hour", "hours",
                                     "cancel", "stop", "band", "delete", "hatao", "remove",
                                     "kitna", "kitne", "how", "much", "left", "baki", "bacha",
                                     "मिनट", "लगाओ", "बंद", "कितना"};

// Alarm / timer / format requests also mention "time" or "clock": the AI handles them.
constexpr Words kNotModeWords = {"timer", "timers", "alarm", "alarms", "pomodoro", "minute",
                                 "minutes", "min", "second", "seconds", "hour", "hours", "ghante",
                                 "ghanta", "set", "laga", "lagao", "lagado", "baje", "format",
                                 "zone", "timezone", "12", "24", "टाइमर", "अलार्म", "मिनट",
                                 "बजे", "घंटे", "फॉर्मेट"};
// Questions ("what time is it", "time kya hua") are for the AI.
constexpr Words kQuestionWords = {"what", "whats", "kya", "kitna", "kitne", "kitni", "batao",
                                  "bata", "bataiye", "tell", "क्या", "कितने", "बताओ"};
// "happy face dikhao" asks for one expression (show_expression tool), not the mode.
constexpr Words kExpressionWords = {"happy", "sad", "angry", "crying", "cry", "roo", "ro",
                                    "gussa", "smile", "smiling", "love", "dil", "wink", "sleepy",
                                    "sleeping", "surprised", "shocked", "khush", "udaas",
                                    "funny", "confused", "excited", "dukhi", "naraz", "cute",
                                    "रो", "गुस्सा", "खुश", "उदास"};

// Ticking of the countdown / stopwatch screens ("ticking sound off karo")
constexpr Words kTickWords = {"ticking", "tick", "ticks", "tik", "tic", "tock", "tiktik",
                              "ticktick", "ticktock", "tiktok", "टिक", "टिकटिक", "टिकिंग"};
constexpr Words kTickPhrases = {"clock sound", "clock sounds", "ghadi ki awaaz",
                                "ghadi ki awaz", "ghadi ki aawaz", "घड़ी की आवाज"};
constexpr Words kTickOffWords = {"off", "band", "bandh", "stop", "disable", "mute", "hatao",
                                 "hata", "silent", "nahi", "mat", "बंद", "हटाओ", "नहीं", "ऑफ"};
constexpr Words kTickOnWords = {"on", "chalu", "start", "enable", "shuru", "wapas", "unmute",
                                "lagao", "ऑन", "चालू", "शुरू"};

bool In(const std::string& word, Words set) {
    for (const char* w : set) {
        if (word == w) return true;
    }
    return false;
}

class Sentence {
public:
    explicit Sentence(const std::string& normalized) : text_(normalized) {
        std::istringstream stream(normalized);
        std::string word;
        while (stream >> word) words_.push_back(word);
    }

    size_t size() const { return words_.size(); }
    const std::vector<std::string>& words() const { return words_; }
    const std::string& text() const { return text_; }

    bool Has(Words set) const {
        return std::any_of(words_.begin(), words_.end(),
                           [&](const std::string& w) { return In(w, set); });
    }
    bool OnlyFrom(Words a, Words b) const {
        return std::all_of(words_.begin(), words_.end(),
                           [&](const std::string& w) { return In(w, a) || In(w, b); });
    }
    // Whole-word phrase match (" so jao " inside " ok so jao ").
    bool HasPhrase(Words phrases) const {
        const std::string padded = " " + text_ + " ";
        for (const char* p : phrases) {
            if (padded.find(" " + std::string(p) + " ") != std::string::npos) return true;
        }
        return false;
    }
    bool Contains(Words fragments) const {
        for (const char* f : fragments) {
            if (text_.find(f) != std::string::npos) return true;
        }
        return false;
    }

private:
    std::string text_;
    std::vector<std::string> words_;
};

VoiceShortcut ParseStandby(const Sentence& s) {
    if (s.size() <= 6 && s.Has(kByeWords) && s.OnlyFrom(kByeWords, kByeFiller)) {
        return VoiceShortcut::Standby;
    }
    // Chinese has no spaces: accept a short text made of these characters.
    if (s.size() <= 2 && s.text().size() <= 12 && s.Contains(kByeChinese)) {
        return VoiceShortcut::Standby;
    }
    if (s.size() <= 6 && (s.Has(kQuietWords) || s.HasPhrase(kQuietPhrases))) {
        return VoiceShortcut::Standby;
    }
    if (s.size() <= 6 && s.HasPhrase(kSleepPhrases)) {
        return VoiceShortcut::Sleep;
    }
    return VoiceShortcut::None;
}

VoiceShortcut ParseTheme(const Sentence& s) {
    if (s.Has(kLampWords)) return VoiceShortcut::None;
    const bool screen = s.Has(kScreenWords);
    if (s.Has(kInvertWords) && !s.Has(kDarkWords)) {
        return s.Has(kOffWords) ? VoiceShortcut::DarkTheme : VoiceShortcut::LightTheme;
    }
    if (s.Has(kDarkWords) && (screen || s.size() == 1)) {
        return VoiceShortcut::DarkTheme;
    }
    if (s.Has(kLightWords) && screen) {
        return s.Has(kOffWords) ? VoiceShortcut::DarkTheme : VoiceShortcut::LightTheme;
    }
    return VoiceShortcut::None;
}

// ---------------------------------------------------------------- timers
constexpr Words kSetWords = {"set", "start", "laga", "lagao", "lagado", "lagaao", "chalu", "shuru",
                             "begin", "chalao", "लगाओ", "लगा", "शुरू", "चालू", "स्टार्ट"};
constexpr Words kStopWords = {"stop", "end", "band", "khatam", "finish", "cancel", "off", "ruko",
                              "dismiss", "enough", "bas", "बंद", "स्टॉप", "रुको", "बस", "खत्म"};
// "stop" may come with these words and still mean "stop the ringing alarm".
constexpr Words kStopFiller = {"it", "the", "alarm", "karo", "kar", "do", "ok", "okay", "please",
                               "now", "ab", "turn", "that", "this", "ringing", "sound", "beep",
                               "करो", "कर", "दो", "अलार्म", "अब", "ओके"};
constexpr Words kPauseWords = {"pause", "hold", "roko", "rok", "रोको", "पॉज़", "पॉज"};
constexpr Words kResumeWords = {"resume", "continue", "unpause", "wapas", "dobara", "फिर",
                                "रिज्यूम"};
constexpr Words kResetWords = {"reset"};
constexpr Words kExtendWords = {"extend", "extension", "more", "aur", "badhao", "badha", "badhado",
                                "add", "snooze", "बढ़ाओ", "बढ़ा", "और", "एक्सटेंड"};
constexpr Words kPomodoroWords = {"pomodoro", "pomodoros", "पोमोडोरो"};
constexpr Words kStopwatchWords = {"stopwatch", "स्टॉपवॉच"};
constexpr Words kCountdownTimerWords = {"timer", "countdown", "टाइमर", "काउंटडाउन"};
constexpr Words kNotTimerWords = {"alarm", "alarms", "remind", "reminder", "yaad", "baje",
                                  "अलार्म", "बजे", "याद"};

struct NumberWord {
    const char* word;
    int value;
};
constexpr NumberWord kNumberWords[] = {
    {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7},
    {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13},
    {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18},
    {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
    {"sixty", 60}, {"a", 1}, {"an", 1},
    {"ek", 1}, {"do", 2}, {"teen", 3}, {"char", 4}, {"chaar", 4}, {"paanch", 5}, {"panch", 5},
    {"chhe", 6}, {"saat", 7}, {"aath", 8}, {"nau", 9}, {"das", 10}, {"barah", 12},
    {"pandrah", 15}, {"bees", 20}, {"pachees", 25}, {"pachis", 25}, {"pachchis", 25},
    {"tees", 30}, {"chalis", 40}, {"pachas", 50},
    {"एक", 1}, {"दो", 2}, {"तीन", 3}, {"चार", 4}, {"पांच", 5}, {"पाँच", 5}, {"छह", 6},
    {"सात", 7}, {"आठ", 8}, {"नौ", 9}, {"दस", 10}, {"पंद्रह", 15}, {"बीस", 20},
    {"पच्चीस", 25}, {"तीस", 30}, {"चालीस", 40}, {"पचास", 50}, {"साठ", 60},
};

int NumberValue(const std::string& w) {
    if (!w.empty() && std::all_of(w.begin(), w.end(), [](unsigned char c) { return isdigit(c); })) {
        return w.size() <= 4 ? std::stoi(w) : -1;
    }
    for (const auto& n : kNumberWords) {
        if (w == n.word) return n.value;
    }
    return -1;
}

int UnitSeconds(const std::string& w) {
    if (In(w, {"minute", "minutes", "min", "mins", "minat", "minut", "मिनट"})) return 60;
    if (In(w, {"second", "seconds", "sec", "secs", "सेकंड", "सेकेंड"})) return 1;
    if (In(w, {"hour", "hours", "hr", "hrs", "ghanta", "ghante", "घंटा", "घंटे"})) return 3600;
    return 0;
}

// ---------------------------------------------------------------- expressions
struct ExpressionWord {
    const char* word;
    const char* expression;  // self.screen.show_expression name
};
constexpr ExpressionWord kExpressionMap[] = {
    {"rone", "crying"},     {"rona", "crying"},       {"roo", "crying"},      {"ro", "crying"},
    {"rote", "crying"},     {"cry", "crying"},        {"crying", "crying"},   {"रोने", "crying"},
    {"रो", "crying"},       {"sad", "sad"},           {"udaas", "sad"},       {"udas", "sad"},
    {"dukhi", "sad"},       {"उदास", "sad"},          {"दुखी", "sad"},        {"happy", "happy"},
    {"khush", "happy"},     {"khushi", "happy"},      {"smile", "happy"},     {"smiling", "happy"},
    {"muskurao", "happy"},  {"खुश", "happy"},         {"excited", "excited"}, {"excite", "excited"},
    {"एक्साइटेड", "excited"}, {"angry", "angry"},       {"gussa", "angry"},     {"gusse", "angry"},
    {"gussewala", "angry"}, {"naraz", "angry"},       {"गुस्सा", "angry"},      {"गुस्से", "angry"},
    {"surprised", "surprised"}, {"surprise", "surprised"}, {"hairan", "surprised"},
    {"हैरान", "surprised"},  {"shocked", "shocked"},   {"shock", "shocked"},   {"sleepy", "sleeping"},
    {"sleeping", "sleeping"}, {"neend", "sleeping"},  {"नींद", "sleeping"},    {"love", "in_love"},
    {"pyar", "in_love"},    {"pyaar", "in_love"},     {"dil", "in_love"},     {"heart", "in_love"},
    {"प्यार", "in_love"},     {"दिल", "in_love"},        {"wink", "wink"},        {"blush", "blush"},
    {"sharma", "blush"},    {"sharmao", "blush"},     {"sharmana", "blush"},  {"शर्मा", "blush"},
    {"confused", "confused"}, {"confuse", "confused"}, {"thinking", "thinking"}, {"soch", "thinking"},
    {"socho", "thinking"},  {"sochte", "thinking"},   {"सोच", "thinking"},     {"annoyed", "annoyed"},
    {"irritated", "annoyed"}, {"chidh", "annoyed"},   {"tease", "tease"},     {"naughty", "tease"},
    {"masti", "tease"},     {"funny", "tease"},       {"curious", "curious"}, {"sweet", "sweet"},
    {"cute", "sweet"},
};
constexpr Words kExpressionShowWords = {"dikhao", "dikha", "dikhaiye", "dikhado", "dikhaao", "show",
                                        "banao", "bana", "banake", "face", "chehra", "expression",
                                        "दिखाओ", "दिखा", "चेहरा", "बनाओ"};

VoiceCommand ParseExpression(const Sentence& s) {
    if (s.size() > 8 || !s.Has(kExpressionShowWords) || s.Has(kQuestionWords)) return {};
    for (const auto& w : s.words()) {
        for (const auto& e : kExpressionMap) {
            if (w == e.word) {
                VoiceCommand command;
                command.type = VoiceShortcut::ShowExpression;
                command.expression = e.expression;
                return command;
            }
        }
    }
    // "aankh maaro" = wink
    if (s.HasPhrase({"aankh maaro", "aankh maro", "आंख मारो"})) {
        VoiceCommand command;
        command.type = VoiceShortcut::ShowExpression;
        command.expression = "wink";
        return command;
    }
    return {};
}

bool HasDigit(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return isdigit(c); });
}

VoiceShortcut ParseMode(const Sentence& s) {
    if (s.Has(kTimerWords)) {
        const bool show = s.Has(kShowWords) || s.Has(kChangeWords);
        if (show && !s.Has(kTimerActionWords) && !s.Has(kQuestionWords) && !HasDigit(s.text()) &&
            !s.Has(kTimeWords) && !s.Has(kEmotionWords)) {
            return VoiceShortcut::TimerMode;
        }
        return VoiceShortcut::None;
    }
    if (s.Has(kNotModeWords) || s.Has(kQuestionWords)) return VoiceShortcut::None;
    const bool time = s.Has(kTimeWords);
    const bool emotion = s.Has(kEmotionWords);
    const bool show = s.Has(kShowWords);
    const bool change = s.Has(kChangeWords);
    if (time && emotion) return VoiceShortcut::None;  // unclear: let the AI ask
    if (time && (show || change)) return VoiceShortcut::TimeMode;
    if (emotion && (show || change) && !s.Has(kExpressionWords)) {
        return VoiceShortcut::EmotionMode;
    }
    if (change && s.Has({"mode", "मोड", "display", "screen"})) return VoiceShortcut::ToggleMode;
    return VoiceShortcut::None;
}

int DurationOf(const Sentence& s) {
    const auto& w = s.words();
    int total = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        // "half an hour" / "aadha ghanta"
        if ((w[i] == "half" || w[i] == "aadha" || w[i] == "आधा") && i + 1 < w.size()) {
            size_t j = i + 1;
            if (j < w.size() && (w[j] == "an" || w[j] == "a")) ++j;
            if (j < w.size() && UnitSeconds(w[j]) == 3600) {
                total += 1800;
                i = j;
                continue;
            }
        }
        int value = NumberValue(w[i]);
        if (value < 0) {
            // "10min", "25mins"
            size_t k = 0;
            while (k < w[i].size() && isdigit(static_cast<unsigned char>(w[i][k]))) ++k;
            if (k > 0 && k < w[i].size() && k <= 4) {
                const int unit = UnitSeconds(w[i].substr(k));
                if (unit > 0) total += std::stoi(w[i].substr(0, k)) * unit;
            }
            continue;
        }
        // "twenty five minutes"
        size_t next = i + 1;
        if (value >= 20 && value % 10 == 0 && next < w.size()) {
            const int ones = NumberValue(w[next]);
            if (ones > 0 && ones < 10) {
                value += ones;
                ++next;
            }
        }
        // "ten more minutes", "10 aur minute"
        if (next + 1 < w.size() && In(w[next], {"more", "aur", "और", "extra"}) &&
            UnitSeconds(w[next + 1]) > 0) {
            ++next;
        }
        if (next < w.size()) {
            const int unit = UnitSeconds(w[next]);
            if (unit > 0) {
                total += value * unit;
                i = next;
            }
        }
    }
    return total;
}

VoiceCommand ParseStopwatch(const Sentence& s) {
    if (s.Has(kPauseWords)) return {VoiceShortcut::StopwatchPause};
    if (s.Has(kResumeWords)) return {VoiceShortcut::StopwatchResume};
    if (s.Has(kStopWords) || s.Has(kResetWords)) return {VoiceShortcut::StopwatchStop};
    if (s.Has(kShowWords) && !s.Has(kSetWords)) return {VoiceShortcut::StopwatchShow};
    return {VoiceShortcut::StopwatchStart};  // "start stopwatch", "stopwatch chalao", "stopwatch"
}

VoiceCommand ParsePomodoro(const Sentence& s) {
    if (s.Has(kPauseWords)) return {VoiceShortcut::PomodoroPause};
    if (s.Has(kResumeWords)) return {VoiceShortcut::PomodoroResume};
    if (s.Has(kStopWords)) return {VoiceShortcut::PomodoroStop};
    if (s.Has(kExtendWords)) return {VoiceShortcut::Extend, DurationOf(s)};
    if (s.Has(kShowWords) && !s.Has(kSetWords)) return {VoiceShortcut::TimerMode};
    return {VoiceShortcut::PomodoroStart, DurationOf(s)};  // "set pomodoro timer"
}

VoiceCommand ParseTimers(const Sentence& s) {
    if (s.Has(kQuestionWords) || s.Has({"how", "much", "left", "baki", "bacha", "remaining"})) {
        return {};  // "how much time is left" is a question for the AI
    }
    const int seconds = DurationOf(s);
    const bool timer_word = s.Has(kCountdownTimerWords);
    if (s.Has(kExtendWords) && (seconds > 0 || s.Has({"extend", "extension", "एक्सटेंड"}))) {
        return {VoiceShortcut::Extend, seconds};
    }
    if (timer_word && seconds > 0 && !s.Has(kNotTimerWords) && !s.Has(kStopWords)) {
        return {VoiceShortcut::TimerSet, seconds};
    }
    if (timer_word && !s.Has(kNotTimerWords) && seconds == 0 && !HasDigit(s.text())) {
        // "start the timer" (nothing to count down from) = stopwatch; "stop the timer"
        if (s.Has(kStopWords)) return {VoiceShortcut::StopTimer};
        if (s.Has(kSetWords) && !s.Has(kShowWords)) return {VoiceShortcut::StopwatchStart};
        if (s.Has(kPauseWords)) return {VoiceShortcut::StopwatchPause};
        if (s.Has(kResumeWords)) return {VoiceShortcut::StopwatchResume};
    }
    // "stop", "stop it", "alarm band karo", "bas"
    if (s.size() <= 5 && s.Has(kStopWords) && s.OnlyFrom(kStopWords, kStopFiller)) {
        return {VoiceShortcut::StopAlarm};
    }
    if (s.size() <= 2 && s.OnlyFrom(kPauseWords, {"it", "please"})) {
        return {VoiceShortcut::StopwatchPause};
    }
    if (s.size() <= 2 && s.OnlyFrom(kResumeWords, {"it", "please"})) {
        return {VoiceShortcut::StopwatchResume};
    }
    return {};
}

VoiceShortcut ParseTicking(const Sentence& s) {
    if (!s.Has(kTickWords) && !s.HasPhrase(kTickPhrases)) return VoiceShortcut::None;
    if (s.Has(kTickOffWords)) return VoiceShortcut::TickingOff;
    if (s.Has(kTickOnWords)) return VoiceShortcut::TickingOn;
    return VoiceShortcut::None;  // e.g. "what is ticking": the AI answers
}

}  // namespace

int ParseDuration(const std::string& normalized_text) {
    return DurationOf(Sentence(normalized_text));
}

std::string NormalizeSpeech(const std::string& text) {
    std::string out;
    bool space = false;
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        // Devanagari danda "।" (E0 A5 A4) and double danda "॥" (E0 A5 A5).
        if (c == 0xE0 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA5 &&
            (static_cast<unsigned char>(text[i + 2]) == 0xA4 ||
             static_cast<unsigned char>(text[i + 2]) == 0xA5)) {
            i += 2;
            space = true;
            continue;
        }
        // Chinese full-width punctuation: "，" (EF BC 8C), "。" (E3 80 82), "！" (EF BC 81),
        // "？" (EF BC 9F).
        if (i + 2 < text.size()) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            if ((c == 0xEF && c1 == 0xBC && (c2 == 0x8C || c2 == 0x81 || c2 == 0x9F)) ||
                (c == 0xE3 && c1 == 0x80 && c2 == 0x82)) {
                i += 2;
                space = true;
                continue;
            }
        }
        if (c < 0x80 && !isalnum(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(c < 0x80 ? tolower(c) : c));
    }
    for (const char* name : {"hey alexa ", "alexa ", "एलेक्सा "}) {
        if (out.rfind(name, 0) == 0) out.erase(0, strlen(name));
    }
    for (const char* name : {" alexa", " एलेक्सा"}) {
        const size_t n = strlen(name);
        if (out.size() > n && out.compare(out.size() - n, n, name) == 0) out.erase(out.size() - n);
    }
    return out;
}

VoiceCommand ParseVoiceCommand(const std::string& raw) {
    std::string text = NormalizeSpeech(raw);
    // "stop watch" -> "stopwatch"
    for (size_t pos; (pos = (" " + text + " ").find(" stop watch ")) != std::string::npos;) {
        text.replace(pos, 10, "stopwatch");
    }
    const Sentence s(text);
    if (s.size() == 0 || s.size() > kMaxShortcutWords) return {};
    if (auto r = ParseStandby(s); r != VoiceShortcut::None) return {r};
    if (auto r = ParseTicking(s); r != VoiceShortcut::None) return {r};
    if (auto r = ParseTheme(s); r != VoiceShortcut::None) return {r};
    if (auto r = ParseExpression(s); r.type != VoiceShortcut::None) return r;
    if (s.Has(kStopwatchWords)) return ParseStopwatch(s);
    if (s.Has(kPomodoroWords)) return ParsePomodoro(s);
    if (auto r = ParseTimers(s); r.type != VoiceShortcut::None) return r;
    return {ParseMode(s)};
}

VoiceShortcut ParseVoiceShortcut(const std::string& raw) { return ParseVoiceCommand(raw).type; }

const char* VoiceShortcutName(VoiceShortcut shortcut) {
    switch (shortcut) {
        case VoiceShortcut::Standby:
            return "standby";
        case VoiceShortcut::TickingOn:
            return "ticking on";
        case VoiceShortcut::TickingOff:
            return "ticking off";
        case VoiceShortcut::Sleep:
            return "sleep";
        case VoiceShortcut::TimeMode:
            return "time mode";
        case VoiceShortcut::EmotionMode:
            return "emotion mode";
        case VoiceShortcut::ToggleMode:
            return "toggle mode";
        case VoiceShortcut::TimerMode:
            return "timer mode";
        case VoiceShortcut::TimerSet:
            return "set timer";
        case VoiceShortcut::Extend:
            return "extend";
        case VoiceShortcut::StopAlarm:
            return "stop alarm";
        case VoiceShortcut::StopTimer:
            return "stop timer";
        case VoiceShortcut::PomodoroStart:
            return "pomodoro start";
        case VoiceShortcut::PomodoroStop:
            return "pomodoro stop";
        case VoiceShortcut::PomodoroPause:
            return "pomodoro pause";
        case VoiceShortcut::PomodoroResume:
            return "pomodoro resume";
        case VoiceShortcut::StopwatchStart:
            return "stopwatch start";
        case VoiceShortcut::StopwatchPause:
            return "stopwatch pause";
        case VoiceShortcut::StopwatchResume:
            return "stopwatch resume";
        case VoiceShortcut::StopwatchStop:
            return "stopwatch stop";
        case VoiceShortcut::StopwatchShow:
            return "stopwatch show";
        case VoiceShortcut::ShowExpression:
            return "show expression";
        case VoiceShortcut::LightTheme:
            return "light theme";
        case VoiceShortcut::DarkTheme:
            return "dark theme";
        case VoiceShortcut::None:
            break;
    }
    return "none";
}
