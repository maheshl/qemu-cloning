/*
 * vhost support
 *
 * Copyright Red Hat, Inc. 2010
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include <sys/ioctl.h>
#include "vhost.h"
#include "hw/hw.h"
#include "range.h"
#include <linux/vhost.h>
#include "exec-memory.h"

static void vhost_dev_sync_region(struct vhost_dev *dev,
                                  MemoryRegionSection *section,
                                  uint64_t mfirst, uint64_t mlast,
                                  uint64_t rfirst, uint64_t rlast)
{
    uint64_t start = MAX(mfirst, rfirst);
    uint64_t end = MIN(mlast, rlast);
    vhost_log_chunk_t *from = dev->log + start / VHOST_LOG_CHUNK;
    vhost_log_chunk_t *to = dev->log + end / VHOST_LOG_CHUNK + 1;
    uint64_t addr = (start / VHOST_LOG_CHUNK) * VHOST_LOG_CHUNK;

    if (end < start) {
        return;
    }
    assert(end / VHOST_LOG_CHUNK < dev->log_size);
    assert(start / VHOST_LOG_CHUNK < dev->log_size);

    for (;from < to; ++from) {
        vhost_log_chunk_t log;
        int bit;
        /* We first check with non-atomic: much cheaper,
         * and we expect non-dirty to be the common case. */
        if (!*from) {
            addr += VHOST_LOG_CHUNK;
            continue;
        }
        /* Data must be read atomically. We don't really
         * need the barrier semantics of __sync
         * builtins, but it's easier to use them than
         * roll our own. */
        log = __sync_fetch_and_and(from, 0);
        while ((bit = sizeof(log) > sizeof(int) ?
                ffsll(log) : ffs(log))) {
            ram_addr_t ram_addr;
            bit -= 1;
            ram_addr = section->offset_within_region + bit * VHOST_LOG_PAGE;
            memory_region_set_dirty(section->mr, ram_addr, VHOST_LOG_PAGE);
            log &= ~(0x1ull << bit);
        }
        addr += VHOST_LOG_CHUNK;
    }
}

static int vhost_sync_dirty_bitmap(struct vhost_dev *dev,
                                   MemoryRegionSection *section,
                                   target_phys_addr_t start_addr,
                                   target_phys_addr_t end_addr)
{
    int i;

    if (!dev->log_enabled || !dev->started) {
        return 0;
    }
    for (i = 0; i < dev->mem->nregions; ++i) {
        struct vhost_memory_region *reg = dev->mem->regions + i;
        vhost_dev_sync_region(dev, section, start_addr, end_addr,
                              reg->guest_phys_addr,
                              range_get_last(reg->guest_phys_addr,
                                             reg->memory_size));
    }
    for (i = 0; i < dev->nvqs; ++i) {
        struct vhost_virtqueue *vq = dev->vqs + i;
        vhost_dev_sync_region(dev, section, start_addr, end_addr, vq->used_phys,
                              range_get_last(vq->used_phys, vq->used_size));
    }
    return 0;
}

static void vhost_log_sync(MemoryListener *listener,
                          MemoryRegionSection *section)
{
    struct vhost_dev *dev = container_of(listener, struct vhost_dev,
                                         memory_listener);
    target_phys_addr_t start_addr = section->offset_within_address_space;
    target_phys_addr_t end_addr = start_addr + section->size;

    vhost_sync_dirty_bitmap(dev, section, start_addr, end_addr);
}

/* Assign/unassign. Keep an unsorted array of non-overlapping
 * memory regions in dev->mem. */
