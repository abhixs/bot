#include "dot_clock.h"

#include <algorithm>

namespace {

constexpr int32_t kPitch = 4;  // distance between dot centres
constexpr int32_t kDot = 3;    // dot size (3x3 px, slightly rounded)
constexpr int32_t kCols = 5;
constexpr int32_t kRows = 7;
constexpr int32_t kCharGap = 5;
// Three characters ("100" | "%") use smaller dots so "100" fits inside the left eye
// (x 3..48 of 0..52); then both groups are drawn small, to match.
constexpr int32_t kSmallPitch = 3;
constexpr int32_t kSmallDot = 2;
constexpr int32_t kSmallGap = 2;

// 5x7 glyphs, one byte per row, bit 4 = leftmost column.
const uint8_t* GlyphRows(char c) {
    static const uint8_t kDigits[10][kRows] = {
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    };
    static const uint8_t kDash[kRows] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t kPercent[kRows] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    if (c == '-') return kDash;
    if (c == '%') return kPercent;
    return nullptr;  // space
}

}  // namespace

DotClock::DotClock(lv_obj_t* parent, int32_t width, int32_t height, int32_t left_cx,
                   int32_t right_cx, lv_color_t on, lv_color_t off)
    : width_(width), height_(height), left_cx_(left_cx), right_cx_(right_cx), on_(on), off_(off) {
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, width_, height_);
    lv_obj_set_style_bg_color(obj_, off_, 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj_, DrawEventCb, LV_EVENT_DRAW_MAIN, this);
}

DotClock::~DotClock() {
    if (obj_ != nullptr) lv_obj_delete(obj_);
}

void DotClock::Set(const std::string& left, const std::string& right, bool colon) {
    if (left == left_ && right == right_ && colon == colon_) return;
    left_ = left;
    right_ = right;
    colon_ = colon;
    lv_obj_invalidate(obj_);
}

void DotClock::DrawEventCb(lv_event_t* e) {
    auto* self = static_cast<DotClock*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (self != nullptr && layer != nullptr) self->Draw(layer);
}

void DotClock::Dot(lv_layer_t* layer, int32_t x, int32_t y) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = on_;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = small_ ? 0 : 1;
    dsc.border_width = 0;
    const int32_t size = small_ ? kSmallDot : kDot;
    lv_area_t area = {x, y, x + size - 1, y + size - 1};
    lv_draw_rect(layer, &dsc, &area);
}

void DotClock::DrawChar(lv_layer_t* layer, int32_t x, int32_t y, char c) {
    const uint8_t* rows = GlyphRows(c);
    if (rows == nullptr) return;
    const int32_t pitch = small_ ? kSmallPitch : kPitch;
    for (int32_t r = 0; r < kRows; r++) {
        for (int32_t col = 0; col < kCols; col++) {
            if (rows[r] & (0x10 >> col)) Dot(layer, x + col * pitch, y + r * pitch);
        }
    }
}

void DotClock::DrawGroup(lv_layer_t* layer, int32_t cx, int32_t cy, const std::string& text) {
    const int32_t n = static_cast<int32_t>(text.size() > 3 ? 3 : text.size());
    if (n == 0) return;
    const int32_t pitch = small_ ? kSmallPitch : kPitch;
    const int32_t dot = small_ ? kSmallDot : kDot;
    const int32_t char_w = (kCols - 1) * pitch + dot;  // 19 px, small 14 px
    const int32_t char_h = (kRows - 1) * pitch + dot;  // 27 px, small 20 px
    const int32_t gap = small_ ? kSmallGap : kCharGap;
    const int32_t total = n * char_w + (n - 1) * gap;  // "100" small: 46 px
    // Centred on the eye, and never off the screen.
    int32_t x = std::max(origin_x_, std::min(cx - total / 2, origin_x_ + width_ - total));
    const int32_t y = cy - char_h / 2;
    for (int32_t i = 0; i < n; i++) {
        DrawChar(layer, x, y, text[i]);
        x += char_w + gap;
    }
}

void DotClock::Draw(lv_layer_t* layer) {
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    const int32_t cy = coords.y1 + height_ / 2;
    origin_x_ = coords.x1;
    small_ = left_.size() >= 3 || right_.size() >= 3;
    DrawGroup(layer, coords.x1 + left_cx_, cy, left_);
    DrawGroup(layer, coords.x1 + right_cx_, cy, right_);
    if (colon_) {
        const int32_t x = coords.x1 + width_ / 2 - kDot / 2;
        Dot(layer, x, cy - kPitch * 2 + 1);
        Dot(layer, x, cy + kPitch * 2 - kDot);
    }
}
