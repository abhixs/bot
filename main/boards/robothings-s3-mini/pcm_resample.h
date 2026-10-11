// Small streaming resampler for speech (no ESP-IDF, unit-tested on a PC): a
// windowed-sinc low-pass at the input rate (so downsampling does not alias), then
// linear interpolation to the output rate. Mono 16-bit.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class PcmResampler {
public:
    PcmResampler(int in_rate, int out_rate) : in_rate_(in_rate), out_rate_(out_rate) {
        const bool down = out_rate < in_rate;
        if (down) {
            // Cut-off a little below the output Nyquist frequency.
            const double fc = 0.45 * out_rate / in_rate;  // cycles per input sample (setup only)
            const int mid = kTaps / 2;
            double sum = 0;
            for (int i = 0; i < kTaps; i++) {
                const int n = i - mid;
                const double sinc = n == 0 ? 2 * fc : std::sin(2 * M_PI * fc * n) / (M_PI * n);
                const double window = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1)) +
                                      0.08 * std::cos(4 * M_PI * i / (kTaps - 1));  // Blackman
                taps_[i] = static_cast<float>(sinc * window);
                sum += taps_[i];
            }
            for (float& t : taps_) t = static_cast<float>(t / sum);
        }
        filter_ = down;
        history_.assign(kTaps, 0.0f);
    }

    int in_rate() const { return in_rate_; }

    void Process(const int16_t* in, size_t n, std::vector<int16_t>& out) {
        const float step = static_cast<float>(in_rate_) / out_rate_;
        for (size_t k = 0; k < n; k++) {
            float x = in[k];
            if (filter_) {
                history_[pos_] = x;
                float acc = 0;
                int idx = pos_;
                for (int i = 0; i < kTaps; i++) {
                    acc += taps_[i] * history_[idx];
                    idx = idx == 0 ? kTaps - 1 : idx - 1;
                }
                pos_ = (pos_ + 1) % kTaps;
                x = acc;
            }
            // Output samples falling between the previous and this input sample.
            while (phase_ <= 1.0f) {
                const float v = prev_ + (x - prev_) * phase_;
                out.push_back(static_cast<int16_t>(std::clamp(v, -32767.0f, 32767.0f)));
                phase_ += step;
            }
            phase_ -= 1.0f;
            prev_ = x;
        }
    }

private:
    static constexpr int kTaps = 31;
    int in_rate_;
    int out_rate_;
    bool filter_ = false;
    float taps_[kTaps] = {};
    std::vector<float> history_;
    int pos_ = 0;
    float phase_ = 1.0f;  // the first output lands on the first input sample
    float prev_ = 0.0f;
};
