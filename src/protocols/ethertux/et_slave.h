/*
 * et_slave.h — Esclavo EtherTux: operación básica de envío y recepción.
 *
 * A diferencia del maestro, el esclavo no gestiona:
 *   - Tabla de nodos ni descubrimiento
 *   - Admisión QoS ni programacion de ventanas
 *   - ET-WKC ni monitoreo topológico
 *   - FEC ni redundancia FRM (soporte básico opcional)
 *
 * El esclavo puede enviar y recibir tramas en la misma interfaz EtherTux,
 * filtrando por destino (MAC local o difusión) y usando la máquina de
 * estados ET-ESM simplificada.
 */
#ifndef ET_SLAVE_H
#define ET_SLAVE_H

#include "ethertux.h"

/* ------------------------------------------------------------------ */
/* Modo de operación                                                   */
/* ------------------------------------------------------------------ */

#define ET_SLAVE_MODE  0u
#define ET_MASTER_MODE 1u

/* ------------------------------------------------------------------ */
/** Contexto del esclavo
 *
 * El contexto del esclavo comparte la mayor parte de la estructura
 * de red subyacente (et_port_t) pero omite los campos específicos
 * del maestro (tabla de nodos, planificador, topología, etc.).
 */
/* ------------------------------------------------------------------ */

/* Estructura opaca para el contexto del esclave */
typedef struct et_slave et_slave_t;

/* et_slave_stats_t está definido en ethertux.h */

/* ------------------------------------------------------------------ */
/* Estadísticas y versión                                              */
/* ------------------------------------------------------------------ */

/*
 * Crear un nuevo contexto de esclave.
 * - `ifname`: nombre de la interfaz de red (ej. "eth0")
 * - `link_bps`: velocidad del enlace (0 = 1 Gbps por defecto)
 * - `cycle_ns`: periodo del superframe en ns (0 = 1 ms por defecto)
 * Retorna NULL si falla la asignación.
 */
et_slave_t* et_slave_create(const char* ifname,
                                                 uint64_t link_bps,
                                                 uint32_t cycle_ns);

/* Destruir un contexto de esclave y liberar todos los recursos. */
void et_slave_destroy(et_slave_t* slave);

/* ------------------------------------------------------------------ */
/* Apertura y cierre de la interfaz                                    */
/* ------------------------------------------------------------------ */

/*
 * Abre la interfaz EtherTux para el modo esclave.
 * Igual que cronosrt_et_master_open_ex pero retorna un contexto de esclave.
 * El esclave no requiere configuración de QoS, scheduler ni topología.
 */
status_t et_slave_open(et_slave_t* slave,
                                             const char* ifname);

/*
 * Cierra la interfaz y libera los recursos del esclave.
 */
void et_slave_close(et_slave_t* slave);

/* ------------------------------------------------------------------ */
/* Estado y estadísticas                                               */
/* ------------------------------------------------------------------ */

/*
 * Retorna el modo actual (MASTER o SLAVE).
 */
uint8_t et_slave_mode(const et_slave_t* slave);

/*
 * Retorna las estadísticas de operación del esclave.
 */
status_t et_slave_stats(const et_slave_t* slave,
                                              et_slave_stats_t* out);

/* ------------------------------------------------------------------ */
/* Envío de tramas                                                     */
/* ------------------------------------------------------------------ */

/*
 * Envía una trama EtherTux sin destino específico (difusión).
 * Equivalente a master_send con dest_id == LOCAL_ID_NONE.
 */
status_t et_slave_send(et_slave_t* slave,
                                            uint8_t priority,
                                            et_type_t type,
                                            uint8_t flow_id,
                                            const void* payload,
                                            uint16_t len);

/*
 * Envía una trama a un destino unicast usando ID local.
 * `dest_id` es el ID local del nodo destino (1..65535).
 * `dest_id == ET_LOCAL_ID_NONE` equivale a difusión.
 */
status_t et_slave_send_to(et_slave_t* slave,
                                               uint16_t dest_id,
                                               uint8_t priority,
                                               et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload,
                                               uint16_t len);

