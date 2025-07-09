#define _GNU_SOURCE	// Enable the use of RTLD_NEXT

#include <sys/select.h>
#include <sys/time.h>

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include "uthread_internal.h"

/**
 * This is the uthread stack size.
 */
#define	UTHR_STACK_SIZE	(1 << 21)

/**
 * These are the uthread stats.
 */
enum uthr_state { UTHR_FREE = 0, UTHR_RUNNABLE, UTHR_BLOCKED, UTHR_JOINING,
    UTHR_ZOMBIE };

/**
 * This is the maximum number of uthreads that a program can create.
 */
#define	NUTHR	64

static struct uthr {
	ucontext_t uctx;
	char *stack_base;
	void *(*start_routine)(void *restrict);
	void *restrict argp;
	void *ret_val;
	enum uthr_state state;
	struct uthr *prev;	// previous thread in the same queue
	struct uthr *next;	// next thread in the same queue
	struct uthr *joiner;
	bool detached;
	int fd;
	enum uthr_op op;
} uthr_array[NUTHR];

/**
 * These are the thread queues that are managed by the uthread scheduler.
 */
static struct uthr runq;
//static struct uthr blockedq;
//static struct uthr reapq;
static struct uthr freeq;

/**
 * This is the currently running thread.
 */
static struct uthr *curr_uthr;

/**
 * This is the context that executes the scheduler.
 */
static ucontext_t sched_uctx;

/**
 * This is the maximum duration that a thread is allowed to run before
 * it is preempted and moved to the tail of the run queue.
 */
static const struct itimerval quantum = { { .tv_sec = 0, .tv_usec = 100000 },
    { .tv_sec = 0, .tv_usec = 100000 } };

/**
 * This is a set of signals with only SIGPROF set.  It should not change after
 * its initialization.
 */
static sigset_t SIGPROF_set;

/**
 * These are pointers to the malloc and free functions from the standard C
 * library.
 */
static void *(*mallocp)(size_t);
static void (*freep)(void *);

static void uthr_scheduler(void);

/**
 * This function checks the current signal mask to ensure that the SIGPROF
 * signal is blocked.  If the signal is not blocked, the function will trigger
 * an assertion failure.
 *
 * This function will terminate the program if it fails to retrieve the current
 * signal mask using sigprocmask.
 */
static void
uthr_assert_SIGPROF_blocked(void)
{
	sigset_t old_set;

	if (sigprocmask(SIG_BLOCK, NULL, &old_set) == -1)
		uthr_exit_errno("sigprocmask");
	assert(sigismember(&old_set, SIGPROF));
}

void
uthr_block_SIGPROF(sigset_t *old_setp)
{
	// (Your code goes here.)
	(void) old_setp;
}

void
uthr_set_sigmask(const sigset_t *setp)
{
	// (Your code goes here.)
	(void) setp;
}

void *
uthr_intern_malloc(size_t size)
{
        printf("uthr_intern_malloc \n");
	uthr_assert_SIGPROF_blocked();
	return (mallocp(size));
}

void
uthr_intern_free(void *ptr)
{
	uthr_assert_SIGPROF_blocked();
	return (freep(ptr));
}

/**
 * This function changes the state of the given thread to UTHR_ZOMBIE.  It
 * first ensures that SIGPROF is blocked and that the thread is currently
 * runnable.  The thread is then removed from the run queue and its state is
 * set to UTHR_ZOMBIE.  If the thread has a joiner, the joiner's state is set
 * to UTHR_RUNNABLE and it is inserted into the run queue.  If the thread is
 * detached and has no joiner, it is inserted into the reap queue.
 *
 * @param td Pointer to the thread to be transitioned to zombie state.
 */
// static void
// uthr_to_zombie(struct uthr *td)
// {
// 	uthr_assert_SIGPROF_blocked();
// 	assert(td->state == UTHR_RUNNABLE);
// 	if (td->joiner != NULL) {
// 		assert(!td->detached);
//                 assert(td->joiner->state == UTHR_JOINING);
// 	}

// 	// (Your code goes here.)
// }

/**
 * Frees the resources associated with a thread and transitions it to the free
 * state.
 *
 * This function should be called when a thread has finished execution and its
 * resources need to be reclaimed.  It asserts that the SIGPROF signal is
 * blocked and that the thread is in the UTHR_ZOMBIE state before proceeding.
 *
 * @param td A pointer to the thread to be freed.
 */
// static void
// uthr_to_free(struct uthr *td)
// {
// 	uthr_assert_SIGPROF_blocked();
// 	assert(td->state == UTHR_ZOMBIE);

// 	// (Your code goes here.)
// 	(void)td;
// }

/**
 * This function is responsible for starting the execution of a user-level
 * thread.  It ensures that the SIGPROF signal is blocked, retrieves the
 * thread structure, and verifies that the thread is in a runnable state.  It
 * then unblocks the SIGPROF signal, calls the thread's start routine, saves
 * that routine's return value in the thread structure, and restores the
 * original signal mask.  Finally, it transitions the thread to a zombie
 * state.
 *
 * @param tidx The index of the thread to start.
 */
