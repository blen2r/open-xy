// Stock OS 1.2.1 GUI tick for track control. 8c0baacc, the GUI's per-tick
// update, calls 8d580074 (void; it calls 8d5be290) just before the sequencer tick; the
// hook replaces that CALL.A, so stock calls it with the C ABI.
//
// Stock keeps each track's live parameters in g_project + 0x4080, 58 words a
// track: the values edits, locks and sequencing set, which 8d5acea8 writes and
// publishes to the audio side. The saved project holds separate base values,
// so an overlay written here never reaches a saved project. Each tick the SDK
// notices stock's own writes over an overlaid value, runs every machine on
// tracks 0..7, then on the aux tracks machines took over (aux_tracks.cpp), in
// track order with every value without overlays, writes their
// overlays through 8d5acea8 (the later track's winning a parameter both
// overlay), and restores what stock last set wherever no machine overlays.
//
// Capture: 8d5ad564, the encoder edit stock's controllers reach through
// 8d5ad5bc, reads the base value at track data + 0x3864 and, unless a step is
// held for a lock, calls the setter 8d5acf28 with the new value. That call at
// 8d5ad5ae routes through sdk_param_edit_set. While a machine captures (one
// copy at a time), it records the edit and the base value from before the
// first one, then makes the original call; when capture ends, the SDK sets each
// base value back.
//
// Display: stock's displayed-value getter 8d5ac9dc shows a parameter's live
// value while the sequencer plays and its base value otherwise, plus any LFO
// amount. A jump hook at 8d5aca48 (track_control_hooks.s) asks
// sdk_param_shown instead, which shows an overlaid parameter's overlay either
// way, except for a moment after an encoder edit of it, when it shows the base
// value the edit changed. Every other parameter shows as stock shows it. The
// host track's own engine controls reach the machine as that getter shows
// them, so an LFO on one of them moves the machine like a turn.
//
// Mute: stock keeps it per scene, in the scene records at g_project (the word
// at 840a0524) + 0x98, 33 bytes each: each track's pattern, then each track's
// flags, in which 0x02 is muted, then one more byte; the current scene is the
// byte at g_project + 6. Aux + track key and the mixer both set it; a new
// scene copies the current one's (measured on the device with a memory probe).
// While a machine's track is muted, as with stock's brain, none of its
// overlays, capture or marks apply; it still runs and is told (Input.muted).
#include "track_control.hpp"
#include "stock.hpp"
#include "synth_controls.hpp"
#include "benchmark.hpp"
#include "../keys.hpp"
#include <opxy/extension.hpp>

