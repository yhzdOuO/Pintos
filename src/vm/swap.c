#include <debug.h>
#include <bitmap.h>
#include <stdio.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "vm/swap.h"
#include "devices/block.h"

#define BLOCKS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

struct swap_table {
    struct block *block;
    struct bitmap *used_map;
    size_t slot_cnt;
    struct lock lock; //只锁used_map和slot_cnt，block底层调用ide_read/write，已经实现同步
};

static struct swap_table swap;

static inline block_sector_t 
slot_to_sector(swap_slot_t slot) {
    return slot * BLOCKS_PER_PAGE;
}

void 
swap_init(void) {
    // initialize swap_table, e.g. get block, init bitmap, etc.
    swap.block = block_get_role(BLOCK_SWAP);
    if (swap.block == NULL) {
        PANIC("No swap block device found, can't initialize swap.");
    }
    swap.slot_cnt = block_size(swap.block) / BLOCKS_PER_PAGE; //页框与扇区并非一样大
    swap.used_map = bitmap_create(swap.slot_cnt);
    if (swap.used_map == NULL) {
        PANIC("Failed to create bitmap for swap table.");
    }
    lock_init(&swap.lock);
}

swap_slot_t 
swap_alloc_slot(void) {
    lock_acquire(&swap.lock);
    swap_slot_t slot = bitmap_scan_and_flip(swap.used_map, 0, 1, false);
    if (slot == BITMAP_ERROR) {
        lock_release(&swap.lock);
        return SLOT_ERROR; // no free slot
    }
    lock_release(&swap.lock);
    // printf("slot rest = %zu\n",bitmap_count(swap.used_map, 0, swap.slot_cnt, false));
    return slot;
}

void 
swap_free_slot(swap_slot_t slot) {
    if (slot >= swap.slot_cnt || slot == SLOT_ERROR) {
        PANIC("Invalid swap slot: %zu", slot);
    }
    lock_acquire(&swap.lock);
    bitmap_reset(swap.used_map, slot);
    lock_release(&swap.lock);
}

void
swap_write_page(swap_slot_t slot, void *kpage)
{
    if (slot >= swap.slot_cnt)
        PANIC("Invalid swap slot: %zu", (size_t) slot);

    block_sector_t base = slot_to_sector(slot);
    const uint8_t *page = kpage;

    for (size_t i = 0; i < BLOCKS_PER_PAGE; i++) {
        block_write(swap.block, base + i, page + i * BLOCK_SECTOR_SIZE);
    }
}

void
swap_read_page(swap_slot_t slot, void *kpage)
{
    if (slot >= swap.slot_cnt)
        PANIC("Invalid swap slot: %zu", (size_t) slot);

    block_sector_t base = slot_to_sector(slot);
    uint8_t *page = kpage;
    
    for (size_t i = 0; i < BLOCKS_PER_PAGE; i++) {
        block_read(swap.block, base + i, page + i * BLOCK_SECTOR_SIZE);
    }
}