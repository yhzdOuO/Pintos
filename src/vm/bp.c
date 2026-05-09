
#include <string.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/bp.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
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

static uint32_t *bp_get_spte_pagedir(struct spte *spte);
static bool bp_test_spte_accessed(struct spte *spte, bool clear);
static bool bp_test_spte_dirty(struct spte *spte, bool clear);
static void bp_clear_spte_page(struct spte *spte);
static void bp_install_spte_page(struct spte *spte, void *kpage);

// 实际上private+COW也算是一种共享

void 
backing_page_init(void) {
    lock_init(&shared_file_bps.lock);
    hash_init(&shared_file_bps.file_bps, file_source_hash_func, file_source_less_func, NULL);
}

static void
bp_init_file(struct backing_page *bp, struct spte_desc *desc) {
    if (desc->sharing == SPTE_SHARED) {
        bp->sharing = BP_SHARED;
        bp->origin = BP_ORIGIN_FILE;
        bp->store = BP_STORE_FILE;
    }
    else if (desc->sharing == SPTE_PRIVATE) {
        bp->sharing = BP_PRIVATE;
        bp->origin = BP_ORIGIN_FILE;
        bp->store = BP_STORE_SWAP; // private file page 需要被换出到
    }
    bp->origin_info.file.inode_ptr = file_get_inode(desc->file);
    bp->origin_info.file.offset = desc->offset;
    bp->origin_info.file.read_bytes = desc->read_bytes;
    bp->origin_info.file.zero_bytes = desc->zero_bytes;

    bp->accessed = bp->dirty = false;
    lock_init(&bp->lock);
    list_init(&bp->sharers);
    bp->busy = BP_NOT_BUSY;
    cond_init(&bp->busy_cond);
    bp->status = BP_NOT_LOADED;
    bp->frame = NULL;
    bp->slot = SLOT_ERROR;
    bp->file_ref_cnt = 1;
}