namespace {
using opxy::i32;
using opxy::u32;
using opxy::u8;
using namespace opxy::platform;
namespace control = opxy::control;

const u32 kProject = 0x86bd027c;
const u32 kLive = kProject + 0x4080;

i32& live(u32 track, u32 index) {
    return *at<i32>(kLive + (track * control::track_parameters + index) * 4);
}
void live_set(u32 track, u32 index, i32 value) {
    stock<void(u32, u32, u32, i32)>(0x8d5acea8)(kProject, track, index, value);
}
void param_set(u32 project, u32 track, u32 index, i32 value) {
    stock<void(u32, u32, u32, i32)>(0x8d5acf28)(project, track, index, value);
}
// Stock's displayed value: 8d5ac9dc(project, track, index, 1), as the pages ask.
const u32 kEngineControls = 8;
// Whether `track` is muted in the current scene.
bool muted(u32 track) {
    const u32 project = word(0x840a0524);
    return byte(project + 0x98 + byte(project + 6) * 33 + 16 + track) & 0x02;
}
i32 shown(u32 track, u32 index) {
    return stock<i32(u32, u32, u32, u32)>(0x8d5ac9dc)(kProject, track, index, 1);
}
i32 base(u32 track, u32 index) {
    return static_cast<i32>(word(stock<u32(u32)>(0x8d587708)(track) + 0x3864 + index * 4));
}

// One overlaid parameter: the value written, stock's own value beneath it,
// and the track of the machine whose overlay it is.
struct Applied { u8 track, index, wanted, owner; i32 written, underlying; u32 block; };
// Every machine's overlays together.
const u32 kAppliedCapacity = 2 * control::overlay_capacity;
Applied applied[kAppliedCapacity];
u32 applied_count;
// This tick's overlays from every machine, the later track's winning a parameter.
struct Wanted { u8 track, index, owner; i32 value; };
Wanted wanted[kAppliedCapacity];
u32 wanted_count;
control::Input input;
control::Overlay overlay;
// Every track, aux tracks included, can host a machine; kNoTrack is none.
const u32 kNoTrack = control::all_tracks;
// The machine each track ran last tick, to reset a copy that arrives, and
// whether that copy's next tick is timed.
const ControlMachine* hosts[control::all_tracks];
bool timed[control::all_tracks];
// The core clock, 400 MHz, as cycles per microsecond, and the cycle count at
// the last tick, for each tick's elapsed time.
const u32 kCyclesPerMicrosecond = 400;
u32 last_cycles;
// Each instrument track's pattern block as the last tick saw it (0 before the first).
u32 seen_blocks[control::all_tracks];
// GUI ticks so far, and the last encoder edit's parameter and tick. For
// kTurnTicks after it the edited parameter shows its base value.
const u32 kTurnTicks = 40;
u32 gui_ticks, turned_track = kNoTrack, turned_index, turned_tick;

// Capture: the track whose copy captures (kNoTrack when none), edits since the
// last tick, and each edited parameter's base value from before the capture.
// A parameter with no room to restore is not captured.
//
// A capture is audible or silent (Overlay::capture 1 or 2). Silently, the
// captured parameters form a layer between stock and the machines' overlays:
// each is held at what stock would play without the capture's edits (`held`:
// its value from before them, then what the sequencer's steps set), which the
// machines read as stock's value and which plays where no machine overlays it.
// An edit then writes only the stored value, which stock's encoder adds the
// next turn to, so nothing of it is heard until the capture ends.
const u32 kOriginals = 2 * control::overlay_capacity;
//
// The owner is a machine (`capture_track`, the track it holds) or an extension
// (`capture_extension`, its place in the collection plus 1), never both. An
// extension asks from its gui_tick (sdk_capture_request), which runs after
// this tick, so its request counts as asking in the next one.
const u32 kAudible = 1, kSilent = 2;
u32 capture_track = kNoTrack, capture_mode, capture_extension;
u32 extension_asks, extension_mode;
// The tracks whose machine asked to mark the pages of the tracks it overlays.
bool marking[control::all_tracks];
control::Entry pending[control::edit_capacity];
u32 pending_count;
struct Original { control::Entry edit; u32 block, engine; i32 last, held; };
Original originals[kOriginals];
u32 original_count;
u32 capture_project;
// What each track's machine passed to its display last tick.
i32 shown_words[control::all_tracks][control::display_words];
// Chord claims' keys for one track's machine (the keys service, keys.cpp),
// for its next tick; and the tracks whose machine takes the held key.
u32 gesture_track = kNoTrack, gesture_count;
control::Gesture gestures[control::gesture_capacity];
bool taking[control::all_tracks];
// The popup a machine asked for, kept while stock shows it.
char popup_text[control::popup_bytes];
// What the pages show this tick in place of what plays (Overlay::shown), from
// every machine, the later track's winning a parameter.
const u32 kShownCapacity = 2 * control::shown_capacity;
control::Entry shown_values[kShownCapacity];
u32 shown_value_count;
control::Entry* shown_value(u32 track, u32 index) {
    for (u32 i = 0; i < shown_value_count; ++i)
        if (shown_values[i].track == track && shown_values[i].index == index) return &shown_values[i];
    return nullptr;
}
void merge_shown(u32 host, const control::Overlay& o) {
    const u32 count = o.shown_count < control::shown_capacity ? o.shown_count : control::shown_capacity;
    for (u32 i = 0; i < count; ++i) {
        const control::Entry& e = o.shown[i];
        if (e.track >= control::all_tracks || e.index >= control::track_parameters || e.track == host) continue;
        control::Entry* to = shown_value(e.track, e.index);
        if (!to && shown_value_count < kShownCapacity) to = &shown_values[shown_value_count++];
        if (to) *to = {e.track, e.index, 0, e.value < 0 ? 0 : e.value};
    }
}

}

