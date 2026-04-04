#include <cstring>
#include <gtest/gtest.h>

extern "C" {
#include <asset.h>

typedef uint64_t um_alloc_handle_t;
uint64_t      rt_alloc(uint64_t size, uint64_t align, uint32_t tag, uint32_t site);
void          rt_free(uint64_t h, uint32_t site);
um_slice_u8_t rt_slice_from_alloc(uint64_t h, uint64_t es, uint64_t ea,
                                  uint64_t el, uint32_t site, uint32_t mf);
void rt_reset_for_testing(void);
}

struct AssetTest : ::testing::Test {
    void SetUp() override { rt_reset_for_testing(); }
};

TEST_F(AssetTest, IdFromName) {
    const uint8_t name[] = "test.png";
    uint64_t id = rt_asset_id_from_name(name, 8);
    EXPECT_NE(id, 0u);
    EXPECT_EQ(id, rt_asset_id_from_name(name, 8));
    EXPECT_NE(id, rt_asset_id_from_name((const uint8_t *)"other.png", 9));
}

TEST_F(AssetTest, IdDeterministic) {
    // same name always produces same id across calls
    const uint8_t a[] = "sound.wav";
    uint64_t id1 = rt_asset_id_from_name(a, sizeof(a) - 1);
    uint64_t id2 = rt_asset_id_from_name(a, sizeof(a) - 1);
    EXPECT_EQ(id1, id2);
}

TEST_F(AssetTest, InitNonexistentFile) {
    const uint8_t path[] = "/nonexistent/path.umpack";
    asset_pack_handle_t p = rt_assets_init(path, sizeof(path) - 1);
    EXPECT_EQ(p, 0u);
}

TEST_F(AssetTest, CleanupNull) {
    rt_assets_cleanup(0); // must not crash
}

TEST_F(AssetTest, ImageMetaNoPackReturnsZero) {
    uint32_t w = 42, h = 42;
    rt_asset_image_meta(0, 123, &w, &h);
    // pack=0 → no-op, values unchanged
    EXPECT_EQ(w, 42u);
    EXPECT_EQ(h, 42u);
}

TEST_F(AssetTest, AudioMetaNoPackReturnsZero) {
    uint64_t fc = 99;
    uint32_t ch = 99, sr = 99;
    rt_asset_audio_meta(0, 123, &fc, &ch, &sr);
    EXPECT_EQ(fc, 99u);
    EXPECT_EQ(ch, 99u);
    EXPECT_EQ(sr, 99u);
}
