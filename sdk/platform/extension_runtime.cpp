#include "extension_runtime.hpp"

namespace opxy { namespace platform {
namespace {
extensions::Context current;
const ExtensionEntry* entries;
const ExtensionRegistry* registry;
u32 entry_count;
bool initialized, dispatching;
char notification[32];

// The actions one callback may take, and what it has done with them: one stock
// undo transaction and at most one notification per callback.
struct Transaction { u32 granted; bool attempted, notified; };
Transaction transaction;
// The extension whose callback runs, as its place in the collection plus 1 (0: none).
u32 running;
// The extensions that take their chord claims' held key (bit: place in the
// collection), until the next GUI tick; and whether the running callback may.
u32 taking;
bool may_take;

bool same_text(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
u32 bits(u32 count) { return count >= 32 ? ~0u : (1u << count) - 1; }

// The collection must agree with the registry, whatever the build checked:
// claims exclusive and known, every action backed by a claim its holder owns.
bool ready() {
    if (initialized) return entries != nullptr;
    initialized = true;
    u32 count = 0;
    const ExtensionEntry* list = extension_collection(count);
    const ExtensionRegistry& known = extension_registry();
    if (!list || count > 16 || known.claim_count > 32 || known.action_count > 32) return false;
    u32 claimed = 0;
    for (u32 i = 0; i < count; ++i) {
        const ExtensionEntry& entry = list[i];
        if (!entry.id || !*entry.id || !entry.hooks || (entry.claims & claimed) ||
            (entry.claims & ~bits(known.claim_count)) || (entry.actions & ~bits(known.action_count)) ||
            // A chord claim's keys arrive as gestures; any other claim's as queued controls.
            ((entry.claims & ~known.chord_claims) && !entry.hooks->queued_control) ||
            ((entry.claims & known.chord_claims) && !entry.hooks->gesture)) return false;
        for (u32 a = 0; a < known.action_count; ++a) {
            if (!(entry.actions & (1u << a))) continue;
            const u32 needs = known.action_claims[a] ? known.action_claims[a] : bits(known.claim_count);
            if (!(entry.claims & needs)) return false;
        }
        claimed |= entry.claims;
        for (u32 j = 0; j < i; ++j)
            if (same_text(entry.id, list[j].id)) return false;
    }
    entries = list;
    registry = &known;
    entry_count = count;
    return true;
}

// Actions the fired claims grant their holder.
u32 grants(const ExtensionEntry& entry, u32 fired) {
    u32 result = 0;
    for (u32 a = 0; fired && a < registry->action_count; ++a) {
        const u32 needs = registry->action_claims[a] ? registry->action_claims[a] : entry.claims;
        if ((entry.actions & (1u << a)) && (fired & needs)) result |= 1u << a;
    }
    return result;
}
// Actions granted while a claimed key fires and stock accepted it. A chord
// claim's keys fire as gestures instead.
u32 granted(const ExtensionEntry& entry, const extensions::Control& control) {
    if (!control.accepted) return 0;
    u32 fired = 0;
    for (u32 c = 0; c < registry->claim_count; ++c)
        if ((entry.claims & ~registry->chord_claims & (1u << c)) && registry->claim_keys[c] == control.key)
            fired |= 1u << c;
    return grants(entry, fired);
}

// The caller's context must still be current: a stale one means the project,
// pattern or selection moved on since the event it acts on.
u32 write_notes(const extensions::Context& expected, u32 track, const RecordedNote* notes, u32 count,
                bool replace) {
    if (!(transaction.granted & action_edit_notes) || transaction.attempted || !notes || !count ||
        count > 120 || track >= 8) return 0;
    bool playing = false;
    const RecordingContext now = extension_context(playing);
    if (!(now == expected.recording) || playing != expected.playing ||
        !(now == current.recording) || playing != current.playing) return 0;
    const TrackPattern target = extension_pattern(track);
    if (!target.editable || !target.length || target.length >= (1u << 23)) return 0;
    for (u32 i = 0; i < count; ++i)
        if (notes[i].pitch > 127 || !notes[i].velocity || notes[i].velocity > 127 ||
            notes[i].position < -240 || notes[i].position >= static_cast<i32>(target.length) ||
            notes[i].length > target.length) return 0;
    transaction.attempted = true; // One undo transaction per callback, including failures.
    return extension_write(track, notes, count, replace);
}
void notify(const char* text) {
    if (!(transaction.granted & action_notify) || transaction.notified || !text) return;
    u32 length = 0;
    while (length < 32 && text[length]) ++length;
    if (!length || length == 32) return;
    for (u32 i = 0; i <= length; ++i) notification[i] = text[i];
    transaction.notified = true;
    extension_notify(notification);
}
u32 insert_notes(const extensions::Context& expected, u32 track, const RecordedNote* notes, u32 count) {
    return write_notes(expected, track, notes, count, false);
}
u32 replace_notes(const extensions::Context& expected, u32 track, const RecordedNote* notes, u32 count) {
    return write_notes(expected, track, notes, count, true);
}
u32 capture(u32 mode) {
    if (!running || !(transaction.granted & action_capture) || mode > 2) return 0;
    sdk_capture_request(running, mode);
    return sdk_capture_holds(running);
}
void take(u32 on) {
    if (!running || !may_take || !(entries[running - 1].claims & registry->chord_claims)) return;
    const u32 bit = 1u << (running - 1);
    taking = on ? taking | bit : taking & ~bit;
}
const extensions::Actions actions = {insert_notes, notify, replace_notes, capture, take};
}

void extensions_sync() {
    if (dispatching || !ready()) return;
    current.recording = extension_context(current.playing);
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i)
        if (entries[i].hooks->context) entries[i].hooks->context(current);
    dispatching = false;
}
void extensions_input_activity() {
    if (dispatching || !ready()) return;
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i)
        if (entries[i].hooks->input_activity) entries[i].hooks->input_activity();
    dispatching = false;
}
void extensions_recordable_note_on(u32 track, u32 pitch, u32 velocity) {
    if (dispatching) return;
    extensions_sync();
    if (!entries || track != current.recording.track || !current.recording.recordable ||
        !current.playing || pitch > 127 || velocity > 127) return;
    const i32 radius = extension_collision_radius(track);
    if (radius < 0) return;
    const extensions::LiveNote note = {track, pitch, velocity, extension_position(track),
                                       static_cast<u32>(radius)};
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i)
        if (entries[i].hooks->recordable_note_on) entries[i].hooks->recordable_note_on(current, note);
    dispatching = false;
}
void extensions_selected_note_off(u32 track, u32 pitch) {
    if (dispatching) return;
    extensions_sync();
    extensions_input_activity();
    if (!entries || track != current.recording.track) return;
    const extensions::NoteOff note = {track, pitch};
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i)
        if (entries[i].hooks->selected_note_off) entries[i].hooks->selected_note_off(current, note);
    dispatching = false;
}
void extensions_recording_tick(u32 delta) {
    if (dispatching) return;
    extensions_sync();
    if (!entries || !current.recording.recordable || !current.playing) return;
    const bool discontinuity = delta > 0x007fffffu;
    const extensions::Tick tick = {discontinuity ? 0 : extension_scaled_delta(current.recording.track, delta),
                                   discontinuity};
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i)
        if (entries[i].hooks->recording_tick) entries[i].hooks->recording_tick(current, tick);
    dispatching = false;
}
// Each extension in turn: the edits it captured since the last tick, then its
// gui_tick, which may renew the capture it holds.
void extensions_gui_tick() {
    if (dispatching) return;
    extensions_sync();
    if (!entries) return;
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i) {
        const ExtensionEntry& entry = entries[i];
        const bool holds = sdk_capture_holds(i + 1) != 0;
        if (holds) {
            extensions::Edit edits[32];
            const u32 count = sdk_capture_edits(i + 1, edits, 32);
            if (count && entry.hooks->captured_edits) entry.hooks->captured_edits(current, edits, count);
        }
        // Taking the held key lasts while renewed here.
        taking &= ~(1u << i);
        if (!entry.hooks->gui_tick) continue;
        running = i + 1;
        may_take = true;
        transaction = {holds ? entry.actions & action_capture : 0u, false, false};
        entry.hooks->gui_tick(current, actions);
        transaction = {0, false, false};
        may_take = false;
        running = 0;
    }
    dispatching = false;
}
void extensions_queued_control(const extensions::Control& control) {
    if (dispatching) return;
    extensions_sync();
    if (!entries) return;
    dispatching = true;
    for (u32 i = 0; i < entry_count; ++i) {
        running = i + 1;
        transaction = {granted(entries[i], control), false, false};
        if (entries[i].hooks->queued_control) entries[i].hooks->queued_control(current, control, actions);
        transaction = {0, false, false};
        running = 0;
    }
    dispatching = false;
}
}}

