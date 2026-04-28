#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>
#include "devices/block.h"

void swap_init(void);

typedef size_t swap_slot_t;
#define SLOT_ERROR ((size_t) -1)

swap_slot_t swap_alloc_slot(void);                // 分配一个 slot
void swap_free_slot(swap_slot_t slot);            // 释放一个 slot
void swap_write_page(swap_slot_t slot, void *kpage);
void swap_read_page(swap_slot_t slot, void *kpage);

#endif /* vm/swap.h */