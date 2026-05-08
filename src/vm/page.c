#include <debug.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/bp.h"

/******************** spt functions ********************/

static struct spte *spt_get_spte(struct spt *spt, void *upage);
static unsigned spte_hash_func(const struct hash_elem *e, void *aux UNUSED);
static bool spte_less_func(const struct hash_elem *a, 
                           const struct hash_elem *b, 
                           void *aux UNUSED);
static void spte_delete_action(struct hash_elem *e, void *aux UNUSED);

void 
spt_init(struct spt* spt, struct thread *owner) {
    lock_init(&spt->lock);
    hash_init(&spt->sptes, spte_hash_func, spte_less_func, NULL); 
    spt->owner = owner;
}

bool 
spt_insert_spte(struct spt *spt, struct spte_desc *desc) {
    struct spte *spte = malloc(sizeof(struct spte));
    if (spte == NULL) {
        return false;
    }
    spte->upage = desc->upage;
    spte->writable = desc->writable;
    spte->cow = desc->writable 
                && desc->kind == SPTE_FILE 
                && desc->sharing == SPTE_PRIVATE;
    spte->owner = NULL;
    spte->bp = NULL;
    
    struct hash_elem *e = hash_insert(&spt->sptes, &spte->spt_elem);
    if (e != NULL) {
        free(spte);
        return false;
    }
    spte->owner = spt;

    struct backing_page *bp = NULL;
    if (desc->kind == SPTE_FILE) {
        bp = bp_get_file(desc);
    }
    else if (desc->kind == SPTE_ANON) {
        bp = bp_get_anon(desc);
    }

    if (bp == NULL) {
        hash_delete(&spt->sptes, &spte->spt_elem);
        spte->owner = NULL;
        free(spte);
        return false;
    }
    
    spte->bp = bp;
    bp_attach(bp, spte);
    return true;
}

static struct spte *
spt_get_spte(struct spt *spt, void* upage) {
    struct spte spte_key;
    spte_key.upage = upage;
    struct hash_elem *e = hash_find(&spt->sptes, &spte_key.spt_elem);
    if (e == NULL) {
        return NULL;
    }
    return hash_entry(e, struct spte, spt_elem);
}

void 
spt_remove_spte(struct spt *spt, void *upage) {
    struct spte *spte = spt_get_spte(spt, upage);
    if (spte == NULL) {
        return;
    }

    struct hash_elem *e = hash_delete(&spt->sptes, &spte->spt_elem);
    ASSERT(e != NULL);
    
    bp_detach(spte->bp, spte);
    spte->bp = NULL;
    spte->owner = NULL;

    free(spte);
}

bool
spt_has_page(struct spt *spt, void *upage) {
    return spt_get_spte(spt, upage) != NULL;
}

bool 
spt_is_valid_page(struct spt *spt, void *upage, bool writable) {
    struct spte *spte = spt_get_spte(spt, upage);
    if (spte == NULL) {
        return false;
    }
    if (writable && !spte->writable) {
        return false;
    }
    return true;
}

bool 
spt_claim_page(struct spt *spt, void *upage) {
    struct spte *spte = spt_get_spte(spt, upage);
    if (spte == NULL) {
        return false;
    }
    return bp_claim_page(spte->bp, spte);
}

void 
spt_destroy(struct spt* spt) {
    hash_destroy(&spt->sptes, spte_delete_action);
}

static unsigned
spte_hash_func(const struct hash_elem *e, void *aux UNUSED) {
    const struct spte *spte = hash_entry(e, struct spte, spt_elem);
    return hash_bytes(&spte->upage, sizeof(spte->upage));
}

static bool
spte_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
    const struct spte *spte_a = hash_entry(a, struct spte, spt_elem);
    const struct spte *spte_b = hash_entry(b, struct spte, spt_elem);
    return (uintptr_t)spte_a->upage < (uintptr_t)spte_b->upage;
}

static void
spte_delete_action(struct hash_elem *e, void *aux UNUSED) {
    struct spte *spte = hash_entry(e, struct spte, spt_elem);
    bp_detach(spte->bp, spte);
    spte->bp = NULL;
    spte->owner = NULL;
    free(spte);
}