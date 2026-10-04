#include "music_player.h"

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "demuxer/ogg_demuxer.h"
#include "audio_codec.h"
#include "mcp_server.h"
#include "mp3_frame.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_ae_rate_cvt.h>
#include <esp_log.h>
#include <esp_mp3_dec.h>
#include <esp_timer.h>
#include <http.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <network_interface.h>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#define TAG "MusicPlayer"

// The Jamendo client ID is injected at build time from the JAMENDO_CLIENT_ID
// GitHub secret (see .github/workflows/robothings.yml); it is not stored in git.
#if __has_include("jamendo_config.h")
#include "jamendo_config.h"
#endif
#ifndef JAMENDO_CLIENT_ID
#define JAMENDO_CLIENT_ID ""
#endif

namespace {
constexpr int kDiscoveryPort = 47123;
constexpr const char* kNvsNamespace = "music";
constexpr int kStreamChunk = 1024;
constexpr int64_t kStartWaitUs = 25LL * 1000000;    // wait for the AI to finish talking
constexpr int64_t kResumeWaitUs = 180LL * 1000000;  // resume after a conversation
constexpr uint32_t kRewindAfterPauseMs = 1500;      // audio that was queued when paused
constexpr int kMp3BufferSize = 16 * 1024;
constexpr int kMp3Kbps = 96;                       // Jamendo "mp31" streams
constexpr const char* kJamendoApi = "https://api.jamendo.com/v3.0/tracks/";

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
    "robothings_music_server.py on their computer (connected to the same Wi-Fi) and try again. "
    "For free online music you can use self.music.play_online instead.";
constexpr const char* kNoJamendoError =
    "Online music is not set up on this device (missing Jamendo client ID).";
constexpr const char* kJamendoFailError =
    "Could not reach the Jamendo music service. Check the internet connection and try again.";
}  // namespace

void MusicPlayer::Initialize() {
    Settings settings(kNvsNamespace, false);
    host_ = settings.GetString("host", "");
    port_ = settings.GetInt("port", 0);
    RegisterTools();
    xTaskCreate(TaskEntry, "music_player", 10240, this, 3, &task_);
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

// ---------------------------------------------------------------- Jamendo

bool MusicPlayer::HasJamendo() const { return JAMENDO_CLIENT_ID[0] != '\0'; }

std::optional<std::vector<MusicPlayer::Track>> MusicPlayer::SearchOnline(const std::string& query,
                                                                        const std::string& genre,
                                                                        int offset, int limit) {
    std::string url = std::string(kJamendoApi) + "?client_id=" + JAMENDO_CLIENT_ID +
                      "&format=json&audioformat=mp31&limit=" + std::to_string(limit) +
                      "&offset=" + std::to_string(offset);
    if (!query.empty()) {
        url += "&search=" + UrlEncode(query);
    }
    if (!genre.empty()) {
        // Jamendo separates tags with '+' (OR search for fuzzytags).
        std::string tags;
        std::string word;
        for (char c : genre + " ") {
            if (c == ' ' || c == ',' || c == '+') {
                if (!word.empty()) tags += (tags.empty() ? "" : "+") + UrlEncode(word);
                word.clear();
            } else {
                word.push_back(c);
            }
        }
        url += "&fuzzytags=" + tags;
    }
    if (query.empty()) {
        url += "&order=popularity_month";
    }

    auto http = OpenUrl(url, "", 8000);
    if (!http) {
        return std::nullopt;
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        return std::nullopt;
    }
    std::vector<Track> tracks;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, results) {
        const cJSON* id = cJSON_GetObjectItem(item, "id");
        const cJSON* name = cJSON_GetObjectItem(item, "name");
        const cJSON* artist = cJSON_GetObjectItem(item, "artist_name");
        const cJSON* audio = cJSON_GetObjectItem(item, "audio");
        if (!cJSON_IsString(audio) || audio->valuestring[0] == '\0') continue;
        Track track;
        track.online = true;
        track.id = cJSON_IsString(id) ? id->valuestring : "";
        track.title = cJSON_IsString(name) ? name->valuestring : "Unknown";
        track.album = cJSON_IsString(artist) ? artist->valuestring : "";
        track.url = audio->valuestring;
        tracks.push_back(track);
    }
    cJSON_Delete(root);
    return tracks;
}

