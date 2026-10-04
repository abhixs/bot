// Plays songs from the user's own music library on their computer.
//
// tools/music-server/robothings_music_server.py runs on a PC in the same Wi-Fi,
// indexes a folder of songs and streams any of them as Opus/Ogg (24 kHz mono,
// 60 ms frames). The device finds it with a UDP broadcast, the AI picks songs
// through the self.music.* MCP tools, and the stream is fed straight into the
// firmware's normal audio decode queue.
//
// While music plays the device stays idle, so the wake word still works: saying
// "Alexa" pauses the song, and it resumes after the conversation unless the user
// asked to stop.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class MusicPlayer {
public:
    struct Track {
        std::string id;
        std::string title;
        std::string album;
    };

    static MusicPlayer& GetInstance() {
        static MusicPlayer instance;
        return instance;
    }

    void Initialize();
    bool IsPlaying() const { return playing_; }
    // Stops playback for good (no auto-resume).
    void Stop();

    // Called from the player task; schedule UI work onto the main task yourself.
    void OnNowPlaying(std::function<void(const std::string& title)> cb) { on_now_playing_ = std::move(cb); }
    void OnStopped(std::function<void()> cb) { on_stopped_ = std::move(cb); }

private:
    MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    void RegisterTools();
    static void TaskEntry(void* arg);
    void TaskLoop();

    // Server access
    bool Discover();
    bool EnsureServer();
    std::optional<std::string> HttpGet(const std::string& path, int timeout_ms = 4000);
    std::optional<std::vector<Track>> Search(const std::string& query, int limit);
    std::optional<Track> RandomTrack(const std::string& exclude_id);
    std::string BaseUrl() const;

    // Playback
    void Request(const Track& track, uint32_t start_ms, bool shuffle);
    enum class StreamResult { kFinished, kInterrupted, kCancelled, kError };
    StreamResult StreamTrack(const Track& track, uint32_t start_ms, uint32_t generation,
                             uint32_t& position_ms);
    bool WaitForIdle(uint32_t generation, bool fresh_request);
    bool WaitUntilIdleAgain(uint32_t generation);
    void SetPlaying(bool playing, const std::string& title);

    std::mutex mutex_;
    std::condition_variable cv_;
    TaskHandle_t task_ = nullptr;

    std::string host_;
    int port_ = 0;

    // Pending request (guarded by mutex_)
    bool has_request_ = false;
    Track request_track_;
    uint32_t request_start_ms_ = 0;

    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> playing_{false};
    bool shuffle_ = false;

    // Last song and where it stopped, for "resume".
    Track last_track_;
    uint32_t last_position_ms_ = 0;

    std::function<void(const std::string&)> on_now_playing_;
    std::function<void()> on_stopped_;
};
