#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/codegen/codegen.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>
#include <compiler/loader.h>
#include <compiler/sema/sema.h>

// declared in runtime_blob.cc and generated at build time by
// cmake/embed_blob.cmake
extern "C" const unsigned char umbral_runtime_blob[];
extern "C" const std::size_t umbral_runtime_blob_size;

struct DriverResult {
  bool ok = false;
  std::string error;
};

struct DriverOptions {
  std::string out_path = "a.out";
  std::string root_override;
  bool dump_ir = false; // print LLVM IR to stdout and stop
};

class Driver {
public:
  // compile src_path to a standalone executable at out_path.
  // libruntime.a is embedded in uc and written to a temp file at link time.
  // The output binary is fully self-contained — Vulkan loader (libvulkan.so)
  // must be present on the target system, but nothing else.
  DriverResult run(const std::string &src_path, const DriverOptions &opts = {});

private:
  DriverResult write_runtime(std::filesystem::path &out);
  DriverResult link(const std::filesystem::path &obj_path,
                    const std::filesystem::path &runtime_path,
                    const std::string &out_path);
};

inline DriverResult Driver::run(const std::string &src_path,
                                const DriverOptions &opts) {
  std::filesystem::path entry(src_path);

  if (!std::filesystem::exists(entry)) {
    return {false, "file: " + src_path + " does not exist"};
  }

  std::filesystem::path root = opts.root_override.empty()
                                   ? entry.parent_path()
                                   : std::filesystem::path(opts.root_override);

  Interner interner;

  // Load entry file and all transitive imports (topological order).
  auto load_r = load_modules(entry, root, interner);
  if (!load_r) return {false, load_r.error()};
  auto &modules = *load_r;

  // Sema over all modules.
  auto sema_r = run_sema(modules, interner);
  if (!sema_r) {
    // Use the entry module's src for error formatting (last in the vector).
    const std::string &entry_src = modules.back().src;
    return {false, format_error(sema_r.error(), entry_src, src_path)};
  }

  // LLVM Codegen
  auto llvm_ir = run_codegen(*sema_r, modules, interner, entry.stem().string());
  if (!llvm_ir) {
    const std::string &entry_src = modules.back().src;
    return {false, format_error(llvm_ir.error(), entry_src, src_path)};
  }

  // Optionally dump LLVM IR
  if (opts.dump_ir) {
    std::fputs(llvm_ir->ir.c_str(), stdout);
    return {true};
  }

  // Emit native object file
  auto obj_path = std::filesystem::temp_directory_path() /
                  ("umbral_" + std::to_string(::getpid()) + ".o");
  auto obj_r =
      emit_object(*llvm_ir->context, *llvm_ir->module, obj_path.string());
  if (!obj_r) return {false, "codegen: " + obj_r.error().msg};

  // Write embedded libruntime.a to a temp file
  std::filesystem::path rt_path;
  auto rt_r = write_runtime(rt_path);
  if (!rt_r.ok) {
    std::filesystem::remove(obj_path);
    return rt_r;
  }

  // Link final executable
  auto link_r = link(obj_path, rt_path, opts.out_path);

  // Clean up temp files regardless of link success.
  std::filesystem::remove(obj_path);
  std::filesystem::remove(rt_path);

  return link_r;
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
  cmd += "-ldl -lpthread -lX11 -lXrandr -lXi -lXcursor -lm -lvulkan ";
#elif defined(__APPLE__)
  cmd +=
      "-framework Cocoa -framework IOKit -framework CoreFoundation -lvulkan ";
#endif
  cmd += "-o " + out_path;

  if (std::system(cmd.c_str()) != 0) return {false, "linker invocation failed"};
  return {true, {}};
}