std::optional<MusicPlayer::Track> MusicPlayer::NextOnline() {
    online_offset_++;
    auto results = SearchOnline(online_query_, online_genre_, online_offset_, 1);
    if ((!results || results->empty()) && online_offset_ > 0) {
        online_offset_ = 0;  // ran out of results: start the list again
        results = SearchOnline(online_query_, online_genre_, online_offset_, 1);
    }
    if (!results || results->empty()) {
        return std::nullopt;
    }
    return results->front();
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
        "self.music.play_online",
        "Play free online music from Jamendo: legal music by independent artists in every genre "
        "(English, instrumental, lofi, rock, pop, electronic, classical, relaxing...). Jamendo does "
        "NOT have Bollywood or other famous label songs; use self.music.play (the user's own "
        "library) for those. Use this when the user asks for online music, a genre or mood, or "
        "just 'some music'. query: optional artist/title words. genre: optional tags such as "
        "'lofi', 'chillout', 'rock', 'piano', 'jazz', 'workout'. More songs keep playing after "
        "the first. Keep your reply short; music starts after it.",
        PropertyList({
            Property("query", kPropertyTypeString, std::string("")).SetMaxLength(80),
            Property("genre", kPropertyTypeString, std::string("")).SetMaxLength(60),
        }),
        [this](const PropertyList& properties) -> ToolResult {
            if (!HasJamendo()) return std::unexpected(std::string(kNoJamendoError));
            std::string query = properties["query"].value<std::string>();
            std::string genre = properties["genre"].value<std::string>();
            auto results = SearchOnline(query, genre, 0, 1);
            if (!results) return std::unexpected(std::string(kJamendoFailError));
            if (results->empty()) {
                return std::unexpected(std::string(
                    "Nothing found on Jamendo for that. Try a genre such as lofi, rock or piano."));
            }
            online_query_ = query;
            online_genre_ = genre;
            online_offset_ = 0;
            const Track& track = results->front();
            Request(track, 0, true);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "playing", track.title.c_str());
            cJSON_AddStringToObject(result, "artist", track.album.c_str());
            cJSON_AddStringToObject(result, "source", "Jamendo");
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

    mcp.AddTool("self.music.next", "Skip to the next song (same source as the current one).",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    auto next = last_track_.online ? NextOnline() : RandomTrack(last_track_.id);
                    if (!next && last_track_.online) {
                        return std::unexpected(std::string(kJamendoFailError));
                    }
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
                auto next = track.online ? NextOnline() : RandomTrack(track.id);
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
    StreamResult result = track.online ? StreamMp3(track, start_ms, generation, position_ms)
                                       : StreamOpus(track, start_ms, generation, position_ms);
    FinishStream(result, generation, position_ms);
    return result;
}

void MusicPlayer::FinishStream(StreamResult result, uint32_t generation, uint32_t& position_ms) {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
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
}

std::unique_ptr<Http> MusicPlayer::OpenUrl(std::string url, const std::string& range,
                                           int timeout_ms) {
    // HttpClient does not follow redirects; CDN links often answer 301/302.
    for (int hop = 0; hop < 4; hop++) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetTimeout(timeout_ms);
        http->SetHeader("User-Agent", "RoboThings-S3-Mini");
        if (!range.empty()) {
            http->SetHeader("Range", range);
        }
        if (!http->Open("GET", url)) {
            return nullptr;
        }
        auto status = http->GetStatusCode();
        if (!status) {
            http->Close();
            return nullptr;
        }
        if (*status == 200 || *status == 206) {
            return http;
        }
        std::string location = http->GetResponseHeader("Location");
        http->Close();
        if ((*status == 301 || *status == 302 || *status == 303 || *status == 307 ||
             *status == 308) &&
            !location.empty()) {
            if (location.rfind("http", 0) != 0) {
                // Relative redirect: keep scheme and host of the current URL.
                size_t host_end = url.find('/', url.find("//") + 2);
                location = url.substr(0, host_end) + (location[0] == '/' ? "" : "/") + location;
            }
            url = location;
            continue;
        }
        ESP_LOGW(TAG, "HTTP %d for %s", *status, url.c_str());
        return nullptr;
    }
    return nullptr;
}

