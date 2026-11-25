// vmem: single-header reserve-only mmap + linear + stack allocators.
//
// Standalone (POSIX mmap), no dependencies, nothing to link: every function is static inline.
//
// Reserve, don't commit: vmem_reserve only reserves address space, the OS commits a page when
// it is first touched, so a reserved worst case costs no RAM. Every arena is fenced with one
// PROT_NONE guard page at each end, so running off either end faults immediately. Committed
// pages are given back with vmem_release, which remaps the range: the pages are dropped right
// away (unlike MADV_FREE, which defers to memory pressure), the range reads back as zeros,
// and the range stays usable.
//
// Not thread-safe. Targets macOS (Linux untested). Blocks handed out by both allocators are
// 16-byte aligned.
//
// Under AddressSanitizer, reserved-but-unallocated space, the padding after each requested
// size, the stack trailers and freed ranges are poisoned, so out-of-bounds and use-after-free
// accesses are reported at the faulting instruction. Outside ASan, the stack's canary is the
// only corruption detection in release builds.
//
// Optional: define VMEM_ON_CORRUPT(s) before including this header to replace the default
// handler (prints to stderr, then __builtin_trap()) called on stack corruption, for example
// to log or to keep the old "return NULL" behaviour.

#ifndef VMEM_H
#define VMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define VMEM_VERSION "0.0.2"

#if (defined(__has_feature) && __has_feature(address_sanitizer)) || defined(__SANITIZE_ADDRESS__)
#define VMEM_ASAN 1
#include <sanitizer/asan_interface.h>
#endif

//-----------------------------------------------------------------------------------------------------------------------------
#if VMEM_ASAN
static inline void vmem__poison(void* p, size_t size)
{
    ASAN_POISON_MEMORY_REGION(p, size);
}

static inline void vmem__unpoison(void* p, size_t size)
{
    ASAN_UNPOISON_MEMORY_REGION(p, size);
}
#else
static inline void vmem__poison(void* p, size_t size)
{
    (void)p;
    (void)size;
}

static inline void vmem__unpoison(void* p, size_t size)
{
    (void)p;
    (void)size;
}
#endif

