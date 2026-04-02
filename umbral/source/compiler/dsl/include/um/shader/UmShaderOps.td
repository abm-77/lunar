// um.shader dialect: high-level shader ops for the Umbral gfx API.
// lowered to the MLIR spirv dialect then serialized to SPIR-V binaries.

include "mlir/IR/OpBase.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/IR/BuiltinAttributes.td"
include "mlir/IR/CommonAttrConstraints.td"
include "mlir/IR/CommonTypeConstraints.td"
include "mlir/Interfaces/SideEffectInterfaces.td"


def UmShader_Dialect : Dialect {
  let name = "um_shader";
  let cppNamespace = "::um::shader";
  let summary = "high-level shader ops reflecting the Umbral gfx API";
  let useDefaultTypePrinterParser = 1;
}


class UmShader_Type<string name, string mnem, list<Trait> traits = []>
    : TypeDef<UmShader_Dialect, name, traits> {
  let mnemonic = mnem;
}

def UmShader_TextureType : UmShader_Type<"Texture", "texture"> {
  let summary = "opaque handle to a bindless 2D texture (spirv image)";
}

def UmShader_SamplerType : UmShader_Type<"Sampler", "sampler"> {
  let summary = "opaque handle to a bindless sampler";
}

def UmShader_DrawPacketType : UmShader_Type<"DrawPacket", "draw_packet"> {
  let summary = "opaque reference to a draw packet in the draw_packets SSBO";
}


class UmShader_Op<string mnemonic, list<Trait> traits = []>
    : Op<UmShader_Dialect, mnemonic, traits>;


def LoadInputOp : UmShader_Op<"load_input", [Pure]> {
  let summary = "load a subfield from a shader input variable (@vs_in or @fs_in)";
  let description = [{
    Reads `self.<field_name>.<sub_field_name>` where `field_name` is a @vs_in
    or @fs_in annotated field on the @shader struct and `sub_field_name` is a
    field of the underlying @shader_pod type.
    Lowered to a `spirv.Load` from the corresponding Input GlobalVariable.
  }];
  let arguments = (ins StrAttr:$field_name, StrAttr:$sub_field_name);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$field_name `,` $sub_field_name attr-dict `:` type($result)";
}

def StoreOutputOp : UmShader_Op<"store_output"> {
  let summary = "store a value into a shader output variable (@vs_out or @fs_out)";
  let description = [{
    Writes `self.<field_name>.<sub_field_name> = value` where `field_name` is a
    @vs_out or @fs_out annotated field. Lowered to a `spirv.Store` to the
    corresponding Output GlobalVariable.
  }];
  let arguments = (ins StrAttr:$field_name, StrAttr:$sub_field_name, AnyType:$value);
  let assemblyFormat = "$field_name `,` $sub_field_name `,` $value attr-dict `:` type($value)";
}


def Texture2dOp : UmShader_Op<"texture2d", [Pure]> {
  let summary = "@texture2d(index) — load a bindless 2D texture handle";
  let arguments = (ins I32:$index);
  let results = (outs UmShader_TextureType:$result);
  let assemblyFormat = "$index attr-dict";
}

def SamplerOp : UmShader_Op<"sampler", [Pure]> {
  let summary = "@sampler(index) — load a bindless sampler handle";
  let arguments = (ins I32:$index);
  let results = (outs UmShader_SamplerType:$result);
  let assemblyFormat = "$index attr-dict";
}

def SampleOp : UmShader_Op<"sample", [Pure]> {
  let summary = "@sample(tex, samp, uv) — sample a 2D texture at UV coordinates";
  let description = [{
    Lowered to `spirv.SampledImage` + `spirv.ImageSampleImplicitLod`.
    `uv` must be a vector<2xf32>; result is vector<4xf32> (RGBA).
  }];
  let arguments = (ins
    UmShader_TextureType:$texture,
    UmShader_SamplerType:$sampler,
    AnyVectorOfAnyRank:$uv
  );
  let results = (outs AnyVectorOfAnyRank:$result);
  let assemblyFormat = "$texture `,` $sampler `,` $uv attr-dict `:` type($uv) `->` type($result)";
}

def DrawIdOp : UmShader_Op<"draw_id", [Pure]> {
  let summary = "@draw_id() — returns the current BuiltIn DrawIndex as i32";
  let results = (outs I32:$result);
  let assemblyFormat = "attr-dict";
}

def VertexIdOp : UmShader_Op<"vertex_id", [Pure]> {
  let summary = "@vertex_id() — returns the current BuiltIn VertexIndex as u32";
  let results = (outs I32:$result);
  let assemblyFormat = "attr-dict";
}

def DrawPacketOp : UmShader_Op<"draw_packet", [Pure]> {
  let summary = "@draw_packet(draw_id) — load a draw packet ref from the SSBO";
  let arguments = (ins I32:$draw_id);
  let results = (outs UmShader_DrawPacketType:$result);
  let assemblyFormat = "$draw_id attr-dict";
}

// TODO: replace with spirv.SampledImage once llvm/llvm-project#189891 lands.
def CombineSampledImageOp : UmShader_Op<"combine_sampled_image", [Pure]> {
  let summary = "combine image + sampler into a sampled image (SPIR-V OpSampledImage)";
  let description = [{
    Mirrors SPIR-V OpSampledImage (opcode 86). Takes an image loaded from
    the textures_2d descriptor and a sampler loaded from the samplers
    descriptor and produces a !spirv.sampled_image value suitable for
    spirv.ImageSampleImplicitLod.
  }];
  let arguments = (ins AnyType:$image, AnyType:$sampler);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$image `,` $sampler attr-dict `:` type($image) `,` type($sampler) `->` type($result)";
}

def DrawPacketFieldOp : UmShader_Op<"draw_packet_field", [Pure]> {
  let summary = "read a field from a draw packet by name";
  let arguments = (ins UmShader_DrawPacketType:$packet, StrAttr:$field_name);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$packet `,` $field_name attr-dict `:` type($result)";
}

def FrameReadOp : UmShader_Op<"frame_read", [Pure]> {
  let summary = "@frame_read<T>(offset) — read typed value from frame arena SSBO";
  let description = [{
    Reads `sizeof(T)` bytes from the frame_arena SSBO starting at `offset` bytes
    and reinterprets as type T. Lowered to an AccessChain + Load + Bitcast.
  }];
  let arguments = (ins I32:$offset, TypeAttr:$element_type);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$offset attr-dict `:` type($result)";
}
