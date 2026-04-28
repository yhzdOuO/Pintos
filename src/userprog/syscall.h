#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void syscall_close_all_files (void);
void syscall_filesys_lock (void);
void syscall_filesys_unlock (void);

#endif /* userprog/syscall.h */