static void 
bp_init_zero(struct backing_page *bp, struct spte_desc *desc) {
    bp->origin = BP_ORIGIN_ANON;
    bp->store = BP_STORE_SWAP; // anon page 需要被换出到swap
    if (desc->sharing == SPTE_SHARED) {
        bp->sharing = BP_SHARED;
    }
    else if (desc->sharing == SPTE_PRIVATE) {
        bp->sharing = BP_PRIVATE;
    }
    bp->accessed = bp->dirty = false;
    lock_init(&bp->lock);
    list_init(&bp->sharers);
    bp->busy = BP_NOT_BUSY;
    cond_init(&bp->busy_cond);
    bp->status = BP_NOT_LOADED;
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

    // 支持COW以后，无论是否标记为SHARED都可以共享
    if (bp->sharing == BP_SHARED) {
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
    bp->accessed |= bp_test_spte_accessed(spte, false);
    bp->dirty |= bp_test_spte_dirty(spte, false);
    bp_clear_spte_page(spte); 
    list_remove(&spte->bp_elem);
    lock_release(&bp->lock);

    bool should_free_bp = false;
    if (bp->origin == BP_ORIGIN_FILE && bp->sharing == BP_SHARED) {
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
    if ((bp->status == BP_NOT_LOADED && bp->origin == BP_ORIGIN_FILE)
        || (bp->status == BP_EVICTED && bp->store == BP_STORE_FILE)) {
        lock_release(&bp->lock);
        const struct file_info *fi = &bp->origin_info.file;
        off_t read_byte = inode_read_at(fi->inode_ptr, f->kpage, 
                                    fi->read_bytes, fi->offset);
        memset(f->kpage + fi->read_bytes, 0, fi->zero_bytes);
        return read_byte == fi->read_bytes;
    }
    else if (bp->status == BP_EVICTED && bp->store == BP_STORE_SWAP) {
        swap_slot_t slot = bp->slot;
        lock_release(&bp->lock);
        swap_read_page(slot, f->kpage);
        swap_free_slot(slot);
        return true;
    }
    else if (bp->status == BP_NOT_LOADED && bp->origin == BP_ORIGIN_ANON) {
        lock_release(&bp->lock);
        memset(f->kpage, 0, PGSIZE);
        return true;
    }
    else {
        lock_release(&bp->lock);
        PANIC("Invalid bp status or origin in bp_page_in. Status: %d, Origin: %d", bp->status, bp->origin);
    }
}

static void 
bp_write_back_locked(struct backing_page *bp) {
    while(bp->busy == BP_BUSY) {
        cond_wait(&bp->busy_cond, &bp->lock);
    }
    bp->busy = BP_BUSY; // 保持busy直到free，防止frame的访问（frame不访问busy状态的bp）
    if (bp->status == BP_LOADED) {
        if (bp->store == BP_STORE_FILE) {
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
        lock_release(&bp->lock);
        frame_free(bp->frame);
        lock_acquire(&bp->lock);
        bp->frame = NULL;
    }
    else if (bp->status == BP_EVICTED) {
        if (bp->store == BP_STORE_SWAP) {
            lock_release(&bp->lock);
            swap_free_slot(bp->slot);
            lock_acquire(&bp->lock);
            bp->slot = SLOT_ERROR;
        }
    }
    else if (bp->status == BP_NOT_LOADED) {
        // 没有加载到内存过，不需要写回
    }
}

// 只有frame会调用这个函数
// frame保证不会同时有多个线程逐出bp，frame释放锁的时候，frame结构体已经移出全局frame表了，所以不会有新的线程拿到这个frame来逐出同一个bp了
// 也不会加载和逐出同时发生，因为frame分配的页会先pin住，等装载完再unpin，不会装载的时候被逐出
bool 
bp_evict_locked(struct backing_page *bp) {
    ASSERT(bp->status == BP_LOADED); // 只有加载完成的页才会被逐出
    ASSERT(bp->busy == BP_NOT_BUSY); // 逐出前应该没有线程在加载这个页了
    bp->busy = BP_BUSY;

    bool success = false;
    // start evicting the page
    if (bp->store == BP_STORE_FILE) {
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
    }
    else if (bp->store == BP_STORE_SWAP) {
        lock_release(&bp->lock);
        swap_slot_t slot = swap_alloc_slot();
        success = slot != SLOT_ERROR;
        if (success) {
            swap_write_page(slot, bp->frame->kpage);
        }
        lock_acquire(&bp->lock);
        if (success) {
            bp->slot = slot;
        }
    }
    else {
        PANIC("Invalid bp store type in bp_evict: %d", bp->store);
    }
    // evicting done, update bp status

    if (success) {
        bp->status = BP_EVICTED;
        bp->frame = NULL;
        bp_clear_pages_locked(bp); // 同步页表
    }
    bp->busy = BP_NOT_BUSY;
    cond_broadcast(&bp->busy_cond, &bp->lock);
    return success;
}

static bool 
bp_load_locked(struct backing_page *bp) {
    while (bp->busy == BP_BUSY) {
        cond_wait(&bp->busy_cond, &bp->lock);
    }

    if (bp->status == BP_FAILED) {
        // 之前加载过了，但加载失败了，说明这个页是不可用的了
        return false;
    }
    else if (bp->status == BP_LOADED) {
        // 已经加载过
        return true;
    }
    else if (bp->status == BP_NOT_LOADED || bp->status == BP_EVICTED) {
        // 没有线程在加载，自己来加载
        bp->busy = BP_BUSY;
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
        bp->status = BP_LOADED;
        bp->busy = BP_NOT_BUSY;
        cond_broadcast(&bp->busy_cond, &bp->lock);
        return true;
        
    fail2:
        frame_free(f);
        
    fail1:
        lock_acquire(&bp->lock);
        bp->busy = BP_NOT_BUSY;
        bp->status = BP_FAILED;
        cond_broadcast(&bp->busy_cond, &bp->lock);
        return false;
    }
    else {
        PANIC("Invalid bp status in bp_load_locked: %d", bp->status);
    }
}

// 返回成功说明bp已经resident了
bool
bp_claim_locked(struct backing_page *bp, struct spte *spte) {
    uint32_t *pagedir = bp_get_spte_pagedir(spte);

    if (pagedir_get_page(pagedir, spte->upage) != NULL) {
        return true;
    } 

    if (!bp_load_locked(bp)) {
        return false;
    }
        
    bp_install_spte_page(spte, bp->frame->kpage);// 这里可能可以直接调用pagedir_set?
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
        accessed |= bp_test_spte_accessed(spte, clear);
    }
    bp->accessed = clear ? false : bp->accessed; 
    return accessed;
}

bool 
bp_collect_dirty_locked(struct backing_page *bp, bool clear) {
    struct list_elem *e;
    bool dirty = bp->dirty;
    for (e = list_begin(&bp->sharers); e != list_end(&bp->sharers); e = list_next(e)) {
        struct spte *spte = list_entry(e, struct spte, bp_elem);
        dirty |= bp_test_spte_dirty(spte, clear); 
    }
    bp->dirty = clear ? false : bp->dirty;
    return dirty;
}

static void 
bp_clear_pages_locked(struct backing_page *bp) {
    struct list_elem *e;
    for (e = list_begin(&bp->sharers); e != list_end(&bp->sharers); e = list_next(e)) {
        struct spte *spte = list_entry(e, struct spte, bp_elem);
        bp_clear_spte_page(spte);
    }
}

/* ----------- bp访问spte页表 ----------- */
// 页表存在项要和bp->status同步
// used by bp
static uint32_t *
bp_get_spte_pagedir(struct spte *spte) {
    // 只有插入spt中的spte才能和bp绑定
    // 绑定意味着bp和pagedir绑定
    ASSERT(spte->owner != NULL);
    struct spt *owner = spte->owner;
    ASSERT(owner->owner != NULL);
    struct thread *owner_thread = owner->owner;
    return owner_thread->pagedir;
}

static bool
bp_test_spte_accessed(struct spte *spte, bool clear) {
    uint32_t *pagedir = bp_get_spte_pagedir(spte);
    bool accessed = pagedir_is_accessed(pagedir, spte->upage);
    if (clear) {
        pagedir_set_accessed(pagedir, spte->upage, false);
    }
    return accessed;
}

static bool
bp_test_spte_dirty(struct spte *spte, bool clear) {
    uint32_t *pagedir = bp_get_spte_pagedir(spte);
    bool dirty = pagedir_is_dirty(pagedir, spte->upage);
    if (clear) {
        pagedir_set_dirty(pagedir, spte->upage, false);
    }
    return dirty;
}

static void
bp_clear_spte_page(struct spte *spte) {
    uint32_t *pagedir = bp_get_spte_pagedir(spte);
    pagedir_clear_page(pagedir, spte->upage);
}

static void
bp_install_spte_page(struct spte *spte, void *kpage) {
    uint32_t *pagedir = bp_get_spte_pagedir(spte);
    pagedir_set_page(pagedir, spte->upage, kpage, spte->writable);
}