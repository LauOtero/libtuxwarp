/*
 * test_resilience.c — Validación de la resiliencia absoluta (F4).
 *
 *   RS-1: snapshot A/B con seqlock (lectura consistente).
 *   RS-3: WAL transaccional con CRC y recuperación determinista ante truncado.
 *   RS-6: golden image con escritura atómica y verificación de integridad.
 */
#include "resilience.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

static int test_count = 0;
static int test_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    test_count++; \
    if (cond) { \
        test_passed++; \
        printf("[PASS] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
    } \
} while (0)

#define WAL_PATH    "/tmp/test_wal.bin"
#define GOLDEN_PATH "/tmp/test_golden.bin"

/* ---------------------------------------------------------------- RS-1 --- */

static void test_snapshot(void) {
    printf("Running snapshot A/B tests (RS-1)...\n");

    snapshot_t s;
    status_t st = snapshot_init(&s, 64);
    TEST_ASSERT(st == SUCCESS, "snapshot_init should succeed");
    TEST_ASSERT(snapshot_sequence(&s) == 0, "initial sequence should be 0");

    uint64_t payload = 0x1122334455667788ULL;
    st = snapshot_write(&s, &payload, sizeof(payload));
    TEST_ASSERT(st == SUCCESS, "snapshot_write should succeed");

    uint64_t readback = 0;
    st = snapshot_read(&s, &readback, sizeof(readback));
    TEST_ASSERT(st == SUCCESS, "snapshot_read should succeed");
    TEST_ASSERT(readback == payload, "readback should match the written payload");
    TEST_ASSERT(snapshot_sequence(&s) == 2, "sequence should advance by 2 per write");

    /* Segunda escritura: alterna al buffer B y la lectura sigue siendo coherente. */
    payload = 0xDEADBEEFCAFEBABEULL;
    snapshot_write(&s, &payload, sizeof(payload));
    readback = 0;
    snapshot_read(&s, &readback, sizeof(readback));
    TEST_ASSERT(readback == payload, "second write should be visible on read");
    TEST_ASSERT(atomic_load(&s.active) == 0, "active buffer should have flipped twice");

    /* Escritura mayor que la capacidad: rechazada. */
    uint8_t big[128] = {0};
    TEST_ASSERT(snapshot_write(&s, big, sizeof(big)) == ERROR_INVALID,
                "oversized write should be rejected");

    snapshot_destroy(&s);
}

/* ---------------------------------------------------------------- RS-3 --- */

typedef struct {
    uint64_t count;
    uint64_t last;
} replay_ctx_t;

static int replay_cb(const void* rec, uint32_t len, void* ud) {
    replay_ctx_t* ctx = (replay_ctx_t*)ud;
    if (len != sizeof(uint64_t)) {
        return -1;
    }
    uint64_t v;
    memcpy(&v, rec, sizeof(v));
    ctx->count++;
    ctx->last = v;
    return 0;
}

static void test_wal(void) {
    printf("\nRunning WAL tests (RS-3)...\n");

    (void)unlink(WAL_PATH);

    wal_t w;
    status_t st = wal_open(&w, WAL_PATH, 2); /* fsync cada 2 entradas */
    TEST_ASSERT(st == SUCCESS, "wal_open should succeed");

    for (uint64_t i = 1; i <= 5; i++) {
        st = wal_append(&w, &i, sizeof(i));
        if (st != SUCCESS) {
            break;
        }
    }
    TEST_ASSERT(st == SUCCESS, "wal_append should succeed for all records");
    TEST_ASSERT(wal_seq(&w) == 5, "wal sequence should be 5");

    st = wal_sync(&w);
    TEST_ASSERT(st == SUCCESS, "wal_sync should succeed");
    wal_close(&w);

    replay_ctx_t ctx = {0};
    int valid = wal_replay(WAL_PATH, replay_cb, &ctx);
    TEST_ASSERT(valid == 5, "replay should recover exactly 5 records");
    TEST_ASSERT(ctx.count == 5, "callback should be invoked once per record");
    TEST_ASSERT(ctx.last == 5, "last replayed record should be 5 (FIFO order)");

    /* Reapertura en modo append: continúa la secuencia sin perder historia. */
    st = wal_open(&w, WAL_PATH, 1);
    TEST_ASSERT(st == SUCCESS, "wal reopen should succeed");
    uint64_t six = 6;
    wal_append(&w, &six, sizeof(six));
    wal_close(&w);

    ctx.count = 0;
    valid = wal_replay(WAL_PATH, replay_cb, &ctx);
    TEST_ASSERT(valid == 6 && ctx.last == 6, "replay should now recover 6 records");

    /* Corrupción/truncado: la reproducción se detiene en la primera entrada mala. */
    int fd = open(WAL_PATH, O_WRONLY | O_APPEND);
    TEST_ASSERT(fd >= 0, "corruption fixture should open the WAL");
    if (fd >= 0) {
        uint8_t garbage[16] = {0xFF};
        (void)write(fd, garbage, sizeof(garbage));
        close(fd);
    }
    ctx.count = 0;
    valid = wal_replay(WAL_PATH, replay_cb, &ctx);
    TEST_ASSERT(valid == 6, "replay should stop deterministically at the corrupt tail");

    (void)unlink(WAL_PATH);
}

/* ---------------------------------------------------------------- RS-6 --- */

static void test_golden(void) {
    printf("\nRunning golden image tests (RS-6)...\n");

    (void)unlink(GOLDEN_PATH);
    (void)unlink(GOLDEN_PATH ".tmp");

    uint8_t data[256];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }

    status_t st = golden_save(GOLDEN_PATH, data, sizeof(data));
    TEST_ASSERT(st == SUCCESS, "golden_save should succeed");
    TEST_ASSERT(access(GOLDEN_PATH ".tmp", F_OK) != 0, "temporary file must be renamed away");

    uint8_t out[256];
    uint32_t out_len = 0;
    st = golden_load(GOLDEN_PATH, out, sizeof(out), &out_len);
    TEST_ASSERT(st == SUCCESS, "golden_load should succeed");
    TEST_ASSERT(out_len == sizeof(data), "loaded length should match");
    TEST_ASSERT(memcmp(out, data, sizeof(data)) == 0, "loaded payload should match exactly");

    uint32_t crc_a = golden_crc(data, sizeof(data));
    uint32_t crc_b = golden_crc(out, out_len);
    TEST_ASSERT(crc_a == crc_b && crc_a != 0, "CRC should be non-zero and reproducible");

    /* Corrupción de un byte del payload: la carga debe detectarlo por CRC. */
    int fd = open(GOLDEN_PATH, O_RDWR);
    TEST_ASSERT(fd >= 0, "golden fixture should open the image");
    if (fd >= 0) {
        uint8_t bogus = 0x00;
        (void)pwrite(fd, &bogus, 1, 10);
        close(fd);

        uint8_t bad_out[256];
        uint32_t bad_len = 0;
        st = golden_load(GOLDEN_PATH, bad_out, sizeof(bad_out), &bad_len);
        TEST_ASSERT(st == ERROR_CORRUPT, "corrupted image must be rejected (CRC)");
    }

    TEST_ASSERT(golden_load("/tmp/definitely_missing_image.bin", out, sizeof(out), NULL)
                    != SUCCESS,
                "missing image should not load");

    (void)unlink(GOLDEN_PATH);
}

int main(void) {
    printf("libtuxwarp Resilience Test Suite v%s\n", VERSION);
    printf("=====================================\n\n");

    test_snapshot();
    test_wal();
    test_golden();

    printf("\n=====================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