opxy::platform::AuxService sdk_aux;

u32 opxy::platform::control_engine(u32 track) {
    if (track < control::tracks) return track_engine(track);
    return sdk_aux.engine ? sdk_aux.engine(track) : 0;
}

namespace {
// Machines steer (overlay and capture) every track but the aux tracks a
// machine holds: their own, and another aux machine's, so none steers itself
// through another. Instrument tracks never hold one.
bool steerable(u32 track) {
    return track < control::tracks || (track < control::all_tracks && !control_engine(track));
}
const ControlMachine* owner(u32 track) {
    const u32 engine = control_engine(track);
    if (!engine) return nullptr;
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        if (m.engine() == engine) return m.ready() == 1 ? &m : nullptr;
    }
    return nullptr;
}
// The page key last handled on an aux track, once, for that track's machine.
u32 key_track = kNoTrack, key_number;
u32 take_page_key(u32 track) {
    if (track != key_track) return 0;
    key_track = kNoTrack;
    return key_number;
}

u32 machine_index(const ControlMachine* m) { return static_cast<u32>(m - sdk_control_machines); }

Applied* find(u32 track, u32 index) {
    for (u32 i = 0; i < applied_count; ++i)
        if (applied[i].track == track && applied[i].index == index) return &applied[i];
    return nullptr;
}

bool capturing() { return capture_track < kNoTrack || capture_extension != 0; }
bool silent() { return capturing() && capture_mode == kSilent; }

// The captured parameter `index` of `track` in its current pattern, or null.
Original* captured(u32 track, u32 index) {
    if (!capturing() || track >= control::all_tracks) return nullptr;
    const u32 block = stock<u32(u32)>(0x8d587708)(track);
    for (u32 i = 0; i < original_count; ++i)
        if (originals[i].edit.track == track && originals[i].edit.index == index && originals[i].block == block)
            return &originals[i];
    return nullptr;
}

// What a silently held parameter plays: a machine's overlay where one applies, else its held value.
i32 silent_value(const Original& o) {
    const Applied* a = find(o.edit.track, o.edit.index);
    return a && a->block == o.block ? a->written : o.held;
}

// A capture changing mode: silently, the captured parameters play their held
// values; audibly again, their edited ones.
void set_capture_mode(u32 mode) {
    if (mode == capture_mode) return;
    capture_mode = mode;
    for (u32 i = 0; i < original_count; ++i) {
        Original& o = originals[i];
        if (o.block != stock<u32(u32)>(0x8d587708)(o.edit.track)) continue;
        if (mode == kSilent) o.held = o.edit.value;
        const i32 value = mode == kSilent ? o.held : base(o.edit.track, o.edit.index);
        // Beneath a machine's overlay, it becomes the value the overlay gives back.
        if (Applied* a = find(o.edit.track, o.edit.index)) {
            if (a->block == o.block) { a->underlying = value; continue; }
        }
        live_set(o.edit.track, o.edit.index, value);
    }
}

