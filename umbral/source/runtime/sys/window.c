#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "window.h"

#ifndef UM_DEBUG
#ifndef NDEBUG
#define UM_DEBUG 1
#else
#define UM_DEBUG 0
#endif
#endif

static um_key_t g_glfw_key_to_um[GLFW_KEY_LAST + 1];
static int g_um_to_glfw_key[UM_KEY_COUNT];

static void um_init_key_tables(void) {
  for (int i = 0; i <= GLFW_KEY_LAST; ++i) g_glfw_key_to_um[i] = UM_KEY_UNKNOWN;
  for (int i = 0; i < (int)UM_KEY_COUNT; ++i) g_um_to_glfw_key[i] = -1;

#define MAP(glfw_k, um_k)                                                      \
  do {                                                                         \
    g_glfw_key_to_um[(glfw_k)] = (um_k);                                       \
    g_um_to_glfw_key[(um_k)] = (glfw_k);                                       \
  } while (0)

  // letters
  MAP(GLFW_KEY_A, UM_KEY_A);
  MAP(GLFW_KEY_B, UM_KEY_B);
  MAP(GLFW_KEY_C, UM_KEY_C);
  MAP(GLFW_KEY_D, UM_KEY_D);
  MAP(GLFW_KEY_E, UM_KEY_E);
  MAP(GLFW_KEY_F, UM_KEY_F);
  MAP(GLFW_KEY_G, UM_KEY_G);
  MAP(GLFW_KEY_H, UM_KEY_H);
  MAP(GLFW_KEY_I, UM_KEY_I);
  MAP(GLFW_KEY_J, UM_KEY_J);
  MAP(GLFW_KEY_K, UM_KEY_K);
  MAP(GLFW_KEY_L, UM_KEY_L);
  MAP(GLFW_KEY_M, UM_KEY_M);
  MAP(GLFW_KEY_N, UM_KEY_N);
  MAP(GLFW_KEY_O, UM_KEY_O);
  MAP(GLFW_KEY_P, UM_KEY_P);
  MAP(GLFW_KEY_Q, UM_KEY_Q);
  MAP(GLFW_KEY_R, UM_KEY_R);
  MAP(GLFW_KEY_S, UM_KEY_S);
  MAP(GLFW_KEY_T, UM_KEY_T);
  MAP(GLFW_KEY_U, UM_KEY_U);
  MAP(GLFW_KEY_V, UM_KEY_V);
  MAP(GLFW_KEY_W, UM_KEY_W);
  MAP(GLFW_KEY_X, UM_KEY_X);
  MAP(GLFW_KEY_Y, UM_KEY_Y);
  MAP(GLFW_KEY_Z, UM_KEY_Z);

  // digits
  MAP(GLFW_KEY_0, UM_KEY_0);
  MAP(GLFW_KEY_1, UM_KEY_1);
  MAP(GLFW_KEY_2, UM_KEY_2);
  MAP(GLFW_KEY_3, UM_KEY_3);
  MAP(GLFW_KEY_4, UM_KEY_4);
  MAP(GLFW_KEY_5, UM_KEY_5);
  MAP(GLFW_KEY_6, UM_KEY_6);
  MAP(GLFW_KEY_7, UM_KEY_7);
  MAP(GLFW_KEY_8, UM_KEY_8);
  MAP(GLFW_KEY_9, UM_KEY_9);

  // whitespace / editing
  MAP(GLFW_KEY_SPACE, UM_KEY_SPACE);
  MAP(GLFW_KEY_ENTER, UM_KEY_ENTER);
  MAP(GLFW_KEY_TAB, UM_KEY_TAB);
  MAP(GLFW_KEY_BACKSPACE, UM_KEY_BACKSPACE);
  MAP(GLFW_KEY_ESCAPE, UM_KEY_ESCAPE);

  // arrows
  MAP(GLFW_KEY_LEFT, UM_KEY_LEFT);
  MAP(GLFW_KEY_RIGHT, UM_KEY_RIGHT);
  MAP(GLFW_KEY_UP, UM_KEY_UP);
  MAP(GLFW_KEY_DOWN, UM_KEY_DOWN);

  // navigation
  MAP(GLFW_KEY_INSERT, UM_KEY_INSERT);
  MAP(GLFW_KEY_DELETE, UM_KEY_DELETE);
  MAP(GLFW_KEY_HOME, UM_KEY_HOME);
  MAP(GLFW_KEY_END, UM_KEY_END);
  MAP(GLFW_KEY_PAGE_UP, UM_KEY_PAGE_UP);
  MAP(GLFW_KEY_PAGE_DOWN, UM_KEY_PAGE_DOWN);

  // modifiers
  MAP(GLFW_KEY_LEFT_SHIFT, UM_KEY_LEFT_SHIFT);
  MAP(GLFW_KEY_RIGHT_SHIFT, UM_KEY_RIGHT_SHIFT);
  MAP(GLFW_KEY_LEFT_CONTROL, UM_KEY_LEFT_CONTROL);
  MAP(GLFW_KEY_RIGHT_CONTROL, UM_KEY_RIGHT_CONTROL);
  MAP(GLFW_KEY_LEFT_ALT, UM_KEY_LEFT_ALT);
  MAP(GLFW_KEY_RIGHT_ALT, UM_KEY_RIGHT_ALT);
  MAP(GLFW_KEY_LEFT_SUPER, UM_KEY_LEFT_SUPER);
  MAP(GLFW_KEY_RIGHT_SUPER, UM_KEY_RIGHT_SUPER);

  // punctuation
  MAP(GLFW_KEY_MINUS, UM_KEY_MINUS);
  MAP(GLFW_KEY_EQUAL, UM_KEY_EQUAL);
  MAP(GLFW_KEY_LEFT_BRACKET, UM_KEY_LEFT_BRACKET);
  MAP(GLFW_KEY_RIGHT_BRACKET, UM_KEY_RIGHT_BRACKET);
  MAP(GLFW_KEY_BACKSLASH, UM_KEY_BACKSLASH);
  MAP(GLFW_KEY_SEMICOLON, UM_KEY_SEMICOLON);
  MAP(GLFW_KEY_APOSTROPHE, UM_KEY_APOSTROPHE);
  MAP(GLFW_KEY_GRAVE_ACCENT, UM_KEY_GRAVE);
  MAP(GLFW_KEY_COMMA, UM_KEY_COMMA);
  MAP(GLFW_KEY_PERIOD, UM_KEY_PERIOD);
  MAP(GLFW_KEY_SLASH, UM_KEY_SLASH);

  // function keys
  MAP(GLFW_KEY_F1, UM_KEY_F1);
  MAP(GLFW_KEY_F2, UM_KEY_F2);
  MAP(GLFW_KEY_F3, UM_KEY_F3);
  MAP(GLFW_KEY_F4, UM_KEY_F4);
  MAP(GLFW_KEY_F5, UM_KEY_F5);
  MAP(GLFW_KEY_F6, UM_KEY_F6);
  MAP(GLFW_KEY_F7, UM_KEY_F7);
  MAP(GLFW_KEY_F8, UM_KEY_F8);
  MAP(GLFW_KEY_F9, UM_KEY_F9);
  MAP(GLFW_KEY_F10, UM_KEY_F10);
  MAP(GLFW_KEY_F11, UM_KEY_F11);
  MAP(GLFW_KEY_F12, UM_KEY_F12);

  // keypad
  MAP(GLFW_KEY_KP_0, UM_KEY_KP_0);
  MAP(GLFW_KEY_KP_1, UM_KEY_KP_1);
  MAP(GLFW_KEY_KP_2, UM_KEY_KP_2);
  MAP(GLFW_KEY_KP_3, UM_KEY_KP_3);
  MAP(GLFW_KEY_KP_4, UM_KEY_KP_4);
  MAP(GLFW_KEY_KP_5, UM_KEY_KP_5);
  MAP(GLFW_KEY_KP_6, UM_KEY_KP_6);
  MAP(GLFW_KEY_KP_7, UM_KEY_KP_7);
  MAP(GLFW_KEY_KP_8, UM_KEY_KP_8);
  MAP(GLFW_KEY_KP_9, UM_KEY_KP_9);
  MAP(GLFW_KEY_KP_DECIMAL, UM_KEY_KP_DECIMAL);
  MAP(GLFW_KEY_KP_DIVIDE, UM_KEY_KP_DIVIDE);
  MAP(GLFW_KEY_KP_MULTIPLY, UM_KEY_KP_MULTIPLY);
  MAP(GLFW_KEY_KP_SUBTRACT, UM_KEY_KP_SUBTRACT);
  MAP(GLFW_KEY_KP_ADD, UM_KEY_KP_ADD);
  MAP(GLFW_KEY_KP_ENTER, UM_KEY_KP_ENTER);

#undef MAP
}

