// Host boundary double for the real track control platform adapter: stock's
// live parameter table, its writer, the GUI tick it wraps and the displayed
// values. The machine is a scripted overlay; no machine behavior is tested.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <track_control.cpp>

namespace {
using opxy::i32;
using opxy::u32;
namespace control = opxy::control;

i32 table[control::tracks * control::track_parameters];
// Stock's base values, in each track's data block at +0x3864.
const u32 kBlocks = 0x90000000, kBlockBytes = 0x10000;
u32 bases[control::tracks][control::track_parameters];
u32 engines[16], ticks, writes, resets, runs, sets;
i32 seen;  // the value of track 0, parameter 1 the machine was given
u32 seen_elapsed, seen_bpm, seen_ticks, seen_playing;
// The core clock, the project tempo and the song position.
u32 cycles;
unsigned short tempo = 1200;
u32 ticks96;
unsigned char playing[2];
i32 seen_own;  // and of its own engine control 1
// Each track's LFO amount, which stock's displayed value adds.
i32 lfo[control::tracks][control::track_parameters];
// The scripted overlay for the next tick.
control::Entry script[8];
u32 script_count;

void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
i32& value(u32 track, u32 index) { return table[track * control::track_parameters + index]; }

void stock_tick() { ++ticks; }
u32 stock_engine(u32 track) { return track < 16 ? engines[track] : 0; }
u32 stock_track_data(u32 track) { return kBlocks + track * kBlockBytes; }
void stock_param_set(u32 project, u32 track, u32 index, i32 v) {
    require(project == kProject && track < 16 && index < control::track_parameters, "Invalid set");
    ++sets;
    if (track >= control::tracks) return;
    bases[track][index] = static_cast<u32>(v < 0 ? 0 : v);
    value(track, index) = v < 0 ? 0 : v;
}
void stock_live_set(u32 project, u32 track, u32 index, i32 v) {
    require(project == kProject && track < control::tracks && index < control::track_parameters && v >= 0,
            "Invalid live write");
    ++writes;
    value(track, index) = v;
}
// 8d5ac9dc: the live value plus the LFO amount, as a page shows it.
i32 stock_shown(u32 project, u32 track, u32 index, u32 flag) {
    require(project == kProject && track < control::tracks && index < control::track_parameters && flag == 1,
            "Invalid displayed value");
    return value(track, index) + lfo[track][index];
}
// 8d5ac910: the base value, which a stopped page shows.
i32 stock_base(u32 project, u32 track, u32 index) {
    require(project == kProject && track < control::tracks && index < control::track_parameters, "Invalid base");
    return static_cast<i32>(bases[track][index]);
}
// 8d4d6f14: the GUI's selected track.
u32 selected = 0;
u32 stock_selected() { return selected; }
int ready() { return 1; }
void reset(u32) { ++resets; }
void run(u32 copy, const control::Input* input, control::Overlay* overlay) {
    require(copy == 0, "A unique machine's copy is not 0");
    ++runs;
    require(overlay->count == 0, "Overlay not cleared");
    seen = input->values[0][1];
    seen_elapsed = input->elapsed_us;
    seen_bpm = input->transport.bpm_x10;
    seen_ticks = input->transport.ticks96;
    seen_playing = input->transport.playing;
    seen_own = input->values[input->track][1];
    require(overlay->display[0] == 0 && overlay->display[3] == 0, "Display words not cleared");
    overlay->display[1] = 77;
    for (u32 i = 0; i < script_count; ++i) overlay->entries[overlay->count++] = script[i];
}
// Machine 41, not unique: each copy overlays its scripted entries, with its
// copy's own count of runs.
u32 other_runs[control::tracks], other_resets[control::tracks];
control::Entry other_script[control::tracks][4];
u32 other_count[control::tracks];
void reset_other(u32 copy) { require(copy < control::tracks, "Copy out of range"); ++other_resets[copy]; }
void run_other(u32 copy, const control::Input* input, control::Overlay* overlay) {
    require(copy == input->track, "A copy is not its track");
    ++other_runs[copy];
    overlay->display[0] = static_cast<i32>(100 + copy);
    for (u32 i = 0; i < other_count[copy]; ++i) overlay->entries[overlay->count++] = other_script[copy][i];
}
void plan(std::initializer_list<control::Entry> entries) {
    script_count = 0;
    for (const control::Entry& e : entries) script[script_count++] = e;
}
void tick(u32 expected_writes, const char* message) {
    writes = 0;
    sdk_gui_tick();
    require(writes == expected_writes, message);
}
}

opxy::u32 sdk_benchmark_cycles() { return cycles; }
const volatile unsigned short* stock_project_tempo() { return &tempo; }

