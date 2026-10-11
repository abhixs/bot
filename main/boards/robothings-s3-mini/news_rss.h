// Pure RSS helpers for the news tool (no ESP-IDF, unit-tested on a PC): pulls the
// headlines out of a Google News RSS feed as it streams in, without keeping the
// whole (100 kB+) feed in memory.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace news_rss {

struct Headline {
    std::string title;
    std::string source;
    std::string published;  // as in the feed, e.g. "Sun, 11 Oct 2026 06:30:00 GMT"
};

inline std::string DecodeEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '&') {
            out.push_back(in[i]);
            continue;
        }
        const size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 8) {
            out.push_back('&');
            continue;
        }
        const std::string name = in.substr(i + 1, semi - i - 1);
        if (name == "amp") out.push_back('&');
        else if (name == "lt") out.push_back('<');
        else if (name == "gt") out.push_back('>');
        else if (name == "quot") out.push_back('"');
        else if (name == "apos") out.push_back('\'');
        else if (name == "nbsp") out.push_back(' ');
        else if (!name.empty() && name[0] == '#') {
            unsigned long cp = name.size() > 1 && (name[1] == 'x' || name[1] == 'X')
                                   ? strtoul(name.c_str() + 2, nullptr, 16)
                                   : strtoul(name.c_str() + 1, nullptr, 10);
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        } else {
            out.append(in, i, semi - i + 1);  // unknown: keep as is
        }
        i = semi;
    }
    return out;
}

// Text of the first <tag ...>...</tag> in `block` (CDATA unwrapped, entities decoded).
inline std::string TagText(const std::string& block, const char* tag) {
    const std::string open = std::string("<") + tag;
    size_t start = block.find(open);
    while (start != std::string::npos) {
        const char next = start + open.size() < block.size() ? block[start + open.size()] : '\0';
        if (next == '>' || next == ' ') break;  // not e.g. <titles>
        start = block.find(open, start + 1);
    }
    if (start == std::string::npos) return "";
    const size_t content = block.find('>', start);
    if (content == std::string::npos) return "";
    const size_t end = block.find(std::string("</") + tag + ">", content);
    if (end == std::string::npos) return "";
    std::string text = block.substr(content + 1, end - content - 1);
    if (text.rfind("<![CDATA[", 0) == 0 && text.size() >= 12) {
        text = text.substr(9, text.size() - 12);
    }
    text = DecodeEntities(text);
    // single line, trimmed
    std::string out;
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Feed the RSS text in pieces; complete <item>s become headlines.
class Parser {
public:
    explicit Parser(size_t max_items) : max_items_(max_items) {}

    void Feed(const char* data, size_t size) {
        if (done()) return;
        buffer_.append(data, size);
        while (!done()) {
            const size_t start = buffer_.find("<item>");
            if (start == std::string::npos) {
                // Keep a little in case "<item>" is split across pieces.
                if (buffer_.size() > 16) buffer_.erase(0, buffer_.size() - 16);
                return;
            }
            const size_t end = buffer_.find("</item>", start);
            if (end == std::string::npos) {
                buffer_.erase(0, start);
                if (buffer_.size() > kMaxItemBytes) buffer_.clear();  // malformed: skip it
                return;
            }
            AddItem(buffer_.substr(start, end - start));
            buffer_.erase(0, end + 7);
        }
    }

    bool done() const { return headlines_.size() >= max_items_; }
    const std::vector<Headline>& headlines() const { return headlines_; }

private:
    static constexpr size_t kMaxItemBytes = 16 * 1024;

    void AddItem(const std::string& block) {
        Headline h;
        h.title = TagText(block, "title");
        h.source = TagText(block, "source");
        h.published = TagText(block, "pubDate");
        // Google News titles end with " - <source>": drop the repeat.
        const std::string suffix = " - " + h.source;
        if (!h.source.empty() && h.title.size() > suffix.size() &&
            h.title.compare(h.title.size() - suffix.size(), suffix.size(), suffix) == 0) {
            h.title.resize(h.title.size() - suffix.size());
        }
        if (h.title.size() > 200) h.title.resize(200);
        if (!h.title.empty()) headlines_.push_back(h);
    }

    size_t max_items_;
    std::string buffer_;
    std::vector<Headline> headlines_;
};

inline std::string UrlEncode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

// Google News RSS: top stories, or a search (last day) when `query` is given.
// language "hi" for Hindi headlines, otherwise English (India edition).
inline std::string FeedUrl(const std::string& query, const std::string& language) {
    const bool hindi = language == "hi" || language == "hindi";
    const std::string locale = hindi ? "hl=hi&gl=IN&ceid=IN:hi" : "hl=en-IN&gl=IN&ceid=IN:en";
    if (query.empty()) return "https://news.google.com/rss?" + locale;
    return "https://news.google.com/rss/search?q=" + UrlEncode(query) + "+when:1d&" + locale;
}

}  // namespace news_rss