/*
 * Envía una trama con pacing por hardware (SO_TXTIME) si la NIC lo soporta.
 */
status_t et_slave_send_at(et_slave_t* slave,
                                               uint8_t priority,
                                               et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload,
                                               uint16_t len,
                                               uint64_t tx_deadline_ns);

/*
 * Envía hasta 16 tramas en una sola llamada usando agregación de flujo.
 * Las tramas se envían concatenadas en un solo PDU.
 */
status_t et_slave_send_aggregated(et_slave_t* slave,
                                                        uint8_t priority,
                                                        et_type_t type,
                                                        uint8_t flow_id,
                                                        const et_flow_entry_t* entries,
                                                        uint32_t count);

/* ------------------------------------------------------------------ */
/* Recepción de tramas                                                 */
/* ------------------------------------------------------------------ */

/*
 * Recibe una trama EtherTux.
 * El esclave filtra por destino: acepta difusión y tramas dirigidas a
 * su propia MAC local. Las tramas unicast ajenas se descartan.
 * Devuelve los bytes de carga útil, o -1 en timeout/error.
 */
int et_slave_recv(et_slave_t* slave,
                             et_header_t* hdr,
                             void* payload,
                             uint32_t bufsize,
                             uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Máquina de estados ET-ESM (simplificada para esclavo)               */
/* ------------------------------------------------------------------ */

/*
 * Reinicia la máquina de estados ET-ESM al estado INIT.
 */
void et_slave_esm_reset(et_slave_t* slave);

/*
 * Retorna el estado actual de la máquina ET-ESM.
 */
et_state_t et_slave_esm_state(const et_slave_t* slave);

/*
 * Solicita una transición de estado en la máquina ET-ESM.
 * Retorna SUCCESS si la transición es válida, ERROR_STATE en caso contrario.
 */
status_t et_slave_esm_request(et_slave_t* slave,
                                                   et_state_t next);

/* ------------------------------------------------------------------ */
/* Mailbox (ET-MBX) simplificado para esclavo                        */
/* ------------------------------------------------------------------ */

/*
 * Inicia una transferencia en un canal lógico del mailbox.
 * Solo se admiten canales 0..7.
 */
status_t et_slave_mbx_begin(et_slave_t* mbx,
                                                 uint8_t channel,
                                                 uint32_t total_bytes,
                                                 bool firmware);

/*
 * Retorna el estado actual del canal mailbox.
 */
status_t et_slave_mbx_status(const et_slave_t* mbx,
                                                  uint8_t channel,
                                                  et_mbx_channel_status_t* out);

/* ------------------------------------------------------------------ */
/* ID local determinista                                               */
/* ------------------------------------------------------------------ */

/*
 * Activa el esquema de ID local (Nivel 2) en el esclave.
 * A partir de aquí la recepción descarta las tramas cuyo destino
 * no sea la MAC local ni difusión.
 */
status_t et_slave_enable_local_ids(et_slave_t* slave,
                                                        const et_aalid_config_t* cfg);

/* ------------------------------------------------------------------ */
/* Sincronización ET-Sync (opcional para esclavo)                     */
/* ------------------------------------------------------------------ */

/*
 * Configura el retardo de enlace calibrado para un nodo.
 * El offset reportado se corrige restándole este retardo.
 */
status_t et_slave_sync_set_link_delay(et_slave_t* slave,
                                                           uint16_t node_id,
                                                           uint32_t delay_ns);

/*
 * Procesa un informe de sincronización y clasifica el estado.
 */
status_t et_slave_sync_process_report(et_slave_t* slave,
                                                           uint16_t node_id,
                                                           int64_t measured_offset_ns,
                                                           uint64_t now_ns,
                                                           et_sync_status_t* out_status);

/* ------------------------------------------------------------------ */
/* estadísticas internas                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Versión y compatibilidad                                            */
/* ------------------------------------------------------------------ */

/** Versión del protocolo de esclave soportado. */
#define ET_SLAVE_PROTO_VERSION 0x0100u

#endif /* ET_SLAVE_H */