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

static struct lock filesys_lock;
static void syscall_handler (struct intr_frame *);

/* ==================================
 * USER MEMORY VALIDATION HELPERS
 * ================================== */
static int get_user (const uint8_t *uaddr);
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
 * @brief Validates a single user virtual address 'uaddr'.
 * If the check fails, the user process is terminated.
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
 * @brief Validates a buffer in user memory.
 */
static void
validate_user_buffer (const void *buffer, unsigned size)
{
  const char *buf_ptr = (const char *) buffer;
  for (unsigned i = 0; i < size; i++)
    {
      /* We just need to check the byte is readable.
         We validate (buf_ptr + i) instead of *buf_ptr
         to check the pointer itself. */
      validate_user_address(buf_ptr + i);
    }
}
/**
 * @brief Validates a null-terminated user string.
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

/* ==================================
 * SYSCALL HANDLER
 * ================================== */
static int
get_user (const uint8_t *uaddr)
{
  /* Check that the pointer is a valid user address */
  if (!is_user_vaddr (uaddr))
    return -1;

  /* Check if the page is mapped (optional but good) */
  if (pagedir_get_page(thread_current()->pagedir, uaddr) == NULL)
    return -1;

  /* Use a special assembly trick to read the byte.
     If the read causes a page fault, the CPU will jump
     to the '1f' (local label 1 forward) and set 'result'
     to -1, instead of panicking the kernel. */
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

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
      
    case SYS_WRITE:
      {
        // 1. Validate stack arguments
        validate_user_address(f->esp + 4);  // int fd
        validate_user_address(f->esp + 8);  // const void *buffer
        validate_user_address(f->esp + 12); // unsigned size

        // 2. Get arguments
        int fd = *(int *)(f->esp + 4);
        const void *buffer = *(const void **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);
        
        // 3. Validate the user buffer
        validate_user_buffer(buffer, size);

        int bytes_written = -1; // Default to error

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
                // Perform the write
                lock_acquire(&filesys_lock);
                bytes_written = file_write(file_ptr, buffer, size);
                lock_release(&filesys_lock);
            }
          }

        f->eax = bytes_written; // Return number of bytes written
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

    /* --- STUBS FOR OTHER SYSCALLS --- */
    case SYS_READ:
      {
        // 1. Validate stack arguments
        validate_user_address(f->esp + 4);  // int fd
        validate_user_address(f->esp + 8);  // void *buffer
        validate_user_address(f->esp + 12); // unsigned size

        // 2. Get arguments
        int fd = *(int *)(f->esp + 4);
        void *buffer = *(void **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);

        // 3. Validate the user buffer
        validate_user_buffer(buffer, size);

        int bytes_read = -1; // Default to error

        if (fd == 0) // STDIN_FILENO
          {
            // Read from keyboard
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
                // Perform the read
                lock_acquire(&filesys_lock);
                bytes_read = file_read(file_ptr, buffer, size);
                lock_release(&filesys_lock);
            }
          }
        
        f->eax = bytes_read; // Return number of bytes read
        break;
      }
    case SYS_FILESIZE:
  {
    // 1. Validate stack argument
    validate_user_address(f->esp + 4); // int fd
    int fd = *(int *)(f->esp + 4);

    int size = -1; // Default to error

    if (fd > 1 && fd < 128) // Regular file
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
case SYS_REMOVE:
      {
        // 1. Validate stack argument
        validate_user_address(f->esp + 4); // const char *file
        
        // 2. Get argument
        const char *file = *(const char **)(f->esp + 4);

        // 3. Validate user string
        validate_user_string(file);

        // 4. Remove the file
        lock_acquire(&filesys_lock);
        bool success = filesys_remove(file);
        lock_release(&filesys_lock);

        // 5. Return success status
        f->eax = success;
        break;
      }

    /* * --- SYS_SEEK (Implemented) ---
     * Changes the next byte to be read/written in an open file.
     */
    case SYS_SEEK:
      {
        // 1. Validate stack arguments
        validate_user_address(f->esp + 4); // int fd
        validate_user_address(f->esp + 8); // unsigned position

        // 2. Get arguments
        int fd = *(int *)(f->esp + 4);
        unsigned position = *(unsigned *)(f->esp + 8);

        // 3. Find the file in the FD table
        if (fd > 1 && fd < 128) // Check for valid fd
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];
            
            if (file_ptr != NULL)
              {
                // 4. Seek the file
                lock_acquire(&filesys_lock);
                file_seek(file_ptr, position);
                lock_release(&filesys_lock);
              }
          }
        
        // This syscall does not return a value.
        break;
      }

    /* * --- SYS_TELL (Implemented) ---
     * Returns the position of the next byte to be read/written.
     */
    case SYS_TELL:
      {
        // 1. Validate stack argument
        validate_user_address(f->esp + 4); // int fd
        
        // 2. Get argument
        int fd = *(int *)(f->esp + 4);

        unsigned position = -1; // Default to error

        // 3. Find the file in the FD table
        if (fd > 1 && fd < 128) // Check for valid fd
          {
            struct thread *cur = thread_current();
            struct file *file_ptr = cur->fd_table[fd];
            
            if (file_ptr != NULL)
              {
                // 4. Get the file's current position
                lock_acquire(&filesys_lock);
                position = file_tell(file_ptr);
                lock_release(&filesys_lock);
              }
          }
        
        // 5. Return the position
        f->eax = position;
        break;
      }

    default:
      printf ("Unknown system call: %d\n", syscall_num);
      terminate_process();
      break;
    }
}