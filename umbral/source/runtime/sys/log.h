#pragma once
// log.h — centralized runtime logging.
// log levels: 0=off, 1=error, 2=warn, 3=info, 4=debug, 5=trace.
// in release builds (UM_DEBUG=0), all logging compiles to nothing.
// in debug builds, the log level is controlled by the UM_LOG_LEVEL env var
// at startup, defaulting to 3 (info).
//
// usage:
//   RT_LOG_ERROR("asset", "cannot open '%s'", path);
//   RT_LOG_INFO("gfx", "selected GPU: %s", name);
//   RT_LOG_DEBUG("gfx", "texture slot=%u handle=0x%lx", slot, handle);
//   RT_LOG_TRACE("gfx", "submit_draw: pipe_slot=%u packets=%u", slot, count);

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RT_LOG_LEVEL_OFF   = 0,
    RT_LOG_LEVEL_ERROR = 1,
    RT_LOG_LEVEL_WARN  = 2,
    RT_LOG_LEVEL_INFO  = 3,
    RT_LOG_LEVEL_DEBUG = 4,
    RT_LOG_LEVEL_TRACE = 5,
};

// call once at startup; reads UM_LOG_LEVEL env var. safe to skip — defaults to info.
void rt_log_init(void);

// current log level; set by rt_log_init or directly
extern int rt_log_level;

#ifdef __cplusplus
}
#endif

#ifndef UM_DEBUG
#define UM_DEBUG 1
#endif

#if UM_DEBUG

#define RT_LOG(level, tag, fmt, ...)                                            \
    do {                                                                        \
        if (rt_log_level >= (level))                                            \
            fprintf(stderr, "[" tag "] " fmt "\n", ##__VA_ARGS__);             \
    } while (0)

#define RT_LOG_ERROR(tag, fmt, ...) RT_LOG(RT_LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define RT_LOG_WARN(tag, fmt, ...)  RT_LOG(RT_LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define RT_LOG_INFO(tag, fmt, ...)  RT_LOG(RT_LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define RT_LOG_DEBUG(tag, fmt, ...) RT_LOG(RT_LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define RT_LOG_TRACE(tag, fmt, ...) RT_LOG(RT_LOG_LEVEL_TRACE, tag, fmt, ##__VA_ARGS__)

#else

#define RT_LOG_ERROR(tag, fmt, ...) ((void)0)
#define RT_LOG_WARN(tag, fmt, ...)  ((void)0)
#define RT_LOG_INFO(tag, fmt, ...)  ((void)0)
#define RT_LOG_DEBUG(tag, fmt, ...) ((void)0)
#define RT_LOG_TRACE(tag, fmt, ...) ((void)0)

#endif
