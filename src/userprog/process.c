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
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void setup_arguments (struct intr_frame *if_, const char *k_cmd_line);
/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* --- PARENT-CHILD SETUP (START) --- */
  /* Child process ko parent ka pointer dena hoga. */
  /* Hum 'fn_copy' ko temporarily ek struct ki tarah use karenge 
     taaki start_process ko parent ka pointer pass kar sakein. */
     
  struct parent_child_pass {
      struct thread *parent;
      const char *file_name;
  };
  
  struct parent_child_pass pass_data;
  pass_data.parent = thread_current();
  pass_data.file_name = fn_copy;

  /* thread_create ko 'pass_data' pass karo */
  /* Note: We pass the *original* file_name to thread_create
     for naming, but pass our 'pass_data' struct as the aux param. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process, &pass_data);
  /* --- PARENT-CHILD SETUP (END) --- */
  
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy); 
  
  return tid;
}
static void
setup_arguments (struct intr_frame *if_, const char *k_cmd_line) {
    char *token, *save_ptr;
    char *argv_tokens[128]; // Max 128 arguments
    int argc = 0;

    /* Make a temporary copy of the command line for parsing */
    char *cmd_copy = palloc_get_page(PAL_ZERO);
    if (cmd_copy == NULL) {
        printf("setup_arguments: out of memory\n");
        return; 
    }
    strlcpy(cmd_copy, k_cmd_line, PGSIZE);

    /* Parse command line into tokens */
    for (token = strtok_r(cmd_copy, " ", &save_ptr); token != NULL;
         token = strtok_r(NULL, " ", &save_ptr)) {
        argv_tokens[argc] = token;
        argc++;
        if (argc >= 128) break;
    }

    /* --- Build the 32-bit Stack (High Address to Low) --- */
    
    char *u_argv_addrs[argc]; // To store user-space addresses

    /* Step 1: Push strings (right-to-left) */
    for (int i = argc - 1; i >= 0; i--) {
        int len = strlen(argv_tokens[i]) + 1;
        if_->esp -= len; // Use esp
        memcpy((void *)if_->esp, argv_tokens[i], len);
        u_argv_addrs[i] = (char *)if_->esp;
    }

    /* Step 2: Word-align (4-byte boundary) */
    int align_pad = (uintptr_t)if_->esp % 4;
    if_->esp -= align_pad;
    memset((void *)if_->esp, 0, align_pad);

    /* Step 3: Null sentinel (argv[argc]) */
    if_->esp -= 4; // 32-bit pointer
    memset((void *)if_->esp, 0, 4);

    /* Step 4: Push argv pointers (argv[argc-1] to argv[0]) */
    for (int i = argc - 1; i >= 0; i--) {
        if_->esp -= 4; // 32-bit pointer
        memcpy((void *)if_->esp, &u_argv_addrs[i], 4);
    }

    /* Step 5: Push argv (pointer to argv[0]) */
    char *argv_0_addr = (char *)if_->esp;
    if_->esp -= 4;
    memcpy((void *)if_->esp, &argv_0_addr, 4);

    /* Step 6: Push argc */
    if_->esp -= 4;
    memcpy((void *)if_->esp, &argc, 4);

    /* Step 7: Push fake return address */
    if_->esp -= 4;
    memset((void *)if_->esp, 0, 4);

    /* --- Stack Finalized --- */
    
    palloc_free_page(cmd_copy); // Free the parsing copy
}
/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *aux) {
    
    /* --- PARENT-CHILD SETUP (CONTINUED) --- */
    struct parent_child_pass {
        struct thread *parent;
        const char *file_name;
    };
    
    struct parent_child_pass *pass_data = (struct parent_child_pass *) aux;
    char *k_cmd_line = (char *) pass_data->file_name;
    
    struct thread *cur = thread_current();
    cur->parent_thread = pass_data->parent; // Parent set karo
    
    /* Child ki tracking structures initialize karo */
    sema_init(&cur->wait_sema, 0); // Parent ispar wait karega
    cur->is_waited_on = false;
    list_init(&cur->child_list);
    lock_init(&cur->child_lock);
    
    /* Child ko parent ki list mein add karo (thread-safe) */
    lock_acquire(&cur->parent_thread->child_lock);
    list_push_back(&cur->parent_thread->child_list, &cur->child_elem);
    lock_release(&cur->parent_thread->child_lock);
    /* --- PARENT-CHILD SETUP (COMPLETE) --- */


    char *file_name;
    struct intr_frame if_;
    bool success;

    /* (Baaki code waisa hi rahega) */
    char *cmd_copy = palloc_get_page(PAL_ZERO);
    if (cmd_copy == NULL) {
        printf("start_process: out of memory\n");
        palloc_free_page(k_cmd_line);
        cur->exit_status = -1; // Set exit status before exit
        thread_exit();
    }
    strlcpy(cmd_copy, k_cmd_line, PGSIZE);

    char *save_ptr;
    file_name = strtok_r(cmd_copy, " ", &save_ptr);

    /* Initialize interrupt frame for 32-bit */
    memset (&if_, 0, sizeof if_);
    if_.cs = SEL_UCSEG;
    if_.ds = SEL_UDSEG;
    if_.es = SEL_UDSEG;
    if_.ss = SEL_UDSEG;
    if_.eflags = FLAG_IF | FLAG_MBS; 
    
    /* Load the executable */
    success = load (file_name, &if_.eip, &if_.esp);
    
    palloc_free_page(cmd_copy); 

    if (success) {
        setup_arguments(&if_, k_cmd_line); 
    }

    /* Clean up original command line copy */
    palloc_free_page (k_cmd_line);

    /* Agar load fail hua, toh parent ko -1 signal karo */
    if (!success) {
        cur->exit_status = -1;
        thread_exit ();
    }

    /* Start the user process */
    asm volatile ("movl %0, %%esp; jmp intr_exit"
              : : "g" (&if_) : "memory");
    NOT_REACHED ();
}
/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
/* Helper function: Child list mein TID se struct thread dhoondhna */
static struct thread *
get_child_by_tid (tid_t tid)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  lock_acquire(&cur->child_lock);
  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list);
       e = list_next (e))
    {
      struct thread *child = list_entry (e, struct thread, child_elem);
      if (child->tid == tid) {
        lock_release(&cur->child_lock);
        return child;
      }
    }
  lock_release(&cur->child_lock);
  return NULL; // Nahi mila
}


