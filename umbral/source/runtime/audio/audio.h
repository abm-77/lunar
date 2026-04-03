#pragma once
// audio.h — public C ABI for the Umbral audio runtime.
// all state is hidden behind opaque uint64_t handles. threading: main thread only
// for all API calls except the audio callback (which is miniaudio-managed).
// ownership: pair every create with a matching destroy. handle validity:
// AUDIO_NULL_HANDLE (0) is invalid; stale-handle access is caught by the mem
// system in UM_DEBUG, undefined behavior in release.

#ifdef __cplusplus
extern "C" {
#endif

#include <common/c_types.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint64_t audio_builder_handle_t; // mutable graph definition; destroyed after compile
typedef uint64_t audio_graph_handle_t;   // immutable compiled graph; shared across contexts
typedef uint64_t audio_ctx_handle_t;     // live execution context; owns the audio device
typedef uint64_t audio_bus_handle_t;     // bus slot within a builder or compiled graph
typedef uint64_t audio_route_handle_t;   // route slot within a builder
typedef uint64_t audio_voice_handle_t;   // active one-shot voice within a context
typedef uint64_t audio_stream_handle_t;  // active streaming voice within a context
typedef uint64_t audio_limiter_handle_t;    // limiter effect attached to a bus in a builder
typedef uint64_t audio_lowpass_handle_t;    // lowpass effect attached to a bus in a builder
typedef uint64_t audio_compressor_handle_t; // compressor effect attached to a bus in a builder

#define AUDIO_NULL_HANDLE ((uint64_t)0)

// ---- graph builder ----

// rt_audio_builder_create — allocate a new mutable graph builder.
// returns AUDIO_NULL_HANDLE on allocation failure.
audio_builder_handle_t rt_audio_builder_create(void);

// rt_audio_builder_destroy — free a builder and all its definitions.
// must not be called after rt_audio_compile_graph (builder is already invalid).
void rt_audio_builder_destroy(audio_builder_handle_t b);

// rt_audio_builder_create_bus — add a named bus to the builder.
// name_ptr/name_len: UTF-8 string; copied internally; need not be null-terminated.
// returns AUDIO_NULL_HANDLE if the bus cap (AUDIO_MAX_BUSES) is exceeded.
audio_bus_handle_t rt_audio_builder_create_bus(audio_builder_handle_t b,
                                               const uint8_t *name_ptr,
                                               uint64_t name_len);

// rt_audio_builder_set_output_bus — designate which bus is the master output.
// exactly one output bus must be set before rt_audio_compile_graph.
void rt_audio_builder_set_output_bus(audio_builder_handle_t b,
                                     audio_bus_handle_t bus);

// rt_audio_builder_create_route — connect two buses with unity gain.
audio_route_handle_t rt_audio_builder_create_route(audio_builder_handle_t b,
                                                   audio_bus_handle_t from,
                                                   audio_bus_handle_t to);

// rt_audio_builder_create_route_with_gain — connect two buses with an explicit gain.
// gain: linear amplitude multiplier; 1.0 = unity.
audio_route_handle_t rt_audio_builder_create_route_with_gain(
    audio_builder_handle_t b, audio_bus_handle_t from, audio_bus_handle_t to,
    float gain);

// rt_audio_builder_add_limiter — attach a peak limiter to a bus.
// the limiter is appended to the end of the bus's effect chain.
audio_limiter_handle_t rt_audio_builder_add_limiter(audio_builder_handle_t b,
                                                    audio_bus_handle_t bus);

// rt_audio_builder_add_lowpass — attach a 1-pole IIR lowpass filter to a bus.
// cutoff_hz: cutoff frequency in Hz; must be > 0 and < sample_rate/2.
audio_lowpass_handle_t rt_audio_builder_add_lowpass(audio_builder_handle_t b,
                                                    audio_bus_handle_t bus,
                                                    float cutoff_hz);

// rt_audio_builder_add_compressor — attach a feed-forward RMS compressor to a bus.
// threshold_db: level above which compression begins (e.g. -18.0f).
// ratio: compression ratio >= 1.0 (e.g. 4.0 = 4:1 compression above threshold).
audio_compressor_handle_t rt_audio_builder_add_compressor(
    audio_builder_handle_t b, audio_bus_handle_t bus, float threshold_db,
    float ratio);

// ---- graph compilation ----

// rt_audio_compile_graph — validate and freeze the graph builder.
// in UM_DEBUG: validates topology (cycle check, output reachability, param ranges)
//   and prints a diagnostic to stderr on failure.
// on success: returns an immutable AudioGraph handle; builder must be destroyed
//   by the caller after this call (it is no longer needed).
// on failure: returns AUDIO_NULL_HANDLE; builder is still valid and may be fixed.
audio_graph_handle_t rt_audio_compile_graph(audio_builder_handle_t b);

// rt_audio_graph_default — build and compile the standard 4-bus graph:
//   music, sfx, ui → master (with a limiter on master).
// equivalent to manually constructing that builder and calling compile_graph.
audio_graph_handle_t rt_audio_graph_default(void);

// rt_audio_graph_destroy — free the compiled graph.
// all contexts using this graph must be destroyed first.
void rt_audio_graph_destroy(audio_graph_handle_t g);

// ---- execution context ----

// rt_audio_ctx_create — create a live execution context from a compiled graph.
// sample_rate: output sample rate in Hz; 0 = use AUDIO_DEFAULT_SAMPLE_RATE (48000).
// block_frames: frames per callback; 0 = use AUDIO_BLOCK_FRAMES (512).
// max_voices: preallocated voice pool size; 0 = use AUDIO_MAX_VOICES_DEFAULT (64).
// if max_voices == 0 the miniaudio device is NOT opened (useful for unit tests).
// returns AUDIO_NULL_HANDLE on failure.
audio_ctx_handle_t rt_audio_ctx_create(audio_graph_handle_t g,
                                       uint32_t sample_rate,
                                       uint32_t block_frames,
                                       uint32_t max_voices);

// rt_audio_ctx_destroy — stop playback, close the audio device, and free the context.
void rt_audio_ctx_destroy(audio_ctx_handle_t ctx);

// ---- bus handles from a compiled graph ----

// rt_audio_graph_bus_by_name — look up a bus handle from the compiled graph by name.
// returns AUDIO_NULL_HANDLE if no bus with that name exists.
audio_bus_handle_t rt_audio_graph_bus_by_name(audio_graph_handle_t g,
                                              const uint8_t *name_ptr,
                                              uint64_t name_len);

// ---- playback ----

// rt_audio_ctx_play_clip — play a one-shot float32 stereo PCM clip.
// pcm: interleaved stereo float32 samples; must remain live until the voice finishes.
// frame_count: number of stereo frames (sample count / 2).
// bus: target bus handle from rt_audio_graph_bus_by_name or rt_audio_builder_create_bus.
// gain_db: initial gain in dB; 0.0 = unity.
// returns AUDIO_NULL_HANDLE if no voice slot is available.
audio_voice_handle_t rt_audio_ctx_play_clip(audio_ctx_handle_t ctx,
                                            const float *pcm,
                                            uint64_t frame_count,
                                            audio_bus_handle_t bus,
                                            float gain_db);

// rt_audio_ctx_play_stream — play a streaming float32 stereo PCM source.
// v0: identical to play_clip (no chunked streaming yet); alias for API completeness.
audio_stream_handle_t rt_audio_ctx_play_stream(audio_ctx_handle_t ctx,
                                               const float *pcm,
                                               uint64_t frame_count,
                                               audio_bus_handle_t bus,
                                               float gain_db);

// rt_audio_ctx_stop_voice — stop a voice before it finishes naturally.
// safe to call with a handle that has already finished (no-op in that case).
void rt_audio_ctx_stop_voice(audio_ctx_handle_t ctx, audio_voice_handle_t v);

void rt_audio_ctx_set_voice_gain_db(audio_ctx_handle_t ctx,
                                    audio_voice_handle_t v, float gain_db);
void rt_audio_ctx_set_voice_pan(audio_ctx_handle_t ctx, audio_voice_handle_t v,
                                float pan);
void rt_audio_ctx_set_voice_pitch(audio_ctx_handle_t ctx,
                                  audio_voice_handle_t v, float pitch);

// ---- bus control ----

void rt_audio_ctx_set_bus_gain_db(audio_ctx_handle_t ctx,
                                  audio_bus_handle_t bus, float gain_db);

// rt_audio_ctx_set_bus_gain_db_ramp — smoothly ramp bus gain over ramp_ns nanoseconds.
void rt_audio_ctx_set_bus_gain_db_ramp(audio_ctx_handle_t ctx,
                                       audio_bus_handle_t bus, float gain_db,
                                       uint64_t ramp_ns);

#ifdef __cplusplus
}
#endif
