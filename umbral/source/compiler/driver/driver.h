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
#include <compiler/driver/loader.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>
#include <compiler/sema/sema.h>
#include <compiler/shader/shader_compile.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <mlir/IR/MLIRContext.h>

// declared in runtime_blob.cc / glfw_blob.cc, generated at build time by
// cmake/embed_blob.cmake
extern "C" const unsigned char umbral_runtime_blob[];
extern "C" const std::size_t umbral_runtime_blob_size;
extern "C" const unsigned char umbral_glfw_blob[];
extern "C" const std::size_t umbral_glfw_blob_size;
extern "C" const unsigned char umbral_lz4_blob[];
extern "C" const std::size_t umbral_lz4_blob_size;

struct DriverResult {
  bool ok = false;
  std::string error;
};

struct DriverOptions {
  std::string out_path = "a.out";
  std::string root_override;
  std::string shader_out;        // if non-empty, compile shaders to .umsh here
  std::string asset_dir;         // if non-empty, include assets from this dir in the pack
  bool has_out = false;          // true when -o was explicitly given
  bool dump_ir = false;          // print LLVM IR to stdout and stop
  bool dump_shader_mlir = false; // print um.shader MLIR to stdout and stop
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
                    const std::filesystem::path &lz4_path,
                    const std::string &out_path);
  DriverResult pack_assets(const std::filesystem::path &shader_dir,
                           const std::string &asset_dir);
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

  // dump um.shader MLIR to stdout (for lit tests)
  if (opts.dump_shader_mlir) {
    bool has_any = false;
    for (const auto &lm : modules)
      if (!lm.mod.shader_stages.empty()) {
        has_any = true;
        break;
      }
    if (has_any) {
      mlir::MLIRContext mlir_ctx;
      auto mlir_mod =
          um::shader::lower_to_mlir(mlir_ctx, modules, *sema_r, interner);
      if (!mlir_mod) return {false, "shader MLIR lowering failed"};
      mlir_mod->print(llvm::outs());
      llvm::outs() << "\n";
    }
    return {true};
  }

  // MLIR-based shader compilation: BodyIR → um.shader → SPIR-V → .umsh
  if (!opts.shader_out.empty()) {
    bool has_any = false;
    for (const auto &lm : modules)
      if (!lm.mod.shader_stages.empty()) {
        has_any = true;
        break;
      }
    if (has_any) {
      if (!um::shader::shader_compile(modules, *sema_r, interner,
                                      {opts.shader_out}))
        return {false, "shader compilation failed"};
    }
    // pack .umsh files into an .umpack asset bundle
    auto pack_r = pack_assets(opts.shader_out, opts.asset_dir);
    if (!pack_r.ok) return pack_r;

    // shader-only mode: stop here unless -o was explicitly given
    if (!opts.has_out) return {true};
  }

  // asset-only packing (no shaders, but --asset-dir was given)
  if (opts.shader_out.empty() && !opts.asset_dir.empty() && opts.has_out) {
    auto out_dir = std::filesystem::path(opts.out_path).parent_path();
    if (out_dir.empty()) out_dir = ".";
    auto pack_r = pack_assets(out_dir.string(), opts.asset_dir);
    if (!pack_r.ok) return pack_r;
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

  // write embedded libraries to temp files
  std::filesystem::path rt_path, glfw_path, lz4_path;
  auto rt_r = write_blob(umbral_runtime_blob, umbral_runtime_blob_size,
                         "umbral_runtime_", rt_path);
  if (!rt_r.ok) { std::filesystem::remove(obj_path); return rt_r; }

  auto glfw_r = write_blob(umbral_glfw_blob, umbral_glfw_blob_size,
                           "umbral_glfw_", glfw_path);
  if (!glfw_r.ok) {
    std::filesystem::remove(obj_path); std::filesystem::remove(rt_path);
    return glfw_r;
  }

  auto lz4_r = write_blob(umbral_lz4_blob, umbral_lz4_blob_size,
                           "umbral_lz4_", lz4_path);
  if (!lz4_r.ok) {
    std::filesystem::remove(obj_path); std::filesystem::remove(rt_path);
    std::filesystem::remove(glfw_path);
    return lz4_r;
  }

  // link final executable
  auto link_r = link(obj_path, rt_path, glfw_path, lz4_path, opts.out_path);

  // clean up temp files regardless of link success.
  std::filesystem::remove(obj_path);
  std::filesystem::remove(rt_path);
  std::filesystem::remove(glfw_path);
  std::filesystem::remove(lz4_path);

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
                                 const std::filesystem::path &lz4_a,
                                 const std::string &out_path) {
  auto cc = llvm::sys::findProgramByName("cc");
  if (!cc) return {false, "cannot find 'cc' on PATH"};

  llvm::SmallVector<llvm::StringRef, 32> args;
  args.push_back(*cc);
  std::string obj_s  = obj.string(),       rt_s   = runtime_a.string(),
              glfw_s = glfw_a.string(),    lz4_s  = lz4_a.string();
  args.push_back(obj_s);
  args.push_back(rt_s);
  args.push_back(glfw_s);
  args.push_back(lz4_s);
#if defined(__linux__)
  args.push_back("-ldl");
  args.push_back("-lpthread");
  args.push_back("-lX11");
  args.push_back("-lXrandr");
  args.push_back("-lXi");
  args.push_back("-lXcursor");
  args.push_back("-lm");
  args.push_back("-lz");
  args.push_back("-lvulkan");
#elif defined(__APPLE__)
  args.push_back("-lz");
  args.push_back("-framework"); args.push_back("Cocoa");
  args.push_back("-framework"); args.push_back("IOKit");
  args.push_back("-framework"); args.push_back("CoreFoundation");
  args.push_back("-framework"); args.push_back("CoreAudio");
  args.push_back("-framework"); args.push_back("AudioToolbox");
  args.push_back("-lvulkan");
#endif
  args.push_back("-o");
  args.push_back(out_path);

  std::string err;
  int rc = llvm::sys::ExecuteAndWait(*cc, args, std::nullopt, {}, 0, 0, &err);
  if (rc != 0) return {false, "linker invocation failed" +
                               (err.empty() ? "" : ": " + err)};
  return {true, {}};
}