static void vhost_dev_unassign_memory(struct vhost_dev *dev,
                                      uint64_t start_addr,
                                      uint64_t size)
{
    int from, to, n = dev->mem->nregions;
    /* Track overlapping/split regions for sanity checking. */
    int overlap_start = 0, overlap_end = 0, overlap_middle = 0, split = 0;

    for (from = 0, to = 0; from < n; ++from, ++to) {
        struct vhost_memory_region *reg = dev->mem->regions + to;
        uint64_t reglast;
        uint64_t memlast;
        uint64_t change;

        /* clone old region */
        if (to != from) {
            memcpy(reg, dev->mem->regions + from, sizeof *reg);
        }

        /* No overlap is simple */
        if (!ranges_overlap(reg->guest_phys_addr, reg->memory_size,
                            start_addr, size)) {
            continue;
        }

        /* Split only happens if supplied region
         * is in the middle of an existing one. Thus it can not
         * overlap with any other existing region. */
        assert(!split);

        reglast = range_get_last(reg->guest_phys_addr, reg->memory_size);
        memlast = range_get_last(start_addr, size);

        /* Remove whole region */
        if (start_addr <= reg->guest_phys_addr && memlast >= reglast) {
            --dev->mem->nregions;
            --to;
            ++overlap_middle;
            continue;
        }

        /* Shrink region */
        if (memlast >= reglast) {
            reg->memory_size = start_addr - reg->guest_phys_addr;
            assert(reg->memory_size);
            assert(!overlap_end);
            ++overlap_end;
            continue;
        }

        /* Shift region */
        if (start_addr <= reg->guest_phys_addr) {
            change = memlast + 1 - reg->guest_phys_addr;
            reg->memory_size -= change;
            reg->guest_phys_addr += change;
            reg->userspace_addr += change;
            assert(reg->memory_size);
            assert(!overlap_start);
            ++overlap_start;
            continue;
        }

        /* This only happens if supplied region
         * is in the middle of an existing one. Thus it can not
         * overlap with any other existing region. */
        assert(!overlap_start);
        assert(!overlap_end);
        assert(!overlap_middle);
        /* Split region: shrink first part, shift second part. */
        memcpy(dev->mem->regions + n, reg, sizeof *reg);
        reg->memory_size = start_addr - reg->guest_phys_addr;
        assert(reg->memory_size);
        change = memlast + 1 - reg->guest_phys_addr;
        reg = dev->mem->regions + n;
        reg->memory_size -= change;
        assert(reg->memory_size);
        reg->guest_phys_addr += change;
        reg->userspace_addr += change;
        /* Never add more than 1 region */
        assert(dev->mem->nregions == n);
        ++dev->mem->nregions;
        ++split;
    }
}

/* Called after unassign, so no regions overlap the given range. */
static void vhost_dev_assign_memory(struct vhost_dev *dev,
                                    uint64_t start_addr,
                                    uint64_t size,
                                    uint64_t uaddr)
{
    int from, to;
    struct vhost_memory_region *merged = NULL;
    for (from = 0, to = 0; from < dev->mem->nregions; ++from, ++to) {
        struct vhost_memory_region *reg = dev->mem->regions + to;
        uint64_t prlast, urlast;
        uint64_t pmlast, umlast;
        uint64_t s, e, u;

        /* clone old region */
        if (to != from) {
            memcpy(reg, dev->mem->regions + from, sizeof *reg);
        }
        prlast = range_get_last(reg->guest_phys_addr, reg->memory_size);
        pmlast = range_get_last(start_addr, size);
        urlast = range_get_last(reg->userspace_addr, reg->memory_size);
        umlast = range_get_last(uaddr, size);

        /* check for overlapping regions: should never happen. */
        assert(prlast < start_addr || pmlast < reg->guest_phys_addr);
        /* Not an adjacent or overlapping region - do not merge. */
        if ((prlast + 1 != start_addr || urlast + 1 != uaddr) &&
            (pmlast + 1 != reg->guest_phys_addr ||
             umlast + 1 != reg->userspace_addr)) {
            continue;
        }

        if (merged) {
            --to;
            assert(to >= 0);
        } else {
            merged = reg;
        }
        u = MIN(uaddr, reg->userspace_addr);
        s = MIN(start_addr, reg->guest_phys_addr);
        e = MAX(pmlast, prlast);
        uaddr = merged->userspace_addr = u;
        start_addr = merged->guest_phys_addr = s;
        size = merged->memory_size = e - s + 1;
        assert(merged->memory_size);
    }

    if (!merged) {
        struct vhost_memory_region *reg = dev->mem->regions + to;
        memset(reg, 0, sizeof *reg);
        reg->memory_size = size;
        assert(reg->memory_size);
        reg->guest_phys_addr = start_addr;
        reg->userspace_addr = uaddr;
        ++to;
    }
    assert(to <= dev->mem->nregions + 1);
    dev->mem->nregions = to;
}

