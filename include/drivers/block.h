/**
 * @file block.h
 * @brief Generic block device layer: registry, per-device request queues,
 *        scatter-gather requests, sync and async I/O.
 *
 * Request flow:
 *   block_submit() validates -> enqueues -> block_pump() hands the head of the
 *   queue to dev->ops->submit() (-E_BUSY = try again later) and reaps finished
 *   commands via dev->ops->poll(). Drivers finish a request with
 *   block_complete(). Order is strictly FIFO, so overlapping writes can never
 *   be reordered.
 */
#ifndef MYOS_DRIVERS_BLOCK_H
#define MYOS_DRIVERS_BLOCK_H

#include "kernel.h"
#include "kernel/spinlock.h"

#define BLOCK_MAX_DEVICES   32
#define BLOCK_MAX_SG        40
#define BLOCK_SG_MAX_SEG    0x400000u        /* AHCI PRDT limit: 4 MiB per entry */
#define BLOCK_QUEUE_MAX     256
#define BLOCK_NAME_MAX      16

#define BLOCK_FLAG_READONLY  (1u << 0)
#define BLOCK_FLAG_NO_FLUSH  (1u << 1)
#define BLOCK_FLAG_REMOVABLE (1u << 2)

#define BLOCK_IOCTL_GET_SECTOR_SIZE  1       /* u32 *          */
#define BLOCK_IOCTL_GET_SECTOR_COUNT 2       /* u64 *          */
#define BLOCK_IOCTL_GET_MODEL        3       /* char[41]       */
#define BLOCK_IOCTL_GET_SERIAL       4       /* char[21]       */
#define BLOCK_IOCTL_GET_FIRMWARE     5       /* char[9]        */
#define BLOCK_IOCTL_FLUSH            6
#define BLOCK_IOCTL_RESET            7       /* driver: reset the device/port */
#define BLOCK_IOCTL_GET_STATS        8       /* block_stats_t * */

typedef enum { BLOCK_OP_READ = 1, BLOCK_OP_WRITE = 2, BLOCK_OP_FLUSH = 3 } block_op_t;

/** One physically contiguous piece of a buffer. */
typedef struct block_sg {
    u64 phys;
    u32 len;
    u32 reserved;
} block_sg_t;

typedef struct block_device  block_device_t;
typedef struct block_request block_request_t;
typedef void (*block_done_fn)(block_request_t *req);

struct block_request {
    block_request_t *next;               /* queue link (owned by the layer) */
    block_device_t  *dev;
    block_op_t       op;
    u64              lba;
    u32              count;              /* sectors; 0 for FLUSH */
    void            *buf;                /* virtual address the SG list was built from */
    block_sg_t       sg[BLOCK_MAX_SG];
    u32              sg_count;
    block_done_fn    on_done;            /* optional; runs in pump context, no locks held */
    void            *ctx;                /* caller cookie */
    void            *drv;                /* driver private */
    volatile int32_t status;
    volatile bool    done;
};

typedef struct {
    u64 reads, writes, flushes;
    u64 sectors_read, sectors_written;
    u64 errors, queue_peak;
} block_stats_t;

typedef struct block_ops {
    /** Start a request. 0 = started, -E_BUSY = full (retry), other = failed. */
    int32_t (*submit)(block_device_t *dev, block_request_t *req);
    /** Reap completions (calls block_complete). Returns how many. May be NULL. */
    int32_t (*poll)(block_device_t *dev);
    /** Driver-specific ioctl. May be NULL. */
    int32_t (*ioctl)(block_device_t *dev, u32 cmd, void *arg, size_t len);
} block_ops_t;

struct block_device {
    char     name[BLOCK_NAME_MAX];
    u32      magic;
    u32      flags;
    u32      sector_size;                /* bytes: power of two, 512..4096 */
    u64      sector_count;
    u32      max_sectors;                /* per request */
    u32      max_sg;                     /* SG entries per request */
    u32      dma_align;                  /* required buffer/segment alignment (power of 2) */
    u64      dma_mask;                   /* highest DMA-reachable address */
    char     model[41];
    char     serial[21];
    char     firmware[9];
    const block_ops_t *ops;
    void    *priv;

    spinlock_t       lock;               /* protects the queue below */
    block_request_t *q_head, *q_tail;
    u32              q_depth;
    bool             pumping;
    bool             registered;
    volatile u32     inflight;           /* queued + running requests */
    block_stats_t    stats;
};

/** @brief Initialise the registry. */
void block_init(void);

/**
 * @brief Register a device; names are `<prefix>a`, `<prefix>b`, ...
 * Required: ops->submit, sector_size, sector_count, max_sectors, max_sg.
 * @return 0, -E_INVAL, -E_NOSPC
 */
int32_t block_register(block_device_t *dev, const char *prefix);
void    block_unregister(block_device_t *dev);

u32             block_count(void);
block_device_t *block_get(u32 index);
block_device_t *block_find(const char *name);

/* ---- Asynchronous API ---- */
block_request_t *block_request_alloc(void);
void             block_request_free(block_request_t *req);

/** @brief Fill a request, building the SG list for a kernel virtual buffer. */
int32_t block_request_setup(block_request_t *req, block_device_t *dev, block_op_t op,
                            u64 lba, u32 count, const void *buf);

/**
 * @brief Queue a request. Checks bounds, limits and DMA reachability.
 * @return 0, -E_RANGE, -E_INVAL, -E_PERM (read-only device), -E_ADDR
 *         (buffer unreachable: bounce it), -E_AGAIN (queue full), -E_NODEV
 */
int32_t block_submit(block_request_t *req);

/** @brief Drive one device's queue. Returns the amount of progress made. */
int32_t block_pump(block_device_t *dev);
int32_t block_pump_all(void);

/** @brief Wait for completion (pumps while waiting). Returns the request status. */
int32_t block_wait(block_request_t *req);

/** @brief Called by drivers when a request finishes. */
void block_complete(block_request_t *req, int32_t status);

/* ---- Synchronous API (any size, any alignment, bounce buffers as needed) ---- */
int32_t block_read(block_device_t *dev, u64 lba, u64 count, void *buf);
int32_t block_write(block_device_t *dev, u64 lba, u64 count, const void *buf);
int32_t block_flush(block_device_t *dev);
int32_t block_ioctl(block_device_t *dev, u32 cmd, void *arg, size_t len);

/** @brief Hook run while spinning in block_wait (e.g. sched_yield once threads exist). */
void block_set_wait_hook(void (*hook)(void));

/** @brief Spawn the "blkd" kernel thread that pumps all queues (needs the scheduler). */
int32_t block_start_worker(void);

/**
 * @brief Read-mostly self-test: consistency, bounce path, bounds, async, flush.
 * With -DMYOS_BLOCK_WRITE_TEST it also does a save/write/verify/restore cycle
 * on the last sectors of the disk.
 */
int32_t block_selftest(block_device_t *dev);

#endif