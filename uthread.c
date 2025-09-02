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
	int uthr_id;
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
void add_thread(struct uthr *td, struct uthr *queue);

void setup_SIGPROF_timer(void);

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
        if (!sigismember(&SIGPROF_set, SIGPROF)) {
            uthr_exit_errno("SIGPROF is not blocked. \n");
        }   
}

void
uthr_block_SIGPROF(sigset_t *old_setp)
{
	if (sigprocmask(SIG_BLOCK, &SIGPROF_set, old_setp) == -1) {
            uthr_exit_errno("sigprocmask");
        }
}


void 
uthr_unblock_SIGPROF(sigset_t *old_setp)
{
       if (sigprocmask(SIG_UNBLOCK, &SIGPROF_set, old_setp) == -1) {
           uthr_exit_errno("sigprocmask");
       }
}

void
uthr_set_sigmask(const sigset_t *setp)
{
	(void) setp;
}

void *
uthr_intern_malloc(size_t size)
{
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
static void
uthr_to_zombie(struct uthr *td)
{
	uthr_assert_SIGPROF_blocked();
	assert(td->state == UTHR_RUNNABLE);
	struct uthr *selected_thread = runq.next;
	struct uthr *prev = NULL;

	// Remove the thread from the run queue
	while (selected_thread != NULL) {
	    if (selected_thread == td) {
	        if (prev == NULL) {
	            runq.next = runq.next->next;
		    break;
	        }

	        else if (selected_thread->next == NULL) {
		    prev->next = NULL;
		    break;
	        }

		else if (prev != NULL && selected_thread->next != NULL) {
		    prev->next = selected_thread->next;
		    selected_thread->prev = prev;
		    break;
		}
	    }

            prev = selected_thread;
	    selected_thread = selected_thread->next;
	}

	td->state = UTHR_ZOMBIE;
}

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
static void
uthr_to_free(struct uthr *td)
{
 	uthr_assert_SIGPROF_blocked();
 	assert(td->state == UTHR_ZOMBIE);

	uthr_intern_free(td->stack_base);
 	td->stack_base = NULL;
	td->state = UTHR_FREE;

	add_thread(td, &freeq);	
}

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
 	
	struct uthr *selected_thread = &uthr_array[tidx];
	assert(selected_thread->state == UTHR_RUNNABLE);

 	/*
 	 * Before running the thread's start routine, SIGPROF signals must be
 	 * unblocked so that the thread can be preempted if it runs for too
 	 * long without blocking.
 	 */
 	if (sigprocmask(SIG_UNBLOCK, &SIGPROF_set, &old_set) == -1)
		uthr_exit_errno("sigprocmask");
	assert(sigismember(&old_set, SIGPROF));
		
	// Execute the specified thread
	selected_thread->ret_val  = selected_thread->start_routine(selected_thread->argp);
	
	// Transition the thread into the Zombie state
	uthr_to_zombie(selected_thread);

	// Switch back into the scheduler
	if (swapcontext(&selected_thread->uctx, &sched_uctx) != 0) {
		uthr_exit_errno("Error swtiching back into the scheduler\n");
	}
}

int
pthread_create(pthread_t *restrict tidp, const pthread_attr_t *restrict attrp,
    void *(*start_routine)(void *restrict), void *restrict argp)
{
        if (attrp != NULL) {
            errno = ENOTSUP;
            return ENOTSUP;
        }

        struct uthr *td = NULL;

	for (volatile long i = 0; i < NUTHR; i++) {
	    if (uthr_array[i].state == UTHR_FREE) {
                td = &uthr_array[i];
		break;
	     }
        }

	if (td == NULL) {
            errno = EAGAIN;
	    return EAGAIN;
	}


	td->state = UTHR_RUNNABLE;
	td->start_routine = start_routine;
	td->argp = argp;
	td->ret_val = NULL;
	td->detached = false;
	td->joiner = NULL;
        td->uthr_id = (int)(td - uthr_array);
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
       td->uctx.uc_link = NULL; // Link to the scheduler context

       // Set the thread's start routine
       makecontext(&td->uctx, (void (*)(void))uthr_start, 1, (int)(td - uthr_array));

       // Insert the thread at the head of the queue if queue is empty
       if (runq.next == NULL) {
           td->next = NULL;
           td->prev = NULL;
           runq.next = td;   
       }
      
      // Traverse to find the last thread in the queue
       else {
           struct uthr *end_queue = runq.next;
      
           while (end_queue->next != NULL) {
               end_queue = end_queue->next;
           }

           end_queue->next = td;
           td->prev = end_queue;
           td->next = NULL;
       }

       // Return the thread ID
       *tidp = td - uthr_array;

	return 0;
}

