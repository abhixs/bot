// Speech made on the device itself, for things the cloud assistant cannot be asked
// to say (it only answers the user): the reminder announcement and its replies, and
// the news headlines read out on request.
//
// Each line is fetched as speech (MP3) from the Google Translate voice over HTTPS
// (Hindi voice, or Indian English), decoded, resampled to the speaker rate and
// queued for playback - in its own task, so the main loop never waits on the
// network. Needs internet; on failure the caller gets kFailed.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class DeviceTts {
public:
    enum class Result { kOk, kFailed, kCancelled };
    struct Line {
        std::string text;
        std::string lang = "hi";  // "hi" (Hindi voice, fine for Hinglish) or "en"
    };
    using Done = std::function<void(Result)>;  // called on the main task

    static DeviceTts& GetInstance() {
        static DeviceTts instance;
        return instance;
    }

    // Speaks the lines in order. False if it is already speaking. With `hold` the
    // speech is fetched at once but only played after Release() (e.g. once a sound
    // in front of it has finished).
    bool Speak(std::vector<Line> lines, Done done, bool hold = false);
    void Release() { hold_.store(false); }
    // Fetches the latest headlines (news_feed.h) and reads out the top ones.
    bool SpeakNews(const std::string& query, bool hindi, Done done);
    // Stops as soon as possible (the caller also drops queued audio).
    void Cancel() { cancel_.store(true); }
    bool busy() const { return busy_.load(); }

private:
    DeviceTts() = default;

    struct Job {
        std::vector<Line> lines;
        bool news = false;
        std::string news_query;
        bool news_hindi = false;
        Done done;
    };

    bool Start(Job* job);
    static void TaskEntry(void* arg);
    Result Run(Job& job);
    Result SpeakLine(const Line& line);

    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> hold_{false};
    bool WaitForRelease();
};
