#include <gtest/gtest.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/lexer.h"
#include "compiler/shader/umshaders.h"
#include "shader_link.h"
#include "spirv_emit.h"

#include <cstdio>

static bool has_spv_magic(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint32_t magic = 0;
  bool ok = fread(&magic, 4, 1, f) == 1;
  fclose(f);
  return ok && magic == 0x07230203u;
}

static bool file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static bool write_buf(const UmshadersWriter &w, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fwrite(w.buf.data(), 1, w.buf.size(), f);
  fclose(f);
  return true;
}

// build a minimal Sidecar: pod "V" with a vec4 "pos" (@vs_in @location 0) and
// vec4 "clip" (@vs_out @builtin position); shader "S" with vin/vout annots.
static Sidecar make_sprite_sidecar() {
  Sidecar sc;
  sc.module_name = "sprite";

  PodType pod;
  pod.name = "V";
  pod.fields.push_back({.name = "pos",
                        .io_kind = IOKind::Location,
                        .location_index = 0,
                        .byte_offset = 0,
                        .type_name = "vec4"});
  pod.fields.push_back({.name = "clip",
                        .io_kind = IOKind::Position,
                        .location_index = 0,
                        .byte_offset = 16,
                        .type_name = "vec4"});
  sc.pods.push_back(pod);

  ShaderType sh;
  sh.name = "S";
  sh.annots.push_back({.field_name = "vin",
                       .shader_field_kind = 1, // vs_in
                       .pod_type_name = "V"});
  sh.annots.push_back({.field_name = "vout",
                       .shader_field_kind = 2, // vs_out
                       .pod_type_name = "V"});
  sc.shaders.push_back(sh);
  return sc;
}

// write a valid minimal .umshaders file using UmshadersWriter (the real
// writer). one pod "P" with a vs_in Location field; one shader "S" with a vin
// annot; one vertex stage with an empty body (body_root=0).
static void write_minimal_sidecar(UmshadersWriter &w) {
  w.u32(UMSHADERS_MAGIC);
  w.u16(UMSHADERS_VERSION);
  w.str("test");
  // pod "P": one vs_in field "pos"
  w.u32(1);
  w.str("P");
  w.u32(1);
  w.str("pos");
  w.u8(1 /*Location*/);
  w.u32(0);
  w.u32(0);
  w.str("vec4");
  // shader "S": one vs_in annot
  w.u32(1);
  w.str("S");
  w.u32(1);
  w.str("vin");
  w.u8(1 /*vs_in*/);
  w.str("P");
  // zero shader_fn helpers
  w.u32(0);
  // one vertex stage with empty body (body_root=0, all counts=0)
  w.u32(1);
  w.str("S");
  w.str("vs");
  w.u8(0 /*vertex*/);
  w.u32(0); // body_root
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0); // counts
}

class SpirvEmitTest : public ::testing::Test {
  static bool s_initialized;

public:
  static void SetUpTestSuite() {
    if (!s_initialized) {
      spirv_init_target();
      s_initialized = true;
    }
  }
};
bool SpirvEmitTest::s_initialized = false;

// empty body (body_root=0) → bare RetVoid → valid SPIR-V magic
TEST_F(SpirvEmitTest, EmptyBodyVertex) {
  Sidecar sc = make_sprite_sidecar();
  StageInfo stage;
  stage.shader_type_name = "S";
  stage.method_name = "vs";
  stage.stage_kind = 0;

  const char *path = "/tmp/um_test_empty_vs.spv";
  remove(path);
  ASSERT_EQ(spirv_emit_stage(sc, stage, path), 0);
  EXPECT_TRUE(has_spv_magic(path)) << "not valid SPIR-V";
  remove(path);
}

// fragment stage (stage_kind=1) with empty body
TEST_F(SpirvEmitTest, EmptyBodyFragment) {
  Sidecar sc;
  sc.module_name = "frag";
  PodType pod;
  pod.name = "FV";
  pod.fields.push_back({.name = "color",
                        .io_kind = IOKind::Location,
                        .location_index = 0,
                        .byte_offset = 0,
                        .type_name = "vec4"});
  sc.pods.push_back(pod);
  ShaderType sh;
  sh.name = "F";
  sh.annots.push_back(
      {.field_name = "fout", .shader_field_kind = 4, .pod_type_name = "FV"});
  sc.shaders.push_back(sh);

  StageInfo stage;
  stage.shader_type_name = "F";
  stage.method_name = "fs";
  stage.stage_kind = 1;

  const char *path = "/tmp/um_test_empty_fs.spv";
  remove(path);
  ASSERT_EQ(spirv_emit_stage(sc, stage, path), 0);
  EXPECT_TRUE(has_spv_magic(path)) << "fragment not valid SPIR-V";
  remove(path);
}

