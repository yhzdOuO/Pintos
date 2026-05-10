#include <debug.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "vm/bp.h"
#include "vm/frame.h"

struct frame_table {
    size_t count;
    struct list frames;
    struct list_elem *clock_hand;
    struct lock lock;
};

static struct frame_table global;

static struct frame *frame_evict(void);
static struct list_elem *clock_next_locked(struct list_elem *e);
static void clock_insert_locked(struct list_elem *e);
static void table_remove_locked(struct list_elem *e);
static bool clock_try_evict(struct frame *f, bool clock_first_time);
static struct frame *frame_evict(void);

void 
frame_init(void) {
    global.count = 0;
    list_init(&global.frames);
    global.clock_hand = NULL;
    lock_init(&global.lock);
}

// 获得当前clock_hand指向的frame，并把clock_hand移动到下一个位置
static struct list_elem *
clock_next_locked(struct list_elem *e) {
    struct list_elem *next = list_next(e);
    if (next == list_end(&global.frames)) {
        next = list_begin(&global.frames);
    }
    return next;
}

// 在clock_hand前面插入一个frame，这样新分配的页会被clock hand跳过，增加被访问的概率
static void
clock_insert_locked(struct list_elem *e) {
    if (global.clock_hand == NULL) {
        list_push_back(&global.frames, e);
        global.clock_hand = e;
    }
    else {
        list_insert(global.clock_hand, e);
    }
    global.count++;
}

// 从全局table中删除一个frame，并保证clock_hand的正确性
static void
table_remove_locked(struct list_elem *e) {
    ASSERT(e != NULL);
    ASSERT(global.clock_hand != NULL);
    if (global.clock_hand == e) {
        struct list_elem *next = clock_next_locked(e);
        if (next == e) {
            // 只有一个frame了，free掉了就没有frame了，clock hand也就没有了
            global.clock_hand = NULL;
        }
        else {
            global.clock_hand = next;
        }
    }
    list_remove(e);
    global.count--;
}

struct frame *
frame_alloc(void) {
    struct frame* f = malloc(sizeof(struct frame));
    if (f == NULL) {
        return NULL;
    }
    void* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL) {
        free(f);
        f = frame_evict();
        // pinned清零的工作这里做比较好，而不是frame_evict
        if (f == NULL) {
            return NULL;
        }
        return f;
    }
    else {
        f->kpage = kpage;
        f->pinned = 1; // 先pin住，等装载完再unpin，防止函数返回后，但在使用前被换出，需要调用者unpin
        lock_acquire(&global.lock);
        clock_insert_locked(&f->elem); // 插入到clock hand前面，这样新分配的页会被clock hand跳过，增加被访问的概率
        lock_release(&global.lock);
        return f;
    }
}
// 缺页中断中获取frame的全局锁可能被阻塞！！！
// 修改pinned，正在处理的不能换出！！！！

void 
frame_free(struct frame *f) {
    // 最后一个spte解除对bp的引用时，调用free_frame，此时frame肯定在内存中
    if (f == NULL) {
        return;
    }
    lock_acquire(&global.lock);

    table_remove_locked(&f->elem);
    palloc_free_page(f->kpage);

    lock_release(&global.lock);
    free(f);
}

void 
frame_pin(struct frame* f) {
    f->pinned++;
}

void 
frame_unpin(struct frame* f) {
    // 这里不需要加锁，因为pinned只在当前线程访问，其他线程只能通过frame_evict访问，
    // frame_evict会跳过pinned的frame
    f->pinned--;
}

void
frame_register(struct frame *f, struct backing_page *bp) {
    ASSERT(f != NULL);
    ASSERT(bp != NULL);
    f->bp = bp;
}

static bool 
clock_try_evict(struct frame *f, bool clock_first_time) {
    struct backing_page *bp = f->bp;
    ASSERT(bp != NULL);
    lock_acquire(&bp->lock);
    if (bp->busy) {
        lock_release(&bp->lock);
        return false;
    }
    if (bp_collect_accessed_locked(bp, true)) {
        lock_release(&bp->lock);
        return false;
    }
    if (clock_first_time && bp_collect_dirty_locked(bp, false)) {
        lock_release(&bp->lock);
        return false; 
    }
    bool success = bp_evict_locked(bp);
    f->bp = NULL;
    lock_release(&bp->lock);
    return success;
}

static struct frame *
frame_evict(void) {
    bool first_time = true;
    struct frame *f = NULL;

    lock_acquire(&global.lock);
    struct list_elem *e = global.clock_hand;
    if (e == NULL) {
        lock_release(&global.lock);
        return NULL;
    }
    while (true) {
        f = list_entry(e, struct frame, elem);
        if (f->pinned == 0) {
            // clock算法
            // 逐出这个frame对应的页，成功了就返回这个frame的kpage，失败了就继续找下一个未被pin的frame来逐出
            if (clock_try_evict(f, first_time)) {
                f->pinned = 1; // 刚被逐出的页是pin住的，等装载完再unpin了，防止函数返回后，但在使用前被换出，需要调用者unpin
                break;
            }
        }
        f = NULL; // 没有找到可逐出的页了
        e = clock_next_locked(e);
        if (e == global.clock_hand) {
            if (first_time) {
                first_time = false;
            }
            else {
                break;
            }
        }
    }
    global.clock_hand = clock_next_locked(e); // 这里不更新 clock hand 了，因为下一次调用 frame_evict 还是从这个位置开始找起，之前的页被访问过了，下一次调用 frame_evict 的时候就不会被跳过了
    lock_release(&global.lock);
    return f;
}
