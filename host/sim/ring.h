/* SPDX-License-Identifier: GPL-3.0-only */
/* Lock-free hand-overs between the simulator's threads (GCC / Clang __atomic builtins, C99).
 *
 * SPSC ring: one producer, one consumer, free-running 32-bit indices (fill = w - r, exact across the wrap),
 * a power-of-two capacity. The producer writes its slots, then publishes w with release; the consumer reads
 * w with acquire, takes the slots, then publishes r with release. Neither side ever waits or locks.
 *
 * Seqlock: the writer makes seq odd, writes, makes it even again; a reader copies between two equal even
 * reads of seq and tries again otherwise. The writer never waits; the snapshot keeps two slots so the one a
 * reader copies is not rewritten until the next publication (sim.h). */
#pragma once
#include <stdint.h>
#include <string.h>

#define AT_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define AT_LOAD_RELAXED(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define AT_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define AT_STORE_RELAXED(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define AT_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_ACQ_REL)
#define CACHE_LINE 128                   /* (Apple silicon) */

typedef struct {
    uint32_t seq;
} seqlock_t;
static inline void seq_write_begin(seqlock_t *s)
{
    AT_STORE_RELAXED(&s->seq, s->seq + 1u);   /* odd: being written */
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static inline void seq_write_end(seqlock_t *s) { AT_STORE(&s->seq, s->seq + 1u); }
/* copy n bytes of src (guarded by s) into dst; returns the (even) sequence the copy belongs to */
static inline uint32_t seq_read(const seqlock_t *s, void *dst, const void *src, size_t n)
{
    for (;;) {
        uint32_t a = AT_LOAD(&s->seq), b;
        if (a & 1u)
            continue;
        memcpy(dst, src, n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = AT_LOAD_RELAXED(&s->seq);
        if (a == b)
            return a;
    }
}