inline DriverResult Driver::pack_assets(const std::filesystem::path &shader_dir,
                                        const std::string &asset_dir) {
  std::vector<std::filesystem::path> files;

  // collect .umsh files from the shader output directory
  if (std::filesystem::is_directory(shader_dir))
    for (const auto &e : std::filesystem::directory_iterator(shader_dir))
      if (e.path().extension() == ".umsh") files.push_back(e.path());

  // collect image and audio files from the asset directory
  if (!asset_dir.empty() && std::filesystem::is_directory(asset_dir)) {
    for (const auto &e : std::filesystem::directory_iterator(asset_dir)) {
      auto ext = e.path().extension().string();
      if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
          ext == ".wav" || ext == ".ogg" ||
          ext == ".ttf" || ext == ".otf")
        files.push_back(e.path());
    }
  }

  if (files.empty()) return {true, {}};

  auto self_path = llvm::sys::fs::getMainExecutable("uc", nullptr);
  auto ul_path =
      (std::filesystem::path(self_path).parent_path() / "ul").string();

  auto pack_path = (shader_dir / "assets.umpack").string();

  llvm::SmallVector<llvm::StringRef, 32> args;
  args.push_back(ul_path);
  args.push_back("--pack");
  args.push_back(pack_path);

  std::vector<std::string> file_strs;
  file_strs.reserve(files.size());
  for (const auto &f : files) {
    file_strs.push_back(f.string());
    args.push_back(file_strs.back());
  }

  std::string err;
  int rc = llvm::sys::ExecuteAndWait(ul_path, args, std::nullopt, {}, 0, 0,
                                     &err);
  if (rc != 0)
    return {false, "asset pack failed" + (err.empty() ? "" : ": " + err)};
  return {true, {}};
}
