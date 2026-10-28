// Stock OS 1.2.1 GUI tick for track control. 8c0baacc, the GUI's per-tick
// update, calls 8d580074 (a void leaf) just before the sequencer tick; the
// hook replaces that CALL.A, so stock calls it with the C ABI.
//
// Stock keeps each track's live parameters in g_project + 0x4080, 58 words a
// track: the values edits, locks and sequencing set, which 8d5acea8 writes and
// publishes to the audio side. The saved project holds separate base values,
// so an overlay written here never reaches a saved project. Each tick the SDK
// notices stock's own writes over an overlaid value, runs every machine on
// tracks 0..7 in track order with every value without overlays, writes their
// overlays through 8d5acea8 (the later track's winning a parameter both
// overlay), and restores what stock last set wherever no machine overlays.
//
// Turns: 8d5ad564, the encoder edit stock's controllers reach through 8d5ad5bc,
// calls the setter 8d5acf28 with the new value unless a step is held for a
// lock. That call at 8d5ad5ae routes through sdk_param_edit_set, which notes
// the parameter turned for the display below, then makes the original call.
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

// One overlaid parameter: the value written, and stock's own value beneath it.
struct Applied { u8 track, index, wanted; i32 written, underlying; };
// Every machine's overlays together.
const u32 kAppliedCapacity = 2 * control::overlay_capacity;
Applied applied[kAppliedCapacity];
u32 applied_count;
// This tick's overlays from every machine, the later track's winning a parameter.
struct Wanted { u8 track, index; i32 value; };
Wanted wanted[kAppliedCapacity];
u32 wanted_count;
control::Input input;
control::Overlay overlay;
// The machine each track ran last tick, to reset a copy that arrives, and
// whether that copy's next tick is timed.
const ControlMachine* hosts[control::tracks];
bool timed[control::tracks];
// The core clock, 400 MHz, as cycles per microsecond, and the cycle count at
// the last tick, for each tick's elapsed time.
const u32 kCyclesPerMicrosecond = 400;
u32 last_cycles;
// GUI ticks so far, and the last encoder edit's parameter and tick. For
// kTurnTicks after it the edited parameter shows its base value.
const u32 kTurnTicks = 40;
u32 gui_ticks, turned_track = control::tracks, turned_index, turned_tick;

// What each track's machine passed to its display last tick.
i32 shown_words[control::tracks][control::display_words];

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

// Adds the overlay of the machine on `host`: entries in range and not on its
// own track; a later entry, or a later track's, replaces one for the same parameter.
void merge(u32 host) {
    for (u32 i = 0; i < overlay.count; ++i) {
        const control::Entry& e = overlay.entries[i];
        if (e.track >= control::tracks || e.index >= control::track_parameters || e.track == host) continue;
        Wanted* w = nullptr;
        for (u32 j = 0; j < wanted_count && !w; ++j)
            if (wanted[j].track == e.track && wanted[j].index == e.index) w = &wanted[j];
        if (!w) {
            if (wanted_count >= kAppliedCapacity) continue;
            w = &wanted[wanted_count++];
            w->track = e.track;
            w->index = e.index;
        }
        w->value = e.value < 0 ? 0 : e.value;
    }
}
}

extern "C" i32 sdk_control_display(u32 index) {
    const u32 track = selected_track();
    return track < control::tracks && index < control::display_words ? shown_words[track][index] : 0;
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
    param_set(project, track, index, value);
}

extern "C" void sdk_gui_tick() {
    stock<void()>(0x8d580074)();
    ++gui_ticks;
    if (!sdk_control_machine_count && !applied_count) return;

    // A value that differs from the one written is stock's: an edit, a lock or a step.
    for (u32 i = 0; i < applied_count; ++i) {
        const i32 value = live(applied[i].track, applied[i].index);
        if (value != applied[i].written) applied[i].underlying = value;
        applied[i].wanted = 0;
    }

    // Every instrument track's ready control machine runs, in track order:
    // each copy of a machine with its own state, except a unique machine that
    // more than one track holds, which none runs.
    const u32 now = opxy::benchmark::cycles(), us = (now - last_cycles) / kCyclesPerMicrosecond;
    last_cycles = now;
    wanted_count = 0;
    bool loaded = false;
    for (u32 t = 0; t < control::tracks; ++t) {
        const ControlMachine* m = owner(t);
        if (m && m->unique && sdk_control_copies(t) > 1) m = nullptr;
        const u32 copy = m && !m->unique ? t : 0;
        if (m != hosts[t]) {
            hosts[t] = m;
            timed[t] = false;
            if (m) m->reset(copy);
        }
        for (u32 i = 0; i < control::display_words; ++i) shown_words[t][i] = 0;
        if (!m) continue;
        if (!loaded) {
            for (u32 u = 0; u < control::tracks; ++u)
                for (u32 p = 0; p < control::track_parameters; ++p) input.values[u][p] = live(u, p);
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
        input.elapsed_us = timed[t] ? (us < 1000000 ? us : 1000000) : 0;
        timed[t] = true;
        overlay.count = 0;
        for (u32 i = 0; i < control::display_words; ++i) overlay.display[i] = 0;
        m->control(copy, &input, &overlay);
        for (u32 p = 0; p < kEngineControls; ++p) input.values[t][p] = own[p];
        if (overlay.count > control::overlay_capacity) overlay.count = control::overlay_capacity;
        for (u32 i = 0; i < control::display_words; ++i) shown_words[t][i] = overlay.display[i];
        merge(t);
    }

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
            a->underlying = live(w.track, w.index);
        }
        a->wanted = 1;
        a->written = w.value;
        if (live(w.track, w.index) != w.value) live_set(w.track, w.index, w.value);
    }
}
