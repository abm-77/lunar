// audio_test_stubs.c — C stubs and test helpers for audio unit tests.
// links together with audio.c + mem.c in the audio_tests GTest target.

#include <audio_internal.h>
#include <audio_test_helpers.h>
#include <string.h>

// mem.c site table — zero entries; all site lookups fall through to <unknown>
typedef struct { const char *file; unsigned line; unsigned col; } um_site_info_t;
const um_site_info_t __um_sites[]  = {};
const unsigned int   __um_sites_count = 0;

// device stubs — context create/destroy work without audio hardware
void audio_device_open(audio_ctx_state_t *ctx)  { (void)ctx; }
void audio_device_close(audio_ctx_state_t *ctx) { (void)ctx; }

// ---- handle helpers ----

uint32_t audio_test_sub_tag(uint64_t handle) {
    return audio_sub_tag(handle);
}

uint32_t audio_test_sub_idx(uint64_t handle) {
    return audio_sub_idx(handle);
}

// ---- compiled graph inspection ----

uint32_t audio_test_graph_bus_count(audio_graph_handle_t g) {
    audio_graph_state_t *gs = graph_ptr(g);
    return gs ? gs->bus_count : 0;
}

uint32_t audio_test_graph_route_count(audio_graph_handle_t g) {
    audio_graph_state_t *gs = graph_ptr(g);
    return gs ? gs->route_count : 0;
}

uint32_t audio_test_graph_effect_count(audio_graph_handle_t g) {
    audio_graph_state_t *gs = graph_ptr(g);
    return gs ? gs->effect_count : 0;
}

audio_test_effect_type_t audio_test_graph_effect_type(audio_graph_handle_t g,
                                                      uint32_t idx) {
    audio_graph_state_t *gs = graph_ptr(g);
    if (!gs || idx >= gs->effect_count) return AUDIO_TEST_EFFECT_NONE;
    return (audio_test_effect_type_t)gs->effects[idx].type;
}

// ---- tag constants ----

uint32_t audio_test_tag_bus(void)   { return AUDIO_TAG_BUS; }
uint32_t audio_test_tag_voice(void) { return AUDIO_TAG_VOICE; }