// Restoring a base value also makes it stock's value beneath any overlay.
void end_capture() {
    for (u32 i = 0; i < original_count; ++i) {
        if (capture_project != word(0x840a0524)) break;
        const Original& original = originals[i];
        const control::Entry& o = original.edit;
        i32* stored = at<i32>(original.block + 0x3864 + o.index * 4);
        // A replaced engine/preset or another writer owns its new value.
        if (word(original.block + 0x18) != original.engine || *stored != original.last) continue;
        if (stock<u32(u32)>(0x8d587708)(o.track) == original.block) {
            param_set(kProject, o.track, o.index, o.value);
            if (Applied* a = find(o.track, o.index)) a->underlying = o.value;
        } else {
            // Restore the pattern actually edited without publishing it into
            // the track's new current pattern. Its edit already dirtied the project.
            *stored = o.value;
        }
    }
    original_count = 0;
    pending_count = 0;
    capture_track = kNoTrack;
    capture_extension = 0;
    capture_mode = 0;
}

// Adds the overlay of the machine on `host`: entries in range and not on its
// own track; a later entry, or a later track's, replaces one for the same parameter.
void merge(u32 host) {
    for (u32 i = 0; i < overlay.count; ++i) {
        const control::Entry& e = overlay.entries[i];
        if (!steerable(e.track) || e.index >= control::track_parameters || e.track == host) continue;
        Wanted* w = nullptr;
        for (u32 j = 0; j < wanted_count && !w; ++j)
            if (wanted[j].track == e.track && wanted[j].index == e.index) w = &wanted[j];
        if (!w) {
            if (wanted_count >= kAppliedCapacity) continue;
            w = &wanted[wanted_count++];
            w->track = e.track;
            w->index = e.index;
        }
        w->owner = static_cast<u8>(host);
        w->value = e.value < 0 ? 0 : e.value;
    }
}
}

Marks opxy::platform::control_marks() {
    if (capture_track < kNoTrack && hosts[capture_track])
        return Marks{kMarkCapture, machine_index(hosts[capture_track])};
    // An extension's capture shows the SDK's marks: the row after the machines'.
    if (capture_extension) return Marks{kMarkCapture, sdk_control_machine_count};
    const u32 track = selected_track();
    for (u32 i = 0; i < applied_count; ++i) {
        const Applied& a = applied[i];
        if (a.track == track && marking[a.owner] && hosts[a.owner])
            return Marks{kMarkOverlaid, machine_index(hosts[a.owner])};
    }
    return Marks{0, 0};
}

extern "C" i32 sdk_control_display(u32 index) {
    const u32 track = selected_track();
    return track < control::all_tracks && index < control::display_words ? shown_words[track][index] : 0;
}

extern "C" u32 sdk_control_copies(u32 track) {
    if (track >= control::all_tracks) return 0;
    const u32 engine = control_engine(track);
    if (!engine) return 0;
    u32 count = 0;
    for (u32 t = 0; t < control::all_tracks; ++t) count += control_engine(t) == engine;
    return count;
}

extern "C" i32 sdk_param_shown(u32 project, u32 track, u32 index, u32 playing) {
    const Applied* a = track < control::all_tracks && index < control::track_parameters ? find(track, index) : nullptr;
    const bool turned = track == turned_track && index == turned_index && gui_ticks - turned_tick < kTurnTicks;
    // A silently captured parameter shows its edited value while it is turned, then what plays.
    const Original* o = silent() ? captured(track, index) : nullptr;
    if (o && turned) return base(track, index);
    // What a machine asks the pages to show, in place of what plays.
    if (const control::Entry* e = turned ? nullptr : shown_value(track, index)) return e->value;
    if (o) return silent_value(*o);
    if (a && !turned) return a->written;
    if (playing && !a) return live(track, index);
    return stock<i32(u32, u32, u32)>(0x8d5ac910)(project, track, index);
}

// A machine on the CV track keeps its own LFO whatever track is selected. Stock's
// LFO telemetry (ids 0x16/0x17) is one pair with no track in it, published only
// for the selected track. So the aux evaluator's publish (8d50f422) also puts this
// track's amount in id 25, which stock neither writes nor reads: a tag, the track,
// the slot and the amount's top 16 bits. The telemetry table (080376e4, one on each
// board) carries it to the GUI, where the displayed-value getter uses it for this
// track in place of the pair. One word carries one track.
namespace {
const u32 kTelemetry = 0x080376e4, kHeldId = 25, kHeldTrack = 11, kHeldTag = 0xa0000000u | kHeldTrack << 24;
}

