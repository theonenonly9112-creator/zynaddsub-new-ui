// ModernKnob.h - flat, modern rotary knob for FLTK 1.3
//
// Look:   dark body, darker "socket", neon green value arc (starts bottom-left and sweeps
//         clockwise to bottom-right), white pointer.
// Input:  drag up = increase, drag down = decrease (hold Shift for fine control),
//         mouse wheel, double-click = reset to the default value.
// Use:    ModernKnob *k = new ModernKnob(x, y, 48, 48, "CUTOFF");
//         k->range(0, 127); k->default_value(64); k->value(64); k->callback(my_cb);
#ifndef MODERN_KNOB_H
#define MODERN_KNOB_H

#include <FL/Fl_Widget.H>

class ModernKnob : public Fl_Widget
{
public:
    ModernKnob(int x, int y, int w, int h, const char *label = 0);

    // value access (same names as Fl_Valuator, so it is easy to swap in)
    double value() const { return value_; }
    int    value(double v);                       // returns 1 if the value changed
    void   range(double minimum, double maximum);
    double minimum() const { return min_; }
    double maximum() const { return max_; }
    void   default_value(double v) { default_ = v; }
    double default_value() const { return default_; }

    // how many pixels of vertical dragging sweep the whole range (default 200)
    void   drag_pixels(int pixels) { drag_pixels_ = pixels > 10 ? pixels : 10; }

protected:
    void draw();
    int  handle(int event);

private:
    double fraction() const;                      // 0.0 .. 1.0
    double clamp(double v) const;

    double value_, min_, max_, default_;
    double start_value_;                          // value when the drag started
    int    start_y_;                              // mouse y when the drag started
    bool   fine_;                                 // Shift held during the drag
    bool   changed_;                              // value changed since the button went down
    int    drag_pixels_;
};

#endif