static void
uthr_start(int tidx) 
{
 	sigset_t old_set;

 	uthr_assert_SIGPROF_blocked();
 	struct uthr *td = &uthr_array[tidx];
 	assert(td->state == UTHR_RUNNABLE);

 	/*
 	 * Before running the thread's start routine, SIGPROF signals must be
 	 * unblocked so that the thread can be preempted if it runs for too
 	 * long without blocking.
 	 */
 	if (sigprocmask(SIG_UNBLOCK, &SIGPROF_set, &old_set) == -1)
		uthr_exit_errno("sigprocmask");
	assert(sigismember(&old_set, SIGPROF));

	// (Your code goes here.)
 	(void) tidx;
}

int
pthread_create(pthread_t *restrict tidp, const pthread_attr_t *restrict attrp,
    void *(*start_routine)(void *restrict), void *restrict argp)
{
        
        printf("pthread_create \n");
        struct uthr *td = NULL;
        (void) attrp;
	// (Your code goes here.)
	for (volatile long i = 0; i < NUTHR; i++) {
	    if (uthr_array[i].state == UTHR_FREE) {
                td = &uthr_array[i];
		break;
	     }
        }

	if (td == NULL) {
            errno = EAGAIN;
	    return -1;
	}


	td->state = UTHR_RUNNABLE;
	td->start_routine = start_routine;
	td->argp = argp;
	td->ret_val = NULL;
	td->detached = false;
	td->joiner = NULL;

	td->stack_base = uthr_intern_malloc(UTHR_STACK_SIZE);
        if (td->stack_base == NULL) {
            errno = ENOMEM;
	    return -1;
	}

	if (getcontext(&td->uctx) == -1) {
            uthr_intern_free(td->stack_base);
	    errno = EFAULT;
	    return -1;
	}

       td->uctx.uc_stack.ss_sp = td->stack_base;
       td->uctx.uc_stack.ss_size = UTHR_STACK_SIZE;
       td->uctx.uc_link = &sched_uctx; // Link to the scheduler context

       // Set the thread's start routine
       makecontext(&td->uctx, (void (*)(void))uthr_start, 1, (int)(td - uthr_array));

       // Insert the thread into the run queue
       td->next = runq.next;
       if (runq.next != NULL) {
           runq.next->prev = td;
       }
       runq.next = td;
       td->prev = &runq;

       // Return the thread ID
       *tidp = td - uthr_array;


	return 95;
}

int
pthread_detach(pthread_t tid)
{
	// (Your code goes here.)
	(void) tid;
	return (0);
}

pthread_t
pthread_self(void)
{
	return (curr_uthr - uthr_array);
}

void
pthread_exit(void *retval)
{
	// (Your code goes here.)

	/*
	 * Since pthread_exit is declared as a function that never
	 * returns, the compiler requires us to ensure that the
	 * function's implementation never returns.
	 */
	for (;;);
	(void) retval;
}

int
pthread_join(pthread_t tid, void **retval)
{
	// (Your code goes here.)
	(void) tid;
	(void) retval;
	return (0);
}

int
sched_yield(void)
{
	// (Your code goes here.)
	return (0);
}

static struct fd_state {
	int maxfd;
	fd_set read_set;
	fd_set write_set;
	struct {
		int readers;
		int writers;
	} blocked[FD_SETSIZE];
} fd_state;

/**
 * Initializes the file descriptor state for the uthread library.
 *
 * This function sets the maximum file descriptor to -1, clears the read and
 * write file descriptor sets, and initializes the blocked readers and writers
 * count for each file descriptor to 0.
 */
static void
uthr_init_fd_state(void)
{
	fd_state.maxfd = -1;
	FD_ZERO(&fd_state.read_set);
	FD_ZERO(&fd_state.write_set);
	for (int i = 0; i < FD_SETSIZE; i++) {
		fd_state.blocked[i].readers = 0;
		fd_state.blocked[i].writers = 0;
	}
}

void
uthr_block_on_fd(int fd, enum uthr_op op)
{
	assert(fd < FD_SETSIZE);
	assert(op == UTHR_OP_READ || op == UTHR_OP_WRITE);

	// (Your code goes here.)
}

/**
 * Checks and handles blocked threads.
 *
 * This function inspects the list of blocked threads and determines if any of
 * them can be moved to the runnable state based on the readiness of file
 * descriptors.  It uses the `select` system call to check the readiness of
 * file descriptors for reading or writing.
 *
 * @param wait If true, the function will wait indefinitely for file descriptors
 *             to become ready.  If false, the function will return immediately
 *             if no file descriptors are ready.
 */
// static void
// uthr_check_blocked(bool wait)
// {
// 	struct timeval timeout = { 0, 0 }, *timeoutp = wait ? NULL : &timeout;
// 	(void) timeoutp;
// 	(void) timeout;
// 	uthr_assert_SIGPROF_blocked();

