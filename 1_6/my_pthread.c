#define _GNU_SOURCE

#include "my_pthread.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>

#define sleep_ms(ms) nanosleep(&(struct timespec){.tv_sec = (ms) / 1000, .tv_nsec = ((ms) % 1000) * 1000000L}, NULL)

#define THREAD_STACK_SIZE (8 * 1024 * 1024)
#define LINUX_PAGE_SIZE 4096
#define JOIN_SLEEP_MS 200

#define THREAD_CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID)

#define TO_MY_PTHREAD_T(ptr) ((my_pthread_t)(uintptr_t)(ptr))
#define MAIN_THREAD_T (TO_MY_PTHREAD_T(NULL))

#define TO_THREAD_ARG(thread_id) ((thread_arg_t *)(uintptr_t)thread_id)

typedef struct
{
    pid_t tid;
    void *stack;
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    atomic_bool is_finished;
} thread_arg_t;

static int thread_task(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;

    targ->retval = targ->start_routine(targ->arg);
    targ->is_finished = true;
    return 0;
}

int my_pthread_create(my_pthread_t *new_thread,
                      void *(*__start_routine)(void *),
                      void *arg)
{
    void *stack = mmap(NULL, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED)
    {
        return ENOMEM;
    }

    int err = madvise(stack, LINUX_PAGE_SIZE, MADV_GUARD_INSTALL);
    if (err != 0)
    {
        munmap(stack, THREAD_STACK_SIZE);
        return errno;
    }

    uintptr_t stack_top = (uintptr_t)stack + THREAD_STACK_SIZE;
    stack_top -= sizeof(thread_arg_t);

    // 16 byte aligned
    stack_top &= ~((uintptr_t)15);

    thread_arg_t *targ = (thread_arg_t *)stack_top;
    targ->stack = stack;
    targ->start_routine = __start_routine;
    targ->arg = arg;
    targ->retval = NULL;
    targ->is_finished = false;

    pid_t tid = clone(thread_task,
                      (void *)stack_top,
                      THREAD_CLONE_FLAGS,
                      targ,
                      &targ->tid);

    if (tid == -1)
    {
        munmap(stack, THREAD_STACK_SIZE);
        return EAGAIN;
    }

    *new_thread = TO_MY_PTHREAD_T(stack_top);
    return 0;
}

int my_pthread_cancel(my_pthread_t thread_id)
{
    thread_arg_t *targ = TO_THREAD_ARG(thread_id);
    if (targ == NULL)
    {
        return EINVAL;
    }

    int err = tgkill(getpid(), targ->tid, SIGKILL);
    return (err == 0) ? 0 : errno;
}

int my_pthread_join(my_pthread_t thread_id, void **retval)
{
    if (thread_id == MAIN_THREAD_T)
    {
        return EINVAL;
    }

    thread_arg_t *targ = TO_THREAD_ARG(thread_id);
    if (targ->tid == gettid())
    {
        return EDEADLK;
    }
    while (!atomic_load_explicit(&targ->is_finished, memory_order_acquire))
    {
        sleep_ms(JOIN_SLEEP_MS);
    }

    if (retval != NULL)
    {
        *retval = targ->retval;
    }
    munmap(targ->stack, THREAD_STACK_SIZE);
    return 0;
}