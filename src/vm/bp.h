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

enum bp_origin_fin {
    BP_ORIGIN_FILE,
    BP_ORIGIN_ANON,
};

enum bp_store_fin {
    BP_STORE_FILE,   // 不在内存时，从 origin 重建
    BP_STORE_SWAP      // 不在内存时，从 swap 重建
};

enum bp_sharing_fin {
    BP_PRIVATE,
    BP_SHARED,
};

enum bp_busy {
    BP_NOT_BUSY,
    BP_BUSY,
};

enum bp_status {
    BP_NOT_LOADED,
    BP_LOADED,
    BP_EVICTED,   // 已经被换出，但还没有被访问到，所以还没有被加载回来
    BP_FAILED,   // 仅在加载过程中发生错误时使用，表示该页无法使用了
};

struct backing_page {
    enum bp_origin_fin origin;        // 原始来源：file / anon
    enum bp_store_fin store;          // 若非 resident，当前应从哪恢复
    enum bp_sharing_fin sharing;      // shared / private 

    bool accessed, dirty;           // 由 clock 算法使用，访问过但未被换出的页不应该被换出

    struct lock lock;             // 保护 frame/status/store/sharers
    struct list sharers;          // list of struct spte
    enum bp_busy busy;            // 阻止修改status
    struct condition busy_cond;   // 加载完成的条件变量，保护 load_status
    enum bp_status status;        // loading / loaded / evicting

    struct frame *frame;          // 若 resident，则非 NULL
    swap_slot_t slot;             // 仅 store == BP_STORE_SWAP 时有效
    

    size_t file_ref_cnt;           // 由shared_file_bps保护，只有shared_file_bps的函数才能修改下面两个字段的值
    struct hash_elem cache_elem;   // in shared_file_bps.file_bps

    union {
        struct file_info{
            struct file *file;
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
bool bp_pin_page(struct backing_page *bp, struct spte *spte);
void bp_unpin_page(struct backing_page *bp);

// used by frame.c
bool bp_evict_locked(struct backing_page *bp);
bool bp_collect_accessed_locked(struct backing_page *bp, bool clear);
bool bp_collect_dirty_locked(struct backing_page *bp, bool clear);

#endif /* vm/bp.h */