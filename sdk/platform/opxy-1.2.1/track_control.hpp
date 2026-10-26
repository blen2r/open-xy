// Synths with track control: generated table (track_control_table.cpp.in);
// the GUI-tick overlay lives in track_control.cpp.
#pragma once
#include <opxy/track_control.hpp>

namespace opxy { namespace platform {
// In a browser kit an unselected machine's ready resolves to a function returning 0.
struct ControlMachine {
    const char* id;  // absolute symbol: the engine ID, supplied at link time
    int (*ready)();
    void (*reset)();
    void (*control)(const control::Input*, control::Overlay*);
    u32 engine() const { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(id)); }
};
}}

extern "C" {
extern const opxy::platform::ControlMachine sdk_control_machines[] __asm__("sdk_control_machines");
extern const opxy::u32 sdk_control_machine_count __asm__("sdk_control_machine_count");
void sdk_gui_tick() __asm__("sdk_gui_tick");
void sdk_param_edit_set(opxy::u32 project, opxy::u32 track, opxy::u32 index, opxy::i32 value)
    __asm__("sdk_param_edit_set");
// The value stock's displayed-value getter shows before its LFO amount, through
// the jump hook sdk_param_shown_hook (track_control_hooks.s).
opxy::i32 sdk_param_shown(opxy::u32 project, opxy::u32 track, opxy::u32 index, opxy::u32 playing)
    __asm__("sdk_param_shown");
}
