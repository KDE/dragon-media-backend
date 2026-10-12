/* SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <time.h>

extern "C" int pthread_clockjoin_np(pthread_t thread, void **retval, clockid_t clockid, const struct timespec *abstime)
{
    for (;;) {
        int result = pthread_tryjoin_np(thread, retval);
        if (result != EBUSY)
            return result;
        if (abstime) {
            struct timespec now;
            if (clock_gettime(clockid, &now) == 0) {
                if (now.tv_sec > abstime->tv_sec || (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
                    return ETIMEDOUT;
            }
        }
        struct timespec pause = {0, 2 * 1000 * 1000};
        nanosleep(&pause, nullptr);
    }
}
#endif
