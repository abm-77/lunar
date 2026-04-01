cmake_minimum_required(VERSION 3.22)
project(umbral LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Passed from superbuild
set(GLFW_ROOT "" CACHE PATH "GLFW install prefix")
set(DEPS_VK_HEADERS "" CACHE PATH "Vulkan-Headers include path")
set(LZ4_DIR "" CACHE PATH "lz4 source root (third_party/lz4)")
set(COMPILER_DIR "${CMAKE_SOURCE_DIR}/source")

# Vulkan loader from system / SDK
find_package(Vulkan REQUIRED)

# LLVM from installed prefix
find_package(LLVM CONFIG REQUIRED)
message(STATUS "LLVM_DIR: ${LLVM_DIR}")

# MLIR from installed prefix (same tree as LLVM)
find_package(MLIR REQUIRED CONFIG)
message(STATUS "MLIR_DIR: ${MLIR_DIR}")
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

# match LLVM's RTTI setting — LLVM is built with -fno-rtti
if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

# GLFW from our install
find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h HINTS "${GLFW_ROOT}/include" REQUIRED)
find_library(GLFW_LIB NAMES glfw3 glfw HINTS "${GLFW_ROOT}/lib" REQUIRED)

# ---- runtime library (GLFW + Vulkan) ----
add_library(runtime STATIC
./source/runtime/fmt.c
./source/runtime/mem.c
./source/runtime/window.c
./source/runtime/gfx/gfx_vulkan.c
)
target_include_directories(runtime PUBLIC
"${DEPS_VK_HEADERS}"
"${GLFW_INCLUDE_DIR}"
./source/runtime
./source/runtime/gfx
)
target_link_libraries(runtime PUBLIC
"${GLFW_LIB}"
Vulkan::Vulkan
)

if(APPLE)
target_link_libraries(runtime PUBLIC
"-framework Cocoa"
"-framework IOKit"
"-framework CoreFoundation"
)
endif()

if(UNIX AND NOT APPLE)
target_link_libraries(runtime PUBLIC
dl pthread X11 Xrandr Xi Xcursor m
)
endif()

# ---- Embed libruntime.a into uc as a byte array ----
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/runtime_blob.cc
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=$<TARGET_FILE:runtime>
    -DOUTPUT=${CMAKE_BINARY_DIR}/runtime_blob.cc
    -DSYMBOL=umbral_runtime_blob
    -P ${CMAKE_SOURCE_DIR}/cmake/embed_blob.cmake
  DEPENDS runtime
  COMMENT "Embedding libruntime.a into uc"
)

# ---- Embed libglfw3.a into uc as a byte array ----
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/glfw_blob.cc
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${GLFW_LIB}
    -DOUTPUT=${CMAKE_BINARY_DIR}/glfw_blob.cc
    -DSYMBOL=umbral_glfw_blob
    -P ${CMAKE_SOURCE_DIR}/cmake/embed_blob.cmake
  DEPENDS ${GLFW_LIB}
  COMMENT "Embedding libglfw3.a into uc"
)

# ---- um.shader dialect tablegen ----
# Sources live in compiler/dsl/include/um/shader/ (includes + .td)
#             and compiler/dsl/lib/mlir/shader/   (implementations)
set(UM_SHADER_INC_DIR "${CMAKE_SOURCE_DIR}/source/compiler/dsl/include/um/shader")
set(UM_SHADER_LIB_DIR "${CMAKE_SOURCE_DIR}/source/compiler/dsl/lib/mlir/shader")

set(LLVM_TARGET_DEFINITIONS "${UM_SHADER_INC_DIR}/UmShaderOps.td")
list(APPEND TABLEGEN_FLAGS "-I${MLIR_INCLUDE_DIRS}")

