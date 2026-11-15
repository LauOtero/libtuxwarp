/* Define _GNU_SOURCE para asegurar disponibilidad de funciones POSIX/GNU
 * como cpu_set_t, CPU_ZERO, CPU_SET, sched_getcpu, etc.
 * Debe estar antes de cualquier include de cabeceras de sistema. */

/* Original header comment continues...
 *
 * Implementa las fases F2 (determinismo) y F3 (tiempo real) del INFORME_IDi:
 *   - B-6/RT-2: event_publish NO asigna memoria: el camino por defecto escribe
 *     directamente en una ranura del anillo (cero pool, cero punteros).
 *   - RT-3: mlockall + prefault + hugepages opcionales (sin page faults).
 *   - RT-4: sondeo de aislamiento de CPU y aviso si es incompleto.
 *   - RT-8: los hilos de tick esperan en un timerfd (o clock_nanosleep),
 *     con un eventfd de parada para que shutdown nunca se cuelgue.
 *   - RT-1: política SCHED_DEADLINE con presupuesto, o SCHED_FIFO.
 *   - PF-3: cola MPMC acotada (Vyukov) opcional frente al anillo SPSC.
 *   - B-9: futex como *eventcount* (secuencia monótona), sin pérdida de despertar.
 *
 * Correcciones de esta revisión (i+d+i):
 *   B-1  el futex ya no pierde despertares: el productor incrementa la secuencia
 *        y solo despierta si hay waiters; el consumidor espera con el valor
 *        observado y un plazo ABSOLUTO.
 *   B-2  no se crean hilos de tick salvo petición explícita (num_cpus > 0) y
 *        nunca sobre CPUs aisladas; el tamaño de pila del config se aplica.
 *   B-3  shutdown señala un eventfd de parada: adiós al cuelgue.
 *   B-4  ARM lee cntvct_el0 (sin syscall) en lugar de read(perf_fd).
 *   B-8  una carga útil mayor que el evento devuelve error, no se trunca.
 *   B-9: sequence es un contador global monótono, no basura reciclada.
 *   I-1  anillo de ranuras de evento en línea (estilo DPDK): un solo acceso.
 */
#include "libtuxwarp.h"
#include "ring.h"
#include "bounded.h"
#include "mem.h"
#include "simd.h"
#include "kernel.h"
#include "tuxwarp_sched.h"
#include "hwaccel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#if defined(__x86_64__)
    #include <immintrin.h>
#endif

#define DEFAULT_EVENT_POOL_SIZE 65536
#define DEFAULT_STACK_SIZE_KB 256
#define DEFAULT_TICK_NS 1000000ULL      /* 1 ms */
#define DEFAULT_DEADLINE_RUNTIME_NS 200000ULL
#define DEFAULT_RT_PRIORITY 10

/* Ranuras del anillo de eventos (potencia de dos). 512 × 128 B = 64 KiB. */
#define EVENT_RING_SLOTS 512u
#define EVENT_RING_MASK  (EVENT_RING_SLOTS - 1u)

typedef struct worker_arg {
    struct core* core;
    int cpu;
} worker_arg_t;

/* Asignación alineada portátil (memalign no está declarada en <stdlib.h>). */
static void* portable_aligned_alloc(size_t align, size_t size) {
    void* p = NULL;
    if (posix_memalign(&p, align, size) != 0) {
        return NULL;
    }
    return p;
}

typedef struct core {
    config_t config;

    /* --- Camino rápido por defecto: anillo SPSC de ranuras en línea (I-1) --- */
    event_t*            slot_ring;
    _Alignas(64) _Atomic uint32_t ring_head;    /* solo productor */
    _Alignas(64) _Atomic uint32_t ring_tail;    /* solo consumidor */

    /* --- Eventcount (B-1): secuencia monótona + waiters --- */
    _Alignas(64) _Atomic uint32_t event_seq;
    _Atomic uint32_t              waiters;

    /* --- Camino MPMC opcional: pool + cola acotada (PF-3) --- */
    bounded_queue_t     bounded;
    bool                          use_bounded;
    mem_pool_t*         pool;

    /* --- Hilos de tick (solo si el usuario los pide) --- */
    pthread_t                     worker_threads[MAX_CPUS];
    worker_arg_t        worker_args[MAX_CPUS];
    uint32_t                      num_workers;
    int                           stop_fd;      /* eventfd de parada (B-3) */
    _Atomic bool                  running;

    /* --- Contador monótono de eventos (B-9) --- */
    _Atomic uint64_t              seq_counter;

    isolation_t         isolation;
    kernel_info_t       kernel;
} core_t;

