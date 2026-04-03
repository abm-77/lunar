#include <audio_internal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

// ---- internal helpers ----

// alloc_typed — allocate a zero-initialised block via the runtime mem system.
static uint64_t alloc_typed(uint64_t size, uint64_t align) {
    uint64_t h = rt_alloc(size, align, AUDIO_ALLOC_TAG, /*site=*/0);
    if (!h) return 0;
    um_slice_u8_t s = rt_slice_from_alloc(h, 1, 1, size, 0, 1);
    if (s.ptr) memset((void *)(uintptr_t)s.ptr, 0, (size_t)size);
    return h;
}

// ---- graph builder ----

audio_builder_handle_t rt_audio_builder_create(void) {
    return (audio_builder_handle_t)alloc_typed(sizeof(audio_builder_state_t),
                                               _Alignof(audio_builder_state_t));
}

void rt_audio_builder_destroy(audio_builder_handle_t b) {
    if (!b) return;
    rt_free(b, 0);
}

audio_bus_handle_t rt_audio_builder_create_bus(audio_builder_handle_t b,
                                               const uint8_t *name_ptr,
                                               uint64_t name_len) {
    audio_builder_state_t *st = builder_ptr(b);
    if (!st) return AUDIO_NULL_HANDLE;

    if (st->bus_count >= AUDIO_MAX_BUSES) {
#if UM_DEBUG
        fprintf(stderr, "audio: bus cap (%d) exceeded\n", AUDIO_MAX_BUSES);
#endif
        return AUDIO_NULL_HANDLE;
    }

    uint32_t idx = st->bus_count++;
    audio_bus_def_t *bus = &st->buses[idx];

    // copy name (truncate to AUDIO_MAX_BUS_NAME - 1 bytes + null terminator)
    uint64_t copy_len = name_len < (AUDIO_MAX_BUS_NAME - 1)
                            ? name_len
                            : (AUDIO_MAX_BUS_NAME - 1);
    if (name_ptr) memcpy(bus->name, name_ptr, (size_t)copy_len);
    bus->name[copy_len] = '\0';

    bus->id = ++st->next_bus_id; // 1-based
    return audio_make_sub_handle(AUDIO_TAG_BUS, idx);
}

void rt_audio_builder_set_output_bus(audio_builder_handle_t b,
                                     audio_bus_handle_t bus) {
    audio_builder_state_t *st = builder_ptr(b);
    if (!st) return;
    uint32_t idx = audio_sub_idx(bus);
    if (idx >= st->bus_count) return;
    st->output_bus_id = st->buses[idx].id;
}

static audio_route_handle_t create_route_internal(audio_builder_handle_t b,
                                                  audio_bus_handle_t from,
                                                  audio_bus_handle_t to,
                                                  float gain) {
    audio_builder_state_t *st = builder_ptr(b);
    if (!st) return AUDIO_NULL_HANDLE;
    if (st->route_count >= AUDIO_MAX_ROUTES) return AUDIO_NULL_HANDLE;

    uint32_t from_idx = audio_sub_idx(from);
    uint32_t to_idx   = audio_sub_idx(to);
    if (from_idx >= st->bus_count || to_idx >= st->bus_count)
        return AUDIO_NULL_HANDLE;

    uint32_t ridx = st->route_count++;
    st->routes[ridx].from_id = st->buses[from_idx].id;
    st->routes[ridx].to_id   = st->buses[to_idx].id;
    st->routes[ridx].gain    = gain;
    return audio_make_sub_handle(AUDIO_TAG_ROUTE, ridx);
}

audio_route_handle_t rt_audio_builder_create_route(audio_builder_handle_t b,
                                                   audio_bus_handle_t from,
                                                   audio_bus_handle_t to) {
    return create_route_internal(b, from, to, 1.0f);
}

audio_route_handle_t rt_audio_builder_create_route_with_gain(
    audio_builder_handle_t b, audio_bus_handle_t from, audio_bus_handle_t to,
    float gain) {
    return create_route_internal(b, from, to, gain);
}

