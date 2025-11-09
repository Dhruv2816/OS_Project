#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed-point.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif
#include <stdlib.h>

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
/* --- NEW MLFQS CODE --- */
static struct list ready_list[PRI_MAX + 1];

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

static int load_avg;
/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);


/* Recalculates the priority of thread T based on MLFQ formula. */
static void
mlfqs_recalculate_priority (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread)
    return;

  /* Use your fixed-point macros for the formula:
     priority = PRI_MAX - (recent_cpu / 4) - (nice * 2) */

  /* (recent_cpu / 4) */
  int recent_cpu_div_4 = FP_TO_INT_ZERO (FP_DIV_INT (t->recent_cpu, 4));

  /* (nice * 2) */
  int nice_mul_2 = t->nice * 2;

  /* Perform the subtraction */
  int priority = PRI_MAX - recent_cpu_div_4 - nice_mul_2;

  /* Ensure priority is within bounds [PRI_MIN, PRI_MAX] */
  if (priority < PRI_MIN)
    priority = PRI_MIN;
  if (priority > PRI_MAX)
    priority = PRI_MAX;

  t->priority = priority;
}

/* Yields if the current thread is not the highest priority.*/
void
thread_yield_if_not_highest_priority(void)
{
  /* We can't yield from an interrupt context */
  if (intr_context ())
    return;

  enum intr_level old_level = intr_disable ();

  if (thread_mlfqs)
    {
      /* --- MLFQ LOGIC --- */
      /* Check all ready lists with a higher priority
         than the current thread. */
      int i;
      for (i = PRI_MAX; i > thread_current ()->priority; i--)
        {
          if (!list_empty (&ready_list[i]))
            {
              /* Found a higher-priority thread, so yield. */
              thread_yield ();
              break; 
            }
        }
    }
  else
    {
      /* --- ORIGINAL PRIORITY SCHEDULER LOGIC --- */
      /* Check only the single ready list (ready_list[0]) */
      if (!list_empty (&ready_list[0]))
        {
          struct thread *top = list_entry (list_front (&ready_list[0]),
                                           struct thread, elem);
          if (top->priority > thread_current ()->priority)
            {
              thread_yield ();
            }
        }
    }
  
  /* Restore interrupts once at the end */
  intr_set_level (old_level);
}
/* Increments recent_cpu for the running thread by 1. */
static void
mlfqs_increment_recent_cpu (void)
{
  if (thread_current () == idle_thread)
    return;
    
  struct thread *cur = thread_current ();
  /* cur->recent_cpu = cur->recent_cpu + 1 */
  cur->recent_cpu = FP_ADD_INT (cur->recent_cpu, 1);
}

/* Recalculates load_avg using the formula:
   load_avg = (59/60) * load_avg + (1/60) * ready_threads */
static void
mlfqs_recalculate_load_avg (void *aux UNUSED)
{
  int ready_threads = 0;
  int i;

  /* --- START FIX --- */
  // if (thread_mlfqs)
  //   {
      for (i = 0; i <= PRI_MAX; i++)
        {
          ready_threads += list_size (&ready_list[i]);
        }
  //   }
  // else
  //   {
  //     ready_threads = list_size (&ready_list[0]);
  //   }
  /* --- END FIX --- */
  
  if (thread_current () != idle_thread)
    ready_threads++;

  /* load_avg = (59/60) * load_avg + (1/60) * ready_threads */
  int term1 = FP_MUL_FP(FP_DIV_INT(INT_TO_FP(59), 60), load_avg);
  int term2 = FP_MUL_INT(FP_DIV_INT(INT_TO_FP(1), 60), ready_threads);
  
  load_avg = FP_ADD_FP(term1, term2);
}

/* Recalculates recent_cpu for a single thread T using the formula:
   recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice */
static void
mlfqs_recalculate_recent_cpu (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread)
    return;

  /* (2 * load_avg) */
  int load_avg_x_2 = FP_MUL_INT (load_avg, 2);

  /* (2 * load_avg + 1) */
  int load_avg_x_2_plus_1 = FP_ADD_INT (load_avg_x_2, 1);
  
  /* (2*load_avg) / (2*load_avg + 1) */
  int coefficient = FP_DIV_FP (load_avg_x_2, load_avg_x_2_plus_1);

  /* ... * recent_cpu */
  int term1 = FP_MUL_FP (coefficient, t->recent_cpu);

  /* ... + nice */
  t->recent_cpu = FP_ADD_INT (term1, t->nice);
}
/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */

