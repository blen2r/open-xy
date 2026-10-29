// Synths with track control: generated table (track_control_table.cpp.in);
// the GUI-tick overlay lives in track_control.cpp.
#pragma once
#include <opxy/track_control.hpp>

namespace opxy { namespace platform {

// In a browser kit an unselected machine's ready resolves to a function returning 0.
struct ControlMachine {
    const char* id;  // absolute symbol: the engine ID, supplied at link time
    int (*ready)();
    // Each copy's state: copy 0 for a unique machine, else the copy's track.
    void (*reset)(u32 copy);
    void (*control)(u32 copy, const control::Input*, control::Overlay*);
    // What it keeps with each project (project_companion.cpp): its machine ID,
    // at most saved_bytes, and save/load, or 0 and null when it keeps nothing.
    const char* name;
    u32 saved_bytes;
    u32 (*save)(u8* data);
    void (*load)(const u8* data, u32 bytes);
    bool unique;  // runs only while one track holds it
    // The aux tracks it may take over (aux_tracks.cpp), one bit per track
    // index, or 0; then its own screen page (an absolute symbol, or null) and
    // its eight controls' defaults, which it starts from on an aux track.
    u32 aux;
    const char* page;
    i32 defaults[8];
    u32 engine() const { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(id)); }
    u32 page_id() const { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(page)); }
};
// The engine whose machine runs on a track: the machine holding an aux track,
// else stock's engine on an instrument track; 0 on an aux track nothing holds.
// GUI only (aux_tracks.cpp).
u32 control_engine(u32 track);
// The marks track control shows over every page (track_control_overlay.cpp.in):
// a machine captures edits, or the selected track holds an overlay from one that
// marks; `machine` indexes sdk_control_machines. GUI only.
const u32 kMarkCapture = 1, kMarkOverlaid = 2;
struct Marks { u32 kind, machine; };
Marks control_marks();
}}

extern "C" {
extern const opxy::platform::ControlMachine sdk_control_machines[] __asm__("sdk_control_machines");
extern const opxy::u32 sdk_control_machine_count __asm__("sdk_control_machine_count");
void sdk_gui_tick() __asm__("sdk_gui_tick");
// Registers the marks' screen slot on the GUI role, after the machines' screens.
void sdk_overlay_startup() __asm__("sdk_overlay_startup");
// Once per GUI tick, before the machines run: aux tracks taken over keep their
// mark word published (aux_tracks.cpp).
void sdk_aux_tick() __asm__("sdk_aux_tick");
void sdk_param_edit_set(opxy::u32 project, opxy::u32 track, opxy::u32 index, opxy::i32 value)
    __asm__("sdk_param_edit_set");
// The value stock's displayed-value getter shows before its LFO amount, through
// the jump hook sdk_param_shown_hook (track_control_hooks.s).
opxy::i32 sdk_param_shown(opxy::u32 project, opxy::u32 track, opxy::u32 index, opxy::u32 playing)
    __asm__("sdk_param_shown");
}
