#include "music_player.h"

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "demuxer/ogg_demuxer.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <http.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <network_interface.h>

#include <unistd.h>

#include <cctype>
#include <cstring>
#include <memory>

#define TAG "MusicPlayer"

namespace {
constexpr int kDiscoveryPort = 47123;
constexpr const char* kNvsNamespace = "music";
constexpr int kStreamChunk = 1024;
constexpr int64_t kStartWaitUs = 25LL * 1000000;    // wait for the AI to finish talking
constexpr int64_t kResumeWaitUs = 180LL * 1000000;  // resume after a conversation
constexpr uint32_t kRewindAfterPauseMs = 1500;      // audio that was queued when paused

std::string UrlEncode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

int64_t NowUs() { return esp_timer_get_time(); }

MusicPlayer::Track TrackFromJson(const cJSON* item) {
    MusicPlayer::Track track;
    const cJSON* id = cJSON_GetObjectItem(item, "id");
    const cJSON* title = cJSON_GetObjectItem(item, "title");
    const cJSON* album = cJSON_GetObjectItem(item, "album");
    if (cJSON_IsString(id)) track.id = id->valuestring;
    if (cJSON_IsString(title)) track.title = title->valuestring;
    if (cJSON_IsString(album)) track.album = album->valuestring;
    return track;
}

constexpr const char* kNoServerError =
    "The music server was not found on the Wi-Fi. Tell the user to start "
    "robothings_music_server.py on their computer (connected to the same Wi-Fi) and try again.";
}  // namespace

void MusicPlayer::Initialize() {
    Settings settings(kNvsNamespace, false);
    host_ = settings.GetString("host", "");
    port_ = settings.GetInt("port", 0);
    RegisterTools();
    xTaskCreate(TaskEntry, "music_player", 6144, this, 3, &task_);
}

// ---------------------------------------------------------------- server access

std::string MusicPlayer::BaseUrl() const { return "http://" + host_ + ":" + std::to_string(port_); }

bool MusicPlayer::Discover() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 400000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kDiscoveryPort);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        const char ask[] = "RTMUSIC?";
        sendto(sock, ask, sizeof(ask) - 1, 0, reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
        char reply[64] = {};
        struct sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, reply, sizeof(reply) - 1, 0, reinterpret_cast<struct sockaddr*>(&src),
                         &src_len);
        if (n > 8 && strncmp(reply, "RTMUSIC ", 8) == 0) {
            int port = atoi(reply + 8);
            if (port > 0 && port < 65536) {
                char ip[16];
                inet_ntoa_r(src.sin_addr, ip, sizeof(ip));
                host_ = ip;
                port_ = port;
                found = true;
            }
        }
    }
    close(sock);

    if (found) {
        ESP_LOGI(TAG, "Music server at %s:%d", host_.c_str(), port_);
        Settings settings(kNvsNamespace, true);
        settings.SetString("host", host_);
        settings.SetInt("port", port_);
    } else {
        ESP_LOGW(TAG, "Music server not found");
    }
    return found;
}

bool MusicPlayer::EnsureServer() { return (!host_.empty() && port_ > 0) || Discover(); }

std::optional<std::string> MusicPlayer::HttpGet(const std::string& path, int timeout_ms) {
    // Try the cached server first; if it does not answer, look for it again once.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1 && !Discover()) {
            break;
        }
        if (!EnsureServer()) {
            continue;
        }
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetTimeout(timeout_ms);
        if (!http->Open("GET", BaseUrl() + path)) {
            continue;
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            http->Close();
            continue;
        }
        std::string body = http->ReadAll();
        http->Close();
        return body;
    }
    return std::nullopt;
}

std::optional<std::vector<MusicPlayer::Track>> MusicPlayer::Search(const std::string& query,
                                                                  int limit) {
    auto body = HttpGet("/api/search?q=" + UrlEncode(query) + "&limit=" + std::to_string(limit));
    if (!body) {
        return std::nullopt;
    }
    std::vector<Track> tracks;
    cJSON* root = cJSON_Parse(body->c_str());
    cJSON* results = cJSON_GetObjectItem(root, "results");
    cJSON* item = nullptr;
    if (cJSON_IsArray(results)) {
        cJSON_ArrayForEach(item, results) {
            Track track = TrackFromJson(item);
            if (!track.id.empty()) tracks.push_back(track);
        }
    }
    cJSON_Delete(root);
    return tracks;
}

std::optional<MusicPlayer::Track> MusicPlayer::RandomTrack(const std::string& exclude_id) {
    auto body = HttpGet("/api/random?exclude=" + UrlEncode(exclude_id));
    if (!body) {
        return std::nullopt;
    }
    cJSON* root = cJSON_Parse(body->c_str());
    cJSON* result = cJSON_GetObjectItem(root, "result");
    std::optional<Track> track;
    if (cJSON_IsObject(result)) {
        track = TrackFromJson(result);
        if (track->id.empty()) track.reset();
    }
    cJSON_Delete(root);
    return track;
}

