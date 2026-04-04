#include "decode.h"
#include <cstdio>
#include <cstring>

// stb_image — decode PNG/JPEG/BMP to RGBA8
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// miniaudio — decode WAV/OGG to float32 PCM
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

DecodedImage decode_image(const uint8_t *data, size_t len) {
    DecodedImage out;
    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(data, (int)len, &w, &h,
                                                   &channels, 4);
    if (!pixels) {
        fprintf(stderr, "decode_image: %s\n", stbi_failure_reason());
        return out;
    }
    out.width  = (uint32_t)w;
    out.height = (uint32_t)h;
    size_t byte_count = (size_t)w * (size_t)h * 4;
    out.pixels.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return out;
}

DecodedAudio decode_audio(const uint8_t *data, size_t len) {
    DecodedAudio out;

    ma_decoder_config cfg = ma_decoder_config_init(
        ma_format_f32, 2, 0); // stereo, native sample rate
    ma_decoder decoder;
    ma_result r = ma_decoder_init_memory(data, len, &cfg, &decoder);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "decode_audio: ma_decoder_init_memory failed (%d)\n", r);
        return out;
    }

    out.channels    = decoder.outputChannels;
    out.sample_rate = decoder.outputSampleRate;

    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);
    if (total_frames == 0) total_frames = 48000 * 60; // fallback: 60s max

    size_t buf_samples = (size_t)total_frames * out.channels;
    std::vector<float> pcm(buf_samples);

    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&decoder, pcm.data(), total_frames, &frames_read);
    ma_decoder_uninit(&decoder);

    if (frames_read == 0) return out;

    out.frame_count = (uint64_t)frames_read;
    size_t byte_count = (size_t)frames_read * out.channels * sizeof(float);
    out.samples.resize(byte_count);
    memcpy(out.samples.data(), pcm.data(), byte_count);
    return out;
}