static uint64_t vhost_get_log_size(struct vhost_dev *dev)
{
    uint64_t log_size = 0;
    int i;
    for (i = 0; i < dev->mem->nregions; ++i) {
        struct vhost_memory_region *reg = dev->mem->regions + i;
        uint64_t last = range_get_last(reg->guest_phys_addr,
                                       reg->memory_size);
        log_size = MAX(log_size, last / VHOST_LOG_CHUNK + 1);
    }
    for (i = 0; i < dev->nvqs; ++i) {
        struct vhost_virtqueue *vq = dev->vqs + i;
        uint64_t last = vq->used_phys + vq->used_size - 1;
        log_size = MAX(log_size, last / VHOST_LOG_CHUNK + 1);
    }
    return log_size;
}

static inline void vhost_dev_log_resize(struct vhost_dev* dev, uint64_t size)
{
    vhost_log_chunk_t *log;
    uint64_t log_base;
    int r, i;
    if (size) {
        log = g_malloc0(size * sizeof *log);
    } else {
        log = NULL;
    }
    log_base = (uint64_t)(unsigned long)log;
    r = ioctl(dev->control, VHOST_SET_LOG_BASE, &log_base);
    assert(r >= 0);
    for (i = 0; i < dev->n_mem_sections; ++i) {
        /* Sync only the range covered by the old log */
        vhost_sync_dirty_bitmap(dev, &dev->mem_sections[i], 0,
                                dev->log_size * VHOST_LOG_CHUNK - 1);
    }
    if (dev->log) {
        g_free(dev->log);
    }
    dev->log = log;
    dev->log_size = size;
}

static int vhost_verify_ring_mappings(struct vhost_dev *dev,
                                      uint64_t start_addr,
                                      uint64_t size)
{
    int i;
    for (i = 0; i < dev->nvqs; ++i) {
        struct vhost_virtqueue *vq = dev->vqs + i;
        target_phys_addr_t l;
        void *p;

        if (!ranges_overlap(start_addr, size, vq->ring_phys, vq->ring_size)) {
            continue;
        }
        l = vq->ring_size;
        p = cpu_physical_memory_map(vq->ring_phys, &l, 1);
        if (!p || l != vq->ring_size) {
            fprintf(stderr, "Unable to map ring buffer for ring %d\n", i);
            return -ENOMEM;
        }
        if (p != vq->ring) {
            fprintf(stderr, "Ring buffer relocated for ring %d\n", i);
            return -EBUSY;
        }
        cpu_physical_memory_unmap(p, l, 0, 0);
    }
    return 0;
}

static struct vhost_memory_region *vhost_dev_find_reg(struct vhost_dev *dev,
						      uint64_t start_addr,
						      uint64_t size)
{
    int i, n = dev->mem->nregions;
    for (i = 0; i < n; ++i) {
        struct vhost_memory_region *reg = dev->mem->regions + i;
        if (ranges_overlap(reg->guest_phys_addr, reg->memory_size,
                           start_addr, size)) {
            return reg;
        }
    }
    return NULL;
}

static bool vhost_dev_cmp_memory(struct vhost_dev *dev,
                                 uint64_t start_addr,
                                 uint64_t size,
                                 uint64_t uaddr)
{
    struct vhost_memory_region *reg = vhost_dev_find_reg(dev, start_addr, size);
    uint64_t reglast;
    uint64_t memlast;

    if (!reg) {
        return true;
    }

    reglast = range_get_last(reg->guest_phys_addr, reg->memory_size);
    memlast = range_get_last(start_addr, size);

    /* Need to extend region? */
    if (start_addr < reg->guest_phys_addr || memlast > reglast) {
        return true;
    }
    /* userspace_addr changed? */
    return uaddr != reg->userspace_addr + start_addr - reg->guest_phys_addr;
}

