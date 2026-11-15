/*
 * test_integration.c — Validación de la coexistencia con `libtuxrt_core` (F7).
 *
 *   - Puente de eventos libtuxwarp → sink externo (desacoplado de tuxrt_*).
 *   - Contadores del puente (OB-2) y trazas (OB-1).
 *   - Coexistencia de espacios de nombres `*` / `tuxrt_*`.
 *   - Pool de memoria opaco (creación/liberación por API pública).
 *
 * El enlace con libtuxrt_core es UNIDIRECCIONAL (consumidor -> proveedor) y se
 * declara con el contrato TuxRT Provider ABI (`tuxrt_provider.h`), publicado
 * por CMake vía `tuxrt_provide()`; libtuxwarp no enlaza a libtuxrt_core.
 */
#include "tuxrt.h"
#include "mem.h"
#include "libtuxwarp.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

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

typedef struct {
    _Atomic uint64_t received;
    _Atomic uint64_t rejected;
    uint32_t         reject_every;
    uint32_t         expected_value;
    bool             value_ok;
} sink_ctx_t;

static int test_sink(const event_t* event, void* user) {
    sink_ctx_t* ctx = (sink_ctx_t*)user;

    uint32_t value = 0;
    memcpy(&value, (const void*)event->data, sizeof(value));

    if (ctx->reject_every != 0 &&
        (atomic_load(&ctx->received) % ctx->reject_every) == 0) {
        atomic_fetch_add(&ctx->rejected, 1);
        return -1;   /* rechazado: el puente lo contabiliza como descartado */
    }

    atomic_fetch_add(&ctx->received, 1);
    return 0;
}

static void test_abi_metadata(void) {
    printf("Running ABI metadata tests...\n");

    const char* v = abi_version_string();
    TEST_ASSERT(v != NULL && strcmp(v, VERSION_STRING) == 0,
                "ABI version string should match the header");
    TEST_ASSERT(abi_version() == (uint32_t)ABI_VERSION,
                "ABI numeric version should match the header");
}

static void test_mem_pool_api(void) {
    printf("\nRunning opaque memory pool tests (RT-2/RT-3)...\n");

    mem_pool_t* pool = mem_pool_create(16, 64);
    TEST_ASSERT(pool != NULL, "mem_pool_create should return a valid pool");
    if (!pool) {
        return;
    }

    TEST_ASSERT(mem_pool_capacity(pool) == 16, "capacity should be 16");
    TEST_ASSERT(mem_pool_available(pool) == 16, "all 16 blocks should be free");

    void* blocks[16];
    bool all = true;
    for (int i = 0; i < 16; i++) {
        blocks[i] = mem_pool_alloc(pool);
        if (!blocks[i]) {
            all = false;
        }
    }
    TEST_ASSERT(all, "all 16 blocks should be allocatable");
    TEST_ASSERT(mem_pool_available(pool) == 0, "pool should now be exhausted");
    TEST_ASSERT(mem_pool_alloc(pool) == NULL, "alloc on empty pool should fail");

    /* Los bloques deben estar alineados a línea de caché y ser disjuntos. */
    bool aligned = true, distinct = true;
    for (int i = 0; i < 16; i++) {
        if (((uintptr_t)blocks[i] % CACHELINE) != 0) {
            aligned = false;
        }
        for (int j = i + 1; j < 16; j++) {
            if (blocks[i] == blocks[j]) {
                distinct = false;
            }
        }
    }
    TEST_ASSERT(aligned, "blocks should be cache-line aligned");
    TEST_ASSERT(distinct, "blocks should be distinct addresses");

    for (int i = 0; i < 16; i++) {
        mem_pool_free(pool, blocks[i]);
    }
    TEST_ASSERT(mem_pool_available(pool) == 16, "all blocks should be returned");

    mem_pool_delete(pool);
    mem_pool_delete(NULL);   /* NULL-safe */
    TEST_ASSERT(true, "mem_pool_delete should be NULL-safe");
}