static uint64_t add_effect(audio_builder_handle_t b, audio_bus_handle_t bus,
                           audio_effect_def_t def, uint32_t tag) {
    audio_builder_state_t *st = builder_ptr(b);
    if (!st) return AUDIO_NULL_HANDLE;
    if (st->effect_count >= AUDIO_MAX_EFFECTS) return AUDIO_NULL_HANDLE;

    uint32_t bidx = audio_sub_idx(bus);
    if (bidx >= st->bus_count) return AUDIO_NULL_HANDLE;

    uint32_t eidx         = st->effect_count++;
    def.bus_idx           = bidx;
    st->effects[eidx]     = def;
    return audio_make_sub_handle(tag, eidx);
}

audio_limiter_handle_t rt_audio_builder_add_limiter(audio_builder_handle_t b,
                                                    audio_bus_handle_t bus) {
    audio_effect_def_t def = {.type = AUDIO_EFFECT_LIMITER};
    return add_effect(b, bus, def, AUDIO_TAG_LIMITER);
}

audio_lowpass_handle_t rt_audio_builder_add_lowpass(audio_builder_handle_t b,
                                                    audio_bus_handle_t bus,
                                                    float cutoff_hz) {
    audio_effect_def_t def = {
        .type = AUDIO_EFFECT_LOWPASS, .cutoff_hz = cutoff_hz};
    return add_effect(b, bus, def, AUDIO_TAG_LOWPASS);
}

audio_compressor_handle_t rt_audio_builder_add_compressor(
    audio_builder_handle_t b, audio_bus_handle_t bus, float threshold_db,
    float ratio) {
    audio_effect_def_t def = {
        .type         = AUDIO_EFFECT_COMPRESSOR,
        .threshold_db = threshold_db,
        .ratio        = ratio,
    };
    return add_effect(b, bus, def, AUDIO_TAG_COMPRESSOR);
}

// ---- topological sort (Kahn's algorithm) ----
// adjacency list encoded as route_def pairs; output stored into bus_order[].
// returns true on success; false on cycle detected.

static bool topo_sort(const audio_builder_state_t *st,
                      uint32_t bus_order[AUDIO_MAX_BUSES],
                      uint32_t *out_count) {
    // in-degree for each bus (by index)
    int in_degree[AUDIO_MAX_BUSES] = {0};
    for (uint32_t i = 0; i < st->route_count; ++i) {
        // find to_idx from to_id
        for (uint32_t j = 0; j < st->bus_count; ++j) {
            if (st->buses[j].id == st->routes[i].to_id) {
                in_degree[j]++;
                break;
            }
        }
    }

    // queue of zero-in-degree buses
    uint32_t queue[AUDIO_MAX_BUSES];
    uint32_t qhead = 0, qtail = 0;
    for (uint32_t i = 0; i < st->bus_count; ++i)
        if (in_degree[i] == 0) queue[qtail++] = i;

    uint32_t count = 0;
    while (qhead < qtail) {
        uint32_t u = queue[qhead++];
        bus_order[count++] = u;

        for (uint32_t i = 0; i < st->route_count; ++i) {
            if (st->buses[u].id != st->routes[i].from_id) continue;
            for (uint32_t j = 0; j < st->bus_count; ++j) {
                if (st->buses[j].id != st->routes[i].to_id) continue;
                if (--in_degree[j] == 0) queue[qtail++] = j;
                break;
            }
        }
    }

    *out_count = count;
    return count == st->bus_count; // cycle exists if not all buses processed
}

// ---- UM_DEBUG graph validation ----

