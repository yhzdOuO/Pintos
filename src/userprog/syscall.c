#include "userprog/syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/stdio.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "vm/mmap.h"

struct file_desc
  {
    int fd;
    struct file *file;
    struct list_elem elem;
  };

static void syscall_handler (struct intr_frame *);
static void kill_process (void) NO_RETURN;
// static void check_uaddr_range (const void *uaddr, size_t size, bool writable);
// static size_t check_cstring (const char *uaddr);
static uint32_t fetch_u32 (const void *uaddr);
static size_t copy_from_user(void *kdst, const void *usrc, size_t size);
static size_t copy_to_user(void *udst, const void *ksrc, size_t size);
static bool copy_string_from_user(char *kdst, const char *usrc, size_t max_size);
static struct file_desc *fd_lookup (int fd);
static int fd_insert (struct file *file);
static void fd_close_desc (struct file_desc *desc);
static bool sys_create (const char *file_name, unsigned initial_size);
static bool sys_remove (const char *file_name);
static tid_t sys_exec (const char *cmd_line);
static int sys_open (const char *file_name);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static void sys_close (int fd);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static mapid_t sys_mmap (int fd, void *addr);
static void sys_munmap (mapid_t mapid);

static struct lock filesys_lock;

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  ASSERT ((f->cs & 3) == 3);   /* Must come from user mode. */
  const uint32_t *esp = f->esp;
  uint32_t syscall_nr = fetch_u32 (esp);

  switch (syscall_nr)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      thread_current ()->exit_code = (int) fetch_u32 (esp + 1);
      thread_exit ();
      break;

    case SYS_WAIT:
      f->eax = process_wait ((tid_t) fetch_u32 (esp + 1));
      break;

    case SYS_EXEC:
      f->eax = sys_exec ((const char *) fetch_u32 (esp + 1));
      break;

    case SYS_CREATE:
      f->eax = sys_create ((const char *) fetch_u32 (esp + 1),
                           (unsigned) fetch_u32 (esp + 2));
      break;

    case SYS_REMOVE:
      f->eax = sys_remove ((const char *) fetch_u32 (esp + 1));
      break;

    case SYS_OPEN:
      f->eax = sys_open ((const char *) fetch_u32 (esp + 1));
      break;

    case SYS_FILESIZE:
      f->eax = sys_filesize ((int) fetch_u32 (esp + 1));
      break;

    case SYS_READ:
      f->eax = sys_read ((int) fetch_u32 (esp + 1),
                         (void *) fetch_u32 (esp + 2),
                         (unsigned) fetch_u32 (esp + 3));
      break;

    case SYS_CLOSE:
      sys_close ((int) fetch_u32 (esp + 1));
      break;

    case SYS_WRITE:
      f->eax = sys_write ((int) fetch_u32 (esp + 1),
                          (const void *) fetch_u32 (esp + 2),
                          (unsigned) fetch_u32 (esp + 3));
      break;

    case SYS_SEEK:
      sys_seek ((int) fetch_u32 (esp + 1),
                (unsigned) fetch_u32 (esp + 2));
      break;

    case SYS_TELL:
      f->eax = sys_tell ((int) fetch_u32 (esp + 1));
      break;
    
    case SYS_MMAP:
      f->eax = sys_mmap ((int) fetch_u32 (esp + 1),
                         (void *) fetch_u32 (esp + 2));
      break;
    
    case SYS_MUNMAP:
      sys_munmap ((mapid_t) fetch_u32 (esp + 1));
      break;

    default:
      kill_process ();
      break;
    }
}

static void
kill_process (void)
{
  thread_current ()->exit_code = -1;
  thread_exit ();
}

/* ----------------------------- */

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}
 
/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

static size_t 
copy_from_user(void *kdst, const void *usrc, size_t size) 
{
  uintptr_t u_start = (uintptr_t) usrc;
  uintptr_t u_end = u_start + size;
  if (u_end < u_start || u_end > (uintptr_t) PHYS_BASE)
    {
      return 0;
    }
  
  struct thread *cur = thread_current ();
  uint8_t *kdst_byte = kdst;
  const uint8_t *usrc_byte = usrc;

  cur->in_user_copy = true;
  size_t i = 0;
  for (; i < size; i++) {
    int byte = get_user (usrc_byte + i);
    if (byte == -1) {
      break;
    }
    kdst_byte[i] = (uint8_t) byte;
  }
  cur->in_user_copy = false;
  return i;
}

