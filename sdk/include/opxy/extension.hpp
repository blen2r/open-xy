#pragma once
#include <opxy/keys.hpp>
#include <opxy/types.hpp>

namespace opxy { namespace extensions {

// Opaque project/pattern identities are only for equality, never dereferencing.
// Positions and lengths use stock pattern ticks (480 per step at scale 1).
struct RecordingContext {
    u32 project, pattern, track, length, scale, quantization;
    // The selected track's pattern can be edited: a project, an instrument
    // track, a pattern, a length and a supported scale.
    bool editable;
    // Live notes on it can be recorded: editable, and the recorder is not in a
    // step or sample workflow.
    bool recordable;
    bool operator==(const RecordingContext& other) const {
        return project == other.project && pattern == other.pattern &&
            track == other.track && length == other.length && scale == other.scale &&
            quantization == other.quantization && editable == other.editable &&
            recordable == other.recordable;
    }
};
struct Context { RecordingContext recording; bool playing; };
struct Note { i32 position; u32 length; u8 pitch, velocity; };
struct LiveNote { u32 track, pitch, velocity; i32 position; u32 collision_radius; };
struct NoteOff { u32 track, pitch; };
struct Tick { u32 delta_q8; bool discontinuity; };
using Time = __UINT64_TYPE__;
// An encoder edit of a track's parameter while an extension captures
// (Actions::capture): the track (0..15: instrument tracks, and aux tracks no
// machine holds), the parameter's index in the track's 58 live words, and the
// edited value.
struct Edit { u32 track, index; i32 value; };
// Key numbers as stock reports them in queued controls (opxy/keys.hpp).
namespace keys = ::opxy::keys;
struct Control {
    u32 key;
    bool pressed;
    // Stock took the event as input: input was enabled and, for Record, stock
    // registered the press or release.
    bool accepted;
    // No other key and no Shift held when the event arrived.
    bool unmodified;
    Time time;
};
// A chord claim's key (profiles/extensions.json, "chord"): while the claim's
// held key (`lead`, the metronome for metronome_chord) is down, a chord key
// (the keyboard's) pressed or released. While the extension takes the held key
// (Actions::take), the held key's own press and release arrive too, with
// key == lead. None of these reach stock.
struct Gesture {
    u32 lead, key;
    bool pressed;
    Time time;
};

// Each action needs its permission (profiles/extensions.json) and works only in
// a queued_control callback for a key the extension claims, once stock accepted
// it. The SDK validates the context again and owns undo/publication: one stock
// undo transaction per callback. Append new actions at the end.
struct Actions {
    // Add notes to an instrument track's pattern (0-7) beside its existing
    // notes. Returns the inserted prefix length. Needs edit_notes.
    u32 (*insert_notes)(const Context&, u32 track, const Note*, u32 count);
    // Show a short popup (1-31 characters), once per callback. Needs notify.
    void (*notify)(const char* text);
    // Replace an instrument track's pattern notes with these. Refused, changing
    // nothing, while stock is still recording a held note there. Needs edit_notes.
    u32 (*replace_notes)(const Context&, u32 track, const Note*, u32 count);
    // Capture encoder edits (track control's capture, shared with machines):
    // mode 1 audibly (the edits play as usual), 2 silently (what plays doesn't
    // change), 0 to end it. It lasts while the extension renews it in every
    // gui_tick; ending it, or not renewing it, returns every captured parameter
    // to its value from before. Starting needs one of the extension's claims (a
    // queued_control callback for its key); renewing works in gui_tick while it
    // holds the capture. One owner at a time, machines and extensions alike:
    // the one capturing keeps it; otherwise machines that ask go first, by
    // track, then extensions, in collection order. Returns 1 while this
    // extension holds the capture (from the tick after it first asks), else 0.
    // Needs capture.
    u32 (*capture)(u32 mode);
    // Take a chord claim's held key (1), or give it back to stock (0): while
    // taken, its presses and releases arrive as gestures and never reach stock,
    // so a tap of it alone is the extension's. It lasts while the extension
    // renews it in every gui_tick. Works in gesture and gui_tick callbacks, for
    // an extension holding a chord claim; no permission of its own.
    void (*take)(u32 on);
};

using OnContext = void (*)(const Context&);
using OnRecordableNoteOn = void (*)(const Context&, const LiveNote&);
using OnSelectedNoteOff = void (*)(const Context&, const NoteOff&);
using OnRecordingTick = void (*)(const Context&, const Tick&);
using OnInputActivity = void (*)();
using OnQueuedControl = void (*)(const Context&, const Control&, const Actions&);
using OnGuiTick = void (*)(const Context&, const Actions&);
using OnCapturedEdits = void (*)(const Context&, const Edit* edits, u32 count);
using OnGesture = void (*)(const Context&, const Gesture&, const Actions&);

// One callback per event, named for what it delivers. Each event lists its
// firmware sites in the profile ("events"); docs/extensions.md says how to
// widen one or propose another. An extension names the events it takes:
//
//     extern constexpr Hooks hooks = Hooks().with_context(context)
//                                           .with_queued_control(queued_control);
//
// All callbacks are synchronous on the controller path, in collection order.
// Arguments are borrowed for the callback. No allocation, blocking or I/O.
// Observers cannot transform live input or consume stock controls.
struct Hooks {
    // Selected-track recording context and play state; before every other event.
    OnContext context;
    // A live onset stock accepted for recording, on the selected recordable
    // instrument track, while playing. Sequenced notes and step entry excluded.
    OnRecordableNoteOn recordable_note_on;
    // A live release on the selected track, playing or not; it may have no
    // preceding recordable onset.
    OnSelectedNoteOff selected_note_off;
    // The selected track's scaled recording time, while playing and recordable.
    OnRecordingTick recording_tick;
    // Any live note input, accepted or not, on any track.
    OnInputActivity input_activity;
    // A queued control after stock handled it; not the keys a chord claim
    // keeps from stock. Actions act only for their claim owner.
    OnQueuedControl queued_control;
    // Once per GUI tick, after stock's own work for the tick and track
    // control's, whether or not the sequencer plays. Its actions renew a
    // capture the extension holds (Actions::capture).
    OnGuiTick gui_tick;
    // While this extension captures: the edits since the last GUI tick, oldest
    // first, at most 32, before gui_tick.
    OnCapturedEdits captured_edits;
    // A chord claim's key, for the extension holding the claim (Gesture),
    // before stock would have seen it. Its actions are the claim's.
    OnGesture gesture;

