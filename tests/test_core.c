#include "libtuxwarp.h"
#include "ring.h"
#include "mpmc.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

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
} while(0)

void test_init_shutdown(void) {
    config_t config = {0};
    config.event_pool_size = 4096;
    config.use_simd = true;
    config.use_cpu_affinity = false;

    handle_t handle;
    status_t status = init(&handle, &config);
    TEST_ASSERT(status == SUCCESS, "Init should succeed");

    if (status == SUCCESS) {
        status = tuxwarp_shutdown(handle);
        TEST_ASSERT(status == SUCCESS, "Shutdown should succeed");
    }
}

void test_event_publish_subscribe(void) {
    config_t config = {0};
    config.event_pool_size = 4096;
    
    handle_t handle;
    status_t status = init(&handle, &config);
    if (status != SUCCESS) {
        printf("[SKIP] Init failed\n");
        return;
    }
    
    uint32_t test_data = 0x12345678;
    status = event_publish(handle, &test_data, sizeof(test_data));
    TEST_ASSERT(status == SUCCESS, "Publish should succeed");
    
    event_t event;
    status = event_subscribe(handle, &event, 1000);
    TEST_ASSERT(status == SUCCESS, "Subscribe should succeed");
    
    tuxwarp_shutdown(handle);
}

void test_latency_measurement(void) {
    config_t config = {0};
    config.event_pool_size = 4096;
    config.use_simd = true;
    
    handle_t handle;
    status_t status = init(&handle, &config);
    if (status != SUCCESS) {
        printf("[SKIP] Init failed\n");
        return;
    }
    
    uint64_t start = get_timestamp_ns();
    uint64_t cycles = get_cycles();
    
    TEST_ASSERT(start > 0, "Timestamp should be valid");
    TEST_ASSERT(cycles > 0, "Cycle counter should be valid");
    
    uint32_t data = 0xDEADBEEF;
    for (int i = 0; i < 100; i++) {
        event_publish(handle, &data, sizeof(data));
    }

    event_t event;
    for (int i = 0; i < 100; i++) {
        status = event_subscribe(handle, &event, 100);
        if (status == SUCCESS) {
            break;
        }
    }

    tuxwarp_shutdown(handle);
}

void test_platform_detection(void) {
    platform_t platform = detect_platform();
    TEST_ASSERT(platform >= 0 && platform <= PLATFORM_GENERIC, 
                "Platform detection should return valid platform");
    printf("Detected platform: %d\n", platform);
}

void test_ring_buffer(void) {
    ring_t ring;
    ring_init(&ring);
    
    TEST_ASSERT(ring_is_empty(&ring), "Ring should be empty initially");
    TEST_ASSERT(!ring_is_full(&ring), "Ring should not be full initially");
    
    int item1 = 1, item2 = 2, item3 = 3;
    
    bool push1 = ring_push(&ring, &item1);
    bool push2 = ring_push(&ring, &item2);
    bool push3 = ring_push(&ring, &item3);
    
    TEST_ASSERT(push1 && push2 && push3, "All pushes should succeed");
    TEST_ASSERT(ring_count(&ring) == 3, "Ring count should be 3");
    
    void* pop1 = ring_pop(&ring);
    void* pop2 = ring_pop(&ring);
    void* pop3 = ring_pop(&ring);
    
    TEST_ASSERT(pop1 == &item1 && pop2 == &item2 && pop3 == &item3, 
                "Pop order should match push order");
    TEST_ASSERT(ring_is_empty(&ring), "Ring should be empty after all pops");
}

void test_mpmc_queue(void) {
    mpmc_queue_t queue;
    mpmc_init(&queue);
    
    TEST_ASSERT(mpmc_count(&queue) == 0, "Queue should be empty initially");
    
    int item1 = 100, item2 = 200, item3 = 300;
    
    bool push1 = mpmc_push(&queue, &item1);
    bool push2 = mpmc_push(&queue, &item2);
    bool push3 = mpmc_push(&queue, &item3);
    
    TEST_ASSERT(push1 && push2 && push3, "All pushes should succeed");
    TEST_ASSERT(mpmc_count(&queue) == 3, "Queue count should be 3");
    
    void* pop1 = mpmc_pop(&queue);
    void* pop2 = mpmc_pop(&queue);
    void* pop3 = mpmc_pop(&queue);
    
    TEST_ASSERT(pop1 == &item1 && pop2 == &item2 && pop3 == &item3,
                "Pop should return items in FIFO order");
    TEST_ASSERT(mpmc_count(&queue) == 0, "Queue should be empty after pops");
    
    mpmc_destroy(&queue);
}

void test_kernel_probe(void) {
    kernel_info_t info;
    status_t status = kernel_probe(&info);
    TEST_ASSERT(status == SUCCESS, "Kernel probe should succeed");
    TEST_ASSERT(info.major >= 1, "Kernel major version should be valid");
}

int main(void) {
    printf("libtuxwarp Test Suite v%s\n", VERSION);
    printf("=====================================\n\n");
    
    printf("Running initialization tests...\n");
    test_init_shutdown();
    printf("\n");
    
    printf("Running platform detection tests...\n");
    test_platform_detection();
    printf("\n");
    
    printf("Running event publish/subscribe tests...\n");
    test_event_publish_subscribe();
    printf("\n");
    
    printf("Running latency measurement tests...\n");
    test_latency_measurement();
    printf("\n");
    
    printf("Running ring buffer tests...\n");
    test_ring_buffer();
    printf("\n");
    
    printf("Running MPMC queue tests...\n");
    test_mpmc_queue();
    printf("\n");
    
    printf("Running kernel probe tests...\n");
    test_kernel_probe();
    printf("\n");
    
    printf("=====================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    
    return test_passed == test_count ? 0 : 1;
}

