#define _GNU_SOURCE

#include "intercept.h"
#include "mythread.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

static long (*real_syscall)(long, ...);

long syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);
    long a6 = va_arg(ap, long);
    va_end(ap);

    if (real_syscall == NULL)
    {
        real_syscall = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    }

    long ret = real_syscall(number, a1, a2, a3, a4, a5, a6);

    int saved_errno = errno;
    preempt_point();
    errno = saved_errno;

    return ret;
}

int usleep(useconds_t usec)
{
    if (usec == 0)
    {
        mythread_yield();
        return 0;
    }

    unsigned int ms = (unsigned int)(usec / 1000);
    if (ms == 0)
    {
        ms = 1;
    }

    mythread_sleep_ms(ms);
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    if (seconds == 0)
    {
        mythread_yield();
        return 0;
    }

    mythread_sleep_ms(seconds * 1000);
    return 0;
}
