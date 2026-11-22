// ModernKnob.cpp - flat, modern rotary knob for FLTK 1.3
#include "ModernKnob.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <cmath>

namespace {

const double PI        = 3.14159265358979323846;
const double START_DEG = 225.0;   // bottom-left, degrees counter-clockwise from 3 o'clock
const double SWEEP_DEG = 270.0;   // sweeps clockwise to the bottom-right

// colors
const uchar BODY[3]    = {0x20, 0x20, 0x20};   // #202020  dark gray body
const uchar SOCKET[3]  = {0x10, 0x10, 0x10};   // darker inner circle
const uchar TRACK[3]   = {0x38, 0x38, 0x38};   // faint full-range track
const uchar GREEN[3]   = {0x00, 0xC8, 0x50};   // #00C850 neon green value arc
const uchar POINTER[3] = {0xF2, 0xF2, 0xF2};   // white pointer (use GREEN for a green one)

inline void set_color(const uchar c[3], bool active)
{
    if(active)
        fl_color(c[0], c[1], c[2]);
    else  // inactive widgets are drawn washed out
        fl_color((uchar)(c[0] / 2 + 60), (uchar)(c[1] / 2 + 60), (uchar)(c[2] / 2 + 60));
}

} // namespace

ModernKnob::ModernKnob(int X, int Y, int W, int H, const char *L)
    : Fl_Widget(X, Y, W, H, L),
      value_(0.0), min_(0.0), max_(1.0), default_(0.0),
      start_value_(0.0), start_y_(0), fine_(false), changed_(false), drag_pixels_(200)
{
    when(FL_WHEN_CHANGED);   // callback while dragging (use FL_WHEN_RELEASE for "on release")
}

double ModernKnob::clamp(double v) const
{
    const double lo = min_ < max_ ? min_ : max_;
    const double hi = min_ < max_ ? max_ : min_;
    return v < lo ? lo : (v > hi ? hi : v);
}

int ModernKnob::value(double v)
{
    v = clamp(v);
    if(v == value_)
        return 0;
    value_ = v;
    redraw();
    return 1;
}

void ModernKnob::range(double minimum, double maximum)
{
    min_ = minimum;
    max_ = maximum;
    value_ = clamp(value_);
    redraw();
}

double ModernKnob::fraction() const
{
    const double span = max_ - min_;
    if(span == 0.0)
        return 0.0;
    const double f = (value_ - min_) / span;
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

void ModernKnob::draw()
{
    fl_push_clip(x(), y(), w(), h());          // never draw outside the widget

    const bool   on   = active_r() != 0;
    const int    size = (w() < h() ? w() : h()) & ~1;       // even diameter
    const int    cx   = x() + w() / 2;
    const int    cy   = y() + h() / 2;
    const int    left = cx - size / 2;
    const int    top  = cy - size / 2;
    const double frac = fraction();

    // 1. dark gray body
    set_color(BODY, on);
    fl_pie(left, top, size, size, 0, 360);

    // 2. darker inner circle: the "socket"
    const int inner = (int)(size * 0.62) & ~1;
    set_color(SOCKET, on);
    fl_pie(cx - inner / 2, cy - inner / 2, inner, inner, 0, 360);

    // 3. value arc: from the bottom-left, clockwise
    const int lw = size >= 40 ? 4 : 3;                       // arc thickness
    const int ai = lw / 2 + 1;                               // inset from the body edge
    const int ax = left + ai, ay = top + ai, as = size - 2 * ai;
    fl_line_style(FL_SOLID | FL_CAP_ROUND, lw);
    set_color(TRACK, on);                                    // faint full-range track
    fl_arc(ax, ay, as, as, START_DEG - SWEEP_DEG, START_DEG);
    if(frac > 0.0) {
        set_color(GREEN, on);                                // FLTK arcs run counter-clockwise,
        fl_arc(ax, ay, as, as, START_DEG - SWEEP_DEG * frac, START_DEG);   // so start .. START_DEG
    }

    // 4. pointer from the center to the edge (stops just inside the arc)
    const double ang = (START_DEG - SWEEP_DEG * frac) * PI / 180.0;
    const double rad = size / 2.0 - lw - 2.0;
    const int    ex  = cx + (int)std::floor(std::cos(ang) * rad + 0.5);
    const int    ey  = cy - (int)std::floor(std::sin(ang) * rad + 0.5);
    fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
    set_color(POINTER, on);
    fl_line(cx, cy, ex, ey);

    fl_line_style(0);                                        // restore the default line style
    fl_pop_clip();
}

int ModernKnob::handle(int event)
{
    switch(event) {
    case FL_ENTER:
    case FL_LEAVE:
        return 1;

    case FL_PUSH:
        if(Fl::event_button() != FL_LEFT_MOUSE)
            return 0;
        if(Fl::event_clicks() > 0) {                         // double-click: reset
            Fl::event_clicks(0);
            if(value(default_))
                do_callback();
        }
        start_y_     = Fl::event_y();
        start_value_ = value_;
        fine_        = (Fl::event_state() & FL_SHIFT) != 0;
        changed_     = false;
        return 1;

    case FL_DRAG: {
        const bool fine = (Fl::event_state() & FL_SHIFT) != 0;
        if(fine != fine_) {                                  // modifier changed: restart the drag
            fine_        = fine;
            start_y_     = Fl::event_y();
            start_value_ = value_;
        }
        const double dy     = start_y_ - Fl::event_y();      // dragging up gives a positive dy
        const double pixels = drag_pixels_ * (fine_ ? 5.0 : 1.0);
        if(value(start_value_ + dy / pixels * (max_ - min_))) {
            changed_ = true;
            if(when() & FL_WHEN_CHANGED)
                do_callback();
        }
        return 1;
    }

    case FL_RELEASE:
        if(changed_ && (when() & FL_WHEN_RELEASE) && !(when() & FL_WHEN_CHANGED))
            do_callback();
        changed_ = false;
        return 1;

    case FL_MOUSEWHEEL: {
        const double step = (max_ - min_) / ((Fl::event_state() & FL_SHIFT) ? 500.0 : 50.0);
        if(value(value_ - Fl::event_dy() * step))            // wheel up (dy < 0) increases
            do_callback();
        return 1;
    }
    }
    return Fl_Widget::handle(event);
}
