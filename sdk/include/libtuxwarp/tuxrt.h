/*
 * tuxrt.h — Coexistencia y puente de eventos hacia libtuxrt_core.
 *
 * libtuxwarp y libtuxrt_core están diseñadas para convivir en el MISMO proceso:
 *   - libtuxwarp exporta símbolos `*` (ABI propia, `API`).
 *   - libtuxrt_core exporta símbolos `tuxrt_*` (ABI C17 en `tuxrt_sdk.h`).
 *   - Los espacios de nombres son disjuntos => enlazado simultáneo sin colisión.
 *
 * DIRECCIÓN DE LA DEPENDENCIA (contrato TuxRT Provider ABI, `tuxrt_provider.h`):
 * el enlace es UNIDIRECCIONAL consumidor -> proveedor. libtuxwarp se publica
 * como PROVEEDOR exportando el símbolo `tuxrt_provider_descriptor` y NO enlaza
 * ni conoce a libtuxrt_core. Es el consumidor (libtuxrt_core) quien lo descubre
 * con dlopen() y dirige los eventos hacia su propia API.
 *
 * Por eso este encabezado sólo declara un puente GENÉRICO (sink desacoplado):
 * libtuxwarp produce eventos y los reenvía al `sink` que el consumidor conecte,
 * sin acoplarse a ningún símbolo `tuxrt_*`.
 */
#ifndef TUXRT_H
#define TUXRT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

#include "libtuxwarp.h"
#include "obs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sink genérico: devuelve 0 si consume el evento, distinto de 0 si lo rechaza. */
typedef int (*event_sink_fn)(const event_t* event, void* user);

typedef struct {
    handle_t        handle;
    event_sink_fn   sink;
    void*                     user;
    counters_t      counters;
    trace_t         trace;
    _Atomic bool              attached;
} bridge_t;

/* Conecta un handle de libtuxwarp con un sink de destino (p. ej. tuxrt_*). */
API status_t bridge_attach(bridge_t* bridge,
                                                         handle_t handle,
                                                         event_sink_fn sink,
                                                         void* user);

API status_t bridge_detach(bridge_t* bridge);

/* Drena hasta `max_events` eventos del handle y los reenvía al sink.
 * `timeout_us` se aplica a la primera lectura (0 = no bloqueante).
 * Devuelve el nº de eventos reenviados con éxito. */
API uint64_t bridge_pump(bridge_t* bridge,
                                             uint32_t max_events,
                                             uint32_t timeout_us);

API const counters_t* bridge_counters(const bridge_t* bridge);

/* Descripción textual de la coexistencia de espacios de nombres. */
API const char* tuxrt_namespace_info(void);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_H */