void
debug_check_list_threads(const struct list *list, const char *tag)
{
  struct list_elem *e;
  for (e = list_begin(list); e != list_end(list); e = list_next(e))
    {
      struct thread *t = NULL;
      /* We assume lists of threads use member 'elem'. */
      t = list_entry(e, struct thread, elem);
      if (!is_thread(t))
        {
          // printf("[BUG DETECT] %s: bad list_elem %p not a thread (name ptr maybe %p)\n",
                //  tag, e, (void *) t);
          /* Print some raw memory to help diagnose */
          uint32_t *p = (uint32_t *) pg_round_down((uint32_t) e);
          // printf("[BUG DETECT] page start %p first 8 words: %08x %08x %08x %08x %08x %08x %08x %08x\n",
          //        (void*)p, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
          return;
        }
    }
  /* nothing suspicious */
  // printf("[LIST OK] %s all entries are thread objects\n", tag);
}
/* Called from init.c:main() *after* options are parsed. */
void
thread_mlfqs_init (void)
{
  ASSERT (thread_mlfqs); /* We should only be here if MLFQ is on */
  
  /* Initialize MLFQ variables for the 'main' thread */
  struct thread *t = thread_current(); 
  t->nice = 0;
  t->recent_cpu = INT_TO_FP(0);
  load_avg = INT_TO_FP(0);
  
  /* Calculate main's priority now that its MLFQ vars are set */
  mlfqs_recalculate_priority(t, NULL);
}
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);

  /* --- FIX: ALWAYS initialize all 64 lists --- */
  int i;
  for (i = 0; i <= PRI_MAX; i++)
    {
      list_init (&ready_list[i]);
    }
  /* --- END FIX --- */
  
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
  
  /* DO NOT initialize nice, recent_cpu, or load_avg here. */

  list_push_back (&all_list, &initial_thread->allelem);
}
/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  debug_check_list_threads(&all_list, "all_list (startup)");

  {
    struct list_elem *e;
    // printf("[ALL THREADS] listing all threads at startup:");
    for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
      {
        struct thread *tt = list_entry(e, struct thread, allelem);
        // printf(" %s(tid=%d,pri=%d)", tt->name, tt->tid, tt->priority);
      }
    // printf("\n");
  }
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
  #ifdef USERPROG
    else if (t->pagedir != NULL)
      user_ticks++;
  #endif
    else
      kernel_ticks++;
    /* --- MLFQS Calculations --- */
      if (thread_mlfqs)
        {
          /* 1. Increment recent_cpu for running thread (every tick). */
          mlfqs_increment_recent_cpu ();
          
          /* 2. Recalculate load_avg and all recent_cpu (every second). */
          if (timer_ticks () % TIMER_FREQ == 0)
            {
              mlfqs_recalculate_load_avg (NULL);
              /* We need a helper to update *all* threads */
              thread_foreach (mlfqs_recalculate_recent_cpu, NULL); 
            }

          /* 3. Recalculate all priorities (every 4 ticks). */
          if (timer_ticks () % TIME_SLICE == 0)
            {
              /* We need a helper to update *all* threads */
              thread_foreach (mlfqs_recalculate_priority, NULL);
            }
        }
      /* --- END MLFQS --- */
  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  // printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
  //         idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{ 
  // printf("[CREATE TRACE] creating thread name=%s pri=%d\n", name, priority);

  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread page. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    {
      // printf("[CREATE FAIL] Could not allocate page for thread %s\n", name);
      return TID_ERROR;
    }

  /* Initialize thread struct. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  if (thread_mlfqs)
  {
    t->nice = thread_current()->nice;
    t->recent_cpu = thread_current()->recent_cpu;

    /* --- FIX: Calculate priority *before* unblocking --- */
    mlfqs_recalculate_priority(t, NULL);
    /* --- END FIX --- */
  }
  
  /* --- FIX: Protect global list update --- */
  enum intr_level old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
  /* --- END FIX --- */

  /* Set up kernel stack frames for context switch. */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add thread to ready queue.
     Now thread_unblock() will use the correct, newly calculated priority. */
  thread_unblock (t);

  /* --- DEBUG PRINT --- */
  // printf("[CREATE] new thread name=%s tid=%d base_pri=%d pri=%d\n",
  //        t->name, t->tid, t->base_priority, t->priority);

  /* If the new thread is higher priority, yield CPU. */
  if (!thread_mlfqs && t->priority > thread_current()->priority)
    {
      // printf("[CREATE] yielding current thread (%s) to higher-priority %s\n",
      //        thread_current()->name, t->name);
      thread_yield();
    }

  return tid;
}


