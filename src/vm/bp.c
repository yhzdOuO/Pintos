
#include <string.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/bp.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/pmap.h"
#include "filesys/inode.h"
#include "filesys/file.h"

/******************** bp functions ********************/
static struct shared_backing_page {
    struct lock lock;
    struct hash file_bps; // key: (file, offset, read_bytes, zero_bytes), value: struct file_source*
} shared_file_bps;

static unsigned file_source_hash_func(const struct hash_elem *e, void *aux UNUSED);
static bool file_source_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

static bool bp_page_in_locked(struct backing_page *bp, struct frame *f);
static bool bp_page_out_locked(struct backing_page *bp);
static bool bp_page_destroy_locked(struct backing_page *bp);

static bool bp_load_locked(struct backing_page *bp);
// bool bp_claim_page(struct backing_page *bp, struct spte *spte); --- IGNORE ---

// bool bp_evict_locked(struct backing_page *bp); --- IGNORE ---

static bool bp_free_up(struct backing_page *bp);
// bp_detatch

static void bp_clear_pages_locked(struct backing_page *bp);

// 实际上private+COW也算是一种共享

void 
backing_page_init(void) {
    lock_init(&shared_file_bps.lock);
    hash_init(&shared_file_bps.file_bps, file_source_hash_func, file_source_less_func, NULL);
}

static void
bp_init_file(struct backing_page *bp, struct spte_desc *desc) {
    bp->kind = BP_KIND_FILE;
    bp->loc = BP_LOC_FILE;

    bp->shared = (desc->sharing == SPTE_SHARED);
    bp->busy = false;
    bp->accessed = bp->dirty = false;

    lock_init(&bp->lock);
    list_init(&bp->sharers);
    cond_init(&bp->busy_cond);

    bp->frame = NULL;
    bp->slot = SLOT_ERROR;

    bp->file_ref_cnt = 1;

    bp->origin_info.file.inode_ptr = file_get_inode(desc->file);
    bp->origin_info.file.offset = desc->offset;
    bp->origin_info.file.read_bytes = desc->read_bytes;
    bp->origin_info.file.zero_bytes = desc->zero_bytes;
}

static void 
bp_init_zero(struct backing_page *bp, struct spte_desc *desc) {
    bp->kind = BP_KIND_ANON;
    bp->loc = BP_LOC_ZERO;

    bp->shared = false;
    bp->busy = false;
    bp->accessed = bp->dirty = false;
    
    lock_init(&bp->lock);
    list_init(&bp->sharers);
    cond_init(&bp->busy_cond);
    
    bp->frame = NULL;
    bp->slot = SLOT_ERROR;
    
    bp->file_ref_cnt = 1;
}

struct backing_page *
bp_get_file(struct spte_desc *desc) {
    struct backing_page *bp = malloc(sizeof(struct backing_page));
    if (bp == NULL) {
        return NULL;
    }
    
    bp_init_file(bp, desc);

    if (bp->shared) {
        lock_acquire(&shared_file_bps.lock);
        // 先查全局共享缓存
        struct hash_elem *e = hash_find(&shared_file_bps.file_bps, &bp->cache_elem);
        if (e != NULL) {
            free(bp);
            bp = hash_entry(e, struct backing_page, cache_elem);
            bp->file_ref_cnt++;
        }
        else {
            hash_insert(&shared_file_bps.file_bps, &bp->cache_elem);
            bp->file_ref_cnt = 1;
        }
        lock_release(&shared_file_bps.lock);
    }

    return bp;
}

struct backing_page *
bp_get_anon(struct spte_desc *desc) {
    struct backing_page *bp = malloc(sizeof(struct backing_page));
    if (bp == NULL) {
        return NULL;
    }
    
    ASSERT(desc->sharing == SPTE_PRIVATE); // zero page 目前只支持 private

    bp_init_zero(bp, desc);
    return bp;
}

void 
bp_attach(struct backing_page *bp, struct spte *spte) {
    lock_acquire(&bp->lock);
    list_push_back(&bp->sharers, &spte->bp_elem);
    lock_release(&bp->lock);
}

