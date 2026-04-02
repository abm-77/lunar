#include "um_shader_dialect.h"

#include <mlir/IR/Builders.h>
#include <mlir/IR/DialectImplementation.h>
#include <mlir/IR/OpImplementation.h>

// generated dialect definitions
#include "UmShaderDialect.cpp.inc"

// generated type definitions
#define GET_TYPEDEF_CLASSES
#include "UmShaderTypes.cpp.inc"

// generated op definitions
#define GET_OP_CLASSES
#include "UmShaderOps.cpp.inc"

void um::shader::UmShaderDialect::initialize() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "UmShaderTypes.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "UmShaderOps.cpp.inc"
      >();
}
