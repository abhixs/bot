#include "robo_eyes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265f;

// Exponential ease toward a target; tau is the time constant in ms.
float Approach(float current, float target, uint32_t elapsed_ms, float tau_ms) {
    float k = 1.0f - std::exp(-static_cast<float>(elapsed_ms) / tau_ms);
    return current + (target - current) * k;
}

bool Equals(const char* a, const char* b) { return a != nullptr && strcmp(a, b) == 0; }

}  // namespace

RoboEyes::RoboEyes(lv_obj_t* parent, int32_t width, int32_t height, lv_color_t on,
                   lv_color_t off)
    : width_(width), height_(height), on_(on), off_(off) {
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, width_, height_);
    lv_obj_set_style_bg_color(obj_, off_, 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj_, DrawEventCb, LV_EVENT_DRAW_MAIN, this);

    // Start from the neutral pose so the first frame is already correct.
    BuildTarget(current_);
}

RoboEyes::~RoboEyes() {
    if (obj_ != nullptr) {
        lv_obj_delete(obj_);
        obj_ = nullptr;
    }
}

RoboEyes::Mood RoboEyes::MoodFromEmotion(const char* e) {
    if (e == nullptr) return Mood::Neutral;
    if (Equals(e, "happy") || Equals(e, "relaxed") || Equals(e, "confident") ||
        Equals(e, "delicious") || Equals(e, "kissy"))
        return Mood::Happy;
    if (Equals(e, "laughing") || Equals(e, "funny") || Equals(e, "silly") || Equals(e, "laugh"))
        return Mood::Laughing;
    if (Equals(e, "sad") || Equals(e, "embarrassed") || Equals(e, "cloud_off") ||
        Equals(e, "cancel"))
        return Mood::Sad;
    if (Equals(e, "crying") || Equals(e, "cry")) return Mood::Crying;
    if (Equals(e, "angry")) return Mood::Angry;
    if (Equals(e, "surprised") || Equals(e, "shocked") || Equals(e, "surprise") ||
        Equals(e, "warning"))
        return Mood::Surprised;
    if (Equals(e, "sleepy")) return Mood::Sleepy;
    if (Equals(e, "loving") || Equals(e, "love")) return Mood::Love;
    if (Equals(e, "thinking") || Equals(e, "think") || Equals(e, "download") ||
        Equals(e, "cloud_download"))
        return Mood::Thinking;
    if (Equals(e, "confused") || Equals(e, "gear") || Equals(e, "link")) return Mood::Confused;
    if (Equals(e, "winking") || Equals(e, "wink")) return Mood::Winking;
    if (Equals(e, "cool")) return Mood::Cool;
    if (Equals(e, "alarm")) return Mood::Alarm;
    return Mood::Neutral;
}

void RoboEyes::SetEmotion(const char* emotion) { SetMood(MoodFromEmotion(emotion)); }

void RoboEyes::SetMood(Mood mood) {
    if (mood == Mood::Winking && mood_ != Mood::Winking) {
        wink_until_ms_ = time_ms_ + 700;
    }
    mood_ = mood;
}

void RoboEyes::SetActivity(Activity activity) {
    if (activity == activity_) return;
    activity_ = activity;
    // A fresh blink makes every state change feel alive.
    if (activity == Activity::Listening || activity == Activity::Speaking) {
        blink_elapsed_ms_ = 0;
        gaze_target_x_ = 0;
        gaze_target_y_ = 0;
    }
}

uint32_t RoboEyes::Random() {
    random_state_ = random_state_ * 1664525u + 1013904223u;
    return random_state_ >> 8;
}

float RoboEyes::RandomRange(float lo, float hi) {
    return lo + (hi - lo) * static_cast<float>(Random() % 10000) / 10000.0f;
}

