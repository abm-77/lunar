#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

extern "C" {
#include "common/types.h"

typedef enum {
  ARG_I8, ARG_I16, ARG_I32, ARG_I64,
  ARG_U8, ARG_U16, ARG_U32, ARG_U64,
  ARG_BOOL,
  ARG_F32, ARG_F64,
  ARG_BYTES,
} um_arg_tag_t;

typedef struct { uint32_t tag; uint64_t a; uint64_t b; } um_fmt_arg_t;
typedef struct { const um_fmt_arg_t *ptr; uint64_t len; } um_slice_arg_t;

void rt_fmt_print_line(um_slice_u8_t fmt, um_slice_arg_t args);
}

static um_slice_u8_t str(const char *s) {
  return { reinterpret_cast<const uint8_t *>(s),
           static_cast<uint64_t>(strlen(s)) };
}

static std::string capture(const char *fmt_str,
                            std::initializer_list<um_fmt_arg_t> args) {
  testing::internal::CaptureStdout();
  um_slice_arg_t sl{ args.begin(), static_cast<uint64_t>(args.size()) };
  rt_fmt_print_line(str(fmt_str), sl);
  return testing::internal::GetCapturedStdout();
}

static uint64_t f64_bits(double v) {
  uint64_t b; memcpy(&b, &v, 8); return b;
}
static uint64_t f32_bits(float v) {
  uint32_t b; memcpy(&b, &v, 4); return static_cast<uint64_t>(b);
}

// plain text

TEST(FmtTest, PlainText) {
  EXPECT_EQ(capture("hello", {}), "hello\n");
}

TEST(FmtTest, EmptyString) {
  EXPECT_EQ(capture("", {}), "\n");
}

// integer types

TEST(FmtTest, I32Positive) {
  EXPECT_EQ(capture("{}", {{ ARG_I32, 42, 0 }}), "42\n");
}

TEST(FmtTest, I32Negative) {
  EXPECT_EQ(capture("{}", {{ ARG_I32, static_cast<uint64_t>(-7), 0 }}), "-7\n");
}

TEST(FmtTest, I8) {
  EXPECT_EQ(capture("{}", {{ ARG_I8, static_cast<uint64_t>(-1), 0 }}), "-1\n");
}

TEST(FmtTest, I64Large) {
  EXPECT_EQ(capture("{}", {{ ARG_I64, static_cast<uint64_t>(1000000000LL * 9), 0 }}),
            "9000000000\n");
}

TEST(FmtTest, U32) {
  EXPECT_EQ(capture("{}", {{ ARG_U32, 255, 0 }}), "255\n");
}

TEST(FmtTest, U64Large) {
  EXPECT_EQ(capture("{}", {{ ARG_U64, UINT64_C(18446744073709551615), 0 }}),
            "18446744073709551615\n");
}

// bool

TEST(FmtTest, BoolTrue) {
  EXPECT_EQ(capture("{}", {{ ARG_BOOL, 1, 0 }}), "true\n");
}

TEST(FmtTest, BoolFalse) {
  EXPECT_EQ(capture("{}", {{ ARG_BOOL, 0, 0 }}), "false\n");
}

// floats

TEST(FmtTest, F64) {
  EXPECT_EQ(capture("{}", {{ ARG_F64, f64_bits(1.5), 0 }}), "1.5\n");
}

TEST(FmtTest, F32) {
  EXPECT_EQ(capture("{}", {{ ARG_F32, f32_bits(2.0f), 0 }}), "2\n");
}

// bytes

TEST(FmtTest, Bytes) {
  const char *s = "hi";
  uint64_t ptr = reinterpret_cast<uint64_t>(s);
  EXPECT_EQ(capture("{}", {{ ARG_BYTES, ptr, 2 }}), "hi\n");
}

// multiple args

TEST(FmtTest, MultipleArgs) {
  EXPECT_EQ(capture("{} + {} = {}", {
    { ARG_I32, 1, 0 },
    { ARG_I32, 2, 0 },
    { ARG_I32, 3, 0 },
  }), "1 + 2 = 3\n");
}

TEST(FmtTest, ArgsConsumedInOrder) {
  EXPECT_EQ(capture("{}{}{}", {
    { ARG_I32, 1, 0 },
    { ARG_I32, 2, 0 },
    { ARG_I32, 3, 0 },
  }), "123\n");
}

// brace escaping

TEST(FmtTest, EscapedOpenBrace) {
  EXPECT_EQ(capture("{{", {}), "{\n");
}

TEST(FmtTest, EscapedCloseBrace) {
  EXPECT_EQ(capture("}}", {}), "}\n");
}

TEST(FmtTest, EscapedBracesMixed) {
  EXPECT_EQ(capture("{{{}}}", {{ ARG_I32, 42, 0 }}), "{42}\n");
}

// edge cases

TEST(FmtTest, ExtraArgIgnored) {
  // more args than placeholders — extras are silently ignored
  EXPECT_EQ(capture("x", {{ ARG_I32, 99, 0 }}), "x\n");
}

TEST(FmtTest, ExtraPlaceholderSilent) {
  // more placeholders than args — excess {} produces nothing
  EXPECT_EQ(capture("{}{}", {{ ARG_I32, 1, 0 }}), "1\n");
}
