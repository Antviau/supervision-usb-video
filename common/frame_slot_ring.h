#ifndef SUPERVISION_FRAME_SLOT_RING_H
#define SUPERVISION_FRAME_SLOT_RING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<uint32_t> sv_ring_index_t;
#define SV_RING_LOAD_RELAXED(p) ((p)->load(std::memory_order_relaxed))
#define SV_RING_LOAD_ACQUIRE(p) ((p)->load(std::memory_order_acquire))
#define SV_RING_STORE_RELAXED(p, v) ((p)->store((v), std::memory_order_relaxed))
#define SV_RING_STORE_RELEASE(p, v) ((p)->store((v), std::memory_order_release))
#else
#include <stdatomic.h>
typedef _Atomic uint32_t sv_ring_index_t;
#define SV_RING_LOAD_RELAXED(p) atomic_load_explicit((p), memory_order_relaxed)
#define SV_RING_LOAD_ACQUIRE(p) atomic_load_explicit((p), memory_order_acquire)
#define SV_RING_STORE_RELAXED(p, v) atomic_store_explicit((p), (v), memory_order_relaxed)
#define SV_RING_STORE_RELEASE(p, v) atomic_store_explicit((p), (v), memory_order_release)
#endif

// Exactly one producer and one consumer per ring. Eight usable slots, with
// a ninth ring cell distinguishing full from empty. Acquire/release publishes
// the associated frame payload and metadata, not only the slot identifier.
#define SV_SLOT_COUNT 8u
typedef struct {
    sv_ring_index_t read_index;
    sv_ring_index_t write_index;
    uint8_t ids[SV_SLOT_COUNT + 1u];
} sv_slot_ring_t;

static inline void sv_slot_ring_init(sv_slot_ring_t *q) {
    SV_RING_STORE_RELAXED(&q->read_index, 0);
    SV_RING_STORE_RELAXED(&q->write_index, 0);
}

static inline uint32_t sv_slot_ring_next(uint32_t index) {
    return index == SV_SLOT_COUNT ? 0u : index + 1u;
}

static inline bool sv_slot_ring_push(sv_slot_ring_t *q, uint8_t id) {
    const uint32_t write = SV_RING_LOAD_RELAXED(&q->write_index);
    const uint32_t next = sv_slot_ring_next(write);
    if (next == SV_RING_LOAD_ACQUIRE(&q->read_index)) return false;
    q->ids[write] = id;
    SV_RING_STORE_RELEASE(&q->write_index, next);
    return true;
}

static inline bool sv_slot_ring_pop(sv_slot_ring_t *q, uint8_t *id) {
    const uint32_t read = SV_RING_LOAD_RELAXED(&q->read_index);
    if (read == SV_RING_LOAD_ACQUIRE(&q->write_index)) return false;
    *id = q->ids[read];
    SV_RING_STORE_RELEASE(&q->read_index, sv_slot_ring_next(read));
    return true;
}

#endif
