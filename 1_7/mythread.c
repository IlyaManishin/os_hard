#define _GNU_SOURCE

#include "mythread.h"
#include "intercept.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define THREAD_STACK_SIZE (1024 * 1024)
#define LINUX_PAGE_SIZE 4096
#define TIME_SLICE_MS 10

#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_SEC 1000000000ULL

#define TO_MYTHREAD_T(ptr) ((mythread_t)(uintptr_t)(ptr))
#define TO_THREAD_CB(thread_id) ((thread_cb_t *)(uintptr_t)(thread_id))

#define MYTHREAD_CANCELED ((void *)-1)

typedef enum
{
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,  // ждёт завершения другого потока (join)
    THREAD_SLEEPING, // ждёт wake_time_ns
    THREAD_FINISHED
} thread_state_t;

typedef struct thread_cb thread_cb_t;

// Что поток исполняет и что вернул
typedef struct
{
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
} thread_entry_t;

// Связи для join
typedef struct
{
    thread_cb_t *joiner;      // кто ждёт нас в join
    thread_cb_t *waiting_for; // кого ждём мы в join
} thread_join_t;

// Кольцевой список всех потоков
typedef struct
{
    thread_cb_t *next;
    thread_cb_t *prev;
} thread_link_t;

struct thread_cb
{
    ucontext_t ctx;
    void *stack;
    thread_state_t state;
    bool cancel_requested;
    bool detached;
    uint64_t wake_time_ns;
    thread_entry_t entry;
    thread_join_t join;
    thread_link_t link;
};

static thread_cb_t main_cb;
static thread_cb_t *current = NULL;
static uint64_t slice_end_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

__attribute__((constructor)) static void mythread_init(void)
{
    main_cb.state = THREAD_RUNNING;
    main_cb.link.next = &main_cb;
    main_cb.link.prev = &main_cb;
    current = &main_cb;
    slice_end_ns = now_ns() + TIME_SLICE_MS * NSEC_PER_MSEC;
}

static void list_add_tail(thread_cb_t *t)
{
    t->link.next = &main_cb;
    t->link.prev = main_cb.link.prev;
    t->link.prev->link.next = t;
    main_cb.link.prev = t;
}

static void list_remove(thread_cb_t *t)
{
    t->link.prev->link.next = t->link.next;
    t->link.next->link.prev = t->link.prev;
}

// Возвращает NULL для нулевого id и для главного потока
static thread_cb_t *target(mythread_t thread_id)
{
    thread_cb_t *t = TO_THREAD_CB(thread_id);
    return t == &main_cb ? NULL : t;
}

static void reap_detached(void)
{
    thread_cb_t *t = main_cb.link.next;

    while (t != &main_cb)
    {
        thread_cb_t *next = t->link.next;

        if (t->detached && t->state == THREAD_FINISHED && t != current)
        {
            list_remove(t);
            munmap(t->stack, THREAD_STACK_SIZE);
        }
        t = next;
    }
}

// Round-robin: ищем следующий готовый поток, начиная со следующего за текущим.
// Если готовых нет, но есть спящие - реально спим до ближайшего пробуждения.
static thread_cb_t *pick_next(void)
{
    for (;;)
    {
        uint64_t now = now_ns();
        uint64_t nearest = UINT64_MAX;
        thread_cb_t *t = current;

        do
        {
            t = t->link.next;

            if (t->state == THREAD_SLEEPING && t->wake_time_ns <= now)
            {
                t->state = THREAD_READY;
            }
            if (t->state == THREAD_READY)
            {
                return t;
            }
            if (t->state == THREAD_SLEEPING && t->wake_time_ns < nearest)
            {
                nearest = t->wake_time_ns;
            }
        } while (t != current);

        if (nearest == UINT64_MAX)
        {
            fprintf(stderr, "mythread: deadlock, no runnable threads\n");
            abort();
        }

        uint64_t delta = nearest - now;
        nanosleep(&(struct timespec){.tv_sec = delta / NSEC_PER_SEC, .tv_nsec = delta % NSEC_PER_SEC}, NULL);
    }
}

// Вызывающий обязан заранее выставить current->state (READY/BLOCKED/SLEEPING/FINISHED)
static void schedule(void)
{
    thread_cb_t *prev = current;
    thread_cb_t *next = pick_next();

    next->state = THREAD_RUNNING;
    slice_end_ns = now_ns() + TIME_SLICE_MS * NSEC_PER_MSEC;

    if (next == prev)
    {
        return;
    }

    current = next;
    swapcontext(&prev->ctx, &next->ctx);

    reap_detached();
}

static void test_cancel(void)
{
    if (current->cancel_requested)
    {
        mythread_exit(MYTHREAD_CANCELED);
    }
}

