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
constexpr size_t kMaxShortcutWords = 9;

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

VoiceShortcut ParseMode(const Sentence& s) {
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

}  // namespace

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

VoiceShortcut ParseVoiceShortcut(const std::string& raw) {
    const Sentence s(NormalizeSpeech(raw));
    if (s.size() == 0 || s.size() > kMaxShortcutWords) return VoiceShortcut::None;
    if (auto r = ParseStandby(s); r != VoiceShortcut::None) return r;
    if (auto r = ParseTheme(s); r != VoiceShortcut::None) return r;
    return ParseMode(s);
}

const char* VoiceShortcutName(VoiceShortcut shortcut) {
    switch (shortcut) {
        case VoiceShortcut::Standby:
            return "standby";
        case VoiceShortcut::Sleep:
            return "sleep";
        case VoiceShortcut::TimeMode:
            return "time mode";
        case VoiceShortcut::EmotionMode:
            return "emotion mode";
        case VoiceShortcut::ToggleMode:
            return "toggle mode";
        case VoiceShortcut::LightTheme:
            return "light theme";
        case VoiceShortcut::DarkTheme:
            return "dark theme";
        case VoiceShortcut::None:
            break;
    }
    return "none";
}
