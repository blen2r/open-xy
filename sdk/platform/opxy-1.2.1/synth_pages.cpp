// Stock OS 1.2.1 hidden instrument pages (definition "hidePages"): part of the
// control service, so only builds with a machine that hides pages or uses track
// control link it. Track control's GUI tick (track_control.cpp) checks the page
// once per tick. The envelope (M2) and filter (M3) pages can be hidden; the LFO
// page (M4) always stays.
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
