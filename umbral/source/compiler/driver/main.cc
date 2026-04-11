#include <compiler/driver/driver.h>
#include <llvm/Support/CommandLine.h>

namespace cl = llvm::cl;

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<file>.um"),
                                      cl::Required);
static cl::opt<std::string> OutputPath("o", cl::desc("output path"),
                                       cl::value_desc("file"),
                                       cl::init("a.out"));
static cl::opt<std::string> RootDir("root", cl::desc("module root directory"),
                                    cl::value_desc("dir"));
static cl::opt<std::string> ShaderOut("shader-out",
                                      cl::desc("emit .umsh shaders to <dir>"),
                                      cl::value_desc("dir"));
static cl::opt<std::string> AssetDir("asset-dir",
                                     cl::desc("include assets from <dir>"),
                                     cl::value_desc("dir"));
static cl::opt<bool> DumpIR("dump-ir", cl::desc("print LLVM IR and stop"));
static cl::opt<bool> DumpShaderMLIR("dump-shader-mlir",
                                    cl::desc("print um.shader MLIR and stop"));
static cl::opt<bool> DebugInfo("g", cl::desc("emit DWARF debug info"));

static cl::opt<unsigned> OptimizationLevel(
    "O", cl::desc("optimization level (0-3)"), cl::Prefix, cl::init(0));

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "umbral compiler\n");

  DriverOptions opts;
  opts.out_path = OutputPath;
  opts.has_out = OutputPath.getNumOccurrences() > 0;
  opts.root_override = RootDir;
  opts.shader_out = ShaderOut;
  opts.asset_dir = AssetDir;
  opts.dump_ir = DumpIR;
  opts.dump_shader_mlir = DumpShaderMLIR;
  opts.debug_info = DebugInfo;
  opts.opt_level = std::min(OptimizationLevel.getValue(), 3u);

  Driver driver;
  auto result = driver.run(InputFile, opts);
  if (!result.ok) {
    std::fprintf(stderr, "uc: %s\n", result.error.c_str());
    return 1;
  }

  return 0;
}
