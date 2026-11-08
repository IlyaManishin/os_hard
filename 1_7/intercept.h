#pragma once

#include <stddef.h>
#include <unistd.h>

void preempt_point(void);

long syscall(long number, ...);
int usleep(useconds_t usec);
unsigned int sleep(unsigned int seconds);