int
pthread_detach(pthread_t tid)
{
        if ((int)tid < 0 || tid >= NUTHR || uthr_array[tid].state == UTHR_FREE) {
            errno = ESRCH;
            return ESRCH;
        }

        struct uthr *td = &uthr_array[tid];
        
        if (td->detached) {
            errno = EINVAL;
            return EINVAL;
        }


        td->detached = true;
        
        // if the thread is in ZOMBIE state, it can be reaped immediately
        if ((td->state) == UTHR_ZOMBIE) {
            uthr_intern_free(td->stack_base);
            td->stack_base = NULL;
            td->state = UTHR_FREE;
            add_thread(td, &freeq);
        }  
        
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
	// Set the return value in the current threadread's return value
	curr_uthr->ret_val = retval;
	
	// Set state to be Zombie
	curr_uthr->state = UTHR_ZOMBIE;        

	// Add the Joining test case
	// Add the Zombie test case	
	
	if (swapcontext(&curr_uthr->uctx, &sched_uctx) != 0) {
		uthr_exit_errno("Error switching back to the scheduler's context");
	}

	/*
	 * Since pthread_exit is declared as a function that never
	 * returns, the compiler requires us to ensure that the
	 * function's implementation never returns.
	 */
	for (;;);
}


void 
add_thread(struct uthr *td, struct uthr *queue) 
{
	struct uthr *itr = queue;
	while (itr != NULL) {
		itr = itr->next;
	}

	// If original queue is empty
	if (queue->next == NULL) {
            td->next = NULL;
            td->prev = NULL;
	    queue->next = td;
	}

        // Traverse to the end of the queue
	else {
            struct uthr *end_queue = queue;
	    while (end_queue->next != NULL) {
	        end_queue = end_queue->next;
            }

            end_queue->next = td;
            td->prev = end_queue;
            td->next = NULL;
       }

       itr = queue;
       while (itr != NULL) {
           itr = itr->next;
       }
}

int
pthread_join(pthread_t tid, void **retval)
{
        if ((int) tid < 0 || tid >= NUTHR) {
	    errno = ESRCH;
            return ESRCH;
        }

        if (tid == 0) {
	    return EDEADLK;
	}

	struct uthr *td = &uthr_array[tid];

        if (td->joiner != NULL) {
	    if (td->joiner->state == UTHR_JOINING) {
	        return EINVAL;
	    }
	    return ESRCH;
	}
	
        if (td->state == UTHR_FREE) {
            return EINVAL;
	}

        if (td->detached) {
	     td->state = UTHR_ZOMBIE;
	     uthr_to_free(td);
            errno = EINVAL;
            return EINVAL;
        }
       
	if (td == curr_uthr) {
	    printf("CAN'T JOIN ITSELF\n");
	    return EDEADLK;
	}
	
	// Set the calling thread to be joining
        curr_uthr->state = UTHR_JOINING;
        td->joiner = curr_uthr;

	// Switch to the scheduler
	makecontext(&sched_uctx, uthr_scheduler, 0);	
	if (swapcontext(&td->joiner->uctx, &sched_uctx) != 0) {
		uthr_exit_errno("Error switching to the scheduler context \n");
	}         

	uthr_to_free(td);
	uthr_intern_free(td->stack_base);

	if (td->ret_val != NULL && retval != NULL){
	    *retval = uthr_intern_malloc(sizeof(int));
	    *retval = td->ret_val;
	}

	curr_uthr = &uthr_array[0];
	return (0);
}

