// Stock OS 1.2.1 GUI tick for track control. 8c0baacc, the GUI's per-tick
// update, calls 8d580074 (a void leaf) just before the sequencer tick; the
// hook replaces that CALL.A, so stock calls it with the C ABI.
//
// Stock keeps each track's live parameters in g_project + 0x4080, 58 words a
// track: the values edits, locks and sequencing set, which 8d5acea8 writes and
// publishes to the audio side. The saved project holds separate base values,
// so an overlay written here never reaches a saved project. Each tick the SDK
// notices stock's own writes over an overlaid value, gives the machine every
// value without overlays, writes its new overlay through 8d5acea8, and
// restores what stock last set wherever the machine stopped overlaying.
//
// Capture: 8d5ad564, the encoder edit stock's controllers reach through
// 8d5ad5bc, reads the base value at track data + 0x3864 and, unless a step is
// held for a lock, calls the setter 8d5acf28 with the new value. That call at
// 8d5ad5ae routes through sdk_param_edit_set. While a machine captures, it
// records the edit and the base value from before the first one, then makes
// the original call; when capture ends, the SDK sets each base value back.
//
// Display: stock's displayed-value getter 8d5ac9dc shows a parameter's live
// value while the sequencer plays and its base value otherwise, plus any LFO
// amount. A jump hook at 8d5aca48 (track_control_hooks.s) asks
// sdk_param_shown instead, which shows an overlaid parameter's overlay either
// way, except for a moment after an encoder edit of it, when it shows the base
// value the edit changed. Every other parameter shows as stock shows it. The
// host track's own engine controls reach the machine as that getter shows
// them, so an LFO on one of them moves the machine like a turn.
#include "track_control.hpp"
#include "stock.hpp"
#include "synth_controls.hpp"
#include "benchmark.hpp"

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
i32 shown(u32 track, u32 index) {
    return stock<i32(u32, u32, u32, u32)>(0x8d5ac9dc)(kProject, track, index, 1);
}
i32 base(u32 track, u32 index) {
    return static_cast<i32>(word(stock<u32(u32)>(0x8d587708)(track) + 0x3864 + index * 4));
}

// One overlaid parameter: the value written, and stock's own value beneath it.
struct Applied { u8 track, index, wanted; i32 written, underlying; };
Applied applied[control::overlay_capacity];
u32 applied_count;
control::Input input;
control::Overlay overlay;
// The machine and track that ran last tick, to reset its state on a change.
const ControlMachine* host_machine;
u32 host_track = control::tracks;
// The core clock, 400 MHz, as cycles per microsecond, and the cycle count at
// the last tick, for each tick's elapsed time.
const u32 kCyclesPerMicrosecond = 400;
u32 last_cycles;
bool timed;
// GUI ticks so far, and the last encoder edit's parameter and tick. For
// kTurnTicks after it the edited parameter shows its base value.
const u32 kTurnTicks = 40;
u32 gui_ticks, turned_track = control::tracks, turned_index, turned_tick;

// Capture: edits since the last tick, and each edited parameter's base value
// from before the capture. A parameter with no room to restore is not captured.
const u32 kOriginals = 2 * control::overlay_capacity;
bool capturing;
// The machine asked to mark the pages of the tracks it overlays.
bool marking;
control::Entry pending[control::edit_capacity];
u32 pending_count;
control::Entry originals[kOriginals];
u32 original_count;

const ControlMachine* owner(u32 track) {
    const u32 engine = track_engine(track);
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        if (m.engine() == engine) return m.ready() == 1 ? &m : nullptr;
    }
    return nullptr;
}

Applied* find(u32 track, u32 index) {
    for (u32 i = 0; i < applied_count; ++i)
        if (applied[i].track == track && applied[i].index == index) return &applied[i];
    return nullptr;
}

// Restoring a base value also makes it stock's value beneath any overlay.
void end_capture() {
    for (u32 i = 0; i < original_count; ++i) {
        const control::Entry& o = originals[i];
        param_set(kProject, o.track, o.index, o.value);
        if (Applied* a = find(o.track, o.index)) a->underlying = o.value;
    }
    original_count = 0;
    pending_count = 0;
}

// An entry the platform applies: in range, not on the host track, and the
// last one for its parameter.
bool applies(u32 i, u32 host) {
    const control::Entry& e = overlay.entries[i];
    if (e.track >= control::tracks || e.index >= control::track_parameters || e.track == host) return false;
    for (u32 j = i + 1; j < overlay.count; ++j)
        if (overlay.entries[j].track == e.track && overlay.entries[j].index == e.index) return false;
    return true;
}
}

u32 opxy::platform::control_marks() {
    if (capturing) return kMarkCapture;
    if (!marking) return 0;
    const u32 track = selected_track();
    for (u32 i = 0; i < applied_count; ++i)
        if (applied[i].track == track) return kMarkOverlaid;
    return 0;
}

extern "C" u32 sdk_control_copies(u32 track) {
    if (track >= 16) return 0;
    const u32 engine = track_engine(track);
    u32 count = 0;
    for (u32 t = 0; t < control::tracks; ++t) count += track_engine(t) == engine;
    return count;
}