/* Comparison function for sorting threads by priority in descending order.
   Returns true if thread 'a' has a higher priority than thread 'b'. */
bool
thread_priority_cmp_greater (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);
  const struct thread *tb = list_entry (b, struct thread, elem);
  
  /* This MUST be a "greater than" symbol > */
  return ta->priority > tb->priority;
}
static bool
donation_cmp_greater (const struct list_elem *a,
                      const struct list_elem *b,
                      void *aux UNUSED)
{
  struct donation *da = list_entry(a, struct donation, elem);
  struct donation *db = list_entry(b, struct donation, elem);
  return da->priority > db->priority;
}

bool
thread_donation_cmp_greater (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED)
{
  struct thread *ta = list_entry(a, struct thread, donation_elem);
  struct thread *tb = list_entry(b, struct thread, donation_elem);
  return ta->priority > tb->priority;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}
/* Checks if the current thread has the highest priority.
   If not, yields the CPU. Assumes interrupts are disabled. */
void
thread_check_preemption (void)
{
  /* If the ready list is empty, there's nothing to preempt with. */
  if (list_empty (&ready_list[0]))
    return;

  /* Get the highest-priority thread from the ready list. */
  struct thread *next_thread = list_entry (list_front (&ready_list[0]),
                                           struct thread, elem);

  /* Compare priorities and yield if necessary. */
  if (thread_current ()->priority < next_thread->priority)
    {
      thread_yield ();
    }
}
/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
/* threads/thread.c */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level = intr_disable ();

  ASSERT (t->status == THREAD_BLOCKED);

  if (thread_mlfqs)
    {
      /* Add to the list corresponding to its new priority */
      list_push_back (&ready_list[t->priority], &t->elem);
    }
  else
    {
      /* Original priority scheduler logic */
      list_insert_ordered (&ready_list[0], &t->elem, 
                           thread_priority_cmp_greater, NULL);
    }
  
  t->status = THREAD_READY;

  /* --- THIS IS THE FIX --- */
  /* --- REMOVE THIS BLOCK --- */
  // if (!intr_context ())
  //  {
  //    thread_yield_if_not_highest_priority();
  //  }
  /* --- END REMOVE --- */
  
  intr_set_level (old_level);
}
/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  // printf("[BUG TRACE] thread_current called: t=%p, status=%d, magic=0x%x\n",
  //   t, t->status, t->magic);

  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
/* threads/thread.c */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level = intr_disable ();

  if (cur != idle_thread) 
    {
      if (thread_mlfqs)
        {
          /* Add to the list corresponding to its priority */
          list_push_back (&ready_list[cur->priority], &cur->elem);
        }
      else
        {
          /* Original priority scheduler logic */
          list_insert_ordered (&ready_list[0], &cur->elem, 
                               thread_priority_cmp_greater, NULL);
        }
    }
  
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}
/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}
/* We limit nesting to 8 levels as suggested. */
#define MAX_DONATION_DEPTH 8
void
thread_donate_priority (struct thread *t)
{
  ASSERT (t != NULL);

  struct thread *cur = thread_current ();
  int depth = 0;

  // printf ("[DONATE START] donor=%s (pri %d) -> target=%s (pri %d)\n",
  //         cur->name, cur->priority,
  //         t->name, t->priority);

  /* Propagate donation up the chain of locks, limited to 8 levels. */
  while (t && depth < 8)
    {
      if (t->priority < cur->priority)
        {
          // printf ("[DONATE APPLY] %s: %d -> %d (via %s)\n",
          //         t->name, t->priority, cur->priority, cur->name);
          t->priority = cur->priority;
        }

      /* If the target itself is waiting on another lock, propagate. */
      if (t->wait_on_lock && t->wait_on_lock->holder)
        {
          // printf ("[DONATE CHAIN] now propagating to %s\n",
          //         t->wait_on_lock->holder->name);
          t = t->wait_on_lock->holder;
        }
      else
        break;

      depth++;
    }
}




