#include <audio_dsp.h>
#include <audio_internal.h>
#include <string.h>

// audio_process_block — real-time audio mixing called from the miniaudio callback.
// output: caller-provided float32 interleaved stereo buffer of frame_count frames.
// RT safety rules: no heap allocation, no locks, no file I/O.

void audio_process_block(audio_ctx_state_t *ctx, float *output,
                         uint32_t frame_count) {
    const audio_graph_state_t *g = ctx->graph;

    // drain command ring
    audio_cmd_t cmd;
    while (audio_ring_pop(&ctx->cmd_ring, &cmd)) {
        switch (cmd.type) {
        case AUDIO_CMD_STOP_VOICE:
            if (cmd.index < ctx->max_voices)
                ctx->voices[cmd.index].active = false;
            break;
        case AUDIO_CMD_VOICE_GAIN:
            if (cmd.index < ctx->max_voices)
                ctx->voices[cmd.index].gain_linear = cmd.f0;
            break;
        case AUDIO_CMD_VOICE_PAN:
            if (cmd.index < ctx->max_voices)
                ctx->voices[cmd.index].pan = cmd.f0;
            break;
        case AUDIO_CMD_VOICE_PITCH:
            if (cmd.index < ctx->max_voices)
                ctx->voices[cmd.index].pitch = cmd.f0;
            break;
        case AUDIO_CMD_BUS_GAIN:
            if (cmd.index < g->bus_count) {
                ctx->bus_rt[cmd.index].gain_linear      = cmd.f0;
                ctx->bus_rt[cmd.index].ramp_samples_left = 0;
            }
            break;
        case AUDIO_CMD_BUS_GAIN_RAMP:
            if (cmd.index < g->bus_count) {
                ctx->bus_rt[cmd.index].gain_linear       = cmd.f0; // start
                ctx->bus_rt[cmd.index].ramp_target       = cmd.f1; // end
                ctx->bus_rt[cmd.index].ramp_samples_left = cmd.u0;
            }
            break;
        default:
            break;
        }
    }

    // zero all bus mix buffers for this block
    uint32_t buf_stride = frame_count * AUDIO_CHANNELS;
    for (uint32_t b = 0; b < g->bus_count; ++b) {
        float *bus_buf = ctx->mix_bufs + (uint64_t)b * ctx->block_frames * AUDIO_CHANNELS;
        memset(bus_buf, 0, sizeof(float) * buf_stride);
    }

    // accumulate voices into their target bus buffers
    for (uint32_t vi = 0; vi < ctx->max_voices; ++vi) {
        audio_voice_state_t *v = &ctx->voices[vi];
        if (!v->active) continue;

        uint32_t frames_left = (uint32_t)(v->frame_count - v->frame_pos);
        uint32_t frames_to_mix = frames_left < frame_count
                                     ? frames_left
                                     : frame_count;

        if (frames_to_mix == 0) {
            v->active = false;
            continue;
        }

        float *dst = ctx->mix_bufs +
                     (uint64_t)v->bus_idx * ctx->block_frames * AUDIO_CHANNELS;
        const float *src = v->pcm + v->frame_pos * AUDIO_CHANNELS;

        // pan: left_gain = cos(pan_angle), right_gain = sin(pan_angle)
        // simplified: equal-power panning between [-1, 1]
        float pan   = v->pan < -1.0f ? -1.0f : (v->pan > 1.0f ? 1.0f : v->pan);
        float norm  = (pan + 1.0f) * 0.5f; // 0..1
        // use linear panning for simplicity (v0)
        float lg = v->gain_linear * (1.0f - norm * 0.5f);
        float rg = v->gain_linear * (0.5f + norm * 0.5f);

        for (uint32_t f = 0; f < frames_to_mix; ++f) {
            dst[f * 2 + 0] += src[f * 2 + 0] * lg;
            dst[f * 2 + 1] += src[f * 2 + 1] * rg;
        }

        v->frame_pos += frames_to_mix;
        if (v->frame_pos >= v->frame_count) v->active = false;
    }

    // process buses in topological order (leaves first, output last)
    for (uint32_t oi = 0; oi < g->bus_count; ++oi) {
        uint32_t bi = g->bus_order[oi];
        float *bus_buf =
            ctx->mix_bufs + (uint64_t)bi * ctx->block_frames * AUDIO_CHANNELS;

        // apply bus gain (with optional ramp)
        audio_bus_rt_state_t *brt = &ctx->bus_rt[bi];
        if (brt->ramp_samples_left > 0) {
            // per-sample interpolation of bus gain over remaining ramp
            uint32_t ramp_frames =
                brt->ramp_samples_left < frame_count
                    ? (uint32_t)brt->ramp_samples_left
                    : frame_count;
            float step = (brt->ramp_target - brt->gain_linear) /
                         (float)brt->ramp_samples_left;
            for (uint32_t f = 0; f < ramp_frames; ++f) {
                brt->gain_linear += step;
                bus_buf[f * 2 + 0] *= brt->gain_linear;
                bus_buf[f * 2 + 1] *= brt->gain_linear;
            }
            // remaining frames after ramp finishes
            for (uint32_t f = ramp_frames; f < frame_count; ++f) {
                bus_buf[f * 2 + 0] *= brt->ramp_target;
                bus_buf[f * 2 + 1] *= brt->ramp_target;
            }
            brt->ramp_samples_left =
                brt->ramp_samples_left > frame_count
                    ? brt->ramp_samples_left - frame_count
                    : 0;
            if (brt->ramp_samples_left == 0)
                brt->gain_linear = brt->ramp_target;
        } else {
            float gain = brt->gain_linear;
            if (gain != 1.0f) {
                for (uint32_t f = 0; f < frame_count * AUDIO_CHANNELS; ++f)
                    bus_buf[f] *= gain;
            }
        }

        // apply effects for this bus in definition order
        for (uint32_t ei = 0; ei < g->effect_count; ++ei) {
            const audio_effect_def_t *ef = &g->effects[ei];
            if (ef->bus_idx != bi) continue;
            audio_effect_state_t *es = &ctx->effect_state[ei];
            switch (ef->type) {
            case AUDIO_EFFECT_LIMITER:
                dsp_limiter_process(&es->limiter, bus_buf, frame_count,
                                    ctx->sample_rate);
                break;
            case AUDIO_EFFECT_LOWPASS:
                dsp_lowpass_process(&es->lowpass, bus_buf, frame_count);
                break;
            case AUDIO_EFFECT_COMPRESSOR:
                dsp_compressor_process(&es->compressor, bus_buf, frame_count,
                                       ef->threshold_db, ef->ratio,
                                       ctx->sample_rate);
                break;
            default:
                break;
            }
        }

        // accumulate this bus into all downstream buses (routes)
        for (uint32_t ri = 0; ri < g->route_count; ++ri) {
            const audio_compiled_route_t *rt_r = &g->routes[ri];
            if (rt_r->from_idx != bi) continue;
            float *dst_buf = ctx->mix_bufs +
                             (uint64_t)rt_r->to_idx * ctx->block_frames *
                                 AUDIO_CHANNELS;
            float route_gain = rt_r->gain;
            for (uint32_t f = 0; f < frame_count * AUDIO_CHANNELS; ++f)
                dst_buf[f] += bus_buf[f] * route_gain;
        }
    }

    // copy master (output) bus to the device output buffer
    uint32_t out_bi = g->output_bus_idx;
    float *out_buf =
        ctx->mix_bufs + (uint64_t)out_bi * ctx->block_frames * AUDIO_CHANNELS;
    memcpy(output, out_buf, sizeof(float) * frame_count * AUDIO_CHANNELS);
}
