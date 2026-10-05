// "[vr] ..." state-change log lines on stdout (the launcher sends stdout to run-stdout.txt).
// Tests capture lines with a sink.
#ifndef DK64VR_LOG_H
#define DK64VR_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define DK64VR_PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define DK64VR_PRINTF_LIKE(a, b)
#endif

typedef void (*dk64vr_log_sink_t)(const char *line);

// "[vr] <line>"
void dk64vr_logf(const char *fmt, ...) DK64VR_PRINTF_LIKE(1, 2);
// "<prefix> <line>", for [vr-perf], [vr-stall] and [openxr] lines.
void dk64vr_logpf(const char *prefix, const char *fmt, ...) DK64VR_PRINTF_LIKE(2, 3);
void dk64vr_log_set_sink(dk64vr_log_sink_t sink);

#ifdef __cplusplus
}
#endif

#endif
