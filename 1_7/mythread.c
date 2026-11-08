#define _GNU_SOURCE

#include "mythread.h"
#include "intercept.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
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

typedef enum
{
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,  // ждёт завершения другого потока (join)
    THREAD_SLEEPING, // ждёт wake_time_ns
    THREAD_FINISHED
} thread_state_t;

typedef struct thread_cb
{
    ucontext_t ctx;
    void *stack;
    void *(*start_routine)(void *);
    void *arg;
    void *retval;
    thread_state_t state;
    bool cancel_requested;
    bool detached;
    uint64_t wake_time_ns;
    struct thread_cb *joiner;      // кто ждёт нас в join
    struct thread_cb *waiting_for; // кого ждём мы в join
    struct thread_cb *next;        // кольцевой список всех потоков
    struct thread_cb *prev;
} thread_cb_t;

static thread_cb_t main_cb;
static thread_cb_t *current = NULL; // NULL до первой инициализации
static uint64_t slice_end_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static void mythread_init(void)
{
    if (current != NULL)
    {
        return;
    }

    main_cb.state = THREAD_RUNNING;
    main_cb.next = &main_cb;
    main_cb.prev = &main_cb;
    current = &main_cb;
    slice_end_ns = now_ns() + TIME_SLICE_MS * NSEC_PER_MSEC;
}

static void list_add_tail(thread_cb_t *t)
{
    t->next = &main_cb;
    t->prev = main_cb.prev;
    t->prev->next = t;
    main_cb.prev = t;
}

static void list_remove(thread_cb_t *t)
{
    t->prev->next = t->next;
    t->next->prev = t->prev;
}

// Round-robin: ищем следующий готовый поток, начиная со следующего за текущим.
// Если готовых нет, но есть спящие - реально спим до ближайшего пробуждения.
static void reap_detached(void);

static thread_cb_t *pick_next(void)
{
    for (;;)
    {
        uint64_t now = now_ns();
        uint64_t nearest = UINT64_MAX;
        thread_cb_t *t = current;

        do
        {
            t = t->next;

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

// Завершившийся detached-поток нельзя освободить из него самого (мы стоим на его стеке),
// поэтому его стек освобождает любой другой поток после переключения контекста.
static void reap_detached(void)
{
    thread_cb_t *t = main_cb.next;

    while (t != &main_cb)
    {
        thread_cb_t *next = t->next;

        if (t->detached && t->state == THREAD_FINISHED && t != current)
        {
            list_remove(t);
            munmap(t->stack, THREAD_STACK_SIZE);
        }
        t = next;
    }
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
    if (self->joiner != NULL)
    {
        self->joiner->waiting_for = NULL;
        self->joiner->state = THREAD_READY;
        self->joiner = NULL;
    }

    schedule();
    abort(); // сюда управление вернуться не должно
}

static void thread_task(void)
{
    thread_cb_t *self = current;

    reap_detached();
    test_cancel(); // отмена могла прийти до первого запуска
    self->retval = self->start_routine(self->arg);
    thread_finish();
}

int mythread_create(mythread_t *new_thread,
                    const mythread_attr_t *attr,
                    void *(*__start_routine)(void *),
                    void *arg)
{
    (void)attr; // всегда NULL
    void *(*start_routine)(void *) = __start_routine;

    if (new_thread == NULL || start_routine == NULL)
    {
        return EINVAL;
    }

    mythread_init();

    void *stack = mmap(NULL, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED)
    {
        return ENOMEM;
    }

    // guard page внизу стека
    if (mprotect(stack, LINUX_PAGE_SIZE, PROT_NONE) != 0)
    {
        int err = errno;
        munmap(stack, THREAD_STACK_SIZE);
        return err;
    }

    uintptr_t stack_top = (uintptr_t)stack + THREAD_STACK_SIZE;
    stack_top -= sizeof(thread_cb_t);

    // 16 byte aligned
    stack_top &= ~((uintptr_t)15);

    thread_cb_t *t = (thread_cb_t *)stack_top;
    memset(t, 0, sizeof(*t));
    t->stack = stack;
    t->start_routine = start_routine;
    t->arg = arg;
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
    mythread_init();

    if (current == &main_cb)
    {
        exit(EXIT_SUCCESS);
    }

    current->retval = retval;
    thread_finish();
    abort();
}

void mythread_yield(void)
{
    mythread_init();

    current->state = THREAD_READY;
    schedule();
    test_cancel();
}

void mythread_sleep_ms(unsigned int ms)
{
    mythread_init();

    current->wake_time_ns = now_ns() + ms * NSEC_PER_MSEC;
    current->state = THREAD_SLEEPING;
    schedule();
    test_cancel();
}

mythread_t mythread_self(void)
{
    mythread_init();
    return TO_MYTHREAD_T(current);
}

int mythread_cancel(mythread_t thread_id)
{
    mythread_init();

    thread_cb_t *t = TO_THREAD_CB(thread_id);
    if (t == NULL || t == &main_cb)
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

    // отложенная отмена: будим спящий/ждущий поток, чтобы он дошёл до точки отмены
    if (t->state == THREAD_BLOCKED)
    {
        t->waiting_for->joiner = NULL;
        t->waiting_for = NULL;
        t->state = THREAD_READY;
    }
    else if (t->state == THREAD_SLEEPING)
    {
        t->state = THREAD_READY;
    }

    return 0;
}

int mythread_detach(mythread_t thread_id)
{
    mythread_init();

    thread_cb_t *t = TO_THREAD_CB(thread_id);
    if (t == NULL || t == &main_cb)
    {
        return EINVAL;
    }
    if (t->detached || t->joiner != NULL)
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
    mythread_init();

    thread_cb_t *t = TO_THREAD_CB(thread_id);
    if (t == NULL || t == &main_cb)
    {
        return EINVAL;
    }
    if (t == current)
    {
        return EDEADLK;
    }
    if (t->detached || t->joiner != NULL)
    {
        return EINVAL;
    }
    for (thread_cb_t *w = t; w != NULL; w = w->waiting_for)
    {
        if (w == current)
        {
            return EDEADLK;
        }
    }

    while (t->state != THREAD_FINISHED)
    {
        t->joiner = current;
        current->waiting_for = t;
        current->state = THREAD_BLOCKED;
        schedule();
        test_cancel(); // join - точка отмены
    }

    if (retval != NULL)
    {
        *retval = t->retval;
    }

    list_remove(t);
    munmap(t->stack, THREAD_STACK_SIZE); // cb лежит на вершине этого же стека
    return 0;
}

// Точка переключения контекста: вызывается после системного вызова.
void preempt_point(void)
{
    if (current != NULL && now_ns() >= slice_end_ns)
    {
        mythread_yield();
    }
}