static um_mouse_button_t g_glfw_mouse_to_um[GLFW_MOUSE_BUTTON_LAST + 1];
static int g_um_to_glfw_mouse[UM_MOUSE_BUTTON_COUNT];

static void um_init_mouse_button_tables(void) {
  for (int i = 0; i <= GLFW_MOUSE_BUTTON_LAST; ++i)
    g_glfw_mouse_to_um[i] = UM_MOUSE_BUTTON_UNKNOWN;
  for (int i = 0; i < (int)UM_MOUSE_BUTTON_COUNT; ++i)
    g_um_to_glfw_mouse[i] = -1;

#define MAP(glfw_b, um_b)                                                      \
  do {                                                                         \
    g_glfw_mouse_to_um[(glfw_b)] = (um_b);                                     \
    g_um_to_glfw_mouse[(um_b)] = (glfw_b);                                     \
  } while (0)

  MAP(GLFW_MOUSE_BUTTON_LEFT, UM_MOUSE_BUTTON_LEFT);
  MAP(GLFW_MOUSE_BUTTON_RIGHT, UM_MOUSE_BUTTON_RIGHT);
  MAP(GLFW_MOUSE_BUTTON_MIDDLE, UM_MOUSE_BUTTON_MIDDLE);
  MAP(GLFW_MOUSE_BUTTON_4, UM_MOUSE_BUTTON_4);
  MAP(GLFW_MOUSE_BUTTON_5, UM_MOUSE_BUTTON_5);
  MAP(GLFW_MOUSE_BUTTON_6, UM_MOUSE_BUTTON_6);
  MAP(GLFW_MOUSE_BUTTON_7, UM_MOUSE_BUTTON_7);
  MAP(GLFW_MOUSE_BUTTON_8, UM_MOUSE_BUTTON_8);

#undef MAP
}

