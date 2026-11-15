/*
 * obs.h — Observabilidad y calidad (F6).
 *
 *   OB-1: trazas de baja sobrecarga vía `trace_marker` (ftrace/user_events).
 *   OB-2: contadores atómicos de runtime consultables en vivo.
 *   OB-4: autotest de colas lock-free (detección de pérdidas y duplicados).
 */
#ifndef OBS_H
#define OBS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "libtuxwarp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- OB-2 --- */

typedef struct {
    _Atomic uint64_t published;
    _Atomic uint64_t consumed;
    _Atomic uint64_t dropped;
    _Atomic uint64_t errors;
    _Atomic uint64_t forwarded;
} counters_t;

API void     counters_init(counters_t* c);
API void     counters_add(counters_t* c, _Atomic uint64_t* field, uint64_t delta);
API void     counters_reset(counters_t* c);
API void     counters_print(const counters_t* c, const char* label);

/* ---------------------------------------------------------------- OB-1 --- */

typedef struct {
    int  fd;
    bool active;
} trace_t;

/* Abre el `trace_marker` de ftrace si el kernel/permiso lo permite. */
API status_t trace_init(trace_t* t);
/* Escribe una marca (no-op si no hay traza activa). Devuelve bytes escritos. */
API int                trace_mark(trace_t* t, const char* msg);
API void               trace_destroy(trace_t* t);

/* ---------------------------------------------------------------- OB-4 --- */

typedef struct {
    uint64_t produced;
    uint64_t consumed;
    uint64_t duplicates;   /* elementos recibidos más de una vez */
    uint64_t lost;         /* elementos nunca recibidos */
} lockfree_stats_t;

/* Fuzzing concurrente de la cola acotada: `producers`×`consumers` sobre
 * `total` elementos únicos. Valida ausencia de pérdidas y duplicados. */
API status_t selftest_bounded_queue(uint32_t producers,
                                                                 uint32_t consumers,
                                                                 uint64_t total,
                                                                 lockfree_stats_t* out);

/* Igual para el ring SPSC (1 productor, 1 consumidor). */
API status_t selftest_ring(uint64_t total,
                                                         lockfree_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OBS_H */