static void vhost_set_memory(MemoryListener *listener,
                             MemoryRegionSection *section,
                             bool add)
{
    struct vhost_dev *dev = container_of(listener, struct vhost_dev,
                                         memory_listener);
    target_phys_addr_t start_addr = section->offset_within_address_space;
    ram_addr_t size = section->size;
    bool log_dirty = memory_region_is_logging(section->mr);
    int s = offsetof(struct vhost_memory, regions) +
        (dev->mem->nregions + 1) * sizeof dev->mem->regions[0];
    uint64_t log_size;
    int r;
    void *ram;

    dev->mem = g_realloc(dev->mem, s);

    if (log_dirty) {
        add = false;
    }

    assert(size);

    /* Optimize no-change case. At least cirrus_vga does this a lot at this time. */
    ram = memory_region_get_ram_ptr(section->mr) + section->offset_within_region;
    if (add) {
        if (!vhost_dev_cmp_memory(dev, start_addr, size, (uintptr_t)ram)) {
            /* Region exists with same address. Nothing to do. */
            return;
        }
    } else {
        if (!vhost_dev_find_reg(dev, start_addr, size)) {
            /* Removing region that we don't access. Nothing to do. */
            return;
        }
    }

    vhost_dev_unassign_memory(dev, start_addr, size);
    if (add) {
        /* Add given mapping, merging adjacent regions if any */
        vhost_dev_assign_memory(dev, start_addr, size, (uintptr_t)ram);
    } else {
        /* Remove old mapping for this memory, if any. */
        vhost_dev_unassign_memory(dev, start_addr, size);
    }

    if (!dev->started) {
        return;
    }

    if (dev->started) {
        r = vhost_verify_ring_mappings(dev, start_addr, size);
        assert(r >= 0);
    }

    if (!dev->log_enabled) {
        r = ioctl(dev->control, VHOST_SET_MEM_TABLE, dev->mem);
        assert(r >= 0);
        return;
    }
    log_size = vhost_get_log_size(dev);
    /* We allocate an extra 4K bytes to log,
     * to reduce the * number of reallocations. */
#define VHOST_LOG_BUFFER (0x1000 / sizeof *dev->log)
    /* To log more, must increase log size before table update. */
    if (dev->log_size < log_size) {
        vhost_dev_log_resize(dev, log_size + VHOST_LOG_BUFFER);
    }
    r = ioctl(dev->control, VHOST_SET_MEM_TABLE, dev->mem);
    assert(r >= 0);
    /* To log less, can only decrease log size after table update. */
    if (dev->log_size > log_size + VHOST_LOG_BUFFER) {
        vhost_dev_log_resize(dev, log_size);
    }
}

static bool vhost_section(MemoryRegionSection *section)
{
    return section->address_space == get_system_memory()
        && memory_region_is_ram(section->mr);
}

static void vhost_begin(MemoryListener *listener)
{
}

static void vhost_commit(MemoryListener *listener)
{
}

static void vhost_region_add(MemoryListener *listener,
                             MemoryRegionSection *section)
{
    struct vhost_dev *dev = container_of(listener, struct vhost_dev,
                                         memory_listener);

    if (!vhost_section(section)) {
        return;
    }

    ++dev->n_mem_sections;
    dev->mem_sections = g_renew(MemoryRegionSection, dev->mem_sections,
                                dev->n_mem_sections);
    dev->mem_sections[dev->n_mem_sections - 1] = *section;
    vhost_set_memory(listener, section, true);
}

