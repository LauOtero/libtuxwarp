/*
 * obs.c — Contadores (OB-2), trazas (OB-1) y autotest lock-free (OB-4).
 *
 * Define _GNU_SOURCE para asegurar disponibilidad de funciones POSIX/GNU
 * como CPU_ZERO, CPU_SET, sched_getcpu, etc.
 */

#include "obs.h"
#include "bounded.h"
#include "ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>

/* ------------------------------------------------------------------------ */
/* OB-2: contadores                                                         */
/* ------------------------------------------------------------------------ */

void counters_init(counters_t* c) {
    if (!c) {
        return;
    }
    atomic_init(&c->published, 0);
    atomic_init(&c->consumed, 0);
    atomic_init(&c->dropped, 0);
    atomic_init(&c->errors, 0);
    atomic_init(&c->forwarded, 0);
}

void counters_add(counters_t* c, _Atomic uint64_t* field, uint64_t delta) {
    if (!c || !field) {
        return;
    }
    atomic_fetch_add_explicit(field, delta, memory_order_relaxed);
}

void counters_reset(counters_t* c) {
    if (!c) {
        return;
    }
    atomic_store_explicit(&c->published, 0, memory_order_relaxed);
    atomic_store_explicit(&c->consumed, 0, memory_order_relaxed);
    atomic_store_explicit(&c->dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&c->errors, 0, memory_order_relaxed);
    atomic_store_explicit(&c->forwarded, 0, memory_order_relaxed);
}

void counters_print(const counters_t* c, const char* label) {
    if (!c) {
        return;
    }
    printf("[libtuxwarp] %s: publicados=%llu consumidos=%llu reenviados=%llu "
           "descartados=%llu errores=%llu\n",
           label ? label : "contadores",
           (unsigned long long)atomic_load_explicit(&c->published, memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&c->consumed, memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&c->forwarded, memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&c->dropped, memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&c->errors, memory_order_relaxed));
}

/* ------------------------------------------------------------------------ */
/* OB-1: trazas                                                            */
/* ------------------------------------------------------------------------ */