/* We will need this helper function*/
void
thread_recalculate_priority(struct thread *t)
{
  ASSERT(t != NULL);

  t->priority = t->base_priority;

  if (!list_empty(&t->donations))
    {
      /* --- FIX: Sort the list to find the highest priority donor --- */
      list_sort(&t->donations, thread_donation_cmp_greater, NULL);
      /* --- END FIX --- */
      struct thread *highest =
          list_entry(list_front(&t->donations), struct thread, donation_elem);
      if (highest->priority > t->priority)
        t->priority = highest->priority;
    }
}


/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs) /* ADD THIS */
    return;         /* ADD THIS */

  enum intr_level old_level = intr_disable ();

  struct thread *cur = thread_current ();
  cur->base_priority = new_priority;

  /* Recalculate effective priority. 
     It will be max(new_priority, highest_donation_from_locks_held). */
  thread_recalculate_priority (cur);

  /* Check if we must yield to a thread in the ready list */
  if (!list_empty (&ready_list[0]))
    {
      struct thread *next_thread = list_entry (list_front (&ready_list[0]),
                                               struct thread, elem);
      if (cur->priority < next_thread->priority)
        {
          thread_yield ();
        }
    }

  intr_set_level (old_level);
}
/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  /* The spec says nice must be between -20 and 20. */
  if (nice < -20)
    nice = -20;
  if (nice > 20)
    nice = 20;

  struct thread *cur = thread_current ();
  cur->nice = nice;

  /* We must recalculate priority immediately after changing nice. */
  mlfqs_recalculate_priority (cur, NULL);

  /* If changing nice lowered our priority, we might need to yield. */
  thread_yield_if_not_highest_priority ();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* load_avg is already fixed-point. 
     Multiply by 100 and round to nearest integer. */
  return FP_TO_INT_NEAREST (FP_MUL_INT (load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* recent_cpu is already fixed-point. 
     Multiply by 100 and round to nearest integer. */
  struct thread *cur = thread_current ();
  return FP_TO_INT_NEAREST (FP_MUL_INT (cur->recent_cpu, 100));
}

/* Called after unblocking or releasing a thread to check
   whether the current thread should yield. */


/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  // printf("[KTHREAD START] %s starting (tid=%d base=%d pri=%d)\n",
  //       thread_current()->name, thread_current()->tid,
  //       thread_current()->base_priority, thread_current()->priority);
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
/* threads/thread.c */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;

  /* --- PRIORITY & DONATION INITIALIZATION --- */
  t->base_priority = priority;
  t->priority = priority;
  list_init (&t->locks_held);
  list_init (&t->donations);
  t->wait_on_lock = NULL;
  /* --- END INIT --- */

#ifdef USERPROG
  t->pagedir = NULL;
#endif

  t->magic = THREAD_MAGIC;
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (thread_mlfqs)
    {
      /* Find the highest-priority non-empty list */
      int i;
      for (i = PRI_MAX; i >= PRI_MIN; i--)
        {
          if (!list_empty (&ready_list[i]))
            {
              /* Return the front thread from that list (Round Robin) */
              return list_entry (list_pop_front (&ready_list[i]), 
                                 struct thread, elem);
            }
        }
      return idle_thread; /* No threads are ready */
    }
  else
    {
      /* Original priority scheduler logic */
      if (list_empty (&ready_list[0]))
        return idle_thread;
      else
        return list_entry (list_pop_front (&ready_list[0]), 
                           struct thread, elem);
    }
}
/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  {
    struct list_elem *e;
    // printf("[SCHEDULE] ready_list:");
    for (e = list_begin(&ready_list[0]); e != list_end(&ready_list[0]); e = list_next(e))
      {
        struct thread *tt = list_entry(e, struct thread, elem);
        // printf(" %s(%d)", tt->name, tt->priority);
      }
    // printf("\n");
  }
  debug_check_list_threads(&ready_list[0], "ready_list (before schedule)");
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);