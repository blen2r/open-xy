// Host boundary double for the real hidden-pages check in synth_controls.cpp:
// stock's page word, its page setter, the selected track and track engines.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <synth_controls.cpp>

namespace {
using opxy::u32;
using opxy::platform::Address;

void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
u32 page, sets, selected, engines[16];
void stock_set_page(u32 value) { page = value; ++sets; }
u32 stock_selected() { return selected; }
u32 stock_engine(u32 track) { return track < 16 ? engines[track] : 0; }
int ready() { return 1; }
}

extern "C" {
// Engine 40 hides the envelope and filter pages; engine 41 hides nothing; 42
// hides every page but the engine page.
const opxy::platform::SynthControls sdk_synth_controls[] = {
    {reinterpret_cast<const char*>(40), ready, {0, 1, 2, 3, 4, 5, 6, 7}, 0, nullptr, false, false, 6},
    {reinterpret_cast<const char*>(41), ready, {0, 1, 2, 3, 4, 5, 6, 7}, 0, nullptr, false, false, 0},
    {reinterpret_cast<const char*>(42), ready, {0, 1, 2, 3, 4, 5, 6, 7}, 0, nullptr, false, false, 14},
    {nullptr, nullptr, {0, 1, 2, 3, 4, 5, 6, 7}, 0, nullptr, false, false, 0}};
const u32 sdk_synth_control_count = 3;
}

Address opxy::platform::host_stock_address(Address address) {
    if (address == 0x080238bc) return reinterpret_cast<Address>(&page);
    if (address == 0x8c0b19c4) return reinterpret_cast<Address>(&stock_set_page);
    if (address == 0x8d4d6f14) return reinterpret_cast<Address>(&stock_selected);
    if (address == 0x8d58781c) return reinterpret_cast<Address>(&stock_engine);
    std::abort();
}

int main() {
    engines[2] = 40;
    engines[3] = 41;
    // A stock engine and a synth hiding nothing keep every page.
    for (u32 track : {0u, 3u})
        for (u32 p = 0; p < 4; ++p) {
            selected = track;
            page = p;
            sdk_synth_pages();
            require(page == p && sets == 0, "Page changed for a synth hiding nothing");
        }
    // A synth hiding every page but the engine page stays there.
    engines[4] = 42;
    selected = 4;
    page = 3;
    sdk_synth_pages();
    require(page == 0 && sets == 1, "LFO page not hidden");
    sets = 0;
    // On the hiding synth, M4 then M2: back to M4; M3 from M1: back to M1.
    selected = 2;
    page = 3;
    sdk_synth_pages();
    page = 1;
    sdk_synth_pages();
    require(page == 3 && sets == 1, "Envelope page not replaced by the page before");
    page = 0;
    sdk_synth_pages();
    page = 2;
    sdk_synth_pages();
    require(page == 0 && sets == 2, "Filter page not replaced by the page before");
    require(sdk_synth_page_key(2) == 3 && sdk_synth_page_key(2) == 0, "M3 press not kept once");
    // Selecting the track while on another track's envelope page: the engine page.
    selected = 3;
    page = 1;
    sdk_synth_pages();
    selected = 2;
    sdk_synth_pages();
    require(page == 0 && sets == 3, "Hidden page kept after selecting the track");
    // Pressing a hidden page's key on the selected track is kept for its
    // machine, once; selecting the track while on that page is not a press.
    require(sdk_synth_page_key(2) == 0, "Track selection taken for a key press");
    sdk_synth_pages();  // the next tick, on the engine page
    page = 2;
    sdk_synth_pages();
    require(page == 0 && sdk_synth_page_key(3) == 0 && sdk_synth_page_key(2) == 3 && sdk_synth_page_key(2) == 0,
            "M3 press not kept once for its track");
    // Auxiliary tracks are left alone.
    const u32 sets_before = sets;
    selected = 9;
    engines[9] = 40;
    page = 1;
    sdk_synth_pages();
    require(page == 1 && sets == sets_before, "Auxiliary track's page changed");
    return 0;
}
