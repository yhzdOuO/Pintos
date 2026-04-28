#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stdbool.h>
#include "threads/synch.h"
#include "threads/thread.h"

struct child_status
  {
    tid_t tid;
    int exit_code;
    bool waited;
    bool load_success;
    int ref_cnt;
    struct lock lock;
    struct semaphore load_sema;
    struct semaphore wait_sema;
    struct list_elem elem;
  };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */