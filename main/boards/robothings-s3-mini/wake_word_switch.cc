#include "wake_word_switch.h"

#include "application.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#include <http.h>
#include <network_interface.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#define TAG "WakeWordSwitch"

// Mirrors of the published wake-word assets (';'-separated base URLs), written by
// CI into robothings_build_config.h: jsDelivr, raw.githubusercontent.com and GitHub
// Pages. Some Indian ISPs block raw.githubusercontent.com, so the device checks
// which one answers before it restarts to download.
#if __has_include("robothings_build_config.h")
#include "robothings_build_config.h"
#endif
#ifndef WAKEWORD_ASSETS_URLS
#define WAKEWORD_ASSETS_URLS ""
#endif

namespace {

struct WakeWord {
    const char* name;   // what the user says
    const char* model;  // ESP-SR WakeNet model (must match tools/wakeword-assets list)
};

// English WakeNet9 models for the ESP32-S3. Keep in sync with
// tools/wakeword-assets/make_wakeword_assets.py.
constexpr WakeWord kWakeWords[] = {
    {"Alexa", "wn9_alexa"},
    {"Hi ESP", "wn9_hiesp"},
    {"Jarvis", "wn9_jarvis_tts"},
    {"Computer", "wn9_computer_tts"},
    {"Sophia", "wn9_sophia_tts"},
    {"Mycroft", "wn9_mycroft_tts"},
    {"Hi Joy", "wn9_hijoy_tts"},
    {"Hi Jason", "wn9_hijason_tts2"},
    {"Hi Andy", "wn9_hiandy_tts2"},
    {"Hey Willow", "wn9_heywillow_tts"},
    {"Hey Wanda", "wn9_heywanda_tts"},
    {"Hey Ivy", "wn9_heyivy_tts2"},
    {"Hey Kira", "wn9_heykira_tts3"},
    {"Hi Lily", "wn9_hilili_tts"},
    {"Hi Telly", "wn9_hitelly_tts"},
    {"Hi Wall E", "wn9_hiwalle_tts2"},
    {"Nihao Xiaozhi", "wn9_nihaoxiaozhi_tts"},
};

std::string Normalize(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (isalnum(c)) out.push_back(static_cast<char>(tolower(c)));
    }
    return out;
}

const WakeWord* Find(const std::string& name) {
    std::string wanted = Normalize(name);
    if (wanted.empty()) return nullptr;
    for (const auto& w : kWakeWords) {
        if (Normalize(w.name) == wanted || Normalize(w.model) == wanted) return &w;
    }
    // "hey jarvis" -> "jarvis", "ok computer" -> "computer"
    for (const auto& w : kWakeWords) {
        std::string n = Normalize(w.name);
        if (n.size() >= 4 && (wanted.find(n) != std::string::npos || n.find(wanted) != std::string::npos)) {
            return &w;
        }
    }
    return nullptr;
}

std::string NameList() {
    std::string list;
    for (const auto& w : kWakeWords) {
        if (!list.empty()) list += ", ";
        list += w.name;
    }
    return list;
}

// The wake word that is really installed: look for a known model name inside the
// srmodels.bin of the assets partition.
const WakeWord* InstalledWakeWord() {
    void* ptr = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("srmodels.bin", ptr, size) || ptr == nullptr) {
        return nullptr;
    }
    const char* data = static_cast<const char*>(ptr);
    for (const auto& w : kWakeWords) {
        const size_t len = strlen(w.model);
        // Match the exact model name (not a prefix of a longer one, e.g. tts vs tts2).
        for (const char* p = static_cast<const char*>(memmem(data, size, w.model, len)); p != nullptr;
             p = static_cast<const char*>(memmem(p + 1, size - (p + 1 - data), w.model, len))) {
            const size_t end = (p - data) + len;
            if (end >= size || !isalnum(static_cast<unsigned char>(data[end]))) return &w;
            if (p + 1 >= data + size) break;
        }
    }
    return nullptr;
}

std::string InstalledName() {
    const WakeWord* w = InstalledWakeWord();
    return w != nullptr ? w->name : "Alexa";
}

std::vector<std::string> MirrorBases() {
    std::vector<std::string> bases;
    std::string all = WAKEWORD_ASSETS_URLS;
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find(';', start);
        if (end == std::string::npos) end = all.size();
        if (end > start) bases.push_back(all.substr(start, end - start));
        start = end + 1;
    }
    return bases;
}

std::string HostOf(const std::string& url) {
    size_t begin = url.find("//");
    begin = begin == std::string::npos ? 0 : begin + 2;
    size_t end = url.find('/', begin);
    return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// Checks a URL the same way the assets updater will use it: a direct 200 answer
// (no redirect) with a Content-Length. Reads only the headers.
bool Probe(const std::string& url, std::string& problem) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(5000);
    if (auto opened = http->Open("GET", url); !opened) {
        problem = "cannot connect (" + opened.error().ToString() + ")";
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        problem = "no answer (" + status.error().ToString() + ")";
        http->Close();
        return false;
    }
    if (*status != 200) {
        problem = "HTTP " + std::to_string(*status);
        http->Close();
        return false;
    }
    if (http->GetBodyLength() == 0) {
        problem = "no Content-Length";
        http->Close();
        return false;
    }
    http->Close();
    return true;
}

