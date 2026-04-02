#pragma once
#include <common/types.h>
#include <expected>
#include <string>
#include <string_view>

struct Error {
  Span span{};
  std::string msg = "";
  u32 module_idx =
      UINT32_MAX; // set for cross-module errors; UINT32_MAX = entry module
};

template <typename T> using Result = std::expected<T, Error>;

// Compute 1-based line and column from a byte offset into source text.
struct SourceLocation {
  u32 line, col;
};
inline SourceLocation source_location(std::string_view src, u32 offset) {
  u32 line = 1, col = 1;
  for (u32 i = 0; i < offset && i < static_cast<u32>(src.size()); ++i) {
    if (src[i] == '\n') {
      ++line;
      col = 1;
    } else ++col;
  }
  return {line, col};
}

// Format: "filename:line:col: error: msg"  (filename may be empty)
inline std::string format_error(const Error &e, std::string_view src,
                                std::string_view filename = "") {
  auto [line, col] = source_location(src, e.span.start);
  std::string out;
  if (!filename.empty()) {
    out += filename;
    out += ':';
  }
  out += std::to_string(line);
  out += ':';
  out += std::to_string(col);
  out += ": error: ";
  out += e.msg;
  return out;
}
