// write a .umsh unified shader bundle from in-memory SPIR-V words + reflection.

#include <um/shader/umsh_emit.h>

#include <common/bin_io.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace um::shader {

// align x up to a multiple of align (must be power-of-two).
static uint64_t align_up(uint64_t x, uint64_t a) { return (x + (a - 1)) & ~(a - 1); }

bool emit_umsh(std::string_view out_dir,
               std::string_view shader_name,
               std::span<const UmshStageData> stages,
               const UmshReflData *refl) {
  // STAGES section ─────────────────────────────────────────────────────────
  // layout inside STAGES data:
  //   u32 stage_count
  //   stage_count × { u8 kind, u8[3] pad, u32 spv_offset, u32 spv_size }
  //   SPIR-V payloads (concatenated, 4-byte aligned)

  const uint32_t stage_count    = static_cast<uint32_t>(stages.size());
  const uint32_t record_table   = 4 + stage_count * 12; // u32 + N×12
  uint32_t spv_cursor = record_table; // running offset within STAGES data
  std::vector<uint32_t> spv_offsets;
  spv_offsets.reserve(stage_count);
  for (const auto &s : stages) {
    spv_offsets.push_back(spv_cursor);
    uint32_t spv_bytes = static_cast<uint32_t>(s.spv_words.size() * 4);
    // each payload is 4-byte aligned (SPIR-V is always 4-byte-aligned by definition)
    spv_cursor += (spv_bytes + 3) & ~3u;
  }
  uint32_t stages_data_size = spv_cursor;

  // REFL section ────────────────────────────────────────────────────────────
  // layout: u32 stride, u32 input_rate, u32 attr_count, attr_count×12
  uint32_t refl_data_size = 0;
  if (refl) {
    refl_data_size = 4 + 4 + 4 + static_cast<uint32_t>(refl->attrs.size()) * 12;
  }

  // compute section table + total file size
  // header: 24 bytes, sections: 24 bytes each, data: 8-byte aligned
  const uint32_t n_sections = refl ? 2u : 1u;
  uint64_t header_size = 24 + static_cast<uint64_t>(n_sections) * 24; // header + section table

  uint64_t stages_offset = align_up(header_size, 8);
  uint64_t refl_offset   = refl ? align_up(stages_offset + stages_data_size, 8) : 0;
  uint64_t total_bytes   = refl ? align_up(refl_offset + refl_data_size, 8)
                                : align_up(stages_offset + stages_data_size, 8);

  BinWriter w;

  // umsh_header_t (24 bytes)
  w.u32(0x554D5348u); // UMSH magic
  w.u16(1);           // version
  w.u16(0x1234);      // LE sentinel
  w.u32(0);           // flags
  w.u32(n_sections);
  w.u32(static_cast<uint32_t>(total_bytes));
  w.u32(0);           // _pad

  // section table (24 bytes each)
  // STAGES section
  w.u32(1u);          // id = STAGES
  w.u32(0);           // flags
  w.u64(stages_offset);
  w.u64(stages_data_size);

  if (refl) {
    // REFL section
    w.u32(2u);
    w.u32(0);
    w.u64(refl_offset);
    w.u64(refl_data_size);
  }

  // pad to stages_offset
  while (w.buf.size() < stages_offset) w.u8(0);

  // STAGES data ─────────────────────────────────────────────────────────────
  w.u32(stage_count);
  for (uint32_t i = 0; i < stage_count; ++i) {
    uint32_t spv_bytes = static_cast<uint32_t>(stages[i].spv_words.size() * 4);
    w.u8(stages[i].kind);
    w.u8(0); w.u8(0); w.u8(0); // _pad[3]
    w.u32(spv_offsets[i]);
    w.u32(spv_bytes);
  }
  for (const auto &s : stages) {
    uint32_t spv_bytes = static_cast<uint32_t>(s.spv_words.size() * 4);
    w.bytes(reinterpret_cast<const uint8_t *>(s.spv_words.data()), spv_bytes);
    // 4-byte align pad (already aligned; SPIR-V is always aligned, but be safe)
    uint32_t rem = spv_bytes & 3u;
    for (uint32_t p = 0; p < (rem ? 4 - rem : 0); ++p) w.u8(0);
  }

  if (refl) {
    // pad to refl_offset
    while (w.buf.size() < refl_offset) w.u8(0);

    // REFL data
    w.u32(refl->stride);
    w.u32(refl->input_rate);
    w.u32(static_cast<uint32_t>(refl->attrs.size()));
    for (const auto &a : refl->attrs) {
      w.u32(a.location);
      w.u32(a.vk_format);
      w.u32(a.offset);
    }
  }

  // pad to total_bytes
  while (w.buf.size() < total_bytes) w.u8(0);

  std::string path = std::string(out_dir) + "/" + std::string(shader_name) + ".umsh";
  if (!w.write_file(path.c_str())) {
    fprintf(stderr, "umsh_emit: failed to write '%s'\n", path.c_str());
    return false;
  }
  return true;
}

} // namespace um::shader
