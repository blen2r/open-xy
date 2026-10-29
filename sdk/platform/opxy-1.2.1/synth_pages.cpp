// Stock OS 1.2.1 hidden instrument pages (definition "hidePages"): part of the
// control service, so only builds with a machine that hides pages or uses track
// control link it. Track control's GUI tick (track_control.cpp) checks the page
// once per tick, and switches off the stock LFO of any track whose synth hides
// the LFO page.
#include "synth_controls.hpp"
#include "stock.hpp"

// 8c0b19c4 sets the page word M1..M4 select, wherever stock does; the page
// keys, track changes and menus all reach it. A tick after it shows a page the
// selected synth hides, the page shown before it comes back. When the track
// was already selected, that was a press of the page's key: it is kept for
// the track's machine (sdk_synth_page_key).
namespace {
using opxy::u32;
using opxy::platform::SynthControls;
using namespace opxy::platform;

// The instrument track controller's page word: 0 on the engine page, 1..3 on the others.
const u32 kInstrumentPage = 0x080238bc;
void set_page(u32 page) { stock<void(u32)>(0x8c0b19c4)(page); }

// A track's stock LFO is on while the byte at track data + 0x24 is set: stock's
// LFO update (8d58a9b4) reads it through 8d5879c0, and the M4 page's on/off sets
// it through 8d587984, which also marks the project changed.
const u32 kLfoPage = 3;
bool lfo_on(u32 track) { return stock<u32(u32)>(0x8d5879c0)(track) != 0; }
void lfo_off(u32 track) { stock<void(u32, u32)>(0x8d587984)(track, 0); }

const SynthControls* owned(u32 engine) {
    for (u32 i = 0; i < sdk_synth_control_count; ++i) {
        const SynthControls& m = sdk_synth_controls[i];
        if (m.engine() == engine && !m.effect) return m.ready() == 1 ? &m : nullptr;
    }
    return nullptr;
}

// The last stock instrument page shown that the selected synth does not hide,
// and the track selected then.
u32 page_before, track_before = kTracks;
u32 key_track = kTracks, key_number;
}

void sdk_synth_pages() {
    // A synth that hides the LFO page leaves no way to reach stock's LFO, so a
    // stock LFO left on by the track's previous engine is switched off.
    for (u32 t = 0; t < kTracks; ++t) {
        const SynthControls* hiding = owned(track_engine(t));
        if (hiding && (hiding->hidden_pages >> kLfoPage & 1) && lfo_on(t)) lfo_off(t);
    }
    const u32 track = selected_track();
    const u32 page = word(kInstrumentPage);
    const SynthControls* m = track < kTracks ? owned(track_engine(track)) : nullptr;
    const bool same_track = track == track_before;
    track_before = track;
    if (page >= opxy::page_controls) return;
    if (!m || !(m->hidden_pages >> page & 1)) {
        page_before = page;
        return;
    }
    if (same_track && !(m->hidden_pages >> page_before & 1)) {
        key_track = track;
        key_number = page + 1;
    }
    set_page(m->hidden_pages >> page_before & 1 ? 0 : page_before);
}

u32 sdk_synth_page_key(u32 track) {
    if (track != key_track) return 0;
    key_track = kTracks;
    return key_number;
}
