/*
 * protocol.h — Selección del protocolo de red de libtuxwarp.
 *
 * libtuxwarp soporta dos protocolos de red MUTUAMENTE EXCLUYENTES por instancia:
 *
 *   - EtherCAT : esclavas con ASIC/FPGA dedicado, ciclos < 100 µs.
 *   - EtherTux : NICs y switches Ethernet comerciales estándar, SBC
 *                heterogéneas y MCUs con controlador Ethernet SPI/QSPI.
 *
 * La elección se resuelve EN TIEMPO DE CONFIGURACIÓN (variable de entorno
 * `PROTOCOL`, fichero de configuración o parámetro de línea de
 * comandos) y no cambia durante la ejecución: ambos backends comparten la HAL
 * de aceleración y el modelo de prioridades, pero sus ciclos de vida son
 * independientes.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "abi.h"
#include "libtuxwarp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTOCOL_ETHERCAT = 0,
    PROTOCOL_ETHERTUX  = 1
} protocol_t;

/* Protocolo por defecto cuando no se especifica ninguno (EtherCAT). */
API protocol_t protocol_default(void);

/* Acepta "ethercat" / "ethertux" (sin distinguir mayúsculas ni espacios). */
API protocol_t protocol_from_string(const char* name);

/* Nombre canónico ("ethercat" / "ethertux"); nunca NULL. */
API const char* protocol_name(protocol_t protocol);

/* Lee PROTOCOL del entorno; si no existe o no es válido, ETHERCAT. */
API protocol_t protocol_from_env(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