void RoboEyes::UpdateIdleBehavior(uint32_t elapsed_ms) {
    // Blink scheduling (not while asleep).
    if (blink_elapsed_ms_ >= 0) {
        blink_elapsed_ms_ += static_cast<int32_t>(elapsed_ms);
        if (blink_elapsed_ms_ > 160) {
            blink_elapsed_ms_ = double_blink_ ? 0 : -1;
            double_blink_ = false;
        }
    } else if (time_ms_ >= next_blink_ms_) {
        blink_elapsed_ms_ = 0;
        double_blink_ = (Random() % 5) == 0;
        uint32_t base = activity_ == Activity::Listening ? 3500 : 2200;
        next_blink_ms_ = time_ms_ + base + Random() % 3800;
    }

    // Gaze: wander while idle, sweep while thinking, look at the user otherwise.
    if (activity_ == Activity::Idle) {
        if (time_ms_ >= next_gaze_ms_) {
            if (Random() % 3 == 0) {
                gaze_target_x_ = 0;
                gaze_target_y_ = 0;
            } else {
                gaze_target_x_ = RandomRange(-12.0f, 12.0f);
                gaze_target_y_ = RandomRange(-5.0f, 5.0f);
            }
            next_gaze_ms_ = time_ms_ + 1200 + Random() % 3000;
        }
    } else if (activity_ == Activity::Thinking || activity_ == Activity::Setup) {
        gaze_target_x_ = std::sin(time_ms_ / 650.0f) * 12.0f;
        gaze_target_y_ = -3.0f + std::sin(time_ms_ / 1300.0f) * 2.0f;
    } else {
        gaze_target_x_ = 0;
        gaze_target_y_ = 0;
    }

    // Speaking: random "syllables" make the eyes bob with the voice.
    if (activity_ == Activity::Speaking) {
        if (time_ms_ >= next_syllable_ms_) {
            static const float kLevels[] = {0.0f, 0.35f, 0.6f, 1.0f};
            speak_target_ = kLevels[Random() % 4];
            next_syllable_ms_ = time_ms_ + 90 + Random() % 90;
        }
    } else {
        speak_target_ = 0;
    }
    speak_level_ = Approach(speak_level_, speak_target_, elapsed_ms, 45.0f);
}