#if UM_DEBUG
static bool validate_graph(const audio_builder_state_t *st) {
    bool ok = true;

    // exactly one output bus
    if (st->output_bus_id == 0) {
        fprintf(stderr, "audio compile_graph: no output bus set\n");
        ok = false;
    }

    // self-routes
    for (uint32_t i = 0; i < st->route_count; ++i) {
        if (st->routes[i].from_id == st->routes[i].to_id) {
            fprintf(stderr, "audio compile_graph: self-route on bus id %u\n",
                    st->routes[i].from_id);
            ok = false;
        }
    }

    // duplicate routes
    for (uint32_t i = 0; i < st->route_count; ++i) {
        for (uint32_t j = i + 1; j < st->route_count; ++j) {
            if (st->routes[i].from_id == st->routes[j].from_id &&
                st->routes[i].to_id == st->routes[j].to_id) {
                fprintf(stderr,
                        "audio compile_graph: duplicate route %u→%u\n",
                        st->routes[i].from_id, st->routes[i].to_id);
                ok = false;
            }
        }
    }

    // cycle check via topo sort
    uint32_t order[AUDIO_MAX_BUSES];
    uint32_t cnt = 0;
    if (!topo_sort(st, order, &cnt)) {
        fprintf(stderr, "audio compile_graph: route graph contains a cycle\n");
        ok = false;
        // skip reachability check if there's a cycle
        return ok;
    }

    // output reachability: BFS from output bus over reversed edges
    uint32_t output_idx = AUDIO_MAX_BUSES;
    for (uint32_t i = 0; i < st->bus_count; ++i) {
        if (st->buses[i].id == st->output_bus_id) { output_idx = i; break; }
    }
    if (output_idx < st->bus_count) {
        bool reached[AUDIO_MAX_BUSES] = {false};
        uint32_t bfs[AUDIO_MAX_BUSES];
        uint32_t bhead = 0, btail = 0;
        reached[output_idx] = true;
        bfs[btail++] = output_idx;
        while (bhead < btail) {
            uint32_t cur = bfs[bhead++];
            for (uint32_t i = 0; i < st->route_count; ++i) {
                if (st->buses[cur].id != st->routes[i].to_id) continue;
                for (uint32_t j = 0; j < st->bus_count; ++j) {
                    if (st->buses[j].id == st->routes[i].from_id &&
                        !reached[j]) {
                        reached[j] = true;
                        bfs[btail++] = j;
                    }
                }
            }
        }
        for (uint32_t i = 0; i < st->bus_count; ++i) {
            if (!reached[i]) {
                fprintf(stderr,
                        "audio compile_graph: bus '%s' has no path to output\n",
                        st->buses[i].name);
                ok = false;
            }
        }
    }

    // effect parameter ranges
    for (uint32_t i = 0; i < st->effect_count; ++i) {
        const audio_effect_def_t *e = &st->effects[i];
        if (e->type == AUDIO_EFFECT_LOWPASS && e->cutoff_hz <= 0.0f) {
            fprintf(stderr,
                    "audio compile_graph: lowpass cutoff_hz must be > 0\n");
            ok = false;
        }
        if (e->type == AUDIO_EFFECT_COMPRESSOR && e->ratio < 1.0f) {
            fprintf(stderr,
                    "audio compile_graph: compressor ratio must be >= 1.0\n");
            ok = false;
        }
        if (e->bus_idx >= st->bus_count) {
            fprintf(stderr,
                    "audio compile_graph: effect references unknown bus\n");
            ok = false;
        }
    }

    return ok;
}
#endif // UM_DEBUG

// ---- compile_graph ----

audio_graph_handle_t rt_audio_compile_graph(audio_builder_handle_t b) {
    audio_builder_state_t *st = builder_ptr(b);
    if (!st) return AUDIO_NULL_HANDLE;

#if UM_DEBUG
    if (!validate_graph(st)) return AUDIO_NULL_HANDLE;
#endif

    uint64_t gh = alloc_typed(sizeof(audio_graph_state_t),
                              _Alignof(audio_graph_state_t));
    if (!gh) return AUDIO_NULL_HANDLE;

    audio_graph_state_t *g = graph_ptr(gh);

    // topological sort
    uint32_t cnt = 0;
    topo_sort(st, g->bus_order, &cnt);
    g->bus_count = st->bus_count;

    // copy bus definitions (preserve original ordering so idx is stable)
    memcpy(g->buses, st->buses, st->bus_count * sizeof(audio_bus_def_t));

    // copy routes (translate id→idx)
    g->route_count = 0;
    for (uint32_t i = 0; i < st->route_count; ++i) {
        uint32_t fi = AUDIO_MAX_BUSES, ti = AUDIO_MAX_BUSES;
        for (uint32_t j = 0; j < st->bus_count; ++j) {
            if (st->buses[j].id == st->routes[i].from_id) fi = j;
            if (st->buses[j].id == st->routes[i].to_id)   ti = j;
        }
        if (fi < AUDIO_MAX_BUSES && ti < AUDIO_MAX_BUSES) {
            audio_compiled_route_t *r = &g->routes[g->route_count++];
            r->from_idx = fi;
            r->to_idx   = ti;
            r->gain     = st->routes[i].gain;
        }
    }

    // copy effects
    g->effect_count = st->effect_count;
    memcpy(g->effects, st->effects,
           st->effect_count * sizeof(audio_effect_def_t));

    // find output_bus_idx
    for (uint32_t i = 0; i < st->bus_count; ++i) {
        if (st->buses[i].id == st->output_bus_id) {
            g->output_bus_idx = i;
            break;
        }
    }

    return (audio_graph_handle_t)gh;
}