static size_t 
copy_to_user(void *udst, const void *ksrc, size_t size) 
{
  uintptr_t u_start = (uintptr_t) udst;
  uintptr_t u_end = u_start + size;
  if (u_end < u_start || u_end > (uintptr_t) PHYS_BASE)
    {
      return 0;
    }

  struct thread *cur = thread_current ();
  uint8_t *udst_byte = udst;
  const uint8_t *ksrc_byte = ksrc;
  cur->in_user_copy = true;
  size_t i = 0;
  for (; i < size; i++) {
    if (!put_user (udst_byte + i, ksrc_byte[i])) {
      break;
    }  
  }
  cur->in_user_copy = false;
  return i;
}

static bool 
copy_string_from_user(char *kdst, const char *usrc, size_t max_len) 
{
  struct thread *cur = thread_current ();
  char *kdst_byte = kdst;
  const char *usrc_byte = usrc;
  bool result = false;
  cur->in_user_copy = true;
  while (max_len-- > 0) 
    {
      if (usrc_byte >= (const char *) PHYS_BASE) {
        break;
      }
      int byte = get_user ((const uint8_t *) usrc_byte);
      if (byte == -1) {
        break;
      }
      *kdst_byte = (char) byte;
      if (*kdst_byte == '\0') {
        result = true;
        break;
      }
      kdst_byte++;
      usrc_byte++;
    }
  cur->in_user_copy = false;
  return result;
}

/* ----------------------------- */


// static void
// check_uaddr_range (const void *uaddr, size_t size, bool writable)
// {
//   uintptr_t start = (uintptr_t) uaddr;
//   uintptr_t end;
//   struct thread *cur = thread_current ();

//   if (size == 0)
//     return;
//   if (start == 0)
//     kill_process ();

//   end = start + size - 1;
//   if (end < start
//       || !is_user_vaddr ((const void *) start)
//       || !is_user_vaddr ((const void *) end))
//     kill_process ();

//   if (!spt_is_valid_range (cur->spt, (void *) start, size, writable))
//     kill_process ();
// }

// static void
// hold_uaddr_range(const void *uaddr, size_t size) 
// {
//   syscall_filesys_lock ();
//   if (!spt_hold_range (thread_current ()->spt, (void *) uaddr, size)) {
//     syscall_filesys_unlock ();
//     kill_process ();
//   }
//   syscall_filesys_unlock ();
// }

// static void 
// release_uaddr_range(const void *uaddr, size_t size) 
// {
//   spt_release_range (thread_current ()->spt, (void *) uaddr, size);
// }

// static size_t
// check_cstring (const char *uaddr)
// {
//   size_t len = 0;

//   if (uaddr == NULL)
//     kill_process ();

  
//   while (true)
//     {
//       char *page_end = (char *) pg_round_up (uaddr + 1);
//       size_t page_left = page_end - (char *) uaddr;
//       size_t offset = 0;
//       check_uaddr_range (uaddr, page_left, false);
//       hold_uaddr_range (uaddr, page_left);
//       while (page_left-- > 0)
//         {
//           if (uaddr[offset] == '\0') {
//             release_uaddr_range (uaddr, offset + 1);
//             return len;
//           }
//           len++;
//           offset++;
//           if (len >= PGSIZE) {
//             release_uaddr_range (uaddr, offset);
//             kill_process ();
//           }
//         }
//       release_uaddr_range (uaddr, offset);
//       uaddr += offset;
//     }
// }

static uint32_t
fetch_u32 (const void *uaddr)
{
  uint32_t result;
  size_t read_bytes = copy_from_user(&result, uaddr, sizeof (uint32_t));
  if (read_bytes != sizeof (uint32_t)) {
    kill_process ();
  }
  return result;
  // check_uaddr_range (uaddr, sizeof (uint32_t), false);
  // hold_uaddr_range (uaddr, sizeof (uint32_t));
  // uint32_t value = *(const uint32_t *) uaddr;
  // release_uaddr_range (uaddr, sizeof (uint32_t));
  // return value;
}

