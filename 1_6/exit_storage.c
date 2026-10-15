#define _GNU_SOURCE

#include "exit_storage.h"

#include <stdatomic.h>

#define STORAGE_CAPACITY 1024

#define TID_FREE 0
#define TID_RESERVED (-1) /* slot claimed, targ is still being filled in */

typedef struct
{
    _Atomic pid_t tid;
    thread_arg_t *targ;
} exit_entry_t;

static exit_entry_t storage[STORAGE_CAPACITY];

void exit_storage_set(pid_t tid, thread_arg_t *targ)
{
    for (int i = 0; i < STORAGE_CAPACITY; i++)
    {
        pid_t expected = TID_FREE;
        if (atomic_compare_exchange_strong(&storage[i].tid, &expected, TID_RESERVED))
        {
            storage[i].targ = targ;
            atomic_store(&storage[i].tid, tid);
            return;
        }
    }
}

thread_arg_t *exit_storage_get(pid_t tid)
{
    for (int i = 0; i < STORAGE_CAPACITY; i++)
    {
        if (atomic_load(&storage[i].tid) == tid)
        {
            return storage[i].targ;
        }
    }
    return NULL;
}

void exit_storage_remove(pid_t tid)
{
    for (int i = 0; i < STORAGE_CAPACITY; i++)
    {
        if (atomic_load(&storage[i].tid) == tid)
        {
            storage[i].targ = NULL;
            atomic_store(&storage[i].tid, TID_FREE);
            return;
        }
    }
}