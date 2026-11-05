/*
- MyOS - src/drivers/block.c
- Lock order: g_block.lock -> dev->lock. No lock is held across a driver call
- or a completion callback. Only the thread that owns dev->pumping removes
- requests from the queue head, which keeps peek-then-pop safe.
 */

#include "kernel.h"
#include "kernel/error.h"
#include "kernel/spinlock.h"
#include "kernel/timeout.h"
#include "mm/heap.h"
#include "mm/dma.h"
#include "kernel/scheduler.h"
#include "drivers/block.h"

#define BLOCK_MAGIC           0xB10CDE70u
#define BLOCK_WAIT_TIMEOUT_MS 60000u

static struct {
    spinlock_t      lock;
    block_device_t *devs[BLOCK_MAX_DEVICES];
    u32             count;
    void          (*wait_hook)(void);
    bool            worker;
} g_block;

void block_init(void) {
    spin_init(&g_block.lock);
    g_block.count = 0;
    kok("Block layer online");
}

void block_set_wait_hook(void (*hook)(void)) {
    g_block.wait_hook = hook;
}

/* =========================================================================
   REGISTRY
   ========================================================================= */

int32_t block_register(block_device_t *dev, const char *prefix) {
    if (dev == NULL || prefix == NULL || dev->ops == NULL || dev->ops->submit == NULL) {
        return -E_INVAL;
    }
    u32 ss = dev->sector_size;
    if (ss < 512 || ss > 4096 || (ss & (ss - 1)) != 0 || dev->sector_count == 0 ||
        dev->max_sectors == 0 || dev->max_sg == 0 || dev->max_sg > BLOCK_MAX_SG) {
        return -E_INVAL;
    }
    if (dev->dma_align == 0) {
        dev->dma_align = 1;
    }
    if ((dev->dma_align & (dev->dma_align - 1)) != 0) {
        return -E_INVAL;
    }
    if (dev->dma_mask == 0) {
        dev->dma_mask = DMA_MASK_64;
    }
    size_t plen = kstrlen(prefix);
    if (plen == 0 || plen > 8) {
        return -E_INVAL;
    }

    spin_init(&dev->lock);
    dev->q_head = dev->q_tail = NULL;
    dev->q_depth = 0;
    dev->pumping = false;

    irq_flags_t fl = spin_lock_irqsave(&g_block.lock);
    if (g_block.count >= BLOCK_MAX_DEVICES) {
        spin_unlock_irqrestore(&g_block.lock, fl);
        return -E_NOSPC;
    }
    u32 same = 0;
    for (u32 i = 0; i < g_block.count; i++) {
        if (memcmp(g_block.devs[i]->name, prefix, plen) == 0 &&
            g_block.devs[i]->name[plen] != '\0' && g_block.devs[i]->name[plen + 1] == '\0') {
            same++;
        }
    }
    if (same >= 26) {
        spin_unlock_irqrestore(&g_block.lock, fl);
        return -E_NOSPC;
    }
    ksnprintf(dev->name, sizeof dev->name, "%s%c", prefix, (int)('a' + same));
    dev->magic      = BLOCK_MAGIC;
    dev->registered = true;
    g_block.devs[g_block.count++] = dev;
    spin_unlock_irqrestore(&g_block.lock, fl);

    char size[32];
    fmt_size(size, sizeof size, dev->sector_count * dev->sector_size);
    kok("block: %s registered: %s, %lu x %u B sectors, max %u sectors/request%s",
        dev->name, size, dev->sector_count, (unsigned)dev->sector_size,
        (unsigned)dev->max_sectors, (dev->flags & BLOCK_FLAG_READONLY) ? ", read-only" : "");
    return 0;
}

void block_unregister(block_device_t *dev) {
    if (dev == NULL || dev->magic != BLOCK_MAGIC) {
        return;
    }
    irq_flags_t fl = spin_lock_irqsave(&g_block.lock);
    for (u32 i = 0; i < g_block.count; i++) {
        if (g_block.devs[i] == dev) {
            for (u32 k = i + 1; k < g_block.count; k++) {
                g_block.devs[k - 1] = g_block.devs[k];
            }
            g_block.count--;
            break;
        }
    }
    spin_unlock_irqrestore(&g_block.lock, fl);

    /* Fail everything still queued; running requests finish via the driver. */
    fl = spin_lock_irqsave(&dev->lock);
    dev->registered = false;
    block_request_t *r = dev->q_head;
    dev->q_head = dev->q_tail = NULL;
    dev->q_depth = 0;
    spin_unlock_irqrestore(&dev->lock, fl);

    while (r != NULL) {
        block_request_t *next = r->next;
        block_complete(r, -E_NODEV);
        r = next;
    }
}

u32 block_count(void) {
    return g_block.count;
}

block_device_t *block_get(u32 index) {
    return (index < g_block.count) ? g_block.devs[index] : NULL;
}

