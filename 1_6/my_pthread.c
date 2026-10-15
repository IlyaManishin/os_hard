#define _GNU_SOURCE

#include "my_pthread.h"
#include "exit_storage.h"

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <setjmp.h>
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

static atomic_bool isPthreadInit = false;

#define THREAD_UNMAP_AND_EXIT(stack_ptr, stack_size)                \
    do                                                              \
    {                                                               \
        __asm__ volatile(                                           \
            "syscall\n\t"                                           \
            "xorl %%edi, %%edi\n\t"                                 \
            "movl %[nr_exit], %%eax\n\t"                            \
            "syscall\n\t"                                           \
            :                                                       \
            : "a"((long)SYS_munmap), "D"(stack_ptr), "S"(stack_size), \
              [nr_exit] "i"(SYS_exit)                               \
            : "rcx", "r11", "memory");                              \
        __builtin_unreachable();                                    \
    } while (0)

typedef _Atomic(thread_arg_t *) pthread_slot_t;

struct thread_arg
{
    volatile pid_t tid;
    void *stack;
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    jmp_buf env;
    pthread_slot_t *slot;
    atomic_bool is_canceled;
    atomic_bool is_detached;
};

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

static void thread_release(thread_arg_t *targ)
{
    exit_storage_remove(gettid());

    if (atomic_load_explicit(&targ->is_detached, memory_order_acquire))
    {
        void *stack = targ->stack;
        size_t stack_size = THREAD_STACK_SIZE;

        atomic_store_explicit(targ->slot, NULL, memory_order_release);
        syscall(SYS_set_tid_address, NULL);
        THREAD_UNMAP_AND_EXIT(stack, stack_size);
    }
}

static int thread_task(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;

    exit_storage_set(gettid(), targ);
    if (setjmp(targ->env) == 0)
    {
        targ->retval = targ->start_routine(targ->arg);
    }
    thread_release(targ);

    return 0;
}

static void thread_cancel_handler(int sig)
{
    (void)sig;
    thread_arg_t *targ = exit_storage_get(gettid());
    if (targ != NULL)
    {
        thread_release(targ);
    }
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
    bool expInit = false;
    bool desInit = true;
    if (atomic_compare_exchange_strong(&isPthreadInit, &expInit, desInit))
    {
        int err = my_pthread_init();
        if (err != 0)
        {
            atomic_store(&isPthreadInit, false);
            return EAGAIN;
        }
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
    targ->slot = &thread_table[slot_idx];
    targ->is_canceled = false;
    targ->is_detached = false;
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

    if (atomic_load_explicit(&targ->is_detached, memory_order_acquire))
    {
        atomic_store_explicit(slot, targ, memory_order_release);
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

    pthread_slot_t *slot = TO_THREAD_SLOT(thread_id);
    thread_arg_t *targ = atomic_load_explicit(slot, memory_order_acquire);
    if (targ == NULL)
    {
        return EINVAL;
    }

    if (atomic_exchange_explicit(&targ->is_detached, true, memory_order_acq_rel))
    {
        return EINVAL;
    }

    if (targ->tid == 0)
    {
        atomic_store_explicit(slot, NULL, memory_order_release);
        return thread_clear(targ);
    }
    return 0;
}

void mythread_exit(void *retval)
{
    thread_arg_t *targ = exit_storage_get(gettid());
    if (targ != NULL)
    {
        targ->retval = retval;
        longjmp(targ->env, 1);
    }
    syscall(SYS_exit, 0);
}

my_pthread_t mythread_self(void)
{
    thread_arg_t *targ = exit_storage_get(gettid());
    if (targ == NULL)
    {
        return MAIN_THREAD_T;
    }
    return TO_MY_PTHREAD_T(targ->slot);
}
