#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/types.h"

typedef enum {
  ARG_I8,
  ARG_I16,
  ARG_I32,
  ARG_I64,
  ARG_U8,
  ARG_U16,
  ARG_U32,
  ARG_U64,
  ARG_BOOL,
  ARG_F32,
  ARG_F64,
  ARG_BYTES,
} um_arg_tag_t;

typedef struct {
  uint32_t tag;
  uint64_t a;
  uint64_t b;
} um_fmt_arg_t;

typedef struct {
  const um_fmt_arg_t *ptr;
  uint64_t len;
} um_slice_arg_t;

static void write_bytes(const uint8_t *p, uint64_t n) {
  if (n) fwrite(p, 1, (size_t)n, stdout);
}

static void write_cstr(const char *s) { fwrite(s, 1, strlen(s), stdout); }

static void write_i64(int64_t v) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
  if (n > 0) fwrite(buf, 1, (size_t)n, stdout);
}

static void write_u64(uint64_t v) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
  if (n > 0) fwrite(buf, 1, (size_t)n, stdout);
}

static void write_f64(double v) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", v);
  if (n > 0) fwrite(buf, 1, (size_t)n, stdout);
}

static void write_bool(uint64_t v) { write_cstr(v ? "true" : "false"); }

static double f64_from_bits(uint64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

static float f32_from_bits(uint32_t bits) {
  float f;
  memcpy(&f, &bits, sizeof(f));
  return f;
}

static void print_arg(const um_fmt_arg_t *arg) {
  switch ((um_arg_tag_t)arg->tag) {
  case ARG_I8: write_i64((int8_t)arg->a); break;
  case ARG_I16: write_i64((int16_t)arg->a); break;
  case ARG_I32: write_i64((int32_t)arg->a); break;
  case ARG_I64: write_i64((int64_t)arg->a); break;
  case ARG_U8: write_u64((uint8_t)arg->a); break;
  case ARG_U16: write_u64((uint16_t)arg->a); break;
  case ARG_U32: write_u64((uint32_t)arg->a); break;
  case ARG_U64: write_u64(arg->a); break;
  case ARG_BOOL: write_bool(arg->a); break;
  case ARG_F32: write_f64((double)f32_from_bits((uint32_t)arg->a)); break;
  case ARG_F64: write_f64(f64_from_bits(arg->a)); break;
  case ARG_BYTES: write_bytes((const uint8_t *)arg->a, arg->b); break;
  }
}

void rt_fmt_print_line(um_slice_u8_t fmt, um_slice_arg_t args) {
  uint64_t arg_idx = 0;
  uint64_t i = 0;
  while (i < fmt.len) {
    uint8_t c = fmt.ptr[i];
    if (c == '{') {
      if (i + 1 < fmt.len && fmt.ptr[i + 1] == '{') {
        fputc('{', stdout);
        i += 2;
      } else if (i + 1 < fmt.len && fmt.ptr[i + 1] == '}') {
        if (arg_idx < args.len) print_arg(&args.ptr[arg_idx++]);
        i += 2;
      } else {
        fputc('{', stdout);
        ++i;
      }
    } else if (c == '}') {
      if (i + 1 < fmt.len && fmt.ptr[i + 1] == '}') {
        fputc('}', stdout);
        i += 2;
      } else {
        fputc('}', stdout);
        ++i;
      }
    } else {
      fputc((char)c, stdout);
      ++i;
    }
  }
  fputc('\n', stdout);
}