#define UM_KEY_WORDS ((UM_KEY_COUNT + 63) / 64)

typedef struct {
  uint64_t keys[UM_KEY_WORDS];
  uint64_t prev_keys[UM_KEY_WORDS];
  uint32_t mouse_buttons;
  uint32_t prev_mouse_buttons;
  double mouse_x, mouse_y, prev_mouse_x, prev_mouse_y;
  double mouse_delta_x, mouse_delta_y;
  float wheel_x, wheel_y;
} um_input_state_t;

static inline void kb_bitset_set_to(uint64_t *bits, um_key_t k, int32_t value) {
  uint32_t w = (uint32_t)k >> 6;
  uint32_t b = (uint32_t)k & 63;
  uint64_t m = 1ull << b;
  bits[w] = value ? bits[w] | m : bits[w] & ~m;
}

static inline int32_t kb_bitset_test(const uint64_t *bits, uint32_t k) {
  uint32_t w = k >> 6;
  uint32_t b = k & 63;
  return (bits[w] >> b) & 1u;
}

static inline void mb_bitset_set_to(uint32_t *bits, um_mouse_button_t b,
                                    int32_t value) {
  uint32_t mask = 1u << (uint32_t)b;
  *bits = value ? *bits | mask : *bits & ~mask;
}

static inline int32_t mb_bitset_test(uint32_t bits, um_mouse_button_t b) {
  return (bits >> (uint32_t)b) & 1u;
}

static inline void input_reset(um_input_state_t *input) {
  memset(input, 0, sizeof(um_input_state_t));
}

enum { UM_MAX_WINDOWS = 4 };

typedef struct {
  GLFWwindow *win;
  uint32_t gen;
  uint8_t live;
  um_input_state_t input;
} um_window_entry_t;

static um_window_entry_t g_windows[UM_MAX_WINDOWS];

static void scroll_callback(GLFWwindow *win, double xoff, double yoff) {
  uint32_t idx = (uint32_t)(uintptr_t)glfwGetWindowUserPointer(win);
  if (idx == 0 || idx >= UM_MAX_WINDOWS || !g_windows[idx].live) return;
  g_windows[idx].input.wheel_x += (float)xoff;
  g_windows[idx].input.wheel_y += (float)yoff;
}

static inline um_window_handle_t make_handle(uint32_t index, uint32_t gen) {
  return (((uint64_t)gen) << 32) | ((uint64_t)index);
}

static inline uint32_t h_index(um_window_handle_t handle) {
  return (uint32_t)(handle & 0xFFFFFFFFu);
}

