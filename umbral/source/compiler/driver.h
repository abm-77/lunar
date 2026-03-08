#pragma once

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>

// declared in runtime_blob.cc and generated at build time by
// cmake/embed_blob.cmake
extern "C" const unsigned char umbral_runtime_blob[];
extern "C" const std::size_t umbral_runtime_blob_size;

struct DriverResult {
  bool ok = false;
  std::string error;
};

class Driver {
public:
  // compile src_path to a standalone executable at out_path.
  // libruntime.a is embedded in uc and written to a temp file at link time.
  // The output binary is fully self-contained — Vulkan loader (libvulkan.so)
  // must be present on the target system, but nothing else.
  DriverResult run(const std::string &src_path, const std::string &out_path);

private:
  DriverResult write_runtime(std::filesystem::path &out);
  DriverResult link(const std::filesystem::path &obj_path,
                    const std::filesystem::path &runtime_path,
                    const std::string &out_path);
};

inline DriverResult Driver::run(const std::string &src_path,
                                const std::string &out_path) {
  // read source
  std::ifstream f(src_path);
  if (!f) return {false, "cannot open '" + src_path + "'"};
  std::string src((std::istreambuf_iterator<char>(f)), {});

  // lex
  Interner interner;
  KeywordTable kws;
  kws.init(interner);
  auto lex_result = lex_source(src, interner, kws);
  if (!lex_result)
    return {false, format_error(lex_result.error(), src, src_path)};

  // parse
  Parser parser(*lex_result);
  parser.parse_module();
  if (parser.error())
    return {false, format_error(*parser.error(), src, src_path)};

  // TODO: type check

  // TODO: codegen — lower AST to LLVM IR and compile to obj_path, then:
  //   std::filesystem::path runtime_path;
  //   if (auto r = write_runtime(runtime_path); !r.ok) return r;
  //   return link(obj_path, runtime_path, out_path);

  return {false, "codegen not yet implemented"};
}

inline DriverResult Driver::write_runtime(std::filesystem::path &out) {
  out = std::filesystem::temp_directory_path() /
        ("umbral_runtime_" + std::to_string(::getpid()) + ".a");
  std::ofstream f(out, std::ios::binary);
  if (!f) return {false, "cannot create temp runtime file"};
  f.write(reinterpret_cast<const char *>(umbral_runtime_blob),
          static_cast<std::streamsize>(umbral_runtime_blob_size));
  if (!f) return {false, "failed to write runtime blob"};
  return {true, {}};
}

inline DriverResult Driver::link(const std::filesystem::path &obj,
                                 const std::filesystem::path &runtime_a,
                                 const std::string &out_path) {
  std::string cmd = "cc ";
  cmd += obj.string() + " ";
  cmd += runtime_a.string() + " ";
#if defined(__linux__)
  cmd += "-ldl -lpthread -lX11 -lXrandr -lXi -lXxf86vm -lXcursor -lm -lvulkan ";
#elif defined(__APPLE__)
  cmd +=
      "-framework Cocoa -framework IOKit -framework CoreFoundation -lvulkan ";
#endif
  cmd += "-o " + out_path;

  if (std::system(cmd.c_str()) != 0) return {false, "linker invocation failed"};
  return {true, {}};
}
