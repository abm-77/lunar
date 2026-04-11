cmake_minimum_required(VERSION 3.22)
project(umbral LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Passed from superbuild
set(GLFW_ROOT "" CACHE PATH "GLFW install prefix")
set(DEPS_VK_HEADERS "" CACHE PATH "Vulkan-Headers include path")
set(LZ4_DIR "" CACHE PATH "lz4 source root (third_party/lz4)")
set(MINIAUDIO_DIR "" CACHE PATH "miniaudio source root (third_party/miniaudio)")
set(STB_DIR "" CACHE PATH "stb source root (third_party/stb)")
set(FREETYPE_DIR "" CACHE PATH "freetype source root (third_party/freetype)")
set(MSDFGEN_DIR  "" CACHE PATH "msdfgen source root (third_party/msdfgen)")
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

# clang 18 reports __cpp_concepts=201907L but libstdc++ 13's <expected> requires
# 202002L; patch it up so standard C++23 headers work correctly.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__cpp_concepts=202002L -Wno-builtin-macro-redefined")

# match LLVM's RTTI setting — LLVM is built with -fno-rtti
if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

# GLFW from our install
find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h HINTS "${GLFW_ROOT}/include" REQUIRED)
find_library(GLFW_LIB NAMES glfw3 glfw HINTS "${GLFW_ROOT}/lib" REQUIRED)

# freetype — strip optional deps for a standalone static build.
# clear any stale find_package results from the cmake cache before FreeType
# runs its own detection, otherwise a cached ZLIB_FOUND=TRUE will override
# FT_DISABLE_ZLIB and link against the system zlib.
unset(ZLIB_FOUND)
unset(ZLIB_FOUND CACHE)
unset(BZIP2_FOUND CACHE)
unset(PNG_FOUND CACHE)
unset(HarfBuzz_FOUND CACHE)
unset(BROTLI_FOUND CACHE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HVF      ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVE ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${FREETYPE_DIR} ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVE})

# msdfgen — core only; no FreeType integration, no standalone executable
set(MSDFGEN_CORE_ONLY        ON  CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP       OFF CACHE BOOL "" FORCE)
add_subdirectory(${MSDFGEN_DIR}  ${CMAKE_BINARY_DIR}/msdfgen  EXCLUDE_FROM_ALL)

# ---- runtime library (GLFW + Vulkan + audio + font) ----
add_library(runtime STATIC
./source/runtime/sys/log.c
./source/runtime/sys/fmt.c
./source/runtime/sys/mem.c
./source/runtime/sys/time.c
./source/runtime/sys/window.c
./source/runtime/asset/asset.c
./source/runtime/gfx/gfx_vulkan.c
./source/runtime/audio/audio.c
./source/runtime/audio/audio_thread.c
./source/runtime/audio/audio_device.c
./source/runtime/font/font.c
)
target_include_directories(runtime PUBLIC
"${DEPS_VK_HEADERS}"
"${GLFW_INCLUDE_DIR}"
"${MINIAUDIO_DIR}"
"${STB_DIR}"
${COMPILER_DIR}
./source/runtime
./source/runtime/gfx
./source/runtime/audio
./source/runtime/asset
./source/runtime/font
)
target_link_libraries(runtime PUBLIC
"${GLFW_LIB}"
Vulkan::Vulkan
lz4_static
)

if(APPLE)
target_link_libraries(runtime PUBLIC
"-framework Cocoa"
"-framework IOKit"
"-framework CoreFoundation"
"-framework CoreAudio"
"-framework AudioToolbox"
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

# ---- Embed liblz4.a into uc as a byte array ----
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/lz4_blob.cc
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=$<TARGET_FILE:lz4_static>
    -DOUTPUT=${CMAKE_BINARY_DIR}/lz4_blob.cc
    -DSYMBOL=umbral_lz4_blob
    -P ${CMAKE_SOURCE_DIR}/cmake/embed_blob.cmake
  DEPENDS lz4_static
  COMMENT "Embedding liblz4.a into uc"
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
set(UM_SHADER_DIR "${CMAKE_SOURCE_DIR}/source/compiler/shader")

set(LLVM_TARGET_DEFINITIONS "${UM_SHADER_DIR}/UmShaderOps.td")
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
  ${UM_SHADER_DIR}/um_shader_dialect.cc
  ${UM_SHADER_DIR}/shader_mlir.cc
  ${UM_SHADER_DIR}/shader_spirv_lower.cc
  ${UM_SHADER_DIR}/shader_spirv_serialize.cc
  ${UM_SHADER_DIR}/shader_spirv_emit.cc
  ${UM_SHADER_DIR}/umrf_emit.cc
  ${UM_SHADER_DIR}/umsh_emit.cc
  ${UM_SHADER_DIR}/shader_compile.cc
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
  ${COMPILER_DIR}                                             # compiler/ + common/
)