static int trace_open_marker(void) {
    static const char* paths[] = {
        "/sys/kernel/tracing/trace_marker",
        "/sys/kernel/debug/tracing/trace_marker",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

status_t trace_init(trace_t* t) {
    if (!t) {
        return ERROR_INVALID;
    }
    t->fd = trace_open_marker();
    t->active = (t->fd >= 0);
    /* La ausencia de traza no es un error: la observabilidad es best-effort. */
    return t->active ? SUCCESS : ERROR_NOTSUP;
}

int trace_mark(trace_t* t, const char* msg) {
    if (!t || !t->active || t->fd < 0 || !msg) {
        return 0;
    }
    size_t len = strlen(msg);
    ssize_t n = write(t->fd, msg, len);
    return (n > 0) ? (int)n : 0;
}

void trace_destroy(trace_t* t) {
    if (!t) {
        return;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    t->active = false;
}

/* ------------------------------------------------------------------------ */
/* OB-4: autotest de colas lock-free                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
    bounded_queue_t* q;
    _Atomic uint64_t  next;          /* siguiente índice a producir */
    _Atomic uint64_t  produced;
    _Atomic uint64_t  consumed;
    _Atomic uint64_t  duplicates;
    _Atomic uint32_t  producers_done;
    uint32_t          expected_producers;
    uint64_t          total;
    _Atomic uint8_t*  seen;
} bounded_test_ctx_t;

static void* bounded_producer(void* arg) {
    bounded_test_ctx_t* ctx = (bounded_test_ctx_t*)arg;
    for (;;) {
        uint64_t idx = atomic_fetch_add_explicit(&ctx->next, 1, memory_order_relaxed);
        if (idx >= ctx->total) {
            break;
        }
        void* payload = (void*)(uintptr_t)(idx + 1);
        while (!bounded_push(ctx->q, payload)) {
            sched_yield();
        }
        atomic_fetch_add_explicit(&ctx->produced, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&ctx->producers_done, 1, memory_order_release);
    return NULL;
}

static void* bounded_consumer(void* arg) {
    bounded_test_ctx_t* ctx = (bounded_test_ctx_t*)arg;
    for (;;) {
        if (atomic_load_explicit(&ctx->consumed, memory_order_relaxed) >= ctx->total) {
            break;
        }
        void* out = NULL;
        if (!bounded_pop(ctx->q, &out)) {
            if (atomic_load_explicit(&ctx->producers_done, memory_order_acquire) >=
                    ctx->expected_producers &&
                bounded_empty(ctx->q)) {
                break;
            }
            sched_yield();
            continue;
        }
        uint64_t value = (uint64_t)(uintptr_t)out;
        if (value >= 1 && value <= ctx->total) {
            if (atomic_exchange_explicit(&ctx->seen[value - 1], 1, memory_order_acq_rel)) {
                atomic_fetch_add_explicit(&ctx->duplicates, 1, memory_order_relaxed);
            }
        }
        atomic_fetch_add_explicit(&ctx->consumed, 1, memory_order_relaxed);
    }
    return NULL;
}

status_t selftest_bounded_queue(uint32_t producers,
                                                    uint32_t consumers,
                                                    uint64_t total,
                                                    lockfree_stats_t* out) {
    if (producers == 0 || consumers == 0 || total == 0 || !out) {
        return ERROR_INVALID;
    }
    if (producers > 64 || consumers > 64) {
        return ERROR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    bounded_queue_t q;
    if (bounded_init(&q, 1024) != SUCCESS) {
        return ERROR_MEMORY;
    }

    bounded_test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.q = &q;
    ctx.total = total;
    ctx.expected_producers = producers;
    atomic_init(&ctx.next, 0);
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.duplicates, 0);
    atomic_init(&ctx.producers_done, 0);

    ctx.seen = (_Atomic uint8_t*)calloc((size_t)total, sizeof(_Atomic uint8_t));
    if (!ctx.seen) {
        bounded_destroy(&q);
        return ERROR_MEMORY;
    }

    pthread_t prod[64], cons[64];
    for (uint32_t i = 0; i < producers; i++) {
        pthread_create(&prod[i], NULL, bounded_producer, &ctx);
    }
    for (uint32_t i = 0; i < consumers; i++) {
        pthread_create(&cons[i], NULL, bounded_consumer, &ctx);
    }
    for (uint32_t i = 0; i < producers; i++) {
        pthread_join(prod[i], NULL);
    }
    for (uint32_t i = 0; i < consumers; i++) {
        pthread_join(cons[i], NULL);
    }

    out->produced   = atomic_load_explicit(&ctx.produced, memory_order_relaxed);
    out->consumed   = atomic_load_explicit(&ctx.consumed, memory_order_relaxed);
    out->duplicates = atomic_load_explicit(&ctx.duplicates, memory_order_relaxed);

    uint64_t got = 0;
    for (uint64_t i = 0; i < total; i++) {
        got += ctx.seen[i] ? 1u : 0u;
    }
    out->lost = (total > got) ? (total - got) : 0;

    free((void*)ctx.seen);
    bounded_destroy(&q);

    return (out->lost == 0 && out->duplicates == 0) ? SUCCESS
                                                    : ERROR_CORRUPT;
}

/* --- ring SPSC --- */

typedef struct {
    ring_t* ring;
    uint64_t          total;
    _Atomic uint64_t  consumed;
    _Atomic uint64_t  duplicates;
    _Atomic bool      producer_done;
    _Atomic uint8_t*  seen;
} ring_test_ctx_t;

static void* ring_producer(void* arg) {
    ring_test_ctx_t* ctx = (ring_test_ctx_t*)arg;
    for (uint64_t i = 0; i < ctx->total; i++) {
        void* payload = (void*)(uintptr_t)(i + 1);
        while (!ring_push(ctx->ring, payload)) {
            sched_yield();
        }
    }
    atomic_store_explicit(&ctx->producer_done, true, memory_order_release);
    return NULL;
}

static void* ring_consumer(void* arg) {
    ring_test_ctx_t* ctx = (ring_test_ctx_t*)arg;
    for (;;) {
        void* out = ring_pop(ctx->ring);
        if (!out) {
            if (atomic_load_explicit(&ctx->producer_done, memory_order_acquire) &&
                ring_is_empty(ctx->ring)) {
                break;
            }
            sched_yield();
            continue;
        }
        uint64_t value = (uint64_t)(uintptr_t)out;
        if (value >= 1 && value <= ctx->total) {
            if (atomic_exchange_explicit(&ctx->seen[value - 1], 1, memory_order_acq_rel)) {
                atomic_fetch_add_explicit(&ctx->duplicates, 1, memory_order_relaxed);
            }
        }
        atomic_fetch_add_explicit(&ctx->consumed, 1, memory_order_relaxed);
    }
    return NULL;
}

status_t selftest_ring(uint64_t total, lockfree_stats_t* out) {
    if (total == 0 || !out) {
        return ERROR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    ring_t* ring = (ring_t*)aligned_alloc(CACHELINE,
                                                              sizeof(ring_t));
    if (!ring) {
        return ERROR_MEMORY;
    }
    ring_init(ring);

    ring_test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ring = ring;
    ctx.total = total;
    atomic_init(&ctx.consumed, 0);
    atomic_init(&ctx.duplicates, 0);
    atomic_init(&ctx.producer_done, false);

    ctx.seen = (_Atomic uint8_t*)calloc((size_t)total, sizeof(_Atomic uint8_t));
    if (!ctx.seen) {
        free(ring);
        return ERROR_MEMORY;
    }

    pthread_t p, c;
    pthread_create(&p, NULL, ring_producer, &ctx);
    pthread_create(&c, NULL, ring_consumer, &ctx);
    pthread_join(p, NULL);
    pthread_join(c, NULL);

    out->produced   = total;
    out->consumed   = atomic_load_explicit(&ctx.consumed, memory_order_relaxed);
    out->duplicates = atomic_load_explicit(&ctx.duplicates, memory_order_relaxed);

    uint64_t got = 0;
    for (uint64_t i = 0; i < total; i++) {
        got += ctx.seen[i] ? 1u : 0u;
    }
    out->lost = (total > got) ? (total - got) : 0;

    free((void*)ctx.seen);
    free(ring);

    return (out->lost == 0 && out->duplicates == 0) ? SUCCESS
                                                    : ERROR_CORRUPT;
}
