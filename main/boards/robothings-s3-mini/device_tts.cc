#include "device_tts.h"

#include "application.h"
#include "board.h"
#include "mp3_frame.h"
#include "news_feed.h"
#include "news_rss.h"
#include "pcm_resample.h"

#include <esp_log.h>
#include <esp_mp3_dec.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#define TAG "DeviceTts"

namespace {

constexpr int kTimeoutMs = 8000;
constexpr size_t kMaxMp3Bytes = 256 * 1024;
constexpr size_t kMaxChunkChars = 180;  // the voice service takes short texts
constexpr int kHeadlines = 5;

// Splits at spaces into pieces of at most kMaxChunkChars bytes.
std::vector<std::string> SplitText(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = std::min(text.size(), start + kMaxChunkChars);
        if (end < text.size()) {
            size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > start) end = space;
        }
        std::string piece = text.substr(start, end - start);
        if (!piece.empty()) out.push_back(piece);
        start = end;
        while (start < text.size() && text[start] == ' ') start++;
    }
    return out;
}

std::string TtsUrl(const std::string& text, const std::string& lang) {
    // Indian English from the .co.in host; the Hindi voice also reads Hinglish well.
    const bool english = lang == "en";
    return std::string(english ? "https://translate.google.co.in" : "https://translate.google.com") +
           "/translate_tts?ie=UTF-8&client=tw-ob&tl=" + (english ? "en" : "hi") +
           "&q=" + news_rss::UrlEncode(text);
}

bool Download(const std::string& url, std::string& body) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;
    auto http = network->CreateHttp(0);
    http->SetTimeout(kTimeoutMs);
    http->SetHeader("User-Agent", "Mozilla/5.0");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Voice service not reachable");
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGW(TAG, "Voice service answered %d", status ? *status : -1);
        http->Close();
        return false;
    }
    body.clear();
    char buffer[1024];
    while (body.size() < kMaxMp3Bytes) {
        auto n = http->Read(buffer, sizeof(buffer));
        if (!n || *n <= 0) break;
        body.append(buffer, static_cast<size_t>(*n));
    }
    http->Close();
    return !body.empty();
}

}  // namespace

bool DeviceTts::Speak(std::vector<Line> lines, Done done, bool hold) {
    auto* job = new Job();
    job->lines = std::move(lines);
    job->done = std::move(done);
    if (busy_.load()) {
        delete job;
        return false;
    }
    hold_.store(hold);
    return Start(job);
}

bool DeviceTts::WaitForRelease() {
    while (hold_.load()) {
        if (cancel_.load()) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !cancel_.load();
}

bool DeviceTts::SpeakNews(const std::string& query, bool hindi, Done done) {
    hold_.store(false);
    auto* job = new Job();
    job->news = true;
    job->news_query = query;
    job->news_hindi = hindi;
    job->done = std::move(done);
    return Start(job);
}

bool DeviceTts::Start(Job* job) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        delete job;
        return false;
    }
    cancel_.store(false);
    if (xTaskCreate(TaskEntry, "device_tts", 10 * 1024, job, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the speech task");
        busy_.store(false);
        delete job;
        return false;
    }
    return true;
}

void DeviceTts::TaskEntry(void* arg) {
    std::unique_ptr<Job> job(static_cast<Job*>(arg));
    auto& self = GetInstance();
    const Result result = self.Run(*job);
    self.busy_.store(false);
    Done done = std::move(job->done);
    job.reset();
    if (done) {
        Application::GetInstance().Schedule([done, result]() { done(result); });
    }
    vTaskDelete(nullptr);
}

DeviceTts::Result DeviceTts::Run(Job& job) {
    if (job.news) {
        std::vector<news_rss::Headline> headlines;
        std::string error;
        if (!FetchHeadlines(job.news_query, job.news_hindi ? "hi" : "en", kHeadlines, headlines,
                            error)) {
            ESP_LOGW(TAG, "News: %s", error.c_str());
            return Result::kFailed;
        }
        if (cancel_.load()) return Result::kCancelled;
        const std::string lang = job.news_hindi ? "hi" : "en";
        std::string intro;
        if (job.news_hindi) {
            intro = job.news_query.empty() ? "आज की ताज़ा खबरें।"
                                           : job.news_query + " की ताज़ा खबरें।";
        } else {
            intro = job.news_query.empty() ? "Here are the latest headlines."
                                           : "Here is the latest news on " + job.news_query + ".";
        }
        job.lines.push_back({intro, lang});
        for (const auto& h : headlines) job.lines.push_back({h.title + ".", lang});
    }
    for (const auto& line : job.lines) {
        const Result r = SpeakLine(line);
        if (r != Result::kOk) return r;
    }
    return Result::kOk;
}

