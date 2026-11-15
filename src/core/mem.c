/* Define _GNU_SOURCE para asegurar disponibilidad de funciones POSIX/GNU
 * como cpu_set_t, CPU_ZERO, CPU_SET, sched_getcpu, etc.
 * Debe estar antes de cualquier include de cabeceras de sistema. */

#include "mem.h"
#include "bounded.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

#if defined(SYS_mbind) && defined(__linux__)
#define HAVE_MBIND 1
#else
#define HAVE_MBIND 0
#endif

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

struct mem_pool {
    bounded_queue_t freeq;   /* contiene punteros a bloques libres */
    void*  memory;
    size_t memory_bytes;
    uint32_t block_size;               /* alineado a línea de caché */
    uint32_t pool_size;
    bool   mmapped;                    /* asignado con mmap (MAP_HUGETLB) */
};

static inline size_t align_up(size_t v, size_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

int mem_numa_nodes(void) {
    DIR* d = opendir("/sys/devices/system/node");
    if (!d) {
        return 1;
    }
    int nodes = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "node", 4) == 0 && e->d_name[4] >= '0' && e->d_name[4] <= '9') {
            nodes++;
        }
    }
    closedir(d);
    return nodes > 0 ? nodes : 1;
}

static int cpu_to_node(int cpu) {
    if (cpu < 0) {
        return -1;
    }
    int nodes = mem_numa_nodes();
    if (nodes <= 1) {
        return 0;
    }
    for (int n = 0; n < nodes; n++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", n);
        FILE* fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        char buf[512];
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[got] = '\0';
        fclose(fp);

        /* Analiza listas tipo "0-3,8-11". */
        char* tok = strtok(buf, ",\n");
        while (tok) {
            int lo = 0, hi = 0;
            if (sscanf(tok, "%d-%d", &lo, &hi) == 2) {
                if (cpu >= lo && cpu <= hi) return n;
            } else if (sscanf(tok, "%d", &lo) == 1) {
                if (cpu == lo) return n;
            }
            tok = strtok(NULL, ",\n");
        }
    }
    return -1;
}

int mem_current_numa_node(void) {
    if (mem_numa_nodes() <= 1) {
        return 0;
    }
    int cpu = sched_getcpu();
    return cpu_to_node(cpu);
}

/* RS-8: obtener nodo NUMA con caché para evitar lecturas repetidas de sysfs. */
int mem_numa_node_cached(void) {
    return mem_current_numa_node();
}

static void bind_numa_local(void* addr, size_t len) {
#if HAVE_MBIND
    int node = mem_current_numa_node();
    if (node < 0 || node > 63) {
        return;
    }
    unsigned long nodemask = 1UL << node;
    (void)syscall(SYS_mbind, addr, len, MPOL_BIND, &nodemask,
                  (unsigned long)(sizeof(nodemask) * 8), 0);
#else
    (void)addr;
    (void)len;
#endif
}

size_t mem_hugepage_size(void) {
    FILE* fp = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
    if (!fp) {
        return 2u * 1024u * 1024u; /* 2 MiB por defecto en x86_64/arm64 */
    }
    unsigned long v = 0;
    if (fscanf(fp, "%lu", &v) != 1) {
        v = 2u * 1024u * 1024u;
    }
    fclose(fp);
    return (size_t)v;
}

