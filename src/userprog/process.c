/* userprog/process.c
   Pintos process loading
*/

#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void setup_arguments (struct intr_frame *if_, const char *k_cmd_line);
static bool install_page (void *upage, void *kpage, bool writable);

/* Structure used to pass data from parent to child in a safe kernel page. */
struct parent_child_pass {
  struct thread *parent;
  char *file_name; /* kernel page containing command line copy */
  struct semaphore load_sema; 
  bool load_success;          
};

/* Starts a new thread running a user program loaded from FILENAME.
   Returns TID_ERROR on error. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *thread_name; 
  char *save_ptr;    
  tid_t tid;

  /* Make a kernel-page copy for the child's k_cmd_line */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Make a second copy just to get the thread name */
  thread_name = palloc_get_page (0);
  if (thread_name == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (thread_name, file_name, PGSIZE);
  
  /* Parse the program name for thread_create() */
  char *program_name = strtok_r (thread_name, " \t\n", &save_ptr);

  /* Allocate kernel page to hold parent-child pass struct. */
  struct parent_child_pass *pass = palloc_get_page (0);
  if (pass == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (thread_name); 
      return TID_ERROR;
    }
  pass->parent = thread_current ();
  pass->file_name = fn_copy; // Pass the FULL command line
  sema_init (&pass->load_sema, 0);
  pass->load_success = false;

  /* Create thread with *only* the program name */
  tid = thread_create (program_name, PRI_DEFAULT, start_process, pass);

  /* Free the copy of the thread name */
  palloc_free_page (thread_name);

  if (tid == TID_ERROR)
    {
      /* Cleanup on failure */
      palloc_free_page (fn_copy);
      palloc_free_page (pass);
    }
  else
    {
      /* Wait for the child to attempt loading */
      sema_down (&pass->load_sema);
      if (!pass->load_success)
        {
          tid = TID_ERROR;
        }
    }
  
  /* Parent is now done with 'pass', so free it. */
  palloc_free_page (pass);
  return tid;
}

/* Build user stack arguments on the intr_frame's esp. */
static void
setup_arguments (struct intr_frame *if_, const char *k_cmd_line)
{
  /* Make a temporary kernel copy to tokenize. */
  char *cmd_copy = palloc_get_page (PAL_ZERO);
  if (cmd_copy == NULL)
    {
      return;
    }
  strlcpy (cmd_copy, k_cmd_line, PGSIZE);

  char *argv_tokens[128];
  int argc = 0;
  char *save_ptr;
  for (char *tok = strtok_r (cmd_copy, " \t\n", &save_ptr);
       tok != NULL && argc < (int)(sizeof argv_tokens / sizeof argv_tokens[0]);
       tok = strtok_r (NULL, " \t\n", &save_ptr))
    {
      argv_tokens[argc++] = tok;
    }

  /* Build stack: push strings, align, push pointers, push argc, push fake ret. */
  uintptr_t esp = (uintptr_t) if_->esp;
  uintptr_t arg_uaddrs[128];

  /* Push strings (right-to-left) */
  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv_tokens[i]) + 1;
      esp -= len;
      memcpy ((void *) esp, argv_tokens[i], len);
      arg_uaddrs[i] = esp;
    }

  /* Word-align to 4 bytes */
  size_t align = esp % sizeof (char *);
  if (align)
    {
      esp -= align;
      memset ((void *) esp, 0, align);
    }

  /* Null sentinel */
  esp -= sizeof (char *);
  memset ((void *) esp, 0, sizeof (char *));

  /* Push argument pointers */
  for (int i = argc - 1; i >= 0; i--)
    {
      esp -= sizeof (char *);
      memcpy ((void *) esp, &arg_uaddrs[i], sizeof (char *));
    }

  /* argv (pointer to argv[0]) */
  uintptr_t argv_addr = esp;
  esp -= sizeof (char *);
  memcpy ((void *) esp, &argv_addr, sizeof (char *));

  /* argc */
  esp -= sizeof (int);
  memcpy ((void *) esp, &argc, sizeof (int));

  /* fake return address */
  esp -= sizeof (void *);
  memset ((void *) esp, 0, sizeof (void *));

  if_->esp = (void *) esp;


  palloc_free_page (cmd_copy);
}