audio_graph_handle_t rt_audio_graph_default(void) {
    audio_builder_handle_t b = rt_audio_builder_create();
    if (!b) return AUDIO_NULL_HANDLE;

    audio_bus_handle_t master = rt_audio_builder_create_bus(
        b, (const uint8_t *)"master", 6);
    audio_bus_handle_t music  = rt_audio_builder_create_bus(
        b, (const uint8_t *)"music", 5);
    audio_bus_handle_t sfx    = rt_audio_builder_create_bus(
        b, (const uint8_t *)"sfx", 3);
    audio_bus_handle_t ui     = rt_audio_builder_create_bus(
        b, (const uint8_t *)"ui", 2);

    rt_audio_builder_create_route(b, music, master);
    rt_audio_builder_create_route(b, sfx,   master);
    rt_audio_builder_create_route(b, ui,    master);
    rt_audio_builder_add_limiter(b, master);
    rt_audio_builder_set_output_bus(b, master);

    audio_graph_handle_t g = rt_audio_compile_graph(b);
    rt_audio_builder_destroy(b);
    return g;
}

void rt_audio_graph_destroy(audio_graph_handle_t g) {
    if (!g) return;
    rt_free(g, 0);
}

audio_bus_handle_t rt_audio_graph_bus_by_name(audio_graph_handle_t g,
                                              const uint8_t *name_ptr,
                                              uint64_t name_len) {
    audio_graph_state_t *gs = graph_ptr(g);
    if (!gs || !name_ptr) return AUDIO_NULL_HANDLE;
    for (uint32_t i = 0; i < gs->bus_count; ++i) {
        const char *n = gs->buses[i].name;
        uint64_t nlen = (uint64_t)strlen(n);
        if (nlen == name_len &&
            memcmp(n, name_ptr, (size_t)name_len) == 0)
            return audio_make_sub_handle(AUDIO_TAG_BUS, i);
    }
    return AUDIO_NULL_HANDLE;
}

// ---- execution context ----

