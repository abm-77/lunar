#pragma once

#include <cstdint>
#include <type_traits>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using f32 = float;
using f64 = double;

struct Span {
  u32 start, end;
};

// operator| and has() for u8-backed bit-flag enum classes.
template <typename T>
  requires std::is_enum_v<T> && std::is_same_v<std::underlying_type_t<T>, u8>
inline T operator|(T a, T b) {
  return static_cast<T>(static_cast<u8>(a) | static_cast<u8>(b));
}
template <typename T>
  requires std::is_enum_v<T> && std::is_same_v<std::underlying_type_t<T>, u8>
inline bool has(T f, T bit) {
  return (static_cast<u8>(f) & static_cast<u8>(bit)) != 0;
}