void RoboEyes::BuildTarget(Pose& t) {
    const float base_h = height_ * 0.72f;  // 34 px on a 48 px tall area
    const float base_w = base_h;
    const float base_r = base_h * 0.27f;

    EyeShape eye;
    eye.w = base_w;
    eye.h = base_h;
    eye.radius = base_r;
    t.left = eye;
    t.right = eye;
    t.gaze_x = gaze_target_x_;
    t.gaze_y = gaze_target_y_;
    t.heart = 0;

    Mood mood = mood_;
    if (activity_ == Activity::Alarm) mood = Mood::Alarm;
    if (activity_ == Activity::Sleeping) mood = Mood::Sleepy;
    if (mood == Mood::Winking && time_ms_ > wink_until_ms_) mood = Mood::Happy;

    switch (mood) {
        case Mood::Neutral:
            break;
        case Mood::Happy:
            t.left.lid_bottom = t.right.lid_bottom = 0.55f;
            t.gaze_y -= 2;
            break;
        case Mood::Laughing:
            t.left.lid_bottom = t.right.lid_bottom = 0.6f;
            t.left.w = t.right.w = base_w + 2;
            t.gaze_y += std::sin(time_ms_ / 55.0f) * 2.5f - 2;
            break;
        case Mood::Sad:
            t.left.lid_outer = t.right.lid_outer = 0.5f;
            t.left.h = t.right.h = base_h - 4;
            t.gaze_y += 4;
            t.gaze_x *= 0.3f;
            break;
        case Mood::Crying:
            // Eyes sit higher and smaller to leave room for the falling tears.
            t.left.lid_outer = t.right.lid_outer = 0.5f;
            t.left.h = t.right.h = base_h - 10;
            t.gaze_y = -6;
            t.gaze_x = 0;
            break;
        case Mood::Angry:
            t.left.lid_inner = t.right.lid_inner = 0.55f;
            t.left.w = t.right.w = base_w + 3;
            t.left.h = t.right.h = base_h - 4;
            break;
        case Mood::Surprised:
            t.left.w = t.right.w = base_w - 4;
            t.left.h = t.right.h = std::min<float>(height_ - 4, base_h + 9);
            t.left.radius = t.right.radius = base_r * 1.8f;
            t.gaze_x *= 0.3f;
            break;
        case Mood::Sleepy:
            t.left.open = t.right.open = 0.06f;
            t.left.lid_outer = t.right.lid_outer = 0.3f;
            t.gaze_x = 0;
            t.gaze_y = 5 + std::sin(time_ms_ / 900.0f) * 1.5f;  // slow breathing
            break;
        case Mood::Love: {
            t.heart = 1;
            float pulse = 1.0f + 0.12f * std::max(0.0f, std::sin(time_ms_ / 160.0f));
            t.left.w = t.right.w = base_w * pulse;
            t.left.h = t.right.h = base_h * pulse;
            t.gaze_x *= 0.4f;
            break;
        }
        case Mood::Thinking:
            t.left.lid_outer = 0.25f;
            t.right.h = base_h - 6;
            t.gaze_x = 9;
            t.gaze_y = -6;
            break;
        case Mood::Confused:
            t.right.h = base_h - 12;
            t.right.lid_inner = 0.3f;
            t.gaze_x = std::sin(time_ms_ / 400.0f) * 6.0f;
            break;
        case Mood::Winking:
            t.left.lid_bottom = 0.5f;
            t.right.open = 0.05f;
            break;
        case Mood::Cool:
            // Wide, flat "visor" eyes.
            t.left.w = t.right.w = base_w + 8;
            t.left.h = t.right.h = base_h * 0.45f;
            t.left.radius = t.right.radius = 3;
            t.gaze_x *= 0.5f;
            break;
        case Mood::Alarm:
            t.left.w = t.right.w = base_w - 2;
            t.left.h = t.right.h = std::min<float>(height_ - 4, base_h + 9);
            t.left.radius = t.right.radius = base_r * 1.6f;
            t.gaze_x = std::sin(time_ms_ / 22.0f) * 3.0f;  // shake
            t.gaze_y = 0;
            break;
    }

    // Activity overlays.
    if (activity_ == Activity::Music && mood != Mood::Alarm) {
        // Bob on a ~110 BPM beat and sway gently side to side.
        float beat = std::fabs(std::sin(time_ms_ * kPi / 545.0f));
        t.gaze_y += 1.0f - beat * 3.0f;
        t.gaze_x = std::sin(time_ms_ * kPi / 1090.0f) * 5.0f;
        if (mood == Mood::Neutral) {
            t.left.lid_bottom = t.right.lid_bottom = 0.45f;
        }
    }
    if (activity_ == Activity::Listening && mood != Mood::Sleepy) {
        float pulse = std::sin(time_ms_ / 330.0f) * 1.5f;
        t.left.h += 3 + pulse;
        t.right.h += 3 + pulse;
        t.left.w += 2;
        t.right.w += 2;
    }
    if (activity_ == Activity::Speaking) {
        float bounce = speak_level_ * 3.0f;
        t.gaze_y -= bounce;
        t.left.h -= bounce * 0.8f;
        t.right.h -= bounce * 0.8f;
    }

    // Blink overrides openness for a moment.
    if (blink_elapsed_ms_ >= 0 && mood != Mood::Sleepy) {
        float phase = std::min(1.0f, blink_elapsed_ms_ / 160.0f);
        float closed = std::sin(phase * kPi);  // 0 -> 1 -> 0
        t.left.open = std::min(t.left.open, 1.0f - closed * 0.95f);
        t.right.open = std::min(t.right.open, 1.0f - closed * 0.95f);
    }
}

