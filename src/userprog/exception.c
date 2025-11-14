#include "userprog/exception.h"
#include "userprog/pagedir.h" /* for pagedir_get_page */
#include <inttypes.h>
#include <stdio.h>
#include <debug.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* Page fault counter. */
static long long page_fault_cnt;

static void kill(struct intr_frame *);
static void page_fault(struct intr_frame *);

void
exception_init(void) 
{
  /* User-invocable exceptions. */
  intr_register_int(3, 3, INTR_ON,  kill, "#BP Breakpoint");
  intr_register_int(4, 3, INTR_ON,  kill, "#OF Overflow");
  intr_register_int(5, 3, INTR_ON,  kill, "#BR Bound Range");

  /* Kernel-only exceptions (user cannot INT these). */
  intr_register_int(0, 0, INTR_ON,  kill, "#DE Divide Error");
  intr_register_int(1, 0, INTR_ON,  kill, "#DB Debug");
  intr_register_int(6, 0, INTR_ON,  kill, "#UD Invalid Opcode");
  intr_register_int(7, 0, INTR_ON,  kill, "#NM Device Not Available");
  intr_register_int(11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int(12, 0, INTR_ON, kill, "#SS Stack Fault");
  intr_register_int(13, 0, INTR_ON, kill, "#GP General Protection");
  intr_register_int(16, 0, INTR_ON, kill, "#MF FPU Floating-Point");
  intr_register_int(19, 0, INTR_ON, kill, "#XF SIMD Floating-Point");

  /* Page faults: interrupts disabled temporarily (CR2 read). */
  intr_register_int(14, 0, INTR_OFF, page_fault, "#PF Page Fault");
}

void
exception_print_stats(void) 
{
}

/* Kill the current process. */
static void
kill(struct intr_frame *f) 
{
  switch (f->cs)
    {
    case SEL_UCSEG:       /* User exception. */
      thread_exit();
      break;

    case SEL_KCSEG:       /* Kernel exception → bug. */
      intr_dump_frame(f);
      break;

    default:              /* Unknown segment. */
      thread_exit();
    }
}

void
page_fault (struct intr_frame *f) 
{
  void *fault_addr;
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  bool user = (f->error_code & PF_U) != 0;

  if (!user) /* Kernel-mode fault */
    {
      /* Check if fault was from get_user() */
      /* We cast f->eax to uint32_t to fix the compiler warning */
      if ((uint32_t) f->eax > 0xc0000000) 
        {
          /* Handle fault from get_user() */
          f->eip = (void *) f->eax;
          f->eax = -1;
          return;
        }
    }
  
  if (user)
    {
      /* User-mode fault */
      thread_current ()->exit_status = -1;
      thread_exit ();
    }
  else
    {
      /* Real kernel-mode fault */
      PANIC ("Kernel page fault");
    }
}