int
process_wait (tid_t child_tid) 
{
  /* 1. Child ko dhoondho */
  struct thread *child = get_child_by_tid (child_tid);

  /* 2. Agar child nahi mila (ya child_tid invalid hai) */
  if (child == NULL) {
    return -1;
  }
  
  /* 3. Agar child par pehle hi wait kar chuke hain */
  if (child->is_waited_on) {
      return -1;
  }
  child->is_waited_on = true;

  /* 4. Child ke khatam hone ka intezaar karo */
  /* child jab exit hoga, toh woh is semaphore ko 'up' karega */
  sema_down (&child->wait_sema);

  /* 5. Child jaag gaya, uska status return karo */
  int child_exit_status = child->exit_status;
  
  /* 6. Child ko list se hatao (cleanup) */
  /* Note: Child ka struct thread abhi free nahi kar sakte 
     kyunki woh shayad abhi bhi 'thread_exit' mein ho sakta hai.
     Proper memory management project ke baad ke hisson mein hota hai.
     Abhi ke liye, bas list se remove karo. */
  lock_acquire(&thread_current()->child_lock);
  list_remove(&child->child_elem);
  lock_release(&thread_current()->child_lock);

  return child_exit_status;
}
/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* --- MODIFICATION: Print Termination Message --- */
  /* Print the process termination message.
     The thread name is used as it holds the full command. */
  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
  /* --- END MODIFICATION --- */


  /* --- CHILD PROCESS CLEANUP --- */
  /* Apne sabhi children ko batao ki parent mar gaya hai 
     (unhe "orphans" bana do) */
  lock_acquire(&cur->child_lock);
  struct list_elem *e = list_begin (&cur->child_list);
  while (e != list_end (&cur->child_list))
    {
      struct thread *child = list_entry (e, struct thread, child_elem);
      child->parent_thread = NULL; // Ab tum orphan ho
      
      /* Agar child pehle hi exit ho chuka hai, toh uski memory free karo 
         (Advanced) - Abhi ke liye bas list clean karo */
      
      e = list_next(e);
      list_remove(&child->child_elem);
      
      /* Agar child abhi bhi zinda hai, toh 'wait_sema' ko 'up' karo
         taaki woh cleanup ho sake (agar woh wait kar raha tha) */
      if (child->status != THREAD_DYING) {
          // (Implementation optional for first pass)
      }
    }
  lock_release(&cur->child_lock);
  /* --- END CHILD CLEANUP --- */


  /* Parent ko signal bhejo ki main khatam ho gaya */
  if (cur->parent_thread != NULL) {
    sema_up (&cur->wait_sema);
  }

  /* --- MODIFICATION: Close Executable File --- */
  /* Re-allow writes and close the executable file. */
  if (cur->executable_file != NULL)
    {
      file_allow_write(cur->executable_file);
      file_close(cur->executable_file);
    }
  /* --- END MODIFICATION --- */

  /* (Original cleanup code) */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
  
  /* Note: thread_exit() ko yeh file (ya thread.c) automatically 
     call karegi. Make sure ki thread_exit() mein bhi 
     sema_up (&cur->wait_sema); ho agar process_exit call na ho.
     Safest tareeka hai ki yeh sema_up yahin rakhein, aur
     thread_exit() hamesha process_exit() ko call kare. */
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
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

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
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

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3S] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* --- MODIFICATION: Deny Write & Store File --- */
  /* Deny writes to the executable and store file in thread struct */
  file_deny_write(file);
  t->executable_file = file; // Store the file pointer in the thread
  /* --- END MODIFICATION --- */


  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
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
        default:
          /* Ignore this segment. */
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
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
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

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  
  /* --- MODIFICATION: Cleanup on Failure --- */
  /* If loading failed, we must re-allow writes and close the file.
     If successful, we leave it open (stored in t->executable_file). */
  if (!success && t->executable_file != NULL) 
    {
      file_allow_write(t->executable_file);
      file_close(t->executable_file);
      t->executable_file = NULL; // Clear pointer in thread struct
    }
  /* file_close (file); */ /* <-- Original line REMOVED */
  /* --- END MODIFICATION --- */
  
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
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
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}