#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include <string.h> // For putbuf

/* --- Global Lock --- */
static struct lock filesys_lock;

/* --- Function Prototypes for Helpers --- */
static void syscall_handler (struct intr_frame *);
static void terminate_process (void);
static int get_user (const uint8_t *uaddr);
static void validate_user_address (const void *uaddr);
static void validate_user_string (const char *ustr);
static void validate_user_buffer (const void *buffer, unsigned size);

/* ==================================
 * SYSCALL INITIALIZATION
 * ================================== */

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

/* ==================================
 * SYSCALL HANDLER
 * ================================== */

static void
syscall_handler (struct intr_frame *f) 
{
  validate_user_address(f->esp);
  int syscall_num = *(int *)f->esp;

  switch (syscall_num)
    {
    case SYS_HALT:
      {
        shutdown_power_off();
        break;
      }
      
    case SYS_EXIT:
      {
        validate_user_address(f->esp + 4);
        int status = *(int *)(f->esp + 4);
        thread_current()->exit_status = status;
        thread_exit();
        break;
      }

    case SYS_EXEC:
      {
        validate_user_address(f->esp + 4);
        const char *cmd_line = *(const char **)(f->esp + 4);
        validate_user_string(cmd_line);
        
        tid_t tid = process_execute(cmd_line);
        f->eax = tid;
        break;
      }

    case SYS_WAIT:
      {
        validate_user_address(f->esp + 4);
        tid_t pid = *(tid_t *)(f->esp + 4);
        int status = process_wait(pid);
        f->eax = status;
        break;
      }

    case SYS_CREATE:
      {
        validate_user_address(f->esp + 4); // const char *file
        validate_user_address(f->esp + 8); // unsigned initial_size

        const char *file = *(const char **)(f->esp + 4);
        unsigned initial_size = *(unsigned *)(f->esp + 8);

        validate_user_string(file);

        lock_acquire(&filesys_lock);
        bool success = filesys_create(file, initial_size);
        lock_release(&filesys_lock);

        f->eax = success;
        break;
      }

    case SYS_REMOVE:
      {
        validate_user_address(f->esp + 4); // const char *file
        const char *file = *(const char **)(f->esp + 4);
        validate_user_string(file);

        lock_acquire(&filesys_lock);
        bool success = filesys_remove(file);
        lock_release(&filesys_lock);

        f->eax = success;
        break;
      }
      
    case SYS_OPEN:
      {
        validate_user_address(f->esp + 4); // const char *file
        const char *file = *(const char **)(f->esp + 4);
        validate_user_string(file);

        struct thread *cur = thread_current();
        
        lock_acquire(&filesys_lock);
        struct file *file_ptr = filesys_open(file);
        lock_release(&filesys_lock);

        if (file_ptr == NULL) {
            f->eax = -1;
        } else {
            int fd = -1;
            // Start from 2 (0=STDIN, 1=STDOUT)
            for (int i = 2; i < 128; i++) {
                if (cur->fd_table[i] == NULL) {
                    cur->fd_table[i] = file_ptr;
                    fd = i;
                    break;
                }
            }
            
            if (fd == -1) {
                // FD table full
                file_close(file_ptr);
            }
            f->eax = fd; 
        }
        break;
      }

    case SYS_FILESIZE:
      {
        validate_user_address(f->esp + 4); // int fd
        int fd = *(int *)(f->esp + 4);
        
        int size = -1; 

        if (fd > 1 && fd < 128) 
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];
            
            if (file_ptr != NULL)
              {
                lock_acquire(&filesys_lock);
                size = file_length(file_ptr);
                lock_release(&filesys_lock);
              }
          }
        
        f->eax = size;
        break;
      }

    case SYS_READ:
      {
        validate_user_address(f->esp + 4);  // int fd
        validate_user_address(f->esp + 8);  // void *buffer
        validate_user_address(f->esp + 12); // unsigned size

        int fd = *(int *)(f->esp + 4);
        void *buffer = *(void **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);

        validate_user_buffer(buffer, size);

        int bytes_read = -1; 

        if (fd == 0) // STDIN_FILENO
          {
            uint8_t *buf_ptr = (uint8_t *) buffer;
            for (unsigned i = 0; i < size; i++) {
                buf_ptr[i] = input_getc();
            }
            bytes_read = size;
          }
        else if (fd > 1 && fd < 128) // Regular file
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];
            
            if (file_ptr == NULL) {
                bytes_read = -1; // Bad FD
            } else {
                lock_acquire(&filesys_lock);
                bytes_read = file_read(file_ptr, buffer, size);
                lock_release(&filesys_lock);
            }
          }
        
        f->eax = bytes_read;
        break;
      }
      
    case SYS_WRITE:
      {
        validate_user_address(f->esp + 4);  // int fd
        validate_user_address(f->esp + 8);  // const void *buffer
        validate_user_address(f->esp + 12); // unsigned size

        int fd = *(int *)(f->esp + 4);
        const void *buffer = *(const void **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);
        
        validate_user_buffer(buffer, size);

        int bytes_written = -1; 

        if (fd == 1) // STDOUT_FILENO
          {
            putbuf(buffer, size);
            bytes_written = size;
          }
        else if (fd > 1 && fd < 128) // Regular file
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];

            if (file_ptr == NULL) {
                bytes_written = -1; // Bad FD
            } else {
                lock_acquire(&filesys_lock);
                bytes_written = file_write(file_ptr, buffer, size);
                lock_release(&filesys_lock);
            }
          }

        f->eax = bytes_written; 
        break;
      }

    case SYS_SEEK:
      {
        validate_user_address(f->esp + 4); // int fd
        validate_user_address(f->esp + 8); // unsigned position

        int fd = *(int *)(f->esp + 4);
        unsigned position = *(unsigned *)(f->esp + 8);

        if (fd > 1 && fd < 128)
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];
            
            if (file_ptr != NULL)
              {
                lock_acquire(&filesys_lock);
                file_seek(file_ptr, position);
                lock_release(&filesys_lock);
              }
          }
        
        break;
      }

    case SYS_TELL:
      {
        validate_user_address(f->esp + 4); // int fd
        int fd = *(int *)(f->esp + 4);
        unsigned position = -1; 

        if (fd > 1 && fd < 128)
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];
            
            if (file_ptr != NULL)
              {
                lock_acquire(&filesys_lock);
                position = file_tell(file_ptr);
                lock_release(&filesys_lock);
              }
          }
        
        f->eax = position;
        break;
      }

    case SYS_CLOSE:
      {
        validate_user_address(f->esp + 4); // int fd
        int fd = *(int *)(f->esp + 4);
        
        if (fd < 2 || fd >= 128) {
            terminate_process();
        }

        struct thread *cur = thread_current();
        struct file *file_ptr = cur->fd_table[fd];

        if (file_ptr == NULL) {
            terminate_process(); // Invalid FD
        }

        file_close(file_ptr);
        cur->fd_table[fd] = NULL;
        
        f->eax = 0; // Success
        break;
      }

    default:
      printf ("Unknown system call: %d\n", syscall_num);
      terminate_process();
      break;
    }
}

