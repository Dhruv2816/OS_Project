#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
/* --- Add these includes --- */
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h" // For SYS_HALT
#include "devices/input.h"    // For SYS_READ
/* -------------------------- */
#include "threads/synch.h"
#include "filesys/filesys.h" 
#include "filesys/file.h"
#include "userprog/process.h"
static struct lock filesys_lock;
static void syscall_handler (struct intr_frame *);

/*
 * =================================================================
 * USER MEMORY VALIDATION HELPERS
 * =================================================================
 */

/**
 * @brief Terminates the current user process with status -1.
 * This is called when the kernel detects an invalid memory access
 * or invalid arguments from the user process.
 */
static void
terminate_process (void) 
{
  /*
   * Set the exit status to -1 (error) so the parent 
   * (if waiting) can retrieve it.
   */
  thread_current ()->exit_status = -1;
  thread_exit (); 
}

/**
 * @brief Validates a single user virtual address 'uaddr'.
 *
 * Checks if 'uaddr' is:
 * 1. Not a null pointer.
 * 2. Within the user virtual address space (below PHYS_BASE).
 * 3. Mapped in the process's page directory.
 *
 * If any check fails, the user process is terminated with status -1.
 *
 * @param uaddr The user virtual address to check.
 */
static void
validate_user_address (const void *uaddr)
{
  struct thread *cur = thread_current ();
  
  // 1. Check for null pointer
  if (uaddr == NULL) {
    terminate_process();
  }

  // 2. Check if it's in user virtual address space
  if (!is_user_vaddr(uaddr)) {
    terminate_process();
  }

  // 3. Check if it's mapped in the page directory
  if (pagedir_get_page(cur->pagedir, uaddr) == NULL) {
    terminate_process();
  }
}

/**
 * @brief Validates a buffer in user memory.
 *
 * Checks that the entire buffer from 'buffer' to 'buffer + size - 1'
 * consists of valid, mapped user memory addresses. 
 *
 * @param buffer The starting address of the buffer.
 * @param size   The size of the buffer in bytes.
 */
static void
validate_user_buffer (const void *buffer, unsigned size)
{
  if (size == 0) {
    return; // Nothing to check
  }
  const char *buf_ptr = (const char *) buffer;
  // Check the first byte and the last byte
  validate_user_address(buf_ptr);
  validate_user_address(buf_ptr + size - 1);
  
  // Check all page boundaries in between (efficient)
  char *ptr = (char *) pg_round_down(buffer);
  char *end_ptr = (char *) pg_round_down(buffer + size - 1);
  for (; ptr <= end_ptr; ptr += PGSIZE) 
  {
    validate_user_address(ptr);
  }
}

/**
 * @brief Validates a null-terminated user string.
 *
 * Checks that every byte of the string, including the null 
 * terminator, is in valid, mapped user memory.
 *
 * @param ustr Pointer to the start of the user string.
 */
static void
validate_user_string (const char *ustr)
{
  validate_user_address(ustr); // Check the first byte
  const char *ptr = ustr;
  while (true) 
  {
    validate_user_address(ptr);
    if (*ptr == '\0') {
      break; // Found the null terminator
    }
    ptr++; // Move to the next byte
  }
}

/*
 * =================================================================
 * END OF USER MEMORY VALIDATION HELPERS
 * =================================================================
 */