static inline uint32_t h_gen(um_window_handle_t handle) {
  return (uint32_t)(handle >> 32);
}

#if UM_DEBUG
static void trap_bad_window(const char *op, um_window_handle_t h,
                            const char *why) {
  fprintf(stderr, "WINDOW ERROR: %s: %s\n", op, why);
  fprintf(stderr, "\thandle: index=%u gen=%u raw=0x%016llx\n", h_index(h),
          h_gen(h), (unsigned long long)h);
  abort();
}

// print the last GLFW error (if any) to stderr.
static void print_glfw_error(void) {
  const char *desc = NULL;
  int code = glfwGetError(&desc);
  if (code != GLFW_NO_ERROR)
    fprintf(stderr, "\tGLFW error 0x%04x: %s\n", code,
            desc ? desc : "(no description)");
}
#endif

// validates h and returns the entry, or NULL (and aborts in debug mode).
static um_window_entry_t *get_window_entry(um_window_handle_t h,
                                           const char *op) {
  if (h == 0) {
#if UM_DEBUG
    trap_bad_window(op, h, "null handle");
#endif
    return NULL;
  }

  uint32_t idx = h_index(h);
  if (idx == 0 || idx >= UM_MAX_WINDOWS) {
#if UM_DEBUG
    trap_bad_window(op, h, "index out of range");
#endif
    return NULL;
  }

  um_window_entry_t *e = &g_windows[idx];
  if (!e->live) {
#if UM_DEBUG
    trap_bad_window(op, h, "window already destroyed (use after destroy)");
#endif
    return NULL;
  }

  if (e->gen != h_gen(h)) {
#if UM_DEBUG
    trap_bad_window(op, h, "generation mismatch (stale handle)");
#endif
    return NULL;
  }

  return e;
}

// return the raw GLFWwindow* for the gfx layer (swapchain surface creation).
void *window_glfw_ptr(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "window_glfw_ptr");
  return e ? (void *)e->win : NULL;
}

#if UM_DEBUG
static void trap_bad_key(const char *op, um_key_t k) {
  fprintf(stderr, "WINDOW ERROR: %s: key %d out of range [0, %d)\n", op, (int)k,
          (int)UM_KEY_COUNT);
  abort();
}

static void trap_bad_mouse_button(const char *op, um_mouse_button_t b) {
  fprintf(stderr, "WINDOW ERROR: %s: mouse button %d out of range [0, %d)\n",
          op, (int)b, (int)UM_MOUSE_BUTTON_COUNT);
  abort();
}
#endif

static inline int key_valid(um_key_t k) {
  return (unsigned)k < (unsigned)UM_KEY_COUNT;
}

static inline int mouse_button_valid(um_mouse_button_t b) {
  return (unsigned)b < (unsigned)UM_MOUSE_BUTTON_COUNT;
}

um_window_handle_t rt_window_create(um_slice_u8_t title, int32_t width,
                                    int32_t height) {
#if UM_DEBUG
  if (!title.ptr) {
    fprintf(stderr, "WINDOW ERROR: rt_window_create: null title pointer\n");
    abort();
  }
  if (width <= 0 || height <= 0) {
    fprintf(stderr,
            "WINDOW ERROR: rt_window_create: invalid dimensions %dx%d\n", width,
            height);
    abort();
  }
#endif

  // slot 0 is reserved as the null handle sentinel
  for (int i = 1; i < UM_MAX_WINDOWS; ++i) {
    if (!g_windows[i].live) {
      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      GLFWwindow *win =
          glfwCreateWindow(width, height, (const char *)title.ptr, NULL, NULL);
#if UM_DEBUG
      if (!win) {
        fprintf(stderr,
                "WINDOW ERROR: rt_window_create: glfwCreateWindow failed\n");
        print_glfw_error();
        abort();
      }
#endif
      glfwSetWindowUserPointer(win, (void *)(uintptr_t)i);
      glfwSetScrollCallback(win, scroll_callback);
      g_windows[i].win = win;
      g_windows[i].live = 1;
      input_reset(&g_windows[i].input);
      return make_handle(i, g_windows[i].gen);
    }
  }

#if UM_DEBUG
  fprintf(stderr,
          "WINDOW ERROR: rt_window_create: no free window slots "
          "(UM_MAX_WINDOWS=%d)\n",
          UM_MAX_WINDOWS);
  abort();
#endif
  return 0;
}