static struct file_desc *
fd_lookup (int fd)
{
  struct list_elem *e;
  struct thread *cur = thread_current ();

  for (e = list_begin (&cur->fd_table); e != list_end (&cur->fd_table);
       e = list_next (e))
    {
      struct file_desc *desc = list_entry (e, struct file_desc, elem);
      if (desc->fd == fd)
        return desc;
    }
  return NULL;
}

static int
fd_insert (struct file *file)
{
  struct thread *cur = thread_current ();
  struct file_desc *desc = malloc (sizeof *desc);

  if (desc == NULL)
    return -1;
  desc->fd = cur->next_fd++;
  desc->file = file;
  list_push_back (&cur->fd_table, &desc->elem);
  return desc->fd;
}

static void
fd_close_desc (struct file_desc *desc)
{
  file_close (desc->file);
  list_remove (&desc->elem);
  free (desc);
}

static bool
sys_create (const char *file_name, unsigned initial_size)
{
  char *kfile_name = palloc_get_page (0);
  if (kfile_name == NULL) {
    return false;
  }
  if (!copy_string_from_user (kfile_name, file_name, PGSIZE)) {
    palloc_free_page (kfile_name);
    kill_process (); // 非法参数需要kill
  }
  
  syscall_filesys_lock ();
  bool ok = filesys_create (kfile_name, (off_t) initial_size);
  syscall_filesys_unlock ();

  palloc_free_page (kfile_name);
  return ok;

  // bool ok;

  // size_t len = check_cstring (file_name);
  // hold_uaddr_range (file_name, len + 1);

  // syscall_filesys_lock ();
  // ok = filesys_create (file_name, (off_t) initial_size);
  // syscall_filesys_unlock ();
  // release_uaddr_range (file_name, len + 1);
  // return ok;
}

static bool
sys_remove (const char *file_name)
{
  char *kfile_name = palloc_get_page (0);
  if (kfile_name == NULL) {
    return false;
  }
  if (!copy_string_from_user (kfile_name, file_name, PGSIZE)) {
    palloc_free_page (kfile_name);
    kill_process (); // 非法参数需要kill
  }

  syscall_filesys_lock ();
  bool ok = filesys_remove (kfile_name);
  syscall_filesys_unlock ();
  
  palloc_free_page (kfile_name);
  return ok;

  // bool ok;

  // size_t len = check_cstring (file_name);
  // hold_uaddr_range (file_name, len + 1);

  // syscall_filesys_lock ();
  // ok = filesys_remove (file_name);
  // syscall_filesys_unlock ();
  // release_uaddr_range (file_name, len + 1);
  // return ok;
}

static tid_t
sys_exec (const char *cmd_line)
{
  char *kcmd = palloc_get_page (0);
  if (kcmd == NULL) {
    return TID_ERROR;
  }
  if (!copy_string_from_user (kcmd, cmd_line, PGSIZE)) {
    palloc_free_page (kcmd);
    kill_process (); // 非法参数需要kill
  }
  tid_t tid = process_execute (kcmd);
  palloc_free_page (kcmd);
  return tid;

  // size_t len = check_cstring (cmd_line);
  // hold_uaddr_range (cmd_line, len + 1);
  // char *kcmd = palloc_get_page (0);
  // tid_t tid;

  // if (kcmd == NULL) {
  //   release_uaddr_range (cmd_line, len + 1);
  //   return TID_ERROR;
  // }
  // memcpy (kcmd, cmd_line, len + 1);
  // release_uaddr_range (cmd_line, len + 1);

  // tid = process_execute (kcmd);
  // palloc_free_page (kcmd);
  // return tid;
}

