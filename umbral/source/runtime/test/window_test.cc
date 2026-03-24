#include <gtest/gtest.h>
#include <cstdint>

extern "C" {
#include "window.h"
}

struct WindowTest : ::testing::Test {
  um_window_handle_t h = 0;

  void SetUp() override {
    rt_window_init_for_testing();
    h = rt_window_inject_for_testing();
  }
  void TearDown() override { rt_window_reset_for_testing(); }
};

// handle validation

TEST_F(WindowTest, NullHandleRejected) {
  EXPECT_EQ(rt_window_should_close(0), 1);
  EXPECT_EQ(rt_get_key_down(0, UM_KEY_A), 0);
  EXPECT_EQ(rt_get_mouse_button_down(0, UM_MOUSE_BUTTON_LEFT), 0);
}

TEST_F(WindowTest, ValidHandleAccepted) {
  EXPECT_NE(h, 0u);
  EXPECT_EQ(rt_window_should_close(h), 0);
}

TEST_F(WindowTest, StaleHandleRejectedAfterDestroy) {
  um_window_handle_t old = h;
  rt_window_destroy(old);
  // old handle now has wrong generation — all queries should return safe defaults
  EXPECT_EQ(rt_window_should_close(old), 1);
  EXPECT_EQ(rt_get_key_down(old, UM_KEY_A), 0);
  EXPECT_EQ(rt_get_mouse_button_down(old, UM_MOUSE_BUTTON_LEFT), 0);
}

TEST_F(WindowTest, SlotReusedAfterDestroy) {
  rt_window_destroy(h);
  um_window_handle_t h2 = rt_window_inject_for_testing();
  EXPECT_NE(h2, 0u);
  EXPECT_NE(h2, h); // new generation, so different handle
  EXPECT_EQ(rt_window_should_close(h2), 0);
}

// key queries

TEST_F(WindowTest, KeyDownWhenHeld) {
  rt_window_set_key_for_testing(h, UM_KEY_A, 1, 1);
  EXPECT_EQ(rt_get_key_down(h, UM_KEY_A), 1);
  EXPECT_EQ(rt_get_key_up(h, UM_KEY_A), 0);
}

TEST_F(WindowTest, KeyUpWhenNotHeld) {
  rt_window_set_key_for_testing(h, UM_KEY_A, 0, 0);
  EXPECT_EQ(rt_get_key_down(h, UM_KEY_A), 0);
  EXPECT_EQ(rt_get_key_up(h, UM_KEY_A), 1);
}

TEST_F(WindowTest, KeyPressedOnDownEdge) {
  rt_window_set_key_for_testing(h, UM_KEY_ESCAPE, 1, 0);
  EXPECT_EQ(rt_get_key_pressed(h, UM_KEY_ESCAPE), 1);
  EXPECT_EQ(rt_get_key_released(h, UM_KEY_ESCAPE), 0);
}

TEST_F(WindowTest, KeyNotPressedWhenAlreadyHeld) {
  rt_window_set_key_for_testing(h, UM_KEY_ESCAPE, 1, 1);
  EXPECT_EQ(rt_get_key_pressed(h, UM_KEY_ESCAPE), 0);
}

TEST_F(WindowTest, KeyReleasedOnUpEdge) {
  rt_window_set_key_for_testing(h, UM_KEY_SPACE, 0, 1);
  EXPECT_EQ(rt_get_key_released(h, UM_KEY_SPACE), 1);
  EXPECT_EQ(rt_get_key_pressed(h, UM_KEY_SPACE), 0);
}

TEST_F(WindowTest, KeyNotReleasedWhenAlreadyUp) {
  rt_window_set_key_for_testing(h, UM_KEY_SPACE, 0, 0);
  EXPECT_EQ(rt_get_key_released(h, UM_KEY_SPACE), 0);
}

TEST_F(WindowTest, TwoKeysIndependent) {
  rt_window_set_key_for_testing(h, UM_KEY_A, 1, 0);
  rt_window_set_key_for_testing(h, UM_KEY_B, 0, 0);
  EXPECT_EQ(rt_get_key_pressed(h, UM_KEY_A), 1);
  EXPECT_EQ(rt_get_key_pressed(h, UM_KEY_B), 0);
  EXPECT_EQ(rt_get_key_down(h, UM_KEY_B), 0);
}

// mouse button queries

TEST_F(WindowTest, MouseButtonPressedOnDownEdge) {
  rt_window_set_mouse_button_for_testing(h, UM_MOUSE_BUTTON_LEFT, 1, 0);
  EXPECT_EQ(rt_get_mouse_button_pressed(h, UM_MOUSE_BUTTON_LEFT), 1);
  EXPECT_EQ(rt_get_mouse_button_released(h, UM_MOUSE_BUTTON_LEFT), 0);
}

TEST_F(WindowTest, MouseButtonNotPressedWhenHeld) {
  rt_window_set_mouse_button_for_testing(h, UM_MOUSE_BUTTON_LEFT, 1, 1);
  EXPECT_EQ(rt_get_mouse_button_pressed(h, UM_MOUSE_BUTTON_LEFT), 0);
  EXPECT_EQ(rt_get_mouse_button_down(h, UM_MOUSE_BUTTON_LEFT), 1);
}

TEST_F(WindowTest, MouseButtonReleasedOnUpEdge) {
  rt_window_set_mouse_button_for_testing(h, UM_MOUSE_BUTTON_RIGHT, 0, 1);
  EXPECT_EQ(rt_get_mouse_button_released(h, UM_MOUSE_BUTTON_RIGHT), 1);
  EXPECT_EQ(rt_get_mouse_button_pressed(h, UM_MOUSE_BUTTON_RIGHT), 0);
}

TEST_F(WindowTest, TwoMouseButtonsIndependent) {
  rt_window_set_mouse_button_for_testing(h, UM_MOUSE_BUTTON_LEFT, 1, 0);
  rt_window_set_mouse_button_for_testing(h, UM_MOUSE_BUTTON_RIGHT, 0, 0);
  EXPECT_EQ(rt_get_mouse_button_pressed(h, UM_MOUSE_BUTTON_LEFT), 1);
  EXPECT_EQ(rt_get_mouse_button_down(h, UM_MOUSE_BUTTON_RIGHT), 0);
}

// mouse position and wheel

TEST_F(WindowTest, MousePosition) {
  rt_window_set_mouse_pos_for_testing(h, 320.0, 240.0, 0.0, 0.0);
  EXPECT_DOUBLE_EQ(rt_get_mouse_x(h), 320.0);
  EXPECT_DOUBLE_EQ(rt_get_mouse_y(h), 240.0);
}

TEST_F(WindowTest, MouseDelta) {
  rt_window_set_mouse_pos_for_testing(h, 100.0, 200.0, -5.0, 3.0);
  EXPECT_DOUBLE_EQ(rt_get_mouse_delta_x(h), -5.0);
  EXPECT_DOUBLE_EQ(rt_get_mouse_delta_y(h), 3.0);
}

TEST_F(WindowTest, MouseWheel) {
  rt_window_set_wheel_for_testing(h, 1.5f, -0.5f);
  EXPECT_FLOAT_EQ(rt_get_mouse_wheel_x(h), 1.5f);
  EXPECT_FLOAT_EQ(rt_get_mouse_wheel_y(h), -0.5f);
}
