#ifndef VM_FRAME_H
#define VM_FRAME_H

struct backing_page;
struct frame;

struct frame {
    void *kpage;
    struct backing_page *bp;
    uint16_t pinned;
    struct list_elem elem;       // in global frame table
};

void frame_init(void);
struct frame *frame_alloc(void); // bp里可以得到当前的frame，返回kpage
void frame_free(struct frame *f);
void frame_pin(struct frame *f);
void frame_unpin(struct frame *f);
void frame_register(struct frame *f, struct backing_page *bp);



#endif /* vm/frame.h */