void rt_window_destroy(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_window_destroy");
  if (!e) return;

  glfwDestroyWindow(e->win);
  e->win = NULL;
  e->live = 0;
  e->gen++;
  input_reset(&e->input);
}

void rt_window_poll_events(void) { glfwPollEvents(); }

int32_t rt_window_should_close(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_window_should_close");
  if (!e) return 1;
  return glfwWindowShouldClose(e->win);
}

void rt_window_request_close(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_window_request_close");
  if (!e) return;
  glfwSetWindowShouldClose(e->win, GLFW_TRUE);
}

void rt_input_begin_frame(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_input_begin_frame");
  if (!e) return;

  GLFWwindow *win = e->win;
  um_input_state_t *input = &e->input;

  input->wheel_x = 0.0f;
  input->wheel_y = 0.0f;

  memcpy(input->prev_keys, input->keys, sizeof(input->keys));
  memset(input->keys, 0, sizeof(input->keys));
  for (uint32_t gk = 0; gk <= GLFW_KEY_LAST; ++gk) {
    um_key_t uk = g_glfw_key_to_um[gk];
    if (uk == UM_KEY_UNKNOWN) continue;
    int32_t down = glfwGetKey(win, gk) == GLFW_PRESS;
    kb_bitset_set_to(input->keys, uk, down);
  }

  input->prev_mouse_buttons = input->mouse_buttons;
  input->mouse_buttons = 0;
  for (uint32_t gmb = 0; gmb <= GLFW_MOUSE_BUTTON_LAST; ++gmb) {
    um_mouse_button_t umb = g_glfw_mouse_to_um[gmb];
    if (umb == UM_MOUSE_BUTTON_UNKNOWN) continue;
    int32_t down = glfwGetMouseButton(win, gmb) == GLFW_PRESS;
    mb_bitset_set_to(&input->mouse_buttons, umb, down);
  }

  input->prev_mouse_x = input->mouse_x;
  input->prev_mouse_y = input->mouse_y;
  glfwGetCursorPos(win, &input->mouse_x, &input->mouse_y);
  input->mouse_delta_x = input->mouse_x - input->prev_mouse_x;
  input->mouse_delta_y = input->mouse_y - input->prev_mouse_y;
}

int32_t rt_get_key_down(um_window_handle_t h, um_key_t k) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_key_down");
  if (!e) return 0;
#if UM_DEBUG
  if (!key_valid(k)) {
    trap_bad_key("rt_get_key_down", k);
    return 0;
  }
#endif
  return kb_bitset_test(e->input.keys, (uint32_t)k);
}

int32_t rt_get_key_up(um_window_handle_t h, um_key_t k) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_key_up");
  if (!e) return 0;
#if UM_DEBUG
  if (!key_valid(k)) {
    trap_bad_key("rt_get_key_up", k);
    return 0;
  }
#endif
  return !kb_bitset_test(e->input.keys, (uint32_t)k);
}

int32_t rt_get_key_pressed(um_window_handle_t h, um_key_t k) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_key_pressed");
  if (!e) return 0;
#if UM_DEBUG
  if (!key_valid(k)) {
    trap_bad_key("rt_get_key_pressed", k);
    return 0;
  }
#endif
  return kb_bitset_test(e->input.keys, (uint32_t)k) &&
         !kb_bitset_test(e->input.prev_keys, (uint32_t)k);
}

int32_t rt_get_key_released(um_window_handle_t h, um_key_t k) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_key_released");
  if (!e) return 0;
#if UM_DEBUG
  if (!key_valid(k)) {
    trap_bad_key("rt_get_key_released", k);
    return 0;
  }
#endif
  return !kb_bitset_test(e->input.keys, (uint32_t)k) &&
         kb_bitset_test(e->input.prev_keys, (uint32_t)k);
}

int32_t rt_get_mouse_button_down(um_window_handle_t h, um_mouse_button_t b) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_button_down");
  if (!e) return 0;
#if UM_DEBUG
  if (!mouse_button_valid(b)) {
    trap_bad_mouse_button("rt_get_mouse_button_down", b);
    return 0;
  }
#endif
  return mb_bitset_test(e->input.mouse_buttons, b);
}

int32_t rt_get_mouse_button_up(um_window_handle_t h, um_mouse_button_t b) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_button_up");
  if (!e) return 0;
#if UM_DEBUG
  if (!mouse_button_valid(b)) {
    trap_bad_mouse_button("rt_get_mouse_button_up", b);
    return 0;
  }