MusicPlayer::StreamResult MusicPlayer::StreamMp3(const Track& track, uint32_t start_ms,
                                                 uint32_t generation, uint32_t& position_ms) {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();

    // Resume by byte range (constant bitrate stream); the frame finder resyncs.
    position_ms = start_ms;
    std::string range;
    if (start_ms > 0) {
        range = "bytes=" + std::to_string(static_cast<uint64_t>(start_ms) * kMp3Kbps / 8) + "-";
    }
    auto http = OpenUrl(track.url, range, 10000);
    if (!http) {
        return StreamResult::kError;
    }
    if (start_ms > 0 && http->GetStatusCode().value_or(200) == 200) {
        position_ms = 0;  // server ignored the range: starting from the top
    }
    ESP_LOGI(TAG, "Streaming online: %s - %s", track.title.c_str(), track.album.c_str());

    void* decoder = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        http->Close();
        return StreamResult::kError;
    }
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_rate = 0;

    std::vector<uint8_t> in(kMp3BufferSize);
    size_t in_len = 0;
    std::vector<uint8_t> pcm_bytes(1152 * 2 * sizeof(int16_t));
    std::vector<int16_t> mono;
    std::vector<int16_t> resampled;
    std::vector<int16_t> pending;
    const size_t push_samples = out_rate / 2;  // queue half a second at a time
    pending.reserve(push_samples + 2048);

    StreamResult result = StreamResult::kFinished;
    bool eof = false;
    bool push_failed = false;

    auto push_pending = [&](bool force) {
        if (pending.empty() || (!force && pending.size() < push_samples)) return;
        if (!audio.PushPcmToPlaybackQueue(std::move(pending), true)) {
            push_failed = true;
        }
        pending = std::vector<int16_t>();
        pending.reserve(push_samples + 2048);
    };

    while (true) {
        if (generation != generation_) {
            result = StreamResult::kCancelled;
            break;
        }
        if (app.GetDeviceState() != kDeviceStateIdle || push_failed) {
            result = StreamResult::kInterrupted;
            break;
        }
        // Top up the input buffer.
        if (!eof && in_len < in.size()) {
            auto n = http->Read(reinterpret_cast<char*>(in.data() + in_len), in.size() - in_len);
            if (!n) {
                ESP_LOGW(TAG, "Online stream read failed");
                result = StreamResult::kError;
                break;
            }
            if (*n == 0) {
                eof = true;
            } else {
                in_len += *n;
            }
        }

        // Decode every whole frame in the buffer.
        size_t offset = 0;
        while (offset < in_len && !push_failed) {
            mp3::FrameInfo frame;
            size_t skip = 0;
            auto found = mp3::FindFrame(in.data() + offset, in_len - offset, frame, skip);
            if (found == mp3::FindResult::kSkip) {
                offset += std::min(skip, in_len - offset);
                continue;
            }
            if (found == mp3::FindResult::kNeedMoreData) {
                // At the very end there is no next header to confirm the last frame.
                if (!(eof && in_len - offset >= 4 && mp3::ParseHeader(in.data() + offset, frame) &&
                      in_len - offset >= frame.length)) {
                    break;
                }
            }
            esp_audio_dec_in_raw_t raw = {
                .buffer = in.data() + offset,
                .len = static_cast<uint32_t>(frame.length),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            esp_audio_dec_out_frame_t out = {
                .buffer = pcm_bytes.data(),
                .len = static_cast<uint32_t>(pcm_bytes.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t info = {};
            esp_audio_err_t ret = esp_mp3_dec_decode(decoder, &raw, &out, &info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_bytes.size()) {
                pcm_bytes.resize(out.needed_size);
                continue;  // retry the same frame with a bigger buffer
            }
            offset += frame.length;
            if (ret != ESP_AUDIO_ERR_OK || out.decoded_size == 0 || info.sample_rate == 0) {
                continue;  // skip a damaged frame
            }

            // Down-mix to mono.
            const int channels = std::max<int>(1, info.channel);
            const size_t frames = out.decoded_size / sizeof(int16_t) / channels;
            const int16_t* samples = reinterpret_cast<const int16_t*>(pcm_bytes.data());
            mono.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                int sum = 0;
                for (int c = 0; c < channels; c++) sum += samples[i * channels + c];
                mono[i] = static_cast<int16_t>(sum / channels);
            }
            position_ms += static_cast<uint32_t>(frames * 1000 / info.sample_rate);

            // Resample to the speaker rate.
            if (static_cast<int>(info.sample_rate) != out_rate) {
                if (resampler == nullptr || resampler_rate != static_cast<int>(info.sample_rate)) {
                    if (resampler != nullptr) esp_ae_rate_cvt_close(resampler);
                    resampler = nullptr;
                    esp_ae_rate_cvt_cfg_t cfg = {};
                    cfg.src_rate = info.sample_rate;
                    cfg.dest_rate = static_cast<uint32_t>(out_rate);
                    cfg.channel = 1;
                    cfg.bits_per_sample = ESP_AUDIO_BIT16;
                    cfg.complexity = 2;
                    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    esp_ae_rate_cvt_open(&cfg, &resampler);  // leaves nullptr on failure
                    resampler_rate = info.sample_rate;
                }
                if (resampler == nullptr) continue;
                uint32_t max_out = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &max_out);
                resampled.resize(max_out);
                uint32_t produced = max_out;
                esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(), mono.size(),
                                        (esp_ae_sample_t)resampled.data(), &produced);
                pending.insert(pending.end(), resampled.begin(), resampled.begin() + produced);
            } else {
                pending.insert(pending.end(), mono.begin(), mono.end());
            }
            push_pending(false);
        }
        // Keep the unparsed tail.
        if (offset > 0) {
            memmove(in.data(), in.data() + offset, in_len - offset);
            in_len -= offset;
        }
        if (eof && (in_len < 4 || offset == 0)) {
            break;  // end of song (any leftover bytes are not a whole frame)
        }
    }
    if (result == StreamResult::kFinished && !push_failed) {
        push_pending(true);
    }
    if (push_failed && result == StreamResult::kFinished) {
        result = StreamResult::kInterrupted;
    }

    http->Close();
    esp_mp3_dec_close(decoder);
    if (resampler != nullptr) esp_ae_rate_cvt_close(resampler);
    return result;
}

MusicPlayer::StreamResult MusicPlayer::StreamOpus(const Track& track, uint32_t start_ms,
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
    return result;
}
