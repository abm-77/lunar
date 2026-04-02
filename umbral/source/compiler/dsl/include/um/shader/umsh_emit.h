#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace um::shader {

struct UmshStageData {
  uint8_t kind; // 0 = vertex, 1 = fragment (UMSH_STAGE_*)
  std::vector<uint32_t> spv_words;
};

struct UmshAttr {
  uint32_t location;
  uint32_t vk_format;
  uint32_t offset;
};

struct UmshReflData {
  uint32_t stride;
  uint32_t input_rate; // 0 = per-vertex
  std::vector<UmshAttr> attrs;
};

// emit a .umsh file to <out_dir>/<shader_name>.umsh.
// refl may be nullptr if the shader has no @vs_in field.
// returns false on failure (prints to stderr).
bool emit_umsh(std::string_view out_dir,
               std::string_view shader_name,
               std::span<const UmshStageData> stages,
               const UmshReflData *refl);

} // namespace um::shader
