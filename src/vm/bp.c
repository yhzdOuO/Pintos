
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
static bool bp_page_in(struct backing_page *bp, struct frame *f);
static void bp_write_back_locked(struct backing_page *bp);

static bool bp_load_locked(struct backing_page *bp);
static bool bp_claim_locked(struct backing_page *bp, struct spte *spte);

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
        lock_acquire(&bp->lock);
        bp_write_back_locked(bp);
        lock_release(&bp->lock);
        free(bp);
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
bp_page_in(struct backing_page *bp, struct frame *f) {
    lock_acquire(&bp->lock);
    if (bp->loc == BP_LOC_FILE) {
        lock_release(&bp->lock);
        const struct file_info *fi = &bp->origin_info.file;
        off_t read_byte = inode_read_at(fi->inode_ptr, f->kpage, 
                                    fi->read_bytes, fi->offset);
        memset(f->kpage + fi->read_bytes, 0, fi->zero_bytes);
        return read_byte == fi->read_bytes;
    }
    else if (bp->loc == BP_LOC_SWAP) {
        swap_slot_t slot = bp->slot;
        lock_release(&bp->lock);
        swap_read_page(slot, f->kpage);
        swap_free_slot(slot);
        return true;
    }
    else if (bp->loc == BP_LOC_ZERO) {
        lock_release(&bp->lock);
        memset(f->kpage, 0, PGSIZE);
        return true;
    }
    else {
        lock_release(&bp->lock);
        PANIC("Invalid bp location in bp_page_in: %d", bp->loc);
    }
}

static void 
bp_write_back_locked(struct backing_page *bp) {
    while(bp->busy) {
        cond_wait(&bp->busy_cond, &bp->lock);
    }
    bp->busy = true; // 保持busy直到free，防止frame的访问（frame不访问busy状态的bp）
    
    if (bp->loc == BP_LOC_FRAME) {
        if (bp->kind == BP_KIND_FILE) {
            if (bp_collect_dirty_locked(bp, false)) {
                const struct file_info *fi = &bp->origin_info.file;
                void *kpage = bp->frame->kpage;
                lock_release(&bp->lock);
                off_t written_bytes = inode_write_at(fi->inode_ptr, kpage, 
                                                    fi->read_bytes, fi->offset);
                if (written_bytes != fi->read_bytes) {
                    PANIC("Failed to write back dirty page to file during bp_free. \n"
                        "Written bytes: %d, expected: %d", written_bytes, fi->read_bytes);
                }
                lock_acquire(&bp->lock);
            }
        }
        else if (bp->kind == BP_KIND_ANON) {
            // 不做任何事
        }
        lock_release(&bp->lock);
        frame_free(bp->frame);
        lock_acquire(&bp->lock);
        bp->frame = NULL;
    }
    else if (bp->loc == BP_LOC_SWAP) {
        lock_release(&bp->lock);
        swap_free_slot(bp->slot);
        lock_acquire(&bp->lock);
        bp->slot = SLOT_ERROR;
    }
    else if (bp->loc == BP_LOC_ZERO || bp->loc == BP_LOC_FILE) {
        // 没有加载到内存，不需要做任何事
    }
}

// 只有frame会调用这个函数
// frame保证不会同时有多个线程逐出bp，frame释放锁的时候，frame结构体已经移出全局frame表了，所以不会有新的线程拿到这个frame来逐出同一个bp了
// 也不会加载和逐出同时发生，因为frame分配的页会先pin住，等装载完再unpin，不会装载的时候被逐出
bool 
bp_evict_locked(struct backing_page *bp) {
    ASSERT(bp->loc == BP_LOC_FRAME); // 只有加载完成的页才会被逐出
    ASSERT(!bp->busy); // 逐出前应该没有线程在加载这个页了
    bp->busy = true;

    bool success = false;
    // start evicting the page
    if (bp->kind == BP_KIND_FILE) {
        if (!bp_collect_dirty_locked(bp, false)) {
            success = true; // 没有被修改过，不需要写回，直接当作写回成功了
        }
        else {
            void *kpage = bp->frame->kpage;
            lock_release(&bp->lock);
            const struct file_info *fi = &bp->origin_info.file;
            off_t written_bytes = inode_write_at(fi->inode_ptr, kpage, 
                                                fi->read_bytes, fi->offset);
            success = written_bytes == fi->read_bytes;
            lock_acquire(&bp->lock);
        }
        if (success) {
            bp->loc = BP_LOC_FILE;
            // 不用清理，因为全部重置了
        }
    }
    else if (bp->kind == BP_KIND_ANON) {
        lock_release(&bp->lock);
        swap_slot_t slot = swap_alloc_slot();
        success = slot != SLOT_ERROR;
        if (success) {
            swap_write_page(slot, bp->frame->kpage);
        }
        lock_acquire(&bp->lock);
        if (success) {
            bp->slot = slot;
            bp->loc = BP_LOC_SWAP;
        }
    }
    else {
        PANIC("Invalid bp kind in bp_evict: %d", bp->kind);
    }
    // evicting done, update bp status

    if (success) {
        bp->frame = NULL;
        bp_clear_pages_locked(bp); // 同步页表，同时也清除页表的A和D位
        bp->accessed = bp->dirty = false; // 重置访问和修改状态
    }
    bp->busy = false;
    cond_broadcast(&bp->busy_cond, &bp->lock);
    return success;
}

static bool 
bp_load_locked(struct backing_page *bp) {
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
        
        struct frame* f = frame_alloc();
        if (f == NULL) {
            goto fail1;
        }
        if (!bp_page_in(bp, f)) {
            goto fail2;
        }
        frame_register(f, bp); // 双向引用
        frame_unpin(f); // 刚分配的frame是pin住的，装载完就可以unpin了

        lock_acquire(&bp->lock);
        bp->frame = f;
        bp->loc = BP_LOC_FRAME;
        bp->busy = false;
        cond_broadcast(&bp->busy_cond, &bp->lock);
        return true;
        
    fail2:
        frame_free(f);
        
    fail1:
        lock_acquire(&bp->lock);
        // bp->loc = BP_LOC_FAILED; 加载失败loc不变
        bp->busy = false;
        cond_broadcast(&bp->busy_cond, &bp->lock);
        return false;
    }
    else {
        PANIC("Invalid bp location in bp_load_locked: %d", bp->loc);
    }
}

// 返回成功说明bp已经resident了
bool
bp_claim_locked(struct backing_page *bp, struct spte *spte) {

    if (pmap_check_spte_mmaped(spte)) {
        return true;
    } 

    if (!bp_load_locked(bp)) {
        return false;
    }
        
    if (!pmap_install_spte_page(spte, bp->frame->kpage)) {
        return false;
    }
    // 分配页表也可能失败，所以要判断
    // 失败后，即使bp已经装载，也不用回滚，此时页表与bp状态相当于spt未访问

    return true;
}

bool bp_claim_page(struct backing_page *bp, struct spte *spte) {
    lock_acquire(&bp->lock);
    bool success = bp_claim_locked(bp, spte);
    lock_release(&bp->lock);
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