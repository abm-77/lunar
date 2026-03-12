#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

// __um_sites / __um_sites_count are defined in mem_test_stubs.c (C file) so
// they get external linkage; count = 0 keeps all site lookups in the
// "unknown" branch and the array is never accessed.
extern "C" {
  typedef uint64_t um_alloc_handle_t;
  struct um_slice_u8_t { const uint8_t *ptr; uint64_t len; };

  um_alloc_handle_t rt_alloc(uint64_t size, uint64_t align,
                              uint32_t tag, uint32_t site);
  void              rt_free(uint64_t handle, uint32_t site);
  um_slice_u8_t     rt_slice_from_alloc(uint64_t handle, uint64_t elem_size,
                                        uint64_t elem_align, uint64_t elem_len,
                                        uint32_t site, uint32_t mut_flag);
  void rt_reset_for_testing(void);
}

struct MemTest : ::testing::Test {
  void SetUp() override { rt_reset_for_testing(); }
};

// ── Basic alloc / free ────────────────────────────────────────────────────

TEST_F(MemTest, AllocReturnsNonZeroHandle) {
  um_alloc_handle_t h = rt_alloc(64, 8, 0, 0);
  EXPECT_NE(h, 0u);
  rt_free(h, 0);
}

TEST_F(MemTest, TwoAllocsHaveDifferentHandles) {
  um_alloc_handle_t a = rt_alloc(64, 8, 0, 0);
  um_alloc_handle_t b = rt_alloc(64, 8, 0, 0);
  EXPECT_NE(a, 0u);
  EXPECT_NE(b, 0u);
  EXPECT_NE(a, b);
  rt_free(a, 0);
  rt_free(b, 0);
}

TEST_F(MemTest, AllocFreeAllocReusesSameSlotWithNewGen) {
  um_alloc_handle_t a = rt_alloc(64, 8, 0, 0);
  rt_free(a, 0);
  um_alloc_handle_t b = rt_alloc(64, 8, 0, 0);
  EXPECT_NE(b, 0u);
  // Same slot index, different generation — handles must differ.
  EXPECT_NE(a, b);
  rt_free(b, 0);
}

// ── Slice access ──────────────────────────────────────────────────────────

TEST_F(MemTest, SliceFromAllocWriteAndRead) {
  const uint64_t count = 16;
  um_alloc_handle_t h = rt_alloc(count * sizeof(int32_t), alignof(int32_t), 0, 0);
  ASSERT_NE(h, 0u);

  um_slice_u8_t s = rt_slice_from_alloc(h, sizeof(int32_t), alignof(int32_t),
                                        count, 0, 0);
  ASSERT_NE(s.ptr, nullptr);
  EXPECT_EQ(s.len, count * sizeof(int32_t));

  // Write through the slice pointer.
  int32_t *arr = const_cast<int32_t *>(reinterpret_cast<const int32_t *>(s.ptr));
  for (uint64_t i = 0; i < count; ++i) arr[i] = (int32_t)i * 2;
  for (uint64_t i = 0; i < count; ++i) EXPECT_EQ(arr[i], (int32_t)i * 2);

  rt_free(h, 0);
}

TEST_F(MemTest, SliceFromAllocRequestingZeroElems) {
  um_alloc_handle_t h = rt_alloc(64, 8, 0, 0);
  ASSERT_NE(h, 0u);
  um_slice_u8_t s = rt_slice_from_alloc(h, 4, 4, 0, 0, 0);
  // Zero-element slice: ptr is valid, len is 0.
  EXPECT_EQ(s.len, 0u);
  rt_free(h, 0);
}

// ── Table growth ──────────────────────────────────────────────────────────

TEST_F(MemTest, AllocBeyondInitialCapacityGrows) {
  const int N = 2048; // > initial 1024 capacity
  std::vector<um_alloc_handle_t> handles(N);
  for (int i = 0; i < N; ++i) {
    handles[i] = rt_alloc(8, 8, 0, 0);
    ASSERT_NE(handles[i], 0u) << "alloc failed at i=" << i;
  }
  for (int i = 0; i < N; ++i) rt_free(handles[i], 0);
}

// ── Error / death paths (UM_DEBUG=1) ─────────────────────────────────────

TEST_F(MemTest, FreeNullHandleAborts) {
  EXPECT_DEATH(rt_free(0, 0), "");
}

TEST_F(MemTest, DoubleFreeAborts) {
  um_alloc_handle_t h = rt_alloc(64, 8, 0, 0);
  rt_free(h, 0);
  EXPECT_DEATH(rt_free(h, 0), "");
}

TEST_F(MemTest, SliceFromFreedHandleAborts) {
  um_alloc_handle_t h = rt_alloc(64, 8, 0, 0);
  rt_free(h, 0);
  EXPECT_DEATH(rt_slice_from_alloc(h, 4, 4, 1, 0, 0), "");
}

TEST_F(MemTest, SliceSizeExceedsAllocAborts) {
  // Allocate 16 bytes, request 8 elements of 4 bytes = 32 bytes — overflow.
  um_alloc_handle_t h = rt_alloc(16, 4, 0, 0);
  ASSERT_NE(h, 0u);
  EXPECT_DEATH(rt_slice_from_alloc(h, 4, 4, 8, 0, 0), "");
  // h is leaked here since the EXPECT_DEATH body runs in a fork.
}

TEST_F(MemTest, SliceAlignExceedsAllocAborts) {
  // Allocate with align=4, request align=16 — unsatisfied.
  um_alloc_handle_t h = rt_alloc(64, 4, 0, 0);
  ASSERT_NE(h, 0u);
  EXPECT_DEATH(rt_slice_from_alloc(h, 4, 16, 4, 0, 0), "");
}

// ── Tag is stored and doesn't affect correctness ──────────────────────────

TEST_F(MemTest, AllocWithTagSucceeds) {
  um_alloc_handle_t h = rt_alloc(32, 8, /*tag=*/42, 0);
  EXPECT_NE(h, 0u);
  rt_free(h, 0);
}
