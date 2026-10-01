#pragma once

#include "stdio.h"
#include "stdlib.h"
#include "pthread.h"

typedef unsigned long int my_pthread_t;

int my_pthread_create(my_pthread_t new_thread, 
    void *(*__start_routine) (void *),
    void *arg
);

int my_pthread_cancel(my_pthread_t thread);