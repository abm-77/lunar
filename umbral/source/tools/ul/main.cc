// asset linker: converts source assets into deployable runtime formats.
//   --shader-pack <name> — pack <name>.vert.spv + .frag.spv + .umrf → .umshader
//   .png/.jpg/.bmp → <name>.umtex
//   .wav/.ogg     → <name>.umaudio
//   --pack <out.umpack> — bundle processed assets into a .umpack archive

#include "audio.h"
#include "pack.h"
#include "shader_pack.h"
#include "tex.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char *file_ext(const char *path) {
  const char *dot = strrchr(path, '.');
  return dot ? dot : "";
}

static std::string stem(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *start = slash ? slash + 1 : path;
  const char *dot = strrchr(start, '.');
  return dot ? std::string(start, dot - start) : std::string(start);
}

static std::string out_path(const char *out_dir, const char *name,
                            const char *ext) {
  return std::string(out_dir) + "/" + name + ext;
}

// dispatch table: file extension → handler

typedef int (*handler_fn)(const char *in_path, const char *out_dir);

static int handle_texture(const char *in_path, const char *out_dir) {
  tex_blob_t blob{};
  if (tex_process(in_path, &blob) != 0) return 1;
  std::string dest = out_path(out_dir, stem(in_path).c_str(), ".umtex");
  FILE *f = fopen(dest.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "ul: cannot write %s\n", dest.c_str());
    tex_blob_free(&blob);
    return 1;
  }
  fwrite(blob.data, 1, blob.len, f);
  fclose(f);
  tex_blob_free(&blob);
  return 0;
}

static int handle_audio(const char *in_path, const char *out_dir) {
  audio_blob_t blob{};
  if (audio_process(in_path, &blob) != 0) return 1;
  std::string dest = out_path(out_dir, stem(in_path).c_str(), ".umaudio");
  FILE *f = fopen(dest.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "ul: cannot write %s\n", dest.c_str());
    audio_blob_free(&blob);
    return 1;
  }
  fwrite(blob.data, 1, blob.len, f);
  fclose(f);
  audio_blob_free(&blob);
  return 0;
}

struct DispatchEntry {
  const char *ext;
  handler_fn fn;
};

static const DispatchEntry k_dispatch[] = {
    {".png", handle_texture},  {".jpg", handle_texture},
    {".bmp", handle_texture},  {".wav", handle_audio},
    {".ogg", handle_audio},
};

int main(int argc, char **argv) {
  const char *out_dir = ".";
  const char *pack_out = nullptr;
  const char *shader_pack_name = nullptr; // --shader-pack <name>: pack <name>.vert.spv + .frag.spv + .umrf
  int compress = 0;
  std::vector<const char *> inputs;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (!strcmp(argv[i], "--pack") && i + 1 < argc) {
      pack_out = argv[++i];
    } else if (!strcmp(argv[i], "--shader-pack") && i + 1 < argc) {
      shader_pack_name = argv[++i];
    } else if (!strcmp(argv[i], "--compress")) {
      compress = 1;
    } else {
      inputs.push_back(argv[i]);
    }
  }

  // --shader-pack mode: pack pre-built .spv + .umrf into .umshader
  if (shader_pack_name) {
    std::string base = std::string(out_dir) + "/" + shader_pack_name;
    std::string vert = base + ".vert.spv";
    std::string frag = base + ".frag.spv";
    std::string umrf = base + ".umrf";
    std::string out  = base + ".umshader";
    return shader_pack(vert.c_str(), frag.c_str(), umrf.c_str(), out.c_str());
  }

  if (inputs.empty()) {
    fprintf(stderr, "usage: ul [--out-dir <dir>] [--pack <out.umpack>] "
                    "[--compress] [--shader-pack <name>] <file> ...\n");
    return 1;
  }

  int errors = 0;
  std::vector<std::string> pack_inputs;

  for (const char *in : inputs) {
    const char *ext = file_ext(in);
    handler_fn fn = nullptr;
    for (const auto &d : k_dispatch) {
      if (!strcmp(ext, d.ext)) {
        fn = d.fn;
        break;
      }
    }
    if (!fn) {
      fprintf(stderr, "ul: unknown extension '%s' for file %s\n", ext, in);
      ++errors;
      continue;
    }
    int rc = fn(in, out_dir);
    if (rc != 0) {
      ++errors;
      continue;
    }
  }

  if (pack_out && errors == 0) {
    std::vector<pack_input_t> pi;
    for (const auto &s : pack_inputs) pi.push_back({s.c_str()});
    if (pack_build(pi.data(), (uint32_t)pi.size(), pack_out, compress) != 0)
      ++errors;
  }

  return errors ? 1 : 0;
}