static int
sys_open (const char *file_name)
{
  char *kfile_name = palloc_get_page (0);
  if (kfile_name == NULL) {
    return -1;
  }
  if (!copy_string_from_user (kfile_name, file_name, PGSIZE)) {
    palloc_free_page (kfile_name);
    kill_process ();
  }
  syscall_filesys_lock ();
  struct file *file = filesys_open (kfile_name);
  syscall_filesys_unlock ();
  if (file == NULL) {
    palloc_free_page (kfile_name);
    return -1;
  }
  int fd = fd_insert (file);
  if (fd == -1) {
    syscall_filesys_lock ();
    file_close (file);
    syscall_filesys_unlock ();
  }
  palloc_free_page (kfile_name);
  return fd;

  // struct file *file;
  // int fd;

  // size_t len = check_cstring (file_name);
  // hold_uaddr_range (file_name, len + 1);
  // syscall_filesys_lock ();
  // file = filesys_open (file_name);
  // if (file == NULL)
  //   {
  //     syscall_filesys_unlock ();
  //     release_uaddr_range (file_name, len + 1);
  //     return -1;
  //   }
  // fd = fd_insert (file);
  // if (fd == -1)
  //   file_close (file);
  // syscall_filesys_unlock ();
  // release_uaddr_range (file_name, len + 1);
  // return fd;
}

static int
sys_filesize (int fd)
{
  struct file_desc *desc = fd_lookup (fd);
  int len;

  if (desc == NULL)
    return -1;

  syscall_filesys_lock ();
  len = (int) file_length (desc->file);
  syscall_filesys_unlock ();
  return len;
}

static int
sys_read (int fd, void *buffer, unsigned size)
{
  if (fd < 0 || fd == 1) {
    return -1;
  }

  uint8_t *kbuffer = palloc_get_page (0);
  if (kbuffer == NULL) {
    return -1;
  }

  size_t read_bytes = 0;
  while (size > 0) {
    size_t chunk_size = size < PGSIZE ? size : PGSIZE;
    size_t bytes = 0;

    if (fd == 0) {
      for (size_t i = 0; i < chunk_size; i++) {
        kbuffer[i] = input_getc ();
      }
      bytes = chunk_size;
    }
    else {
      struct file_desc *desc = fd_lookup (fd);
      if (desc == NULL) {
        palloc_free_page (kbuffer);
        return read_bytes > 0 ? (int)read_bytes : -1;
      }
      syscall_filesys_lock ();
      off_t ret = file_read (desc->file, kbuffer, (off_t) chunk_size);
      syscall_filesys_unlock ();
      bytes = (size_t) ret;
    }

    if (bytes == 0) {
      break;
    }
    size_t copied = copy_to_user (buffer, kbuffer, bytes);
    if (copied < bytes) {
      kill_process(); // 遇到非法访问的指针
    }
    read_bytes += copied;
    buffer = (uint8_t *) buffer + copied;
    size -= copied;
    
    if (copied < chunk_size) {
      break; // 短读 or EOF
    }
  }
  palloc_free_page (kbuffer);
  return (int) read_bytes;

  // uint8_t *dst = buffer;

  // check_uaddr_range (buffer, size, true);
  // hold_uaddr_range (buffer, size);

  // int read_bytes = -1;
  // if (fd == 0)
  //   {
  //     unsigned i;
  //     for (i = 0; i < size; i++)
  //       dst[i] = input_getc ();
  //     read_bytes = size;
  //   }
  // else
  //   {
  //     struct file_desc *desc = fd_lookup (fd);
  //     if (desc != NULL)
  //       {
  //         syscall_filesys_lock ();
  //         read_bytes = (int) file_read (desc->file, buffer, (off_t) size);
  //         syscall_filesys_unlock ();
  //       }
  //   }
  // release_uaddr_range (buffer, size);
  // return read_bytes;
}

static void
sys_close (int fd)
{
  struct file_desc *desc = fd_lookup (fd);
  if (desc != NULL)
    {
      syscall_filesys_lock ();
      fd_close_desc (desc);
      syscall_filesys_unlock ();
    }
}

