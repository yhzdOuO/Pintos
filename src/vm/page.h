#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <hash.h>
#include <list.h>
#include "threads/synch.h"
#include "filesys/file.h"

struct spt {
    struct lock lock;            // 暂时不用
    struct hash sptes;           // key: upage
    struct thread *owner;        // 反向指针，方便调试
};

struct spte {
    void *upage;                  // 用户虚页
    bool writable;                // 该映射是否允许写
    struct backing_page *bp;      // 页内容对象

    //struct mmap_entry *mmap;       // 属于某次 mmap；否则为 NULL
    struct spt *owner;            // 所属地址空间

    struct hash_elem spt_elem;    // in spt->sptes
    struct list_elem bp_elem;     // in bp->sharers
    //struct list_elem mmap_elem;   // in mmap_entry->spte_list
};

enum spte_init_kind {
    SPTE_FILE,
    SPTE_ANON,
};

enum spte_sharing {
    SPTE_PRIVATE,
    SPTE_SHARED,
};

struct spte_desc {
    void *upage;
    bool writable;
    //struct mmap_entry *mmap;

    enum spte_init_kind kind;
    enum spte_sharing sharing;
    
    struct file *file;
    off_t offset;
    size_t read_bytes;
    size_t zero_bytes;
};

// used by spt
void spt_init(struct spt* spt, struct thread *owner);
void spt_destroy(struct spt* spt);

bool spt_insert_spte(struct spt *spt, struct spte_desc *desc);
void spt_remove_spte(struct spt *spt, void *upage);

bool spt_has_page(struct spt *spt, void *upage);
bool spt_is_valid_page(struct spt *spt, void *upage, bool writable);
bool spt_claim_page(struct spt *spt, void *upage);

#endif /* vm/page.h */