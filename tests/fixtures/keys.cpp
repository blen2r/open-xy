// The keys service's gesture layer with stock doubles: the metronome chord
// claim's held key waits, chord keys go to the owner, everything else to stock.
#include <keys.cpp>
#define CHECK(value) do { if (!(value)) return __LINE__; } while (false)
using namespace opxy;
using namespace opxy::platform;
namespace {
u32 input_enabled = 1, shift = 0;
__UINT64_TYPE__ clock_now = 1000;
// What reached stock, in order: key * 2 + pressed.
u32 stocked[32], stock_count;
// The owners, played by the test: an extension (place + 1) or a machine's track (+ 1).
u32 extension_owner, machine_track, extension_takes, machine_takes;
KeyGesture heard[16];
u32 heard_count, heard_by_extension, heard_track;
u32 shift_down() { return shift; }
void get_clock(__UINT64_TYPE__* t) { *t = clock_now; }
void dispatch_stock(const u32* event) { if (stock_count < 32) stocked[stock_count++] = event[0] * 2 + (event[1] != 0); }
}
extern "C" {
void sdk_extensions_key(const u32* event) { sdk_key_stock(event); }
u32 sdk_extensions_claim_owner(u32 claim) { return claim == 1 ? extension_owner : 0; }
void sdk_extensions_gesture(u32 owner, const KeyGesture* g) {
    if (heard_count < 16) heard[heard_count++] = *g;
    heard_by_extension = owner;
}
u32 sdk_extensions_takes(u32 owner) { return owner == extension_owner && extension_takes; }
u32 sdk_control_claimant(u32 claim) { return claim == 1 ? machine_track : 0; }
void sdk_control_queue_gesture(u32 track, const KeyGesture* g) {
    if (heard_count < 16) heard[heard_count++] = *g;
    heard_track = track + 1;
}
u32 sdk_control_takes(u32 track) { return track + 1 == machine_track && machine_takes; }
u32 keyboard_track;
u32 sdk_control_keyboard_track() { return keyboard_track; }
}
namespace opxy { namespace platform {
Address host_stock_address(Address address) {
    switch (address) {
        case 0x08023c48: return reinterpret_cast<Address>(&input_enabled);
        case 0x8d51f184: return reinterpret_cast<Address>(&shift_down);
        case 0x8d5be3a0: return reinterpret_cast<Address>(&get_clock);
        case 0x8c0b5d58: return reinterpret_cast<Address>(&dispatch_stock);
    }
    __builtin_trap();
}
}}
namespace {
bool key(u32 k, bool pressed) {
    const u32 event[4] = {k, pressed ? 100u : 0u, 1, 2};
    return sdk_key_queue(event) == 0x08023c78;
}
void reset() { stock_count = heard_count = heard_by_extension = heard_track = 0; }
bool stocked_is(const u32* expected, u32 count) {
    if (stock_count != count) return false;
    for (u32 i = 0; i < count; ++i) if (stocked[i] != expected[i]) return false;
    return true;
}
}
int main() {
    // No owner: every key reaches stock at once.
    CHECK(key(1, true) && key(30, true) && key(30, false) && key(1, false));
    const u32 plain[] = {3, 61, 60, 2};
    CHECK(stocked_is(plain, 4) && heard_count == 0);
    // An extension holds the claim: the metronome waits; released alone, its press then its release reach stock.
    reset(); extension_owner = 2;
    key(1, true); CHECK(stock_count == 0);
    key(1, true); CHECK(stock_count == 0);  // a repeat while it waits
    key(1, false);
    const u32 tap[] = {3, 2};
    CHECK(stocked_is(tap, 2) && heard_count == 0);
    // A chord: the keyboard key and its release go to the owner; nothing reaches stock.
    reset(); clock_now = 5000;
    key(1, true); key(30, true); key(31, true); key(30, false); key(1, false); key(31, false);
    CHECK(stock_count == 0 && heard_count == 4 && heard_by_extension == 2);
    CHECK(heard[0].claim == 1 && heard[0].lead == 1 && heard[0].key == 30 && heard[0].pressed && heard[0].time == 5000);
    CHECK(heard[1].key == 31 && heard[1].pressed && heard[2].key == 30 && !heard[2].pressed);
    CHECK(heard[3].key == 31 && !heard[3].pressed);  // released after the metronome: still the owner's
    // Another key while it waits: the metronome is stock's after all, its press first.
    reset();
    key(1, true); key(14, true); key(14, false); key(30, true); key(30, false); key(1, false);
    const u32 other[] = {3, 29, 28, 61, 60, 2};
    CHECK(stocked_is(other, 6) && heard_count == 0);
    // After a chord, another key passes and the metronome stays the owner's.
    reset();
    key(1, true); key(30, true); key(14, true); key(14, false); key(30, false); key(1, false);
    const u32 after[] = {29, 28};
    CHECK(stocked_is(after, 2) && heard_count == 2);
    // Shift + metronome, and keys stock ignores, stay stock's.
    reset(); shift = 1;
    key(1, true); key(30, true); key(30, false); key(1, false); shift = 0;
    CHECK(stocked_is(plain, 4) && heard_count == 0);
    reset(); input_enabled = 0;
    key(1, true); key(1, false); input_enabled = 1;
    CHECK(stocked_is(tap, 2));
    // Taken: the metronome's own press and release are the owner's, as gestures.
    reset(); extension_takes = 1;
    key(1, true); key(30, true); key(30, false); key(1, false);
    CHECK(stock_count == 0 && heard_count == 4 && heard[0].key == 1 && heard[0].pressed &&
          heard[1].key == 30 && heard[3].key == 1 && !heard[3].pressed);
    reset(); key(1, true); key(1, false);
    CHECK(stock_count == 0 && heard_count == 2);
    extension_takes = 0;
    // A machine holds it while no extension does: its gestures queue for its track.
    reset(); extension_owner = 0; machine_track = 13;
    key(1, true); key(40, true); key(40, false); key(1, false);
    CHECK(stock_count == 0 && heard_count == 2 && heard_track == 13 && heard_by_extension == 0);
    reset(); machine_takes = 1;
    key(1, true); key(1, false);
    CHECK(stock_count == 0 && heard_count == 2 && heard[0].key == 1);
    // The extension goes first when both would hold it.
    reset(); extension_owner = 1; machine_takes = 0;
    key(1, true); key(40, true); key(40, false); key(1, false);
    CHECK(heard_by_extension == 1 && heard_track == 0);
    // A machine taking the keyboard on its selected track gets keyboard keys, lead 0;
    // other keys, and a chord, are as before.
    reset(); extension_owner = 0; machine_track = 0; keyboard_track = 12;
    key(30, true); key(14, true); key(14, false); key(30, false);
    const u32 others[] = {29, 28};
    CHECK(stocked_is(others, 2) && heard_count == 2 && heard_track == 12 && heard[0].lead == 0 &&
          heard[0].key == 30 && heard[0].claim == ~0u && !heard[1].pressed);
    reset(); input_enabled = 0;
    key(30, true); key(30, false); input_enabled = 1;
    CHECK(stock_count == 2 && heard_count == 0);
    reset(); machine_track = 12;
    key(1, true); key(30, true); key(30, false); key(1, false);
    CHECK(stock_count == 0 && heard_count == 2 && heard[0].lead == 1);
    keyboard_track = 0;
    return 0;
}