# ---- compiler binary ----
llvm_map_components_to_libnames(LLVM_LIBS
  core support irreader native nativecodegen target mc passes
)

add_executable(uc
  ./source/compiler/driver/main.cc
  ${UM_SHADER_DSL_SOURCES}
  ${CMAKE_BINARY_DIR}/runtime_blob.cc
  ${CMAKE_BINARY_DIR}/glfw_blob.cc
  ${CMAKE_BINARY_DIR}/lz4_blob.cc
)
add_dependencies(uc UmShaderTableGen)
target_include_directories(uc PRIVATE ${UM_SHADER_INCLUDE_DIRS})
separate_arguments(_llvm_defs UNIX_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(uc PRIVATE ${_llvm_defs})
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

set(UNIT_TEST_DIR "${CMAKE_SOURCE_DIR}/source/test/unit")

add_executable(interner_tests
  ${UNIT_TEST_DIR}/interner_test.cc
)
target_include_directories(interner_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(interner_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(interner_tests)

add_executable(lexer_tests
  ${UNIT_TEST_DIR}/lexer_test.cc
)
target_include_directories(lexer_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(lexer_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(lexer_tests)

add_executable(parser_tests
  ${UNIT_TEST_DIR}/parser_test.cc
)
target_include_directories(parser_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(parser_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(parser_tests)

add_executable(sema_tests
  ${UNIT_TEST_DIR}/sema_test.cc
)
target_include_directories(sema_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(sema_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(sema_tests)

add_executable(meta_tests
  ${UNIT_TEST_DIR}/meta_test.cc
)
target_include_directories(meta_tests PRIVATE ${COMPILER_DIR})
target_link_libraries(meta_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(meta_tests)

add_executable(runtime_tests
  ${UNIT_TEST_DIR}/mem_test.cc
  ${UNIT_TEST_DIR}/mem_test_stubs.c
  ./source/runtime/sys/mem.c
)
target_include_directories(runtime_tests PRIVATE ./source/runtime ${COMPILER_DIR})
target_compile_definitions(runtime_tests PRIVATE UM_TESTING=1)
target_link_libraries(runtime_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(runtime_tests)

add_executable(asset_tests
  ${UNIT_TEST_DIR}/asset_test.cc
  ${UNIT_TEST_DIR}/asset_test_stubs.c
  ./source/runtime/asset/asset.c
  ./source/runtime/sys/mem.c
)
target_include_directories(asset_tests PRIVATE
  ./source/runtime
  ./source/runtime/asset
  ${COMPILER_DIR}
)
target_compile_definitions(asset_tests PRIVATE UM_TESTING=1 UM_DEBUG=1)
target_link_libraries(asset_tests PRIVATE GTest::gtest_main lz4_static)
gtest_discover_tests(asset_tests)

add_executable(audio_tests
  ${UNIT_TEST_DIR}/audio_test.cc
  ${UNIT_TEST_DIR}/audio_test_stubs.c
  ./source/runtime/audio/audio.c
  ./source/runtime/sys/mem.c
)
target_include_directories(audio_tests PRIVATE
  "${MINIAUDIO_DIR}"
  ./source/runtime
  ./source/runtime/audio
  ${COMPILER_DIR}
)
target_compile_definitions(audio_tests PRIVATE UM_TESTING=1 UM_DEBUG=1)
target_link_libraries(audio_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(audio_tests)

add_executable(fmt_tests
  ${UNIT_TEST_DIR}/fmt_test.cc
  ./source/runtime/sys/fmt.c
)
target_include_directories(fmt_tests PRIVATE ./source/runtime ${COMPILER_DIR})
target_link_libraries(fmt_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(fmt_tests)

add_executable(window_tests
  ${UNIT_TEST_DIR}/window_test.cc
  ${UNIT_TEST_DIR}/window_test_stubs.c
  ./source/runtime/sys/window.c
)
target_include_directories(window_tests PRIVATE "${GLFW_INCLUDE_DIR}" ./source/runtime ${COMPILER_DIR})
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
  ./source/tools/ul/pack.cc
  ./source/tools/ul/decode.cc
  ./source/tools/ul/decode_font.cc
)
target_include_directories(ul PRIVATE
  ${LLVM_INCLUDE_DIRS}
  ${COMPILER_DIR}
  "${MINIAUDIO_DIR}"
  "${STB_DIR}"
  "${FREETYPE_DIR}/include"
  "${MSDFGEN_DIR}"
  ./source/tools/ul
  ./source
)
target_compile_options(ul PRIVATE ${_llvm_defs})
target_link_libraries(ul PRIVATE ${UL_LLVM_LIBS} lz4_static freetype msdfgen-core)

add_executable(shader_mlir_tests
  ${UM_SHADER_DSL_SOURCES}
  ${UNIT_TEST_DIR}/shader_mlir_test.cc
)
add_dependencies(shader_mlir_tests UmShaderTableGen)
target_include_directories(shader_mlir_tests PRIVATE ${UM_SHADER_INCLUDE_DIRS})
separate_arguments(_llvm_defs_tests UNIX_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(shader_mlir_tests PRIVATE ${_llvm_defs_tests})
target_link_libraries(shader_mlir_tests PRIVATE GTest::gtest_main ${LLVM_LIBS} ${MLIR_SHADER_LIBS})
gtest_discover_tests(shader_mlir_tests)

add_executable(shader_pack_tests
  ./source/tools/ul/shader_pack.cc
  ${UNIT_TEST_DIR}/shader_pack_test.cc
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
set(LIT_TEST_DIR "${CMAKE_SOURCE_DIR}/source/test/lit")

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

# ---- examples ----
# each subdirectory of examples/ with a main.um is an example.
# adding a new example requires no CMake changes.
set(EXAMPLES_DIR "${CMAKE_SOURCE_DIR}/../examples")
file(GLOB EXAMPLE_DIRS "${EXAMPLES_DIR}/*/")
foreach(EXAMPLE_DIR ${EXAMPLE_DIRS})
  get_filename_component(EXAMPLE_NAME ${EXAMPLE_DIR} NAME)
  if(EXISTS "${EXAMPLE_DIR}/main.um")
    file(GLOB EXAMPLE_UM_SOURCES "${EXAMPLE_DIR}/*.um")
    file(GLOB EXAMPLE_ASSETS "${EXAMPLE_DIR}/assets/*")
    set(ASSET_DIR_ARG "")
    if(EXISTS "${EXAMPLE_DIR}/assets")
      set(ASSET_DIR_ARG --asset-dir ${EXAMPLE_DIR}/assets)
    endif()
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/examples/${EXAMPLE_NAME}/${EXAMPLE_NAME}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/examples/${EXAMPLE_NAME}
      COMMAND $<TARGET_FILE:uc>
        ${EXAMPLE_DIR}/main.um
        --root ${CMAKE_SOURCE_DIR}/source/std
        --shader-out ${CMAKE_BINARY_DIR}/examples/${EXAMPLE_NAME}
        ${ASSET_DIR_ARG}
        -o ${CMAKE_BINARY_DIR}/examples/${EXAMPLE_NAME}/${EXAMPLE_NAME}
      DEPENDS uc ul ${EXAMPLE_UM_SOURCES} ${EXAMPLE_ASSETS}
      COMMENT "Building example: ${EXAMPLE_NAME}"
    )
    list(APPEND EXAMPLE_OUTPUTS ${CMAKE_BINARY_DIR}/examples/${EXAMPLE_NAME}/${EXAMPLE_NAME})
  endif()
endforeach()

add_custom_target(build_examples DEPENDS ${EXAMPLE_OUTPUTS})