void RoboEyes::Tick(uint32_t elapsed_ms) {
    if (obj_ == nullptr) return;
    time_ms_ += elapsed_ms;
    UpdateIdleBehavior(elapsed_ms);

    Pose target;
    BuildTarget(target);

    const float tau = 70.0f;
    auto ease = [elapsed_ms, tau](EyeShape& c, const EyeShape& t) {
        c.w = Approach(c.w, t.w, elapsed_ms, tau);
        c.h = Approach(c.h, t.h, elapsed_ms, tau);
        c.radius = Approach(c.radius, t.radius, elapsed_ms, tau);
        c.dx = Approach(c.dx, t.dx, elapsed_ms, tau);
        c.dy = Approach(c.dy, t.dy, elapsed_ms, tau);
        c.lid_outer = Approach(c.lid_outer, t.lid_outer, elapsed_ms, tau);
        c.lid_inner = Approach(c.lid_inner, t.lid_inner, elapsed_ms, tau);
        c.lid_bottom = Approach(c.lid_bottom, t.lid_bottom, elapsed_ms, tau);
        // Blinks need to be snappy, so openness follows almost directly.
        c.open = Approach(c.open, t.open, elapsed_ms, 18.0f);
    };
    ease(current_.left, target.left);
    ease(current_.right, target.right);
    current_.gaze_x = Approach(current_.gaze_x, target.gaze_x, elapsed_ms, 110.0f);
    current_.gaze_y = Approach(current_.gaze_y, target.gaze_y, elapsed_ms, 110.0f);
    current_.heart = target.heart;  // shape switch is instant, size eases

    lv_obj_invalidate(obj_);
}

void RoboEyes::DrawEventCb(lv_event_t* e) {
    auto* self = static_cast<RoboEyes*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (self != nullptr && layer != nullptr) {
        self->Draw(layer);
    }
}

void RoboEyes::FillRect(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        int32_t radius, lv_color_t color) {
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    dsc.border_width = 0;
    dsc.outline_width = 0;
    dsc.shadow_width = 0;
    lv_area_t area = {x1, y1, x2, y2};
    lv_draw_rect(layer, &dsc, &area);
}

void RoboEyes::FillTriangle(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2, lv_color_t color) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

void RoboEyes::DrawHeart(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t size) {
    int32_t r = size / 4 + 1;
    int32_t top = cy - size / 4;
    // Two round lobes and a point at the bottom.
    FillRect(layer, cx - 2 * r + 1, top - r, cx + 1, top + r, LV_RADIUS_CIRCLE, on_);
    FillRect(layer, cx - 1, top - r, cx + 2 * r - 1, top + r, LV_RADIUS_CIRCLE, on_);
    FillTriangle(layer, cx - 2 * r + 1, top + r / 3, cx + 2 * r - 1, top + r / 3, cx,
                 cy + size / 2, on_);
}

void RoboEyes::DrawZ(lv_layer_t* layer, int32_t x, int32_t y, int32_t size) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = on_;
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;
    auto line = [&](int32_t ax, int32_t ay, int32_t bx, int32_t by) {
        dsc.p1.x = ax;
        dsc.p1.y = ay;
        dsc.p2.x = bx;
        dsc.p2.y = by;
        lv_draw_line(layer, &dsc);
    };
    line(x, y, x + size, y);
    line(x + size, y, x, y + size);
    line(x, y + size, x + size, y + size);
}

void RoboEyes::DrawEye(lv_layer_t* layer, int32_t cx, int32_t cy, const EyeShape& eye,
                       bool is_left, float heart) {
    int32_t w = std::max<int32_t>(4, static_cast<int32_t>(eye.w + 0.5f));
    int32_t full_h = std::max<int32_t>(2, static_cast<int32_t>(eye.h + 0.5f));
    int32_t h = std::max<int32_t>(2, static_cast<int32_t>(eye.h * eye.open + 0.5f));
    cx += static_cast<int32_t>(eye.dx);
    cy += static_cast<int32_t>(eye.dy);

    if (heart > 0.5f && eye.open > 0.5f) {
        DrawHeart(layer, cx, cy, std::min(w, full_h));
        return;
    }

    int32_t x1 = cx - w / 2;
    int32_t x2 = x1 + w - 1;
    int32_t y1 = cy - h / 2;
    int32_t y2 = y1 + h - 1;
    int32_t radius = std::min<int32_t>(static_cast<int32_t>(eye.radius), std::min(w, h) / 2);
    if (h <= 4) radius = 1;
    FillRect(layer, x1, y1, x2, y2, radius, on_);

    if (h <= 4) return;  // closed: just a line, no eyelids needed

    // The "outer" side is away from the nose: left side of the left eye.
    int32_t outer_x = is_left ? x1 - 1 : x2 + 1;
    int32_t inner_x = is_left ? x2 + 1 : x1 - 1;

    if (eye.lid_outer > 0.02f) {
        int32_t drop = static_cast<int32_t>(eye.lid_outer * h);
        FillTriangle(layer, x1 - 1, y1 - 1, x2 + 1, y1 - 1, outer_x, y1 + drop, off_);
    }
    if (eye.lid_inner > 0.02f) {
        int32_t drop = static_cast<int32_t>(eye.lid_inner * h);
        FillTriangle(layer, x1 - 1, y1 - 1, x2 + 1, y1 - 1, inner_x, y1 + drop, off_);
    }
    if (eye.lid_bottom > 0.02f) {
        // A big rounded "cheek" pushes up from below and leaves an arched ^ shape.
        int32_t cut = static_cast<int32_t>(eye.lid_bottom * h);
        FillRect(layer, x1 - 3, y2 - cut, x2 + 3, y2 + h, w / 2 + 2, off_);
    }
}