extern "C" {
const opxy::platform::ControlMachine sdk_control_machines[] = {
    {reinterpret_cast<const char*>(40), ready, reset, run, true},
    {reinterpret_cast<const char*>(41), ready, reset_other, run_other, false},
    {nullptr, nullptr, nullptr, nullptr, false}};
const opxy::u32 sdk_control_machine_count = 2;
}

opxy::platform::Address opxy::platform::host_stock_address(opxy::platform::Address address) {
    if (address >= kLive && address < kLive + sizeof(table))
        return reinterpret_cast<Address>(table) + (address - kLive);
    if (address == 0x8d580074) return reinterpret_cast<Address>(&stock_tick);
    if (address == 0x8d58781c) return reinterpret_cast<Address>(&stock_engine);
    if (address == 0x8d5acea8) return reinterpret_cast<Address>(&stock_live_set);
    if (address == 0x8d5acf28) return reinterpret_cast<Address>(&stock_param_set);
    if (address == 0x8d587708) return reinterpret_cast<Address>(&stock_track_data);
    if (address == 0x8d5ac9dc) return reinterpret_cast<Address>(&stock_shown);
    if (address == 0x8d5ac910) return reinterpret_cast<Address>(&stock_base);
    if (address == 0x8d4d6f14) return reinterpret_cast<Address>(&stock_selected);
    if (address == 0x8d584210) return reinterpret_cast<Address>(&stock_project_tempo);
    if (address == 0x86bd0080) return reinterpret_cast<Address>(&ticks96);
    if (address == 0x860c9b39) return reinterpret_cast<Address>(&playing[0]);
    if (address == 0x860c9b3a) return reinterpret_cast<Address>(&playing[1]);
    if (address >= kBlocks + 0x3864 && address < kBlocks + control::tracks * kBlockBytes) {
        const u32 track = (address - kBlocks) / kBlockBytes, at = address - kBlocks - track * kBlockBytes - 0x3864;
        if (track < control::tracks && at < sizeof(bases[0]))
            return reinterpret_cast<Address>(&bases[track][at / 4]);
    }
    std::abort();
}

