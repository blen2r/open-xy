// Stock OS 1.2.1 hidden instrument pages (definition "hidePages"): part of the
// control service, so only builds with a machine that hides pages or uses track
// control link it. Track control's GUI tick (track_control.cpp) checks the page
// once per tick. The envelope (M2) and filter (M3) pages can be hidden; the LFO
// page (M4) always stays. Pages work the same on an aux track a machine took
// over (aux_tracks.cpp), whose page is the aux controller's.
#include "synth_controls.hpp"
#include "stock.hpp"
#include "track_control.hpp"

// 8c0b19c4 sets the page word M1..M4 select, wherever stock does; the page
// keys, track changes and menus all reach it. A tick after it shows a page the
// selected synth hides, the page shown before it comes back. When the track
// was already selected, that was a press of the page's key: it is kept for
// the track's machine (sdk_synth_page_key).
//
// On aux tracks the page word is 0801eae4 (M1..M4 as 0..3), set by the aux
// controller's 8c08f74c(controller, tab), which uses only the tab. A held
// track's hidden tab shows the machine's own page (aux_tracks.cpp), so nothing
// flashes; the tick sets the tab back as above. Stock's M2 selects no tab on
// tracks without a second page (CV among them), so it does nothing there.
namespace {
using opxy::u32;
using opxy::platform::SynthControls;
using namespace opxy::platform;

// The instrument track controller's page word: 0 on the engine page, 1..3 on the others.
const u32 kInstrumentPage = 0x080238bc;
const u32 kAuxPage = 0x0801eae4, kAuxController = 0x08014db4;
const u32 kAllTracks = 16;
u32 page_word(u32 track) { return word(track < kTracks ? kInstrumentPage : kAuxPage); }
void set_page(u32 track, u32 page) {
    if (track < kTracks) stock<void(u32)>(0x8c0b19c4)(page);
    else stock<void(u32, u32)>(0x8c08f74c)(kAuxController, page);
}

const SynthControls* owned(u32 engine) {
    for (u32 i = 0; i < sdk_synth_control_count; ++i) {
        const SynthControls& m = sdk_synth_controls[i];
        if (m.engine() == engine && !m.effect) return m.ready() == 1 ? &m : nullptr;
    }
    return nullptr;
}

// The engine whose machine runs on a track: the machine holding an aux track.
u32 engine_on(u32 track) { return track < kTracks ? track_engine(track) : control_engine(track); }

// The last stock page shown that the selected synth does not hide, and the
// track selected then.
u32 page_before, track_before = kAllTracks;
u32 key_track = kAllTracks, key_number;
}

void sdk_synth_pages() {
    const u32 track = selected_track();
    if (track >= kAllTracks) return;
    const u32 page = page_word(track);
    const u32 engine = engine_on(track);
    const SynthControls* m = engine ? owned(engine) : nullptr;
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
    set_page(track, m->hidden_pages >> page_before & 1 ? 0 : page_before);
}

u32 sdk_synth_page_key(u32 track) {
    if (track != key_track) return 0;
    key_track = kAllTracks;
    return key_number;
}
