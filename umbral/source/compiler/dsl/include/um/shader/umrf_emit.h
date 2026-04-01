#pragma once

#include <compiler/loader.h>
#include <common/interner.h>

#include <string_view>

namespace um::shader {

// emit a .umrf vertex reflection blob for the @vs_in field of shader_type
// in module lm. writes to <out_dir>/<ShaderName>.umrf.
// returns false on failure (prints to stderr).
bool emit_umrf(const LoadedModule &lm, const Interner &interner,
               SymId shader_type, std::string_view out_dir);

} // namespace um::shader