using namespace opxy;
using namespace opxy::platform;

// The keys service's gesture layer (keys.cpp) asks who holds a chord claim,
// delivers its keys, and asks whether the holder takes the held key.
u32 sdk_extensions_claim_owner(u32 claim) {
    if (!ready() || claim >= registry->claim_count) return 0;
    for (u32 i = 0; i < entry_count; ++i)
        if (entries[i].claims & (1u << claim)) return i + 1;
    return 0;
}
void sdk_extensions_gesture(u32 owner, const KeyGesture* g) {
    if (dispatching || !ready() || !owner || owner > entry_count || !g || g->claim >= registry->claim_count) return;
    extensions_sync();
    const ExtensionEntry& entry = entries[owner - 1];
    if (!entry.hooks->gesture || !(entry.claims & (1u << g->claim))) return;
    const extensions::Gesture gesture = {g->lead, g->key, g->pressed != 0, g->time};
    dispatching = true;
    running = owner;
    may_take = true;
    transaction = {grants(entry, 1u << g->claim), false, false};
    entry.hooks->gesture(current, gesture, actions);
    transaction = {0, false, false};
    may_take = false;
    running = 0;
    dispatching = false;
}
u32 sdk_extensions_takes(u32 owner) { return owner && owner <= 32 && (taking & (1u << (owner - 1))) ? 1 : 0; }