static void thread_finish(void)
{
    thread_cb_t *self = current;

    self->state = THREAD_FINISHED;
    if (self->join.joiner != NULL)
    {
        self->join.joiner->join.waiting_for = NULL;
        self->join.joiner->state = THREAD_READY;
        self->join.joiner = NULL;
    }

    schedule();
    abort();
}

static void thread_task(void)
{
    thread_cb_t *self = current;

    reap_detached();
    test_cancel();
    self->entry.retval = self->entry.start_routine(self->entry.arg);
    thread_finish();
}

int mythread_create(mythread_t *new_thread,
                    const mythread_attr_t *attr,
                    void *(*start_routine)(void *),
                    void *arg)
{
    (void)attr;

    if (new_thread == NULL || start_routine == NULL)
    {
        return EINVAL;
    }

    void *stack = mmap(NULL, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED)
    {
        return ENOMEM;
    }

    if (mprotect(stack, LINUX_PAGE_SIZE, PROT_NONE) != 0)
    {
        int err = errno;
        munmap(stack, THREAD_STACK_SIZE);
        return err;
    }

    uintptr_t stack_top = (uintptr_t)stack + THREAD_STACK_SIZE;
    stack_top -= sizeof(thread_cb_t);

    stack_top &= ~((uintptr_t)15);

    thread_cb_t *t = (thread_cb_t *)stack_top;
    memset(t, 0, sizeof(*t));
    t->stack = stack;
    t->entry.start_routine = start_routine;
    t->entry.arg = arg;
    t->state = THREAD_READY;

    if (getcontext(&t->ctx) != 0)
    {
        int err = errno;
        munmap(stack, THREAD_STACK_SIZE);
        return err;
    }
    t->ctx.uc_stack.ss_sp = (char *)stack + LINUX_PAGE_SIZE;
    t->ctx.uc_stack.ss_size = stack_top - (uintptr_t)stack - LINUX_PAGE_SIZE;
    t->ctx.uc_link = NULL;
    makecontext(&t->ctx, thread_task, 0);

    list_add_tail(t);

    *new_thread = TO_MYTHREAD_T(t);
    return 0;
}

void mythread_exit(void *retval)
{
    if (current == &main_cb)
    {
        exit(EXIT_SUCCESS);
    }

    current->entry.retval = retval;
    thread_finish();
    abort();
}

void mythread_yield(void)
{
    current->state = THREAD_READY;
    schedule();
    test_cancel();
}

void mythread_sleep_ms(unsigned int ms)
{
    current->wake_time_ns = now_ns() + ms * NSEC_PER_MSEC;
    current->state = THREAD_SLEEPING;
    schedule();
    test_cancel();
}

mythread_t mythread_self(void)
{
    return TO_MYTHREAD_T(current);
}

int mythread_cancel(mythread_t thread_id)
{
    thread_cb_t *t = target(thread_id);
    if (t == NULL)
    {
        return EINVAL;
    }
    if (t->state == THREAD_FINISHED)
    {
        return 0;
    }

    t->cancel_requested = true;

    if (t == current)
    {
        test_cancel();
    }

    if (t->state == THREAD_BLOCKED)
    {
        t->join.waiting_for->join.joiner = NULL;
        t->join.waiting_for = NULL;
    }
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING)
    {
        t->state = THREAD_READY;
    }

    return 0;
}

int mythread_detach(mythread_t thread_id)
{
    thread_cb_t *t = target(thread_id);
    if (t == NULL)
    {
        return EINVAL;
    }
    if (t->detached || t->join.joiner != NULL)
    {
        return EINVAL;
    }

    t->detached = true;
    if (t->state == THREAD_FINISHED)
    {
        reap_detached();
    }
    return 0;
}

int mythread_join(mythread_t thread_id, void **retval)
{
    thread_cb_t *t = target(thread_id);
    if (t == NULL)
    {
        return EINVAL;
    }
    if (t->detached || t->join.joiner != NULL)
    {
        return EINVAL;
    }
    // Проверка на самоджойн и на цикл ожидания (t == current на первой итерации)
    for (thread_cb_t *w = t; w != NULL; w = w->join.waiting_for)
    {
        if (w == current)
        {
            return EDEADLK;
        }
    }

    while (t->state != THREAD_FINISHED)
    {
        t->join.joiner = current;
        current->join.waiting_for = t;
        current->state = THREAD_BLOCKED;
        schedule();
        test_cancel();
    }

    if (retval != NULL)
    {
        *retval = t->entry.retval;
    }

    list_remove(t);
    munmap(t->stack, THREAD_STACK_SIZE);
    return 0;
}

void preempt_point(void)
{
    // current == NULL, пока не отработал конструктор (syscall из чужих конструкторов)
    if (current != NULL && now_ns() >= slice_end_ns)
    {
        mythread_yield();
    }
}