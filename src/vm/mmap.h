#ifndef MMAP_H
#define MMAP_H

#include <stddef.h>
#include <list.h>
#include <hash.h>
#include "filesys/off_t.h"

typedef int mapid_t;

struct mmap_table {
    struct list mappings;
    mapid_t next_mapid;
    struct spt *spt;
};

struct mmap_entry {
    mapid_t mapid;
    void *addr;
    struct file *file;
    off_t length;
    size_t page_cnt;
    // struct list spte_list;
    struct list_elem elem;
};

void mmap_table_init(struct mmap_table *table, struct spt *spt);
void mmap_table_destroy(struct mmap_table *table);

mapid_t mmap_table_map(struct mmap_table *table, struct file *file, void *addr);
bool mmap_table_unmap(struct mmap_table *table, mapid_t mapid);

//struct mmap_entry *mmap_table_lookup(struct mmap_table *table, mapid_t mapid);


#endif /* vm/mmap.h */