/*
 * rt.h — Instrumentación de tiempo real (F3).
 *
 *   RT-6: histograma de latencia para publicar WCET/percentiles MEDIDOS.
 *   RS-4: watchdog software con heartbeat (y detección de watchdog HW).
 *   RT-8: fuente de tick periódico por `timerfd` (sin busy-wait).
 */
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "libtuxwarp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- RT-6 --- */

#define HIST_BUCKETS 64

typedef struct {
    uint64_t bucket_ns;                       /* ancho de bucket en ns */
    uint64_t buckets[HIST_BUCKETS];
    uint64_t prefix_sum[HIST_BUCKETS]; /* prefix-sum incremental (B-7) */
    uint64_t count;
    uint64_t sum_ns;
    uint64_t sum_sq_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t overflow;                        /* muestras por encima del rango */
    bool     prefix_sum_valid;               /* cache válida para búsqueda O(log n) */
    /* Recíproco para dividir por `bucket_ns` sin `idiv` (I+D+i: el registro
     * ocurre una vez por muestra y el divisor es variable en runtime). */
    uint64_t bucket_recip;
} hist_t;

API void     hist_init(hist_t* h, uint64_t bucket_ns);
API void     hist_reset(hist_t* h);
API void     hist_record(hist_t* h, uint64_t latency_ns);
API uint64_t hist_count(const hist_t* h);
API uint64_t hist_mean_ns(const hist_t* h);
API uint64_t hist_stddev_ns(const hist_t* h);
API uint64_t hist_percentile_ns(hist_t* h, double p);
API uint64_t hist_wcet_ns(const hist_t* h);
API uint64_t hist_best_ns(const hist_t* h);
API void     hist_print(const hist_t* h, const char* label);

/* ---------------------------------------------------------------- RS-4 --- */

typedef struct {
    _Atomic uint64_t last_kick_ns;
    uint64_t         timeout_ns;
} watchdog_t;

API status_t watchdog_init(watchdog_t* wd, uint64_t timeout_ns);
API void               watchdog_kick(watchdog_t* wd);
API bool               watchdog_expired(const watchdog_t* wd);
API uint64_t           watchdog_elapsed_ns(const watchdog_t* wd);
API void               watchdog_destroy(watchdog_t* wd);
/* ¿Existe /dev/watchdog accesible? (no se abre para no armar el temporizador). */
API bool               watchdog_hw_available(void);

/* ---------------------------------------------------------------- RT-8 --- */

typedef struct {
    int      fd;
    uint64_t period_ns;
} tick_t;

API status_t tick_init(tick_t* tick, uint64_t period_ns);
API int                tick_fd(const tick_t* tick);
/* Bloquea hasta el siguiente tick. Devuelve nº de expiraciones o -1 en error. */
API int                tick_wait(tick_t* tick);
API void               tick_destroy(tick_t* tick);

#ifdef __cplusplus
}
#endif

#endif /* RT_H */
