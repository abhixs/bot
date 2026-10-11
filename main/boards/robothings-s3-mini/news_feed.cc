#include "news_feed.h"

#include "alarm_schedule.h"
#include "board.h"
#include "mcp_server.h"
#include "news_rss.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#define TAG "News"

namespace {

constexpr int kTimeoutMs = 6000;
constexpr size_t kChunk = 2048;
constexpr size_t kMaxFeedBytes = 160 * 1024;  // stop reading after this much

// Downloads the feed (following up to 2 redirects) and keeps the first headlines.
bool FetchHeadlines(const std::string& first_url, size_t max_items,
                    std::vector<news_rss::Headline>& out, std::string& error) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "No network.";
        return false;
    }
    std::string url = first_url;
    for (int hop = 0; hop < 3; hop++) {
        auto http = network->CreateHttp(0);
        http->SetTimeout(kTimeoutMs);
        http->SetHeader("User-Agent", "Mozilla/5.0 (RoboThings voice assistant)");
        http->SetHeader("Accept", "application/rss+xml, application/xml, text/xml");
        if (!http->Open("GET", url)) {
            error = "Could not reach the news server (no internet?).";
            return false;
        }
        auto status = http->GetStatusCode();
        if (!status) {
            http->Close();
            error = "No answer from the news server.";
            return false;
        }
        if (*status == 301 || *status == 302 || *status == 303 || *status == 307 ||
            *status == 308) {
            const std::string location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty()) break;
            url = location;
            continue;
        }
        if (*status != 200) {
            http->Close();
            error = "The news server answered " + std::to_string(*status) + ".";
            return false;
        }
        news_rss::Parser parser(max_items);
        std::unique_ptr<char[]> buffer(new char[kChunk]);
        size_t total = 0;
        while (!parser.done() && total < kMaxFeedBytes) {
            auto n = http->Read(buffer.get(), kChunk);
            if (!n || *n <= 0) break;
            total += static_cast<size_t>(*n);
            parser.Feed(buffer.get(), static_cast<size_t>(*n));
        }
        http->Close();
        out = parser.headlines();
        ESP_LOGI(TAG, "%d headline(s) from %u bytes", static_cast<int>(out.size()),
                 static_cast<unsigned>(total));
        if (out.empty()) {
            error = "No news found for that.";
            return false;
        }
        return true;
    }
    error = "The news server kept redirecting.";
    return false;
}

std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

void RegisterNewsTools() {
    McpServer::GetInstance().AddTool(
        "self.news.latest",
        "Get the LATEST news live from the web (Google News, India edition): today's top "
        "headlines, or the newest news on a topic. ALWAYS call this for 'latest news', "
        "'aaj ki taaza khabar', 'news batao', 'what's happening in ...', 'cricket ki news', "
        "'Gurugram news'; never answer news from memory.\n"
        "query: topic to search ('cricket', 'stock market', 'Gurugram', 'ISRO'); empty = top "
        "stories. language: 'en' (default) or 'hi' for Hindi headlines.\n"
        "Then tell the user the 4-5 most important headlines in one short sentence each, in "
        "their language, without reading out sources or times unless asked.",
        PropertyList({
            Property("query", kPropertyTypeString, std::string("")).SetMaxLength(60),
            Property("language", kPropertyTypeString, std::string("en")).SetMaxLength(8),
            Property("count", kPropertyTypeInteger, 6, 1, 10),
        }),
        [](const PropertyList& properties) -> ToolResult {
            const std::string query = properties["query"].value<std::string>();
            const std::string language = Lower(properties["language"].value<std::string>());
            const int count = properties["count"].value<int>();
            std::vector<news_rss::Headline> headlines;
            std::string error;
            if (!FetchHeadlines(news_rss::FeedUrl(query, language), count, headlines, error)) {
                return std::unexpected(error);
            }
            cJSON* result = cJSON_CreateObject();
            const time_t now = time(nullptr);
            if (alarm_schedule::IsTimeValid(now)) {
                cJSON_AddStringToObject(result, "fetched_at",
                                        alarm_schedule::FormatDateTime(now).c_str());
            }
            cJSON_AddStringToObject(result, "source", "Google News");
            if (!query.empty()) cJSON_AddStringToObject(result, "query", query.c_str());
            cJSON* list = cJSON_AddArrayToObject(result, "headlines");
            for (const auto& h : headlines) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "title", h.title.c_str());
                if (!h.source.empty()) cJSON_AddStringToObject(item, "source", h.source.c_str());
                if (!h.published.empty()) {
                    cJSON_AddStringToObject(item, "published_gmt", h.published.c_str());
                }
                cJSON_AddItemToArray(list, item);
            }
            return result;
        });
}