#endif
  return !mb_bitset_test(e->input.mouse_buttons, b);
}

int32_t rt_get_mouse_button_pressed(um_window_handle_t h, um_mouse_button_t b) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_button_pressed");
  if (!e) return 0;
#if UM_DEBUG
  if (!mouse_button_valid(b)) {
    trap_bad_mouse_button("rt_get_mouse_button_pressed", b);
    return 0;
  }
#endif
  return mb_bitset_test(e->input.mouse_buttons, b) &&
         !mb_bitset_test(e->input.prev_mouse_buttons, b);
}

int32_t rt_get_mouse_button_released(um_window_handle_t h,
                                     um_mouse_button_t b) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_button_released");
  if (!e) return 0;
#if UM_DEBUG
  if (!mouse_button_valid(b)) {
    trap_bad_mouse_button("rt_get_mouse_button_released", b);
    return 0;
  }
#endif
  return !mb_bitset_test(e->input.mouse_buttons, b) &&
         mb_bitset_test(e->input.prev_mouse_buttons, b);
}

double rt_get_mouse_x(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_x");
  return e ? e->input.mouse_x : 0.0;
}

double rt_get_mouse_y(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_y");
  return e ? e->input.mouse_y : 0.0;
}

double rt_get_mouse_delta_x(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_delta_x");
  return e ? e->input.mouse_delta_x : 0.0;
}

double rt_get_mouse_delta_y(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_delta_y");
  return e ? e->input.mouse_delta_y : 0.0;
}

float rt_get_mouse_wheel_x(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_wheel_x");
  return e ? e->input.wheel_x : 0.0f;
}

float rt_get_mouse_wheel_y(um_window_handle_t h) {
  um_window_entry_t *e = get_window_entry(h, "rt_get_mouse_wheel_y");
  return e ? e->input.wheel_y : 0.0f;
}

#ifndef UM_TESTING
__attribute__((constructor)) static void window_rt_init(void) {
  um_init_key_tables();
  um_init_mouse_button_tables();
  if (!glfwInit()) {
#if UM_DEBUG
    fprintf(stderr, "WINDOW ERROR: glfwInit() failed\n");
    print_glfw_error();
    abort();
#endif
  }
}
#endif

#ifdef UM_TESTING
void rt_window_init_for_testing(void) {
  um_init_key_tables();
  um_init_mouse_button_tables();
  memset(g_windows, 0, sizeof(g_windows));
}

um_window_handle_t rt_window_inject_for_testing(void) {
  for (int i = 1; i < UM_MAX_WINDOWS; ++i) {
    if (!g_windows[i].live) {
      // use a non-null sentinel so get_window_entry doesn't reject it;
      // the pointer is never dereferenced in the query paths under UM_TESTING.
      g_windows[i].win = (GLFWwindow *)1;
      g_windows[i].live = 1;
      input_reset(&g_windows[i].input);
      return make_handle(i, g_windows[i].gen);
    }
  }
  return 0;
}

void rt_window_set_key_for_testing(um_window_handle_t h, um_key_t k,
                                   int32_t down, int32_t prev_down) {
  uint32_t idx = h_index(h);
  kb_bitset_set_to(g_windows[idx].input.keys, k, down);
  kb_bitset_set_to(g_windows[idx].input.prev_keys, k, prev_down);
}

void rt_window_set_mouse_button_for_testing(um_window_handle_t h,
                                            um_mouse_button_t b, int32_t down,
                                            int32_t prev_down) {
  uint32_t idx = h_index(h);
  mb_bitset_set_to(&g_windows[idx].input.mouse_buttons, b, down);
  mb_bitset_set_to(&g_windows[idx].input.prev_mouse_buttons, b, prev_down);
}

void rt_window_set_mouse_pos_for_testing(um_window_handle_t h, double x,
                                         double y, double dx, double dy) {
  uint32_t idx = h_index(h);
  g_windows[idx].input.mouse_x = x;
  g_windows[idx].input.mouse_y = y;
  g_windows[idx].input.mouse_delta_x = dx;
  g_windows[idx].input.mouse_delta_y = dy;
}

void rt_window_set_wheel_for_testing(um_window_handle_t h, float wx, float wy) {
  uint32_t idx = h_index(h);
  g_windows[idx].input.wheel_x = wx;
  g_windows[idx].input.wheel_y = wy;
}

void rt_window_reset_for_testing(void) {
  memset(g_windows, 0, sizeof(g_windows));
}
#endif
