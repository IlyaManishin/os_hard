#define _GNU_SOURCE

#include "my_pthread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRINT_PERIOD_MS 100

static void sleep_ms(long ms)
{
    nanosleep(&(struct timespec){.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L}, NULL);
}

void *mythread(void *arg)
{
    (void)arg;

    while (1)
    {
        printf("mythread [%d]: running inside loop...\n", gettid());
        sleep_ms(PRINT_PERIOD_MS);
    }

    return NULL;
}

int main(void)
{
    my_pthread_t pthread_id;
    int err;

    err = mythread_create(&pthread_id, mythread, NULL);
    if (err != 0)
    {
        printf("main: mythread_create() failed: %s\n", strerror(err));
        return EXIT_FAILURE;
    }

    sleep_ms(PRINT_PERIOD_MS * 10);

    printf("main: sending cancellation request...\n");
    err = my_pthread_cancel(pthread_id);
    if (err != 0)
    {
        printf("main: my_pthread_cancel() failed: %s\n", strerror(err));
        return EXIT_FAILURE;
    }

    void *res = NULL;
    
    err = mythread_join(pthread_id, &res);
    if (err != 0)
    {
        printf("main: mythread_join() failed: %s\n", strerror(err));
        return EXIT_FAILURE;
    }
    printf("Second join:\n");
    err = mythread_join(pthread_id, &res);
    if (err != 0)
    {
        printf("main: mythread_join() failed: %s\n", strerror(err));
        return EXIT_FAILURE;
    }

    printf("main: thread joined successfully\n");

    return EXIT_SUCCESS;
}