int main() {
    value(0, 1) = 100;
    value(3, 9) = 7;
    plan({{0, 1, 0, 500}});
    tick(0, "Overlay written without a machine");
    require(ticks == 1 && runs == 0 && value(0, 1) == 100, "Stock tick not delegated alone");

    engines[2] = 40;
    // Own track and out-of-range entries are ignored; the later duplicate wins.
    plan({{0, 1, 0, 300}, {2, 0, 0, 9}, {9, 1, 0, 9}, {0, 58, 0, 9}, {0, 1, 0, 500}});
    tick(1, "Overlay not written once per parameter");
    require(runs == 1 && resets == 1 && seen == 100 && value(0, 1) == 500 && value(2, 0) == 0, "Overlay not applied");
    tick(0, "Unchanged overlay rewritten");
    require(seen == 100 && resets == 1, "Machine given the overlay instead of stock's value");

    // A stock edit under the overlay becomes the value beneath it.
    value(0, 1) = 120;
    tick(1, "Overlay not restored over a stock edit");
    require(seen == 120 && value(0, 1) == 500, "Stock edit not seen");

    // Dropping a parameter restores stock's value; negatives clamp to 0.
    plan({{3, 9, 0, -5}});
    tick(2, "Release and new overlay not written");
    require(value(0, 1) == 120 && value(3, 9) == 0, "Release not restored");

    // A stock write in the same tick as the release is kept.
    value(3, 9) = 11;
    plan({});
    tick(0, "Stock's own write overwritten on release");
    require(value(3, 9) == 11, "Stock write lost");

    // Moving the machine resets it; removing it restores everything.
    plan({{0, 1, 0, 600}});
    tick(1, "Overlay not written");
    engines[2] = 0;
    engines[5] = 40;
    tick(0, "Same overlay rewritten after a move");
    require(resets == 2, "Moved machine not reset");
    engines[5] = 0;
    tick(1, "Overlay not restored when the machine left");
    require(value(0, 1) == 120 && ticks == 9, "Removal not restored");

    // Edits pass through untouched.
    bases[0][5] = 50;
    sdk_param_edit_set(kProject, 0, 5, 70);
    require(sets == 1 && bases[0][5] == 70, "Edit not delegated");
    bases[0][5] = 50;

    // The machine back on track 2, with nothing to overlay.
    engines[2] = 40;
    plan({});
    tick(0, "Machine without an overlay wrote one");

    // The machine's own engine controls arrive as its page shows them.
    value(2, 1) = 50;
    lfo[2][1] = 1000;
    tick(0, "Own LFO wrote an overlay");
    require(seen_own == 1050, "Own LFO not included");

    // Pages show an overlay playing or stopped; other parameters as stock does.
    plan({{0, 1, 0, 500}});
    tick(1, "Overlay not written");
    bases[0][2] = 30;
    value(0, 2) = 40;
    require(sdk_param_shown(kProject, 0, 1, 0) == 500 && sdk_param_shown(kProject, 0, 1, 1) == 500,
            "Overlay not shown");
    require(sdk_param_shown(kProject, 0, 2, 0) == 30 && sdk_param_shown(kProject, 0, 2, 1) == 40,
            "Stock value not shown");
    // Turning an overlaid parameter shows its base value for a while.
    sdk_param_edit_set(kProject, 0, 1, 80);
    for (u32 i = 0; i < kTurnTicks; ++i) {
        require(sdk_param_shown(kProject, 0, 1, 1) == 80, "Turned base not shown");
        tick(i == 0 ? 1 : 0, "Overlay not restored once over the turn");
    }
    require(sdk_param_shown(kProject, 0, 1, 1) == 500, "Overlay not shown after the turn");

    plan({});
    tick(1, "Release not written");

    // Two copies of the machine: neither runs, and its overlay ends as if it
    // had left, until one copy is removed.
    const i32 own = value(0, 1);
    plan({{0, 1, 0, 700}});
    tick(1, "Overlay not written");
    engines[6] = 40;
    require(sdk_control_copies(2) == 2 && sdk_control_copies(6) == 2 && sdk_control_copies(16) == 0,
            "Copies not counted");
    const u32 ran = runs;
    tick(1, "Overlay not released with two copies");
    require(runs == ran && value(0, 1) == own, "Two copies ran");
    engines[6] = 0;
    tick(1, "Overlay not written once one copy is left");
    require(runs == ran + 1 && value(0, 1) == 700 && sdk_control_copies(2) == 1, "Single copy not run again");

    // Each tick carries the time since the last, from the 400 MHz core clock
    // (0 after a reset, at most a second), and the tempo and song position.
    cycles += 400 * 2500;
    ticks96 = 480;
    playing[1] = 1;
    tick(0, "Timing rewrote the overlay");
    require(seen_elapsed == 2500 && seen_bpm == 1200 && seen_ticks == 480 && seen_playing == 1, "Time not given");
    cycles += 400u * 5000000u;
    playing[1] = 0;
    tick(0, "Timing rewrote the overlay");
    require(seen_elapsed == 1000000 && seen_playing == 0, "Elapsed time not capped");

    // What the control passes to its display, for the selected track.
    selected = 2;
    require(sdk_control_display(1) == 77 && sdk_control_display(0) == 0 && sdk_control_display(9) == 0,
            "Display words not kept");

    // Several machines: machine 40 on track 2 and two copies of machine 41,
    // which is not unique, on tracks 4 and 6. Every one runs, each copy with
    // its own state, and each display reads its own track's words.
    plan({{0, 1, 0, 500}});
    tick(1, "Overlay not written");
    const i32 before = value(0, 2);
    engines[4] = 41;
    engines[6] = 41;
    other_count[4] = 1;
    other_script[4][0] = control::Entry{0, 2, 0, 222};
    tick(1, "Second machine's overlay not written");
    require(other_runs[4] == 1 && other_runs[6] == 1 && other_resets[4] == 1 && other_resets[6] == 1 &&
            value(0, 1) == 500 && value(0, 2) == 222, "Machines not run together");
    selected = 6;
    require(sdk_control_display(0) == 106, "Display words not the selected track's");
    selected = 2;
    require(sdk_control_display(1) == 77 && sdk_control_display(0) == 0, "First machine's words lost");
    // Two machines on one parameter: the later track's wins.
    other_count[6] = 1;
    other_script[6][0] = control::Entry{0, 1, 0, 666};
    tick(1, "Contested parameter not written once");
    require(value(0, 1) == 666 && sdk_param_shown(kProject, 0, 1, 1) == 666, "Later track did not win");
    // The later machine gives the parameter up: the earlier one's comes back.
    other_count[6] = 0;
    tick(1, "Earlier overlay not written back");
    require(value(0, 1) == 500, "Earlier machine's overlay not restored");
    // A copy leaving ends its overlay; the others carry on.
    engines[4] = 0;
    tick(1, "Departed copy's overlay not released");
    require(value(0, 2) == before && value(0, 1) == 500, "Departed copy not released alone");
    engines[6] = 0;
    return 0;
}
