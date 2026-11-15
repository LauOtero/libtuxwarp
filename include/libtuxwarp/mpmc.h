#ifndef MPMC_H
#define MPMC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct mpmc_node {
    _Alignas(64) void* data;
    _Atomic(struct mpmc_node*) next;
} mpmc_node_t;

typedef struct mpmc_queue {
    _Alignas(64) _Atomic(mpmc_node_t*) head;
    _Alignas(64) _Atomic(mpmc_node_t*) tail;
    _Alignas(64) _Atomic uint64_t count;
} mpmc_queue_t;

static inline void mpmc_init(mpmc_queue_t* queue) {
    mpmc_node_t* dummy = (mpmc_node_t*)malloc(sizeof(mpmc_node_t));
    dummy->data = NULL;
    dummy->next = NULL;
    
    atomic_init(&queue->head, dummy);
    atomic_init(&queue->tail, dummy);
    atomic_init(&queue->count, 0);
}

static inline void mpmc_destroy(mpmc_queue_t* queue) {
    mpmc_node_t* current = atomic_load(&queue->head);
    while (current != NULL) {
        mpmc_node_t* next = atomic_load(&current->next);
        free(current);
        current = next;
    }
}

static inline bool mpmc_push(mpmc_queue_t* queue, void* data) {
    mpmc_node_t* node = (mpmc_node_t*)malloc(sizeof(mpmc_node_t));
    if (!node) return false;
    
    node->data = data;
    atomic_init(&node->next, NULL);
    
    mpmc_node_t* tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    mpmc_node_t* null_ptr = NULL;
    
    while (!atomic_compare_exchange_weak_explicit(
        &tail->next,
        &null_ptr,
        node,
        memory_order_release,
        memory_order_relaxed
    )) {
        null_ptr = NULL;
        if (atomic_load(&tail->next) != NULL) {
            atomic_store_explicit(&queue->tail, atomic_load(&tail->next), memory_order_release);
            tail = atomic_load(&queue->tail);
        }
    }
    
    atomic_store_explicit(&queue->tail, node, memory_order_release);
    atomic_fetch_add_explicit(&queue->count, 1, memory_order_release);
    
    return true;
}

static inline void* mpmc_pop(mpmc_queue_t* queue) {
    atomic_fetch_sub_explicit(&queue->count, 1, memory_order_release);
    
    mpmc_node_t* head = atomic_load_explicit(&queue->head, memory_order_acquire);
    mpmc_node_t* next = atomic_load_explicit(&head->next, memory_order_acquire);
    
    if (next == NULL) {
        atomic_fetch_add_explicit(&queue->count, 1, memory_order_release);
        return NULL;
    }
    
    void* data = next->data;
    atomic_store_explicit(&queue->head, next, memory_order_release);
    free(head);
    
    return data;
}

static inline uint64_t mpmc_count(mpmc_queue_t* queue) {
    return atomic_load_explicit(&queue->count, memory_order_acquire);
}

#endif

