/*
 * rt.c — Histograma de latencia (RT-6), watchdog (RS-4) y tick (RT-8).
 */
#include "rt.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

/* ------------------------------------------------------------------------ */
/* RT-6: histograma de latencia                                             */
/* ------------------------------------------------------------------------ */

void hist_init(hist_t* h, uint64_t bucket_ns) {
    if (!h) {
        return;
    }
    memset(h, 0, sizeof(*h));
    h->bucket_ns = bucket_ns ? bucket_ns : 100; /* 100 ns por bucket por defecto */
    h->min_ns = UINT64_MAX;
    h->prefix_sum_valid = false;
    /* Recíproco con redondeo hacia arriba: floor(n/b) == (n*recip) >> 64 para
     * todo n < 2^63, que cubre cualquier latencia expresada en ns. */
    h->bucket_recip = (UINT64_MAX / h->bucket_ns) + 1u;
}

void hist_reset(hist_t* h) {
    if (!h) {
        return;
    }
    uint64_t width = h->bucket_ns;
    hist_init(h, width);
}

void hist_record(hist_t* h, uint64_t latency_ns) {
    if (!h) {
        return;
    }
    /* División por recíproco: evita el `idiv` de 64 bits por muestra. */
    const uint64_t idx = (uint64_t)(((__uint128_t)latency_ns * h->bucket_recip) >> 64);
    if (idx < HIST_BUCKETS) {
        h->buckets[idx]++;
        /* Invalidar cache de prefix-sum (se reconstruye lazy al siguiente percentile query). */
        h->prefix_sum_valid = false;
    } else {
        h->overflow++;
    }
    h->count++;
    h->sum_ns += latency_ns;
    h->sum_sq_ns += latency_ns * latency_ns;
    if (latency_ns < h->min_ns) {
        h->min_ns = latency_ns;
    }
    if (latency_ns > h->max_ns) {
        h->max_ns = latency_ns;
    }
}

uint64_t hist_count(const hist_t* h) {
    return h ? h->count : 0;
}

uint64_t hist_mean_ns(const hist_t* h) {
    if (!h || h->count == 0) {
        return 0;
    }
    return h->sum_ns / h->count;
}

uint64_t hist_stddev_ns(const hist_t* h) {
    if (!h || h->count == 0) {
        return 0;
    }
    double mean = (double)h->sum_ns / (double)h->count;
    double var  = (double)h->sum_sq_ns / (double)h->count - mean * mean;
    if (var <= 0.0) {
        return 0;
    }
    return (uint64_t)sqrt(var);
}

uint64_t hist_percentile_ns(hist_t* h, double p) {
    if (!h || h->count == 0) {
        return 0;
    }
    if (p <= 0.0) p = 0.0;
    if (p >= 100.0) p = 100.0;

    uint64_t target = (uint64_t)((p / 100.0) * (double)h->count);
    if (target == 0) {
        target = 1;
    }

    /* Reconstruir cache de prefix-sum lazy si está invalidada. */
    if (!h->prefix_sum_valid) {
        uint64_t acc = 0;
        for (uint64_t i = 0; i < HIST_BUCKETS; i++) {
            acc += h->buckets[i];
            h->prefix_sum[i] = acc;
        }
        h->prefix_sum_valid = true;
    }

    /* Búsqueda binaria O(log n) sobre prefix_sum. */
    uint64_t lo = 0, hi = HIST_BUCKETS - 1;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (h->prefix_sum[mid] >= target) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return (lo + 1u) * h->bucket_ns;
}

uint64_t hist_wcet_ns(const hist_t* h) {
    return h ? h->max_ns : 0;
}

uint64_t hist_best_ns(const hist_t* h) {
    return (h && h->count > 0) ? h->min_ns : 0;
}

void hist_print(const hist_t* h, const char* label) {
    if (!h) {
        return;
    }
    /* El percentil reconstruye la caché de prefix-sum (mutación benigna del
     * estado interno); se documenta con un cast explícito. */
    hist_t* hm = (hist_t*)(uintptr_t)h;
    printf("[libtuxwarp] %s: n=%llu min=%llu ns mean=%llu ns stddev=%llu ns "
           "p99=%llu ns p99.9=%llu ns WCET=%llu ns (overflow=%llu)\n",
           label ? label : "latencia",
           (unsigned long long)h->count,
           (unsigned long long)hist_best_ns(h),
           (unsigned long long)hist_mean_ns(h),
           (unsigned long long)hist_stddev_ns(h),
           (unsigned long long)hist_percentile_ns(hm, 99.0),
           (unsigned long long)hist_percentile_ns(hm, 99.9),
           (unsigned long long)hist_wcet_ns(h),
           (unsigned long long)h->overflow);
}

/* ------------------------------------------------------------------------ */
/* RS-4: watchdog software con heartbeat                                    */
/* ------------------------------------------------------------------------ */

status_t watchdog_init(watchdog_t* wd, uint64_t timeout_ns) {
    if (!wd || timeout_ns == 0) {
        return ERROR_INVALID;
    }
    wd->timeout_ns = timeout_ns;
    atomic_init(&wd->last_kick_ns, get_timestamp_ns());
    return SUCCESS;
}

void watchdog_kick(watchdog_t* wd) {
    if (!wd) {
        return;
    }
    atomic_store_explicit(&wd->last_kick_ns, get_timestamp_ns(),
                          memory_order_release);
}

uint64_t watchdog_elapsed_ns(const watchdog_t* wd) {
    if (!wd) {
        return 0;
    }
    uint64_t last = atomic_load_explicit(&wd->last_kick_ns, memory_order_acquire);
    uint64_t now  = get_timestamp_ns();
    return (now >= last) ? (now - last) : 0;
}

bool watchdog_expired(const watchdog_t* wd) {
    if (!wd) {
        return false;
    }
    return watchdog_elapsed_ns(wd) > wd->timeout_ns;
}

void watchdog_destroy(watchdog_t* wd) {
    if (!wd) {
        return;
    }
    wd->timeout_ns = 0;
}

bool watchdog_hw_available(void) {
    return access("/dev/watchdog", W_OK) == 0;
}

/* ------------------------------------------------------------------------ */
/* RT-8: tick periódico por timerfd                                         */
/* ------------------------------------------------------------------------ */

status_t tick_init(tick_t* tick, uint64_t period_ns) {
    if (!tick || period_ns == 0) {
        return ERROR_INVALID;
    }
    tick->fd = -1;
    tick->period_ns = period_ns;

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0) {
        return ERROR_NOTSUP;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_sec  = (time_t)(period_ns / 1000000000ULL);
    its.it_interval.tv_nsec = (long)(period_ns % 1000000000ULL);
    its.it_value = its.it_interval;

    if (timerfd_settime(fd, 0, &its, NULL) != 0) {
        close(fd);
        return ERROR_IO;
    }

    tick->fd = fd;
    return SUCCESS;
}

int tick_fd(const tick_t* tick) {
    return tick ? tick->fd : -1;
}

int tick_wait(tick_t* tick) {
    if (!tick || tick->fd < 0) {
        return -1;
    }
    uint64_t expirations = 0;
    ssize_t n = read(tick->fd, &expirations, sizeof(expirations));
    if (n != (ssize_t)sizeof(expirations)) {
        return -1;
    }
    return (int)expirations;
}

void tick_destroy(tick_t* tick) {
    if (!tick) {
        return;
    }
    if (tick->fd >= 0) {
        close(tick->fd);
        tick->fd = -1;
    }
    tick->period_ns = 0;
}
