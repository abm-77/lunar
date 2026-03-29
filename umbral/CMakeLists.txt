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

# ---- compiler binary ----
add_executable(uc
  ./source/compiler/main.cc
  ${CMAKE_BINARY_DIR}/runtime_blob.cc
  ${CMAKE_BINARY_DIR}/glfw_blob.cc
)

# LLVM include/defs
target_include_directories(uc PRIVATE ${LLVM_INCLUDE_DIRS} ${COMPILER_DIR})
target_compile_definitions(uc PRIVATE ${LLVM_DEFINITIONS})

# Map LLVM component libs you use
llvm_map_components_to_libnames(LLVM_LIBS
  core support irreader native nativecodegen target mc passes
)
target_link_libraries(uc PRIVATE ${LLVM_LIBS})

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
# SPIR-V backend — requires LLVM built with -DLLVM_TARGETS_TO_BUILD=X86;SPIRV
# (already set in root CMakeLists.txt; rebuild llvm_ep if not yet done).
llvm_map_components_to_libnames(UL_LLVM_LIBS
  SPIRVCodeGen SPIRVDesc SPIRVInfo
  Core Support MC TargetParser
)

add_executable(ul
  ./source/tools/ul/main.cc
  ./source/tools/ul/tex.cc
  ./source/tools/ul/audio.cc
  ./source/tools/ul/pack.cc
  ./source/tools/ul/shader_link.cc
  ./source/tools/ul/spirv_emit.cc
  ./source/tools/ul/umrf_emit.cc
)
target_include_directories(ul PRIVATE
  ${COMPILER_DIR}
  ./source/tools/ul
  ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(ul PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(ul PRIVATE ${UL_LLVM_LIBS} lz4_static)

add_executable(shader_tests
  ./source/tools/ul/spirv_emit.cc
  ./source/tools/ul/shader_link.cc
  ./source/tools/ul/umrf_emit.cc
  ./source/tools/ul/test/shader_tests.cc
)
target_include_directories(shader_tests PRIVATE
  ${COMPILER_DIR}
  ./source/tools/ul
  ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(shader_tests PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(shader_tests PRIVATE GTest::gtest_main ${UL_LLVM_LIBS})
gtest_discover_tests(shader_tests)

# lit functional tests
set(FILECHECK "${CMAKE_SOURCE_DIR}/../build/_b_llvm/bin/FileCheck" CACHE PATH "Path to FileCheck binary" FORCE)
set(LIT_PY "${CMAKE_SOURCE_DIR}/../third_party/llvm-project/llvm/utils/lit/lit.py")
set(LIT_TEST_DIR "${CMAKE_SOURCE_DIR}/source/compiler/test")

configure_file(
  ${LIT_TEST_DIR}/lit.site.cfg.py.in
  ${CMAKE_BINARY_DIR}/lit.site.cfg.py
  @ONLY
)

add_custom_target(check-lit
  COMMAND python3 ${LIT_PY} -v ${CMAKE_BINARY_DIR}
  DEPENDS uc
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running lit functional tests"
)
