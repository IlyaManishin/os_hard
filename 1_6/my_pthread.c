#include "my_pthread.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int my_pthread_create(my_pthread_t new_thread,
                      void *(*__start_routine)(void *),
                      void *arg)
{
}