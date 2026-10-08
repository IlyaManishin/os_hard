#define _GNU_SOURCE

#include "my_pthread.h"
#include "exit_storage.h"

#include <setjmp.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define THREAD_STACK_SIZE (8 * 1024 * 1024)
#define LINUX_PAGE_SIZE 4096
#define MAX_THREADS 1024

#define THREAD_CANCEL_SIGNAL SIGUSR1
#define THREAD_CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                            CLONE_THREAD | CLONE_SYSVSEM | CLONE_CHILD_SETTID | \
                            CLONE_CHILD_CLEARTID)

#define TO_MY_PTHREAD_T(ptr) ((my_pthread_t)(uintptr_t)(ptr))
#define MAIN_THREAD_T (TO_MY_PTHREAD_T(NULL))

#define SLOT_BUSY_VAL 1
#define TO_THREAD_SLOT(thread_id) ((pthread_slot_t *)(uintptr_t)thread_id)

static bool isPthreadInit = false;

typedef struct
{
    volatile pid_t tid;
    void *stack;
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    atomic_bool is_canceled;
    atomic_bool is_detached;
} thread_arg_t;

typedef _Atomic(thread_arg_t *) pthread_slot_t;

static pthread_slot_t thread_table[MAX_THREADS] = {NULL};

static int thread_clear(thread_arg_t *targ)
{
    void *stack = targ->stack;
    if (munmap(stack, THREAD_STACK_SIZE) == -1)
    {
        return errno;
    }
    return 0;
}

static int thread_task(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    pid_t tid = gettid();

    jmp_buf env;
    if (setjmp(env) == 0)
    {
        exit_storage_set(tid, env, &targ->retval);
        targ->retval = targ->start_routine(targ->arg);
    }
    exit_storage_remove(tid);

    return 0;
}

static void thread_cancel_handler(int sig)
{
    (void)sig;
    exit_storage_remove(gettid());
    syscall(SYS_exit, 0);
}

static int my_pthread_init(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = thread_cancel_handler;
    return sigaction(THREAD_CANCEL_SIGNAL, &sa, NULL);
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_THREADS; i++)
    {
        thread_arg_t *expected = NULL;
        if (atomic_compare_exchange_strong(&thread_table[i], &expected, (thread_arg_t *)SLOT_BUSY_VAL))
        {
            return i;
        }
    }
    return -1;
}

int mythread_create(my_pthread_t *new_thread,
                      void *(*__start_routine)(void *),
                      void *arg)
{
    if (!isPthreadInit)
    {
        int err = my_pthread_init();
        if (err != 0)
        {
            return EAGAIN;
        }
        isPthreadInit = true;
    }

    int slot_idx = find_free_slot();
    if (slot_idx == -1)
    {
        return EAGAIN;
    }

    void *stack = mmap(NULL, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED)
    {
        atomic_store(&thread_table[slot_idx], NULL);
        return ENOMEM;
    }

    int err = madvise(stack, LINUX_PAGE_SIZE, MADV_GUARD_INSTALL);
    if (err != 0)
    {
        atomic_store(&thread_table[slot_idx], NULL);
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
    targ->is_canceled = false;
    targ->tid = -1;

    atomic_store(&thread_table[slot_idx], targ);

    pid_t tid = clone(thread_task,
                      (void *)stack_top,
                      THREAD_CLONE_FLAGS,
                      targ,
                      NULL,
                      NULL,
                      &targ->tid);

    if (tid == -1)
    {
        atomic_store(&thread_table[slot_idx], NULL);
        munmap(stack, THREAD_STACK_SIZE);
        return EAGAIN;
    }

    *new_thread = TO_MY_PTHREAD_T(&thread_table[slot_idx]);
    return 0;
}

int my_pthread_cancel(my_pthread_t thread_id)
{
    pthread_slot_t *slot = TO_THREAD_SLOT(thread_id);
    if (slot == NULL)
    {
        return EINVAL;
    }

    thread_arg_t *targ = atomic_load_explicit(slot, memory_order_acquire);
    if (targ == NULL)
    {
        return ESRCH;
    }

    pid_t target_tid = targ->tid;
    if (target_tid == 0)
    {
        return ESRCH;
    }

    atomic_store_explicit(&targ->is_canceled, true, memory_order_release);

    int err = tgkill(getpid(), target_tid, THREAD_CANCEL_SIGNAL);
    return (err == 0) ? 0 : errno;
}

int mythread_join(my_pthread_t thread_id, void **retval)
{
    if (thread_id == MAIN_THREAD_T)
    {
        return EINVAL;
    }

    pthread_slot_t *slot = TO_THREAD_SLOT(thread_id);
    if (slot == NULL)
    {
        return EINVAL;
    }

    thread_arg_t *targ = atomic_exchange_explicit(slot, NULL, memory_order_acq_rel);
    if (targ == NULL)
    {
        return EINVAL;
    }

    if (targ->tid == gettid())
    {
        atomic_store_explicit(slot, targ, memory_order_release);
        return EDEADLK;
    }

    while (1)
    {
        pid_t cur_tid = targ->tid;
        if (cur_tid == 0)
        {
            break;
        }
        syscall(SYS_futex, &targ->tid, FUTEX_WAIT, cur_tid, NULL, NULL, 0);
    }

    if (retval != NULL)
    {
        if (atomic_load_explicit(&targ->is_canceled, memory_order_acquire))
        {
            *retval = PTHREAD_CANCELED;
        }
        else
        {
            *retval = targ->retval;
        }
    }

    int res = thread_clear(targ);
    return res;
}

int mythread_detach(my_pthread_t thread_id)
{
    if (thread_id == MAIN_THREAD_T)
    {
        return EINVAL;
    }
    return 0;
}

void mythread_exit(void *retval)
{
    exit_storage_exit(gettid(), retval);
}