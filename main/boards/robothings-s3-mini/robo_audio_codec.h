// I2S codec for the RoboThings S3 Mini: INMP441 microphone + MAX98357A amplifier
// (NoAudioCodecSimplex), extended with
//
// - a software playback reference: the MAX98357A cannot loop its output back, so
//   the samples sent to the speaker are kept, resampled to 16 kHz and handed to the
//   AFE as the "R" channel next to the microphone ("MR"). That lets the AFE echo
//   canceller remove the device's own sound (alarm beep, voice, music) from the
//   microphone signal while the wake word is being listened for;
// - a mute switch used to silence a reply at once (e.g. after "bye") without
//   touching the saved volume;
// - a microphone clipping counter, logged so a too-loud or badly wired mic shows
//   up in the serial log.
#pragma once

#include "codecs/no_audio_codec.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class RoboAudioCodec : public NoAudioCodecSimplex {
public:
    RoboAudioCodec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk,
                   gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws,
                   gpio_num_t mic_din);

    void SetMuted(bool muted) { muted_.store(muted); }

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

private:
    // Reference ring indexed by absolute time in 16 kHz samples (1 s long). The
    // writer stores each block at the time it will leave the speaker; the reader
    // takes the samples for the time its microphone block was captured and clears
    // them, so slots that were never written read as silence.
    static constexpr int kRefRate = 16000;
    static constexpr int kRingSize = kRefRate;
    // Output DMA queue (6 x 240 frames at 24 kHz = 60 ms) in 16 kHz samples.
    static constexpr int64_t kOutputQueue = (AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM) *
                                            kRefRate / 24000;
    // Let the reference lead the echo a little: the echo canceller handles a late
    // echo (up to its filter length) but not an early one.
    static constexpr int64_t kReferenceLead = kRefRate * 15 / 1000;

    std::vector<int16_t> ring_;
    std::mutex ring_mutex_;
    int64_t next_play_pos_ = 0;  // where the next written block starts, if continuous

    // Resampler state (output rate -> 16 kHz, linear interpolation)
    double resample_pos_ = 0.0;
    int16_t resample_last_ = 0;
    std::vector<int16_t> resampled_;  // only used by the output task

    std::vector<int16_t> mic_;        // only used by the input task
    // Microphone capture clock (input task only)
    int64_t capture_index_ = 0;   // samples read so far
    int64_t capture_offset_ = 0;  // time (16 kHz samples) minus sample index
    int64_t last_read_pos_ = 0;
    bool capture_synced_ = false;
    std::vector<int16_t> silence_;    // only used by the output task
    std::atomic<bool> muted_{false};

    // Microphone level diagnostics (input task only)
    int64_t stats_since_us_ = 0;
    uint32_t clipped_ = 0;
    int16_t peak_ = 0;

    static int64_t NowPos();
    void PushReference(const int16_t* data, int samples);
    void PopReference(int16_t* dest, int samples, int stride);
    void UpdateMicStats(const int16_t* mic, int samples);
};
