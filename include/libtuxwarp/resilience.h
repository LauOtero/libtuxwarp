/*
 * resilience.h — Resiliencia absoluta (F4).
 *
 *   RS-1: snapshot A/B con seqlock (lectura consistente sin bloqueos).
 *   RS-3: WAL transaccional idempotente con `fsync` por lotes y CRC por entrada.
 *   RS-6: checkpoint con *golden image* de escritura atómica (`rename`).
 */
#ifndef RESILIENCE_H
#define RESILIENCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "libtuxwarp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- RS-1 --- */

typedef struct {
    uint32_t          size;         /* tamaño del payload */
    _Atomic uint32_t  seq;          /* seqlock del escritor */
    _Atomic uint32_t  active;       /* buffer publicado (0 ó 1) */
    uint8_t*          buf[2];       /* doble buffer A/B */
} snapshot_t;

API status_t snapshot_init(snapshot_t* s, uint32_t size);
API status_t snapshot_write(snapshot_t* s, const void* data, uint32_t size);
API status_t snapshot_read(const snapshot_t* s, void* out, uint32_t size);
API uint32_t           snapshot_sequence(const snapshot_t* s);
API void               snapshot_destroy(snapshot_t* s);

/* ---------------------------------------------------------------- RS-3 --- */

#define WAL_PATH_MAX 256
#define WAL_REC_MAX  (1u << 20)

typedef struct {
    int      fd;
    char     path[WAL_PATH_MAX];
    uint32_t batch;                 /* entradas entre `fsync` */
    uint32_t pending;
    uint64_t seq;
} wal_t;

API status_t wal_open(wal_t* w, const char* path, uint32_t batch);
API status_t wal_append(wal_t* w, const void* rec, uint32_t len);
API status_t wal_sync(wal_t* w);
API uint64_t           wal_seq(const wal_t* w);
API void               wal_close(wal_t* w);

/* Reproduce el WAL verificando CRC; se detiene en la primera entrada corrupta
 * (escritura truncada). Devuelve el nº de entradas válidas reproducidas. */
API int wal_replay(const char* path,
                                       int (*cb)(const void* rec, uint32_t len, void* ud),
                                       void* ud);

/* ---------------------------------------------------------------- RS-6 --- */

/* Escritura atómica: escribe en `<path>.tmp`, `fsync` y `rename` sobre `path`. */
API status_t golden_save(const char* path, const void* data, uint32_t len);
API status_t golden_load(const char* path, void* out, uint32_t cap, uint32_t* out_len);
API uint32_t           golden_crc(const void* data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* RESILIENCE_H */
