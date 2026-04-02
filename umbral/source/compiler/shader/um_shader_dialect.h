#pragma once

#include <llvm/ADT/TypeSwitch.h>
#include <mlir/Bytecode/BytecodeOpInterface.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Dialect.h>
#include <mlir/IR/OpDefinition.h>
#include <mlir/IR/TypeSupport.h>
#include <mlir/IR/Types.h>
#include <mlir/Interfaces/SideEffectInterfaces.h>

// generated dialect declaration (dialect class + registration)
#include "UmShaderDialect.h.inc"

// generated type declarations
#define GET_TYPEDEF_CLASSES
#include "UmShaderTypes.h.inc"

// generated op declarations
#define GET_OP_CLASSES
#include "UmShaderOps.h.inc"