void 
bp_detach(struct backing_page *bp, struct spte *spte) {
    lock_acquire(&bp->lock);
    bp->accessed |= pmap_test_spte_accessed(spte, false);
    bp->dirty |= pmap_test_spte_dirty(spte, false);
    pmap_clear_spte_page(spte); 
    list_remove(&spte->bp_elem);
    lock_release(&bp->lock);

    bool should_free_bp = false;
    if (bp->kind == BP_KIND_FILE && bp->shared) {
        lock_acquire(&shared_file_bps.lock);
        bp->file_ref_cnt--;
        if (bp->file_ref_cnt == 0) {
            hash_delete(&shared_file_bps.file_bps, &bp->cache_elem);
            should_free_bp = true;
        }
        lock_release(&shared_file_bps.lock);
    }
    else {
        bp->file_ref_cnt--;
        if (bp->file_ref_cnt == 0) {
            should_free_bp = true;
        }
    }

    if (should_free_bp) {
        bp_free_up(bp);
    }
}

static unsigned 
file_source_hash_func(const struct hash_elem *e, void *aux UNUSED) {
    const struct backing_page *bp = hash_entry(e, struct backing_page, cache_elem);
    return  hash_bytes(&bp->origin_info.file, sizeof(bp->origin_info.file));
}

static bool 
file_source_less_func(const struct hash_elem *a, const struct hash_elem *b, 
                    void *aux UNUSED) {
    const struct backing_page *bp_a = hash_entry(a, struct backing_page, cache_elem);
    const struct backing_page *bp_b = hash_entry(b, struct backing_page, cache_elem);
    const struct file_info *fi_a = &bp_a->origin_info.file;
    const struct file_info *fi_b = &bp_b->origin_info.file;
    if (fi_a->inode_ptr != fi_b->inode_ptr) {
        return fi_a->inode_ptr < fi_b->inode_ptr;
    }
    if (fi_a->offset != fi_b->offset) {
        return fi_a->offset < fi_b->offset;
    }
    if (fi_a->read_bytes != fi_b->read_bytes) {
        return fi_a->read_bytes < fi_b->read_bytes;
    }
    return fi_a->zero_bytes < fi_b->zero_bytes;
}

static bool
bp_file_page_in (struct inode *inode_ptr, off_t offset, void *kpage, off_t read_bytes, off_t zero_bytes) {
    off_t bytes = inode_read_at(inode_ptr, kpage, read_bytes, offset);
    memset(kpage + read_bytes, 0, zero_bytes);
    return bytes == read_bytes;
}

static bool
bp_file_page_out(struct inode *inode_ptr, off_t offset, void *kpage, off_t written_bytes) {
    off_t bytes = inode_write_at(inode_ptr, kpage, written_bytes, offset);
    return bytes == written_bytes;
}

static bool
bp_swap_page_in(swap_slot_t slot, void *kpage) {
    swap_read_page(slot, kpage);
    swap_free_slot(slot);
    return true;
}

static bool
bp_swap_page_out(swap_slot_t slot, void *kpage) {
    swap_write_page(slot, kpage);
    return true;
}

static bool
bp_swap_page_destroy(swap_slot_t slot) {
    swap_free_slot(slot);
    return true;
}

static bool
bp_zero_page_in(void *kpage) {
    memset(kpage, 0, PGSIZE);
    return true;
}

static bool
bp_page_in_locked(struct backing_page *bp, struct frame *f) {
    // page in 只是把page装进来，判断要不要装不是它做的事情
    ASSERT(lock_held_by_current_thread(&bp->lock));
    ASSERT(bp->busy); 
    
    bool success = false;
    if (bp->loc == BP_LOC_FILE) {
        const struct file_info *fi = &bp->origin_info.file;
        lock_release(&bp->lock);
        success = bp_file_page_in(fi->inode_ptr, fi->offset, f->kpage, 
                        fi->read_bytes, fi->zero_bytes);
        lock_acquire(&bp->lock);
        if (success && !bp->shared) {
            // 私有file页，加载后变为anon，不写回也不参与共享
            bp->kind = BP_KIND_ANON;
        }
    }
    else if (bp->loc == BP_LOC_SWAP) {
        swap_slot_t slot = bp->slot;
        lock_release(&bp->lock);
        success = bp_swap_page_in(slot, f->kpage);
        lock_acquire(&bp->lock);
        if (success) {
            bp->slot = SLOT_ERROR;
        }
    }
    else if (bp->loc == BP_LOC_ZERO) {
        lock_release(&bp->lock);
        success = bp_zero_page_in(f->kpage);
        lock_acquire(&bp->lock);
    }
    else if (bp->loc == BP_LOC_FRAME) {
        PANIC("bp_page_in should not be called when bp is already resident");
    }
    else {
        PANIC("Invalid bp location in bp_page_in: %d", bp->loc);
    }

    if (success) {
        bp->frame = f;
        bp->loc = BP_LOC_FRAME;
    }
    return success;
}

