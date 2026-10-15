#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef struct thread_arg thread_arg_t;

void exit_storage_set(pid_t tid, thread_arg_t *targ);
thread_arg_t *exit_storage_get(pid_t tid);
void exit_storage_remove(pid_t tid);