static void vhost_region_del(MemoryListener *listener,
                             MemoryRegionSection *section)
{
    struct vhost_dev *dev = container_of(listener, struct vhost_dev,
                                         memory_listener);
    int i;

    if (!vhost_section(section)) {
        return;
    }

    vhost_set_memory(listener, section, false);
    for (i = 0; i < dev->n_mem_sections; ++i) {
        if (dev->mem_sections[i].offset_within_address_space
            == section->offset_within_address_space) {
            --dev->n_mem_sections;
            memmove(&dev->mem_sections[i], &dev->mem_sections[i+1],
                    (dev->n_mem_sections - i) * sizeof(*dev->mem_sections));
            break;
        }
    }
}

static void vhost_region_nop(MemoryListener *listener,
                             MemoryRegionSection *section)
{
}

static int vhost_virtqueue_set_addr(struct vhost_dev *dev,
                                    struct vhost_virtqueue *vq,
                                    unsigned idx, bool enable_log)
{
    struct vhost_vring_addr addr = {
        .index = idx,
        .desc_user_addr = (uint64_t)(unsigned long)vq->desc,
        .avail_user_addr = (uint64_t)(unsigned long)vq->avail,
        .used_user_addr = (uint64_t)(unsigned long)vq->used,
        .log_guest_addr = vq->used_phys,
        .flags = enable_log ? (1 << VHOST_VRING_F_LOG) : 0,
    };
    int r = ioctl(dev->control, VHOST_SET_VRING_ADDR, &addr);
    if (r < 0) {
        return -errno;
    }
    return 0;
}

static int vhost_dev_set_features(struct vhost_dev *dev, bool enable_log)
{
    uint64_t features = dev->acked_features;
    int r;
    if (enable_log) {
        features |= 0x1 << VHOST_F_LOG_ALL;
    }
    r = ioctl(dev->control, VHOST_SET_FEATURES, &features);
    return r < 0 ? -errno : 0;
}

static int vhost_dev_set_log(struct vhost_dev *dev, bool enable_log)
{
    int r, t, i;
    r = vhost_dev_set_features(dev, enable_log);
    if (r < 0) {
        goto err_features;
    }
    for (i = 0; i < dev->nvqs; ++i) {
        r = vhost_virtqueue_set_addr(dev, dev->vqs + i, i,
                                     enable_log);
        if (r < 0) {
            goto err_vq;
        }
    }
    return 0;
err_vq:
    for (; i >= 0; --i) {
        t = vhost_virtqueue_set_addr(dev, dev->vqs + i, i,
                                     dev->log_enabled);
        assert(t >= 0);
    }
    t = vhost_dev_set_features(dev, dev->log_enabled);
    assert(t >= 0);
err_features:
    return r;
}

static int vhost_migration_log(MemoryListener *listener, int enable)
{
    struct vhost_dev *dev = container_of(listener, struct vhost_dev,
                                         memory_listener);
    int r;
    if (!!enable == dev->log_enabled) {
        return 0;
    }
    if (!dev->started) {
        dev->log_enabled = enable;
        return 0;
    }
    if (!enable) {
        r = vhost_dev_set_log(dev, false);
        if (r < 0) {
            return r;
        }
        if (dev->log) {
            g_free(dev->log);
        }
        dev->log = NULL;
        dev->log_size = 0;
    } else {
        vhost_dev_log_resize(dev, vhost_get_log_size(dev));
        r = vhost_dev_set_log(dev, true);
        if (r < 0) {
            return r;
        }
    }
    dev->log_enabled = enable;
    return 0;
}

static void vhost_log_global_start(MemoryListener *listener)
{
    int r;

    r = vhost_migration_log(listener, true);
    if (r < 0) {
        abort();
    }
}

static void vhost_log_global_stop(MemoryListener *listener)
{
    int r;

    r = vhost_migration_log(listener, false);
    if (r < 0) {
        abort();
    }
}

static void vhost_log_start(MemoryListener *listener,
                            MemoryRegionSection *section)
{
    /* FIXME: implement */
}

static void vhost_log_stop(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    /* FIXME: implement */
}

