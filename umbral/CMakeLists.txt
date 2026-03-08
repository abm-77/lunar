cmake_minimum_required(VERSION 3.22)
project(umbral LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Passed from superbuild
set(GLFW_ROOT "" CACHE PATH "GLFW install prefix")
set(DEPS_VK_HEADERS "" CACHE PATH "Vulkan-Headers include path")
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
./source/runtime/rt.cc
)
target_include_directories(runtime PUBLIC
"${DEPS_VK_HEADERS}"
"${GLFW_INCLUDE_DIR}"
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
dl pthread X11 Xrandr Xi Xxf86vm Xcursor m
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

# ---- compiler binary ----
add_executable(uc
  ./source/compiler/main.cc
  ${CMAKE_BINARY_DIR}/runtime_blob.cc
)

# LLVM include/defs
target_include_directories(uc PRIVATE ${LLVM_INCLUDE_DIRS} ${COMPILER_DIR})
target_compile_definitions(uc PRIVATE ${LLVM_DEFINITIONS})

# Map LLVM component libs you use
llvm_map_components_to_libnames(LLVM_LIBS
  core support irreader native target
)
target_link_libraries(uc PRIVATE ${LLVM_LIBS})

# ---- Testing ----
enable_testing()

# GTest (from submodule, path passed by superbuild)
set(GOOGLETEST_DIR "" CACHE PATH "googletest source root")
add_subdirectory(${GOOGLETEST_DIR} ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)

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

# lit functional tests
set(FILECHECK "" CACHE PATH "Path to FileCheck binary")
set(LIT_PY "${CMAKE_SOURCE_DIR}/../third_party/llvm-project/llvm/utils/lit/lit.py")
set(LIT_TEST_DIR "${CMAKE_SOURCE_DIR}/source/compiler/test")

configure_file(
  ${LIT_TEST_DIR}/lit.site.cfg.py.in
  ${CMAKE_BINARY_DIR}/lit.site.cfg.py
  @ONLY
)

add_custom_target(check-lit
  COMMAND python3 ${LIT_PY} -v ${LIT_TEST_DIR}
  DEPENDS uc
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running lit functional tests"
)