mlir_tablegen(UmShaderOps.h.inc       -gen-op-decls      ${TABLEGEN_FLAGS})
mlir_tablegen(UmShaderOps.cpp.inc     -gen-op-defs       ${TABLEGEN_FLAGS})
mlir_tablegen(UmShaderTypes.h.inc     -gen-typedef-decls --dialect=um_shader ${TABLEGEN_FLAGS})
mlir_tablegen(UmShaderTypes.cpp.inc   -gen-typedef-defs  --dialect=um_shader ${TABLEGEN_FLAGS})
mlir_tablegen(UmShaderDialect.h.inc   -gen-dialect-decls --dialect=um_shader ${TABLEGEN_FLAGS})
mlir_tablegen(UmShaderDialect.cpp.inc -gen-dialect-defs  --dialect=um_shader ${TABLEGEN_FLAGS})

add_custom_target(UmShaderTableGen DEPENDS
  UmShaderOps.h.inc    UmShaderOps.cpp.inc
  UmShaderTypes.h.inc  UmShaderTypes.cpp.inc
  UmShaderDialect.h.inc UmShaderDialect.cpp.inc
)

set(UM_SHADER_DSL_SOURCES
  ${UM_SHADER_LIB_DIR}/um_shader_dialect.cc
  ${UM_SHADER_LIB_DIR}/shader_mlir.cc
  ${UM_SHADER_LIB_DIR}/shader_spirv_lower.cc
  ${UM_SHADER_LIB_DIR}/shader_spirv_serialize.cc
  ${UM_SHADER_LIB_DIR}/shader_spirv_emit.cc
  ${UM_SHADER_LIB_DIR}/umrf_emit.cc
  ${UM_SHADER_LIB_DIR}/shader_compile.cc
)

# MLIR libraries needed for um.shader dialect + SPIR-V lowering
set(MLIR_SHADER_LIBS
  MLIRIR
  MLIRDialect
  MLIRPass
  MLIRTransforms
  MLIRFuncDialect
  MLIRArithDialect
  MLIRSCFDialect
  MLIRControlFlowDialect
  MLIRVectorDialect
  MLIRMemRefDialect
  MLIRSPIRVDialect
  MLIRSPIRVConversion
  MLIRSPIRVSerialization
  MLIRArithToSPIRV
  MLIRFuncToSPIRV
  MLIRSCFToControlFlow
  MLIRControlFlowToSPIRV
  MLIRSCFToSPIRV
  MLIRMemRefToSPIRV
  MLIRVectorToSPIRV
  MLIRRewrite
  MLIRTransformUtils
  MLIRInferTypeOpInterface
  MLIRSupport
  MLIRBytecodeOpInterface
)

# Shared include dirs for anything touching the um.shader DSL
set(UM_SHADER_INCLUDE_DIRS
  ${LLVM_INCLUDE_DIRS}
  ${MLIR_INCLUDE_DIRS}
  ${CMAKE_BINARY_DIR}                                         # generated .inc files
  ${CMAKE_SOURCE_DIR}/source/compiler/dsl/include             # um/shader/*.h
  ${COMPILER_DIR}                                             # compiler/ + common/
)

# ---- compiler binary ----
llvm_map_components_to_libnames(LLVM_LIBS
  core support irreader native nativecodegen target mc passes
)

add_executable(uc
  ./source/compiler/main.cc
  ${UM_SHADER_DSL_SOURCES}
  ${CMAKE_BINARY_DIR}/runtime_blob.cc
  ${CMAKE_BINARY_DIR}/glfw_blob.cc
)
add_dependencies(uc UmShaderTableGen)
target_include_directories(uc PRIVATE ${UM_SHADER_INCLUDE_DIRS})
target_compile_definitions(uc PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(uc PRIVATE ${LLVM_LIBS} ${MLIR_SHADER_LIBS})

# ---- Testing ----
enable_testing()

# GTest (from submodule, path passed by superbuild)
set(GOOGLETEST_DIR "" CACHE PATH "googletest source root")
add_subdirectory(${GOOGLETEST_DIR} ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)

# LZ4 — static library only; disable the CLI tool and shared lib.
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVE ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${LZ4_DIR}/build/cmake ${CMAKE_BINARY_DIR}/lz4 EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVE})

include(GoogleTest)