/* Thread function to load and start the user process. */
static void
start_process (void *aux)
{
  struct parent_child_pass *pass = aux;
  struct thread *parent = pass->parent;
  char *k_cmd_line = pass->file_name;
  bool success = false; 

  struct thread *cur = thread_current ();
  
  cur->parent_thread = parent;
  sema_init (&cur->wait_sema, 0);
  sema_init (&cur->reap_sema, 0);
  cur->is_waited_on = false;
  list_init (&cur->child_list);
  lock_init (&cur->child_lock);
  if (parent != NULL) {
    lock_acquire (&parent->child_lock);
    list_push_back (&parent->child_list, &cur->child_elem);
    lock_release (&parent->child_lock);
  }

  cur->pagedir = pagedir_create ();
  if (cur->pagedir == NULL) {
      cur->exit_status = -1;
      pass->load_success = false;
      sema_up (&pass->load_sema);
      palloc_free_page (k_cmd_line);
      palloc_free_page (pass);
      thread_exit ();
  }
  process_activate ();
  cur->next_fd = 2;
  for (int i = 0; i < FDCOUNT_LIMIT; i++)
    cur->fd_table[i] = NULL;
  
  struct intr_frame if_;
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = SEL_UDSEG;
  if_.ds = SEL_UDSEG;
  if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  char *cmd_copy = palloc_get_page (PAL_ZERO);
  if (cmd_copy == NULL) {
      cur->exit_status = -1;
      pass->load_success = false;
      sema_up (&pass->load_sema);
      palloc_free_page (k_cmd_line);
      palloc_free_page (pass);
      thread_exit ();
  }
  strlcpy (cmd_copy, k_cmd_line, PGSIZE);

  char *save_ptr;
  char *file_name = strtok_r (cmd_copy, " \t\n", &save_ptr);

  /* Load executable */
  success = load (file_name, &if_.eip, &if_.esp);
  palloc_free_page (cmd_copy);

  /* Signal parent *after* load attempt */
  pass->load_success = success;
  sema_up (&pass->load_sema);

  if (success)
    {
      setup_arguments (&if_, k_cmd_line);
    }

  /* Free resources (child no longer needs them) */
  palloc_free_page (k_cmd_line);
  /* palloc_free_page (pass); <-- This is correctly removed */ 

  /* Exit if load failed */
  if (!success)
    {
      cur->exit_status = -1;
      thread_exit ();
    }

  /* Jump to user mode */
  asm volatile ("movl %0, %%esp; jmp intr_exit"
                :
                : "g" (&if_)
                : "memory");

  NOT_REACHED ();
}

/* Look up child thread by tid in current thread's child list. */
static struct thread *
get_child_by_tid (tid_t tid)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  lock_acquire (&cur->child_lock);
  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list);
       e = list_next (e))
    {
      struct thread *child = list_entry (e, struct thread, child_elem);
      if (child->tid == tid)
        {
          lock_release (&cur->child_lock);
          return child;
        }
    }
  lock_release (&cur->child_lock);
  return NULL;
}

/* Wait for a child process to die and return its exit status. */
int
process_wait (tid_t child_tid) 
{
  struct thread *child = get_child_by_tid (child_tid);

  if (child == NULL)
    return -1;
  if (child->is_waited_on)
    return -1;

  child->is_waited_on = true;

  sema_down (&child->wait_sema);

  int status = child->exit_status;

  lock_acquire (&thread_current ()->child_lock);
  list_remove (&child->child_elem);
  lock_release (&thread_current ()->child_lock);
  sema_up (&child->reap_sema);
  return status;
}

/* Free current process resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
  uint32_t *pd;

  /* Reparent children (simple) */
  lock_acquire (&cur->child_lock);
  struct list_elem *e = list_begin (&cur->child_list);
  while (e != list_end (&cur->child_list))
    {
      struct thread *child = list_entry (e, struct thread, child_elem);
      child->parent_thread = NULL;
      sema_up (&child->reap_sema);
      e = list_next (e);
      list_remove (&child->child_elem);
    }
  lock_release (&cur->child_lock);


  /* Close executable file */
  if (cur->executable_file != NULL)
    {
      file_allow_write (cur->executable_file);
      file_close (cur->executable_file);
      cur->executable_file = NULL;
    }


  /* Signal parent (if present) */
  if (cur->parent_thread != NULL){
    sema_up (&cur->wait_sema);
    sema_down (&cur->reap_sema);
  }

  
  /* Destroy pagedir */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Activate process's page tables (called on context switch). */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ELF loading helpers below. */

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half    e_type;
  Elf32_Half    e_machine;
  Elf32_Word    e_version;
  Elf32_Addr    e_entry;
  Elf32_Off     e_phoff;
  Elf32_Off     e_shoff;
  Elf32_Word    e_flags;
  Elf32_Half    e_ehsize;
  Elf32_Half    e_phentsize;
  Elf32_Half    e_phnum;
  Elf32_Half    e_shentsize;
  Elf32_Half    e_shnum;
  Elf32_Half    e_shstrndx;
};

struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off  p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable into the current thread. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Open executable. */
  file = filesys_open (file_name);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", file_name);
      goto done;
    }

  /* Deny write to executable and remember file */
  file_deny_write (file);
  t->executable_file = file;

  /* Read and verify header */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;

      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

 done:
  if (!success && t->executable_file != NULL)
    {
      file_allow_write (t->executable_file);
      file_close (t->executable_file);
      t->executable_file = NULL;
    }
  return success;
}

/* Helpers */

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false; 
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;
  if (phdr->p_memsz < phdr->p_filesz) /* FIX: Was phdr.p_filesz */
    return false; 
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) /* FIX: Was phdr.p_vaddr */
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;
  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false; 
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  void *upage = (void *) ((uint8_t *) PHYS_BASE - PGSIZE);

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  if (!install_page (upage, kpage, true))
    {
      palloc_free_page (kpage);
      return false;
    }

  *esp = PHYS_BASE;
  return true;
}

/* Map UPAGE to KPAGE in the current thread's pagedir. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}