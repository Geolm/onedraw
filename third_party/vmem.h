// vmem: single-header reserve-only mmap + linear allocator.
//
// Standalone (POSIX mmap), no dependencies, nothing to link: every function is static inline.
//
// Reserve, don't commit: vmem_reserve only reserves address space, the OS commits a page when
// it is first touched, so a reserved worst case costs no RAM. Pages are given back with
// vmem_release (madvise MADV_FREE), the range stays usable.

#ifndef VMEM_H
#define VMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/mman.h>

//-----------------------------------------------------------------------------------------------------------------------------
static inline void* vmem_reserve(size_t size)
{
    if (size == 0)
        return NULL;

    // reserve-only : no page is committed until it is first touched
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void vmem_release(void* ptr, size_t size)  // madvise(MADV_FREE): pages back to the OS, content lost, range still usable
{
    if (ptr != NULL && size > 0)
        madvise(ptr, size, MADV_FREE);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void vmem_free(void* ptr, size_t size)    // munmap
{
    if (ptr != NULL && size > 0)
        munmap(ptr, size);
}

//-----------------------------------------------------------------------------------------------------------------------------
struct linear_allocator
{
    uint8_t* base;
    size_t used;
    size_t peak;   // highest used since release
    size_t size;
};

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool linear_init(struct linear_allocator* a, size_t size)    // vmem_reserve
{
    a->base = (uint8_t*)vmem_reserve(size);
    a->size = size;
    a->used = 0;
    a->peak = 0;
    return a->base != NULL;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void* linear_alloc(struct linear_allocator* a, size_t size)   // 16-byte aligned (malloc's); NULL when full
{
    // round up to 16 bytes: malloc's alignment (the base is page-aligned)
    size = (size + 15) & ~(size_t)15;
    if (a->base == NULL || a->used + size > a->size)
        return NULL;
    void* p = a->base + a->used;
    a->used += size;
    if (a->used > a->peak)
        a->peak = a->used;
    return p;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void linear_reset(struct linear_allocator* a)                // used = 0
{
    a->used = 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void linear_release(struct linear_allocator* a)              // vmem_release of [0, peak), then used = peak = 0
{
    if (a->base != NULL && a->peak > 0)
        vmem_release(a->base, a->peak);
    a->used = 0;
    a->peak = 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void linear_free(struct linear_allocator* a)                 // vmem_free
{
    if (a->base != NULL)
        vmem_free(a->base, a->size);
    a->base = NULL;
    a->size = 0;
    a->used = 0;
    a->peak = 0;
}

#endif // VMEM_H
