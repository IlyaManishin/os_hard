#define _GNU_SOURCE

#include "mythread.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void busy_ms(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long end = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000 + ms;

    for (;;)
    {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec * 1000LL + ts.tv_nsec / 1000000 >= end)
        {
            return;
        }
    }
}

void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;

    for (int i = 0; i < 4; i++)
    {
        busy_ms(15);
        printf("worker %d: step %d\n", id, i);

        syscall(SYS_getpid); // точка переключения контекста
    }

    return (void *)(intptr_t)(id * 10);
}

void *sleeper(void *arg)
{
    (void)arg;

    printf("sleeper: go to sleep\n");
    mythread_sleep_ms(100);
    printf("sleeper: woke up\n");

    return (void *)(intptr_t)7;
}

void *detached_task(void *arg)
{
    (void)arg;

    printf("detached: hello, thread %lu\n", mythread_self());
    return NULL;
}

void *victim(void *arg)
{
    (void)arg;

    for (;;)
    {
        busy_ms(5);
        syscall(SYS_getpid);
    }

    return NULL;
}

int main(void)
{
    mythread_t workers[3], sl, vic;
    int err;

    printf("main [%d]: Hello from main!\n", getpid());

    for (int i = 0; i < 3; i++)
    {
        err = mythread_create(&workers[i], NULL, worker, (void *)(intptr_t)(i + 1));
        if (err != 0)
        {
            printf("main: mythread_create() failed: %s\n", strerror(err));
            return EXIT_FAILURE;
        }
    }

    err = mythread_create(&sl, NULL, sleeper, NULL);
    err |= mythread_create(&vic, NULL, victim, NULL);
    if (err != 0)
    {
        printf("main: mythread_create() failed\n");
        return EXIT_FAILURE;
    }

    mythread_t det;
    err = mythread_create(&det, NULL, detached_task, NULL);
    if (err != 0 || mythread_detach(det) != 0)
    {
        printf("main: detached thread failed\n");
        return EXIT_FAILURE;
    }
    printf("main: join detached -> %s\n", strerror(mythread_join(det, NULL)));

    mythread_sleep_ms(50);
    printf("main: cancel victim\n");
    mythread_cancel(vic);

    void *ret_val = NULL;

    for (int i = 0; i < 3; i++)
    {
        err = mythread_join(workers[i], &ret_val);
        if (err != 0)
        {
            printf("main: mythread_join() failed: %s\n", strerror(err));
            return EXIT_FAILURE;
        }
        printf("worker %d returned: %d\n", i + 1, (int)(intptr_t)ret_val);
    }

    mythread_join(sl, &ret_val);
    printf("sleeper returned: %d\n", (int)(intptr_t)ret_val);

    mythread_join(vic, &ret_val);
    printf("victim canceled: %s\n", ret_val == MYTHREAD_CANCELED ? "yes" : "no");

    return EXIT_SUCCESS;
}
