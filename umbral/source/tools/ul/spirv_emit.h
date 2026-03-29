#pragma once

#include "shader_link.h"
#include <stdint.h>

// register the LLVM SPIR-V backend.
// must be called once before any spirv_emit_stage calls.
void spirv_init_target(void);

// lower one stage body to a SPIR-V binary file.
//   sc:       parsed .umshaders data (for IO layout and pod types)
//   stage:    the stage to emit
//   out_path: destination .spv path (null-terminated)
//   returns 0 on success; -1 on error
int spirv_emit_stage(const Sidecar &sc, const StageInfo &stage,
                     const char *out_path);

// build the SPIR-V-targeted LLVM module for one stage and print the IR to stdout.
// useful for testing: pipe through FileCheck to verify codegen output.
// returns 0 on success; -1 on error
int spirv_dump_stage_ir(const Sidecar &sc, const StageInfo &stage);
