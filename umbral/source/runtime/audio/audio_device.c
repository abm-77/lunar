// audio_device.c — miniaudio device glue.
// this is the ONLY file that defines MINIAUDIO_IMPLEMENTATION; all other files
// include miniaudio.h for type definitions only.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <audio_internal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

// audio_ma_callback — miniaudio data callback, called on the audio thread.
// bridges miniaudio's callback signature to audio_process_block.
static void audio_ma_callback(ma_device *device, void *output,
                               const void *input, ma_uint32 frame_count) {
    (void)input;
    audio_ctx_state_t *ctx = (audio_ctx_state_t *)device->pUserData;
    if (!ctx || !ctx->mix_bufs) {
        memset(output, 0, sizeof(float) * frame_count * AUDIO_CHANNELS);
        return;
    }
    // clamp to preallocated block size to avoid buffer overrun
    uint32_t frames = frame_count < ctx->block_frames
                          ? (uint32_t)frame_count
                          : ctx->block_frames;
    audio_process_block(ctx, (float *)output, frames);
    // if miniaudio asked for more frames than our block, zero the remainder
    if (frames < frame_count) {
        float *out = (float *)output;
        memset(out + (uint64_t)frames * AUDIO_CHANNELS, 0,
               sizeof(float) * (frame_count - frames) * AUDIO_CHANNELS);
    }
}

void audio_device_open(audio_ctx_state_t *ctx) {
    // allocate the ma_device struct via rt_alloc so it is tracked
    uint64_t dh = rt_alloc(sizeof(ma_device), _Alignof(ma_device), AUDIO_ALLOC_TAG, 0);
    if (!dh) {
#if UM_DEBUG
        fprintf(stderr, "audio: failed to allocate ma_device\n");
#endif
        return;
    }
    um_slice_u8_t ds = rt_slice_from_alloc(dh, 1, 1, sizeof(ma_device), 0, 1);
    ma_device *dev   = (ma_device *)ds.ptr;
    memset(dev, 0, sizeof(*dev));

    ctx->device_handle = dh;
    ctx->device        = dev;

    ma_device_config cfg     = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format      = ma_format_f32;
    cfg.playback.channels    = AUDIO_CHANNELS;
    cfg.sampleRate           = ctx->sample_rate;
    cfg.dataCallback         = audio_ma_callback;
    cfg.pUserData            = ctx;
    cfg.periodSizeInFrames   = ctx->block_frames;

    ma_result result = ma_device_init(NULL, &cfg, dev);
    if (result != MA_SUCCESS) {
#if UM_DEBUG
        fprintf(stderr, "audio: ma_device_init failed (%d)\n", result);
#endif
        rt_free(dh, 0);
        ctx->device_handle = 0;
        ctx->device        = NULL;
        return;
    }

    result = ma_device_start(dev);
    if (result != MA_SUCCESS) {
#if UM_DEBUG
        fprintf(stderr, "audio: ma_device_start failed (%d)\n", result);
#endif
        ma_device_uninit(dev);
        rt_free(dh, 0);
        ctx->device_handle = 0;
        ctx->device        = NULL;
        return;
    }

    ctx->device_started = true;
}

void audio_device_close(audio_ctx_state_t *ctx) {
    if (!ctx->device) return;
    ma_device_stop(ctx->device);
    ma_device_uninit(ctx->device);
    ctx->device         = NULL;
    ctx->device_started = false;
    // device_handle freed by rt_audio_ctx_destroy after this call
}
