// The keys service: stock's queued key dispatch (8c0b5d58), called from the
// key queue at 8c0b6184, shared by extensions and by chord claims. Each queued
// key passes the gesture layer first. A chord claim's held key waits until it
// is known not to start a chord: released alone, or followed by a key that
// isn't a chord key, its press reaches stock after all; chord keys pressed
// meanwhile go to the claim's owner, with their releases, and never to stock.
// Everything else continues to the extensions' wrapper, or stock.
#include "../keys.hpp"
#include <opxy/keys.hpp>
#include "stock.hpp"

namespace opxy { namespace platform {
namespace {
const u32 kQueue = 0x08023c78;
// The registry's chord claims (profiles/extensions.json, "chord": "keyboard"),
// by their place among its claims, and their held keys; a test keeps the two
// in step. Their chord keys are the keyboard's.
struct ChordClaim { u32 claim, lead; };
const ChordClaim kChordClaims[] = {{1, keys::metronome}};
const u32 kChordCount = sizeof(kChordClaims) / sizeof(kChordClaims[0]);

// A claim's owner: an extension (its place plus 1) before a machine's track (plus 1).
struct Owner {
    u32 extension, track;
    bool any() const { return extension || track; }
};
Owner owner_of(u32 claim) {
    if (const u32 extension = sdk_extensions_claim_owner(claim)) return {extension, 0};
    return {0, sdk_control_claimant(claim)};
}
bool takes(const Owner& o) {
    if (o.extension) return sdk_extensions_takes(o.extension) != 0;
    return o.track && sdk_control_takes(o.track - 1);
}
void deliver(const Owner& o, u32 chord, u32 key, bool pressed) {
    KeyGesture g = {kChordClaims[chord].claim, kChordClaims[chord].lead, key, pressed ? 1u : 0u, 0};
    // The clock writes a full 64-bit microsecond value through its argument.
    stock<void(__UINT64_TYPE__*)>(0x8d5be3a0)(&g.time);
    if (o.extension) sdk_extensions_gesture(o.extension, &g);
    else if (o.track) sdk_control_queue_gesture(o.track - 1, &g);
}

// Each chord claim's held key: down for its owner (taken, or its press
// waiting), whether its press waits for stock, whether a chord key came since.
struct Lead {
    bool down, waiting, chorded;
    Owner owner;
    u32 event[4];
};
Lead leads[kChordCount];
// The keys whose press went to an owner: their release goes there too.
const u32 kKeys = 64;
struct Kept { bool kept; u32 chord; Owner owner; };
Kept kept[kKeys];

void keep(u32 key, u32 chord, const Owner& o) {
    if (key < kKeys) kept[key] = {true, chord, o};
}

void dispatch(const u32* event) {
    const u32 key = event[0];
    const bool pressed = event[1] != 0;
    if (key < kKeys && !pressed && kept[key].kept) {
        const Kept k = kept[key];
        kept[key].kept = false;
        if (key == kChordClaims[k.chord].lead) leads[k.chord].down = false;
        deliver(k.owner, k.chord, key, false);
        return;
    }
    for (u32 c = 0; c < kChordCount; ++c) {
        Lead& l = leads[c];
        const u32 lead = kChordClaims[c].lead;
        if (key == lead) {
            if (!pressed) {
                if (!l.waiting) break;  // stock has its press, so its release too
                l.waiting = l.down = false;
                if (!l.chorded) {
                    sdk_extensions_key(l.event);
                    sdk_extensions_key(event);
                }
                return;
            }
            if (l.waiting) return;  // a repeat while its press waits
            const Owner o = owner_of(kChordClaims[c].claim);
            // Shift + the held key, and keys stock ignores, stay stock's.
            if (!o.any() || !byte(kInputEnabled) || shift_held()) break;
            l = Lead{};
            l.down = true;
            l.owner = o;
            if (takes(o)) {
                keep(key, c, o);
                deliver(o, c, key, true);
                return;
            }
            l.waiting = true;
            for (u32 i = 0; i < 4; ++i) l.event[i] = event[i];
            return;
        }
        if (!l.down || !pressed) continue;
        if (keys::keyboard(key)) {
            l.chorded = true;
            keep(key, c, l.owner);
            deliver(l.owner, c, key, true);
            return;
        }
        // Another key: the held key is stock's after all, its press first,
        // unless a chord already used it.
        if (l.waiting && !l.chorded) {
            l.waiting = l.down = false;
            sdk_extensions_key(l.event);
        }
    }
    sdk_extensions_key(event);
}
}
}}

using namespace opxy;

u32 sdk_key_queue(const u32* event) {
    platform::dispatch(event);
    // The authenticated span includes stock's following R0 = queue address.
    // A 12-byte span admits CALL.A across the module's relative-call range.
    return platform::kQueue;
}
void sdk_key_stock(const u32* event) { platform::stock<void(const u32*)>(0x8c0b5d58)(event); }
