
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/pmap.h"

static uint32_t *pmap_get_spte_pagedir(struct spte *spte);

static uint32_t *
pmap_get_spte_pagedir(struct spte *spte) {
    // 只有插入spt中的spte才能和bp绑定
    // 绑定意味着bp和pagedir绑定
    ASSERT(spte->owner != NULL);
    struct spt *owner = spte->owner;
    ASSERT(owner->owner != NULL);
    struct thread *owner_thread = owner->owner;
    return owner_thread->pagedir;
}


/* ----------- 访问spte页表 ----------- */
// 页表存在项要和bp->status同步

bool
pmap_test_spte_accessed(struct spte *spte, bool clear) {
    uint32_t *pagedir = pmap_get_spte_pagedir(spte);
    bool accessed = pagedir_is_accessed(pagedir, spte->upage);
    if (clear) {
        pagedir_set_accessed(pagedir, spte->upage, false);
    }
    return accessed;
}

bool
pmap_test_spte_dirty(struct spte *spte, bool clear) {
    uint32_t *pagedir = pmap_get_spte_pagedir(spte);
    bool dirty = pagedir_is_dirty(pagedir, spte->upage);
    if (clear) {
        pagedir_set_dirty(pagedir, spte->upage, false);
    }
    return dirty;
}

bool
pmap_is_mmaped(struct spte *spte) {
    uint32_t *pagedir = pmap_get_spte_pagedir(spte);
    return pagedir_get_page(pagedir, spte->upage) != NULL;
}

void
pmap_clear_spte_page(struct spte *spte) {
    uint32_t *pagedir = pmap_get_spte_pagedir(spte);
    pagedir_clear_page(pagedir, spte->upage);
}

bool
pmap_install_spte_page(struct spte *spte, void *kpage) {
    uint32_t *pagedir = pmap_get_spte_pagedir(spte);
    return pagedir_set_page(pagedir, spte->upage, kpage, spte->writable);
}