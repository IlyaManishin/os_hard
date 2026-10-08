#pragma once

#include <setjmp.h>
#include <sys/types.h>

void exit_storage_set(pid_t tid, jmp_buf env, void **retval_ptr);
void exit_storage_remove(pid_t tid);
void exit_storage_exit(pid_t tid, void *retval);