static int vhost_virtqueue_init(struct vhost_dev *dev,
                                struct VirtIODevice *vdev,
                                struct vhost_virtqueue *vq,
                                unsigned idx)
{
    target_phys_addr_t s, l, a;
    int r;
    struct vhost_vring_file file = {
        .index = idx,
    };
    struct vhost_vring_state state = {
        .index = idx,
    };
    struct VirtQueue *vvq = virtio_get_queue(vdev, idx);

    vq->num = state.num = virtio_queue_get_num(vdev, idx);
    r = ioctl(dev->control, VHOST_SET_VRING_NUM, &state);
    if (r) {
        return -errno;
    }

    state.num = virtio_queue_get_last_avail_idx(vdev, idx);
    r = ioctl(dev->control, VHOST_SET_VRING_BASE, &state);
    if (r) {
        return -errno;
    }

    s = l = virtio_queue_get_desc_size(vdev, idx);
    a = virtio_queue_get_desc_addr(vdev, idx);
    vq->desc = cpu_physical_memory_map(a, &l, 0);
    if (!vq->desc || l != s) {
        r = -ENOMEM;
        goto fail_alloc_desc;
    }
    s = l = virtio_queue_get_avail_size(vdev, idx);
    a = virtio_queue_get_avail_addr(vdev, idx);
    vq->avail = cpu_physical_memory_map(a, &l, 0);
    if (!vq->avail || l != s) {
        r = -ENOMEM;
        goto fail_alloc_avail;
    }
    vq->used_size = s = l = virtio_queue_get_used_size(vdev, idx);
    vq->used_phys = a = virtio_queue_get_used_addr(vdev, idx);
    vq->used = cpu_physical_memory_map(a, &l, 1);
    if (!vq->used || l != s) {
        r = -ENOMEM;
        goto fail_alloc_used;
    }

    vq->ring_size = s = l = virtio_queue_get_ring_size(vdev, idx);
    vq->ring_phys = a = virtio_queue_get_ring_addr(vdev, idx);
    vq->ring = cpu_physical_memory_map(a, &l, 1);
    if (!vq->ring || l != s) {
        r = -ENOMEM;
        goto fail_alloc_ring;
    }

    r = vhost_virtqueue_set_addr(dev, vq, idx, dev->log_enabled);
    if (r < 0) {
        r = -errno;
        goto fail_alloc;
    }
    file.fd = event_notifier_get_fd(virtio_queue_get_host_notifier(vvq));
    r = ioctl(dev->control, VHOST_SET_VRING_KICK, &file);
    if (r) {
        r = -errno;
        goto fail_kick;
    }

    file.fd = event_notifier_get_fd(virtio_queue_get_guest_notifier(vvq));
    r = ioctl(dev->control, VHOST_SET_VRING_CALL, &file);
    if (r) {
        r = -errno;
        goto fail_call;
    }

    return 0;

fail_call:
fail_kick:
fail_alloc:
    cpu_physical_memory_unmap(vq->ring, virtio_queue_get_ring_size(vdev, idx),
                              0, 0);
fail_alloc_ring:
    cpu_physical_memory_unmap(vq->used, virtio_queue_get_used_size(vdev, idx),
                              0, 0);
fail_alloc_used:
    cpu_physical_memory_unmap(vq->avail, virtio_queue_get_avail_size(vdev, idx),
                              0, 0);
fail_alloc_avail:
    cpu_physical_memory_unmap(vq->desc, virtio_queue_get_desc_size(vdev, idx),
                              0, 0);
fail_alloc_desc:
    return r;
}