static bool
bp_page_out_locked(struct backing_page *bp) {    
    ASSERT(lock_held_by_current_thread(&bp->lock));
    ASSERT(bp->busy); 
    ASSERT(bp->loc == BP_LOC_FRAME);
    
    bool success = false;
    if (bp->kind == BP_KIND_FILE) {
        if (bp_collect_dirty_locked(bp, false)) {
            const struct file_info *fi = &bp->origin_info.file;
            void *kpage = bp->frame->kpage;
            lock_release(&bp->lock);
            success = bp_file_page_out(fi->inode_ptr, fi->offset, kpage, fi->read_bytes);
            lock_acquire(&bp->lock);
        }
        else {
            success = true; // 没有被修改过，不需要写回，直接当作写回成功了
        }

        if (success) {
            bp->loc = BP_LOC_FILE;
        }
    }
    else if (bp->kind == BP_KIND_ANON) {
        void *kpage = bp->frame->kpage;
        lock_release(&bp->lock);
        swap_slot_t slot = swap_alloc_slot();
        if (slot != SLOT_ERROR) {
            success = bp_swap_page_out(slot, kpage);
        }
        lock_acquire(&bp->lock);
        if (success) {
            bp->slot = slot;
            bp->loc = BP_LOC_SWAP;
        }
    }
    else {
        PANIC("Invalid bp kind in bp_page_out: %d", bp->kind);
    }

    if (success) {
        bp->frame = NULL;
    }
    return success;
}

static bool
bp_page_destroy_locked(struct backing_page *bp) {
    ASSERT(lock_held_by_current_thread(&bp->lock));
    ASSERT(bp->busy); 
    
    bool success = false;
    if (bp->loc == BP_LOC_FRAME) {
        if (bp->kind == BP_KIND_FILE) {
            if (bp_collect_dirty_locked(bp, false)) {
                const struct file_info *fi = &bp->origin_info.file;
                void *kpage = bp->frame->kpage;
                lock_release(&bp->lock);
                bool success = bp_file_page_out(fi->inode_ptr, fi->offset, kpage, fi->read_bytes);
                lock_acquire(&bp->lock);
                if (!success) {
                    PANIC("Failed to write back dirty page to file during bp_page_destroy_locked. \n"
                        "Written bytes may be less than expected");
                }
            }
            else {
                success = true;
            }
        }
        else if (bp->kind == BP_KIND_ANON) {
            // do nothing
            success = true;
        }
        else {
            PANIC("Invalid bp kind in bp_page_destroy_locked: %d", bp->kind);
        }
        lock_release(&bp->lock);
        frame_free(bp->frame);
        lock_acquire(&bp->lock);
        bp->frame = NULL;
    }
    else if (bp->loc == BP_LOC_SWAP) {
        swap_slot_t slot = bp->slot;
        lock_release(&bp->lock);
        success = bp_swap_page_destroy(slot);
        lock_acquire(&bp->lock);
        if (success) {
            bp->slot = SLOT_ERROR;
        }
        else {
            PANIC("Failed to free swap slot during bp_page_destroy_locked. \n"
                "This may cause swap space leak.");
        }
    }
    else if (bp->loc == BP_LOC_ZERO || bp->loc == BP_LOC_FILE) {
        // 没有加载到内存，不需要做任何事
        return true;
    }
    else {
        PANIC("Invalid bp location in bp_page_destroy_locked: %d", bp->loc);
    }
    // 不可能写回失败，失败必须panic
    return success;
}

