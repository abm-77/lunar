// stubs for GLFW functions referenced by window.c under UM_TESTING.
// none of the stubbed functions are actually called during tests — they exist
// only to satisfy the linker.
#include <GLFW/glfw3.h>

int          glfwInit(void)                                                  { return GLFW_TRUE; }
void         glfwTerminate(void)                                             {}
int          glfwGetError(const char **desc)                                 { if (desc) *desc = 0; return 0; }
void         glfwWindowHint(int hint, int value)                             { (void)hint;(void)value; }
GLFWwindow  *glfwCreateWindow(int w, int h, const char *t,
                               GLFWmonitor *m, GLFWwindow *s)                { (void)w;(void)h;(void)t;(void)m;(void)s; return 0; }
void         glfwDestroyWindow(GLFWwindow *w)                                { (void)w; }
int          glfwWindowShouldClose(GLFWwindow *w)                            { (void)w; return 0; }
void         glfwSetWindowShouldClose(GLFWwindow *w, int v)                  { (void)w;(void)v; }
void         glfwPollEvents(void)                                             {}
void         glfwSetWindowUserPointer(GLFWwindow *w, void *p)                { (void)w;(void)p; }
void        *glfwGetWindowUserPointer(GLFWwindow *w)                         { (void)w; return 0; }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow *w, GLFWscrollfun cb)         { (void)w;(void)cb; return 0; }
int          glfwGetKey(GLFWwindow *w, int k)                                { (void)w;(void)k; return 0; }
int          glfwGetMouseButton(GLFWwindow *w, int b)                        { (void)w;(void)b; return 0; }
void         glfwGetCursorPos(GLFWwindow *w, double *x, double *y)           { (void)w; if (x) *x=0; if (y) *y=0; }
