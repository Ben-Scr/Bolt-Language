#pragma once
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static int64_t bench_now(void) {
    LARGE_INTEGER value;
    if (!QueryPerformanceCounter(&value)) abort();
    return value.QuadPart;
}
static int64_t bench_to_ticks(int64_t delta) {
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) abort();
    return delta / frequency.QuadPart * 10000000 + delta % frequency.QuadPart * 10000000 / frequency.QuadPart;
}
#else
#include <time.h>
static int64_t bench_now(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static int64_t bench_to_ticks(int64_t delta) { return delta / 100; }
#endif