block_device_t *block_find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (u32 i = 0; i < g_block.count; i++) {
        if (kstrlen(g_block.devs[i]->name) == kstrlen(name) &&
            memcmp(g_block.devs[i]->name, name, kstrlen(name)) == 0) {
            return g_block.devs[i];
        }
    }
    return NULL;
}

/* =========================================================================
   REQUESTS
   ========================================================================= */

block_request_t *block_request_alloc(void) {
    return kzalloc(sizeof(block_request_t));
}

void block_request_free(block_request_t *req) {
    kfree(req);
}

/* Builds a physically-coalesced SG list for a kernel virtual buffer. */
static int32_t build_sg(block_request_t *req, const void *buf, size_t bytes) {
    const u8 *p = buf;
    size_t left = bytes;
    u32 n = 0;

    while (left != 0) {
        u64 pa;
        int32_t rc = dma_virt_to_phys(p, &pa);

        if (rc != 0) {
            return rc;
        }
        size_t chunk = PAGE_SIZE - ((uintptr_t)p & (PAGE_SIZE - 1));
        if (chunk > left) {
            chunk = left;
        }
        if (n != 0 && req->sg[n - 1].phys + req->sg[n - 1].len == pa &&
            (u64)req->sg[n - 1].len + chunk <= BLOCK_SG_MAX_SEG) {
            req->sg[n - 1].len += (u32)chunk;
        } else {
            if (n == BLOCK_MAX_SG) {
                return -E_OVERFLOW;
            }
            req->sg[n].phys     = pa;
            req->sg[n].len      = (u32)chunk;
            req->sg[n].reserved = 0;
            n++;
        }
        p    += chunk;
        left -= chunk;
    }
    req->sg_count = n;
    return 0;
}

int32_t block_request_setup(block_request_t *req, block_device_t *dev, block_op_t op,
                            u64 lba, u32 count, const void *buf) {
    if (req == NULL || dev == NULL || dev->magic != BLOCK_MAGIC) {
        return -E_INVAL;
    }
    memset(req, 0, sizeof *req);
    req->dev = dev;
    req->op  = op;
    req->lba = lba;

    if (op == BLOCK_OP_FLUSH) {
        return 0;
    }
    if ((op != BLOCK_OP_READ && op != BLOCK_OP_WRITE) || buf == NULL ||
        count == 0 || count > dev->max_sectors) {
        return -E_INVAL;
    }
    req->count = count;
    req->buf   = (void *)(uintptr_t)buf;
    return build_sg(req, buf, (size_t)count * dev->sector_size);
}

static bool sg_reachable(const block_device_t *dev, const block_request_t *req) {
    for (u32 i = 0; i < req->sg_count; i++) {
        const block_sg_t *s = &req->sg[i];

        if (!dma_addr_ok(s->phys, s->len, dev->dma_mask) ||
            ((s->phys | s->len) & (dev->dma_align - 1u)) != 0) {
            return false;
        }
    }
    return true;
}

static int32_t validate_request(const block_device_t *dev, const block_request_t *req) {
    switch (req->op) {
    case BLOCK_OP_FLUSH:
        return (req->count == 0) ? 0 : -E_INVAL;
    case BLOCK_OP_READ:
    case BLOCK_OP_WRITE:
        break;
    default:
        return -E_NOTSUP;
    }
    if (req->count == 0 || req->count > dev->max_sectors) {
        return -E_INVAL;
    }
    if (req->lba >= dev->sector_count || req->count > dev->sector_count - req->lba) {
        return -E_RANGE;
    }
    if (req->op == BLOCK_OP_WRITE && (dev->flags & BLOCK_FLAG_READONLY)) {
        return -E_PERM;
    }
    if (req->sg_count == 0 || req->sg_count > dev->max_sg) {
        return -E_INVAL;
    }
    u64 total = 0;
    for (u32 i = 0; i < req->sg_count; i++) {
        total += req->sg[i].len;
    }
    if (total != (u64)req->count * dev->sector_size) {
        return -E_INVAL;
    }
    return sg_reachable(dev, req) ? 0 : -E_ADDR;
}

