#pragma once
// audio_test_helpers.h — C functions that expose internal audio state for unit
// testing. included by audio_test.cc (C++) via extern "C"; implemented in
// audio_test_stubs.c. never shipped as part of the runtime.

#ifdef __cplusplus
extern "C" {
#endif

#include <audio.h>
#include <stdint.h>

// ---- handle sub-field accessors (mirror audio_sub_tag / audio_sub_idx) ----

uint32_t audio_test_sub_tag(uint64_t handle);
uint32_t audio_test_sub_idx(uint64_t handle);

// ---- compiled graph inspection ----

uint32_t audio_test_graph_bus_count(audio_graph_handle_t g);
uint32_t audio_test_graph_route_count(audio_graph_handle_t g);
uint32_t audio_test_graph_effect_count(audio_graph_handle_t g);

// audio_test_effect_type_t mirrors audio_effect_type_t without exposing the
// internal header to C++.
typedef enum {
    AUDIO_TEST_EFFECT_NONE       = 0,
    AUDIO_TEST_EFFECT_LIMITER    = 1,
    AUDIO_TEST_EFFECT_LOWPASS    = 2,
    AUDIO_TEST_EFFECT_COMPRESSOR = 3,
} audio_test_effect_type_t;

// returns the type of effect[idx] in the compiled graph
audio_test_effect_type_t audio_test_graph_effect_type(audio_graph_handle_t g,
                                                      uint32_t idx);

// ---- handle tag constants (mirrored so C++ test doesn't need audio_internal.h) ----

uint32_t audio_test_tag_bus(void);
uint32_t audio_test_tag_voice(void);

#ifdef __cplusplus
}
#endif
