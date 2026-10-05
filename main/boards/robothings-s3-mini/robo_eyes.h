// RoboEyes: two eyes on a 128x64 monochrome OLED, each shown as a small
// symbol (bar, square, arc, cross, heart, dots...), following the
// RoboThings expression sheet: happy, sad, excited, thinking, speaking, sleeping,
// angry, surprised, wink, blush, loading, curious, tease, confused, in love,
// shocked, annoyed, focused, happy-closed and sweet.
//
// Layout: both sockets touch the left and right screen edges with a 5 mm gap
// between them (22 px on a 1.3" 128x64 panel, 0.23 mm per pixel), which makes
// each socket 53 px (about 12.2 mm) wide.
//
// The symbols move inside the sockets (looking around), blink, squash into each
// other when the expression changes, pulse with speech and dance with music.
#pragma once

#include <lvgl.h>

#include <cstdint>

class RoboEyes {
public:
    enum class Mood {
        Focused,  // neutral: small square pupils that look around
        Happy,
        Sad,
        Excited,
        Thinking,
        Speaking,
        Sleeping,
        Angry,
        Surprised,
        Wink,
        Blush,
        Loading,
        Curious,
        Tease,
        Confused,
        InLove,
        Shocked,
        Annoyed,
        HappyClosed,
        Sweet,
    };

    // What the assistant is doing right now (derived from the device state).
    enum class Activity {
        Idle,
        Listening,
        Thinking,  // connecting / waiting for the server
        Speaking,
        Setup,     // Wi-Fi setup, activation, upgrade
        Sleeping,  // idle for a while / power-save
        Alarm,     // an alarm or timer is ringing
        Music,     // a song is playing
    };

    // `on` is the color that lights an OLED pixel, `off` the background color.
    RoboEyes(lv_obj_t* parent, int32_t width, int32_t height, lv_color_t on, lv_color_t off);
    ~RoboEyes();

    // Accepts the server's emotion names (happy, sad, loving, winking, ...) and the
    // names on the expression sheet (excited, blush, tease, sweet, ...).
    void SetEmotion(const char* emotion);
    void SetMood(Mood mood);
    void SetActivity(Activity activity);
    Mood mood() const { return mood_; }
    Activity activity() const { return activity_; }

    // Advances the animation; call regularly (e.g. every 40 ms) with the LVGL lock held.
    void Tick(uint32_t elapsed_ms);

    lv_obj_t* obj() const { return obj_; }

    static Mood MoodFromEmotion(const char* emotion);

private:
    enum class Glyph { Bar, Square, Arc, Cross, HalfDisc, Dots4, Spinner, Heart, Equals };

    // What one eye shows: a symbol, its size and where it sits inside the socket.
    struct EyeGlyph {
        Glyph glyph = Glyph::Square;
        float w = 10, h = 10;   // size in px (Arc: w = diameter, h = stroke)
        float radius = 2;       // corner radius for rectangles
        float dx = 0, dy = 0;   // offset from the socket centre
    };

    static void DrawEventCb(lv_event_t* e);
    void Draw(lv_layer_t* layer);
    void DrawGlyph(lv_layer_t* layer, int32_t cx, int32_t cy, const EyeGlyph& g, float scale,
                   float scale_y);
    void FillRect(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                  int32_t radius, lv_color_t color);
    void FillCircle(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t r, lv_color_t color);
    void FillTriangle(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int32_t x2, int32_t y2);
    void DrawLine(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t width);

    Mood EffectiveMood() const;
    void GlyphsFor(Mood mood, EyeGlyph& left, EyeGlyph& right) const;
    void UpdateBehavior(uint32_t elapsed_ms);
    uint32_t Random();
    float RandomRange(float lo, float hi);

    lv_obj_t* obj_ = nullptr;
    int32_t width_;
    int32_t height_;
    lv_color_t on_;
    lv_color_t off_;

    // Socket geometry
    int32_t eye_diameter_ = 53;
    int32_t left_cx_ = 26, right_cx_ = 101, eye_cy_ = 32;
    int32_t ring_width_ = 2;

    Mood mood_ = Mood::Focused;
    Activity activity_ = Activity::Idle;

    // Expression change: squash the old symbol flat, swap, open the new one.
    Mood shown_mood_ = Mood::Focused;
    int32_t swap_elapsed_ms_ = -1;  // -1 = no swap running
    static constexpr int32_t kSwapHalfMs = 90;

    uint32_t time_ms_ = 0;
    uint32_t random_state_ = 0x1234567u;

    // Blink
    uint32_t next_blink_ms_ = 2500;
    int32_t blink_elapsed_ms_ = -1;
    bool double_blink_ = false;

    // Gaze (symbols move inside the sockets)
    uint32_t next_gaze_ms_ = 1500;
    float gaze_target_x_ = 0, gaze_target_y_ = 0;
    float gaze_x_ = 0, gaze_y_ = 0;

    // Speech pulse
    uint32_t next_syllable_ms_ = 0;
    float speak_target_ = 0, speak_level_ = 0;

    // Wink is a short one-shot, then the eyes go back to happy.
    uint32_t wink_until_ms_ = 0;
};