extern "C" i32 sdk_lfo_held_amount(u32 track, u32 index, i32 stock_amount) {
    if (track != kHeldTrack || !hosts[track]) return stock_amount;
    if (!hosts[track]->shared_lfo) return 0;
    const u32 held = word(kTelemetry + kHeldId * 4);
    if ((held & 0xff000000u) != kHeldTag) return stock_amount;
    if (!byte(stock<u32(u32)>(0x8d587708)(track) + 0x24) || ((held >> 16) & 0xff) != index) return 0;
    return static_cast<i32>(held << 16);
}

extern "C" void sdk_param_edit_set(u32 project, u32 track, u32 index, i32 value) {
    turned_track = track;
    turned_index = index;
    turned_tick = gui_ticks;
    if (capturing() && steerable(track) && track != capture_track &&
        index < control::track_parameters) {
        const u32 block = stock<u32(u32)>(0x8d587708)(track);
        Original* known = nullptr;
        for (u32 i = 0; i < original_count && !known; ++i)
            if (originals[i].edit.track == track && originals[i].edit.index == index && originals[i].block == block)
                known = &originals[i];
        if (known && (known->engine != track_engine(track) || base(track, index) != known->last)) {
            // A new engine/preset or another writer replaced the captured base.
            // Further edits now restore to that replacement, not the earlier one,
            // and a silent capture holds it.
            known->engine = track_engine(track);
            known->edit.value = base(track, index);
            known->held = known->edit.value;
        }
        if (!known && original_count < kOriginals) {
            known = &originals[original_count++];
            known->block = block;
            known->engine = track_engine(track);
            control::Entry& o = known->edit;
            o.track = static_cast<u8>(track);
            o.index = static_cast<u8>(index);
            o.reserved = 0;
            o.value = base(track, index);
            known->held = silent() && !find(track, index) ? live(track, index) : o.value;
        }
        if (known) known->last = value < 0 ? 0 : value;
        if (known && pending_count < control::edit_capacity) {
            control::Entry& e = pending[pending_count++];
            e.track = static_cast<u8>(track);
            e.index = static_cast<u8>(index);
            e.reserved = 0;
            e.value = value < 0 ? 0 : value;
        }
        if (known && silent()) {
            // Only the stored value moves, which the encoder's next turn adds to.
            *at<i32>(block + 0x3864 + index * 4) = known->last;
            return;
        }
    }
    param_set(project, track, index, value);
}

// Called by the GUI tick after a project load or new project, before the
// machines run (sdk_control_kept_apply). It touches none of stock's buffers.
extern "C" void sdk_control_queue_page_key(u32 track, u32 key) {
    key_track = track;
    key_number = key;
}

// An extension's capture (extension_runtime.cpp): `owner` is its place in the
// collection plus 1. A request, from its gui_tick or a claimed key, counts in
// the next GUI tick; mode 0 ends a capture it holds at once.
extern "C" {
u32 sdk_capture_request(u32 owner, u32 mode) __asm__("sdk_capture_request");
u32 sdk_capture_holds(u32 owner) __asm__("sdk_capture_holds");
u32 sdk_capture_edits(u32 owner, opxy::extensions::Edit* edits, u32 capacity) __asm__("sdk_capture_edits");
}
extern "C" u32 sdk_capture_request(u32 owner, u32 mode) {
    if (!owner || mode > kSilent) return 0;
    if (mode == 0) {
        if (capture_extension == owner) end_capture();
        if (extension_asks == owner) extension_asks = 0;
        return 0;
    }
    if (capture_track < kNoTrack || (capture_extension && capture_extension != owner)) return 0;
    if (extension_asks && extension_asks != owner) return 0;
    extension_asks = owner;
    extension_mode = mode;
    return 1;
}
extern "C" u32 sdk_capture_holds(u32 owner) { return owner && capture_extension == owner; }
// The edits since the last tick, for the extension that captures; they are its once.
extern "C" u32 sdk_capture_edits(u32 owner, opxy::extensions::Edit* edits, u32 capacity) {
    if (!owner || capture_extension != owner) return 0;
    const u32 count = pending_count < capacity ? pending_count : capacity;
    for (u32 i = 0; i < count; ++i) edits[i] = {pending[i].track, pending[i].index, pending[i].value};
    pending_count = 0;
    return count;
}

