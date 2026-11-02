// The keys service (opxy-1.2.1/keys.cpp): the key queue's one site, shared by
// extensions and by chord claims, and what its gesture layer calls.
#pragma once
#include <opxy/types.hpp>

namespace opxy { namespace platform {
// One key a chord claim keeps from stock, for the claim's owner: `claim` is the
// claim's place in profiles/extensions.json.
struct KeyGesture { u32 claim, lead, key, pressed; __UINT64_TYPE__ time; };
}}

// Builds without extensions or without track control get stand-ins from
// startup; a browser kit's selection gets the service fallbacks (browser_kit.py).
extern "C" {
// The key queue's call (8c0b6184): the gesture layer, then the extensions'
// wrapper. Returns the queue's address, which the replaced span loaded.
opxy::u32 sdk_key_queue(const opxy::u32* event) __asm__("sdk_key_queue");
// Stock's queued key dispatch, where the extensions' wrapper is not linked.
void sdk_key_stock(const opxy::u32* event) __asm__("sdk_key_stock");
// The extensions' wrapper (extension_stock.cpp): stock's dispatch, then queued_control.
void sdk_extensions_key(const opxy::u32* event) __asm__("sdk_extensions_key");
// The extension holding a claim, as its place in the collection plus 1, or 0;
// a gesture for it; and whether it takes its chord claims' held key.
opxy::u32 sdk_extensions_claim_owner(opxy::u32 claim) __asm__("sdk_extensions_claim_owner");
void sdk_extensions_gesture(opxy::u32 owner, const opxy::platform::KeyGesture* gesture)
    __asm__("sdk_extensions_gesture");
opxy::u32 sdk_extensions_takes(opxy::u32 owner) __asm__("sdk_extensions_takes");
// The track (plus 1) whose running machine holds a claim, or 0; a gesture for
// its next tick; and whether it takes its chord claims' held key.
opxy::u32 sdk_control_claimant(opxy::u32 claim) __asm__("sdk_control_claimant");
void sdk_control_queue_gesture(opxy::u32 track, const opxy::platform::KeyGesture* gesture)
    __asm__("sdk_control_queue_gesture");
opxy::u32 sdk_control_takes(opxy::u32 track) __asm__("sdk_control_takes");
// The track (plus 1) of a running, unmuted machine that takes the keyboard
// while its track is selected (resources.trackControl.keyboard), or 0.
opxy::u32 sdk_control_keyboard_track() __asm__("sdk_control_keyboard_track");
}
