#pragma once

#include <um/shader/umsh_emit.h>

#include <compiler/loader.h>
#include <common/interner.h>

#include <optional>

namespace um::shader {

// collect_refl — build vertex reflection data for a shader type from its
// @vs_in field and @shader_pod layout. returns nullopt when the shader has
// no @vs_in field (not an error; just no reflection needed).
// returns nullopt and prints to stderr on a real error.
std::optional<UmshReflData> collect_refl(const LoadedModule &lm,
                                          const Interner &interner,
                                          SymId shader_type);

} // namespace um::shader
