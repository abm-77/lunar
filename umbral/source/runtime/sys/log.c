#include "log.h"
#include <stdlib.h>

int rt_log_level = RT_LOG_LEVEL_INFO;

void rt_log_init(void) {
    const char *env = getenv("UM_LOG_LEVEL");
    if (env) {
        int v = atoi(env);
        if (v >= RT_LOG_LEVEL_OFF && v <= RT_LOG_LEVEL_TRACE)
            rt_log_level = v;
    }
}
