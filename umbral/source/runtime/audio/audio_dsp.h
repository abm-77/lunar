#pragma once
// audio_dsp.h — inline DSP algorithms: limiter, lowpass, compressor.
// all functions operate on interleaved stereo float32 buffers.
// called only from the audio callback thread; no allocation, no locks.

#include <audio_internal.h>
#include <math.h>

// db_to_linear — convert dB to linear amplitude; clamped at -120 dB floor.
static inline float db_to_linear(float db) {
    if (db <= -120.0f) return 0.0f;
    return powf(10.0f, db * 0.05f);
}

// linear_to_db — convert linear amplitude to dB; clamped at -120 dB floor.
static inline float linear_to_db(float lin) {
    if (lin <= 1e-6f) return -120.0f;
    return 20.0f * log10f(lin);
}

// smooth_coeff — one-pole smoothing coefficient for a given time constant.
// time_secs: smoothing time in seconds; sample_rate: samples per second.
static inline float smooth_coeff(float time_secs, uint32_t sample_rate) {
    if (time_secs <= 0.0f || sample_rate == 0) return 1.0f;
    return 1.0f - expf(-1.0f / (time_secs * (float)sample_rate));
}

// ---- peak limiter ----
// threshold: linear amplitude ceiling (e.g. 0.98f for near-full-scale).
// attack/release: smoothing time constants in seconds.
// processes buf in-place (interleaved stereo, frame_count frames).

static inline void dsp_limiter_process(audio_limiter_state_t *st,
                                       float *buf, uint32_t frame_count,
                                       uint32_t sample_rate) {
    const float threshold  = 0.98f;
    const float atk  = smooth_coeff(0.001f, sample_rate);  // 1 ms attack
    const float rel  = smooth_coeff(0.100f, sample_rate);  // 100 ms release

    for (uint32_t i = 0; i < frame_count; ++i) {
        float l = buf[i * 2 + 0];
        float r = buf[i * 2 + 1];
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);

        float target = (peak > threshold) ? (threshold / peak) : 1.0f;
        float coeff  = (target < st->gain_env) ? atk : rel;
        st->gain_env += coeff * (target - st->gain_env);

        buf[i * 2 + 0] = l * st->gain_env;
        buf[i * 2 + 1] = r * st->gain_env;
    }
}

// ---- 1-pole IIR lowpass ----
// alpha is pre-computed from cutoff_hz and sample_rate at context creation.
// y[n] = alpha*x[n] + (1-alpha)*y[n-1]

static inline void dsp_lowpass_process(audio_lowpass_state_t *st,
                                       float *buf, uint32_t frame_count) {
    const float a   = st->alpha;
    const float inv = 1.0f - a;
    float yl = st->y_prev_l;
    float yr = st->y_prev_r;

    for (uint32_t i = 0; i < frame_count; ++i) {
        yl = a * buf[i * 2 + 0] + inv * yl;
        yr = a * buf[i * 2 + 1] + inv * yr;
        buf[i * 2 + 0] = yl;
        buf[i * 2 + 1] = yr;
    }

    st->y_prev_l = yl;
    st->y_prev_r = yr;
}

// dsp_lowpass_alpha — compute the filter coefficient from cutoff_hz.
// call once at context creation and store in audio_lowpass_state_t.alpha.
static inline float dsp_lowpass_alpha(float cutoff_hz, uint32_t sample_rate) {
    if (cutoff_hz <= 0.0f || sample_rate == 0) return 1.0f;
    const float dt = 1.0f / (float)sample_rate;
    const float rc = 1.0f / (6.2831853f * cutoff_hz); // 2π·fc
    return dt / (dt + rc);
}

// ---- feed-forward RMS compressor ----
// threshold_db: level above which compression kicks in.
// ratio: compression ratio (>= 1.0; higher = more squashing).
// processes an entire block to compute block RMS, then applies gain.

static inline void dsp_compressor_process(audio_compressor_state_t *st,
                                          float *buf, uint32_t frame_count,
                                          float threshold_db, float ratio,
                                          uint32_t sample_rate) {
    const float atk = smooth_coeff(0.005f, sample_rate); // 5 ms attack
    const float rel = smooth_coeff(0.200f, sample_rate); // 200 ms release

    // block RMS across both channels
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < frame_count * 2; ++i)
        sum_sq += buf[i] * buf[i];
    float rms = sqrtf(sum_sq / (float)(frame_count * 2));
    float rms_db = linear_to_db(rms);

    // gain computation
    float target_db;
    if (rms_db > threshold_db)
        target_db = threshold_db + (rms_db - threshold_db) / ratio;
    else
        target_db = rms_db;

    float gain_db = rms_db - target_db; // positive = attenuation
    float coeff   = (gain_db > st->gain_db_prev) ? atk : rel;
    st->gain_db_prev += coeff * (gain_db - st->gain_db_prev);
    float gain_linear = db_to_linear(-st->gain_db_prev);

    for (uint32_t i = 0; i < frame_count * 2; ++i)
        buf[i] *= gain_linear;
}
