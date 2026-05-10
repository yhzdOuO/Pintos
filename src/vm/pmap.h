#ifndef VM_PMAP_H
#define VM_PMAP_H
#include <stdint.h>

struct spte;

// used by bp
bool pmap_test_spte_accessed(struct spte *spte, bool clear);
bool pmap_test_spte_dirty(struct spte *spte, bool clear);

bool pmap_is_mmaped(struct spte *spte);
void pmap_clear_spte_page(struct spte *spte);
bool pmap_install_spte_page(struct spte *spte, void *kpage);

#endif /* VM_PMAP_H */