// The keys service's gesture layer (keys.cpp): the track whose running,
// unmuted machine holds a chord claim, plus 1, or 0; and its keys.
extern "C" u32 sdk_control_claimant(u32 claim) {
    if (claim >= 32) return 0;
    for (u32 t = 0; t < control::all_tracks; ++t)
        if (hosts[t] && (hosts[t]->claims & (1u << claim)) && !muted(t)) return t + 1;
    return 0;
}
extern "C" void sdk_control_queue_gesture(u32 track, const KeyGesture* g) {
    if (!g || track >= control::all_tracks) return;
    if (track != gesture_track) gesture_track = track, gesture_count = 0;
    if (gesture_count >= control::gesture_capacity) return;
    gestures[gesture_count++] = {static_cast<u8>(g->lead), static_cast<u8>(g->key),
                                 static_cast<u8>(g->pressed ? 1 : 0), 0};
}
extern "C" u32 sdk_control_takes(u32 track) { return track < control::all_tracks && taking[track]; }

extern "C" void sdk_control_forget() {
    sdk_control_queue_page_key(kNoTrack, 0);
    gesture_track = kNoTrack;
    gesture_count = 0;
    shown_value_count = 0;
    original_count = pending_count = applied_count = wanted_count = 0;
    capture_track = kNoTrack;
    capture_extension = extension_asks = 0;
    for (u32 t = 0; t < control::all_tracks; ++t) {
        hosts[t] = nullptr;
        timed[t] = marking[t] = taking[t] = false;
        for (u32 i = 0; i < control::display_words; ++i) shown_words[t][i] = 0;
    }
}

// 8d5937b4, 8d5938c2, 8d59396a: where stock's sequencer writes a step's parameter
// locks to the live parameters (8d5936c0, from 8d593a78 for every track, and
// 8d593980), plain or sliding toward the next lock. Without this, a lock on an
// overlaid parameter would sound until the next tick puts the overlay back: a
// click. An overlaid parameter keeps its overlay instead, and the lock becomes
// stock's value beneath it, as the next tick would have found it.
extern "C" void sdk_param_step_set(u32 project, u32 track, u32 index, i32 value) {
    // Silently captured: a step's lock plays and is held; the stored value it
    // would write back is the edited one, so the value from before the edits plays.
    if (Original* o = silent() ? captured(track, index) : nullptr) {
        o->held = value == base(track, index) ? o->edit.value : (value < 0 ? 0 : value);
        value = o->held;
    }
    if (track < control::all_tracks)
        if (Applied* a = find(track, index))
            if (a->block == stock<u32(u32)>(0x8d587708)(track)) {
                a->underlying = value < 0 ? 0 : value;
                value = a->written;
            }
    stock<void(u32, u32, u32, i32)>(0x8d5acea8)(project, track, index, value);
}