// 	// (Your code goes here.)
// }

/**
 * Scheduler function for user-level threads.
 *
 * This function is responsible for managing the execution of user-level
 * threads.  It continuously checks for runnable threads and context switches
 * to them.  If a thread is preempted, it is moved to the tail of the run
 * queue.  If no threads are runnable, it waits for a blocked thread to become
 * ready.  It also reaps and frees detached threads that have finished
 * execution.
 *
 * This function runs in an infinite loop and does not return.
 */
static void
uthr_scheduler(void)
{
 	uthr_assert_SIGPROF_blocked();
 	for (;;) {

 		// (Your code goes here.)

 	}
}

/**
 * Timer (SIGPROF) signal handler for user-level threads.
 *
 * This function is called when a timer signal is received.  It saves the
 * current errno value, performs a context switch from the current user-level
 * thread to the scheduler context, and then restores the errno value.
 *
 * @param signum The signal number.
 */
static void
uthr_timer_handler(int signum)
{
	(void)signum;
	int save_errno = errno;
	if (swapcontext(&curr_uthr->uctx, &sched_uctx) == -1)
		uthr_exit_errno("swapcontext");
	errno = save_errno;
}

/**
 * @brief Initializes the user-level threading library.
 *
 * This function is automatically called before the main function is executed,
 * due to the __attribute__((constructor)) attribute.  It performs the following
 * initialization tasks:
 * 
 * - Initializes the SIGPROF signal set.
 * - Looks up the symbols for malloc and free functions.
 * - Sets up the scheduler context.
 * - Initializes the currently running thread.
 * - Initializes the queue of free threads.
 * - Initializes the file descriptor state.
 * - Sets up the SIGPROF signal handler.
 * - Configures the interval timer for thread scheduling.
 *
 * If any of the initialization steps fail, the function will call
 * uthr_exit_errno() to terminate the program with an error message.
 */
__attribute__((constructor))
static void
uthr_init(void)
{
	static char sched_stack[UTHR_STACK_SIZE];
	printf("uthr_init function \n");


	/*
	 * SIGPROF_set must be initialized before calling uthr_lookup_symbol().
	 */
	// Initialize SIGPORF_set to include SIGPROF
	sigemptyset(&SIGPROF_set);
	sigaddset(&SIGPROF_set, SIGPROF);

	// (Your code goes here.)
	uthr_lookup_symbol((void *)&mallocp, "malloc");
	uthr_lookup_symbol((void *)&freep, "free");

	/*
	 * Initialize the scheduler context using the above sched_stack.
	 */
        if (getcontext(&sched_uctx) == -1) {
            uthr_exit_errno("getcontext");
        }
        sched_uctx.uc_stack.ss_sp = sched_stack;
        sched_uctx.uc_stack.ss_size = UTHR_STACK_SIZE;
        sched_uctx.uc_link = NULL;
        makecontext(&sched_uctx, uthr_scheduler, 0);	

	/*
	 * Initialize the currently running thread and insert it at the tail
	 * of the run queue.
	 */
	curr_uthr = &uthr_array[0];
	curr_uthr->state = UTHR_RUNNABLE;
	curr_uthr->stack_base = NULL;
	curr_uthr->prev = NULL;
        curr_uthr->next = NULL;

	freeq.next = NULL;
	struct uthr *prev = &freeq;

	// Initialize queue of free threads
	for (int i = 1; i < NUTHR; i++) {
	    uthr_array[i].state = UTHR_FREE;
	    uthr_array[i].stack_base = NULL;
	    uthr_array[i].prev = prev;
	    uthr_array[i].next = NULL;
	    prev->next = &uthr_array[i];
	    prev = &uthr_array[i];
	}

	// (Your code goes here.)
	 
	uthr_init_fd_state();

	/*
	 * Set up the SIGPROF signal handler.
	 */
	// (Your code goes here.)
	struct sigaction sa = {
		.sa_handler = uthr_timer_handler,
		.sa_flags = SA_RESTART | SA_SIGINFO
	};
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPROF, &sa, NULL) == -1) {
		uthr_exit_errno("sigaction");
	}
		
	 
	/*
	 * Configure the interval timer for thread scheduling.
	 */
	if (setitimer(ITIMER_PROF, &quantum, NULL) == -1) {
		uthr_exit_errno("setitimer");	
	}
}

void
uthr_lookup_symbol(void **addrp, const char *symbol)
{
	/*
	 * Block preemption because dlerror() may use a static buffer.
	 */
	// (Your code goes here.)
	 
	/*
	 * A concurrent lookup by another thread may have already set the
	 * address.
	 */
	if (*addrp == NULL) {
		/*
		 * See the manual page for dlopen().
		 */
		// (Your code goes here.)
	}
	
	(void) symbol;
	// (Your code goes here.)
}

void
uthr_exit_errno(const char *message)
{
	perror(message);
	exit(EXIT_FAILURE);
}
