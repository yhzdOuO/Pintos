#include <debug.h>
#include <round.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/mmap.h"
#include "vm/page.h"
#include "filesys/file.h"
#include "filesys/inode.h"

static void mmap_entry_destroy(struct mmap_entry *entry, struct spt *spt);
static struct mmap_entry *mmap_table_lookup(struct mmap_table *mmap_table, mapid_t mapid);

void 
mmap_table_init(struct mmap_table *mmap_table, struct spt *spt) {
    list_init(&mmap_table->mappings);
    mmap_table->next_mapid = 0;
    mmap_table->spt = spt;
}

void 
mmap_table_destroy(struct mmap_table *mmap_table) {
    struct list_elem *e;
    while (!list_empty(&mmap_table->mappings)) {
        e = list_pop_front(&mmap_table->mappings);
        struct mmap_entry *entry = list_entry(e, struct mmap_entry, elem);
        mmap_entry_destroy(entry, mmap_table->spt);
    }
}

mapid_t 
mmap_table_map(struct mmap_table *mmap_table, struct file *file, void *addr) {
  if ( file == NULL || addr == NULL || pg_ofs(addr) != 0) {
    return -1;
  }

  if (!inode_is_writable(file_get_inode(file))) {
    return -1;
  }

  off_t file_size = file_length(file);
  if (file_size == 0) {
    return -1;
  }

  uintptr_t start = (uintptr_t) addr;
  uintptr_t end = start + (uintptr_t) file_size;
  if (end < start || end > (uintptr_t) PHYS_BASE) {
    return -1;
  }

  file = file_reopen(file);
  if (file == NULL) {
    return -1;
  }

  struct mmap_entry *entry = malloc (sizeof *entry);
  if (entry == NULL) {
    file_close(file);
    return -1;
  }
  entry->mapid = -1;
  entry->page_cnt = 0;
  entry->addr = addr;
  entry->file = file;
  entry->length = file_size;
  // list_init (&entry->spte_list);

  bool success = true;
  int page_cnt = DIV_ROUND_UP (file_size, PGSIZE);
  char *upage = addr;
  off_t offset = 0;
  uint32_t read_bytes = file_size;
  for (int i = 0; i < page_cnt; i++) {
    
    if ((void*)upage >= PHYS_BASE || spt_has_page(mmap_table->spt, (void *) upage)) {
      success = false;
      break;
    }

    size_t page_read_bytes = read_bytes >= PGSIZE ? PGSIZE : read_bytes;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;
    
    struct spte_desc desc = {
      .upage = upage,
      .writable = true,
      //.mmap = entry,
      .kind = SPTE_FILE,
      .sharing = SPTE_SHARED,
      .file = file,
      .offset = offset,
      .read_bytes = page_read_bytes,
      .zero_bytes = page_zero_bytes,
    };
    if (!spt_insert_spte(mmap_table->spt, &desc)) {
      success = false;
      break;
    }
    entry->page_cnt++;
    upage += PGSIZE;
    offset += page_read_bytes;
    read_bytes -= page_read_bytes;
  }

  if (!success) {
    mmap_entry_destroy(entry, mmap_table->spt);
    return -1;
  }
  
  entry->mapid = mmap_table->next_mapid++;
  list_push_back(&mmap_table->mappings, &entry->elem);
  return entry->mapid;
}

bool 
mmap_table_unmap(struct mmap_table *mmap_table, mapid_t mapid) {
    struct mmap_entry *entry = mmap_table_lookup(mmap_table, mapid);
    if (entry == NULL) {
        return false;
    }

    list_remove(&entry->elem);
    mmap_entry_destroy(entry, mmap_table->spt);
    return true;
}

static struct mmap_entry *
mmap_table_lookup(struct mmap_table *mmap_table, mapid_t mapid) {
    struct list_elem *e;
    for (e = list_begin(&mmap_table->mappings); e != list_end(&mmap_table->mappings); 
        e = list_next(e)) {
        struct mmap_entry *entry = list_entry(e, struct mmap_entry, elem);
        if (entry->mapid == mapid) {
            return entry;
        }
    }
    return NULL;
}

static void 
mmap_entry_destroy(struct mmap_entry *entry, struct spt *spt) {
    uintptr_t start = (uintptr_t) entry->addr;
    uintptr_t end = start + (uintptr_t) entry->length;
    size_t page_cnt = entry->page_cnt;
    for (uintptr_t upage = start; upage < end && page_cnt > 0; 
          upage += PGSIZE, page_cnt--) {
        spt_remove_spte(spt, (void *) upage);
    }

    file_close(entry->file);
    
    free (entry);
}

