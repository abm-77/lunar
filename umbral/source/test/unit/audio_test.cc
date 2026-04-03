#include <cstring>
#include <gtest/gtest.h>

extern "C" {
#include <audio.h>
#include <audio_test_helpers.h>

// mem API used in the MemoryAPIUsage test (um_slice_u8_t comes from audio.h → c_types.h)
typedef uint64_t um_alloc_handle_t;
uint64_t      rt_alloc(uint64_t size, uint64_t align, uint32_t tag, uint32_t site);
void          rt_free(uint64_t h, uint32_t site);
um_slice_u8_t rt_slice_from_alloc(uint64_t h, uint64_t elem_sz, uint64_t elem_align,
                                  uint64_t elem_len, uint32_t site, uint32_t mut_flag);
void rt_reset_for_testing(void);
}

struct AudioTest : ::testing::Test {
    void SetUp() override { rt_reset_for_testing(); }
};

// ---- builder ----

TEST_F(AudioTest, BuilderCreateDestroy) {
    audio_builder_handle_t b = rt_audio_builder_create();
    EXPECT_NE(b, 0u);
    rt_audio_builder_destroy(b);
}

TEST_F(AudioTest, BuilderCreateBus) {
    audio_builder_handle_t b = rt_audio_builder_create();
    const uint8_t name[] = "master";
    audio_bus_handle_t bus = rt_audio_builder_create_bus(b, name, 6);
    EXPECT_NE(bus, 0u);
    EXPECT_EQ(audio_test_sub_tag(bus), audio_test_tag_bus());
    EXPECT_EQ(audio_test_sub_idx(bus), 0u);
    rt_audio_builder_destroy(b);
}

TEST_F(AudioTest, BuilderSetOutputBus) {
    audio_builder_handle_t b = rt_audio_builder_create();
    const uint8_t name[] = "out";
    audio_bus_handle_t bus = rt_audio_builder_create_bus(b, name, 3);
    rt_audio_builder_set_output_bus(b, bus);  // must not crash
    rt_audio_builder_destroy(b);
}

// ---- compile_graph ----

TEST_F(AudioTest, CompileGraphSingleBus) {
    audio_builder_handle_t b = rt_audio_builder_create();
    const uint8_t name[] = "master";
    audio_bus_handle_t bus = rt_audio_builder_create_bus(b, name, 6);
    rt_audio_builder_set_output_bus(b, bus);
    audio_graph_handle_t g = rt_audio_compile_graph(b);
    EXPECT_NE(g, 0u);
    rt_audio_builder_destroy(b);
    rt_audio_graph_destroy(g);
}

TEST_F(AudioTest, CompileGraphMultiBusRoute) {
    audio_builder_handle_t b      = rt_audio_builder_create();
    audio_bus_handle_t master     = rt_audio_builder_create_bus(b, (const uint8_t*)"master", 6);
    audio_bus_handle_t sfx        = rt_audio_builder_create_bus(b, (const uint8_t*)"sfx", 3);
    rt_audio_builder_create_route(b, sfx, master);
    rt_audio_builder_add_limiter(b, master);
    rt_audio_builder_set_output_bus(b, master);
    audio_graph_handle_t g = rt_audio_compile_graph(b);
    EXPECT_NE(g, 0u);
    EXPECT_EQ(audio_test_graph_bus_count(g), 2u);
    EXPECT_EQ(audio_test_graph_route_count(g), 1u);
    EXPECT_EQ(audio_test_graph_effect_count(g), 1u);
    rt_audio_builder_destroy(b);
    rt_audio_graph_destroy(g);
}

// ---- default graph ----

TEST_F(AudioTest, DefaultGraph) {
    audio_graph_handle_t g = rt_audio_graph_default();
    EXPECT_NE(g, 0u);
    rt_audio_graph_destroy(g);
}

TEST_F(AudioTest, DefaultGraphTopology) {
    audio_graph_handle_t g = rt_audio_graph_default();
    ASSERT_NE(g, 0u);

    EXPECT_EQ(audio_test_graph_bus_count(g), 4u);   // master, music, sfx, ui
    EXPECT_EQ(audio_test_graph_route_count(g), 3u);  // music→master, sfx→master, ui→master
    EXPECT_EQ(audio_test_graph_effect_count(g), 1u);
    EXPECT_EQ(audio_test_graph_effect_type(g, 0), AUDIO_TEST_EFFECT_LIMITER);

    const uint8_t master_name[] = "master";
    audio_bus_handle_t master_h =
        rt_audio_graph_bus_by_name(g, master_name, 6);
    EXPECT_NE(master_h, 0u);

    rt_audio_graph_destroy(g);
}

// ---- UM_DEBUG validation ----

#if defined(UM_DEBUG) && UM_DEBUG

TEST_F(AudioTest, CompileGraphNoCycleDebug) {
    // A → B → A should fail (cycle)
    audio_builder_handle_t b    = rt_audio_builder_create();
    audio_bus_handle_t a        = rt_audio_builder_create_bus(b, (const uint8_t*)"a", 1);
    audio_bus_handle_t bbus     = rt_audio_builder_create_bus(b, (const uint8_t*)"b", 1);
    rt_audio_builder_create_route(b, a, bbus);
    rt_audio_builder_create_route(b, bbus, a);
    rt_audio_builder_set_output_bus(b, a);
    audio_graph_handle_t g = rt_audio_compile_graph(b);
    EXPECT_EQ(g, 0u);
    rt_audio_builder_destroy(b);
}