// Once per GUI tick, from the tick service's dispatcher (startup.cpp.in), after stock's call.
extern "C" void sdk_control_tick() {
    ++gui_ticks;
    if (!sdk_control_machine_count && !applied_count) return;
    sdk_control_kept_apply();
    if (sdk_aux.tick) sdk_aux.tick();

    // A pattern change while the sequencer is stopped leaves the track's live
    // values as the old pattern had them until play starts (stock's stopped pages
    // show the stored values instead). A machine would read, and overlay, those
    // stale values: give the track its new pattern's stored values, as starting
    // play does.
    const bool playing = byte(kPlaying) || byte(kPlaying + 1);
    for (u32 t = 0; t < control::all_tracks; ++t) {
        const u32 block = stock<u32(u32)>(0x8d587708)(t);
        if (!playing && seen_blocks[t] && block != seen_blocks[t] && steerable(t))
            for (u32 p = 0; p < control::track_parameters; ++p) {
                // A silently captured parameter of that pattern keeps playing its held value.
                const Original* o = silent() ? captured(t, p) : nullptr;
                const i32 value = o ? silent_value(*o) : base(t, p);
                if (live(t, p) != value) live_set(t, p, value);
            }
        seen_blocks[t] = block;
    }

    // A value that differs from the one written is stock's: an edit, a lock or a step.
    u32 retained = 0;
    for (u32 i = 0; i < applied_count; ++i) {
        Applied a = applied[i];
        // A pattern change, or a machine taking the aux track over (its values are the machine's now).
        if (a.block != stock<u32(u32)>(0x8d587708)(a.track) || !steerable(a.track)) continue;
        const i32 value = live(a.track, a.index);
        if (const Original* o = silent() ? captured(a.track, a.index) : nullptr) a.underlying = o->held;
        else if (value != a.written) a.underlying = value;
        a.wanted = 0;
        applied[retained++] = a;
    }
    applied_count = retained;

    // Every track's ready control machine runs, in track order:
    // each copy of a machine with its own state, except a unique machine that
    // more than one track holds, which none runs.
    const u32 now = opxy::benchmark::cycles(), us = (now - last_cycles) / kCyclesPerMicrosecond;
    last_cycles = now;
    wanted_count = 0;
    shown_value_count = 0;
    bool loaded = false, kept = false;
    u32 asking = kNoTrack, kept_mode = 0, asking_mode = 0;
    for (u32 t = 0; t < control::all_tracks; ++t) {
        const ControlMachine* m = owner(t);
        if (m && m->unique && sdk_control_copies(t) > 1) m = nullptr;
        const u32 copy = m && !m->unique ? t : 0;
        if (m != hosts[t]) {
            if (capture_track == t) end_capture();
            hosts[t] = m;
            timed[t] = false;
            // A unique machine keeps one state for the project: it is reset before
            // its first call and after a load or new project, not on each arrival.
            if (m && !(m->unique && sdk_control_prepared[machine_index(m)])) m->reset(copy);
            if (m && m->unique) sdk_control_prepared[machine_index(m)] = true;
        }
        marking[t] = taking[t] = false;
        for (u32 i = 0; i < control::display_words; ++i) shown_words[t][i] = 0;
        if (!m) continue;
        if (!loaded) {
            for (u32 u = 0; u < control::all_tracks; ++u)
                for (u32 p = 0; p < control::track_parameters; ++p) input.values[u][p] = live(u, p);
            for (u32 u = 0; u < control::all_tracks; ++u) {
                i32& last = input.values[u][control::track_parameters - 1];
                if (last == kAuxMark || last == kAuxRelease) last = base(u, control::track_parameters - 1);
            }
            for (u32 i = 0; i < applied_count; ++i)
                input.values[applied[i].track][applied[i].index] = applied[i].underlying;
            input.transport = opxy::Transport{project_bpm_x10(), word(kTicks96), byte(kPlaying) || byte(kPlaying + 1)};
            loaded = true;
        }
        // The host's own controls as its page shows them, LFO included.
        i32 own[kEngineControls];
        for (u32 p = 0; p < kEngineControls; ++p) {
            own[p] = input.values[t][p];
            input.values[t][p] = shown(t, p);
        }
        input.track = t;
        input.page_key = take_page_key(t);
        input.muted = muted(t);
        input.capturing = capture_track == t;
        input.edit_count = input.capturing ? pending_count : 0;
        for (u32 i = 0; i < input.edit_count; ++i) input.edits[i] = pending[i];
        if (input.capturing) pending_count = 0;
        input.gesture_count = t == gesture_track ? gesture_count : 0;
        for (u32 i = 0; i < input.gesture_count; ++i) input.gestures[i] = gestures[i];
        if (t == gesture_track) gesture_track = kNoTrack, gesture_count = 0;
        input.elapsed_us = timed[t] ? (us < 1000000 ? us : 1000000) : 0;
        timed[t] = true;
        overlay.count = 0;
        overlay.capture = 0;
        overlay.mark = 0;
        overlay.take = 0;
        overlay.popup[0] = 0;
        overlay.shown_count = 0;
        for (u32 i = 0; i < control::display_words; ++i) overlay.display[i] = 0;
        m->control(copy, &input, &overlay);
        for (u32 p = 0; p < kEngineControls; ++p) input.values[t][p] = own[p];
        if (overlay.count > control::overlay_capacity) overlay.count = control::overlay_capacity;
        if (input.muted) overlay.count = overlay.capture = overlay.mark = overlay.take = overlay.shown_count = 0;
        merge_shown(t, overlay);
        taking[t] = overlay.take && m->claims;
        if (overlay.popup[0] && !input.muted) {
            u32 n = 0;
            while (n + 1 < control::popup_bytes && overlay.popup[n]) popup_text[n] = overlay.popup[n], ++n;
            if (!overlay.popup[n]) {
                popup_text[n] = 0;
                show_popup(popup_text);
            }
        }
        if (overlay.capture) {
            const u32 mode = overlay.capture == kSilent ? kSilent : kAudible;
            if (t == capture_track) kept = true, kept_mode = mode;
            else if (asking == kNoTrack) asking = t, asking_mode = mode;
        }
        marking[t] = overlay.mark != 0;
        for (u32 i = 0; i < control::display_words; ++i) shown_words[t][i] = overlay.display[i];
        merge(t);
    }

    // The copy capturing keeps on while it asks; otherwise the first asking
    // starts. Ending a capture sets its base values back before this tick's overlays.
    // An extension that captures keeps on while it renews; one asking starts
    // only when no machine does.
    if (capture_extension && extension_asks == capture_extension) kept = true, kept_mode = extension_mode;
    if (!kept) {
        if (capturing()) end_capture();
        capture_track = asking;
        capture_extension = asking == kNoTrack ? extension_asks : 0;
        capture_project = word(0x840a0524);
        capture_mode = asking < kNoTrack ? asking_mode : capture_extension ? extension_mode : 0;
    } else {
        set_capture_mode(kept_mode);
    }
    extension_asks = 0;

    // Mark what the machines still overlay; restore and drop the rest.
    for (u32 i = 0; i < wanted_count; ++i)
        if (Applied* a = find(wanted[i].track, wanted[i].index)) a->wanted = 1;
    u32 count = 0;
    for (u32 i = 0; i < applied_count; ++i) {
        const Applied& a = applied[i];
        if (a.wanted) applied[count++] = a;
        else if (live(a.track, a.index) == a.written) live_set(a.track, a.index, a.underlying);
    }
    applied_count = count;

    // Write the overlays.
    for (u32 i = 0; i < wanted_count; ++i) {
        const Wanted& w = wanted[i];
        Applied* a = find(w.track, w.index);
        if (!a) {
            if (applied_count >= kAppliedCapacity) continue;
            a = &applied[applied_count++];
            a->track = w.track;
            a->index = w.index;
            a->block = stock<u32(u32)>(0x8d587708)(w.track);
            a->underlying = live(w.track, w.index);
        }
        a->wanted = 1;
        a->owner = w.owner;
        a->written = w.value;
        if (live(w.track, w.index) != w.value) live_set(w.track, w.index, w.value);
    }
}