void RoboEyes::Draw(lv_layer_t* layer) {
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    const int32_t ox = coords.x1;
    const int32_t oy = coords.y1;
    const int32_t cx = ox + width_ / 2 + static_cast<int32_t>(current_.gaze_x);
    const int32_t cy = oy + height_ / 2 + static_cast<int32_t>(current_.gaze_y);

    // Eye spacing follows eye width so bigger eyes do not overlap.
    const float spacing = std::max(current_.left.w, current_.right.w) / 2.0f + height_ * 0.3f;
    const int32_t left_cx = cx - static_cast<int32_t>(spacing);
    const int32_t right_cx = cx + static_cast<int32_t>(spacing);

    DrawEye(layer, left_cx, cy, current_.left, true, current_.heart);
    DrawEye(layer, right_cx, cy, current_.right, false, current_.heart);

    Mood mood = activity_ == Activity::Sleeping ? Mood::Sleepy : mood_;
    if (activity_ == Activity::Alarm) mood = Mood::Alarm;

    if (mood == Mood::Sleepy) {
        // Three Zs float up from the right eye and fade out at the top.
        for (int i = 0; i < 3; i++) {
            uint32_t phase = (time_ms_ + i * 700) % 2100;
            int32_t rise = static_cast<int32_t>(phase * (height_ - 8) / 2100);
            int32_t size = 3 + i;
            int32_t zx = right_cx + static_cast<int32_t>(current_.right.w / 2) + 2 + i * 4;
            int32_t zy = oy + height_ - 10 - rise;
            if (zy > oy) DrawZ(layer, zx, zy, size);
        }
    }

    if (mood == Mood::Crying) {
        // Tears drip from the outer corners.
        for (int side = 0; side < 2; side++) {
            const EyeShape& eye = side == 0 ? current_.left : current_.right;
            int32_t ex = side == 0 ? left_cx - static_cast<int32_t>(eye.w / 2) + 2
                                   : right_cx + static_cast<int32_t>(eye.w / 2) - 6;
            int32_t start = cy + static_cast<int32_t>(eye.h / 2) + 1;
            int32_t travel = std::max<int32_t>(4, oy + height_ - 7 - start);
            uint32_t phase = (time_ms_ + side * 500) % 1000;
            int32_t ty = start + static_cast<int32_t>(phase * travel / 1000);
            FillRect(layer, ex, ty, ex + 3, ty + 5, 2, on_);
        }
    }

    if (mood == Mood::Alarm && (time_ms_ / 250) % 2 == 0) {
        // Flashing "!" marks next to the eyes.
        int32_t lx = ox + 3;
        int32_t rx = ox + width_ - 6;
        for (int32_t x : {lx, rx}) {
            FillRect(layer, x, cy - 10, x + 2, cy + 3, 1, on_);
            FillRect(layer, x, cy + 6, x + 2, cy + 8, 1, on_);
        }
    }
}
