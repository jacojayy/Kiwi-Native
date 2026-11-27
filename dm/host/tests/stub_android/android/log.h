// PEANUTS: <android/log.h> for the tests that build an Android-side file on a Linux PC (t_peanuts_host.c): the log goes to stderr
#pragma once
#include <stdarg.h>
#include <stdio.h>
enum { ANDROID_LOG_VERBOSE = 2, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL };
__attribute__((format(printf, 3, 4))) static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); flockfile(stderr);
    fprintf(stderr, "[log %d %s] ", prio, tag); int n = vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    funlockfile(stderr); va_end(ap); return n;
}