void RebootSoon() {
    // Give the assistant time to say the confirmation before restarting.
    static esp_timer_handle_t timer = nullptr;
    if (timer == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void*) {
                Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wakeword_reboot",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &timer);
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, 8 * 1000 * 1000);
}

// After a switch the device reboots, downloads the new assets during start-up and
// then needs one more restart, because the voice engine only loads its model at
// boot. This watcher does that second restart once the device is idle again.
void StartPendingSwitchWatcher() {
    Settings settings("wakeword", false);
    if (settings.GetString("pending", "").empty()) {
        return;
    }
    static esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {
        .callback =
            [](void*) {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() != kDeviceStateIdle) return;
                Settings assets("assets", false);
                if (!assets.GetString("download_url", "").empty()) return;  // not tried yet
                std::string pending;
                {
                    Settings wake("wakeword", true);
                    pending = wake.GetString("pending", "");
                    wake.EraseKey("pending");
                }
                esp_timer_stop(timer);
                const WakeWord* installed = InstalledWakeWord();
                if (installed == nullptr || pending != installed->model) {
                    ESP_LOGW(TAG, "Wake word download failed, keeping the old one");
                    app.Schedule([]() {
                        if (auto display = Board::GetInstance().GetDisplay()) {
                            display->ShowNotification("Wake word change failed", 6000);
                        }
                    });
                    return;
                }
                ESP_LOGI(TAG, "New wake word downloaded, restarting to load it");
                app.Schedule([]() { Application::GetInstance().Reboot(); });
            },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wakeword_watch",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 3 * 1000 * 1000);
    }
}

}  // namespace

void RegisterWakeWordTools() {
    StartPendingSwitchWatcher();

    auto& mcp = McpServer::GetInstance();
    const std::string names = NameList();

    mcp.AddTool("self.wake_word.get",
                "Get the device's current wake word and the wake words it can switch to.",
                PropertyList(), [names](const PropertyList&) -> ToolResult {
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddStringToObject(result, "current", InstalledName().c_str());
                    cJSON_AddStringToObject(result, "available", names.c_str());
                    return result;
                });

    mcp.AddTool(
        "self.wake_word.set",
        "Change the wake word (the name the device listens for). Only these names are "
        "possible: " + names +
            ". The device restarts, downloads the new voice model (needs internet, about one "
            "minute) and then answers to the new name. Tell the user this before calling. If "
            "the user asks for another name, explain it is not available and list the options.",
        PropertyList({Property("name", kPropertyTypeString).SetMaxLength(40)}),
        [names](const PropertyList& properties) -> ToolResult {
            const std::vector<std::string> bases = MirrorBases();
            if (bases.empty()) {
                return std::unexpected(std::string(
                    "Wake word switching is not set up in this firmware build."));
            }
            const WakeWord* word = Find(properties["name"].value<std::string>());
            if (word == nullptr) {
                return std::unexpected("That wake word is not available. Possible names: " +
                                       names + ".");
            }
            if (InstalledName() == word->name) {
                return std::string("{\"success\":true,\"note\":\"already using this wake word\"}");
            }
            // Find a mirror this network can reach before restarting.
            std::string url;
            std::string problems;
            for (const auto& base : bases) {
                std::string problem;
                const std::string candidate = base + word->model + ".bin";
                if (Probe(candidate, problem)) {
                    url = candidate;
                    break;
                }
                ESP_LOGW(TAG, "%s: %s", HostOf(base).c_str(), problem.c_str());
                problems += (problems.empty() ? "" : "; ") + HostOf(base) + ": " + problem;
            }
            if (url.empty()) {
                return std::unexpected(
                    "Could not reach any download server for the wake word (" + problems +
                    "). The internet provider may be blocking them. Tell the user to try again "
                    "on another Wi-Fi or a mobile hotspot, or to switch on GitHub Pages for the "
                    "repository. The wake word was not changed.");
            }
            ESP_LOGI(TAG, "Wake word download URL: %s", url.c_str());
            {
                Settings assets("assets", true);
                assets.SetString("download_url", url);
            }
            {
                Settings settings("wakeword", true);
                settings.SetString("pending", word->model);
            }
            ESP_LOGI(TAG, "Switching wake word to %s (%s)", word->name, word->model);
            RebootSoon();
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "new_wake_word", word->name);
            cJSON_AddStringToObject(result, "note",
                                    "Restarting in a few seconds to download the new wake word.");
            return result;
        });
}