void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  // First, validate the stack pointer itself.
  validate_user_address(f->esp);

  // Get the system call number from the user stack
  int syscall_num = *(int *)f->esp;

  switch (syscall_num)
    {
    case SYS_HALT:
      {
        shutdown_power_off();
        break;
      }
      
    /* * --- SYS_EXIT (Implemented) ---
     * Reads the exit status from the stack, saves it in the
     * thread structure, and terminates the thread.
     */
    case SYS_EXIT:
      {
        // 1. Validate the stack pointer for the status argument
        validate_user_address(f->esp + 4);
        
        // 2. Get the status argument
        int status = *(int *)(f->esp + 4);
        
        // 3. Save the exit status in the thread structure
        thread_current()->exit_status = status;
        
        // 4. Terminate the thread
        thread_exit();
        break;
      }
      
    /* * --- SYS_WRITE (Partial) ---
     * Handles writing to the console (fd = 1).
     */
    case SYS_WRITE:
      {
        // 1. Validate all 3 arguments on the stack
        validate_user_address(f->esp + 4);  // int fd
        validate_user_address(f->esp + 8);  // const void *buffer
        validate_user_address(f->esp + 12); // unsigned size

        // 2. Get arguments
        int fd = *(int *)(f->esp + 4);
        const void *buffer = *(const void **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);
        
        // 3. Validate the entire user buffer
        validate_user_buffer(buffer, size);

        // 4. Perform the write
        if (fd == 1) { // STDOUT_FILENO
          putbuf(buffer, size);
          f->eax = size; // Return number of bytes written
        } else {
          // TODO: Implement file writing for other fds
          f->eax = -1; // Fail for now
        }
        break;
      }
    /* * --- SYS_EXEC (Implemented) ---
     * Reads the command line string from the stack,
     * validates it, and starts a new process.
     * Returns the new process's TID or -1 on failure.
     */
    case SYS_EXEC:
      {
        // 1. Validate the stack pointer for the cmd_line argument
        validate_user_address(f->esp + 4);
        
        // 2. Get the const char *cmd_line argument
        const char *cmd_line = *(const char **)(f->esp + 4);
        
        // 3. Validate the entire command line string
        validate_user_string(cmd_line);
        
        // 4. Call process_execute
        /* Note: process_execute is complex and can fail (e.g., OOM).
           It will return TID_ERROR (-1) on failure. */
        tid_t tid = process_execute(cmd_line);
        
        // 5. Return the new TID to the user process
        f->eax = tid;
        break;
      }

    /* * --- SYS_WAIT (Implemented) ---
     * Reads the child TID (pid) from the stack
     * and calls process_wait() to wait for it.
     * Returns the child's exit status.
     */
    case SYS_WAIT:
      {
        // 1. Validate the stack pointer for the pid argument
        validate_user_address(f->esp + 4);
        
        // 2. Get the tid_t pid argument
        tid_t pid = *(tid_t *)(f->esp + 4);
        
        // 3. Call process_wait
        /* process_wait handles all logic:
           - Checking if pid is a valid child
           - Checking if it's already waited on
           - Blocking the parent
           - Returning the exit status
           - Returns -1 if wait fails */
        int status = process_wait(pid);
        
        // 4. Return the exit status to the user process
        f->eax = status;
        break;
      }

    case SYS_CREATE:
      {
        // 1. Validate stack arguments
        validate_user_address(f->esp + 4); // const char *file
        validate_user_address(f->esp + 8); // unsigned initial_size

        // 2. Get arguments
        const char *file = *(const char **)(f->esp + 4);
        unsigned initial_size = *(unsigned *)(f->esp + 8);

        // 3. Validate user string
        validate_user_string(file);

        // 4. File system ko access karo (lock ke sath)
        lock_acquire(&filesys_lock);
        bool success = filesys_create(file, initial_size);
        lock_release(&filesys_lock);

        // 5. Result return karo
        f->eax = success;
        break;
      }
    case SYS_OPEN:
      {
        // 1. Validate stack argument
        validate_user_address(f->esp + 4); // const char *file

        // 2. Get argument
        const char *file = *(const char **)(f->esp + 4);

        // 3. Validate user string
        validate_user_string(file);

        struct thread *cur = thread_current();
        
        lock_acquire(&filesys_lock);
        struct file *file_ptr = filesys_open(file);
        lock_release(&filesys_lock);

        if (file_ptr == NULL) {
            // File nahi mili
            f->eax = -1;
        } else {
            // File mil gayi, FD table mein add karo
            
            // Agla free FD dhoondo (simple linear scan)
            // Hum 2 se start karte hain (0=STDIN, 1=STDOUT)
            int fd = -1;
            for (int i = 2; i < 128; i++) {
                if (cur->fd_table[i] == NULL) {
                    cur->fd_table[i] = file_ptr;
                    fd = i;
                    break;
                }
            }
            
            if (fd == -1) {
                // FD table full hai, file ko wapas close kardo
                file_close(file_ptr);
            }
            
            f->eax = fd; // Naya FD (ya -1 agar fail)
        }
        break;
      }

    /* * --- SYS_CLOSE (Implemented) ---
     * File ko close karta hai.
     */
    case SYS_CLOSE:
      {
        // 1. Validate stack argument
        validate_user_address(f->esp + 4); // int fd

        // 2. Get argument
        int fd = *(int *)(f->esp + 4);
        
        // 3. FD ko validate karo
        if (fd < 2 || fd >= 128) { // 0 aur 1 ko close nahi kar sakte
            terminate_process(); // Ya f->eax = -1;
            break;
        }

        struct thread *cur = thread_current();
        struct file *file_ptr = cur->fd_table[fd];

        if (file_ptr == NULL) {
            // Invalid FD (pehle se closed ya kabhi open nahi hua)
            terminate_process(); // Ya f->eax = -1;
            break;
        }

        // File close karo aur FD table se hatao
        file_close(file_ptr);
        cur->fd_table[fd] = NULL;
        
        f->eax = 0; // Success (optional)
        break;
      }
    // ... other cases (SYS_CREATE, SYS_OPEN, etc.) ...

    default:
      printf ("Unknown system call: %d\n", syscall_num);
      terminate_process();
      break;
    }
}