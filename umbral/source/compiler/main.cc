#include <cstdio>
#include <string>

#include <compiler/driver.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: uc <file>.um [-o <output>] [--root <dir>]\n");
    return 1;
  }

  std::string src = argv[1];
  std::string out = "a.out";
  std::string root;
  for (int i = 2; i < argc - 1; ++i) {
    std::string flag = argv[i];
    if (flag == "-o") {
      out = argv[i + 1];
      ++i;
    } else if (flag == "--root") {
      root = argv[i + 1];
      ++i;
    }
  }

  Driver driver;
  auto result = driver.run(src, out, root);
  if (!result.ok) {
    std::fprintf(stderr, "uc: %s\n", result.error.c_str());
    return 1;
  }

  return 0;
}
