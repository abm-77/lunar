#pragma once

// clang-format off
// .umshaders binary sidecar format.
// written by the compiler alongside the object file; read by the `ul` asset linker.
// all integers are little-endian; all strings are u32 byte-length + UTF-8 (no null terminator).
//
// on-disk layout:
//   u32    magic          = 0x554D5348 ("UMSH")
//   u16    version        = 1
//   u32    module_name    (string)
//   u32    pod_count
//   [each @shader_pod]:
//     string  name
//     u32     field_count
//     [each field]:
//       string  field_name
//       u8      io_kind        (0=none, 1=location, 2=builtin_position)
//       u32     location_index (valid when io_kind == 1)
//       u32     byte_offset
//       string  field_type_name
//   u32    shader_count
//   [each @shader struct]:
//     string  name
//     u32     annot_count
//     [each field annotation]:
//       string  field_name
//       u8      shader_field_kind (0=none,1=vs_in,2=vs_out,3=fs_in,4=fs_out,5=draw_data)
//       string  type_name
//   u32    stage_count
//   [each @stage method]:
//     string  shader_type_name
//     string  method_name
//     u8      stage_kind         (0=vertex, 1=fragment)
//   [serialized BodyIR — see shader_emit.h serialize_body_ir / shader_link.cc read_body]:
//     u32     body_root          (NodeId of the method body Block in node array)
//     u32     node_count
//     [each node — 22 bytes]: u16 kind, u32 span_s, u32 span_e, u32 a, u32 b, u32 c
//     u32     list_count         [u32 each]
//     u32     fors_count         [ForPayload: 4×u32 each]
//     u32     array_lits_count   [ArrayLitPayload: 4×u32 each]
//     u32     float_lits_count   [f64 each]
//     u32     sym_count          [sym_id: u32, name: string]
// clang-format on

#include <common/bin_io.h>

#define UMSHADERS_MAGIC 0x554D5348u // "UMSH"
#define UMSHADERS_VERSION 1u

using UmshadersWriter = BinWriter;
