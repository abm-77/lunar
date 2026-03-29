#include <cstdio>
#include <string>

#include <compiler/driver.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: uc <file>.um [-o <output>] [--root <dir>] [--dump-ir] [--sidecar-out <dir>]\n");
    return 1;
  }

  std::string src = argv[1];
  DriverOptions opts;
  for (int i = 2; i < argc; ++i) {
    std::string flag = argv[i];
    if (flag == "-o" && i + 1 < argc) {
      opts.out_path = argv[++i];
    } else if (flag == "--root" && i + 1 < argc) {
      opts.root_override = argv[++i];
    } else if (flag == "--dump-ir") {
      opts.dump_ir = true;
    } else if (flag == "--sidecar-out" && i + 1 < argc) {
      opts.sidecar_out = argv[++i];
    }
  }

  Driver driver;
  auto result = driver.run(src, opts);
  if (!result.ok) {
    std::fprintf(stderr, "uc: %s\n", result.error.c_str());
    return 1;
  }

  return 0;
}