add_executable(interner_tests
  ./source/compiler/test/unit/interner_test.cc
)
target_include_directories(interner_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(interner_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(interner_tests)

add_executable(lexer_tests
  ./source/compiler/test/unit/lexer_test.cc
)
target_include_directories(lexer_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(lexer_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(lexer_tests)

add_executable(parser_tests
  ./source/compiler/test/unit/parser_test.cc
)
target_include_directories(parser_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(parser_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(parser_tests)

add_executable(sema_tests
  ./source/compiler/test/unit/sema_test.cc
)
target_include_directories(sema_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(sema_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(sema_tests)

add_executable(meta_tests
  ./source/compiler/test/unit/meta_test.cc
)
target_include_directories(meta_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(meta_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(meta_tests)

add_executable(runtime_tests
  ./source/runtime/test/mem_test.cc
  ./source/runtime/test/mem_test_stubs.c
  ./source/runtime/mem.c
)
target_include_directories(runtime_tests PRIVATE ./source/runtime)
target_compile_definitions(runtime_tests PRIVATE UM_TESTING=1)
target_link_libraries(runtime_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(runtime_tests)

add_executable(fmt_tests
  ./source/runtime/test/fmt_test.cc
  ./source/runtime/fmt.c
)
target_include_directories(fmt_tests PRIVATE ./source/runtime)
target_link_libraries(fmt_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(fmt_tests)

add_executable(window_tests
  ./source/runtime/test/window_test.cc
  ./source/runtime/test/window_test_stubs.c
  ./source/runtime/window.c
)
target_include_directories(window_tests PRIVATE "${GLFW_INCLUDE_DIR}" ./source/runtime)
target_compile_definitions(window_tests PRIVATE UM_TESTING=1 UM_DEBUG=0)
target_link_libraries(window_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(window_tests)

# ---- asset linker ----
# ul no longer compiles shaders; only needs Core/Support for potential future use.
llvm_map_components_to_libnames(UL_LLVM_LIBS
  Core Support
)

add_executable(ul
  ./source/tools/ul/main.cc
  ./source/tools/ul/tex.cc
  ./source/tools/ul/audio.cc
  ./source/tools/ul/pack.cc
  ./source/tools/ul/shader_pack.cc
)
target_include_directories(ul PRIVATE
  ${COMPILER_DIR}
  ./source/tools/ul
  ./source          # runtime/gfx/umshader.h
)
target_link_libraries(ul PRIVATE lz4_static)

add_executable(shader_mlir_tests
  ${UM_SHADER_DSL_SOURCES}
  ./source/compiler/test/unit/shader_mlir_test.cc
)
add_dependencies(shader_mlir_tests UmShaderTableGen)
target_include_directories(shader_mlir_tests PRIVATE ${UM_SHADER_INCLUDE_DIRS})
target_compile_definitions(shader_mlir_tests PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(shader_mlir_tests PRIVATE GTest::gtest_main ${LLVM_LIBS} ${MLIR_SHADER_LIBS})
gtest_discover_tests(shader_mlir_tests)

add_executable(shader_pack_tests
  ./source/tools/ul/shader_pack.cc
  ./source/tools/ul/test/shader_pack_test.cc
)
target_include_directories(shader_pack_tests PRIVATE
  ${COMPILER_DIR}
  ./source/tools/ul
  ./source          # runtime/gfx/umshader.h
)
target_link_libraries(shader_pack_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(shader_pack_tests)

# lit functional tests
set(FILECHECK "${CMAKE_SOURCE_DIR}/../build/_b_llvm/bin/FileCheck" CACHE PATH "Path to FileCheck binary" FORCE)
set(SPIRV_VAL "" CACHE PATH "Path to spirv-val binary")
set(LIT_PY "${CMAKE_SOURCE_DIR}/../third_party/llvm-project/llvm/utils/lit/lit.py")
set(LIT_TEST_DIR "${CMAKE_SOURCE_DIR}/source/compiler/test")

configure_file(
  ${LIT_TEST_DIR}/lit.site.cfg.py.in
  ${CMAKE_BINARY_DIR}/lit.site.cfg.py
  @ONLY
)

add_custom_target(check-lit
  COMMAND python3 ${LIT_PY} -v ${CMAKE_BINARY_DIR}
  DEPENDS uc ul
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running lit functional tests"
)