audio_ctx_handle_t rt_audio_ctx_create(audio_graph_handle_t g,
                                       uint32_t sample_rate,
                                       uint32_t block_frames,
                                       uint32_t max_voices) {
    audio_graph_state_t *gs = graph_ptr(g);
    if (!gs) return AUDIO_NULL_HANDLE;

    if (sample_rate  == 0) sample_rate  = AUDIO_DEFAULT_SAMPLE_RATE;
    if (block_frames == 0) block_frames = AUDIO_BLOCK_FRAMES;
    if (max_voices   == 0) {
        // zero max_voices: no device opened (used in tests and headless mode)
        // still allocate context so the API is exercisable
    }

    uint64_t ch = alloc_typed(sizeof(audio_ctx_state_t),
                              _Alignof(audio_ctx_state_t));
    if (!ch) return AUDIO_NULL_HANDLE;

    audio_ctx_state_t *ctx = ctx_ptr(ch);
    ctx->graph        = gs;
    ctx->graph_handle = g;
    ctx->sample_rate  = sample_rate;
    ctx->block_frames = block_frames;
    ctx->max_voices   = max_voices;

    // initialise bus RT state (unity gain, no ramp)
    for (uint32_t i = 0; i < gs->bus_count; ++i)
        ctx->bus_rt[i].gain_linear = 1.0f;

    // pre-compute lowpass filter coefficients from graph definition
    for (uint32_t i = 0; i < gs->effect_count; ++i) {
        if (gs->effects[i].type == AUDIO_EFFECT_LOWPASS) {
            // dsp_lowpass_alpha is defined in audio_dsp.h; include it here via
            // a helper below to avoid pulling the math header into this file
            float dt = 1.0f / (float)sample_rate;
            float rc = 1.0f / (6.2831853f * gs->effects[i].cutoff_hz);
            ctx->effect_state[i].lowpass.alpha = dt / (dt + rc);
        }
        // limiter: gain_env starts at 1.0 (unity)
        if (gs->effects[i].type == AUDIO_EFFECT_LIMITER)
            ctx->effect_state[i].limiter.gain_env = 1.0f;
    }

    // allocate voice pool (if max_voices > 0)
    if (max_voices > 0) {
        uint64_t vsize = (uint64_t)sizeof(audio_voice_state_t) * max_voices;
        ctx->voices_handle = alloc_typed(vsize, _Alignof(audio_voice_state_t));
        if (!ctx->voices_handle) {
            rt_free(ch, 0);
            return AUDIO_NULL_HANDLE;
        }
        um_slice_u8_t vs = rt_slice_from_alloc(
            ctx->voices_handle, 1, 1, vsize, 0, 1);
        ctx->voices = (audio_voice_state_t *)(uintptr_t)vs.ptr;
    }

    // allocate mix buffers: bus_count * block_frames * channels floats
    if (max_voices > 0 && gs->bus_count > 0) {
        uint64_t mbsize = (uint64_t)sizeof(float) * gs->bus_count *
                          block_frames * AUDIO_CHANNELS;
        ctx->mix_bufs_handle = alloc_typed(mbsize, _Alignof(float));
        if (!ctx->mix_bufs_handle) {
            rt_free(ctx->voices_handle, 0);
            rt_free(ch, 0);
            return AUDIO_NULL_HANDLE;
        }
        um_slice_u8_t ms = rt_slice_from_alloc(
            ctx->mix_bufs_handle, 1, 1, mbsize, 0, 1);
        ctx->mix_bufs = (float *)(uintptr_t)ms.ptr;
    }

    // open audio device (no-op when max_voices == 0)
    if (max_voices > 0) {
        gs->sample_rate = sample_rate; // store for DSP use
        audio_device_open(ctx);
    }

    return (audio_ctx_handle_t)ch;
}

void rt_audio_ctx_destroy(audio_ctx_handle_t h) {
    if (!h) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx) return;

    // close device before freeing buffers (device thread may be running)
    if (ctx->device_started)
        audio_device_close(ctx);

    if (ctx->mix_bufs_handle)   rt_free(ctx->mix_bufs_handle, 0);
    if (ctx->voices_handle)     rt_free(ctx->voices_handle, 0);
    if (ctx->device_handle)     rt_free(ctx->device_handle, 0);
    rt_free(h, 0);
}

// ---- playback control (main thread → command ring) ----

// find a free voice slot; returns AUDIO_MAX_VOICES if none available
static uint32_t alloc_voice(audio_ctx_state_t *ctx) {
    for (uint32_t i = 0; i < ctx->max_voices; ++i)
        if (!ctx->voices[i].active) return i;
    return ctx->max_voices; // none free
}

// db_to_linear inline for non-DSP files (avoids pulling math.h everywhere)
static float gain_from_db(float db) {
    if (db <= -120.0f) return 0.0f;
    // powf(10, db/20) ≈ expf(db * 0.11512925f)
    return __builtin_exp(db * 0.11512925f);
}

static audio_voice_handle_t play_voice_internal(audio_ctx_handle_t h,
                                                const float *pcm,
                                                uint64_t frame_count,
                                                audio_bus_handle_t bus,
                                                float gain_db) {
    if (!h) return AUDIO_NULL_HANDLE;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx || !ctx->voices) return AUDIO_NULL_HANDLE;

    uint32_t bus_idx = audio_sub_idx(bus);
    if (bus_idx >= ctx->graph->bus_count) return AUDIO_NULL_HANDLE;

    uint32_t vi = alloc_voice(ctx);
    if (vi == ctx->max_voices) return AUDIO_NULL_HANDLE; // pool exhausted

    // write directly to voice slot (main thread; device not yet reading it
    // because active=false until we flip it, which is a single store)
    audio_voice_state_t *v = &ctx->voices[vi];
    v->pcm         = pcm;
    v->frame_count = frame_count;
    v->frame_pos   = 0;
    v->bus_idx     = bus_idx;
    v->gain_linear = gain_from_db(gain_db);
    v->pan         = 0.0f;
    v->pitch       = 1.0f;
    // publish: the audio thread checks active after reading the fields above.
    // on x86/arm64 stores are sequentially consistent enough for our purpose;
    // a release store would be ideal but requires _Atomic on the struct member.
    v->active = true;

    return audio_make_sub_handle(AUDIO_TAG_VOICE, vi);
}