//-----------------------------------------------------------------------------------------------------------------------------
// Reserves `size` bytes of address space (no RAM until a page is touched): maps size rounded
// up to the page size, fenced with one PROT_NONE guard page at each end, and returns the
// address just past the leading guard. The guards take address space only, no RAM.
// vmem_free and vmem_release expect the size as passed here, not the rounded one.
static inline void* vmem_reserve(size_t size)
{
    if (size == 0)
        return NULL;

    size_t page = (size_t)getpagesize();
    size_t rounded = (size + page - 1) & ~(page - 1);   // getpagesize() is a power of two

    void* start = mmap(NULL, rounded + page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED)
        return NULL;

    mprotect(start, page, PROT_NONE);                              // leading guard
    mprotect((char*)start + rounded + page, page, PROT_NONE);      // trailing guard
    return (char*)start + page;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Remaps [ptr, ptr + size) as fresh anonymous pages (MAP_FIXED): the old pages are dropped
// right away, so the process's phys_footprint falls immediately, the range reads back as
// zeros, and the range stays usable. The guards placed by vmem_reserve are never touched.
// Returns false on failure.
static inline bool vmem_release(void* ptr, size_t size)
{
    if (ptr == NULL || size == 0)
        return false;

    void* p = mmap(ptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p != MAP_FAILED;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Unmaps the whole mapping created by vmem_reserve, guards included (size as passed there).
static inline void vmem_free(void* ptr, size_t size)
{
    if (ptr == NULL || size == 0)
        return;

    size_t page = (size_t)getpagesize();
    size_t rounded = (size + page - 1) & ~(page - 1);
    munmap((char*)ptr - page, rounded + page * 2);
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
// vmem_reserve of `size`; the whole arena is poisoned under ASan until allocated.
static inline bool linear_init(struct linear_allocator* a, size_t size)
{
    a->base = (uint8_t*)vmem_reserve(size);
    a->size = size;
    a->used = 0;
    a->peak = 0;
    if (a->base == NULL)
        return false;
    vmem__poison(a->base, size);
    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// 16-byte aligned (malloc's; the base is page-aligned). Returns NULL for size 0, for sizes
// whose round-up would overflow, or when the arena is full.
static inline void* linear_alloc(struct linear_allocator* a, size_t size)
{
    size_t req = size;
    if (a->base == NULL || req == 0 || req > SIZE_MAX - 15)   // bound so req + 15 cannot wrap
        return NULL;

    size = (req + 15) & ~(size_t)15;
    if (size > a->size - a->used)                             // used <= size always, no wrap
        return NULL;

    void* p = a->base + a->used;
    a->used += size;
    if (a->used > a->peak)
        a->peak = a->used;
    vmem__unpoison(p, req);    // exactly the requested bytes; the padding stays poisoned
    return p;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void linear_reset(struct linear_allocator* a)                // used = 0
{
    if (a->base != NULL && a->used > 0)
        vmem__poison(a->base, a->used);
    a->used = 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
// vmem_release of [0, peak), then used = peak = 0.
static inline void linear_release(struct linear_allocator* a)
{
    if (a->base != NULL && a->peak > 0)
    {
        vmem_release(a->base, a->peak);
        vmem__poison(a->base, a->peak);
    }
    a->used = 0;
    a->peak = 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void linear_free(struct linear_allocator* a)                 // vmem_free, zeroes the struct
{
    if (a->base != NULL)
    {
        vmem_free(a->base, a->size);
        a->base = NULL;
    }
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
// - canary: sits after the payload rounded up to 16 bytes, so an overrun of up to 15 bytes
//           past the requested size lands in the padding and is not detected by the canary
//           (under ASan the poisoned padding catches it instead). The stack is flagged when
//           the bad trailer is next read: at pop, top, the next alloc, rewind or check.
// - prev:   the used offset before this block was allocated; pop rewinds to it.
//
// Every block ends with its trailer, so the top block's trailer is always at used - 16:
// pop reads it directly, no size needed. If the canary fails or prev is invalid (not
// 16-byte aligned, or prev + 32 > used), the stack is marked corrupt and VMEM_ON_CORRUPT(s)
// is called (default: print + trap); every further operation re-reports via the hook and
// returns NULL rather than trusting the clobbered state.
struct stack_allocator
{
    uint8_t* base;
    size_t used;
    size_t peak;   // highest used since release
    size_t size;
    bool corrupt;  // a canary failed or a check failed; stored pointers can no longer be trusted
};

static const uint64_t stack_canary = 0xA5C3E1F07B9D2E4CULL;   // high entropy, no zero bytes

#ifndef VMEM_ON_CORRUPT
#define VMEM_ON_CORRUPT(s) vmem__on_corrupt(s)
#endif

//-----------------------------------------------------------------------------------------------------------------------------
static inline void vmem__on_corrupt(struct stack_allocator* s)
{
    (void)s;
    fprintf(stderr, "vmem: stack allocator corruption detected\n");
    __builtin_trap();
}

//-----------------------------------------------------------------------------------------------------------------------------
// vmem_reserve of `size`; the whole arena is poisoned under ASan until allocated.
static inline bool stack_init(struct stack_allocator* s, size_t size)
{
    s->base = (uint8_t*)vmem_reserve(size);
    s->size = size;
    s->used = 0;
    s->peak = 0;
    s->corrupt = false;
    if (s->base == NULL)
        return false;
    vmem__poison(s->base, size);
    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Reads and validates the trailer of the block that ends at offset u (canary at u - 16,
// prev at u - 8). The trailer is unpoisoned around the access and poisoned again. On an
// invalid canary or prev: sets corrupt, calls VMEM_ON_CORRUPT, returns false.
static inline bool stack__read_trailer(struct stack_allocator* s, size_t u, uint64_t* prev)
{
    uint8_t* t = s->base + u - 16;
    uint64_t canary;
    vmem__unpoison(t, 16);
    memcpy(&canary, t, 8);
    memcpy(prev, t + 8, 8);
    vmem__poison(t, 16);

    if (canary != stack_canary || (*prev & 15) != 0 || *prev > u - 32)   // prev + 32 <= u, wrap-free
    {
        s->corrupt = true;
        VMEM_ON_CORRUPT(s);
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// 16-byte aligned (malloc's); the block carries a 16-byte trailer after the rounded payload.
// Returns NULL for size 0, for overflowing sizes, when full, or when the stack is corrupt:
// the current top's trailer is checked before pushing, so an overrun of it is caught here.
static inline void* stack_alloc(struct stack_allocator* s, size_t size)
{
    size_t req = size;
    if (s->base == NULL)
        return NULL;
    if (s->corrupt)
    {
        VMEM_ON_CORRUPT(s);
        return NULL;
    }
    if (req == 0 || req > SIZE_MAX - 31)   // bound so the round-up plus the 16-byte trailer cannot wrap
        return NULL;

    if (s->used > 0)
    {
        uint64_t prev;
        if (!stack__read_trailer(s, s->used, &prev))
            return NULL;
    }

    size = (req + 15) & ~(size_t)15;
    if (size + 16 > s->size - s->used)
        return NULL;

    uint8_t* p = s->base + s->used;              // payload
    uint8_t* t = p + size;                       // trailer, right after the rounded payload
    uint64_t canary = stack_canary;
    uint64_t prev = s->used;                     // where used stood before this block
    vmem__unpoison(t, 16);
    memcpy(t, &canary, 8);
    memcpy(t + 8, &prev, 8);
    vmem__poison(t, 16);                         // metadata, not user memory
    vmem__unpoison(p, req);                      // exactly the requested bytes; padding stays poisoned

    s->used += size + 16;
    if (s->used > s->peak)
        s->peak = s->used;
    return (void*)p;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Pops the top block: `p` must be the pointer stack_alloc returned for it; a different
// pointer is an out-of-order free and fails loudly. Returns p on success, otherwise NULL
// and used is unchanged.
static inline void* stack_pop(struct stack_allocator* s, void* p)
{
    if (s->base == NULL)
        return NULL;
    if (s->corrupt)
    {
        VMEM_ON_CORRUPT(s);
        return NULL;
    }
    if (s->used == 0)
        return NULL;

    uint64_t prev;
    if (!stack__read_trailer(s, s->used, &prev))
        return NULL;

    if (p != (void*)(s->base + prev))   // not the top block's payload
    {
        s->corrupt = true;
        VMEM_ON_CORRUPT(s);
        return NULL;
    }

    size_t old = s->used;
    s->used = prev;
    vmem__poison((uint8_t*)p, old - prev);   // whole freed block: payload, padding, trailer
    return p;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Payload of the current top block, or NULL when empty, corrupt or on a failed check.
static inline void* stack_top(struct stack_allocator* s)
{
    if (s->base == NULL)
        return NULL;
    if (s->corrupt)
    {
        VMEM_ON_CORRUPT(s);
        return NULL;
    }
    if (s->used == 0)
        return NULL;

    uint64_t prev;
    if (!stack__read_trailer(s, s->used, &prev))
        return NULL;
    return (void*)(s->base + prev);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Returns used: a mark to hand back to stack_rewind.
static inline size_t stack_mark(struct stack_allocator* s)
{
    return s->used;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Rewinds used to mark m: m must be <= used and a real block boundary, reachable by
// following the prev chain from the top (0 always is). On failure VMEM_ON_CORRUPT(s) is
// called, the stack is marked corrupt, and used is unchanged.
static inline void stack_rewind(struct stack_allocator* s, size_t m)
{
    if (s->base == NULL)
        return;
    if (s->corrupt)
    {
        VMEM_ON_CORRUPT(s);
        return;
    }
    if (m > s->used)
    {
        s->corrupt = true;
        VMEM_ON_CORRUPT(s);
        return;
    }

    size_t u = s->used;
    while (u > 0 && u != m)
    {
        uint64_t prev;
        if (!stack__read_trailer(s, u, &prev))
            return;
        u = (size_t)prev;
    }
    if (u != m)   // walked to the bottom without finding m: not a block boundary
    {
        s->corrupt = true;
        VMEM_ON_CORRUPT(s);
        return;
    }

    vmem__poison(s->base + m, s->used - m);   // discarded blocks
    s->used = m;
}

//-----------------------------------------------------------------------------------------------------------------------------
// O(n) walk of the prev chain from top to bottom, validating every trailer. Returns false
// on the first invalid one (which also calls VMEM_ON_CORRUPT and sets corrupt).
static inline bool stack_check(struct stack_allocator* s)
{
    if (s->base == NULL || s->corrupt)
        return false;

    size_t u = s->used;
    while (u > 0)
    {
        uint64_t prev;
        if (!stack__read_trailer(s, u, &prev))
            return false;
        u = (size_t)prev;
    }
    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// used = 0, corrupt = false; the freed range is re-poisoned under ASan.
static inline void stack_reset(struct stack_allocator* s)
{
    if (s->base != NULL && s->used > 0)
        vmem__poison(s->base, s->used);
    s->used = 0;
    s->corrupt = false;
}

//-----------------------------------------------------------------------------------------------------------------------------
// vmem_release of [0, peak), then used = peak = 0, corrupt = false.
static inline void stack_release(struct stack_allocator* s)
{
    if (s->base != NULL && s->peak > 0)
    {
        vmem_release(s->base, s->peak);
        vmem__poison(s->base, s->peak);
    }
    s->used = 0;
    s->peak = 0;
    s->corrupt = false;
}

//-----------------------------------------------------------------------------------------------------------------------------
// vmem_free, zeroes the struct.
static inline void stack_free(struct stack_allocator* s)
{
    if (s->base != NULL)
    {
        vmem_free(s->base, s->size);
        s->base = NULL;
    }
    s->size = 0;
    s->used = 0;
    s->peak = 0;
    s->corrupt = false;
}

#endif // VMEM_H
