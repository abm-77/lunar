// asset linker: bundles processed assets into a .umpack archive.
//   --pack <out.umpack> -- bundle all input files into a .umpack
//   --compress          -- LZ4-compress each entry when smaller

#include "pack.h"
#include <cstdio>
#include <cstring>
#include <vector>


int main(int argc, char **argv) {
  const char *out_dir = ".";
  const char *pack_out = nullptr;
  int compress = 0;
  std::vector<const char *> inputs;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (!strcmp(argv[i], "--pack") && i + 1 < argc) {
      pack_out = argv[++i];
    } else if (!strcmp(argv[i], "--compress")) {
      compress = 1;
    } else {
      inputs.push_back(argv[i]);
    }
  }

  (void)out_dir; // used by callers; kept for forward compat

  if (inputs.empty() || !pack_out) {
    fprintf(stderr, "usage: ul --pack <out.umpack> [--compress] <file>...\n");
    return 1;
  }

  std::vector<pack_input_t> pi;
  for (const char *in : inputs) pi.push_back({in});
  return pack_build(pi.data(), (uint32_t)pi.size(), pack_out, compress) == 0 ? 0 : 1;
}
