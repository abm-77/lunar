#pragma once

// pack pre-built .vert.spv + .frag.spv + .umrf files into a .umshader bundle.

// reads vert_spv_path, frag_spv_path, and umrf_path from disk and writes
// a single .umshader file at out_path.
// returns 0 on success; -1 on error (prints to stderr).
int shader_pack(const char *vert_spv_path, const char *frag_spv_path,
                const char *umrf_path, const char *out_path);
