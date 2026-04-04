// asset linker: bundles processed assets into a .umpack archive.
//   --pack <out.umpack> -- bundle all input files into a .umpack
//   --compress          -- LZ4-compress each entry when smaller (default for decoded assets)
// image files (.png/.jpg/.jpeg/.bmp) are decoded to RGBA8 at pack time.
// audio files (.wav/.ogg) are decoded to float32 stereo PCM at pack time.
// all other files are stored as raw bytes.

#include "decode.h"
#include "pack.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return false;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static bool is_image(const char *path) {
    return ends_with(path, ".png") || ends_with(path, ".jpg") ||
           ends_with(path, ".jpeg") || ends_with(path, ".bmp");
}

static bool is_audio(const char *path) {
    return ends_with(path, ".wav") || ends_with(path, ".ogg");
}

static std::vector<uint8_t> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::vector<uint8_t> buf(sz > 0 ? (size_t)sz : 0u);
    if (sz > 0) fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *pack_out = nullptr;
    int compress = 0;
    std::vector<const char *> inputs;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack_out = argv[++i];
        else if (!strcmp(argv[i], "--compress"))
            compress = 1;
        else
            inputs.push_back(argv[i]);
    }

    if (inputs.empty() || !pack_out) {
        fprintf(stderr,
                "usage: ul --pack <out.umpack> [--compress] <file>...\n");
        return 1;
    }

    // compression is opt-in via --compress for now

    // build pack_input_t entries, decoding images and audio along the way.
    // decoded data is kept alive in these vectors until pack_build finishes.
    struct DecodedEntry {
        std::vector<uint8_t> data;
        pack_input_t input;
    };
    std::vector<DecodedEntry> decoded_entries;
    decoded_entries.reserve(inputs.size());

    for (const char *in : inputs) {
        DecodedEntry de;
        de.input.path = in;
        de.input.decoded_data = nullptr;
        de.input.decoded_len  = 0;
        de.input.meta_type    = UMPACK_META_RAW;
        memset(de.input.meta, 0, sizeof(de.input.meta));

        if (is_image(in)) {
            auto raw = read_file(in);
            if (raw.empty()) {
                fprintf(stderr, "ul: cannot read %s\n", in);
                return 1;
            }
            auto img = decode_image(raw.data(), raw.size());
            if (img.pixels.empty()) {
                fprintf(stderr, "ul: failed to decode image %s\n", in);
                return 1;
            }
            fprintf(stderr, "ul: decoded %s → %ux%u RGBA8 (%zu bytes)\n",
                    in, img.width, img.height, img.pixels.size());
            de.input.meta_type = UMPACK_META_IMAGE;
            de.input.meta[0]   = img.width;
            de.input.meta[1]   = img.height;
            de.input.meta[2]   = 4; // channels
            de.input.meta[3]   = 0;
            de.data = std::move(img.pixels);
            de.input.decoded_data = de.data.data();
            de.input.decoded_len  = (uint32_t)de.data.size();
        } else if (is_audio(in)) {
            auto raw = read_file(in);
            if (raw.empty()) {
                fprintf(stderr, "ul: cannot read %s\n", in);
                return 1;
            }
            auto aud = decode_audio(raw.data(), raw.size());
            if (aud.samples.empty()) {
                fprintf(stderr, "ul: failed to decode audio %s\n", in);
                return 1;
            }
            fprintf(stderr, "ul: decoded %s → %lu frames, %uch, %u Hz (%zu bytes)\n",
                    in, (unsigned long)aud.frame_count, aud.channels,
                    aud.sample_rate, aud.samples.size());
            de.input.meta_type = UMPACK_META_AUDIO;
            de.input.meta[0]   = (uint32_t)(aud.frame_count & 0xFFFFFFFFu);
            de.input.meta[1]   = (uint32_t)(aud.frame_count >> 32);
            de.input.meta[2]   = aud.channels;
            de.input.meta[3]   = aud.sample_rate;
            de.data = std::move(aud.samples);
            de.input.decoded_data = de.data.data();
            de.input.decoded_len  = (uint32_t)de.data.size();
        }

        decoded_entries.push_back(std::move(de));
    }

    // build the pack_input_t array (pointers into decoded_entries)
    std::vector<pack_input_t> pi;
    pi.reserve(decoded_entries.size());
    for (auto &de : decoded_entries) pi.push_back(de.input);

    return pack_build(pi.data(), (uint32_t)pi.size(), pack_out, compress) == 0
               ? 0 : 1;
}