void block_complete(block_request_t *req, int32_t status) {
    if (req == NULL) {
        return;
    }
    block_device_t *dev = req->dev;
    block_done_fn   cb  = req->on_done;

    if (status == 0) {
        switch (req->op) {
        case BLOCK_OP_READ:
            __atomic_fetch_add(&dev->stats.reads, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&dev->stats.sectors_read, req->count, __ATOMIC_RELAXED);
            break;
        case BLOCK_OP_WRITE:
            __atomic_fetch_add(&dev->stats.writes, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&dev->stats.sectors_written, req->count, __ATOMIC_RELAXED);
            break;
        default:
            __atomic_fetch_add(&dev->stats.flushes, 1, __ATOMIC_RELAXED);
            break;
        }
    } else {
        __atomic_fetch_add(&dev->stats.errors, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_sub(&dev->inflight, 1u, __ATOMIC_RELAXED);

    req->status = status;
    __atomic_store_n(&req->done, true, __ATOMIC_RELEASE);
    /* From here the owner may free `req`: only the cached callback is used. */
    if (cb != NULL) {
        cb(req);
    }
}

int32_t block_submit(block_request_t *req) {
    if (req == NULL || req->dev == NULL) {
        return -E_INVAL;
    }
    block_device_t *dev = req->dev;
    if (dev->magic != BLOCK_MAGIC || !dev->registered) {
        return -E_NODEV;
    }
    int32_t rc = validate_request(dev, req);
    if (rc != 0) {
        return rc;
    }

    req->next   = NULL;
    req->status = -E_AGAIN;
    req->done   = false;

    irq_flags_t fl = spin_lock_irqsave(&dev->lock);
    if (dev->q_depth >= BLOCK_QUEUE_MAX) {
        spin_unlock_irqrestore(&dev->lock, fl);
        return -E_AGAIN;
    }

    if (dev->q_tail == NULL) {
        dev->q_head = req;
    } else {
        dev->q_tail->next = req;
    }
    dev->q_tail = req;
    dev->q_depth++;

    bool start_pump = !dev->pumping;
    if (start_pump) {
        dev->pumping = true;
    }
    spin_unlock_irqrestore(&dev->lock, fl);

    if (start_pump) {
        while (true) {
            fl = spin_lock_irqsave(&dev->lock);
            block_request_t *curr = dev->q_head;
            if (curr == NULL) {
                dev->pumping = false;
                spin_unlock_irqrestore(&dev->lock, fl);
                break;
            }
            dev->q_head = curr->next;
            if (dev->q_head == NULL) {
                dev->q_tail = NULL;
            }
            dev->q_depth--;
            spin_unlock_irqrestore(&dev->lock, fl);

            __atomic_fetch_add(&dev->inflight, 1u, __ATOMIC_RELAXED);
            int32_t rc_sub = dev->ops->submit(dev, curr);
            if (rc_sub != 0) {
                block_complete(curr, rc_sub);
            }
        }
    }
    return 0;
}

int32_t block_read(block_device_t *dev, u64 lba, u64 count, void *buf) {
    block_request_t *req = block_request_alloc();
    if (req == NULL) {
        return -E_NOMEM;
    }
    int32_t rc = block_request_setup(req, dev, BLOCK_OP_READ, lba, count, buf);
    if (rc != 0) {
        block_request_free(req);
        return rc;
    }
    rc = block_submit(req);
    if (rc != 0) {
        block_request_free(req);
        return rc;
    }

    u64 start = timer_ms();
    while (!__atomic_load_n(&req->done, __ATOMIC_ACQUIRE)) {
        if (g_block.wait_hook != NULL) {
            g_block.wait_hook();
        }
        if (timer_ms() - start > BLOCK_WAIT_TIMEOUT_MS) {
            block_request_free(req);
            return -E_TIMEOUT;
        }
    }
    rc = req->status;
    block_request_free(req);
    return rc;
}

int32_t block_write(block_device_t *dev, u64 lba, u64 count, const void *buf) {
    block_request_t *req = block_request_alloc();
    if (req == NULL) {
        return -E_NOMEM;
    }
    int32_t rc = block_request_setup(req, dev, BLOCK_OP_WRITE, lba, count, buf);
    if (rc != 0) {
        block_request_free(req);
        return rc;
    }
    rc = block_submit(req);
    if (rc != 0) {
        block_request_free(req);
        return rc;
    }

    u64 start = timer_ms();
    while (!__atomic_load_n(&req->done, __ATOMIC_ACQUIRE)) {
        if (g_block.wait_hook != NULL) {
            g_block.wait_hook();
        }
        if (timer_ms() - start > BLOCK_WAIT_TIMEOUT_MS) {
            block_request_free(req);
            return -E_TIMEOUT;
        }
    }
    rc = req->status;
    block_request_free(req);
    return rc;
}

int32_t block_flush(block_device_t *dev) {
    block_request_t *req = block_request_alloc();
    if (req == NULL) {
        return -E_NOMEM;
    }
    int32_t rc = block_request_setup(req, dev, BLOCK_OP_FLUSH, 0, 0, NULL);
    if (rc != 0) {
        block_request_free(req);
        return rc;
    }
    rc = block_submit(req);
    if (rc != 0) {
        block_request_free(req);
        return rc;
    }

    u64 start = timer_ms();
    while (!__atomic_load_n(&req->done, __ATOMIC_ACQUIRE)) {
        if (g_block.wait_hook != NULL) {
            g_block.wait_hook();
        }
        if (timer_ms() - start > BLOCK_WAIT_TIMEOUT_MS) {
            block_request_free(req);
            return -E_TIMEOUT;
        }
    }
    rc = req->status;
    block_request_free(req);
    return rc;
}