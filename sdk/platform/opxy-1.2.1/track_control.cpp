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
#include "track_control.hpp"
#include "stock.hpp"

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

// One overlaid parameter: the value written, and stock's own value beneath it.
struct Applied { u8 track, index, wanted; i32 written, underlying; };
Applied applied[control::overlay_capacity];
u32 applied_count;
control::Input input;
control::Overlay overlay;
// The machine and track that ran last tick, to reset its state on a change.
const ControlMachine* host_machine;
u32 host_track;

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

extern "C" void sdk_gui_tick() {
    stock<void()>(0x8d580074)();
    if (!sdk_control_machine_count && !applied_count) return;

    // A value that differs from the one written is stock's: an edit, a lock or a step.
    for (u32 i = 0; i < applied_count; ++i) {
        const i32 value = live(applied[i].track, applied[i].index);
        if (value != applied[i].written) applied[i].underlying = value;
        applied[i].wanted = 0;
    }

    // The first instrument track that holds a ready control machine runs it.
    const ControlMachine* m = nullptr;
    u32 track = control::tracks;
    for (u32 t = 0; t < control::tracks && !m; ++t)
        if ((m = owner(t))) track = t;
    overlay.count = 0;
    if (m) {
        for (u32 t = 0; t < control::tracks; ++t)
            for (u32 p = 0; p < control::track_parameters; ++p) input.values[t][p] = live(t, p);
        for (u32 i = 0; i < applied_count; ++i)
            input.values[applied[i].track][applied[i].index] = applied[i].underlying;
        input.track = track;
        if (m != host_machine || track != host_track) m->reset();
        m->control(&input, &overlay);
        if (overlay.count > control::overlay_capacity) overlay.count = control::overlay_capacity;
    }
    host_machine = m;
    host_track = track;

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