int
sched_yield(void)
{
	assert(curr_uthr->state == UTHR_RUNNABLE);
	
	// Grab the first thread in the queue
	struct uthr* current_run_thread = runq.next;
	
	// Logic if there is another thread in the queue
	if (current_run_thread->next != NULL) {
		// Move the current thread to the end of the queue
		struct uthr* end = runq.next;
		while (end->next != NULL) {
			end = end->next;
		} 
		
		end->next = current_run_thread;
		current_run_thread->next = NULL;
		current_run_thread->prev = end;

		// Update the run pointer to next one
		runq.next = runq.next->next;
	}
	
	// Switch to the scheduler context
	if (swapcontext(&curr_uthr->uctx, &sched_uctx) == - 1) {
		uthr_exit_errno("Error switching the scheduler context\n");
	}
	
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

		if (runq.next != NULL) {
			// Selects the current thread in RUNNABLE queue
			struct uthr *selected_thread = runq.next;

			runq.next = selected_thread->next;
			selected_thread->next = NULL;
			selected_thread->prev = NULL;	
			
			curr_uthr = selected_thread;
					
			if (swapcontext(&sched_uctx, &selected_thread->uctx) != 0){
				uthr_exit_errno("Error switching context to the thread\n");
			}
                        
			if (selected_thread->state == UTHR_ZOMBIE){

                                // Appends the joining thread to the front of the queue
				if (selected_thread->joiner != NULL) {
					struct uthr* joined_thread = selected_thread->joiner;
					joined_thread->state= UTHR_RUNNABLE;
					joined_thread->prev = NULL;
					joined_thread->next = runq.next;
					if (runq.next != NULL) {
						runq.next->prev = joined_thread;
					}	

					runq.next = joined_thread;
					runq.next->prev = NULL;
				}
				
				curr_uthr = &uthr_array[0];

				if (selected_thread->detached) {
					uthr_to_free(selected_thread);					
				}
			}
		}

		else{

			if (swapcontext(&sched_uctx, &curr_uthr->uctx) != 0){
                                uthr_exit_errno("Error switching context to the current thread\n");
                        }

			break;
		}
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
	//int save_errno = errno;
	//if (swapcontext(&curr_uthr->uctx, &sched_uctx) == -1)
	//	uthr_exit_errno("swapcontext");
	//errno = save_errno;
}

void setup_SIGPROF_timer(void)
{
	struct sigaction sa = {
		.sa_handler = uthr_timer_handler,
		.sa_flags = SA_RESTART
	};

	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGPROF, &sa, NULL) == -1) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	if (setitimer(ITIMER_PROF, &quantum, NULL) == -1) {
		perror("setitimer");
		exit(EXIT_FAILURE);
	}
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

	/*
	 * SIGPROF_set must be initialized before calling uthr_lookup_symbol().
	 */
	// Initialize SIGPORF_set to include SIGPROF
	sigemptyset(&SIGPROF_set);
	sigaddset(&SIGPROF_set, SIGPROF);
        uthr_block_SIGPROF(&SIGPROF_set);           
 
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
        curr_uthr->uthr_id = 0;
        curr_uthr->detached = false;

        if (getcontext(&curr_uthr->uctx) == -1) {
	    uthr_exit_errno("getcontext");
	}

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
	setup_SIGPROF_timer();
}



void
uthr_lookup_symbol(void **addrp, const char *symbol)
{
	/*
	 * Block preemption because dlerror() may use a static buffer.
	 */
        uthr_block_SIGPROF(&SIGPROF_set);
        uthr_assert_SIGPROF_blocked();

	/*
	 * A concurrent lookup by another thread may have already set the
	 * address.
	 */
	if (*addrp == NULL) {
	    /*
            * See the manual page for dlopen().
            */
	   *addrp = dlsym(RTLD_NEXT, symbol);
           if (*addrp == NULL) {
               char buf[256];
	       snprintf(buf, sizeof(buf), "failed to find symbol %s: %s\n", symbol, dlerror());
               uthr_exit_errno(buf);
	   }
        
	} else {
	    uthr_unblock_SIGPROF(&SIGPROF_set);
	}
        
        // uthr_unblock_SIGPROF(&SIGPROF_set);
}

void
uthr_exit_errno(const char *message)
{
	perror(message);
	exit(EXIT_FAILURE);
}