/* ------------------------------------------------------------------------ */
/* Contadores de alta resolución                                            */
/* ------------------------------------------------------------------------ */

static inline uint64_t read_cycle_counter(void) {
#if defined(__x86_64__)
    /* rdtscp serializa la lectura respecto a la ejecución previa (B-7). */
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    (void)aux;
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    /* B-4: contador virtual del sistema, sin syscall. */
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* Reloj monotónico en ns. En x86_64/ARM pasa por el reloj rápido de la HAL
 * (TSC/CNTVCT calibrado) cuando la plataforma lo permite. */
uint64_t get_timestamp_ns(void) {
    return tx_timestamp_ns();
}

/* CPU actual cacheada por hilo: `sched_getcpu` deja de estar en el camino. */
static _Thread_local int tl_cpu_hint = -2;

static inline uint32_t current_cpu(void) {
    int c = tl_cpu_hint;
    if (__builtin_expect(c < 0, 0)) {
        c = sched_getcpu();
        tl_cpu_hint = c;
    }
    return (uint32_t)c;
}

/* ------------------------------------------------------------------------ */
/* Wait/wake: eventcount sobre futex privado                                */
/* ------------------------------------------------------------------------ */

/* Espera con plazo RELATIVO (ns). Devuelve 0 si despertó, ETIMEDOUT si venció. */
static int futex_wait(_Atomic uint32_t* futex, uint32_t expected, uint64_t timeout_ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(timeout_ns / 1000000000ULL);
    ts.tv_nsec = (long)(timeout_ns % 1000000000ULL);
    return (int)syscall(SYS_futex, futex, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        expected, &ts, NULL, 0);
}

/* Despierta hasta `n` waiters (normalmente 1: sin efecto rebaño). */
static void futex_wake(_Atomic uint32_t* futex, int n) {
    (void)syscall(SYS_futex, futex, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, NULL, NULL, 0);
}

/* Publica la llegada de un evento: secuencia + despertar solo si hay waiters. */
static inline void event_signal(core_t* core) {
    atomic_fetch_add_explicit(&core->event_seq, 1u, memory_order_release);
    if (__builtin_expect(atomic_load_explicit(&core->waiters, memory_order_relaxed) != 0u, 1)) {
        futex_wake(&core->event_seq, 1);
    }
}

/* ------------------------------------------------------------------------ */
/* Hilo de tick RT                                                          */
/* ------------------------------------------------------------------------ */

static void worker_apply_policy(core_t* core, int cpu) {
    if (cpu >= 0) {
        (void)sched_set_affinity_current(cpu);
    }

    if (core->config.use_deadline) {
        deadline_attr_t d;
        d.runtime_ns  = core->config.deadline_runtime_ns ? core->config.deadline_runtime_ns
                                                         : DEFAULT_DEADLINE_RUNTIME_NS;
        d.period_ns   = core->config.deadline_period_ns ? core->config.deadline_period_ns
                                                        : DEFAULT_TICK_NS;
        d.deadline_ns = d.period_ns;
        d.cpu         = -1; /* la afinidad ya se fijó arriba */
        if (sched_set_deadline_current(&d) != SUCCESS) {
            /* Sin admisión DEADLINE: degradar a FIFO manteniendo RT. */
            (void)sched_set_fifo_current(core->config.rt_priority
                                                       ? (int)core->config.rt_priority
                                                       : DEFAULT_RT_PRIORITY);
        }
    } else {
        (void)sched_set_fifo_current(core->config.rt_priority
                                                   ? (int)core->config.rt_priority
                                                   : DEFAULT_RT_PRIORITY);
    }
}

static void* worker(void* arg) {
    worker_arg_t* wa = (worker_arg_t*)arg;
    core_t* core = wa->core;

    worker_apply_policy(core, wa->cpu);

    uint64_t tick_ns = core->config.deadline_period_ns ? core->config.deadline_period_ns
                                                       : DEFAULT_TICK_NS;

    if (core->config.use_timerfd) {
        int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd >= 0) {
            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            its.it_interval.tv_sec  = (time_t)(tick_ns / 1000000000ULL);
            its.it_interval.tv_nsec = (long)(tick_ns % 1000000000ULL);
            its.it_value = its.it_interval;
            if (timerfd_settime(tfd, 0, &its, NULL) == 0) {
                /* B-3: se espera por timerfd Y por el eventfd de parada. */
                struct pollfd pfd[2];
                pfd[0].fd = tfd;          pfd[0].events = POLLIN; pfd[0].revents = 0;
                pfd[1].fd = core->stop_fd; pfd[1].events = POLLIN; pfd[1].revents = 0;
                const nfds_t nfds = (nfds_t)((core->stop_fd >= 0) ? 2 : 1);
                while (atomic_load_explicit(&core->running, memory_order_acquire)) {
                    int pr = poll(pfd, nfds, -1);
                    if (pr < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (nfds == 2 && (pfd[1].revents & POLLIN) != 0) {
                        break;   /* parada solicitada */
                    }
                    if ((pfd[0].revents & POLLIN) != 0) {
                        uint64_t expirations = 0;
                        (void)read(tfd, &expirations, sizeof(expirations));
                    }
                }
                close(tfd);
                return NULL;
            }
            close(tfd);
        }
        /* Si timerfd falla, se continúa con nanosleep. */
    }

    /* B-3: nanosleep con plazo absoluto, para no acumular deriva y salir rápido. */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&core->running, memory_order_acquire)) {
        next.tv_sec  += (time_t)(tick_ns / 1000000000ULL);
        next.tv_nsec += (long)(tick_ns % 1000000000ULL);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec  += 1;
        }
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Ciclo de vida                                                            */
/* ------------------------------------------------------------------------ */