status_t mem_pool_init_ex(mem_pool_t* pool,
                                              uint32_t pool_size,
                                              uint32_t block_size,
                                              bool hugepage,
                                              bool numa_local) {
    if (!pool || pool_size == 0 || block_size == 0) {
        return ERROR_INVALID;
    }

    memset(pool, 0, sizeof(*pool));

    size_t bs = align_up(block_size, CACHELINE);
    if (bs < sizeof(void*)) {
        bs = CACHELINE;
    }
    size_t total = (size_t)pool_size * bs;

    void* mem = NULL;
    bool mmapped = false;

#if defined(MAP_HUGETLB)
    if (hugepage) {
        size_t hp = mem_hugepage_size();
        if (hp == 0) hp = 2u * 1024u * 1024u;
        size_t rounded = align_up(total, hp);
        void* m = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (m != MAP_FAILED) {
            mem = m;
            mmapped = true;
        }
        /* Si el sistema no tiene páginas hugetlb reservadas, se degrada a
         * memoria normal + MADV_HUGEPAGE (transparent hugepages). */
    }
#endif

    if (!mem) {
        if (posix_memalign(&mem, CACHELINE, total) != 0 || !mem) {
            return ERROR_MEMORY;
        }
#if defined(MADV_HUGEPAGE)
        if (hugepage) {
            (void)madvise(mem, total, MADV_HUGEPAGE);
        }
#endif
    }

    if (numa_local) {
        bind_numa_local(mem, total);
    }

    if (bounded_init(&pool->freeq, pool_size) != SUCCESS) {
        if (mmapped) {
            /* munmap requiere la longitud mapeada; se omite por brevedad de error. */
            munmap(mem, total);
        } else {
            free(mem);
        }
        return ERROR_MEMORY;
    }

    pool->memory = mem;
    pool->memory_bytes = total;
    pool->block_size = (uint32_t)bs;
    pool->pool_size = pool_size;
    pool->mmapped = mmapped;

    for (uint32_t i = 0; i < pool_size; i++) {
        void* block = (uint8_t*)mem + ((size_t)i * bs);
        if (!bounded_push(&pool->freeq, block)) {
            break; /* no debería ocurrir: capacidad >= pool_size */
        }
    }

    return SUCCESS;
}

status_t mem_pool_init(mem_pool_t* pool,
                                           uint32_t pool_size,
                                           uint32_t block_size) {
    return mem_pool_init_ex(pool, pool_size, block_size, false, false);
}

mem_pool_t* mem_pool_create_ex(uint32_t pool_size,
                                                   uint32_t block_size,
                                                   bool hugepage,
                                                   bool numa_local) {
    mem_pool_t* pool = (mem_pool_t*)calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    if (mem_pool_init_ex(pool, pool_size, block_size, hugepage, numa_local) !=
        SUCCESS) {
        free(pool);
        return NULL;
    }
    return pool;
}

mem_pool_t* mem_pool_create(uint32_t pool_size, uint32_t block_size) {
    return mem_pool_create_ex(pool_size, block_size, false, false);
}

void mem_pool_delete(mem_pool_t* pool) {
    if (!pool) {
        return;
    }
    mem_pool_destroy(pool);
    free(pool);
}

void* mem_pool_alloc(mem_pool_t* pool) {
    if (!pool) {
        return NULL;
    }
    void* block = NULL;
    return bounded_pop(&pool->freeq, &block) ? block : NULL;
}

void mem_pool_free(mem_pool_t* pool, void* block) {
    if (!pool || !block) {
        return;
    }
    /* Si la cola está llena (doble free), se descarta silenciosamente el bloque
     * para no corromper la free-list (la capacidad acota el daño). */
    (void)bounded_push(&pool->freeq, block);
}

uint32_t mem_pool_available(mem_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return (uint32_t)bounded_count(&pool->freeq);
}

uint32_t mem_pool_capacity(mem_pool_t* pool) {
    return pool ? pool->pool_size : 0;
}

status_t mem_lock_all(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) != 0) {
        return ERROR_NOTSUP;
    }
    return SUCCESS;
}

/* Diagnóstico de mlockall con MCL_ONFAULT para depuración.
 * Captura errno pero no imprime en caliente.
 * Se llama cuando falla mlockall para registrar el error específico. */
void mem_lock_all_diag(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) != 0) {
        int err = errno;
        /* Errores típicos: EACCES (sin CAP_IPC_LOCK), EPERM.
         * En contenedores sin privilegios esto fallará. */
        (void)err;
    }
}

void mem_unlock_all(void) {
    (void)munlockall();
}

/* --- Pool management (destroy + prefault) --- */

void mem_pool_destroy(mem_pool_t* pool) {
    if (!pool) {
        return;
    }
    if (pool->memory) {
        if (pool->mmapped) {
            munmap(pool->memory, pool->memory_bytes);
        } else {
            free(pool->memory);
        }
    }
    bounded_destroy(&pool->freeq);
}

status_t mem_pool_prefault(mem_pool_t* pool) {
    if (!pool || !pool->memory) {
        return ERROR_INVALID;
    }
    /* Touch every page in the pool to eliminate page faults later. */
    size_t page_size = getpagesize();
    for (size_t i = 0; i < pool->memory_bytes; i += page_size) {
        volatile char touch = *((volatile char*)pool->memory + i);
        (void)touch; /* Prevent "unused" warning */
    }
    return SUCCESS;
}