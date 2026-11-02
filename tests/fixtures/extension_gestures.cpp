// Chord claims in the runtime: who holds one, gestures with the claim's actions,
// and taking the held key, renewed in every gui_tick.
#include <extension_runtime.cpp>
#define CHECK(value) do { if (!(value)) return __LINE__; } while (false)
using namespace opxy;
using namespace opxy::platform;
namespace {
RecordingContext stock_context = {1, 2, 0, 7680, 3, 255, true, true};
u32 notices, requests, gestures, last_key, last_lead, controls, granted_notify;
bool last_pressed, take_in_gesture, renew;
void on_gesture(const extensions::Context&, const extensions::Gesture& g, const extensions::Actions& actions) {
    ++gestures; last_key = g.key; last_lead = g.lead; last_pressed = g.pressed;
    actions.notify("chord");
    actions.capture(2);
    if (take_in_gesture) actions.take(1);
    // Reentrant gestures are ignored.
    const KeyGesture inner = {1, 1, 30, 1, 0};
    sdk_extensions_gesture(2, &inner);
}
void on_tick(const extensions::Context&, const extensions::Actions& actions) { if (renew) actions.take(1); }
void on_control(const extensions::Context&, const extensions::Control&, const extensions::Actions& actions) {
    ++controls;
    const u32 before = notices;
    actions.notify("control");
    granted_notify += notices != before;
    actions.take(1);  // only gesture and gui_tick callbacks take
}
const extensions::Hooks plain = extensions::Hooks();
const extensions::Hooks chorder = extensions::Hooks().with_gesture(on_gesture).with_gui_tick(on_tick)
                                                     .with_queued_control(on_control);
// Claims: Record double-tap (key 50), then a chord claim on the metronome (key 1).
u32 claim_keys[] = {50, 1}, action_claims[] = {0, 0, 0};
const ExtensionRegistry tables = {claim_keys, 2, 2u, action_claims, 3};
ExtensionEntry collection[] = {{"plain", 0, 0, &plain}, {"chorder", 2u, action_notify | action_capture, &chorder}};
}
extern "C" u32 sdk_capture_request(u32, u32) { ++requests; return 1; }
extern "C" u32 sdk_capture_holds(u32) { return 0; }
extern "C" u32 sdk_capture_edits(u32, extensions::Edit*, u32) { return 0; }
namespace opxy { namespace platform {
const ExtensionEntry* extension_collection(u32& count) { count = 2; return collection; }
const ExtensionRegistry& extension_registry() { return tables; }
TrackPattern extension_pattern(u32) { return {2, 7680, 3, true}; }
RecordingContext extension_context(bool& p) { p = false; return stock_context; }
i32 extension_position(u32) { return 0; }
u32 extension_scaled_delta(u32, u32 delta) { return delta; }
i32 extension_collision_radius(u32) { return 0; }
u32 extension_write(u32, const RecordedNote*, u32, bool) { return 0; }
void extension_notify(const char*) { ++notices; }
}}
int main() {
    CHECK(sdk_extensions_claim_owner(1) == 2 && sdk_extensions_claim_owner(0) == 0 && sdk_extensions_claim_owner(9) == 0);
    // A gesture reaches its holder with the claim's actions, once.
    const KeyGesture g = {1, 1, 30, 1, 77};
    sdk_extensions_gesture(2, &g);
    CHECK(gestures == 1 && last_key == 30 && last_lead == 1 && last_pressed && notices == 1 && requests == 1);
    // Not for another extension, an unknown owner or a claim it doesn't hold.
    sdk_extensions_gesture(1, &g); sdk_extensions_gesture(3, &g);
    const KeyGesture other = {0, 50, 50, 1, 0};
    sdk_extensions_gesture(2, &other);
    CHECK(gestures == 1);
    // Taking lasts until the next gui_tick unless renewed there.
    CHECK(!sdk_extensions_takes(2));
    take_in_gesture = true; sdk_extensions_gesture(2, &g); CHECK(sdk_extensions_takes(2) == 1);
    extensions_gui_tick(); CHECK(!sdk_extensions_takes(2));
    renew = true; extensions_gui_tick(); CHECK(sdk_extensions_takes(2) == 1);
    renew = false; extensions_gui_tick(); CHECK(!sdk_extensions_takes(2));
    // The held key's queued control grants nothing: the chord claim fires as gestures.
    const extensions::Control metronome = {1, true, true, true, 0};
    extensions_queued_control(metronome);
    CHECK(controls == 1 && granted_notify == 0 && !sdk_extensions_takes(2));
    // A chord claim's holder without a gesture callback disables the collection.
    initialized = false; entries = nullptr; collection[1].hooks = &plain;
    CHECK(sdk_extensions_claim_owner(1) == 0);
    return 0;
}
