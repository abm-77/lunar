#pragma once
#include <cstdint>
#include <vector>

// build-time asset decoding for the asset linker.
// images are decoded to RGBA8; audio to float32 stereo PCM.
// fonts are baked to an MTSDF atlas + glyph metrics table.

struct DecodedImage {
  std::vector<uint8_t> pixels; // RGBA8, width * height * 4 bytes
  uint32_t width  = 0;
  uint32_t height = 0;
};

struct DecodedAudio {
  std::vector<uint8_t> samples; // float32 interleaved PCM as raw bytes
  uint64_t frame_count  = 0;
  uint32_t channels     = 0;
  uint32_t sample_rate  = 0;
};

// per-glyph metrics stored in the umpack font entry after the atlas pixels.
// layout must match font_glyph_metrics_t in runtime/font/font.h exactly.
struct PackedGlyphMetrics {
  uint32_t codepoint;
  float uv_x0, uv_y0, uv_x1, uv_y1;
  float bearing_x, bearing_y;
  float advance;
  float width, height;
};

struct DecodedFont {
  std::vector<uint8_t> data;   // atlas pixels (RGBA8) followed by glyph metrics table
  uint32_t atlas_w  = 0;
  uint32_t atlas_h  = 0;
  uint32_t glyph_count = 0;
  float    em_size  = 0.0f;
};

// returns empty pixels on failure
DecodedImage decode_image(const uint8_t *data, size_t len);

// returns empty samples on failure; output is always stereo float32
DecodedAudio decode_audio(const uint8_t *data, size_t len);

// bake TTF/OTF bytes into an MTSDF atlas + glyph metrics table.
// bakes ASCII printable (32–126) at the given em_size into an atlas that auto-sizes.
// returns empty data on failure.
DecodedFont decode_font(const uint8_t *data, size_t len,
                        float em_size = 72.0f,
                        uint32_t atlas_w = 1024, uint32_t atlas_h = 512);
