#pragma once

#include "stdio.h"
#include "stdlib.h"

typedef unsigned long int my_pthread_t;

int mythread_create(my_pthread_t* new_thread,
                      void *(*__start_routine)(void *),
                      void *arg);

int my_pthread_cancel(my_pthread_t thread_id);
int mythread_join(my_pthread_t thread_id, void **retval);
int mythread_detach(my_pthread_t thread_id);
void mythread_exit(void *retval);