static void vhost_virtqueue_cleanup(struct vhost_dev *dev,
                                    struct VirtIODevice *vdev,
                                    struct vhost_virtqueue *vq,
                                    unsigned idx)
{
    struct vhost_vring_state state = {
        .index = idx,
    };
    int r;
    r = ioctl(dev->control, VHOST_GET_VRING_BASE, &state);
    if (r < 0) {
        fprintf(stderr, "vhost VQ %d ring restore failed: %d\n", idx, r);
        fflush(stderr);
    }
    virtio_queue_set_last_avail_idx(vdev, idx, state.num);
    assert (r >= 0);
    cpu_physical_memory_unmap(vq->ring, virtio_queue_get_ring_size(vdev, idx),
                              0, virtio_queue_get_ring_size(vdev, idx));
    cpu_physical_memory_unmap(vq->used, virtio_queue_get_used_size(vdev, idx),
                              1, virtio_queue_get_used_size(vdev, idx));
    cpu_physical_memory_unmap(vq->avail, virtio_queue_get_avail_size(vdev, idx),
                              0, virtio_queue_get_avail_size(vdev, idx));
    cpu_physical_memory_unmap(vq->desc, virtio_queue_get_desc_size(vdev, idx),
                              0, virtio_queue_get_desc_size(vdev, idx));
}

static void vhost_eventfd_add(MemoryListener *listener,
                              MemoryRegionSection *section,
                              bool match_data, uint64_t data, EventNotifier *e)
{
}

static void vhost_eventfd_del(MemoryListener *listener,
                              MemoryRegionSection *section,
                              bool match_data, uint64_t data, EventNotifier *e)
{
}

int vhost_dev_init(struct vhost_dev *hdev, int devfd, bool force)
{
    uint64_t features;
    int r;
    if (devfd >= 0) {
        hdev->control = devfd;
    } else {
        hdev->control = open("/dev/vhost-net", O_RDWR);
        if (hdev->control < 0) {
            return -errno;
        }
    }
    r = ioctl(hdev->control, VHOST_SET_OWNER, NULL);
    if (r < 0) {
        goto fail;
    }

    r = ioctl(hdev->control, VHOST_GET_FEATURES, &features);
    if (r < 0) {
        goto fail;
    }
    hdev->features = features;

    hdev->memory_listener = (MemoryListener) {
        .begin = vhost_begin,
        .commit = vhost_commit,
        .region_add = vhost_region_add,
        .region_del = vhost_region_del,
        .region_nop = vhost_region_nop,
        .log_start = vhost_log_start,
        .log_stop = vhost_log_stop,
        .log_sync = vhost_log_sync,
        .log_global_start = vhost_log_global_start,
        .log_global_stop = vhost_log_global_stop,
        .eventfd_add = vhost_eventfd_add,
        .eventfd_del = vhost_eventfd_del,
        .priority = 10
    };
    hdev->mem = g_malloc0(offsetof(struct vhost_memory, regions));
    hdev->n_mem_sections = 0;
    hdev->mem_sections = NULL;
    hdev->log = NULL;
    hdev->log_size = 0;
    hdev->log_enabled = false;
    hdev->started = false;
    memory_listener_register(&hdev->memory_listener, NULL);
    hdev->force = force;
    return 0;
fail:
    r = -errno;
    close(hdev->control);
    return r;
}

void vhost_dev_cleanup(struct vhost_dev *hdev)
{
    memory_listener_unregister(&hdev->memory_listener);
    g_free(hdev->mem);
    g_free(hdev->mem_sections);
    close(hdev->control);
}

bool vhost_dev_query(struct vhost_dev *hdev, VirtIODevice *vdev)
{
    return !vdev->binding->query_guest_notifiers ||
        vdev->binding->query_guest_notifiers(vdev->binding_opaque) ||
        hdev->force;
}

/* Stop processing guest IO notifications in qemu.
 * Start processing them in vhost in kernel.
 */
int vhost_dev_enable_notifiers(struct vhost_dev *hdev, VirtIODevice *vdev)
{
    int i, r;
    if (!vdev->binding->set_host_notifier) {
        fprintf(stderr, "binding does not support host notifiers\n");
        r = -ENOSYS;
        goto fail;
    }

    for (i = 0; i < hdev->nvqs; ++i) {
        r = vdev->binding->set_host_notifier(vdev->binding_opaque, i, true);
        if (r < 0) {
            fprintf(stderr, "vhost VQ %d notifier binding failed: %d\n", i, -r);
            goto fail_vq;
        }
    }

    return 0;
fail_vq:
    while (--i >= 0) {
        r = vdev->binding->set_host_notifier(vdev->binding_opaque, i, false);
        if (r < 0) {
            fprintf(stderr, "vhost VQ %d notifier cleanup error: %d\n", i, -r);
            fflush(stderr);
        }
        assert (r >= 0);
    }
fail:
    return r;
}

