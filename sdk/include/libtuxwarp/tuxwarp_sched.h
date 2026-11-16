#ifndef TUXWARDSCHED_H
#define TUXWARDSCHED_H

#include "libtuxwarp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- RT-1 --- */
/* Política de planificación de tiempo real con presupuesto de admisión. */

typedef struct {
    uint64_t runtime_ns;   /* presupuesto de ejecución por periodo */
    uint64_t deadline_ns;  /* deadline relativo al periodo */
    uint64_t period_ns;    /* periodo */
    int      cpu;          /* -1 = sin fijación de afinidad */
} deadline_attr_t;

/* ¿El kernel soporta SCHED_DEADLINE? */
API bool sched_deadline_supported(void);

/* Aplica SCHED_DEADLINE al hilo actual (admisión garantizada por el kernel).
 * Devuelve ERROR_NOTSUP si el kernel lo rechaza (p. ej. sin -rt). */
API status_t sched_set_deadline_current(const deadline_attr_t* attr);

/* Aplica SCHED_FIFO con prioridad [1..99] al hilo actual. */
API status_t sched_set_fifo_current(int priority);

/* Fija la afinidad del hilo actual a `cpu`. */
API status_t sched_set_affinity_current(int cpu);

/* ---------------------------------------------------------------- RT-4 --- */
/* Aislamiento de CPU verificado (isolcpus / nohz_full / rcu_nocbs). */

typedef struct {
    bool     isolcpus;
    bool     nohz_full;
    bool     rcu_nocbs;
    uint32_t num_isolated;
    int      isolated[MAX_CPUS];
    bool     strict_ok;   /* los tres tokens presentes y con CPUs válidas */
} isolation_t;

API status_t sched_isolation_probe(isolation_t* out);
API bool sched_cpu_is_isolated(const isolation_t* iso, int cpu);

/* Imprime por stdout el estado de aislamiento y avisa por núcleo no aislado. */
API void sched_isolation_print(const isolation_t* iso);

/* Rango de CPUs disponibles en el sistema. */
API int sched_online_cpus(void);

#ifdef __cplusplus
}
#endif

#endif