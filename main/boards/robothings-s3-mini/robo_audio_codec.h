// I2S codec for the RoboThings S3 Mini: INMP441 microphone + MAX98357A amplifier
// on ONE I2S port (NoAudioCodecDuplex, 16 kHz both ways), extended with
//
// - one shared clock: the speaker's BCLK / LRCK are also sent out on the old
//   microphone SCK / WS pins, so the microphone runs on exactly the speaker's
//   clock whether it is wired to those pins or to the speaker's (or both, joined).
//   Two separate I2S clocks fought each other once the pins were joined (heavy
//   crackle), and drifted apart otherwise (poor echo cancellation);
// - a software playback reference: the MAX98357A cannot loop its output back, so
//   the samples sent to the speaker are kept, resampled to 16 kHz and handed to the
//   AFE as the "R" channel next to the microphone ("MR"). That lets the AFE echo
//   canceller remove the device's own sound (alarm beep, voice, music) from the
//   microphone signal while the wake word is being listened for;
// - a mute switch used to silence a reply at once (e.g. after "bye") without
//   touching the saved volume, ramped over 5 ms so muting never clicks;
// - microphone headroom: the INMP441's 24-bit samples are scaled to 16 bits with
//   6 dB more headroom than the generic driver (>> 13 instead of >> 12), so the
//   loud echo of the speaker next to it does not clip - a clipped echo is
//   non-linear and the echo canceller cannot remove it. The AFE output gain
//   (CONFIG_AFE_OUTPUT_GAIN_PERCENT) gives the speech sent to the server its usual
//   level back, after the echo is removed;
// - a gentle 2nd-order high-pass at 100 Hz on the microphone: air flow, fan rumble
//   and handling noise sit mostly below speech, so they are cut before the AFE;
// - a microphone clipping counter, logged so a too-loud or badly wired mic shows
//   up in the serial log.
#pragma once

#include "codecs/no_audio_codec.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class RoboAudioCodec : public NoAudioCodecDuplex {
public:
    RoboAudioCodec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk,
                   gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws,
                   gpio_num_t mic_din);

    void SetMuted(bool muted) { muted_.store(muted); }

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;
    // The microphone needs the speaker's clock: output stays on while input is on.
    void EnableInput(bool enable) override;

private:
    // Reference ring indexed by absolute time in 16 kHz samples (1 s long). The
    // writer stores each block at the time it will leave the speaker; the reader
    // takes the samples for the time its microphone block was captured and clears
    // them, so slots that were never written read as silence.
    static constexpr int kRefRate = 16000;
    static constexpr int kRingSize = kRefRate;
    // Output DMA queue (6 x 240 frames = 90 ms at 16 kHz) in 16 kHz samples.
    int64_t output_queue_ = 0;
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
    std::vector<int32_t> raw_;        // only used by the input task
    // High-pass biquad state and coefficients (input task only)
    float hp_b0_ = 1, hp_b1_ = 0, hp_b2_ = 0, hp_a1_ = 0, hp_a2_ = 0;
    float hp_x1_ = 0, hp_x2_ = 0, hp_y1_ = 0, hp_y2_ = 0;
    float mute_gain_ = 1.0f;          // output task only: ramps to 0 / 1
    std::vector<int16_t> ramped_;     // only used by the output task
    // Microphone capture clock (input task only)
    int64_t capture_index_ = 0;   // samples read so far
    int64_t capture_offset_ = 0;  // time (16 kHz samples) minus sample index
    int64_t last_read_pos_ = 0;
    bool capture_synced_ = false;
    std::vector<int16_t> silence_;    // only used by the output task
    std::atomic<bool> muted_{false};
    static constexpr int kMicShift = 13;

    // Microphone level diagnostics (input task only)
    int64_t stats_since_us_ = 0;
    uint32_t clipped_ = 0;
    int16_t peak_ = 0;

    static int64_t NowPos();
    static void MirrorClock(gpio_num_t pin, int signal);
    void PushReference(const int16_t* data, int samples);
    void PopReference(int16_t* dest, int samples, int stride);
    void UpdateMicStats(const int16_t* mic, int samples);
};
