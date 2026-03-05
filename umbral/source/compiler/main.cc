#include <cstdio>
#include <string>

#include <compiler/driver.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: uc <file>.um [-o <output>]\n");
    return 1;
  }

  std::string src = argv[1];
  std::string out = "a.out";
  for (int i = 2; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "-o") {
      out = argv[i + 1];
      break;
    }
  }

  Driver driver;
  auto result = driver.run(src, out);
  if (!result.ok) {
    std::fprintf(stderr, "uc: %s\n", result.error.c_str());
    return 1;
  }

  return 0;
}
