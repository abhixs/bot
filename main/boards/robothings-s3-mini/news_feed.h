// Latest news for the RoboThings S3 Mini: the MCP tool self.news.latest fetches the
// current headlines live from Google News (RSS, no key needed) - top stories or a
// search on a topic - and hands them to the AI, which reads out a short summary.
#pragma once

#include "news_rss.h"

#include <string>
#include <vector>

void RegisterNewsTools();

// Downloads the current headlines (blocking, up to ~6 s). `language` "hi" = Hindi.
bool FetchHeadlines(const std::string& query, const std::string& language, size_t max_items,
                    std::vector<news_rss::Headline>& out, std::string& error);