// body with Block → ReturnStmt — exercises the body_root != 0 path
TEST_F(SpirvEmitTest, ReturnVoidBody) {
  Sidecar sc = make_sprite_sidecar();
  StageInfo stage;
  stage.shader_type_name = "S";
  stage.method_name = "vs";
  stage.stage_kind = 0;

  // nodes[0]: sentinel
  // nodes[1]: Block(b=0, c=1)   list[0]=2
  // nodes[2]: ReturnStmt(a=0)
  auto &body = stage.body;
  body.body_root = 1;
  body.nodes.push_back({0, 0, 0, 0, 0, 0});
  body.nodes.push_back({(u16)NodeKind::Block, 0, 0, 0, 0, 1});
  body.nodes.push_back({(u16)NodeKind::ReturnStmt, 0, 0, 0, 0, 0});
  body.list = {2};

  const char *path = "/tmp/um_test_retvoid.spv";
  remove(path);
  ASSERT_EQ(spirv_emit_stage(sc, stage, path), 0);
  EXPECT_TRUE(has_spv_magic(path));
  remove(path);
}

// ConstStmt → IntLit — exercises SSA binding (no alloca needed)
TEST_F(SpirvEmitTest, ConstBinding) {
  Sidecar sc = make_sprite_sidecar();
  StageInfo stage;
  stage.shader_type_name = "S";
  stage.method_name = "vs";
  stage.stage_kind = 0;

  auto &body = stage.body;
  body.body_root = 1;
  body.sym_names[42] = "x";

  // nodes[0]: sentinel
  // nodes[1]: Block(b=0, c=2)
  // nodes[2]: ConstStmt(a=42 sym, b=0, c=3 init)
  // nodes[3]: IntLit(a=7)
  // nodes[4]: ReturnStmt(a=0)
  // list: [2, 4]
  body.nodes.push_back({0, 0, 0, 0, 0, 0});
  body.nodes.push_back({(u16)NodeKind::Block, 0, 0, 0, 0, 2});
  body.nodes.push_back({(u16)NodeKind::ConstStmt, 0, 0, 42, 0, 3});
  body.nodes.push_back({(u16)NodeKind::IntLit, 0, 0, 7, 0, 0});
  body.nodes.push_back({(u16)NodeKind::ReturnStmt, 0, 0, 0, 0, 0});
  body.list = {2, 4};

  const char *path = "/tmp/um_test_const.spv";
  remove(path);
  ASSERT_EQ(spirv_emit_stage(sc, stage, path), 0);
  EXPECT_TRUE(has_spv_magic(path));
  remove(path);
}

// Field(Field(Ident(self), "vout"), "clip") = vec4(1,0,0,1)
// exercises IOVarMap lookup + vec4 InsertElement chain
TEST_F(SpirvEmitTest, IoVarWrite) {
  Sidecar sc = make_sprite_sidecar();
  StageInfo stage;
  stage.shader_type_name = "S";
  stage.method_name = "vs";
  stage.stage_kind = 0;

  auto &body = stage.body;
  body.body_root = 1;
  body.sym_names[1] = "self";
  body.sym_names[2] = "vout";
  body.sym_names[3] = "clip";
  body.sym_names[4] = "vec4";
  body.float_lits = {1.0, 0.0, 0.0, 1.0};

  // nodes[0]:  sentinel
  // nodes[1]:  Block(b=0, c=2)            list[0]=2, list[1]=12
  // nodes[2]:  AssignStmt(a=3,b=6,c=Equal)
  // nodes[3]:  Field(a=4, b=3)            .clip
  // nodes[4]:  Field(a=5, b=2)            .vout
  // nodes[5]:  Ident(a=1)                 self
  // nodes[6]:  Call(a=7, b=2, c=4)        vec4(list[2..5])
  // nodes[7]:  Ident(a=4)                 vec4
  // nodes[8..11]: FloatLit(a=0..3)
  // nodes[12]: ReturnStmt(a=0)
  // list: [2, 12, 8, 9, 10, 11]
  auto Equal = static_cast<uint32_t>(TokenKind::Equal);
  body.nodes.push_back({0, 0, 0, 0, 0, 0});
  body.nodes.push_back({(u16)NodeKind::Block, 0, 0, 0, 0, 2});
  body.nodes.push_back({(u16)NodeKind::AssignStmt, 0, 0, 3, 6, Equal});
  body.nodes.push_back({(u16)NodeKind::Field, 0, 0, 4, 3, 0});
  body.nodes.push_back({(u16)NodeKind::Field, 0, 0, 5, 2, 0});
  body.nodes.push_back({(u16)NodeKind::Ident, 0, 0, 1, 0, 0});
  body.nodes.push_back({(u16)NodeKind::Call, 0, 0, 7, 2, 4});
  body.nodes.push_back({(u16)NodeKind::Ident, 0, 0, 4, 0, 0});
  body.nodes.push_back({(u16)NodeKind::FloatLit, 0, 0, 0, 0, 0});
  body.nodes.push_back({(u16)NodeKind::FloatLit, 0, 0, 1, 0, 0});
  body.nodes.push_back({(u16)NodeKind::FloatLit, 0, 0, 2, 0, 0});
  body.nodes.push_back({(u16)NodeKind::FloatLit, 0, 0, 3, 0, 0});
  body.nodes.push_back({(u16)NodeKind::ReturnStmt, 0, 0, 0, 0, 0});
  body.list = {2, 12, 8, 9, 10, 11};

  const char *path = "/tmp/um_test_io_write.spv";
  remove(path);
  ASSERT_EQ(spirv_emit_stage(sc, stage, path), 0);
  EXPECT_TRUE(has_spv_magic(path));
  remove(path);
}

