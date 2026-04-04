#pragma once
#include <cstdint>
#include <vector>

// build-time asset decoding for the asset linker.
// images are decoded to RGBA8; audio to float32 stereo PCM.

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

// returns empty pixels on failure
DecodedImage decode_image(const uint8_t *data, size_t len);

// returns empty samples on failure; output is always stereo float32
DecodedAudio decode_audio(const uint8_t *data, size_t len);
