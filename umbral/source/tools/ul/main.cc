// asset linker: bundles processed assets into a .umpack archive.
// image files (.png/.jpg/.jpeg/.bmp) are decoded to RGBA8 at pack time.
// audio files (.wav/.ogg) are decoded to float32 stereo PCM at pack time.
// all other files are stored as raw bytes.

#include "decode.h"
#include "pack.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <llvm/Support/CommandLine.h>

namespace cl = llvm::cl;

static cl::opt<std::string> PackOut("pack", cl::desc("output .umpack path"),
                                    cl::value_desc("file"), cl::Required);
static cl::opt<bool> Compress("compress",
                              cl::desc("LZ4-compress each entry"));
static cl::list<std::string> InputFiles(cl::Positional,
                                        cl::desc("<file>..."),
                                        cl::OneOrMore);

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

static bool is_font(const char *path) {
    return ends_with(path, ".ttf") || ends_with(path, ".otf");
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
    cl::ParseCommandLineOptions(argc, argv, "umbral asset linker\n");

    // build pack_input_t entries, decoding images and audio along the way.
    // decoded data is kept alive in these vectors until pack_build finishes.
    struct DecodedEntry {
        std::vector<uint8_t> data;
        pack_input_t input;
    };
    std::vector<DecodedEntry> decoded_entries;
    decoded_entries.reserve(InputFiles.size());

    for (const auto &in_str : InputFiles) {
        const char *in = in_str.c_str();
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
        } else if (is_font(in)) {
            auto raw = read_file(in);
            if (raw.empty()) {
                fprintf(stderr, "ul: cannot read %s\n", in);
                return 1;
            }
            auto fnt = decode_font(raw.data(), raw.size());
            if (fnt.data.empty()) {
                fprintf(stderr, "ul: failed to bake font atlas %s\n", in);
                return 1;
            }
            fprintf(stderr, "ul: baked font %s → %ux%u atlas, %u glyphs, %.0f em\n",
                    in, fnt.atlas_w, fnt.atlas_h, fnt.glyph_count, fnt.em_size);
            de.input.meta_type = UMPACK_META_FONT;
            de.input.meta[0]   = fnt.atlas_w;
            de.input.meta[1]   = fnt.atlas_h;
            de.input.meta[2]   = fnt.glyph_count;
            uint32_t em_bits;
            memcpy(&em_bits, &fnt.em_size, sizeof(em_bits));
            de.input.meta[3]   = em_bits;
            de.data = std::move(fnt.data);
            de.input.decoded_data = de.data.data();
            de.input.decoded_len  = (uint32_t)de.data.size();
        }

        decoded_entries.push_back(std::move(de));
    }

    // build the pack_input_t array (pointers into decoded_entries)
    std::vector<pack_input_t> pi;
    pi.reserve(decoded_entries.size());
    for (auto &de : decoded_entries) pi.push_back(de.input);

    return pack_build(pi.data(), (uint32_t)pi.size(), PackOut.c_str(),
                      Compress ? 1 : 0) == 0
               ? 0 : 1;
}