// ---------------------------------------------------------------- MCP tools

void MusicPlayer::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.music.play",
        "Play a song from the user's OWN music library (songs on their computer, shared by the "
        "RoboThings music server). Use it whenever the user asks to play a song, gaana, music or "
        "an artist/film. query: song name and/or singer or film, in Latin letters (e.g. 'tum hi "
        "ho', 'arijit singh', 'kesariya'). Leave query empty or set shuffle=true to play random "
        "songs one after another. Music starts after your reply, so keep the reply short.",
        PropertyList({
            Property("query", kPropertyTypeString, std::string("")).SetMaxLength(80),
            Property("shuffle", kPropertyTypeBoolean, false),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            std::string query = properties["query"].value<std::string>();
            bool shuffle = properties["shuffle"].value<bool>() || query.empty();
            Track track;
            std::vector<Track> others;
            if (shuffle && query.empty()) {
                auto random = RandomTrack("");
                if (!random) return std::unexpected(std::string(kNoServerError));
                track = *random;
            } else {
                auto results = Search(query, 4);
                if (!results) return std::unexpected(std::string(kNoServerError));
                if (results->empty()) {
                    return std::unexpected("No song matching '" + query +
                                           "' in the user's music library. Tell the user it is "
                                           "not in their collection.");
                }
                track = results->front();
                others.assign(results->begin() + 1, results->end());
            }
            Request(track, 0, shuffle);

            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "playing", track.title.c_str());
            if (!track.album.empty()) cJSON_AddStringToObject(result, "album", track.album.c_str());
            cJSON_AddBoolToObject(result, "shuffle", shuffle);
            if (!others.empty()) {
                cJSON* list = cJSON_AddArrayToObject(result, "other_matches");
                for (const auto& t : others) cJSON_AddItemToArray(list, cJSON_CreateString(t.title.c_str()));
            }
            return result;
        });

    mcp.AddTool(
        "self.music.search",
        "Search the user's own music library and list matching songs without playing them.",
        PropertyList({Property("query", kPropertyTypeString).SetMaxLength(80)}),
        [this](const PropertyList& properties) -> ToolResult {
            auto results = Search(properties["query"].value<std::string>(), 8);
            if (!results) return std::unexpected(std::string(kNoServerError));
            cJSON* list = cJSON_CreateArray();
            for (const auto& t : *results) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "title", t.title.c_str());
                if (!t.album.empty()) cJSON_AddStringToObject(item, "album", t.album.c_str());
                cJSON_AddItemToArray(list, item);
            }
            return list;
        });

    mcp.AddTool("self.music.stop",
                "Stop or pause the music. Call this whenever the user says stop, pause, band karo, "
                "or 'enough music'.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    Stop();
                    return true;
                });

    mcp.AddTool("self.music.resume", "Resume the last song from where it was stopped.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    if (last_track_.id.empty()) {
                        return std::unexpected(std::string("Nothing to resume. Ask what to play."));
                    }
                    Request(last_track_, last_position_ms_, shuffle_);
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddStringToObject(result, "resuming", last_track_.title.c_str());
                    return result;
                });

    mcp.AddTool("self.music.next", "Skip to another (random) song from the user's library.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    auto next = RandomTrack(last_track_.id);
                    if (!next) return std::unexpected(std::string(kNoServerError));
                    Request(*next, 0, true);
                    cJSON* result = cJSON_CreateObject();
                    cJSON_AddStringToObject(result, "playing", next->title.c_str());
                    return result;
                });
}

// ---------------------------------------------------------------- playback

void MusicPlayer::Request(const Track& track, uint32_t start_ms, bool shuffle) {
    std::lock_guard<std::mutex> lock(mutex_);
    request_track_ = track;
    request_start_ms_ = start_ms;
    has_request_ = true;
    shuffle_ = shuffle;
    generation_++;
    cv_.notify_all();
}

void MusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_request_ = false;
        generation_++;
    }
    if (playing_) {
        SetPlaying(false, "");
    }
}

void MusicPlayer::SetPlaying(bool playing, const std::string& title) {
    playing_ = playing;
    if (playing) {
        if (on_now_playing_) on_now_playing_(title);
    } else {
        if (on_stopped_) on_stopped_();
    }
}

void MusicPlayer::TaskEntry(void* arg) { static_cast<MusicPlayer*>(arg)->TaskLoop(); }

