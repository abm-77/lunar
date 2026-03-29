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
#include <compiler/shader/shader_emit.h>

// declared in runtime_blob.cc / glfw_blob.cc, generated at build time by
// cmake/embed_blob.cmake
extern "C" const unsigned char umbral_runtime_blob[];
extern "C" const std::size_t umbral_runtime_blob_size;
extern "C" const unsigned char umbral_glfw_blob[];
extern "C" const std::size_t umbral_glfw_blob_size;

struct DriverResult {
  bool ok = false;
  std::string error;
};

struct DriverOptions {
  std::string out_path = "a.out";
  std::string root_override;
  std::string sidecar_out; // if non-empty, write .umshaders to this path and stop
  bool dump_ir = false;    // print LLVM IR to stdout and stop
};

class Driver {
public:
  // compile src_path to a standalone executable at out_path.
  // libruntime.a is embedded in uc and written to a temp file at link time.
  // the output binary is fully self-contained — Vulkan loader (libvulkan.so)
  // must be present on the target system, but nothing else.
  DriverResult run(const std::string &src_path, const DriverOptions &opts = {});

private:
  DriverResult write_blob(const unsigned char *data, std::size_t size,
                          const std::string &prefix,
                          std::filesystem::path &out);
  DriverResult link(const std::filesystem::path &obj_path,
                    const std::filesystem::path &runtime_path,
                    const std::filesystem::path &glfw_path,
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

  // load entry file and all transitive imports (topological order).
  auto load_r = load_modules(entry, root, interner);
  if (!load_r) return {false, load_r.error().msg};
  auto &modules = *load_r;

  // sema over all modules.
  auto sema_r = run_sema(modules, interner);
  if (!sema_r) {
    const auto &err = sema_r.error();
    u32 midx = err.module_idx;
    bool has_mod = midx != UINT32_MAX && midx < modules.size();
    const std::string &esrc = has_mod ? modules[midx].src : modules.back().src;
    std::string epath = has_mod ? modules[midx].abs_path.string() : src_path;
    return {false, format_error(err, esrc, epath)};
  }

  // emit .umshaders sidecars for any module with shader declarations
  {
    bool has_any = false;
    for (const auto &lm : modules)
      if (!lm.mod.shader_stages.empty()) { has_any = true; break; }
    if (has_any) {
      std::string sidecar_dir = opts.sidecar_out.empty()
                                    ? std::filesystem::temp_directory_path().string()
                                    : opts.sidecar_out;
      emit_umshaders(modules, *sema_r, interner, sidecar_dir);
      if (!opts.sidecar_out.empty()) return {true};
    }
  }

  // LLVM codegen
  auto llvm_ir = run_codegen(*sema_r, modules, interner, entry.stem().string());
  if (!llvm_ir) {
    const std::string &entry_src = modules.back().src;
    return {false, format_error(llvm_ir.error(), entry_src, src_path)};
  }

  // optionally dump LLVM IR
  if (opts.dump_ir) {
    std::fputs(llvm_ir->ir.c_str(), stdout);
    return {true};
  }

  // emit native object file
  auto obj_path = std::filesystem::temp_directory_path() /
                  ("umbral_" + std::to_string(::getpid()) + ".o");
  auto obj_r =
      emit_object(*llvm_ir->context, *llvm_ir->module, obj_path.string());
  if (!obj_r) return {false, "codegen: " + obj_r.error().msg};

  // write embedded libruntime.a and libglfw3.a to temp files
  std::filesystem::path rt_path, glfw_path;
  auto rt_r = write_blob(umbral_runtime_blob, umbral_runtime_blob_size,
                         "umbral_runtime_", rt_path);
  if (!rt_r.ok) { std::filesystem::remove(obj_path); return rt_r; }
  auto glfw_r = write_blob(umbral_glfw_blob, umbral_glfw_blob_size,
                           "umbral_glfw_", glfw_path);
  if (!glfw_r.ok) {
    std::filesystem::remove(obj_path);
    std::filesystem::remove(rt_path);
    return glfw_r;
  }

  // link final executable
  auto link_r = link(obj_path, rt_path, glfw_path, opts.out_path);

  // clean up temp files regardless of link success.
  std::filesystem::remove(obj_path);
  std::filesystem::remove(rt_path);
  std::filesystem::remove(glfw_path);

  return link_r;
}

inline DriverResult Driver::write_blob(const unsigned char *data,
                                       std::size_t size,
                                       const std::string &prefix,
                                       std::filesystem::path &out) {
  out = std::filesystem::temp_directory_path() /
        (prefix + std::to_string(::getpid()) + ".a");
  std::ofstream f(out, std::ios::binary);
  if (!f) return {false, "cannot create temp file: " + out.string()};
  f.write(reinterpret_cast<const char *>(data),
          static_cast<std::streamsize>(size));
  if (!f) return {false, "failed to write blob: " + out.string()};
  return {true, {}};
}

inline DriverResult Driver::link(const std::filesystem::path &obj,
                                 const std::filesystem::path &runtime_a,
                                 const std::filesystem::path &glfw_a,
                                 const std::string &out_path) {
  std::string cmd = "cc ";
  cmd += obj.string() + " ";
  cmd += runtime_a.string() + " ";
  cmd += glfw_a.string() + " ";
#if defined(__linux__)
  cmd += "-ldl -lpthread -lX11 -lXrandr -lXi -lXcursor -lm -lvulkan ";
#elif defined(__APPLE__)
  cmd += "-framework Cocoa -framework IOKit -framework CoreFoundation -lvulkan ";
#endif
  cmd += "-o " + out_path;

  if (std::system(cmd.c_str()) != 0) return {false, "linker invocation failed"};
  return {true, {}};
}
