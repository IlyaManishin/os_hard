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
#include <unistd.h>

#define STACK_SIZE (8 * 1024 * 1024)
#define LINUX_PAGE_SIZE 4096
#define THREAD_CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID)

#define TO_MY_PTHREAD_T(ptr) ((my_pthread_t)(uintptr_t)(ptr))
#define MAIN_THREAD_T (TO_MY_PTHREAD_T(NULL))

#define TO_THREAD_ARG(thread_id) ((thread_arg_t *)thread_id)

typedef struct
{
    pid_t tid;
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    bool is_finished;
} thread_arg_t;

static int thread_task(uintptr_t stack_top)
{
    thread_arg_t *targ = (thread_arg_t *)stack_top;

    targ->retval = targ->start_routine(targ->arg);
    targ->is_finished = true;
    return 0;
}

int my_pthread_create(my_pthread_t *new_thread,
                      void *(*__start_routine)(void *),
                      void *arg)
{
    void *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED)
    {
        return ENOMEM;
    }

    int err = madvise(stack, LINUX_PAGE_SIZE, MADV_GUARD_INSTALL);
    if (err != 0)
    {
        munmap(stack, STACK_SIZE);
        return EPERM;
    }

    uintptr_t stack_top = (uintptr_t)stack + STACK_SIZE;
    stack_top -= sizeof(thread_arg_t);

    // 16 byte aligned
    stack_top &= ~((uintptr_t)15);

    thread_arg_t *targ = (thread_arg_t *)stack_top;
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
        munmap(stack, STACK_SIZE);
        return EAGAIN;
    }
    *new_thread = TO_MY_PTHREAD_T(stack_top);
    return 0;
}

int my_pthread_cancel(my_pthread_t thread_id)
{
    thread_arg_t *targ = TO_THREAD_ARG(thread_id);
    
    int err = kill(SIGKILL, targ->tid);
    return err;
}

int my_pthread_join(my_pthread_t thread_id, void **res)
{
    if (thread_id == MAIN_THREAD_T)
    {
        return EINVAL;
    }
    return 0;
}
