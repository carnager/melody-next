// SPDX-License-Identifier: GPL-3.0-only

// ThreadSanitizer has no interceptor for pthread_clockjoin_np
// (llvm/llvm-project#146683), yet Qt >= 6.9 joins every QThread through it.
// Joined threads therefore never leave the sanitizer's live-thread registry,
// and once glibc hands out a recycled pthread id the runtime aborts with
// "CHECK failed: sanitizer_thread_registry.cpp". This translation unit is
// linked into every executable of the tsan preset only (see
// cmake/Sanitizers.cmake) and preempts the glibc symbol with a forwarder to
// the join functions ThreadSanitizer does intercept. Remove it once the
// upstream interceptor ships.

#if defined(MELODY_THREAD_SANITIZER) && defined(__linux__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <ctime>

#include <pthread.h>

extern "C" int pthread_clockjoin_np(pthread_t thread, void** thread_return, clockid_t clock_id,
                                    const struct timespec* abstime) {
    if (abstime == nullptr)
        return pthread_join(thread, thread_return);
    if (clock_id == CLOCK_REALTIME)
        return pthread_timedjoin_np(thread, thread_return, abstime);
    // pthread_timedjoin_np measures its deadline against CLOCK_REALTIME, so
    // rebase the caller's deadline from its clock onto the realtime clock.
    struct timespec source_now{};
    if (clock_gettime(clock_id, &source_now) != 0)
        return errno;
    struct timespec realtime_now{};
    if (clock_gettime(CLOCK_REALTIME, &realtime_now) != 0)
        return errno;
    constexpr long nanoseconds_per_second = 1'000'000'000L;
    struct timespec deadline{};
    deadline.tv_sec = realtime_now.tv_sec + (abstime->tv_sec - source_now.tv_sec);
    deadline.tv_nsec = realtime_now.tv_nsec + (abstime->tv_nsec - source_now.tv_nsec);
    while (deadline.tv_nsec >= nanoseconds_per_second) {
        deadline.tv_nsec -= nanoseconds_per_second;
        ++deadline.tv_sec;
    }
    while (deadline.tv_nsec < 0) {
        deadline.tv_nsec += nanoseconds_per_second;
        --deadline.tv_sec;
    }
    return pthread_timedjoin_np(thread, thread_return, &deadline);
}

#endif
