#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "abi.h"

#define RING_CAPACITY 4096
#define RING_MASK (RING_CAPACITY - 1)

/*
 * Ring SPSC lock-free de verdad.
 *
 * Corrección respecto a la versión anterior: se elimina el contador compartido
 * `count` (dos `fetch_add`/`fetch_sub` por operación sobre la MISMA línea de
 * caché). Productor y consumidor solo escriben su propio índice:
 *
 *   - el productor escribe `head` y lee `tail` (cached para no rebotar la línea),
 *   - el consumidor escribe `tail` y lee `head`.
 *
 * No hay ningún RMW atómico ni línea compartida escrita por ambos lados, que es
 * la propiedad que hace que un SPSC escale a decenas de millones de ops/s.
 */
#if defined(__x86_64__) || defined(__i386__)
  #include <emmintrin.h>
  #define ring_prefetch(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#elif defined(__aarch64__)
  #define ring_prefetch(p) __builtin_prefetch((p), 1, 3)
#else
  #define ring_prefetch(p) ((void)(p))
#endif

typedef struct ring {
    _Alignas(CACHELINE) _Atomic uint32_t head;   /* solo productor */
    _Alignas(CACHELINE) _Atomic uint32_t tail;   /* solo consumidor */
    _Alignas(CACHELINE) void* buffer[RING_CAPACITY];
} ring_t;

static inline void ring_init(ring_t* ring) {
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    for (int i = 0; i < RING_CAPACITY; i++) {
        ring->buffer[i] = NULL;
    }
}

static inline bool ring_push(ring_t* ring, void* item) {
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if ((uint32_t)(head - tail) >= RING_CAPACITY) {
        return false;   /* llena */
    }

    ring_prefetch(&ring->buffer[(head + 1u) & RING_MASK]);
    ring->buffer[head & RING_MASK] = item;
    atomic_store_explicit(&ring->head, head + 1u, memory_order_release);
    return true;
}

static inline void* ring_pop(ring_t* ring) {
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head) {
        return NULL;    /* vacío */
    }

    ring_prefetch(&ring->buffer[(tail + 1u) & RING_MASK]);
    void* item = ring->buffer[tail & RING_MASK];
    atomic_store_explicit(&ring->tail, tail + 1u, memory_order_release);
    return item;
}

/* Instantánea aproximada (los índices avanzan por separado). */
static inline uint32_t ring_count(ring_t* ring) {
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    return head - tail;
}

static inline bool ring_is_empty(ring_t* ring) {
    return atomic_load_explicit(&ring->head, memory_order_acquire) ==
           atomic_load_explicit(&ring->tail, memory_order_acquire);
}

static inline bool ring_is_full(ring_t* ring) {
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return (uint32_t)(head - tail) >= RING_CAPACITY;
}

#endif