    constexpr Hooks() : Hooks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) {}
    // Every callback in order; prefer the with_ builders below.
    constexpr Hooks(OnContext a, OnRecordableNoteOn b, OnSelectedNoteOff c, OnRecordingTick d,
                    OnInputActivity e, OnQueuedControl f, OnGuiTick g, OnCapturedEdits h, OnGesture i)
        : context(a), recordable_note_on(b), selected_note_off(c), recording_tick(d),
          input_activity(e), queued_control(f), gui_tick(g), captured_edits(h), gesture(i) {}

    constexpr Hooks with_context(OnContext f) const {
        return Hooks(f, recordable_note_on, selected_note_off, recording_tick, input_activity, queued_control, gui_tick, captured_edits, gesture);
    }
    constexpr Hooks with_recordable_note_on(OnRecordableNoteOn f) const {
        return Hooks(context, f, selected_note_off, recording_tick, input_activity, queued_control, gui_tick, captured_edits, gesture);
    }
    constexpr Hooks with_selected_note_off(OnSelectedNoteOff f) const {
        return Hooks(context, recordable_note_on, f, recording_tick, input_activity, queued_control, gui_tick, captured_edits, gesture);
    }
    constexpr Hooks with_recording_tick(OnRecordingTick f) const {
        return Hooks(context, recordable_note_on, selected_note_off, f, input_activity, queued_control, gui_tick, captured_edits, gesture);
    }
    constexpr Hooks with_input_activity(OnInputActivity f) const {
        return Hooks(context, recordable_note_on, selected_note_off, recording_tick, f, queued_control, gui_tick, captured_edits, gesture);
    }
    constexpr Hooks with_queued_control(OnQueuedControl f) const {
        return Hooks(context, recordable_note_on, selected_note_off, recording_tick, input_activity, f, gui_tick, captured_edits, gesture);
    }
    constexpr Hooks with_gui_tick(OnGuiTick f) const {
        return Hooks(context, recordable_note_on, selected_note_off, recording_tick, input_activity, queued_control, f, captured_edits, gesture);
    }
    constexpr Hooks with_captured_edits(OnCapturedEdits f) const {
        return Hooks(context, recordable_note_on, selected_note_off, recording_tick, input_activity, queued_control, gui_tick, f, gesture);
    }
    constexpr Hooks with_gesture(OnGesture f) const {
        return Hooks(context, recordable_note_on, selected_note_off, recording_tick, input_activity, queued_control, gui_tick, captured_edits, f);
    }
};
}}
