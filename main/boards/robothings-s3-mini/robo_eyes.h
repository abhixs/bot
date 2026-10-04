// RoboEyes: expressive animated robot eyes for a 128x64 monochrome OLED (LVGL 9).
//
// Two rounded-rectangle eyes with eyelids are drawn into one LVGL object. Every
// value (size, position, eyelids, bounce...) eases toward a target, so mood and
// state changes morph smoothly instead of jumping. On top of the mood the eyes
// blink, look around while idle, focus while listening, bounce while speaking,
// scan while thinking, snore while sleeping and shake when an alarm rings.
#pragma once

#include <lvgl.h>

#include <cstdint>

class RoboEyes {
public:
    enum class Mood {
        Neutral,
        Happy,
        Laughing,
        Sad,
        Crying,
        Angry,
        Surprised,
        Sleepy,
        Love,
        Thinking,
        Confused,
        Winking,
        Cool,
        Alarm,
    };

    // What the assistant is doing right now (derived from the device state).
    enum class Activity {
        Idle,
        Listening,
        Thinking,  // connecting / waiting for the server
        Speaking,
        Setup,     // Wi-Fi setup, activation, upgrade
        Sleeping,  // power-save
        Alarm,     // an alarm or timer is ringing
    };

    // `on` is the color that lights an OLED pixel, `off` the background color.
    RoboEyes(lv_obj_t* parent, int32_t width, int32_t height, lv_color_t on, lv_color_t off);
    ~RoboEyes();

    // Accepts the server's emotion names (happy, sad, loving, winking, ...).
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
    struct EyeShape {
        float w = 0, h = 0, radius = 0;
        float dx = 0, dy = 0;        // per-eye offset
        float lid_outer = 0;         // tired / sad eyelid, 0..1 of eye height
        float lid_inner = 0;         // angry eyelid, 0..1
        float lid_bottom = 0;        // happy cheek, 0..1
        float open = 1;              // 1 = open, 0 = closed (wink / sleep)
    };
    struct Pose {
        EyeShape left, right;
        float gaze_x = 0, gaze_y = 0;
        float heart = 0;  // 0 = normal eyes, 1 = heart eyes
    };

    static void DrawEventCb(lv_event_t* e);
    void Draw(lv_layer_t* layer);
    void DrawEye(lv_layer_t* layer, int32_t cx, int32_t cy, const EyeShape& eye, bool is_left,
                 float heart);
    void DrawHeart(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t size);
    void DrawZ(lv_layer_t* layer, int32_t x, int32_t y, int32_t size);
    void FillRect(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                  int32_t radius, lv_color_t color);
    void FillTriangle(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int32_t x2, int32_t y2, lv_color_t color);

    void BuildTarget(Pose& target);
    void UpdateIdleBehavior(uint32_t elapsed_ms);
    uint32_t Random();
    float RandomRange(float lo, float hi);

    lv_obj_t* obj_ = nullptr;
    int32_t width_;
    int32_t height_;
    lv_color_t on_;
    lv_color_t off_;

    Mood mood_ = Mood::Neutral;
    Activity activity_ = Activity::Idle;
    Pose current_;

    uint32_t time_ms_ = 0;
    uint32_t random_state_ = 0x1234567u;

    // Blink
    uint32_t next_blink_ms_ = 2500;
    int32_t blink_elapsed_ms_ = -1;  // -1 = not blinking
    bool double_blink_ = false;

    // Idle gaze wandering
    uint32_t next_gaze_ms_ = 1500;
    float gaze_target_x_ = 0, gaze_target_y_ = 0;

    // Speaking bounce
    uint32_t next_syllable_ms_ = 0;
    float speak_target_ = 0, speak_level_ = 0;

    // Wink is a short one-shot animation, then the mood falls back to happy.
    uint32_t wink_until_ms_ = 0;
};
