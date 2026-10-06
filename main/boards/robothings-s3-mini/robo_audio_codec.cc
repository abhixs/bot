#include "robo_audio_codec.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#define TAG "RoboAudioCodec"

RoboAudioCodec::RoboAudioCodec(int input_sample_rate, int output_sample_rate,
                               gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                               gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
    : NoAudioCodecSimplex(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout,
                          mic_sck, mic_ws, mic_din) {
    // Microphone + playback reference, interleaved ("MR" for the AFE).
    input_reference_ = true;
    input_channels_ = 2;
    ring_.assign(kRingSize, 0);
    ESP_LOGI(TAG, "Software playback reference enabled for echo cancellation");
}

int64_t RoboAudioCodec::NowPos() { return esp_timer_get_time() * kRefRate / 1000000; }

int RoboAudioCodec::Write(const int16_t* data, int samples) {
    if (samples <= 0) {
        return 0;
    }
    const int16_t* out = data;
    if (muted_.load()) {
        silence_.assign(samples, 0);  // keep the I2S timing, play silence
        out = silence_.data();
    }
    // Recorded before the (blocking) write: the block leaves the speaker right after
    // what is already queued, or now if the output ran dry.
    PushReference(out, samples);
    return NoAudioCodecSimplex::Write(out, samples);
}

void RoboAudioCodec::PushReference(const int16_t* data, int samples) {
    // What the speaker really plays: the samples scaled by the volume curve that
    // NoAudioCodec::Write applies.
    const float volume = std::pow(output_volume_ / 100.0f, 2.0f);

    // Resample to 16 kHz (linear interpolation, phase kept across blocks).
    const double step = static_cast<double>(output_sample_rate_) / kRefRate;
    resampled_.clear();
    double pos = resample_pos_;
    while (pos < samples - 1) {
        const int i = static_cast<int>(std::floor(pos));
        const float a = i < 0 ? resample_last_ : data[i];
        const float b = data[i + 1];
        const float v = (a + (b - a) * static_cast<float>(pos - i)) * volume;
        resampled_.push_back(static_cast<int16_t>(std::clamp(v, -32767.0f, 32767.0f)));
        pos += step;
    }
    resample_pos_ = pos - samples;
    resample_last_ = data[samples - 1];

    const int n = static_cast<int>(resampled_.size());
    if (n == 0) {
        return;
    }
    const int64_t now = NowPos();
    std::lock_guard<std::mutex> lock(ring_mutex_);
    int64_t start = next_play_pos_;
    if (start < now || start > now + kOutputQueue + kRefRate / 10) {
        start = now;  // output was idle (or the estimate drifted): playback starts now
    }
    for (int i = 0; i < n; ++i) {
        ring_[static_cast<size_t>((start + i) % kRingSize)] = resampled_[i];
    }
    next_play_pos_ = start + n;
}

void RoboAudioCodec::PopReference(int16_t* dest, int samples, int stride) {
    // Capture time of this block. A read returns as soon as enough samples are in
    // the DMA buffers, so the return time jitters by up to a buffer (15 ms); the
    // sample count does not. The offset between the two is taken from the reads
    // that had to wait (the smallest offset seen), with a slow upward drift so it
    // follows the I2S clock.
    const int64_t now = NowPos();
    const int64_t end_index = capture_index_ + samples;
    const int64_t candidate = now - end_index;
    if (!capture_synced_ || now - last_read_pos_ > kRefRate / 10) {
        capture_offset_ = candidate;  // first read, or input restarted
        capture_synced_ = true;
    } else {
        capture_offset_ = std::min(candidate, capture_offset_ + 1);
    }
    last_read_pos_ = now;
    const int64_t capture_start = capture_index_ + capture_offset_;
    capture_index_ = end_index;

    // Pair each microphone sample with the playback kReferenceLead later, so the
    // reference reaches the echo canceller a little before the echo itself (checked
    // in a host simulation of the I2S queues).
    const int64_t first = capture_start + kReferenceLead;
    std::lock_guard<std::mutex> lock(ring_mutex_);
    for (int i = 0; i < samples; ++i) {
        int16_t& slot = ring_[static_cast<size_t>((first + i) % kRingSize)];
        dest[i * stride] = slot;
        slot = 0;
    }
}

int RoboAudioCodec::Read(int16_t* dest, int samples) {
    const int frames = samples / input_channels_;
    mic_.resize(frames);
    const int got = NoAudioCodec::Read(mic_.data(), frames);
    if (got <= 0) {
        return 0;
    }
    for (int i = 0; i < got; ++i) {
        dest[i * 2] = mic_[i];
    }
    PopReference(dest + 1, got, 2);
    UpdateMicStats(mic_.data(), got);
    return got * 2;
}

void RoboAudioCodec::UpdateMicStats(const int16_t* mic, int samples) {
    for (int i = 0; i < samples; ++i) {
        const int16_t v = static_cast<int16_t>(std::abs(static_cast<int>(mic[i])));
        if (v > peak_) peak_ = v;
        if (v >= INT16_MAX) ++clipped_;
    }
    const int64_t now = esp_timer_get_time();
    if (now - stats_since_us_ < 10 * 1000000LL) {
        return;
    }
    if (clipped_ > 0) {
        ESP_LOGW(TAG, "Microphone clipped %lu samples in 10 s (too loud or too close)",
                 static_cast<unsigned long>(clipped_));
    }
    ESP_LOGD(TAG, "Microphone peak %.1f dBFS",
             peak_ > 0 ? 20.0f * std::log10(peak_ / 32767.0f) : -96.0f);
    stats_since_us_ = now;
    clipped_ = 0;
    peak_ = 0;
}
