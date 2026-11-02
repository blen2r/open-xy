#pragma once
#include <opxy/types.hpp>

namespace opxy {

// Reads a chord claim's gestures (extensions::Gesture, control::Gesture) the
// same way for every owner: a chord is a chord key pressed while the held key
// is down; a tap is the held key released with no chord key since its press.
// Taps arrive only while the owner takes the held key; otherwise its tap is
// stock's.
class ChordTap {
public:
    enum Result { none, chord, tap };
    constexpr ChordTap() : down_(false), chorded_(false) {}

    Result feed(u32 lead, u32 key, bool pressed) {
        if (key != lead) {
            if (!pressed) return none;
            chorded_ = true;
            return chord;
        }
        if (pressed) {
            down_ = true;
            chorded_ = false;
            return none;
        }
        const bool alone = down_ && !chorded_;
        down_ = chorded_ = false;
        return alone ? tap : none;
    }
    // The held key is down, as far as its gestures tell.
    bool held() const { return down_; }

private:
    bool down_, chorded_;
};

}