// Decodes a whole MP3 answer to mono PCM at `out_rate`. False if nothing decoded.
static bool DecodeMp3(const std::string& mp3, int out_rate, std::vector<int16_t>& pcm) {
    void* decoder = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        return false;
    }
    std::unique_ptr<PcmResampler> resampler;
    std::vector<uint8_t> pcm_bytes(1152 * 2 * sizeof(int16_t));
    std::vector<int16_t> mono;
    const auto* data = reinterpret_cast<const uint8_t*>(mp3.data());
    size_t offset = 0;
    while (offset < mp3.size()) {
        mp3::FrameInfo frame;
        size_t skip = 0;
        auto found = mp3::FindFrame(data + offset, mp3.size() - offset, frame, skip);
        if (found == mp3::FindResult::kSkip) {
            offset += std::max<size_t>(1, std::min(skip, mp3.size() - offset));
            continue;
        }
        if (found == mp3::FindResult::kNeedMoreData) {
            // The last frame has no next header to confirm it.
            if (!(mp3.size() - offset >= 4 && mp3::ParseHeader(data + offset, frame) &&
                  mp3.size() - offset >= frame.length)) {
                break;
            }
        }
        esp_audio_dec_in_raw_t raw = {
            .buffer = const_cast<uint8_t*>(data + offset),
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
            continue;  // the same frame again with a bigger buffer
        }
        offset += frame.length;
        if (ret != ESP_AUDIO_ERR_OK || out.decoded_size == 0 || info.sample_rate == 0) {
            continue;  // skip a damaged frame
        }
        const int channels = std::max<int>(1, info.channel);
        const size_t frames = out.decoded_size / sizeof(int16_t) / channels;
        const auto* samples = reinterpret_cast<const int16_t*>(pcm_bytes.data());
        mono.resize(frames);
        for (size_t i = 0; i < frames; i++) {
            int sum = 0;
            for (int c = 0; c < channels; c++) sum += samples[i * channels + c];
            mono[i] = static_cast<int16_t>(sum / channels);
        }
        if (!resampler || resampler->in_rate() != static_cast<int>(info.sample_rate)) {
            resampler = std::make_unique<PcmResampler>(info.sample_rate, out_rate);
        }
        resampler->Process(mono.data(), mono.size(), pcm);
    }
    esp_mp3_dec_close(decoder);
    return !pcm.empty();
}

// The voice service speaks quietly: bring the peak up to about -2 dBFS (at most
// x6), so it is as loud as the assistant's own voice at the same volume setting.
static void Normalize(std::vector<int16_t>& pcm) {
    int peak = 0;
    for (int16_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    if (peak < 64) return;
    const float gain = std::min(6.0f, 26000.0f / peak);
    if (gain <= 1.05f) return;
    for (auto& v : pcm) {
        v = static_cast<int16_t>(std::clamp(v * gain, -32767.0f, 32767.0f));
    }
}

DeviceTts::Result DeviceTts::SpeakLine(const Line& line) {
    auto& audio = Application::GetInstance().GetAudioService();
    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    for (const auto& piece : SplitText(line.text)) {
        if (cancel_.load()) return Result::kCancelled;
        std::string mp3;
        if (!Download(TtsUrl(piece, line.lang), mp3)) return Result::kFailed;
        if (cancel_.load()) return Result::kCancelled;
        // Decoded completely first, so playback never waits for the decoder (gaps).
        std::vector<int16_t> pcm;
        pcm.reserve(static_cast<size_t>(out_rate) * 6);
        if (!DecodeMp3(mp3, out_rate, pcm)) {
            ESP_LOGW(TAG, "No audio in the voice answer (%u bytes)",
                     static_cast<unsigned>(mp3.size()));
            return Result::kFailed;
        }
        mp3 = std::string();
        Normalize(pcm);
        if (!WaitForRelease()) return Result::kCancelled;
        // Queued in half-second blocks (the playback queue holds two).
        const size_t block = static_cast<size_t>(out_rate) / 2;
        for (size_t at = 0; at < pcm.size(); at += block) {
            if (cancel_.load()) return Result::kCancelled;
            const size_t end = std::min(pcm.size(), at + block);
            std::vector<int16_t> part(pcm.begin() + at, pcm.begin() + end);
            if (!audio.PushPcmToPlaybackQueue(std::move(part), true)) return Result::kCancelled;
        }
    }
    return Result::kOk;
}
