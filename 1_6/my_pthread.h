#pragma once

#include "stdio.h"
#include "stdlib.h"

typedef unsigned long int mythread_t;
typedef unsigned long int mythread_attr_t;

int mythread_create(mythread_t *new_thread,
                    const mythread_attr_t *attr,
                    void *(*__start_routine)(void *),
                    void *arg);

int my_pthread_cancel(mythread_t thread_id);
int mythread_join(mythread_t thread_id, void **retval);
int mythread_detach(mythread_t thread_id);
void mythread_exit(void *retval);
mythread_t mythread_self(void);