class ShaderLinkTest : public ::testing::Test {
  static bool s_initialized;

public:
  static void SetUpTestSuite() {
    if (!s_initialized) {
      spirv_init_target();
      s_initialized = true;
    }
  }
};
bool ShaderLinkTest::s_initialized = false;

// valid sidecar → shader_link produces .spv + .umrf
TEST_F(ShaderLinkTest, RoundTrip) {
  const char *sc_path = "/tmp/um_test.umshaders";
  const char *spv_path = "/tmp/S_vs.spv";
  const char *umrf_path = "/tmp/S.umrf";
  remove(sc_path);
  remove(spv_path);
  remove(umrf_path);

  UmshadersWriter w;
  write_minimal_sidecar(w);
  ASSERT_TRUE(write_buf(w, sc_path));

  ASSERT_EQ(shader_link(sc_path, "/tmp"), 0);
  EXPECT_TRUE(file_exists(spv_path)) << "vertex .spv not produced";
  EXPECT_TRUE(has_spv_magic(spv_path)) << "vertex .spv has wrong magic";
  EXPECT_TRUE(file_exists(umrf_path)) << ".umrf not produced";

  remove(sc_path);
  remove(spv_path);
  remove(umrf_path);
}

// wrong magic → sidecar_read rejects → shader_link returns -1
TEST_F(ShaderLinkTest, BadMagic) {
  const char *path = "/tmp/um_test_badmagic.umshaders";
  UmshadersWriter w;
  w.u32(0xDEADBEEFu);
  w.u16(UMSHADERS_VERSION);
  w.str("m");
  ASSERT_TRUE(write_buf(w, path));
  EXPECT_EQ(shader_link(path, "/tmp"), -1);
  remove(path);
}

// wrong version → sidecar_read rejects → shader_link returns -1
TEST_F(ShaderLinkTest, BadVersion) {
  const char *path = "/tmp/um_test_badver.umshaders";
  UmshadersWriter w;
  w.u32(UMSHADERS_MAGIC);
  w.u16(0xFFFFu);
  w.str("m");
  ASSERT_TRUE(write_buf(w, path));
  EXPECT_EQ(shader_link(path, "/tmp"), -1);
  remove(path);
}

// sidecar with sym_names table — verifies read_body deserializes it correctly;
// the stage body uses a ReturnStmt so the sym_names don't affect codegen but
// the reader must consume them without corrupting the parse position.
TEST_F(ShaderLinkTest, SymNamesRoundTrip) {
  const char *sc_path = "/tmp/um_test_syms.umshaders";
  const char *spv_path = "/tmp/S_vs.spv";
  remove(sc_path);
  remove(spv_path);

  UmshadersWriter w;
  w.u32(UMSHADERS_MAGIC);
  w.u16(UMSHADERS_VERSION);
  w.str("test");
  // pod "P" with a vs_in field so umrf_emit is satisfied
  w.u32(1);
  w.str("P");
  w.u32(1);
  w.str("pos");
  w.u8(1);
  w.u32(0);
  w.u32(0);
  w.str("vec4");
  // shader "S" with one vs_in annot
  w.u32(1);
  w.str("S");
  w.u32(1);
  w.str("vin");
  w.u8(1);
  w.str("P");
  // zero shader_fn helpers
  w.u32(0);
  // one vertex stage
  w.u32(1);
  w.str("S");
  w.str("vs");
  w.u8(0);
  // body: body_root=1, 3 nodes (sentinel + Block + ReturnStmt), list=[2]
  w.u32(1); // body_root
  w.u32(3); // node_count
  // sentinel
  w.u16(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  // Block: kind, span_s, span_e, a=0, b=0, c=1
  w.u16((uint16_t)NodeKind::Block);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(1);
  // ReturnStmt: a=0
  w.u16((uint16_t)NodeKind::ReturnStmt);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(1); // list_count
  w.u32(2); // list[0] = ReturnStmt node id
  w.u32(0);
  w.u32(0);
  w.u32(0);
  w.u32(0); // fors/array_lits/float_lits/sym counts = 0
  // sym_names: {99 → "self"} — ensures read_body reads and skips the table
  w.u32(1);
  w.u32(99);
  w.str("self");

  ASSERT_TRUE(write_buf(w, sc_path));
  ASSERT_EQ(shader_link(sc_path, "/tmp"), 0);
  EXPECT_TRUE(has_spv_magic(spv_path));

  remove(sc_path);
  remove(spv_path);
  remove("/tmp/S.umrf");
}