audio_voice_handle_t rt_audio_ctx_play_clip(audio_ctx_handle_t ctx,
                                            const float *pcm,
                                            uint64_t frame_count,
                                            audio_bus_handle_t bus,
                                            float gain_db) {
    return play_voice_internal(ctx, pcm, frame_count, bus, gain_db);
}

audio_stream_handle_t rt_audio_ctx_play_stream(audio_ctx_handle_t ctx,
                                               const float *pcm,
                                               uint64_t frame_count,
                                               audio_bus_handle_t bus,
                                               float gain_db) {
    // v0: identical to play_clip
    return (audio_stream_handle_t)play_voice_internal(
        ctx, pcm, frame_count, bus, gain_db);
}

void rt_audio_ctx_stop_voice(audio_ctx_handle_t h, audio_voice_handle_t v) {
    if (!h || !v) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx || !ctx->voices) return;
    uint32_t vi = audio_sub_idx(v);
    if (vi >= ctx->max_voices) return;
    audio_cmd_t cmd = {.type = AUDIO_CMD_STOP_VOICE, .index = vi};
    audio_ring_push(&ctx->cmd_ring, &cmd);
}

void rt_audio_ctx_set_voice_gain_db(audio_ctx_handle_t h,
                                    audio_voice_handle_t v, float gain_db) {
    if (!h || !v) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx) return;
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_VOICE_GAIN,
        .index = audio_sub_idx(v),
        .f0 = gain_from_db(gain_db),
    };
    audio_ring_push(&ctx->cmd_ring, &cmd);
}

void rt_audio_ctx_set_voice_pan(audio_ctx_handle_t h, audio_voice_handle_t v,
                                float pan) {
    if (!h || !v) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx) return;
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_VOICE_PAN, .index = audio_sub_idx(v), .f0 = pan};
    audio_ring_push(&ctx->cmd_ring, &cmd);
}

void rt_audio_ctx_set_voice_pitch(audio_ctx_handle_t h, audio_voice_handle_t v,
                                  float pitch) {
    if (!h || !v) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx) return;
    audio_cmd_t cmd = {
        .type = AUDIO_CMD_VOICE_PITCH, .index = audio_sub_idx(v), .f0 = pitch};
    audio_ring_push(&ctx->cmd_ring, &cmd);
}

void rt_audio_ctx_set_bus_gain_db(audio_ctx_handle_t h, audio_bus_handle_t bus,
                                  float gain_db) {
    if (!h) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx) return;
    audio_cmd_t cmd = {
        .type  = AUDIO_CMD_BUS_GAIN,
        .index = audio_sub_idx(bus),
        .f0    = gain_from_db(gain_db),
    };
    audio_ring_push(&ctx->cmd_ring, &cmd);
}

void rt_audio_ctx_set_bus_gain_db_ramp(audio_ctx_handle_t h,
                                       audio_bus_handle_t bus, float gain_db,
                                       uint64_t ramp_ns) {
    if (!h) return;
    audio_ctx_state_t *ctx = ctx_ptr(h);
    if (!ctx) return;

    // convert ramp_ns to samples
    uint64_t ramp_samples =
        (uint64_t)((double)ramp_ns * ctx->sample_rate / 1e9);
    if (ramp_samples == 0) ramp_samples = 1;

    audio_cmd_t cmd = {
        .type  = AUDIO_CMD_BUS_GAIN_RAMP,
        .index = audio_sub_idx(bus),
        .f0    = ctx->bus_rt[audio_sub_idx(bus)].gain_linear, // ramp from current
        .f1    = gain_from_db(gain_db),
        .u0    = ramp_samples,
    };
    audio_ring_push(&ctx->cmd_ring, &cmd);
}
