#include <stdarg.h>
#include <stdio.h>

#include "dk64vr_log.h"

static dk64vr_log_sink_t sink = NULL;

void dk64vr_log_set_sink(dk64vr_log_sink_t s) {
	sink = s;
}

static void emit(const char *prefix, const char *fmt, va_list args) {
	char line[1024];
	vsnprintf(line, sizeof(line), fmt, args);
	if (sink) {
		sink(line);
		return;
	}
	printf("%s %s\n", prefix, line);
	fflush(stdout);
}

void dk64vr_logf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	emit("[vr]", fmt, args);
	va_end(args);
}

void dk64vr_logpf(const char *prefix, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	emit(prefix, fmt, args);
	va_end(args);
}