static int
sys_write (int fd, const void *buffer, unsigned size)
{
  if (fd <= 0) {
    return -1;
  }

  uint8_t *kbuffer = palloc_get_page (0);
  if (kbuffer == NULL) {
    return -1;
  }

  size_t written_bytes = 0;

  while (size > 0) {
    size_t chunk_size = size < PGSIZE ? size : PGSIZE;

    size_t copied = copy_from_user (kbuffer, buffer, chunk_size);
    if (copied < chunk_size) {
      palloc_free_page (kbuffer);
      kill_process ();
    }

    size_t bytes = 0;

    if (fd == 1) {
      putbuf ((const char *) kbuffer, copied);
      bytes = copied;
    }
    else {
      struct file_desc *desc = fd_lookup (fd);
      if (desc == NULL) {
        palloc_free_page (kbuffer);
        return written_bytes > 0 ? (int)written_bytes : -1;
      }

      syscall_filesys_lock ();
      off_t ret = file_write (desc->file, kbuffer, (off_t) copied);
      syscall_filesys_unlock ();

      bytes = (size_t) ret;
    }

    written_bytes += bytes;
    buffer = (const uint8_t *) buffer + bytes;
    size -= bytes;

    if (bytes < chunk_size) {
      break;
    }
  }

  palloc_free_page (kbuffer);
  return (int) written_bytes;

  // const uint8_t *src = buffer;
  // struct file_desc *desc;

  // if (fd == 0)
  //   return -1;

  // check_uaddr_range (buffer, size, false);
  // hold_uaddr_range (buffer, size);

  // int written_bytes = -1;
  // if (fd == 1)
  //   {
  //     unsigned remaining = size;

  //     while (remaining > 0)
  //       {
  //         uint8_t kbuf[256];
  //         unsigned chunk = remaining < sizeof kbuf ? remaining : sizeof kbuf;
  //         memcpy (kbuf, src, chunk);
  //         putbuf ((const char *) kbuf, chunk);
  //         src += chunk;
  //         remaining -= chunk;
  //       }
  //     written_bytes = (int) size;
  //   }
  // else 
  //   {
  //     desc = fd_lookup (fd);
  //     if (desc != NULL) {
  //       syscall_filesys_lock ();
  //       written_bytes = (int) file_write (desc->file, buffer, (off_t) size);
  //       syscall_filesys_unlock ();
  //     }
  //   }
  // release_uaddr_range (buffer, size);
  // return written_bytes;
}

static void
sys_seek (int fd, unsigned position)
{
  struct file_desc *desc = fd_lookup (fd);

  if (desc == NULL)
    return;

  syscall_filesys_lock ();
  file_seek (desc->file, (off_t) position);
  syscall_filesys_unlock ();
}

static unsigned
sys_tell (int fd)
{
  struct file_desc *desc = fd_lookup (fd);
  unsigned pos;

  if (desc == NULL)
    return (unsigned) -1;

  syscall_filesys_lock ();
  pos = (unsigned) file_tell (desc->file);
  syscall_filesys_unlock ();
  return pos;
}

static mapid_t
sys_mmap(int fd, void *addr) {
  struct file_desc *desc = fd_lookup (fd);
  if (desc == NULL) {
    return -1;
  }
  syscall_filesys_lock ();
  struct file *file = file_reopen(desc->file);
  if (file == NULL) {
    syscall_filesys_unlock ();
    return -1;
  }
  mapid_t mapid = mmap_table_map(thread_current()->mmap_table, file, addr);
  syscall_filesys_unlock ();
  return mapid;
}

static void 
sys_munmap(mapid_t mapid) {
  struct thread *cur = thread_current ();
  syscall_filesys_lock ();
  mmap_table_unmap(cur->mmap_table, mapid);
  syscall_filesys_unlock ();
}

void
syscall_close_all_files (void)
{
  struct thread *cur = thread_current ();

  syscall_filesys_lock ();
  while (!list_empty (&cur->fd_table))
    {
      struct list_elem *e = list_pop_front (&cur->fd_table);
      struct file_desc *desc = list_entry (e, struct file_desc, elem);
      file_close (desc->file);
      free (desc);
    }
  syscall_filesys_unlock ();
}

void
syscall_filesys_lock (void)
{
  lock_acquire (&filesys_lock);
}

void
syscall_filesys_unlock (void)
{
  lock_release (&filesys_lock);
}