TEST_F(AudioTest, CompileGraphNoOutputDebug) {
    audio_builder_handle_t b = rt_audio_builder_create();
    rt_audio_builder_create_bus(b, (const uint8_t*)"bus", 3);
    audio_graph_handle_t g = rt_audio_compile_graph(b);
    EXPECT_EQ(g, 0u);
    rt_audio_builder_destroy(b);
}

TEST_F(AudioTest, CompileGraphSelfRouteDebug) {
    audio_builder_handle_t b = rt_audio_builder_create();
    audio_bus_handle_t bus   = rt_audio_builder_create_bus(b, (const uint8_t*)"m", 1);
    rt_audio_builder_create_route(b, bus, bus);
    rt_audio_builder_set_output_bus(b, bus);
    audio_graph_handle_t g = rt_audio_compile_graph(b);
    EXPECT_EQ(g, 0u);
    rt_audio_builder_destroy(b);
}

TEST_F(AudioTest, CompileGraphUnreachableBusDebug) {
    audio_builder_handle_t b = rt_audio_builder_create();
    audio_bus_handle_t master = rt_audio_builder_create_bus(b, (const uint8_t*)"master", 6);
    rt_audio_builder_create_bus(b, (const uint8_t*)"isolated", 8); // no route
    rt_audio_builder_set_output_bus(b, master);
    audio_graph_handle_t g = rt_audio_compile_graph(b);
    EXPECT_EQ(g, 0u);
    rt_audio_builder_destroy(b);
}

#endif // UM_DEBUG

// ---- context ----

TEST_F(AudioTest, ContextCreateDestroy) {
    audio_graph_handle_t g   = rt_audio_graph_default();
    ASSERT_NE(g, 0u);
    audio_ctx_handle_t ctx   = rt_audio_ctx_create(g, 48000, 512, /*max_voices=*/0);
    EXPECT_NE(ctx, 0u);
    rt_audio_ctx_destroy(ctx);
    rt_audio_graph_destroy(g);
}

TEST_F(AudioTest, ContextCreateWithVoices) {
    audio_graph_handle_t g = rt_audio_graph_default();
    ASSERT_NE(g, 0u);
    // device open is stubbed by audio_test_stubs.c
    audio_ctx_handle_t ctx = rt_audio_ctx_create(g, 48000, 512, 4);
    EXPECT_NE(ctx, 0u);
    rt_audio_ctx_destroy(ctx);
    rt_audio_graph_destroy(g);
}

// ---- voice lifecycle ----

TEST_F(AudioTest, VoicePlayStopNullPCM) {
    audio_graph_handle_t g = rt_audio_graph_default();
    ASSERT_NE(g, 0u);
    audio_ctx_handle_t ctx = rt_audio_ctx_create(g, 48000, 512, 4);
    ASSERT_NE(ctx, 0u);

    const uint8_t sfx_name[] = "sfx";
    audio_bus_handle_t sfx = rt_audio_graph_bus_by_name(g, sfx_name, 3);
    EXPECT_NE(sfx, 0u);

    audio_voice_handle_t v = rt_audio_ctx_play_clip(ctx, nullptr, 0, sfx, 0.0f);
    EXPECT_NE(v, 0u);
    EXPECT_EQ(audio_test_sub_tag(v), audio_test_tag_voice());

    rt_audio_ctx_stop_voice(ctx, v);  // must not crash

    rt_audio_ctx_destroy(ctx);
    rt_audio_graph_destroy(g);
}

// ---- bus gain control ----

TEST_F(AudioTest, BusGainSetValid) {
    audio_graph_handle_t g = rt_audio_graph_default();
    ASSERT_NE(g, 0u);
    audio_ctx_handle_t ctx = rt_audio_ctx_create(g, 48000, 512, 0);
    ASSERT_NE(ctx, 0u);

    audio_bus_handle_t music = rt_audio_graph_bus_by_name(
        g, (const uint8_t*)"music", 5);
    EXPECT_NE(music, 0u);

    rt_audio_ctx_set_bus_gain_db(ctx, music, -6.0f);  // must not crash

    rt_audio_ctx_destroy(ctx);
    rt_audio_graph_destroy(g);
}

// ---- memory API usage example ----
// shows that audio builder state lives in the runtime allocator and that
// the handle correctly resolves to the allocated block.

TEST_F(AudioTest, MemoryAPIUsage) {
    // create a builder — internally calls rt_alloc(sizeof(audio_builder_state_t), ...)
    audio_builder_handle_t b = rt_audio_builder_create();
    ASSERT_NE(b, 0u);

    // resolve the handle to its backing bytes via the same rt_slice_from_alloc
    // that the runtime uses internally; size >= sizeof(audio_builder_state_t)
    um_slice_u8_t s = rt_slice_from_alloc(b, 1, 1, 1, 0, 1);
    EXPECT_NE(s.ptr, nullptr);
    EXPECT_GE(s.len, 1u);

    rt_audio_builder_destroy(b); // calls rt_free — slot is now free

    // in release mode (UM_DEBUG=0) a freed handle returns null slice
#if !UM_DEBUG
    um_slice_u8_t dead = rt_slice_from_alloc(b, 1, 1, 1, 0, 0);
    EXPECT_EQ(dead.ptr, nullptr);
#endif
}
