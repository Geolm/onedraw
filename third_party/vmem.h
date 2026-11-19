// vmem: single-header reserve-only mmap + linear + stack allocators.
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
#include <string.h>

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

//-----------------------------------------------------------------------------------------------------------------------------
// stack allocator: LIFO with a canary trailer, on top of vmem_reserve.
//
// Every block is the 16-byte-aligned payload plus a 16-byte trailer at its end:
//
//   |  payload (16B-aligned)  | trailer: canary | prev |
//
// - canary: sits right after the payload; an overrun of the block is detected when its
//           trailer is read (pop/top).
// - prev:   the used offset before this block was allocated; pop rewinds to it.
//
// Every block ends with its trailer, so the top block's trailer is always at used - 16:
// pop reads it directly, no size needed. If the canary fails the stack is marked corrupt
// and every further operation returns NULL, rather than trusting a possibly clobbered pointer.

struct stack_allocator
{
    uint8_t* base;
    size_t used;
    size_t peak;   // highest used since release
    size_t size;
    bool corrupt;  // a canary failed; stored pointers can no longer be trusted
};

static const uint64_t stack_canary = 0x0102030405060708ULL;   // every byte distinct: even a 1-byte clobber is detected

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool stack_init(struct stack_allocator* s, size_t size)      // vmem_reserve
{
    s->base = (uint8_t*)vmem_reserve(size);
    s->size = size;
    s->used = 0;
    s->peak = 0;
    s->corrupt = false;
    return s->base != NULL;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void* stack_alloc(struct stack_allocator* s, size_t size)     // 16-byte aligned (malloc's); NULL when full or corrupt
{
    // round up to 16 bytes: malloc's alignment (the base is page-aligned)
    size = (size + 15) & ~(size_t)15;
    if (s->base == NULL || s->corrupt || size == 0 || s->used + size + 16 > s->size)
        return NULL;

    uint8_t* p = s->base + s->used;                    // payload
    uint8_t* t = p + size;                            // trailer, right after the payload
    *(uint64_t*)t       = stack_canary;
    *(uint64_t*)(t + 8) = (uint64_t)s->used;          // prev: where used stood before this block

    s->used += size + 16;
    if (s->used > s->peak)
        s->peak = s->used;
    return p;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void* stack_pop(struct stack_allocator* s)                     // rewind past the top block; NULL when empty or corrupt
{
    if (s->base == NULL || s->corrupt || s->used == 0)
        return NULL;

    uint8_t* t = s->base + s->used - 16;                 // trailer of the top block
    uint64_t canary, prev;
    memcpy(&canary, t, 8);
    memcpy(&prev, t + 8, 8);
    if (canary != stack_canary || (size_t)prev >= s->used)
    {
        s->corrupt = true;
        return NULL;                                     // the top block overran its payload
    }

    s->used = (size_t)prev;
    return s->base + s->used;                            // payload of the popped block
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void* stack_top(struct stack_allocator* s)                      // payload of the current top; NULL when empty or corrupt
{
    if (s->base == NULL || s->corrupt || s->used == 0)
        return NULL;

    uint8_t* t = s->base + s->used - 16;                 // trailer of the top block
    uint64_t canary, prev;
    memcpy(&canary, t, 8);
    memcpy(&prev, t + 8, 8);
    if (canary != stack_canary || (size_t)prev >= s->used)
    {
        s->corrupt = true;
        return NULL;
    }
    return s->base + (size_t)prev;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void stack_reset(struct stack_allocator* s)                    // used = 0
{
    s->used = 0;
    s->corrupt = false;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void stack_release(struct stack_allocator* s)                   // vmem_release of [0, peak), then used = peak = 0
{
    if (s->base != NULL && s->peak > 0)
        vmem_release(s->base, s->peak);
    s->used = 0;
    s->peak = 0;
    s->corrupt = false;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void stack_free(struct stack_allocator* s)                      // vmem_free
{
    if (s->base != NULL)
        vmem_free(s->base, s->size);
    s->base = NULL;
    s->size = 0;
    s->used = 0;
    s->peak = 0;
    s->corrupt = false;
}

#endif // VMEM_H
