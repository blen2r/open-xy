// Key numbers as stock reports them in its key queue, shared by extensions
// (extensions::Control, extensions::Gesture) and machines (control::Gesture).
#pragma once
#include <opxy/types.hpp>

namespace opxy { namespace keys {
const u32 metronome = 1;
const u32 m1 = 6;             // M1..M4: 6..9
const u32 encoder_press = 10; // the four encoders' presses: 10..13
const u32 track = 14;         // the eight track keys: 14..21
// The keyboard's 24 keys: 26..49.
const u32 keyboard_first = 26, keyboard_last = 49;
const u32 record = 50;
inline bool keyboard(u32 key) { return key >= keyboard_first && key <= keyboard_last; }
}}
