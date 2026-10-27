// Track control (SDK 1.4): a synth that declares resources.trackControl runs
// control() on the GUI side once per stock GUI tick while it is on an
// instrument track. It reads every instrument track's live parameters and may
// overlay other tracks' values for that tick, or capture their encoder edits.
// Overlays never reach the saved project: stock keeps its own values, and a
// parameter the machine stops overlaying returns to them.
#pragma once
#include <opxy/types.hpp>

extern "C" opxy::u32 sdk_control_copies(opxy::u32 track);

namespace opxy { namespace control {
// Instrument tracks 0..7, and stock's live parameter words for each: the
// engine's eight controls (0..7), then stock's own track parameters in stock's
// units. Every live value is non-negative.
const u32 tracks = 8;
const u32 track_parameters = 58;
const u32 overlay_capacity = 64;
const u32 edit_capacity = 32;
// A machine that declares resources.trackControl.savedBytes (1..saved_capacity)
// keeps data with each project, on the GUI side, through two more functions:
//   u32 control_save(const ControlState&, u8* data, u32 capacity);
// writes at most `capacity` (its savedBytes) bytes when stock saves a project,
// the workspace included, and returns how many; and
//   void control_load(ControlState&, const u8* data, u32 bytes);
// gets them back after stock loads a project saved with them, or `bytes` 0
// after it loads a project without them.
const u32 saved_capacity = 8192;

// One parameter of an instrument track: an overlay entry or a captured edit.
struct Entry { u8 track, index; u16 reserved; i32 value; };

struct Input {
    u32 track;  // the instrument track this machine is on
    // Each track's live values as stock sets them (sequenced, locked or
    // edited), without any overlay.
    i32 values[tracks][track_parameters];
    // While the machine captures: encoder edits of other instrument tracks'
    // parameters since the last tick, oldest first, with the edited value.
    u32 edit_count;
    Entry edits[edit_capacity];
};

// What the machine returns each tick. An overlay entry for the machine's own
// track, or outside tracks x track_parameters, is ignored; a later entry for
// the same parameter replaces an earlier one; negative values are treated as 0.
struct Overlay {
    u32 count;  // 0 on entry; at most overlay_capacity
    Entry entries[overlay_capacity];
    // 0 on entry. 1 captures encoder edits: they still play and show as
    // usual, and arrive in the next tick's edits. When the machine stops
    // capturing, every parameter edited meanwhile returns to its value from
    // before the capture.
    u32 capture;
    // 0 on entry. 1 marks the pages of the tracks this tick overlays with a
    // thin dark red frame. While capturing, every page has a red frame anyway.
    u32 mark;
};

// GUI side, for a display: how many instrument tracks hold the same machine as
// `track`, `track` included. Track control runs only while exactly one does.
// Browser previews answer 1.
inline u32 copies(u32 track) { return sdk_control_copies(track); }

inline void add(Overlay& overlay, u32 track, u32 index, i32 value) {
    if (overlay.count >= overlay_capacity || track >= tracks || index >= track_parameters) return;
    Entry& entry = overlay.entries[overlay.count++];
    entry.track = static_cast<u8>(track);
    entry.index = static_cast<u8>(index);
    entry.reserved = 0;
    entry.value = value;
}
}}
