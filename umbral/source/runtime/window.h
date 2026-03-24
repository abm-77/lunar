#pragma once
#include <stdint.h>
#include "common/types.h"

typedef uint64_t um_window_handle_t;

typedef enum {
  UM_KEY_UNKNOWN = 0,
  UM_KEY_A, UM_KEY_B, UM_KEY_C, UM_KEY_D, UM_KEY_E, UM_KEY_F, UM_KEY_G,
  UM_KEY_H, UM_KEY_I, UM_KEY_J, UM_KEY_K, UM_KEY_L, UM_KEY_M, UM_KEY_N,
  UM_KEY_O, UM_KEY_P, UM_KEY_Q, UM_KEY_R, UM_KEY_S, UM_KEY_T, UM_KEY_U,
  UM_KEY_V, UM_KEY_W, UM_KEY_X, UM_KEY_Y, UM_KEY_Z,
  UM_KEY_0, UM_KEY_1, UM_KEY_2, UM_KEY_3, UM_KEY_4,
  UM_KEY_5, UM_KEY_6, UM_KEY_7, UM_KEY_8, UM_KEY_9,
  UM_KEY_SPACE, UM_KEY_ENTER, UM_KEY_TAB, UM_KEY_BACKSPACE, UM_KEY_ESCAPE,
  UM_KEY_LEFT, UM_KEY_RIGHT, UM_KEY_UP, UM_KEY_DOWN,
  UM_KEY_INSERT, UM_KEY_DELETE, UM_KEY_HOME, UM_KEY_END,
  UM_KEY_PAGE_UP, UM_KEY_PAGE_DOWN,
  UM_KEY_LEFT_SHIFT, UM_KEY_RIGHT_SHIFT,
  UM_KEY_LEFT_CONTROL, UM_KEY_RIGHT_CONTROL,
  UM_KEY_LEFT_ALT, UM_KEY_RIGHT_ALT,
  UM_KEY_LEFT_SUPER, UM_KEY_RIGHT_SUPER,
  UM_KEY_MINUS, UM_KEY_EQUAL, UM_KEY_LEFT_BRACKET, UM_KEY_RIGHT_BRACKET,
  UM_KEY_BACKSLASH, UM_KEY_SEMICOLON, UM_KEY_APOSTROPHE, UM_KEY_GRAVE,
  UM_KEY_COMMA, UM_KEY_PERIOD, UM_KEY_SLASH,
  UM_KEY_F1, UM_KEY_F2, UM_KEY_F3, UM_KEY_F4, UM_KEY_F5, UM_KEY_F6,
  UM_KEY_F7, UM_KEY_F8, UM_KEY_F9, UM_KEY_F10, UM_KEY_F11, UM_KEY_F12,
  UM_KEY_KP_0, UM_KEY_KP_1, UM_KEY_KP_2, UM_KEY_KP_3, UM_KEY_KP_4,
  UM_KEY_KP_5, UM_KEY_KP_6, UM_KEY_KP_7, UM_KEY_KP_8, UM_KEY_KP_9,
  UM_KEY_KP_DECIMAL, UM_KEY_KP_DIVIDE, UM_KEY_KP_MULTIPLY,
  UM_KEY_KP_SUBTRACT, UM_KEY_KP_ADD, UM_KEY_KP_ENTER,
  UM_KEY_COUNT
} um_key_t;

typedef enum {
  UM_MOUSE_BUTTON_UNKNOWN = 0,
  UM_MOUSE_BUTTON_LEFT,
  UM_MOUSE_BUTTON_RIGHT,
  UM_MOUSE_BUTTON_MIDDLE,
  UM_MOUSE_BUTTON_4,
  UM_MOUSE_BUTTON_5,
  UM_MOUSE_BUTTON_6,
  UM_MOUSE_BUTTON_7,
  UM_MOUSE_BUTTON_8,
  UM_MOUSE_BUTTON_COUNT
} um_mouse_button_t;

um_window_handle_t rt_window_create(um_slice_u8_t title, int32_t width, int32_t height);
void               rt_window_destroy(um_window_handle_t h);
void               rt_window_poll_events(void);
int32_t            rt_window_should_close(um_window_handle_t h);
void               rt_window_request_close(um_window_handle_t h);

void    rt_input_begin_frame(um_window_handle_t h);
int32_t rt_get_key_down(um_window_handle_t h, um_key_t k);
int32_t rt_get_key_up(um_window_handle_t h, um_key_t k);
int32_t rt_get_key_pressed(um_window_handle_t h, um_key_t k);
int32_t rt_get_key_released(um_window_handle_t h, um_key_t k);
int32_t rt_get_mouse_button_down(um_window_handle_t h, um_mouse_button_t b);
int32_t rt_get_mouse_button_up(um_window_handle_t h, um_mouse_button_t b);
int32_t rt_get_mouse_button_pressed(um_window_handle_t h, um_mouse_button_t b);
int32_t rt_get_mouse_button_released(um_window_handle_t h, um_mouse_button_t b);
double  rt_get_mouse_x(um_window_handle_t h);
double  rt_get_mouse_y(um_window_handle_t h);
double  rt_get_mouse_delta_x(um_window_handle_t h);
double  rt_get_mouse_delta_y(um_window_handle_t h);
float   rt_get_mouse_wheel_x(um_window_handle_t h);
float   rt_get_mouse_wheel_y(um_window_handle_t h);

#ifdef UM_TESTING
// initialize key/mouse tables without calling glfwInit
void rt_window_init_for_testing(void);
// stamp a fake live window slot and return its handle
um_window_handle_t rt_window_inject_for_testing(void);
// directly set key state (curr and prev frames)
void rt_window_set_key_for_testing(um_window_handle_t h, um_key_t k,
                                   int32_t down, int32_t prev_down);
// directly set mouse button state
void rt_window_set_mouse_button_for_testing(um_window_handle_t h,
                                            um_mouse_button_t b,
                                            int32_t down, int32_t prev_down);
// directly set mouse position and delta
void rt_window_set_mouse_pos_for_testing(um_window_handle_t h,
                                         double x, double y,
                                         double dx, double dy);
// directly set scroll wheel state
void rt_window_set_wheel_for_testing(um_window_handle_t h, float wx, float wy);
// clear all window slots
void rt_window_reset_for_testing(void);
#endif
