#pragma once
#include "compiler/frontend/ast.h"
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

enum IOKind : u8 {
  None,
  Location,
  Position,
};

struct PodField {
  std::string name;
  uint8_t io_kind; // 0=none, 1=location, 2=builtin_position
  uint32_t location_index;
  uint32_t byte_offset;
  std::string type_name; // e.g. "vec2", "f32"
};

struct PodType {
  std::string name;
  std::vector<PodField> fields;
};

struct ShaderAnnot {
  std::string field_name;
  uint8_t
      shader_field_kind; // 1=vs_in, 2=vs_out, 3=fs_in, 4=fs_out, 5=draw_data
  std::string pod_type_name;
};

struct ShaderType {
  std::string name;
  std::vector<ShaderAnnot> annots;
};

struct BodyNode {
  uint16_t kind;
  uint32_t span_s, span_e;
  uint32_t a, b, c;
};

struct StageBody {
  std::vector<BodyNode> nodes;
  std::vector<uint32_t> list;
  std::vector<ForPayload> fors;
  std::vector<ArrayLitPayload> array_lits;
  std::vector<double> float_lits;
  uint32_t body_root = 0; // NodeId of the method body Block in nodes
  std::unordered_map<uint32_t, std::string>
      sym_names; // SymId → interned name string
};

struct StageInfo {
  std::string shader_type_name;
  std::string method_name;
  uint8_t stage_kind; // 0=vertex, 1=fragment
  StageBody body;
};

struct Sidecar {
  std::string module_name;
  std::vector<PodType> pods;
  std::vector<ShaderType> shaders;
  std::vector<StageInfo> stages;
};

// shader_link — process a .umshaders sidecar file into SPIR-V + UMRF.
//   sidecar_path: null-terminated path to the .umshaders file
//   out_dir:      null-terminated output directory (no trailing slash)
//   returns 0 on success; non-zero on error (prints reason to stderr)
//
// produces:
//   <out_dir>/<shader_name>_vs.spv   vertex SPIR-V binary
//   <out_dir>/<shader_name>_fs.spv   fragment SPIR-V binary
//   <out_dir>/<shader_name>.umrf     UMRF vertex reflection blob
int shader_link(const char *sidecar_path, const char *out_dir);

// shader_link_dump_ir — read a .umshaders sidecar and print the SPIR-V-targeted
//   LLVM IR for each stage to stdout. intended for lit tests via FileCheck.
//   returns 0 on success; non-zero on error
int shader_link_dump_ir(const char *sidecar_path);
