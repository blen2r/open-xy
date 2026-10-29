// Track control machines on aux tracks (issue #220). A machine whose
// trackControl lists "aux" tracks can take over one of them, chosen with
// Shift + M1 on that track as an engine is on tracks 1-8; the list's first
// entry is the track's stock role. While a machine holds the track, its M1 page
// shows the machine's own page, its knobs edit the machine's controls
// (synth_controls.cpp), track control runs it there (track_control.cpp) and the
// stock role is kept silent. M2..M4 stay stock. Part of the control service.
//
// Stock OS 1.2.1, GUI side. The aux controller (method table 0803fb6c) serves
// tracks 8..15; the selected track is the byte at 840a0534 and its page tab
// (M1..M4 as 0..3) the word at 0801eae4.
// - Its shifted-key method (8c08f6b0) ignores Shift + M1..M4. Its Shift + M1
//   case (8c08f6ec) is hooked (aux_hooks.s): on a track a machine can take
//   over, it opens the chooser.
// - Its page method 8c08f53c answers the screen page for the selected track
//   and tab; the hook at 8c08f540 answers the machine's page on tab 0.
//
// Taking a track over keeps the stock role's eight controls aside and gives the
// machine its defaults, through stock's set (8d5acf28), which stores and
// publishes them; giving it back restores them. The choice and the role's
// controls are kept with the project (track_control_project.cpp); a new project
// starts with every track stock.
//
// Silencing, audio side. GUI and audio share no memory: a held track carries
// kMark in its last word, which stock never locks, set through stock's set
// (8d5acf28) as its stored value too, so stock's own resets of live values
// (as when the sequencer starts) keep it; stock publishes it, and the audio
// hooks read it from the audio side's copy of the track's words (84eab39c +
// track * 0xe8). The project saves the mark, so a track whose holder is gone
// after a load (a project from a build with the machine) gets its word back.
// - CV (track 12, index 11): the audio loop's CV output (8d50f1cc, from
//   8d512214) sets the voltage from word 0 and its mode from word 14, and a
//   note on raises the gate (8d5b5fe0, from 8d50e95c). Both are skipped while
//   held. When the track is taken, the audio side clears stock's held CV notes
//   (8d5b59a0) and lowers the gate (8d5b5f78(0)) once.
#include "track_control.hpp"
#include "stock.hpp"
#include "stock_browser.hpp"
#include "synth_controls.hpp"

extern "C" {
void sdk_aux_shift_m1() __asm__("sdk_aux_shift_m1");
opxy::i32 sdk_aux_page() __asm__("sdk_aux_page");
void sdk_cv_output(opxy::u32) __asm__("sdk_cv_output");
void sdk_cv_gate_on(opxy::u32, opxy::u32, opxy::u32) __asm__("sdk_cv_gate_on");
// For track_control_project.cpp: the machine ID holding each aux track, and
// the stock role's controls kept aside, to save; and their restoration on load.
const char* sdk_aux_holder_id(opxy::u32 track) __asm__("sdk_aux_holder_id");
const opxy::i32* sdk_aux_kept_controls(opxy::u32 track) __asm__("sdk_aux_kept_controls");
void sdk_aux_restore(opxy::u32 track, const char* id, opxy::u32 id_length, const opxy::i32* controls)
    __asm__("sdk_aux_restore");
void sdk_aux_forget() __asm__("sdk_aux_forget");
}

