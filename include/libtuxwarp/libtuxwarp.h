#ifndef LIBTUXWARP_LIBTUXWARP_H
#define LIBTUXWARP_LIBTUXWARP_H

/* CRÍTICO: _GNU_SOURCE debe estar definido ANTES de cualquier header del sistema
 * para que cpu_set_t, CPU_ZERO/CPU_SET, sched_getcpu, etc. estén disponibles. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Headers básicos C necesarios antes de cualquier uso de tipos */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* abi.h define API, VERSION_STRING, etc. */
#include "abi.h"

/* Definiciones de tipos básicos que deben estar antes de <sched.h> para evitar
 * errores de tipo desconocido cuando tuxwarp_sched.h incluye libtuxwarp.h */
#define MAX_CPUS 16

typedef enum {
    SUCCESS = 0,
    ERROR_INIT = -1,
    ERROR_MEMORY = -2,
    ERROR_CPU = -3,
    ERROR_TIMEOUT = -4,
    ERROR_INVALID = -5,
    ERROR_NO_HARDWARE = -6,
    ERROR_NOTSUP = -7,     /* operación no soportada en este kernel/HW */
    ERROR_STATE = -8,      /* estado inválido para la operación */
    ERROR_IO = -9,         /* error de E/S */
    ERROR_CORRUPT = -10,   /* corrupción de integridad (CRC/ECC) */
    ERROR_FULL = -11,      /* recurso lleno (cola/ring) */
    ERROR_BACKPRESSURE = -12, /* el productor debe reducir la tasa   */
    ERROR_DROPPED = -13,      /* descartada por política best-effort */
    ERROR_NOTFOUND = -14      /* nodo/flujo/canal no registrado      */
} status_t;

typedef enum {
    MODE_LATENCY,
    MODE_THROUGHPUT,
    MODE_JITTER,
    MODE_STRESS,
    MODE_WCET
} benchmark_mode_t;

typedef enum {
    PLATFORM_RPI4,
    PLATFORM_RPI5,
    PLATFORM_JETSON_NANO,
    PLATFORM_JETSON_ORIN,
    PLATFORM_INTEL_NUC,
    PLATFORM_ORANGE_PI5,
    PLATFORM_ROCK5B,
    PLATFORM_X86_64_RT,
    PLATFORM_GENERIC
} platform_t;

/* Version y macros */
#define VERSION VERSION_STRING
#define MAX_EVENTS 1048576

/* Arquitectura específica */
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #define ARCH_X86_64 1
    #define CACHE_LINE 64
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define ARCH_ARM64 1
    #define CACHE_LINE 64
#else
    #define ARCH_GENERIC 1
    #define CACHE_LINE 64
#endif

/* Alineación de línea de caché */
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE)))

/* Para C++, los headers de C deben ir dentro de extern "C" */
#ifdef __cplusplus
extern "C" {
#endif

/* Ahora incluimos <sched.h> y <pthread.h> que necesitan los tipos definidos arriba */
#include <sched.h>
#include <pthread.h>

/* Tipos de datos adicionales que dependen de stdatomic.h */
typedef struct CACHE_ALIGNED {
    volatile uint64_t sequence;
    volatile uint64_t timestamp_ns;
    volatile uint32_t cpu_id;
    volatile uint8_t  data[56];
} event_t;

typedef struct CACHE_ALIGNED {
    uint64_t total_events;
    uint64_t total_cycles;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    uint64_t avg_latency_ns;
    uint64_t stddev_ns;
    double   cpu_usage_percent;
    double   cache_miss_rate;
    uint64_t memory_bandwidth_mbps;
    uint64_t wcet_ns;
    uint64_t best_case_ns;
    /* Percentiles de la ventana medida (0 si el modo no los calcula). */
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    /* Muestras que superan el objetivo de 10 µs (cola del histograma). */
    uint64_t latency_spikes;
} metrics_t;

typedef struct {
    /* Lista explícita de CPUs para los hilos de tick. `num_cpus == 0` (valor
     * por defecto) NO crea hilos: el runtime es puramente pull, y un hilo
     * durmiente por núcleo solo añadiría ruido a los núcleos aislados. */
    uint32_t enabled_cpus[MAX_CPUS];
    uint32_t num_cpus;
    uint32_t event_pool_size;
    uint32_t batch_size;
    uint32_t stack_size_kb;
    bool     use_hugepages;
    bool     use_numa_aware;
    bool     use_simd;
    bool     use_cpu_affinity;
    platform_t platform;

    /* --- Extensiones ABI v1 (fases F2–F7 del INFORME_IDi) --- */
    bool     lock_memory;          /* RT-3: mlockall + prefault */
    bool     use_hugepages_2mb;    /* RT-3: MADV_HUGEPAGE en el pool */
    bool     strict_isolation;     /* RT-4: exigir isolcpus/nohz_full/rcu_nocbs */
    bool     use_deadline;         /* RT-1: SCHED_DEADLINE para hilos RT */
    bool     use_bounded_mpmc;     /* PF-3: cola bounded Vyukov (defecto: MPMC M-S) */
    bool     use_timerfd;          /* RT-8: worker con timerfd en vez de usleep */
    uint32_t rt_priority;          /* RT-1/RT-8: prioridad SCHED_FIFO (0 = auto) */
    uint64_t deadline_runtime_ns;  /* RT-1: presupuesto de ejecución */
    uint64_t deadline_period_ns;   /* RT-1: periodo */
} config_t;

typedef struct ring ring_t;
typedef struct mpmc_queue mpmc_queue_t;
typedef struct mem_pool memory_pool_t;

typedef void* handle_t;

/* Objetivo de latencia del proyecto: por encima de este umbral la muestra se
 * contabiliza como pico (`latency_spikes`). */
#define LATENCY_TARGET_NS 10000ULL

/* --- Declaraciones de API (después de todas las definiciones de tipos) --- */
API status_t init(handle_t* handle, const config_t* config);
API status_t tuxwarp_shutdown(handle_t handle);
API status_t event_publish(handle_t handle, const void* data, uint32_t size);
API status_t event_subscribe(handle_t handle, event_t* event, uint32_t timeout_us);
API status_t benchmark_run(handle_t handle, benchmark_mode_t mode, uint32_t duration_sec, metrics_t* metrics);
API status_t benchmark_run_ex(handle_t handle, benchmark_mode_t mode, uint32_t duration_sec, uint32_t threads, metrics_t* metrics);
API status_t metrics_report(const metrics_t* metrics);
API uint64_t get_cycles(void);
API uint64_t get_timestamp_ns(void);
API platform_t detect_platform(void);
API const char* abi_version_string(void);
API uint32_t    abi_version(void);

#ifdef __cplusplus
}
#endif

#endif