extern "C" i32 sdk_param_shown(u32 project, u32 track, u32 index, u32 playing) {
    const Applied* a = track < control::tracks && index < control::track_parameters ? find(track, index) : nullptr;
    const bool turned = track == turned_track && index == turned_index && gui_ticks - turned_tick < kTurnTicks;
    if (a && !turned) return a->written;
    if (playing && !a) return live(track, index);
    return stock<i32(u32, u32, u32)>(0x8d5ac910)(project, track, index);
}

extern "C" void sdk_param_edit_set(u32 project, u32 track, u32 index, i32 value) {
    turned_track = track;
    turned_index = index;
    turned_tick = gui_ticks;
    if (capturing && track < control::tracks && track != host_track && index < control::track_parameters) {
        bool known = false;
        for (u32 i = 0; i < original_count && !known; ++i)
            known = originals[i].track == track && originals[i].index == index;
        if (!known && original_count < kOriginals) {
            control::Entry& o = originals[original_count++];
            o.track = static_cast<u8>(track);
            o.index = static_cast<u8>(index);
            o.reserved = 0;
            o.value = base(track, index);
            known = true;
        }
        if (known && pending_count < control::edit_capacity) {
            control::Entry& e = pending[pending_count++];
            e.track = static_cast<u8>(track);
            e.index = static_cast<u8>(index);
            e.reserved = 0;
            e.value = value < 0 ? 0 : value;
        }
    }
    param_set(project, track, index, value);
}

extern "C" void sdk_gui_tick() {
    stock<void()>(0x8d580074)();
    ++gui_ticks;
    sdk_synth_pages();
    if (!sdk_control_machine_count && !applied_count) return;

    // A value that differs from the one written is stock's: an edit, a lock or a step.
    for (u32 i = 0; i < applied_count; ++i) {
        const i32 value = live(applied[i].track, applied[i].index);
        if (value != applied[i].written) applied[i].underlying = value;
        applied[i].wanted = 0;
    }

    // The first instrument track that holds a ready control machine runs it,
    // unless another track holds the same machine: then none runs.
    const ControlMachine* m = nullptr;
    u32 track = control::tracks;
    for (u32 t = 0; t < control::tracks && !m; ++t)
        if ((m = owner(t))) track = t;
    if (m && sdk_control_copies(track) > 1) {
        m = nullptr;
        track = control::tracks;
    }
    overlay.count = 0;
    overlay.capture = 0;
    overlay.mark = 0;
    if (m) {
        for (u32 t = 0; t < control::tracks; ++t)
            for (u32 p = 0; p < control::track_parameters; ++p) input.values[t][p] = live(t, p);
        for (u32 i = 0; i < applied_count; ++i)
            input.values[applied[i].track][applied[i].index] = applied[i].underlying;
        // The host's own controls as its page shows them, LFO included.
        for (u32 p = 0; p < kEngineControls; ++p) input.values[track][p] = shown(track, p);
        input.track = track;
        input.transport = opxy::Transport{project_bpm_x10(), word(kTicks96), byte(kPlaying) || byte(kPlaying + 1)};
        input.edit_count = pending_count;
        for (u32 i = 0; i < pending_count; ++i) input.edits[i] = pending[i];
        pending_count = 0;
        const u32 now = opxy::benchmark::cycles(), us = (now - last_cycles) / kCyclesPerMicrosecond;
        input.elapsed_us = timed ? (us < 1000000 ? us : 1000000) : 0;
        last_cycles = now;
        timed = true;
        if (m != host_machine || track != host_track) {
            if (capturing) end_capture();
            m->reset();
            input.elapsed_us = 0;
        }
        m->control(&input, &overlay);
        if (overlay.count > control::overlay_capacity) overlay.count = control::overlay_capacity;
    }
    host_machine = m;
    host_track = track;
    // Ending a capture sets its base values back before this tick's overlay.
    const bool capture = m && overlay.capture;
    if (capturing && !capture) end_capture();
    capturing = capture;
    marking = m && overlay.mark;

    // Mark what the machine still overlays; restore and drop the rest.
    for (u32 i = 0; i < overlay.count; ++i) {
        if (!applies(i, track)) continue;
        if (Applied* a = find(overlay.entries[i].track, overlay.entries[i].index)) a->wanted = 1;
    }
    u32 kept = 0;
    for (u32 i = 0; i < applied_count; ++i) {
        const Applied& a = applied[i];
        if (a.wanted) applied[kept++] = a;
        else if (live(a.track, a.index) == a.written) live_set(a.track, a.index, a.underlying);
    }
    applied_count = kept;

    // Write the overlay.
    for (u32 i = 0; i < overlay.count; ++i) {
        if (!applies(i, track)) continue;
        const control::Entry& e = overlay.entries[i];
        const i32 value = e.value < 0 ? 0 : e.value;
        Applied* a = find(e.track, e.index);
        if (!a) {
            if (applied_count >= control::overlay_capacity) continue;
            a = &applied[applied_count++];
            a->track = e.track;
            a->index = e.index;
            a->underlying = live(e.track, e.index);
        }
        a->wanted = 1;
        a->written = value;
        if (live(e.track, e.index) != value) live_set(e.track, e.index, value);
    }
}