static bool 
bp_load_locked(struct backing_page *bp) {
    ASSERT(lock_held_by_current_thread(&bp->lock));

    while (bp->busy) {
        cond_wait(&bp->busy_cond, &bp->lock);
    }

    if (bp->loc == BP_LOC_FRAME) {
        // 已经加载过
        return true;
    }
    else if (bp->loc == BP_LOC_FILE || bp->loc == BP_LOC_SWAP || bp->loc == BP_LOC_ZERO) {
        // 没有线程在加载，自己来加载
        bp->busy = true;
        lock_release(&bp->lock);
        
        bool success = false;
        struct frame* f = frame_alloc();
        if (f != NULL) {
            lock_acquire(&bp->lock);
            success = bp_page_in_locked(bp, f);
            lock_release(&bp->lock);
            if (success) {
                frame_register(f, bp); // 双向引用
                frame_unpin(f); //刚分配的frame是pin住的，装载完就可以unpin了
            }
            else {
                frame_free(f);
            }
        }
        lock_acquire(&bp->lock);
        bp->busy = false;
        cond_broadcast(&bp->busy_cond, &bp->lock);
        return success;
    }
    else {
        PANIC("Invalid bp location in bp_load_locked: %d", bp->loc);
    }
}

// 返回成功说明bp已经resident了
bool
bp_claim_page(struct backing_page *bp, struct spte *spte) {

    lock_acquire(&bp->lock);
    // 先查页表
    bool success = false;
    if (pmap_check_spte_mmaped(spte)) {
        success = true;
        goto DONE;
    } 

    if (!bp_load_locked(bp)) {
        success = false;
        goto DONE;
    }
        
    if (!pmap_install_spte_page(spte, bp->frame->kpage)) {
        success = false;
        goto DONE;
    }
    // 分配页表也可能失败，所以要判断
    // 失败后，即使bp已经装载，也不用回滚，此时页表与bp状态相当于spt未访问
    success = true;

DONE:
    lock_release(&bp->lock);
    return success;
}

// 只有frame会调用这个函数
// frame保证不会同时有多个线程逐出bp，frame释放锁的时候，frame结构体已经移出全局frame表了，所以不会有新的线程拿到这个frame来逐出同一个bp了
// 也不会加载和逐出同时发生，因为frame分配的页会先pin住，等装载完再unpin，不会装载的时候被逐出
bool 
bp_try_evict_locked(struct backing_page *bp) {
    ASSERT(lock_held_by_current_thread(&bp->lock));
    ASSERT(bp->loc == BP_LOC_FRAME); // 只有加载完成的页才会被逐出
    ASSERT(!bp->busy); // 逐出前应该没有线程在加载这个页了
    bp->busy = true;
    bool success = bp_page_out_locked(bp);
    if (success) {
        bp_clear_pages_locked(bp); // 同步页表，同时也清除页表的A和D位
        bp->accessed = bp->dirty = false; // 重置访问和修改状态
    }
    bp->busy = false;
    cond_broadcast(&bp->busy_cond, &bp->lock);
    return success;
}

static bool
bp_free_up(struct backing_page *bp) {
    lock_acquire(&bp->lock);
    while (bp->busy) {
        cond_wait(&bp->busy_cond, &bp->lock);
    }
    bp->busy = true; // 防止frame操作
    bool success = bp_page_destroy_locked(bp);
    bp->busy = false;
    cond_broadcast(&bp->busy_cond, &bp->lock); // 感觉没必要
    lock_release(&bp->lock);
    free(bp);
    return success;
}

/* ----------- bp访问sharers的页表 ----------- */
bool 
bp_collect_accessed_locked(struct backing_page *bp, bool clear) {
    struct list_elem *e;
    bool accessed = bp->accessed;
     for (e = list_begin(&bp->sharers); e != list_end(&bp->sharers); e = list_next(e)) {
        struct spte *spte = list_entry(e, struct spte, bp_elem);
        accessed |= pmap_test_spte_accessed(spte, clear);
    }
    bp->accessed = clear ? false : accessed; 
    return accessed;
}

bool 
bp_collect_dirty_locked(struct backing_page *bp, bool clear) {
    struct list_elem *e;
    bool dirty = bp->dirty;
    for (e = list_begin(&bp->sharers); e != list_end(&bp->sharers); e = list_next(e)) {
        struct spte *spte = list_entry(e, struct spte, bp_elem);
        dirty |= pmap_test_spte_dirty(spte, clear); 
    }
    bp->dirty = clear ? false : dirty;
    return dirty;
}

static void 
bp_clear_pages_locked(struct backing_page *bp) {
    struct list_elem *e;
    for (e = list_begin(&bp->sharers); e != list_end(&bp->sharers); e = list_next(e)) {
        struct spte *spte = list_entry(e, struct spte, bp_elem);
        pmap_clear_spte_page(spte);
    }
}