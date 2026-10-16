// ps5-homebrew-ui - Process-lifetime heap for the OpenGL runtime.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-opengl native-app/app_heap.c. tools/build.sh links with
// --wrap for the malloc family whenever this file exists, routing allocations
// into a fixed 128 MiB sceLibcMspace that is never unmapped.

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *address, size_t size);
void __real_free(void *address);
int __real_posix_memalign(void **address, size_t alignment, size_t size);
size_t __real_malloc_usable_size(const void *address);

void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags);
void *sceLibcMspaceMalloc(void *mspace, size_t size);
void *sceLibcMspaceCalloc(void *mspace, size_t count, size_t size);
void *sceLibcMspaceRealloc(void *mspace, void *address, size_t size);
void sceLibcMspaceFree(void *mspace, void *address);
int sceLibcMspacePosixMemalign(void *mspace, void **address, size_t alignment, size_t size);
size_t sceLibcMspaceMallocUsableSize(const void *address);

/* Fixed, process-lifetime heap: do not unmap underneath late C++ destructors. */
#define HUI_HEAP_SIZE (128u * 1024u * 1024u)

static atomic_int hui_heap_state;
static void *hui_heap_base;
static void *hui_heap_mspace;
/* Owned-heap usable bytes only; relaxed counters are observations, not fences. */
static atomic_size_t hui_heap_live_bytes, hui_heap_peak_bytes, hui_heap_blocks;
static atomic_size_t hui_heap_failures;

/* The mspace is not documented as thread-safe, and worker threads (media
 * upload) allocate concurrently with the render thread. Serialize every
 * mspace call; the lock is never held across a callback into wrapped
 * code, so it cannot recurse. */
static atomic_flag hui_heap_lock = ATOMIC_FLAG_INIT;

static void hui_heap_lock_acquire(void)
{
    while (atomic_flag_test_and_set_explicit(&hui_heap_lock, memory_order_acquire))
    {
    }
}

static void hui_heap_lock_release(void)
{
    atomic_flag_clear_explicit(&hui_heap_lock, memory_order_release);
}

static void hui_heap_resize_stats(size_t before, size_t after)
{
    size_t live = after >= before ? atomic_fetch_add_explicit(&hui_heap_live_bytes, after - before,
                                                              memory_order_relaxed) +
                                        after - before
                                  : atomic_fetch_sub_explicit(&hui_heap_live_bytes, before - after,
                                                              memory_order_relaxed) -
                                        (before - after);
    size_t peak = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&hui_heap_peak_bytes, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed))
    {
    }
}

static void *hui_heap_record_allocation(void *address, int nonzero)
{
    if (address)
    {
        hui_heap_lock_acquire();
        size_t usable = sceLibcMspaceMallocUsableSize(address);
        hui_heap_lock_release();
        hui_heap_resize_stats(0, usable);
        atomic_fetch_add_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
    }
    else if (nonzero)
    {
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    }
    return address;
}

static int hui_heap_ready(void)
{
    int state = atomic_load_explicit(&hui_heap_state, memory_order_acquire);
    if (state == 2)
        return 1;
    if (state != 0)
        return 0;

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&hui_heap_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == 2;

    void *base = mmap(NULL, HUI_HEAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
    {
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    hui_heap_base = base;
    hui_heap_mspace = sceLibcMspaceCreate("ps5-homebrew-ui", base, HUI_HEAP_SIZE, 0);
    if (hui_heap_mspace == NULL)
    {
        hui_heap_base = NULL;
        munmap(base, HUI_HEAP_SIZE);
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    atomic_store_explicit(&hui_heap_state, 2, memory_order_release);
    return 1;
}

static int hui_heap_owns(const void *address)
{
    /* Acquire publication before reading non-atomic heap metadata. */
    if (atomic_load_explicit(&hui_heap_state, memory_order_acquire) != 2)
        return 0;
    uintptr_t value = (uintptr_t)address;
    uintptr_t base = (uintptr_t)hui_heap_base;
    return value >= base && value - base < HUI_HEAP_SIZE;
}

void *__wrap_malloc(size_t size)
{
    if (!hui_heap_ready())
        return __real_malloc(size);
    hui_heap_lock_acquire();
    void *address = sceLibcMspaceMalloc(hui_heap_mspace, size);
    hui_heap_lock_release();
    return hui_heap_record_allocation(address, size != 0);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (!hui_heap_ready())
        return __real_calloc(count, size);
    hui_heap_lock_acquire();
    void *address = sceLibcMspaceCalloc(hui_heap_mspace, count, size);
    hui_heap_lock_release();
    return hui_heap_record_allocation(address, count != 0 && size != 0);
}

void *__wrap_realloc(void *address, size_t size)
{
    if (address == NULL)
        return __wrap_malloc(size);
    if (!hui_heap_owns(address))
        return __real_realloc(address, size);
    hui_heap_lock_acquire();
    size_t before = sceLibcMspaceMallocUsableSize(address);
    void *result = sceLibcMspaceRealloc(hui_heap_mspace, address, size);
    size_t after = result ? sceLibcMspaceMallocUsableSize(result) : 0;
    hui_heap_lock_release();
    if (result)
        hui_heap_resize_stats(before, after);
    else if (size)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

void __wrap_free(void *address)
{
    if (hui_heap_owns(address))
    {
        hui_heap_lock_acquire();
        size_t usable = sceLibcMspaceMallocUsableSize(address);
        sceLibcMspaceFree(hui_heap_mspace, address);
        hui_heap_lock_release();
        hui_heap_resize_stats(usable, 0);
        atomic_fetch_sub_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
    }
    else
    {
        __real_free(address);
    }
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size)
{
    if (!hui_heap_ready())
        return __real_posix_memalign(address, alignment, size);
    hui_heap_lock_acquire();
    int result = sceLibcMspacePosixMemalign(hui_heap_mspace, address, alignment, size);
    hui_heap_lock_release();
    if (result == 0)
        hui_heap_record_allocation(*address, size != 0);
    else if (result == ENOMEM)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

size_t __wrap_malloc_usable_size(const void *address)
{
    if (!hui_heap_owns(address))
        return __real_malloc_usable_size(address);
    hui_heap_lock_acquire();
    size_t usable = sceLibcMspaceMallocUsableSize(address);
    hui_heap_lock_release();
    return usable;
}

/* Diagnostics: current heap counters for logs and the diagnostics screen. */
void hui_heap_stats(size_t *live_bytes, size_t *peak_bytes, size_t *blocks, size_t *failures)
{
    if (live_bytes)
        *live_bytes = atomic_load_explicit(&hui_heap_live_bytes, memory_order_relaxed);
    if (peak_bytes)
        *peak_bytes = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    if (blocks)
        *blocks = atomic_load_explicit(&hui_heap_blocks, memory_order_relaxed);
    if (failures)
        *failures = atomic_load_explicit(&hui_heap_failures, memory_order_relaxed);
}