/* Stop processing guest IO notifications in vhost.
 * Start processing them in qemu.
 * This might actually run the qemu handlers right away,
 * so virtio in qemu must be completely setup when this is called.
 */
void vhost_dev_disable_notifiers(struct vhost_dev *hdev, VirtIODevice *vdev)
{
    int i, r;

    for (i = 0; i < hdev->nvqs; ++i) {
        r = vdev->binding->set_host_notifier(vdev->binding_opaque, i, false);
        if (r < 0) {
            fprintf(stderr, "vhost VQ %d notifier cleanup failed: %d\n", i, -r);
            fflush(stderr);
        }
        assert (r >= 0);
    }
}

/* Host notifiers must be enabled at this point. */
int vhost_dev_start(struct vhost_dev *hdev, VirtIODevice *vdev)
{
    int i, r;
    if (!vdev->binding->set_guest_notifiers) {
        fprintf(stderr, "binding does not support guest notifiers\n");
        r = -ENOSYS;
        goto fail;
    }

    r = vdev->binding->set_guest_notifiers(vdev->binding_opaque, true);
    if (r < 0) {
        fprintf(stderr, "Error binding guest notifier: %d\n", -r);
        goto fail_notifiers;
    }

    r = vhost_dev_set_features(hdev, hdev->log_enabled);
    if (r < 0) {
        goto fail_features;
    }
    r = ioctl(hdev->control, VHOST_SET_MEM_TABLE, hdev->mem);
    if (r < 0) {
        r = -errno;
        goto fail_mem;
    }
    for (i = 0; i < hdev->nvqs; ++i) {
        r = vhost_virtqueue_init(hdev,
                                 vdev,
                                 hdev->vqs + i,
                                 i);
        if (r < 0) {
            goto fail_vq;
        }
    }

    if (hdev->log_enabled) {
        hdev->log_size = vhost_get_log_size(hdev);
        hdev->log = hdev->log_size ?
            g_malloc0(hdev->log_size * sizeof *hdev->log) : NULL;
        r = ioctl(hdev->control, VHOST_SET_LOG_BASE,
                  (uint64_t)(unsigned long)hdev->log);
        if (r < 0) {
            r = -errno;
            goto fail_log;
        }
    }

    hdev->started = true;

    return 0;
fail_log:
fail_vq:
    while (--i >= 0) {
        vhost_virtqueue_cleanup(hdev,
                                vdev,
                                hdev->vqs + i,
                                i);
    }
fail_mem:
fail_features:
    vdev->binding->set_guest_notifiers(vdev->binding_opaque, false);
fail_notifiers:
fail:
    return r;
}

/* Host notifiers must be enabled at this point. */
void vhost_dev_stop(struct vhost_dev *hdev, VirtIODevice *vdev)
{
    int i, r;

    for (i = 0; i < hdev->nvqs; ++i) {
        vhost_virtqueue_cleanup(hdev,
                                vdev,
                                hdev->vqs + i,
                                i);
    }
    for (i = 0; i < hdev->n_mem_sections; ++i) {
        vhost_sync_dirty_bitmap(hdev, &hdev->mem_sections[i],
                                0, (target_phys_addr_t)~0x0ull);
    }
    r = vdev->binding->set_guest_notifiers(vdev->binding_opaque, false);
    if (r < 0) {
        fprintf(stderr, "vhost guest notifier cleanup failed: %d\n", r);
        fflush(stderr);
    }
    assert (r >= 0);

    hdev->started = false;
    g_free(hdev->log);
    hdev->log = NULL;
    hdev->log_size = 0;
}
