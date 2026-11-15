/*
 * bridge.c — Puente de eventos libtuxwarp → sink externo.
 *
 * Genérico y desacoplado: libtuxwarp no conoce ni enlaza a libtuxrt_core. El
 * consumidor (p. ej. libtuxrt_core) conecta aquí el sink de su elección. La
 * dependencia del ecosistema es UNIDIRECCIONAL y se declara con el contrato
 * TuxRT Provider ABI (símbolo `tuxrt_provider_descriptor`, ver tuxrt_provider.h).
 */
#include "tuxrt.h"

#include <stdio.h>
#include <string.h>

status_t bridge_attach(bridge_t* bridge,
                                           handle_t handle,
                                           event_sink_fn sink,
                                           void* user) {
    if (!bridge || !handle || !sink) {
        return ERROR_INVALID;
    }

    bridge->handle = handle;
    bridge->sink = sink;
    bridge->user = user;
    counters_init(&bridge->counters);
    (void)trace_init(&bridge->trace);
    atomic_init(&bridge->attached, true);

    trace_mark(&bridge->trace, "libtuxwarp: bridge attach\n");
    return SUCCESS;
}

status_t bridge_detach(bridge_t* bridge) {
    if (!bridge) {
        return ERROR_INVALID;
    }
    atomic_store_explicit(&bridge->attached, false, memory_order_release);
    trace_mark(&bridge->trace, "libtuxwarp: bridge detach\n");
    trace_destroy(&bridge->trace);
    bridge->handle = NULL;
    bridge->sink = NULL;
    bridge->user = NULL;
    return SUCCESS;
}

uint64_t bridge_pump(bridge_t* bridge,
                               uint32_t max_events,
                               uint32_t timeout_us) {
    if (!bridge || !atomic_load_explicit(&bridge->attached, memory_order_acquire)) {
        return 0;
    }
    if (max_events == 0) {
        max_events = UINT32_MAX;
    }

    uint64_t forwarded = 0;
    for (uint32_t i = 0; i < max_events; i++) {
        event_t event;
        status_t st = event_subscribe(
            bridge->handle, &event, (i == 0) ? timeout_us : 0);

        if (st == ERROR_TIMEOUT) {
            break;
        }
        if (st != SUCCESS) {
            counters_add(&bridge->counters, &bridge->counters.errors, 1);
            break;
        }

        counters_add(&bridge->counters, &bridge->counters.consumed, 1);

        if (bridge->sink(&event, bridge->user) == 0) {
            forwarded++;
            counters_add(&bridge->counters, &bridge->counters.forwarded, 1);
        } else {
            counters_add(&bridge->counters, &bridge->counters.dropped, 1);
        }
    }

    return forwarded;
}

const counters_t* bridge_counters(const bridge_t* bridge) {
    return bridge ? &bridge->counters : NULL;
}

const char* tuxrt_namespace_info(void) {
    return "libtuxwarp " VERSION_STRING
           " (ABI 0x000100, prefijo *) "
           "coexiste con libtuxrt_core (prefijo tuxrt_*) sin colisión de símbolos";
}

