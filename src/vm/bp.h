#ifndef VM_BP_H
#define VM_BP_H

#include <stddef.h>
#include <stdbool.h>
#include <hash.h>
#include "threads/synch.h"
#include "vm/swap.h"
#include "filesys/off_t.h"

struct spte;
struct spte_desc;

enum bp_kind {
    BP_KIND_FILE,
    BP_KIND_ANON,
};

enum bp_location {
    BP_LOC_FILE,
    BP_LOC_ZERO,
    BP_LOC_SWAP,
    BP_LOC_FRAME, // 可以改名为resident
};

struct backing_page {
    enum bp_kind kind;
    enum bp_location loc;

    bool shared;
    bool busy;
    bool accessed, dirty;           // 由 clock 算法使用，访问过但未被换出的页不应该被换出

    struct lock lock;             // 保护 frame/status/store/sharers
    struct list sharers;          // list of struct spte
    struct condition busy_cond;   // 加载完成的条件变量，保护 load_status

    struct frame *frame;          // 若 resident，则非 NULL
    swap_slot_t slot;             // 仅 store == BP_STORE_SWAP 时有效
    

    size_t file_ref_cnt;           // 由shared_file_bps保护，只有shared_file_bps的函数才能修改下面两个字段的值
    struct hash_elem cache_elem;   // in shared_file_bps.file_bps

    union {
        struct file_info{
            struct inode *inode_ptr;
            off_t offset;
            off_t read_bytes;
            off_t zero_bytes;
        } file;

        struct {
            /* nothing */
        } anon;
    } origin_info;
};

void backing_page_init(void);
// used by spt in page.c
struct backing_page *bp_get_file(struct spte_desc *desc);
struct backing_page *bp_get_anon(struct spte_desc *desc);
void bp_attach(struct backing_page *bp, struct spte *spte);
void bp_detach(struct backing_page *bp, struct spte *spte);
bool bp_claim_page(struct backing_page *bp, struct spte *spte); 

// used by frame.c
bool bp_evict_locked(struct backing_page *bp);
bool bp_collect_accessed_locked(struct backing_page *bp, bool clear);
bool bp_collect_dirty_locked(struct backing_page *bp, bool clear);

#endif /* vm/bp.h */