static void test_namespace_coexistence(void) {
    printf("\nRunning namespace coexistence tests...\n");

    const char* info = tuxrt_namespace_info();
    TEST_ASSERT(info != NULL, "namespace info should be available");
    TEST_ASSERT(strstr(info, "") != NULL, "info should mention the  prefix");
    TEST_ASSERT(strstr(info, "tuxrt_") != NULL, "info should mention the tuxrt_ prefix");
    printf("  %s\n", info);
}

static void test_bridge_roundtrip(void) {
    printf("\nRunning libtuxwarp → sink bridge round-trip...\n");

    config_t config = {0};
    config.event_pool_size = 4096;
    config.use_simd = true;
    config.use_bounded_mpmc = true;

    handle_t handle = NULL;
    if (init(&handle, &config) != SUCCESS) {
        printf("[SKIP] init failed\n");
        return;
    }

    sink_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    atomic_init(&ctx.received, 0);
    atomic_init(&ctx.rejected, 0);
    ctx.reject_every = 0;

    bridge_t bridge;
    status_t st = bridge_attach(&bridge, handle, test_sink, &ctx);
    TEST_ASSERT(st == SUCCESS, "bridge_attach should succeed");
    TEST_ASSERT(bridge_counters(&bridge) != NULL, "bridge counters should be exposed");

    const uint32_t N = 100;
    uint32_t published = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t value = i;
        if (event_publish(handle, &value, sizeof(value)) == SUCCESS) {
            published++;
        }
    }
    TEST_ASSERT(published == N, "all N events should be published");

    uint64_t forwarded = bridge_pump(&bridge, N, 1000);
    TEST_ASSERT(forwarded == N, "bridge should forward all N events");
    TEST_ASSERT(atomic_load(&ctx.received) == N, "sink should have received all N events");

    const counters_t* c = bridge_counters(&bridge);
    TEST_ASSERT(atomic_load(&c->consumed) == N, "bridge consumed counter should be N");
    TEST_ASSERT(atomic_load(&c->forwarded) == N, "bridge forwarded counter should be N");
    TEST_ASSERT(atomic_load(&c->dropped) == 0, "bridge dropped counter should be 0");

    bridge_detach(&bridge);
    TEST_ASSERT(bridge_pump(&bridge, N, 0) == 0,
                "detached bridge should forward nothing");

    tuxwarp_shutdown(handle);
}

static void test_bridge_rejection(void) {
    printf("\nRunning bridge rejection accounting...\n");

    config_t config = {0};
    config.event_pool_size = 512;

    handle_t handle = NULL;
    if (init(&handle, &config) != SUCCESS) {
        printf("[SKIP] init failed\n");
        return;
    }

    sink_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    atomic_init(&ctx.received, 0);
    atomic_init(&ctx.rejected, 0);
    ctx.reject_every = 2;   /* rechaza uno de cada dos */

    bridge_t bridge;
    bridge_attach(&bridge, handle, test_sink, &ctx);

    const uint32_t N = 20;
    for (uint32_t i = 0; i < N; i++) {
        event_publish(handle, &i, sizeof(i));
    }
    uint64_t forwarded = bridge_pump(&bridge, N, 1000);

    TEST_ASSERT(forwarded + atomic_load(&ctx.rejected) == N,
                "forwarded + rejected should account for all events");
    TEST_ASSERT(atomic_load(&ctx.rejected) > 0, "some events should have been rejected");

    const counters_t* c = bridge_counters(&bridge);
    TEST_ASSERT(atomic_load(&c->dropped) == atomic_load(&ctx.rejected),
                "dropped counter should match sink rejections");
    printf("  forwarded=%llu dropped=%llu\n",
           (unsigned long long)atomic_load(&c->forwarded),
           (unsigned long long)atomic_load(&c->dropped));

    bridge_detach(&bridge);
    tuxwarp_shutdown(handle);
}

int main(void) {
    printf("libtuxwarp Integration Test Suite v%s\n", VERSION);
    printf("=====================================\n\n");

    test_abi_metadata();
    test_mem_pool_api();
    test_namespace_coexistence();
    test_bridge_roundtrip();
    test_bridge_rejection();

    printf("\n=====================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
