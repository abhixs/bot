// Minimal MPEG audio Layer III frame finder (no ESP-IDF dependencies).
// Used to cut an HTTP MP3 stream into whole frames for esp_mp3_dec.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mp3 {

struct FrameInfo {
    int sample_rate = 0;
    int samples = 0;      // PCM samples per channel in this frame
    size_t length = 0;    // bytes, header included
};

// Parses a 4-byte frame header. Returns false if it is not a valid Layer III header.
inline bool ParseHeader(const uint8_t* h, FrameInfo& info) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    int version = (h[1] >> 3) & 3;  // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5, 1 = reserved
    int layer = (h[1] >> 1) & 3;    // 1 = Layer III
    int bitrate_index = (h[2] >> 4) & 0xF;
    int rate_index = (h[2] >> 2) & 3;
    int padding = (h[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
        return false;
    }
    static const int kBitrateV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBitrateV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kRates[3][3] = {{11025, 12000, 8000}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    const bool mpeg1 = version == 3;
    const int bitrate = (mpeg1 ? kBitrateV1 : kBitrateV2)[bitrate_index] * 1000;
    const int rate = kRates[version == 3 ? 2 : (version == 2 ? 1 : 0)][rate_index];
    info.sample_rate = rate;
    info.samples = mpeg1 ? 1152 : 576;
    info.length = static_cast<size_t>((mpeg1 ? 144 : 72) * bitrate / rate + padding);
    return info.length > 4;
}

// Size of an ID3v2 tag starting at `data`, or 0 if there is none (needs 10 bytes).
inline size_t Id3v2Size(const uint8_t* data, size_t size) {
    if (size < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') return 0;
    size_t tag = (static_cast<size_t>(data[6] & 0x7F) << 21) | (static_cast<size_t>(data[7] & 0x7F) << 14) |
                 (static_cast<size_t>(data[8] & 0x7F) << 7) | static_cast<size_t>(data[9] & 0x7F);
    size_t footer = (data[5] & 0x10) ? 10 : 0;
    return 10 + tag + footer;
}

enum class FindResult { kFrame, kNeedMoreData, kSkip };

// Looks at data[0..size). kFrame: a frame of info.length bytes starts at 0.
// kSkip: drop `skip` bytes (garbage / tags) and call again. kNeedMoreData: read more.
// A frame is only accepted when the following header also looks valid (or the
// buffer ends exactly there), which avoids false syncs inside audio data.
inline FindResult FindFrame(const uint8_t* data, size_t size, FrameInfo& info, size_t& skip) {
    skip = 0;
    if (size < 10) return FindResult::kNeedMoreData;
    size_t id3 = Id3v2Size(data, size);
    if (id3 > 0) {
        skip = id3;
        return FindResult::kSkip;
    }
    if (!ParseHeader(data, info)) {
        // Jump to the next possible sync byte.
        size_t i = 1;
        while (i < size && data[i] != 0xFF) i++;
        skip = i;
        return FindResult::kSkip;
    }
    if (size < info.length + 4) return FindResult::kNeedMoreData;
    FrameInfo next;
    if (!ParseHeader(data + info.length, next) && !(data[info.length] == 'T' && data[info.length + 1] == 'A' &&
                                                    data[info.length + 2] == 'G')) {
        skip = 1;
        return FindResult::kSkip;
    }
    return FindResult::kFrame;
}

}  // namespace mp3
