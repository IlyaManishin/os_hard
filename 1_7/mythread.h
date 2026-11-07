#pragma once

#include "mythread_types.h"

#define MYTHREAD_CANCELED ((void *)-1)

int mythread_create(mythread_t *new_thread,
                    const mythread_attr_t *attr,
                    void *(*__start_routine)(void *),
                    void *arg);

int mythread_cancel(mythread_t thread_id);
int mythread_join(mythread_t thread_id, void **retval);
int mythread_detach(mythread_t thread_id);
void mythread_exit(void *retval) __attribute__((noreturn));
mythread_t mythread_self(void);

void mythread_yield(void);
void mythread_sleep_ms(unsigned int ms);
