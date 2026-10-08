#define _GNU_SOURCE

#include "exit_storage.h"

#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define STORAGE_CAPACITY 1024

#define TID_FREE 0
#define TID_RESERVED (-1) /* slot claimed, env is still being filled in */

typedef struct
{
    _Atomic pid_t tid;
    jmp_buf env;
    void **retval_ptr;
} exit_entry_t;

static exit_entry_t storage[STORAGE_CAPACITY];

void exit_storage_set(pid_t tid, jmp_buf env, void **retval_ptr)
{
    for (int i = 0; i < STORAGE_CAPACITY; i++)
    {
        pid_t expected = TID_FREE;
        if (atomic_compare_exchange_strong_explicit(&storage[i].tid, &expected, TID_RESERVED,
                                                    memory_order_acq_rel, memory_order_relaxed))
        {
            memcpy(storage[i].env, env, sizeof(jmp_buf));
            storage[i].retval_ptr = retval_ptr;
            atomic_store_explicit(&storage[i].tid, tid, memory_order_release);
            return;
        }
    }
}

void exit_storage_remove(pid_t tid)
{
    for (int i = 0; i < STORAGE_CAPACITY; i++)
    {
        if (atomic_load_explicit(&storage[i].tid, memory_order_acquire) == tid)
        {
            storage[i].retval_ptr = NULL;
            atomic_store_explicit(&storage[i].tid, TID_FREE, memory_order_release);
            return;
        }
    }
}

void exit_storage_exit(pid_t tid, void *retval)
{
    for (int i = 0; i < STORAGE_CAPACITY; i++)
    {
        if (atomic_load_explicit(&storage[i].tid, memory_order_acquire) == tid)
        {
            if (storage[i].retval_ptr != NULL)
            {
                *storage[i].retval_ptr = retval;
            }
            /* The entry is removed by thread_task after the jump. */
            longjmp(storage[i].env, 1);
        }
    }

    /* Thread is not registered: nothing to jump back to. */
    syscall(SYS_exit, 0);
    for (;;)
    {
    }
}