namespace {
using opxy::i32;
using opxy::u32;
using namespace opxy::platform;
namespace control = opxy::control;
namespace browser = opxy::platform::stock_browser;

const u32 kProject = 0x86bd027c;
const u32 kAuxTab = 0x0801eae4;
const u32 kFirstAux = 8;
const u32 kControls = 8;
// The aux tracks a machine can take over: those whose stock role has a silencer.
const u32 kCv = 11;
const u32 kTakeable = 1u << kCv;
const char* role_name(u32 track) { return track == kCv ? "cv" : nullptr; }

// The last live word, which carries the mark, and the mark: "OXAU".
const u32 kMarkWord = control::track_parameters - 1;
const i32 kMark = 0x4f584155;

// Each aux track's holder, as 1 + its index in sdk_control_machines, or 0 for
// the stock role; and the role's controls while a machine holds it.
u32 holders[control::all_tracks];
i32 kept[control::all_tracks][kControls];
// The last word's stored value before the track was marked, to give back.
i32 unmarked[control::all_tracks];

bool takeable(u32 track) { return track < control::all_tracks && (kTakeable >> track & 1); }
const ControlMachine* holder(u32 track) {
    if (!takeable(track) || !holders[track] || holders[track] > sdk_control_machine_count) return nullptr;
    const ControlMachine& m = sdk_control_machines[holders[track] - 1];
    return m.ready() == 1 ? &m : nullptr;
}

i32 base(u32 track, u32 index) {
    return static_cast<i32>(word(stock<u32(u32)>(0x8d587708)(track) + 0x3864 + index * 4));
}
void param_set(u32 track, u32 index, i32 value) {
    stock<void(u32, u32, u32, i32)>(0x8d5acf28)(kProject, track, index, value);
}
i32 live(u32 track, u32 index) {
    return *at<const volatile i32>(kProject + 0x4080 + (track * control::track_parameters + index) * 4);
}
void live_set(u32 track, u32 index, i32 value) {
    stock<void(u32, u32, u32, i32)>(0x8d5acea8)(kProject, track, index, value);
}

// Stores and publishes a held track's mark before anything else reaches the
// audio side, so the role stays silent from the first word the machine sets;
// and sets it live again over any stock reset that missed the stored value.
void mark(u32 track) {
    const i32 stored = base(track, kMarkWord);
    if (stored != kMark) {
        unmarked[track] = stored;
        param_set(track, kMarkWord, kMark);
    } else if (live(track, kMarkWord) != kMark) {
        live_set(track, kMarkWord, kMark);
    }
}
// A track nothing holds keeps no mark, stored or live.
void unmark(u32 track) {
    if (base(track, kMarkWord) == kMark) {
        param_set(track, kMarkWord, unmarked[track]);
        unmarked[track] = 0;
    } else if (live(track, kMarkWord) == kMark) {
        live_set(track, kMarkWord, base(track, kMarkWord));
    }
}

// Gives the track to machine `choice` (1 + its index) or back to its role (0);
// the next GUI tick clears the mark of a track given back.
void take(u32 track, u32 choice) {
    if (choice == holders[track]) return;
    if (!holders[track])
        for (u32 i = 0; i < kControls; ++i) kept[track][i] = base(track, i);
    holders[track] = choice;
    const ControlMachine* m = holder(track);
    if (m) mark(track);
    for (u32 i = 0; i < kControls; ++i) param_set(track, i, m ? m->defaults[i] : kept[track][i]);
}

// The chooser: the stock role, then each machine that may take the track over.
const u32 kChoices = 8;
browser::StockSelector<kChoices> chooser = {};
u32 chooser_track, chooser_machines[kChoices];
void choose(void*, u32 index) { take(chooser_track, chooser_machines[index]); }

bool open_chooser(u32 track) {
    const char* labels[kChoices] = {role_name(track)};
    u32 count = 1, initial = 0;
    chooser_machines[0] = 0;
    for (u32 i = 0; i < sdk_control_machine_count && count < kChoices; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        if (!(m.aux >> track & 1) || !m.page || m.ready() != 1) continue;
        if (holders[track] == i + 1) initial = count;
        chooser_machines[count] = i + 1;
        labels[count++] = m.name;
    }
    if (count == 1) return false;  // nothing to choose: M1 as stock
    chooser_track = track;
    return chooser.open("engine", labels, count, initial, browser::Callbacks{nullptr, choose, nullptr});
}

// The audio side's copy of a track's words, as stock's audio loop reads them.
const volatile i32* audio_words(u32 track) { return at<const volatile i32>(0x84eab39c + track * 0xe8); }
bool audio_held(u32 track) { return audio_words(track)[kMarkWord] == kMark; }
bool cv_silenced;  // audio side: the held CV notes were cleared and the gate lowered
}

u32 opxy::platform::control_engine(u32 track) {
    if (track < control::tracks) return track_engine(track);
    const ControlMachine* m = holder(track);
    return m ? m->engine() : 0;
}

// For synth_controls.cpp, a shared unit that links without track control too:
// it asks through sdk_aux_engine, which the first GUI tick points here.
static u32 aux_engine(u32 track) { return track >= kFirstAux ? control_engine(track) : 0; }

extern "C" void sdk_aux_tick() {
    sdk_aux_engine = aux_engine;
    for (u32 t = kFirstAux; t < control::all_tracks; ++t) {
        if (!takeable(t)) continue;
        if (holder(t)) mark(t);
        else unmark(t);
    }
}

// 8c08f6ec: Shift + M1 on an aux track, which stock ignores. A track a
// machine can take over opens the chooser.
void sdk_aux_shift_m1() {
    const u32 track = byte(kSelectedTrack);
    if (takeable(track)) open_chooser(track);
}

// 8c08f540: the selected aux track's page on tab 0 while a machine holds it, or
// -1 for stock's.
i32 sdk_aux_page() {
    const ControlMachine* m = word(kAuxTab) == 0 ? holder(byte(kSelectedTrack)) : nullptr;
    return m ? static_cast<i32>(m->page_id()) : -1;
}

// 8d512214, audio side: CV output, unless a machine holds the CV track.
void sdk_cv_output(u32 argument) {
    if (!audio_held(kCv)) {
        cv_silenced = false;
        stock<void(u32)>(0x8d50f1cc)(argument);
        return;
    }
    if (cv_silenced) return;
    stock<void()>(0x8d5b59a0)();
    stock<void(u32)>(0x8d5b5f78)(0);
    cv_silenced = true;
}

// 8d50e95c, audio side: a note on the CV track raises the gate, unless held.
void sdk_cv_gate_on(u32 a, u32 b, u32 c) {
    if (!audio_held(kCv)) stock<void(u32, u32, u32)>(0x8d5b5fe0)(a, b, c);
}

const char* sdk_aux_holder_id(u32 track) {
    const ControlMachine* m = holder(track);
    return m ? m->name : nullptr;
}
const i32* sdk_aux_kept_controls(u32 track) { return kept[track]; }

// After a project loads: the machine named `id` holds `track` again, with the
// role's controls as kept; a machine this build lacks leaves the role in place.
void sdk_aux_restore(u32 track, const char* id, u32 id_length, const i32* controls) {
    if (!takeable(track)) return;
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const ControlMachine& m = sdk_control_machines[i];
        u32 n = 0;
        while (n < id_length && m.name[n] == id[n]) ++n;
        if (n != id_length || m.name[n] || !(m.aux >> track & 1) || m.ready() != 1) continue;
        holders[track] = i + 1;
        for (u32 c = 0; c < kControls; ++c) kept[track][c] = controls[c];
        mark(track);
        return;
    }
}

// Before a project loads, and for a new one: every aux track has its stock role.
void sdk_aux_forget() {
    for (u32 t = 0; t < control::all_tracks; ++t) holders[t] = 0;
}
