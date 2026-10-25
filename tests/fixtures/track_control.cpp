// Host boundary double for the real track control platform adapter: stock's
// live parameter table, its writer and the GUI tick it wraps. The machine is
// a scripted overlay; no machine behavior is tested.
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
u32 engines[16], ticks, writes, resets, runs, sets, edits_seen;
control::Entry last_edit;
u32 capture;
i32 seen;  // the value of track 0, parameter 1 the machine was given
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
int ready() { return 1; }
void reset() { ++resets; }
void run(const control::Input* input, control::Overlay* overlay) {
    ++runs;
    require(overlay->count == 0, "Overlay not cleared");
    seen = input->values[0][1];
    edits_seen = input->edit_count;
    if (input->edit_count) last_edit = input->edits[input->edit_count - 1];
    require(overlay->capture == 0, "Capture not cleared");
    overlay->capture = capture;
    for (u32 i = 0; i < script_count; ++i) overlay->entries[overlay->count++] = script[i];
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

extern "C" {
const opxy::platform::ControlMachine sdk_control_machines[] = {
    {reinterpret_cast<const char*>(40), ready, reset, run}, {nullptr, nullptr, nullptr, nullptr}};
const opxy::u32 sdk_control_machine_count = 1;
}

opxy::platform::Address opxy::platform::host_stock_address(opxy::platform::Address address) {
    if (address >= kLive && address < kLive + sizeof(table))
        return reinterpret_cast<Address>(table) + (address - kLive);
    if (address == 0x8d580074) return reinterpret_cast<Address>(&stock_tick);
    if (address == 0x8d58781c) return reinterpret_cast<Address>(&stock_engine);
    if (address == 0x8d5acea8) return reinterpret_cast<Address>(&stock_live_set);
    if (address == 0x8d5acf28) return reinterpret_cast<Address>(&stock_param_set);
    if (address == 0x8d587708) return reinterpret_cast<Address>(&stock_track_data);
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

    // Edits pass through untouched while nothing captures.
    bases[0][5] = 50;
    sdk_param_edit_set(kProject, 0, 5, 70);
    require(sets == 1 && bases[0][5] == 70, "Edit not delegated");
    bases[0][5] = 50;

    // Capture: edits of other tracks still reach stock and arrive next tick;
    // the host's own and other tracks' edits are not captured.
    engines[2] = 40;
    capture = 1;
    plan({});
    tick(0, "Capture wrote an overlay");
    sdk_param_edit_set(kProject, 0, 5, 900);
    sdk_param_edit_set(kProject, 2, 0, 5);
    sdk_param_edit_set(kProject, 9, 0, 5);
    require(sets == 4 && bases[0][5] == 900, "Captured edit not delegated");
    tick(0, "Capture wrote an overlay");
    require(edits_seen == 1 && last_edit.track == 0 && last_edit.index == 5 && last_edit.value == 900,
            "Edit not captured");
    sdk_param_edit_set(kProject, 0, 5, -3);
    tick(0, "Capture wrote an overlay");
    require(edits_seen == 1 && last_edit.value == 0, "Second edit not captured");
    tick(0, "Capture wrote an overlay");
    require(edits_seen == 0, "Edits delivered twice");
    // Ending the capture sets the base from before the first edit back.
    capture = 0;
    sets = 0;
    tick(0, "End of capture wrote an overlay");
    require(sets == 1 && bases[0][5] == 50 && value(0, 5) == 50, "Capture not restored");
    sdk_param_edit_set(kProject, 0, 5, 60);
    tick(0, "Uncaptured edit wrote an overlay");
    require(edits_seen == 0 && bases[0][5] == 60, "Edit captured after the capture ended");
    return 0;
}
