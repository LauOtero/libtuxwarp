#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "libtuxwarp.h"

typedef struct mem_pool mem_pool_t;

/* --- Pool pre-asignado (free-list lock-free, sin ABA) --------------------- */

API status_t mem_pool_init(mem_pool_t* pool,
                                                         uint32_t pool_size,
                                                         uint32_t block_size);

/* Creación/liberación sobre heap (el tipo es opaco para el consumidor). */
API mem_pool_t* mem_pool_create(uint32_t pool_size, uint32_t block_size);
API mem_pool_t* mem_pool_create_ex(uint32_t pool_size,
                                                                uint32_t block_size,
                                                                bool hugepage,
                                                                bool numa_local);
API void                  mem_pool_delete(mem_pool_t* pool);

/* RT-3 (hugepages) + PF-5 (NUMA-aware): variante extendida. */
API status_t mem_pool_init_ex(mem_pool_t* pool,
                                                            uint32_t pool_size,
                                                            uint32_t block_size,
                                                            bool hugepage,
                                                            bool numa_local);

API void*    mem_pool_alloc(mem_pool_t* pool);
API void     mem_pool_free(mem_pool_t* pool, void* block);
API void     mem_pool_destroy(mem_pool_t* pool);
API uint32_t mem_pool_available(mem_pool_t* pool);
API uint32_t mem_pool_capacity(mem_pool_t* pool);

/* Pre-toca todos los bloques para eliminar page faults en el camino caliente. */
API status_t mem_pool_prefault(mem_pool_t* pool);

/* --- RT-3: bloqueo de memoria y hugepages --------------------------------- */

/* mlockall(MCL_CURRENT|MCL_FUTURE): elimina page faults del proceso RT. */
API status_t mem_lock_all(void);
API void               mem_unlock_all(void);

/* Tamaño de la hugepage transparente del sistema (0 si no disponible). */
API size_t mem_hugepage_size(void);

/* --- PF-5: NUMA ----------------------------------------------------------- */

/* Nodo NUMA de la CPU actual (-1 si no determinable o no NUMA). */
API int mem_current_numa_node(void);
/* Número de nodos NUMA visibles (1 si no NUMA). */
API int mem_numa_nodes(void);

#endif
