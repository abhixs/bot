// Numbers as natural English words for what the assistant says ("twenty-five",
// "three thousand six hundred fifty", "one million four thousand five hundred").
// No ESP-IDF dependencies (unit-tested on a PC).
#pragma once

#include <cstdint>
#include <string>

inline std::string NumberToEnglishWords(int64_t n) {
    static const char* kOnes[] = {"zero",    "one",     "two",       "three",    "four",
                                  "five",    "six",     "seven",     "eight",    "nine",
                                  "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                                  "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* kTens[] = {"",      "",      "twenty",  "thirty", "forty",
                                  "fifty", "sixty", "seventy", "eighty", "ninety"};
    if (n < 0) return "minus " + NumberToEnglishWords(-n);
    if (n < 20) return kOnes[n];
    if (n < 100) {
        std::string s = kTens[n / 10];
        if (n % 10) s += std::string("-") + kOnes[n % 10];
        return s;
    }
    if (n < 1000) {
        std::string s = std::string(kOnes[n / 100]) + " hundred";
        if (n % 100) s += " " + NumberToEnglishWords(n % 100);
        return s;
    }
    static const struct {
        int64_t value;
        const char* name;
    } kScales[] = {{1000000000LL, "billion"}, {1000000LL, "million"}, {1000LL, "thousand"}};
    for (const auto& scale : kScales) {
        if (n >= scale.value) {
            std::string s = NumberToEnglishWords(n / scale.value) + " " + scale.name;
            if (n % scale.value) s += " " + NumberToEnglishWords(n % scale.value);
            return s;
        }
    }
    return std::to_string(n);
}