/* Filtra las CPUs aisladas: un hilo de tick ahí destruiría `nohz_full`. */
static void filter_worker_cpus(core_t* core) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < core->config.num_cpus; i++) {
        int cpu = (int)core->config.enabled_cpus[i];
        if (cpu < 0) {
            continue;
        }
        if (sched_cpu_is_isolated(&core->isolation, cpu)) {
            continue;
        }
        core->config.enabled_cpus[out++] = (uint32_t)cpu;
    }
    core->config.num_cpus = out;
}

status_t init(handle_t* handle, const config_t* config) {
    if (!handle || !config) {
        return ERROR_INVALID;
    }

    core_t* core = (core_t*)calloc(1, sizeof(core_t));
    if (!core) {
        return ERROR_MEMORY;
    }

    memcpy(&core->config, config, sizeof(config_t));
    if (core->config.event_pool_size == 0) {
        core->config.event_pool_size = DEFAULT_EVENT_POOL_SIZE;
    }
    if (core->config.stack_size_kb == 0) {
        core->config.stack_size_kb = DEFAULT_STACK_SIZE_KB;
    }
    core->stop_fd = -1;
    core->use_bounded = core->config.use_bounded_mpmc;

    /* RT-3: memoria bloqueada y sin page faults en el camino caliente. */
    if (core->config.lock_memory) {
        (void)mem_lock_all();
    }

    if (core->use_bounded) {
        /* Camino MPMC: pool de bloques + cola acotada. */
        core->pool = mem_pool_create_ex(core->config.event_pool_size,
                                                  (uint32_t)sizeof(event_t),
                                                  core->config.use_hugepages || core->config.use_hugepages_2mb,
                                                  core->config.use_numa_aware);
        if (!core->pool) {
            free(core);
            return ERROR_MEMORY;
        }
        if (core->config.lock_memory) {
            (void)mem_pool_prefault(core->pool);
        }
        if (bounded_init(&core->bounded, core->config.event_pool_size) != SUCCESS) {
            mem_pool_delete(core->pool);
            free(core);
            return ERROR_MEMORY;
        }
    } else {
        /* I-1: anillo de ranuras de evento en línea (sin pool ni indirección). */
        core->slot_ring = (event_t*)aligned_alloc(
            CACHELINE, (size_t)EVENT_RING_SLOTS * sizeof(event_t));
        if (!core->slot_ring) {
            free(core);
            return ERROR_MEMORY;
        }
        memset(core->slot_ring, 0, (size_t)EVENT_RING_SLOTS * sizeof(event_t));
        atomic_init(&core->ring_head, 0u);
        atomic_init(&core->ring_tail, 0u);
    }

    atomic_init(&core->event_seq, 0u);
    atomic_init(&core->waiters, 0u);
    atomic_init(&core->seq_counter, 0u);
    atomic_init(&core->running, true);

    /* Sondeo de kernel (F7) + aislamiento de CPU (RT-4). */
    kernel_probe(&core->kernel);
    kernel_print(&core->kernel);

    sched_isolation_probe(&core->isolation);
    if (core->config.strict_isolation) {
        sched_isolation_print(&core->isolation);
    }

    /*
     * B-2: los hilos de tick solo se crean si el usuario los pide (num_cpus > 0)
     * y nunca sobre CPUs aisladas. El diseño anterior creaba uno por CPU para
     * dormir 1 ms: puro ruido sobre los núcleos del camino caliente.
     */
    core->num_workers = 0;
    if (core->config.num_cpus > MAX_CPUS) {
        core->config.num_cpus = MAX_CPUS;
    }
    if (core->config.num_cpus > 0) {
        filter_worker_cpus(core);
        if (core->config.num_cpus > 0) {
            core->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        size_t stack = (size_t)core->config.stack_size_kb * 1024u;
        if (stack < (size_t)PTHREAD_STACK_MIN) {
            stack = (size_t)PTHREAD_STACK_MIN;
        }
        (void)pthread_attr_setstacksize(&attr, stack);
        for (uint32_t i = 0; i < core->config.num_cpus; i++) {
            core->worker_args[i].core = core;
            core->worker_args[i].cpu  = (int)core->config.enabled_cpus[i];
            if (pthread_create(&core->worker_threads[i], &attr, worker,
                               &core->worker_args[i]) == 0) {
                core->num_workers++;
            }
        }
        pthread_attr_destroy(&attr);
    }

    *handle = (handle_t)core;
    return SUCCESS;
}

status_t tuxwarp_shutdown(handle_t handle) {
    if (!handle) {
        return ERROR_INVALID;
    }

    core_t* core = (core_t*)handle;
    bool lock_memory = core->config.lock_memory;
    atomic_store_explicit(&core->running, false, memory_order_release);

    /* Despierta a los consumidores y a los hilos de tick. */
    atomic_fetch_add_explicit(&core->event_seq, 1u, memory_order_release);
    futex_wake(&core->event_seq, INT_MAX);
    if (core->stop_fd >= 0) {
        uint64_t one = 1u;
        (void)write(core->stop_fd, &one, sizeof(one));
    }

    for (uint32_t i = 0; i < core->num_workers; i++) {
        pthread_join(core->worker_threads[i], NULL);
    }
    if (core->stop_fd >= 0) {
        close(core->stop_fd);
        core->stop_fd = -1;
    }

    if (core->use_bounded) {
        bounded_destroy(&core->bounded);
        mem_pool_delete(core->pool);
    } else {
        free(core->slot_ring);
    }

    free(core);

    if (lock_memory) {
        mem_unlock_all();
    }

    return SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Camino caliente: publicar / suscribir (cero asignación dinámica)          */
/* ------------------------------------------------------------------------ */

/* Intenta extraer un evento del anillo SPSC (o de la cola MPMC). */
static inline bool core_try_pop(core_t* core, event_t* out) {
    if (core->use_bounded) {
        void* p = NULL;
        if (!bounded_pop(&core->bounded, &p)) {
            return false;
        }
        memcpy(out, p, sizeof(event_t));
        mem_pool_free(core->pool, p);
        return true;
    }

    const uint32_t tail = atomic_load_explicit(&core->ring_tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&core->ring_head, memory_order_acquire);
    if (tail == head) {
        return false;
    }
    memcpy(out, &core->slot_ring[tail & EVENT_RING_MASK], sizeof(event_t));
    atomic_store_explicit(&core->ring_tail, tail + 1u, memory_order_release);
    return true;
}

status_t event_publish(handle_t handle, const void* data, uint32_t size) {
    if (!handle || (!data && size > 0)) {
        return ERROR_INVALID;
    }

    core_t* core = (core_t*)handle;

    /* B-8: un evento no puede llevar más carga que su campo `data`. */
    if (size > (uint32_t)sizeof(((event_t*)0)->data)) {
        return ERROR_INVALID;
    }

    const uint64_t now = get_timestamp_ns();
    const uint64_t seq = atomic_fetch_add_explicit(&core->seq_counter, 1u,
                                                   memory_order_relaxed) + 1u;

    if (!core->use_bounded) {
        const uint32_t head = atomic_load_explicit(&core->ring_head, memory_order_relaxed);
        const uint32_t tail = atomic_load_explicit(&core->ring_tail, memory_order_acquire);
        if ((uint32_t)(head - tail) >= EVENT_RING_SLOTS) {
            return ERROR_FULL;
        }

        event_t* ev = &core->slot_ring[head & EVENT_RING_MASK];
        ev->timestamp_ns = now;
        ev->cpu_id = current_cpu();
        ev->sequence = seq;
        if (size > 0u) {
            if (core->config.use_simd) {
                simd_memcpy((void*)ev->data, data, size);
            } else {
                memcpy((void*)ev->data, data, size);
            }
        }
        atomic_store_explicit(&core->ring_head, head + 1u, memory_order_release);
        event_signal(core);
        return SUCCESS;
    }

    /* Camino MPMC: bloque del pool + cola acotada. */
    event_t* event = (event_t*)mem_pool_alloc(core->pool);
    if (!event) {
        return ERROR_FULL;
    }
    event->timestamp_ns = now;
    event->cpu_id = current_cpu();
    event->sequence = seq;
    if (size > 0u) {
        if (core->config.use_simd) {
            simd_memcpy((void*)event->data, data, size);
        } else {
            memcpy((void*)event->data, data, size);
        }
    }
    if (!bounded_push(&core->bounded, event)) {
        mem_pool_free(core->pool, event);
        return ERROR_FULL;
    }
    event_signal(core);
    return SUCCESS;
}

status_t event_subscribe(handle_t handle, event_t* event,
                                             uint32_t timeout_us) {
    if (!handle || !event) {
        return ERROR_INVALID;
    }

    core_t* core = (core_t*)handle;

    if (core_try_pop(core, event)) {
        return SUCCESS;
    }
    if (timeout_us == 0u) {
        return ERROR_TIMEOUT;
    }

    const uint64_t deadline = get_timestamp_ns() + (uint64_t)timeout_us * 1000u;

    for (;;) {
        /*
         * B-1 (eventcount): se lee la secuencia ANTES de comprobar la cola. Si
         * el productor publica después, incrementa la secuencia y el futex
         * devuelve EAGAIN al instante; si publica antes, la cola ya lo ve.
         */
        const uint32_t seq = atomic_load_explicit(&core->event_seq, memory_order_acquire);
        if (core_try_pop(core, event)) {
            return SUCCESS;
        }
        if (!atomic_load_explicit(&core->running, memory_order_acquire)) {
            return ERROR_TIMEOUT;
        }

        const uint64_t now = get_timestamp_ns();
        if (now >= deadline) {
            return ERROR_TIMEOUT;
        }
        const uint64_t remaining = deadline - now;

        atomic_fetch_add_explicit(&core->waiters, 1u, memory_order_seq_cst);
        /* Reintento: evita la ventana entre leer `seq` y dormir. */
        if (core_try_pop(core, event)) {
            atomic_fetch_sub_explicit(&core->waiters, 1u, memory_order_seq_cst);
            return SUCCESS;
        }
        (void)futex_wait(&core->event_seq, seq, remaining);
        atomic_fetch_sub_explicit(&core->waiters, 1u, memory_order_seq_cst);
    }
}

/* ------------------------------------------------------------------------ */
/* Utilidades                                                               */
/* ------------------------------------------------------------------------ */

uint64_t get_cycles(void) {
    return read_cycle_counter();
}

platform_t detect_platform(void) {
#if defined(__aarch64__)
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "Raspberry Pi")) {
                fclose(fp);
                return strstr(line, "5") ? PLATFORM_RPI5 : PLATFORM_RPI4;
            }
            if (strstr(line, "Jetson")) {
                fclose(fp);
                return strstr(line, "Orin") ? PLATFORM_JETSON_ORIN
                                            : PLATFORM_JETSON_NANO;
            }
            if (strstr(line, "Rockchip")) {
                fclose(fp);
                return strstr(line, "RK3588") ? PLATFORM_ROCK5B
                                              : PLATFORM_ORANGE_PI5;
            }
        }
        fclose(fp);
    }
    return PLATFORM_RPI4;
#elif defined(__x86_64__)
    return PLATFORM_INTEL_NUC;
#else
    return PLATFORM_GENERIC;
#endif
}

const char* abi_version_string(void) {
    return VERSION_STRING;
}

uint32_t abi_version(void) {
    return (uint32_t)ABI_VERSION;
}