void MusicPlayer::TaskLoop() {
    while (true) {
        Track track;
        uint32_t position_ms = 0;
        uint32_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return has_request_; });
            track = request_track_;
            position_ms = request_start_ms_;
            has_request_ = false;
            generation = generation_;
        }

        bool fresh = true;
        while (true) {
            if (!WaitForIdle(generation, fresh)) {
                break;
            }
            fresh = false;
            SetPlaying(true, track.title);
            StreamResult result = StreamTrack(track, position_ms, generation, position_ms);
            last_track_ = track;
            last_position_ms_ = position_ms;

            if (result == StreamResult::kCancelled) {
                break;  // Stop() or a newer request already updated the state
            }
            SetPlaying(false, "");
            if (result == StreamResult::kInterrupted) {
                // Someone said the wake word: pause, then carry on after the chat.
                if (!WaitUntilIdleAgain(generation)) break;
                continue;
            }
            if (result == StreamResult::kFinished && shuffle_ && generation == generation_) {
                auto next = RandomTrack(track.id);
                if (!next) break;
                track = *next;
                position_ms = 0;
                continue;
            }
            if (result == StreamResult::kFinished) {
                last_position_ms_ = 0;
            }
            break;
        }
    }
}

bool MusicPlayer::WaitForIdle(uint32_t generation, bool fresh_request) {
    auto& app = Application::GetInstance();
    const int64_t start = NowUs();
    int64_t last_toggle = 0;
    bool saw_speaking = false;
    while (generation == generation_) {
        DeviceState state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            return true;
        }
        if (state == kDeviceStateSpeaking) {
            saw_speaking = true;  // the AI is announcing the song; let it finish
        }
        int64_t elapsed = NowUs() - start;
        // After the AI's reply the device keeps listening; end that conversation so the
        // song can start. Do not cut the reply itself: wait until it was spoken (or a
        // few seconds passed without one).
        if (state == kDeviceStateListening && (saw_speaking || elapsed > 7LL * 1000000) &&
            NowUs() - last_toggle > 2LL * 1000000) {
            app.ToggleChatState();
            last_toggle = NowUs();
        }
        if (elapsed > (fresh_request ? kStartWaitUs : kResumeWaitUs)) {
            ESP_LOGW(TAG, "Device did not become idle; not starting music");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

bool MusicPlayer::WaitUntilIdleAgain(uint32_t generation) {
    auto& app = Application::GetInstance();
    const int64_t start = NowUs();
    // Give the conversation a moment to begin before checking for idle.
    vTaskDelay(pdMS_TO_TICKS(1500));
    while (generation == generation_) {
        if (app.GetDeviceState() == kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(800));  // stays idle, not just between states
            if (generation == generation_ && app.GetDeviceState() == kDeviceStateIdle) {
                return true;
            }
        }
        if (NowUs() - start > kResumeWaitUs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

MusicPlayer::StreamResult MusicPlayer::StreamTrack(const Track& track, uint32_t start_ms,
                                                   uint32_t generation, uint32_t& position_ms) {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    const uint32_t start_s = start_ms / 1000;
    position_ms = start_s * 1000;

    if (!EnsureServer()) {
        return StreamResult::kError;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(10000);
    std::string path = "/api/stream/" + track.id + "?start=" + std::to_string(start_s);
    if (!http->Open("GET", BaseUrl() + path)) {
        if (!Discover()) return StreamResult::kError;
        http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetTimeout(10000);
        if (!http->Open("GET", BaseUrl() + path)) return StreamResult::kError;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        http->Close();
        return StreamResult::kError;
    }
    ESP_LOGI(TAG, "Streaming %s from %lus", track.title.c_str(), static_cast<unsigned long>(start_s));

    auto demuxer = std::make_unique<OggDemuxer>();
    bool push_failed = false;
    demuxer->OnPacket([&](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t len) {
        if (push_failed) return;
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->payload.assign(data, data + len);
        // Blocks while the decode queue is full: this paces the download.
        if (audio.PushPacketToDecodeQueue(std::move(packet), true)) {
            position_ms += frame_duration_ms;
        } else {
            push_failed = true;  // the decoder was reset (conversation started)
        }
    });

    std::unique_ptr<char[]> buffer(new char[kStreamChunk]);
    StreamResult result = StreamResult::kFinished;
    while (true) {
        if (generation != generation_) {
            result = StreamResult::kCancelled;
            break;
        }
        if (app.GetDeviceState() != kDeviceStateIdle || push_failed) {
            result = StreamResult::kInterrupted;
            break;
        }
        auto n = http->Read(buffer.get(), kStreamChunk);
        if (!n) {
            ESP_LOGW(TAG, "Stream read failed");
            result = StreamResult::kError;
            break;
        }
        if (*n == 0) {
            break;  // end of song
        }
        demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.get()), *n);
    }
    http->Close();

    if (result == StreamResult::kFinished) {
        // Let the queued tail of the song play out.
        int64_t wait_start = NowUs();
        while (!audio.IsPlaybackIdle() && generation == generation_ &&
               app.GetDeviceState() == kDeviceStateIdle && NowUs() - wait_start < 4LL * 1000000) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else if (result == StreamResult::kInterrupted || result == StreamResult::kCancelled) {
        // Drop the queued music so it does not play over the conversation or the next song.
        if (result == StreamResult::kInterrupted || app.GetDeviceState() == kDeviceStateIdle) {
            audio.ResetDecoder();
        }
        position_ms = position_ms > kRewindAfterPauseMs ? position_ms - kRewindAfterPauseMs : 0;
    }
    return result;
}
