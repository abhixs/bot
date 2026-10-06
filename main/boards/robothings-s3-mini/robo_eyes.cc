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

int32_t Round(float v) { return static_cast<int32_t>(std::lround(v)); }

}  // namespace

RoboEyes::RoboEyes(lv_obj_t* parent, int32_t width, int32_t height, lv_color_t on,
                   lv_color_t off)
    : width_(width), height_(height), on_(on), off_(off) {
    // 5 mm gap on a 128 px wide, 29.4 mm panel = 22 px; sockets touch the edges.
    const int32_t gap = (width_ * 22 + 64) / 128;
    eye_diameter_ = std::min<int32_t>((width_ - gap) / 2, height_ - 2);
    left_cx_ = eye_diameter_ / 2;
    right_cx_ = width_ - 1 - eye_diameter_ / 2;
    eye_cy_ = height_ / 2;

    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, width_, height_);
    lv_obj_set_style_bg_color(obj_, off_, 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj_, DrawEventCb, LV_EVENT_DRAW_MAIN, this);
}

RoboEyes::~RoboEyes() {
    if (obj_ != nullptr) {
        lv_obj_delete(obj_);
        obj_ = nullptr;
    }
}

RoboEyes::Mood RoboEyes::MoodFromEmotion(const char* e) {
    if (e == nullptr) return Mood::Focused;
    // Names from the expression sheet
    if (Equals(e, "excited") || Equals(e, "laughing") || Equals(e, "laugh")) return Mood::Excited;
    if (Equals(e, "happy")) return Mood::Happy;
    if (Equals(e, "crying") || Equals(e, "cry")) return Mood::Crying;
    if (Equals(e, "sad") || Equals(e, "cancel") || Equals(e, "cloud_off")) return Mood::Sad;
    if (Equals(e, "thinking") || Equals(e, "think")) return Mood::Thinking;
    if (Equals(e, "speaking")) return Mood::Speaking;
    if (Equals(e, "sleeping") || Equals(e, "sleepy")) return Mood::Sleeping;
    if (Equals(e, "angry")) return Mood::Angry;
    if (Equals(e, "surprised") || Equals(e, "surprise")) return Mood::Surprised;
    if (Equals(e, "wink") || Equals(e, "winking")) return Mood::Wink;
    if (Equals(e, "blush") || Equals(e, "embarrassed")) return Mood::Blush;
    if (Equals(e, "loading") || Equals(e, "download") || Equals(e, "cloud_download") ||
        Equals(e, "gear") || Equals(e, "link"))
        return Mood::Loading;
    if (Equals(e, "curious")) return Mood::Curious;
    if (Equals(e, "tease") || Equals(e, "funny") || Equals(e, "silly") || Equals(e, "delicious"))
        return Mood::Tease;
    if (Equals(e, "confused")) return Mood::Confused;
    if (Equals(e, "in_love") || Equals(e, "loving") || Equals(e, "love")) return Mood::InLove;
    if (Equals(e, "shocked") || Equals(e, "warning") || Equals(e, "alarm")) return Mood::Shocked;
    if (Equals(e, "annoyed") || Equals(e, "cool")) return Mood::Annoyed;
    if (Equals(e, "happy_closed") || Equals(e, "relaxed")) return Mood::HappyClosed;
    if (Equals(e, "sweet") || Equals(e, "kissy")) return Mood::Sweet;
    // neutral, focused, confident and anything unknown
    return Mood::Focused;
}

void RoboEyes::SetEmotion(const char* emotion) { SetMood(MoodFromEmotion(emotion)); }

void RoboEyes::SetMood(Mood mood) {
    if (mood == Mood::Wink && mood_ != Mood::Wink) {
        wink_until_ms_ = time_ms_ + 900;
    }
    mood_ = mood;
}

