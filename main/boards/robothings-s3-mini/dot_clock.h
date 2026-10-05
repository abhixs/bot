// Dot-matrix clock face for the RoboThings eyes screen.
//
// Shows two 2-character groups, one where each eye sits (e.g. "12" | "45"), in a
// classic 5x7 dot-matrix font made of small square dots, with a two-dot colon in
// the gap between the eyes. Used for the clock ("show clock") and for timer /
// Pomodoro countdowns (minutes | seconds).
#pragma once

#include <lvgl.h>

#include <cstdint>
#include <string>

class DotClock {
public:
    DotClock(lv_obj_t* parent, int32_t width, int32_t height, int32_t left_cx, int32_t right_cx,
             lv_color_t on, lv_color_t off);
    ~DotClock();

    // Each group is up to 2 characters from "0123456789- "; a single character is
    // centred. Redraws only when something changed.
    void Set(const std::string& left, const std::string& right, bool colon);

    lv_obj_t* obj() const { return obj_; }

private:
    static void DrawEventCb(lv_event_t* e);
    void Draw(lv_layer_t* layer);
    void DrawGroup(lv_layer_t* layer, int32_t cx, int32_t cy, const std::string& text);
    void DrawChar(lv_layer_t* layer, int32_t x, int32_t y, char c);
    void Dot(lv_layer_t* layer, int32_t x, int32_t y);

    lv_obj_t* obj_ = nullptr;
    int32_t width_, height_, left_cx_, right_cx_;
    lv_color_t on_, off_;
    std::string left_, right_;
    bool colon_ = false;
};
