#define _GNU_SOURCE

#include "my_pthread.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define THREAD_SLEEP_SEC 2

void *mythread(void *arg)
{
    (void)arg;

    printf("mythread [%d %d %d]: Hello from mythread!\n", getpid(), getppid(), gettid());

    sleep(THREAD_SLEEP_SEC);

    my_pthread_exit((void *)(intptr_t)42);
    return (void *)(intptr_t)42;
}

int main(void)
{
    my_pthread_t tid;
    int err;

    printf("main [%d %d %d]: Hello from main!\n", getpid(), getppid(), gettid());

    err = my_pthread_create(&tid, mythread, NULL);
    if (err != 0)
    {
        printf("main: my_pthread_create() failed: %s\n", strerror(err));
        return EXIT_FAILURE;
    }

    void *ret_val = NULL;

    err = my_pthread_join(tid, &ret_val);
    if (err != 0)
    {
        printf("main: my_pthread_join() failed: %s\n", strerror(err));
        return EXIT_FAILURE;
    }

    printf("Thread returned number: %d\n", (int)(intptr_t)ret_val);

    return EXIT_SUCCESS;
}