void RoboEyes::SetActivity(Activity activity) {
    if (activity == activity_) return;
    activity_ = activity;
    if (activity == Activity::Listening || activity == Activity::Speaking) {
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

RoboEyes::Mood RoboEyes::EffectiveMood() const {
    switch (activity_) {
        case Activity::Alarm:
            return Mood::Shocked;
        case Activity::Sleeping:
            // Standby: big square "surprised" eyes that drift around and blink slowly.
            return Mood::Surprised;
        case Activity::Setup:
            return Mood::Loading;
        case Activity::Music:
            return Mood::HappyClosed;
        // Voice states: always the same face, so it is clear what the device is doing.
        case Activity::WakeHeard:
            return Mood::Sweet;
        case Activity::Listening:
            return Mood::Blush;
        case Activity::Speaking:
            return Mood::Excited;
        default:
            break;
    }
    if (mood_ == Mood::Wink && time_ms_ > wink_until_ms_) return Mood::Happy;
    return mood_;
}

// Symbol sizes are designed for a 53 px socket and scaled for other sizes.
void RoboEyes::GlyphsFor(Mood mood, EyeGlyph& l, EyeGlyph& r) const {
    auto make = [](Glyph glyph, float w, float h, float radius = 0, float dx = 0, float dy = 0) {
        EyeGlyph g;
        g.glyph = glyph;
        g.w = w;
        g.h = h;
        g.radius = radius;
        g.dx = dx;
        g.dy = dy;
        return g;
    };
    switch (mood) {
        case Mood::Focused:
            l = r = make(Glyph::Square, 11, 11, 2);
            break;
        case Mood::Happy:
            l = r = make(Glyph::Bar, 24, 6, 3);
            break;
        case Mood::Sad:
            l = make(Glyph::Square, 11, 11, 2, -4, 6);
            r = make(Glyph::Square, 11, 11, 2, 4, 6);
            break;
        case Mood::Crying:
            l = make(Glyph::Square, 11, 11, 2, -3, -4);
            r = make(Glyph::Square, 11, 11, 2, 3, -4);
            break;
        case Mood::Excited:
            l = r = make(Glyph::Arc, 22, 5, 0, 0, 4);
            break;
        case Mood::Thinking:
            l = make(Glyph::Square, 11, 11, 2, -7, -4);
            r = make(Glyph::Square, 11, 11, 2, 6, -4);
            break;
        case Mood::Speaking:
        case Mood::Confused:
            l = r = make(Glyph::Cross, 15, 4);
            break;
        case Mood::Sleeping:
            l = r = make(Glyph::Cross, 14, 4);
            break;
        case Mood::Angry:
            l = r = make(Glyph::HalfDisc, 24, 9, 0, 0, 3);
            break;
        case Mood::Surprised:
            l = r = make(Glyph::Square, 26, 26, 6);
            break;
        case Mood::Wink:
            l = make(Glyph::Bar, 22, 6, 3);
            r = make(Glyph::Square, 22, 22, 4);
            break;
        case Mood::Blush:
            l = r = make(Glyph::Dots4, 10, 3.5f);
            break;
        case Mood::Loading:
            l = r = make(Glyph::Spinner, 20, 2);
            break;
        case Mood::Curious:
            l = r = make(Glyph::Square, 11, 11, 2, -6, 2);
            break;
        case Mood::Tease:
            l = r = make(Glyph::HalfDisc, 26, 12, 0, 0, 2);
            break;
        case Mood::InLove:
            l = r = make(Glyph::Heart, 22, 22);
            break;
        case Mood::Shocked:
            l = r = make(Glyph::Square, 28, 28, 4);
            break;
        case Mood::Annoyed:
            l = r = make(Glyph::Bar, 26, 6, 3, 0, -1);
            break;
        case Mood::HappyClosed:
            l = r = make(Glyph::Arc, 24, 4, 0, 0, 4);
            break;
        case Mood::Sweet:
            l = r = make(Glyph::Equals, 22, 4, 2);
            break;
    }
    const float k = eye_diameter_ / 53.0f;
    for (EyeGlyph* g : {&l, &r}) {
        g->w *= k;
        g->h *= k;
        g->radius *= k;
        g->dx *= k;
        g->dy *= k;
    }
}

void RoboEyes::UpdateBehavior(uint32_t elapsed_ms) {
    const Mood mood = EffectiveMood();

    // Blink (not while asleep or loading).
    const bool standby = activity_ == Activity::Sleeping;
    if (blink_elapsed_ms_ >= 0) {
        blink_elapsed_ms_ += static_cast<int32_t>(elapsed_ms);
        if (blink_elapsed_ms_ > blink_duration_ms_) {
            blink_elapsed_ms_ = double_blink_ ? 0 : -1;
            double_blink_ = false;
        }
    } else if (time_ms_ >= next_blink_ms_ && mood != Mood::Sleeping && mood != Mood::Loading &&
               mood != Mood::Speaking && mood != Mood::Confused && activity_ != Activity::Speaking) {
        // (no blinking on the cross symbols or while talking: it reads as jitter)
        blink_elapsed_ms_ = 0;
        if (standby) {
            // Slow, relaxed blinks at a calm pace.
            blink_duration_ms_ = 520;
            double_blink_ = false;
            next_blink_ms_ = time_ms_ + 4500 + Random() % 3000;
        } else {
            blink_duration_ms_ = 160;
            double_blink_ = (Random() % 5) == 0;
            next_blink_ms_ = time_ms_ + 2200 + Random() % 3800;
        }
    }

    // Gaze: the pupils wander while idle and look at the user otherwise.
    const bool wander = (activity_ == Activity::Idle &&
                         (mood == Mood::Focused || mood == Mood::Curious ||
                          mood == Mood::Surprised)) ||
                        standby;
    if (wander) {
        if (time_ms_ >= next_gaze_ms_) {
            if (Random() % 4 == 0) {
                gaze_target_x_ = 0;
                gaze_target_y_ = 0;
            } else if (standby) {
                // Anywhere inside the visible circle (the drawing clamps to it).
                gaze_target_x_ = RandomRange(-10.0f, 10.0f);
                gaze_target_y_ = RandomRange(-8.0f, 8.0f);
            } else {
                gaze_target_x_ = RandomRange(-7.0f, 7.0f);
                gaze_target_y_ = RandomRange(-4.0f, 4.0f);
            }
            next_gaze_ms_ = time_ms_ + (standby ? 3500 + Random() % 4000 : 2500 + Random() % 3500);
        }
    } else {
        gaze_target_x_ = 0;
        gaze_target_y_ = 0;
    }
    // Standby drifts slowly and smoothly; awake eyes move quicker.
    const float gaze_tau = standby ? 900.0f : 260.0f;
    gaze_x_ = Approach(gaze_x_, gaze_target_x_, elapsed_ms, gaze_tau);
    gaze_y_ = Approach(gaze_y_, gaze_target_y_, elapsed_ms, gaze_tau);

    // Speaking: random "syllables" make the symbols pulse with the voice.
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

    // Expression change: squash the old symbol, then open the new one.
    if (swap_elapsed_ms_ < 0 && mood != shown_mood_) {
        swap_elapsed_ms_ = 0;
    }
    if (swap_elapsed_ms_ >= 0) {
        int32_t before = swap_elapsed_ms_;
        swap_elapsed_ms_ += static_cast<int32_t>(elapsed_ms);
        if (before < kSwapHalfMs && swap_elapsed_ms_ >= kSwapHalfMs) {
            shown_mood_ = mood;  // fully squashed: switch symbols
        }
        if (swap_elapsed_ms_ >= 2 * kSwapHalfMs) {
            swap_elapsed_ms_ = -1;
            shown_mood_ = mood;
        }
    }
}

void RoboEyes::Tick(uint32_t elapsed_ms) {
    if (obj_ == nullptr) return;
    time_ms_ += elapsed_ms;
    UpdateBehavior(elapsed_ms);
    lv_obj_invalidate(obj_);
}

void RoboEyes::DrawEventCb(lv_event_t* e) {
    auto* self = static_cast<RoboEyes*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (self != nullptr && layer != nullptr) {
        self->Draw(layer);
    }
}

// ---------------------------------------------------------------- primitives

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

void RoboEyes::FillCircle(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t r, lv_color_t color) {
    FillRect(layer, cx - r, cy - r, cx + r, cy + r, LV_RADIUS_CIRCLE, color);
}

void RoboEyes::FillTriangle(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = on_;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

void RoboEyes::DrawLine(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        int32_t width) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = on_;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x0;
    dsc.p1.y = y0;
    dsc.p2.x = x1;
    dsc.p2.y = y1;
    lv_draw_line(layer, &dsc);
}

// ---------------------------------------------------------------- symbols

void RoboEyes::DrawGlyph(lv_layer_t* layer, int32_t cx, int32_t cy, const EyeGlyph& g,
                         float scale, float scale_y) {
    const float w = g.w * scale;
    const float h = g.h * scale;

    // Nearly shut (blink or expression swap): every symbol becomes a thin line.
    if (scale_y < 0.3f && g.glyph != Glyph::Spinner) {
        const int32_t half = std::max<int32_t>(2, Round(std::max(w, h * 0.6f) / 2));
        FillRect(layer, cx - half, cy - 1, cx + half, cy + 1, 1, on_);
        return;
    }

    switch (g.glyph) {
        case Glyph::Bar:
        case Glyph::Square: {
            const int32_t hw = Round(w / 2);
            const int32_t hh = std::max<int32_t>(1, Round(h * scale_y / 2));
            const int32_t radius = std::min<int32_t>(Round(g.radius * scale), std::min(hw, hh));
            FillRect(layer, cx - hw, cy - hh, cx + hw - 1, cy + hh - 1, radius, on_);
            break;
        }
        case Glyph::Arc: {
            // An upside-down U: "^" happy eyes.
            const int32_t radius = Round(w / 2);
            const int32_t stroke = std::max<int32_t>(2, Round(h));
            lv_draw_arc_dsc_t dsc;
            lv_draw_arc_dsc_init(&dsc);
            dsc.color = on_;
            dsc.opa = LV_OPA_COVER;
            dsc.width = stroke;
            dsc.rounded = 1;
            dsc.start_angle = 180;
            dsc.end_angle = 360;
            dsc.center.x = cx;
            dsc.center.y = cy + Round(radius * (1.0f - scale_y) * 0.5f);
            dsc.radius = static_cast<uint16_t>(radius);
            lv_draw_arc(layer, &dsc);
            break;
        }
        case Glyph::Cross: {
            const int32_t sx = Round(w / 2);
            const int32_t sy = std::max<int32_t>(1, Round(w / 2 * scale_y));
            const int32_t stroke = std::max<int32_t>(2, Round(h));
            DrawLine(layer, cx - sx, cy - sy, cx + sx, cy + sy, stroke);
            DrawLine(layer, cx - sx, cy + sy, cx + sx, cy - sy, stroke);
            break;
        }
        case Glyph::HalfDisc: {
            // Flat on top, round at the bottom.
            const int32_t hw = Round(w / 2);
            const int32_t hh = std::max<int32_t>(2, Round(h * scale_y));
            const int32_t top = cy - hh / 2;
            FillRect(layer, cx - hw, top - hh, cx + hw - 1, top + hh, hh, on_);
            FillRect(layer, cx - hw - 1, top - hh - 1, cx + hw, top - 1, 0, off_);
            break;
        }
        case Glyph::Dots4: {
            const int32_t r = std::max<int32_t>(1, Round(h));
            const int32_t ox = Round(w / 2);
            const int32_t oy = std::max<int32_t>(1, Round(w / 2 * scale_y));
            FillCircle(layer, cx - ox, cy - oy, r, on_);
            FillCircle(layer, cx + ox, cy - oy, r, on_);
            FillCircle(layer, cx - ox, cy + oy, r, on_);
            FillCircle(layer, cx + ox, cy + oy, r, on_);
            break;
        }
        case Glyph::Spinner: {
            // Eight dots in a ring; a two-dot gap travels around.
            const float ring = w / 2;
            const int32_t r = std::max<int32_t>(1, Round(h));
            const int gap = static_cast<int>((time_ms_ / 110) % 8);
            for (int i = 0; i < 8; i++) {
                if (i == gap || i == (gap + 1) % 8) continue;
                float a = i * kPi / 4;
                FillCircle(layer, cx + Round(std::cos(a) * ring), cy + Round(std::sin(a) * ring), r,
                           on_);
            }
            break;
        }
        case Glyph::Heart: {
            const int32_t size = Round(w);
            const int32_t r = size / 4 + 1;
            const int32_t sy_size = Round(size * scale_y);
            const int32_t top = cy - sy_size / 4;
            FillCircle(layer, cx - r + 1, top, r, on_);
            FillCircle(layer, cx + r - 1, top, r, on_);
            FillTriangle(layer, cx - 2 * r + 1, top + r / 3, cx + 2 * r - 1, top + r / 3, cx,
                         cy + sy_size / 2);
            break;
        }
        case Glyph::Equals: {
            const int32_t hw = Round(w / 2);
            const int32_t bar = std::max<int32_t>(2, Round(h));
            const int32_t spread = std::max<int32_t>(bar, Round((h + g.radius * 1.5f) * scale_y));
            FillRect(layer, cx - hw, cy - spread - bar / 2, cx + hw - 1, cy - spread + bar / 2, 1, on_);
            FillRect(layer, cx - hw, cy + spread - bar / 2, cx + hw - 1, cy + spread + bar / 2, 1, on_);
            break;
        }
    }
}

void RoboEyes::Draw(lv_layer_t* layer) {
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    const int32_t ox = coords.x1;
    const int32_t oy = coords.y1;

    // The sockets themselves are not drawn: only the symbols show, placed where
    // the 12.2 mm eyes sit (touching the screen edges, 5 mm apart).
    const int32_t d = eye_diameter_;

    EyeGlyph left, right;
    GlyphsFor(shown_mood_, left, right);

    // Vertical squash from blink and expression swaps.
    float scale_y = 1.0f;
    if (blink_elapsed_ms_ >= 0) {
        float phase = std::min(1.0f, static_cast<float>(blink_elapsed_ms_) / blink_duration_ms_);
        scale_y = std::min(scale_y, 1.0f - 0.95f * std::sin(phase * kPi));
    }
    if (swap_elapsed_ms_ >= 0) {
        float t = static_cast<float>(swap_elapsed_ms_) / kSwapHalfMs;  // 0..2
        scale_y = std::min(scale_y, t < 1.0f ? 1.0f - t : t - 1.0f);
    }

    // Size pulses and whole-face motion per activity / mood.
    float scale = 1.0f;
    float move_x = gaze_x_;
    float move_y = gaze_y_;
    const Mood mood = shown_mood_;
    switch (activity_) {
        case Activity::Listening:
            // Steady, with a very slow, small "breath".
            scale = 1.0f + 0.05f * (0.5f - 0.5f * std::cos(time_ms_ * 2.0f * kPi / 3200.0f));
            break;
        case Activity::Speaking:
            // Slow, even pulse instead of random jumps: no position change at all.
            scale = 1.0f + 0.07f * (0.5f - 0.5f * std::cos(time_ms_ * 2.0f * kPi / 2000.0f));
            break;
        case Activity::Music: {
            float beat = 0.5f - 0.5f * std::cos(time_ms_ * 2.0f * kPi / 1090.0f);
            move_y += 1.5f - beat * 3.0f;
            move_x += std::sin(time_ms_ * kPi / 2180.0f) * 3.0f;
            break;
        }
        case Activity::Alarm:
            move_x += std::sin(time_ms_ / 22.0f) * 3.0f;  // shaking
            break;
        default:
            break;
    }
    if (mood == Mood::Sleeping) {
        scale_y = std::min(scale_y, 0.85f + 0.15f * std::sin(time_ms_ / 900.0f));  // breathing
    }
    if (mood == Mood::InLove) {
        scale *= 1.0f + 0.12f * std::max(0.0f, std::sin(time_ms_ / 160.0f));
    }

    // Keep each symbol inside its socket.
    // The eye circles are not drawn: they only bound where the symbols may move
    // (a small margin keeps them clear of the circle's edge).
    const float inner = d / 2.0f - kEdgeMargin;
    auto place = [&](int32_t socket_cx, const EyeGlyph& g) {
        float half = std::max(g.w, g.glyph == Glyph::Arc ? g.w / 2 : g.h) * scale / 2.0f;
        float limit = std::max(0.0f, inner - half);
        float x = g.dx + move_x;
        float y = g.dy + move_y;
        float dist = std::sqrt(x * x + y * y);
        if (dist > limit && dist > 0.0f) {
            x *= limit / dist;
            y *= limit / dist;
        }
        DrawGlyph(layer, ox + socket_cx + Round(x), oy + eye_cy_ + Round(y), g, scale, scale_y);
    };
    place(left_cx_, left);
    place(right_cx_, right);

    if (mood == Mood::Crying && scale_y > 0.6f) {
        // A tear slides down below each pupil, one after the other.
        const float k = d / 53.0f;
        for (int side = 0; side < 2; side++) {
            const EyeGlyph& g = side == 0 ? left : right;
            const int32_t socket_cx = side == 0 ? left_cx_ : right_cx_;
            const uint32_t phase = (time_ms_ + side * 600) % 1200;
            const float start = g.dy + g.h / 2 + 3 * k;
            const float travel = d / 2.0f - 6 * k - start;
            const int32_t tx = ox + socket_cx + Round(g.dx + (side == 0 ? -2 : 2) * k);
            const int32_t ty = oy + eye_cy_ + Round(start + travel * phase / 1200.0f);
            const int32_t tw = std::max<int32_t>(2, Round(3 * k));
            const int32_t th = std::max<int32_t>(3, Round(5 * k));
            FillRect(layer, tx - tw / 2, ty, tx - tw / 2 + tw - 1, ty + th - 1, tw / 2, on_);
        }
    }
}