/* ==================================
 * USER MEMORY VALIDATION HELPERS
 * ================================== */

/**
 * @brief Terminates the current user process with status -1.
 */
static void
terminate_process (void) 
{
  thread_current ()->exit_status = -1;
  thread_exit (); 
}

/**
 * @brief Safely reads a byte from user memory.
 * Returns the byte value or -1 on failure.
 */
static int
get_user (const uint8_t *uaddr)
{
  /* Check if the pointer is a valid user address */
  if (!is_user_vaddr (uaddr))
    return -1;
  
  /* --- THIS IS THE FIX --- */
  /* Check if the thread's page directory is valid */
  struct thread *cur = thread_current();
  if (cur->pagedir == NULL)
    return -1;
  /* ----------------------- */

  /* Check if the page is mapped */
  if (pagedir_get_page(cur->pagedir, uaddr) == NULL)
    return -1;

  /* All checks passed, try to read the byte */
  int result;
  asm volatile ("movl $1f, %%eax; movzbl %1, %0; 1:"
               : "=&a" (result) : "m" (*uaddr));
  return result;
}

/**
 * @brief Validates a single user virtual address.
 * If invalid, terminates the process.
 */
static void
validate_user_address (const void *uaddr)
{
  if (get_user((const uint8_t*) uaddr) == -1)
    {
      terminate_process();
    }
}

/**
 * @brief Validates a user-provided string.
 */
static void
validate_user_string (const char *ustr)
{
  while (true)
    {
      int byte = get_user((const uint8_t*) ustr);
      
      if (byte == -1) // Invalid address
        {
          terminate_process();
        }
      if (byte == '\0') // End of string
        {
          break;
        }
      ustr++; // Move to next byte
    }
}

/**
 * @brief Validates a user-provided buffer.
 */
static void
validate_user_buffer (const void *buffer, unsigned size)
{
  const char *buf_ptr = (const char *) buffer;
  for (unsigned i = 0; i < size; i++)
    {
      validate_user_address(buf_ptr + i);
    }
}