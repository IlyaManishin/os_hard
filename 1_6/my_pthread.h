#pragma once

#include "mythread_types.h"

#include <stdio.h>
#include <stdlib.h>

int mythread_create(mythread_t *new_thread,
                    const mythread_attr_t *attr,
                    void *(*__start_routine)(void *),
                    void *arg);

int mypthread_cancel(mythread_t thread_id);
int mythread_join(mythread_t thread_id, void **retval);
int mythread_detach(mythread_t thread_id);
void mythread_exit(void *retval);
mythread_t mythread_self(void);
