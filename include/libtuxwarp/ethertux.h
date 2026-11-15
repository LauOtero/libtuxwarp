/*
 * ethertux.h — Protocolo EtherTux de libtuxwarp (API pública, C11).
 *
 * EtherTux es el tejido de comunicación determinista para Ethernet COMERCIAL
 * (NICs y switches estándar, SBC heterogéneas y MCUs con controlador Ethernet
 * SPI/QSPI). Convive con el maestro EtherCAT: el operador elige uno u otro en
 * tiempo de configuración (`protocol.h`), nunca ambos a la vez sobre
 * la misma interfaz.
 *
 * Principios de diseño (blindaje absoluto):
 *
 *   1. Time-triggered para P0–P2: cada trama crítica tiene su ranura en un
 *      superframe; no hay contención.
 *   2. Antibloqueo: P0 es "express"; el planificador no encola tráfico de baja
 *      prioridad si hay una P0 pendiente (y la NIC puede preemptar si lo
 *      soporta).
 *   3. FEC Reed-Solomon: recuperación de paquetes perdidos SIN retransmisión.
 *   4. Cero congestión por diseño: control de admisión + token bucket
 *      jerárquico por clase + backpressure consciente de prioridad.
 *   5. LAPM: migración dinámica de prioridad con histéresis bajo carga.
 *   6. CRC-32C por instrucción en todas las SBCs (ARMv8.1+ / SSE4.2).
 *
 * Trama: Ethernet II con EtherType 0x88B5 y cabecera EtherTux de 22 bytes.
 * (El informe indica "20 bytes", pero la suma de los campos de su tabla es 22;
 * se implementa la tabla, que es la fuente de verdad de los campos.)
 */
#ifndef ETHERTUX_H
#define ETHERTUX_H

#include "abi.h"
#include "libtuxwarp.h"
#include "hwaccel.h"

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations for internal types */
struct et_master;
struct et_slave;
typedef struct et_master et_master_t;
typedef struct et_slave et_slave_t;

#ifdef __cplusplus
extern "C" {
#endif

/* EtherType propio de EtherTux. */
#define ET_ETHERTYPE 0x88B5

/* Geometría (cabecera v0.1.0: 26 B en el cable). */
#define ET_HEADER_SIZE  26u    /* cabecera EtherTux en el cable   */
#define ET_ETH_HDR_SIZE 14u    /* cabecera Ethernet II            */
#define ET_MAX_FRAME    1518u  /* MTU Ethernet estándar           */
#define ET_MAX_PAYLOAD  1478u  /* MAX_FRAME - ETH_HDR - HEADER    */
#define ET_MAX_NODES    64u
#define ET_MAX_FLOWS    255u
#define ET_PRIORITIES   6u
#define ET_P0_MAX_FRAME 64u    /* las P0 se limitan a 64 B (§4.2) */

/* Tipos de trama (campo `Type`, §4.3). */
typedef enum {
    ET_TYPE_DATA            = 0x01,  /* datos / setpoints        */
    ET_TYPE_FEC             = 0x02,  /* paridad Reed-Solomon     */
    ET_TYPE_CONTROL         = 0x03,  /* emergencia, ACK/NACK     */
    ET_TYPE_SYNC            = 0x04,  /* sincronización básica    */
    ET_TYPE_LAPM            = 0x05,  /* migración de prioridad   */
    ET_TYPE_DISCOVERY       = 0x06,  /* Discovery / DiscoveryReply */
    ET_TYPE_SYNC_CALIBRATED = 0x07,  /* ET-Sync calibrado        */
    ET_TYPE_STATE_CHANGE    = 0x08,  /* transición ET-ESM        */
    ET_TYPE_MAILBOX_DATA    = 0x09,  /* ET-MBX: datos            */
    ET_TYPE_MAILBOX_ACK     = 0x0A,  /* ET-MBX: ACK/NACK         */
    ET_TYPE_SYNC_REPORT     = 0x0B,  /* Monitor de sincronización */
    ET_TYPE_TOPO_CHECK      = 0x0C   /* verificación topológica  */
} et_type_t;

/* Niveles de prioridad / clase de servicio. */
typedef enum {
    ET_P0 = 0,   /* express: emergencia, ACK/NACK crítico  */
    ET_P1 = 1,   /* express: sync, heartbeat, LAPM         */
    ET_P2 = 2,   /* time-triggered: setpoints de control   */
    ET_P3 = 3,   /* rate-constrained: telemetría           */
    ET_P4 = 4,   /* rate-constrained: configuración        */
    ET_P5 = 5    /* best-effort: logs, firmware, bulk      */
} et_priority_t;

/* Bits del campo `Flags` (§4.2). */
#define ET_FLAG_FEC          0x01u
#define ET_FLAG_PREEMPTABLE  0x02u
#define ET_FLAG_EXPRESS      0x04u
#define ET_FLAG_DISCOVERY    0x08u
#define ET_FLAG_FRM          0x10u   /* duplicado de redundancia FRM */

/* Estados ET-ESM del nodo emisor (§6.1). */
typedef enum {
    ET_STATE_INIT   = 0,   /* solo Discovery                     */
    ET_STATE_CONFIG = 1,   /* Discovery + mailbox                */
    ET_STATE_SAFE   = 2,   /* P0, P1 y mailbox                   */
    ET_STATE_OP     = 3,   /* P0–P5                              */
    ET_STATE_FAULT  = 4    /* cuarentena: solo Discovery         */
} et_state_t;

/* Cabecera EtherTux deserializada (v0.1.0, 26 B en el cable). */
typedef struct {
    uint8_t  type;          /* et_type_t                       */
    uint8_t  priority;      /* et_priority_t                   */
    uint8_t  flags;         /* ET_FLAG_*                       */
    uint8_t  state;         /* et_state_t del emisor           */
    uint8_t  flow_id;       /* 1..255                                    */
    uint32_t seq_num;       /* secuencia por flujo                       */
    uint64_t tx_timestamp;  /* ns del reloj monotónico en la transmisión */
    uint64_t rx_timestamp;  /* ns del reloj monotónico en la recepción */
    uint16_t wkc;           /* ET-WKC: nodos que procesaron la trama     */
    uint16_t block_id;      /* (bloque << 8) | índice dentro del bloque  */
    uint8_t  flow_count;    /* flujos agrupados (1 = sin agregación)     */
    uint32_t crc32;         /* CRC-32C de cabecera + carga útil          */
} et_header_t;

/* ------------------------------------------------------------------ */
/* Trama y CRC (helpers puros, verificables sin hardware)              */
/* ------------------------------------------------------------------ */

/** Serializa la cabecera (22 B, little-endian). Devuelve 22. */
API uint32_t et_header_build(uint8_t out[ET_HEADER_SIZE],
                                                const et_header_t* h);

/** Construye cabecera + carga útil y rellena el CRC-32C. Devuelve los bytes. */
API uint32_t et_pdu_build(uint8_t* out, uint32_t out_size,
                                              const et_header_t* h,
                                              const void* payload, uint16_t len);

/**
 * Construye SOLO la cabecera (26 B) con el CRC-32C ya calculado sobre
 * cabecera + carga útil, **sin copiar la carga útil**. Permite enviar con
 * `sendmsg`/iovec (cero copias de la aplicación) y evita el `memcpy` por trama.
 * Devuelve 26, o 0 si los parámetros no son válidos.
 */
API uint32_t et_pdu_build_header(uint8_t out[ET_HEADER_SIZE],
                                                     const et_header_t* h,
                                                     const void* payload, uint16_t len);

/* ------------------------------------------------------------------ */
/* Tasa de trama alcanzable (física del enlace)                        */
/* ------------------------------------------------------------------ */

/** Bytes en el cable de una trama: 14 Eth + 26 EtherTux + n + 4 FCS + 20 (preámbulo+IFG). */
API uint32_t et_wire_frame_bytes(uint32_t payload_len);

/**
 * Tramas por segundo que permite el enlace para esa carga útil (redondeo hacia
 * abajo). Es el techo **físico**: p. ej. 1 Gbps y 1464 B ⇒ ~81 800 tramas/s;
 * con 20 B (trama mínima de 64 B en el cable) ⇒ ~1 488 000 tramas/s.
 */
API uint32_t et_wire_pps(uint64_t link_bps, uint32_t payload_len);

/** Carga útil máxima que cabe a `pps` tramas/s en un enlace de `link_bps`. */
API uint32_t et_payload_for_pps(uint64_t link_bps, uint32_t pps);

/**
 * Deserializa y VALIDA el CRC. Devuelve los bytes consumidos o 0 si la trama
 * está mal formada o corrupta. `payload` apunta dentro de `in`.
 */
API uint32_t et_pdu_parse(const uint8_t* in, uint32_t in_len,
                                              et_header_t* h,
                                              const uint8_t** payload);

/** CRC-32C de cabecera (con el campo crc a cero) + carga útil. */
API uint32_t et_crc(const et_header_t* h,
                                        const void* payload, uint16_t len);

/** Nombre legible del tipo de trama. */
API const char* et_type_name(et_type_t type);

/* ------------------------------------------------------------------ */
/* FEC Reed-Solomon bloque a bloque                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    ET_FEC_OFF    = 0,   /* sin FEC                          */
    ET_FEC_LIGHT  = 1,   /* RS(10,8): 25 % de overhead       */
    ET_FEC_ROBUST = 2    /* RS(16,8): 100 % de overhead      */
} et_fec_mode_t;

typedef struct et_fec et_fec_t;

/** Crea un codec RS(n,k) explícito. Requiere 1 <= k < n <= 64. */
API et_fec_t* et_fec_create(uint8_t k, uint8_t n);
/** Crea el codec asociado a un modo predefinido (OFF => NULL). */
API et_fec_t* et_fec_create_mode(et_fec_mode_t mode);
API void et_fec_destroy(et_fec_t* fec);

API uint8_t  et_fec_k(const et_fec_t* fec);
API uint8_t  et_fec_n(const et_fec_t* fec);
API uint32_t et_fec_overhead_percent(const et_fec_t* fec);

/** Codifica k bloques de datos en n-k bloques de paridad. */
API status_t et_fec_encode(const et_fec_t* fec,
                                                         const uint8_t* const* data,
                                                         uint8_t* const* parity,
                                                         uint32_t len);

/** Reconstruye in situ hasta n-k bloques perdidos. */
API status_t et_fec_decode(const et_fec_t* fec,
                                                         uint8_t* const* blocks,
                                                         const bool* present,
                                                         uint32_t len);

/* ------------------------------------------------------------------ */
/* QoS: token bucket jerárquico, backpressure y LAPM                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t rate_bytes_per_sec;   /* tasa garantizada de la clase    */
    uint32_t burst_bytes;          /* ráfaga máxima admitida          */
} et_bucket_cfg_t;

typedef struct {
    et_bucket_cfg_t bucket[ET_PRIORITIES];
    uint8_t pause_percent;      /* backpressure P0 (def. 80 %)         */
    bool    lapm_enabled;       /* migración dinámica bajo carga       */
    uint8_t lapm_high_percent;  /* umbral de subida (def. 80 %)        */
    uint8_t lapm_low_percent;   /* umbral de bajada (def. 60 %, hist.) */
} et_qos_config_t;

typedef struct {
    uint64_t admitted;
    uint64_t backpressure;
    uint64_t dropped;
    uint64_t degraded;
    uint64_t bytes_admitted;
} et_qos_stats_t;

typedef struct et_qos et_qos_t;

API et_qos_t* et_qos_create(void);
API void et_qos_destroy(et_qos_t* qos);
API void et_qos_default_config(et_qos_config_t* cfg);
API status_t et_qos_configure(et_qos_t* qos,
                                                            const et_qos_config_t* cfg);

/**
 * Control de admisión. Devuelve:
 *   SUCCESS                 -> admitida (consume tokens)
 *   ERROR_BACKPRESSURE -> P0–P4 no admitida: el productor debe esperar
 *   ERROR_DROPPED      -> P5 descartada (best-effort)
 * La degradación entre clases (P1→P2→P4, P4→P5) es automática y con
 * histéresis.
 */
API status_t et_qos_admit(et_qos_t* qos,
                                                        uint8_t priority, uint32_t bytes,
                                                        uint64_t now_ns);

/** `true` si el productor de esa clase debe pausarse (backpressure activa). */
API bool et_qos_backpressure(const et_qos_t* qos, uint8_t priority);

/** Prioridad efectiva tras aplicar LAPM (puede ser mayor que la nominal). */
API uint8_t et_qos_effective_priority(const et_qos_t* qos,
                                                           uint8_t priority);

API status_t et_qos_stats(const et_qos_t* qos,
                                                        et_qos_stats_t* out);
API void et_qos_reset_stats(et_qos_t* qos);

/* ------------------------------------------------------------------ */
/* Planificador time-triggered (superframe con ventanas dedicadas)    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  priority;
    uint16_t node;
    uint16_t frame_bytes;
    uint32_t offset_ns;     /* desplazamiento dentro del superframe */
} et_slot_t;

typedef struct et_sched et_sched_t;

/** Crea el planificador. `link_bps` = 0 usa 1 Gbps. `cycle_ns` = 0 usa 1 ms. */
API et_sched_t* et_sched_create(uint32_t cycle_ns, uint64_t link_bps);
API void et_sched_destroy(et_sched_t* sched);

/** Reparte el superframe en las cuatro ventanas (control, determinista, rate, BE). */
API status_t et_sched_set_windows(et_sched_t* sched,
                                                                uint32_t control_ns,
                                                                uint32_t deterministic_ns,
                                                                uint32_t rate_ns,
                                                                uint32_t best_effort_ns);

API status_t et_sched_add_slot(et_sched_t* sched,
                                                             uint8_t priority, uint16_t node,
                                                             uint16_t frame_bytes,
                                                             uint32_t offset_ns);
API int  et_sched_remove_node(et_sched_t* sched, uint16_t node);
API uint32_t et_sched_slot_count(const et_sched_t* sched);
API bool et_sched_slot(const et_sched_t* sched, uint32_t index,
                                           et_slot_t* out);
API uint32_t et_sched_cycle_ns(const et_sched_t* sched);

/** Ventana activa en `now_ns` (0=control, 1=determinista, 2=rate, 3=BE). */
API uint8_t et_sched_window_at(const et_sched_t* sched, uint64_t now_ns);

/** `true` si esa prioridad puede transmitir en la ventana actual. */
API bool et_sched_can_transmit(const et_sched_t* sched,
                                                    uint8_t priority, uint64_t now_ns);

/** Tiempo de transmisión de una trama a la tasa del enlace. */
API uint64_t et_tx_time_ns(uint32_t frame_bytes, uint64_t link_bps);

/* ------------------------------------------------------------------ */
/* Flow Aggregator (§7.6): varias entradas (FlowID, len, data)        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t        flow_id;
    uint16_t       len;
    const uint8_t* data;
} et_flow_entry_t;

/** Serializa la lista de flujos como carga útil agregada. */
API status_t et_flowagg_build(const et_flow_entry_t* entries,
                                                            uint32_t count,
                                                            uint8_t* out, uint32_t out_size,
                                                            uint32_t* out_len);

/** Deserializa la carga útil agregada en vistas sobre el propio búfer. */
API status_t et_flowagg_parse(const uint8_t* in, uint32_t len,
                                                            et_flow_entry_t* views,
                                                            uint32_t max_views, uint32_t* out_count);

/**
 * Bytes de cabecera ahorrados frente a enviar cada flujo en su propia trama.
 * Cada entrada cuesta 3 B (FlowID + longitud) en lugar de una cabecera EtherTux
 * completa (26 B) más la de Ethernet (14 B).
 */
API uint32_t et_flowagg_saved_bytes(uint32_t count, uint32_t data_bytes);

/* ------------------------------------------------------------------ */
/* ET-ESM: máquina de estados del nodo (§6.1)                          */
/* ------------------------------------------------------------------ */

typedef struct et_esm et_esm_t;

API et_esm_t* et_esm_create(void);
API void et_esm_destroy(et_esm_t* esm);
/** Vuelve a `INIT` (arranque o recuperación desde `FAULT`). */
API void et_esm_reset(et_esm_t* esm);
API et_state_t et_esm_state(const et_esm_t* esm);

/** Transiciones válidas: INIT→CONFIG→SAFE→OP y cualquiera→FAULT; FAULT→INIT. */
API bool et_esm_can_transition(et_state_t from,
                                                   et_state_t to);
/** Aplica una transición validada (el maestro la difunde vía `StateChange`). */
API status_t et_esm_request(et_esm_t* esm,
                                                          et_state_t next);

/** Tráfico permitido en cada estado (§6.1). */
API bool et_esm_allows_traffic(et_state_t state,
                                                   uint8_t type, uint8_t priority);

/** Carga útil de `StateChange`: estado destino (1 B) + motivo (1 B). */
API uint32_t et_esm_build_state_change(et_state_t target,
                                                           uint8_t reason,
                                                           uint8_t out[2]);
API status_t et_esm_parse_state_change(const uint8_t* in, uint32_t len,
                                                                    et_state_t* target,
                                                                    uint8_t* reason);
/** Valida el campo `State` de una trama; falso si no es un estado conocido. */
API bool et_esm_state_from_wire(uint8_t raw, et_state_t* out);

/* ------------------------------------------------------------------ */
/* ET-Sync + Monitor de Ventana de Sincronización (§5)                 */
/* ------------------------------------------------------------------ */

typedef enum {
    ET_SYNC_OK = 0,
    ET_SYNC_WARN = 1,
    ET_SYNC_OUT_OF_SYNC = 2
} et_sync_status_t;

typedef struct {
    uint32_t warn_threshold_ns;         /* desviación para WARN (def. 1 µs)   */
    uint32_t out_of_sync_threshold_ns;  /* desviación para fuera de sync (2 µs) */
    uint32_t report_period_frames;      /* periodo de SyncReport (def. 10)    */
} et_sync_config_t;

typedef struct {
    uint32_t nodes;
    uint32_t ok;
    uint32_t warn;
    uint32_t out_of_sync;
    uint64_t max_abs_offset_ns;
} et_sync_stats_t;

typedef struct et_sync et_sync_t;

API et_sync_t* et_sync_create(void);
API void et_sync_destroy(et_sync_t* sync);
API void et_sync_default_config(et_sync_config_t* cfg);
API status_t et_sync_configure(et_sync_t* sync,
                                                             const et_sync_config_t* cfg);
/**
 * Modelo de retardo calibrado del switch para un nodo (medido en `CONFIG`).
 * El offset reportado se corrige restándole este retardo.
 */
API status_t et_sync_set_link_delay(et_sync_t* sync,
                                                                  uint16_t node_id,
                                                                  uint32_t delay_ns);
/** Procesa un `SyncReport` (offset medido por el nodo) y clasifica el estado. */
API status_t et_sync_process_report(et_sync_t* sync,
                                                                  uint16_t node_id,
                                                                  int64_t measured_offset_ns,
                                                                  uint64_t now_ns,
                                                                  et_sync_status_t* out_status);
API status_t et_sync_status_of(const et_sync_t* sync,
                                                             uint16_t node_id,
                                                             int64_t* offset_ns,
                                                             et_sync_status_t* out_status);
/** Deriva estimada en partes por billón (Δoffset/Δt). */
API status_t et_sync_drift_ppb(const et_sync_t* sync,
                                                             uint16_t node_id,
                                                             int64_t* out_ppb);
API status_t et_sync_stats(const et_sync_t* sync,
                                                         et_sync_stats_t* out);
/** Carga útil de `SyncCalibrated`: timestamp de referencia del maestro (8 B). */
API uint32_t et_sync_build_calibrated(uint64_t master_ns, uint8_t out[8]);
API status_t et_sync_parse_calibrated(const uint8_t* in, uint32_t len,
                                                                   uint64_t* out_master_ns);
/** Carga útil de `SyncReport`: offset medido por el nodo (8 B, int64). */
API uint32_t et_sync_build_report(int64_t offset_ns, uint8_t out[8]);
API status_t et_sync_parse_report(const uint8_t* in, uint32_t len,
                                                               int64_t* out_offset_ns);

/* ------------------------------------------------------------------ */
/* Registro de nodos, ET-WKC y Hot Connect (§6.2–6.5)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t id;                  /* NodeID                                     */
    uint8_t  mac[6];
    uint16_t link_mbps;           /* capacidad declarada                        */
    uint16_t buffer_bytes;
    bool     fec_supported;
    uint16_t slot;                /* ranura TDMA (0xFFFF = libre)               */
    et_state_t state;   /* estado ET-ESM                              */
    uint16_t last_wkc;            /* último ET-WKC observado                    */
    uint16_t expected_wkc;        /* ET-WKC esperado según el registro          */
    uint8_t  wkc_mismatch_count;  /* discrepancias consecutivas                 */
    int64_t  sync_offset_ns;
    et_sync_status_t sync_status;
    uint64_t last_seen_ns;        /* temporizador de heartbeat                  */
} et_node_info_t;

typedef struct {
    uint8_t  wkc_mismatch_threshold;    /* def. 3 superframes                  */
    uint32_t hot_connect_period_frames; /* def. 100 superframes (100 ms)       */
    uint32_t heartbeat_timeout_frames;  /* def. 3 superframes                  */
} et_topology_config_t;

#define ET_SLOT_FREE 0xFFFFu

/** Tamaño del `DiscoveryReply` en el cable. */
#define ET_DISCOVERY_REPLY_SIZE 8u

typedef struct et_topology et_topology_t;

API et_topology_t* et_topology_create(void);
API void et_topology_destroy(et_topology_t* topo);
API void et_topology_default_config(et_topology_config_t* cfg);
API status_t et_topology_configure(et_topology_t* topo,
                                                                 const et_topology_config_t* cfg);

/** Inserta o actualiza un nodo del registro. */
API status_t et_topology_upsert(et_topology_t* topo,
                                                              const et_node_info_t* info);
API status_t et_topology_remove(et_topology_t* topo,
                                                              uint16_t node_id);
API const et_node_info_t* et_topology_find(const et_topology_t* topo,
                                                                         uint16_t node_id);
API uint32_t et_topology_count(const et_topology_t* topo);
API const et_node_info_t* et_topology_at(const et_topology_t* topo,
                                                                      uint32_t index);
API uint16_t et_topology_next_free_slot(const et_topology_t* topo);
API status_t et_topology_assign_slot(et_topology_t* topo,
                                                                   uint16_t node_id,
                                                                   uint16_t slot);

/**
 * Procesa una trama recibida: refresca el heartbeat, el estado ET-ESM y compara
 * el ET-WKC con el esperado (incrementando `wkc_mismatch_count` al discrepar).
 */
API status_t et_topology_on_frame(et_topology_t* topo,
                                                                const uint8_t mac[6],
                                                                const et_header_t* h,
                                                                uint64_t now_ns);
/** `true` si algún nodo superó el umbral de discrepancias y toca `TopoCheck`. */
API bool et_topology_needs_topo_check(const et_topology_t* topo,
                                                          uint16_t* out_node_id);
/** Marca en `FAULT` los nodos sin heartbeat y libera sus slots. */
API uint32_t et_topology_tick(et_topology_t* topo,
                                                  uint64_t now_ns, uint32_t cycle_ns);
/** `true` si toca un `Discovery` unicast a una dirección libre (Hot Connect). */
API bool et_topology_hot_connect_due(const et_topology_t* topo,
                                                         uint64_t frame_index,
                                                         uint16_t* out_node_addr);

API uint32_t et_topology_build_reply(const et_node_info_t* info,
                                                         uint8_t out[ET_DISCOVERY_REPLY_SIZE]);
API status_t et_topology_parse_reply(const uint8_t* in, uint32_t len,
                                                                   et_node_info_t* out);

/* ------------------------------------------------------------------ */
/* ET-MBX: mailbox de gestión (§9)                                     */
/* ------------------------------------------------------------------ */

#define ET_MBX_CHANNELS       8u
#define ET_MBX_SEGMENT_BYTES  512u
#define ET_MBX_MAX_RETRIES    3u
#define ET_MBX_ACK_SIZE       4u

typedef enum {
    ET_MBX_IDLE = 0,
    ET_MBX_SENDING,
    ET_MBX_WAIT_ACK,
    ET_MBX_DONE,
    ET_MBX_FAILED
} et_mbx_state_t;

typedef struct {
    uint8_t  max_retries;        /* def. 3                                      */
    uint16_t segment_bytes;      /* def. 512                                    */
    uint8_t  p4_degrade_percent; /* def. 70: por encima se degrada a P5          */
} et_mbx_config_t;

typedef struct {
    et_mbx_state_t state;
    uint16_t seq;
    uint16_t segment;
    uint16_t total_segments;
    uint8_t  retries;
    bool     firmware;           /* firmware => P5                              */
} et_mbx_channel_status_t;

typedef struct et_mbx et_mbx_t;

API et_mbx_t* et_mbx_create(void);
API void et_mbx_destroy(et_mbx_t* mbx);
API void et_mbx_default_config(et_mbx_config_t* cfg);
API status_t et_mbx_configure(et_mbx_t* mbx,
                                                            const et_mbx_config_t* cfg);
/** Inicia una transferencia en un canal lógico (0..7). */
API status_t et_mbx_begin(et_mbx_t* mbx, uint8_t channel,
                                                        uint32_t total_bytes, bool firmware);
API status_t et_mbx_status(const et_mbx_t* mbx,
                                                         uint8_t channel,
                                                         et_mbx_channel_status_t* out);
/**
 * Construye el siguiente segmento (`MailboxData`): canal, seq, índice, total,
 * flags y hasta `segment_bytes` de datos. Devuelve los bytes escritos.
 */
API status_t et_mbx_next_segment(et_mbx_t* mbx,
                                                               uint8_t channel,
                                                               const uint8_t* data, uint32_t len,
                                                               uint8_t* out, uint32_t out_size,
                                                               uint32_t* out_len);
/** Procesa el ACK/NACK del nodo; programa el reintento con backoff si procede. */
API status_t et_mbx_on_ack(et_mbx_t* mbx, uint8_t channel,
                                                         uint16_t seq, bool accepted,
                                                         uint64_t now_ns);
/** `true` si el reintento pendiente ya venció (backoff exponencial). */
API bool et_mbx_retry_due(const et_mbx_t* mbx, uint8_t channel,
                                              uint64_t now_ns);
/** Prioridad efectiva del mailbox según la ocupación de P4. */
API uint8_t et_mbx_effective_priority(const et_mbx_t* mbx,
                                                          uint8_t p4_occupancy_percent);
API uint32_t et_mbx_build_ack(uint8_t channel, uint16_t seq, bool accepted,
                                                  uint8_t out[ET_MBX_ACK_SIZE]);
API status_t et_mbx_parse_ack(const uint8_t* in, uint32_t len,
                                                            uint8_t* channel, uint16_t* seq,
                                                            bool* accepted);
API status_t et_mbx_parse_data(const uint8_t* in, uint32_t len,
                                                             uint8_t* channel, uint16_t* seq,
                                                             uint16_t* index, uint16_t* total,
                                                             const uint8_t** data, uint16_t* data_len);

/* ------------------------------------------------------------------ */
/* FRM: redundancia lógica de flujo (§8.2)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t duplicates_tx;
    uint64_t duplicates_rx;
    uint64_t dropped_dup;
    uint32_t path_switches;
} et_frm_stats_t;

typedef struct et_frm et_frm_t;

API et_frm_t* et_frm_create(void);
API void et_frm_destroy(et_frm_t* frm);
API void et_frm_reset(et_frm_t* frm);
/** Solo P0/P1 se duplican (flujos críticos). */
API bool et_frm_should_duplicate(const et_frm_t* frm, uint8_t priority);
/** FlowID del camino alternativo (bit 7 invertido, evitando 0). */
API uint8_t et_frm_alternate_flow(uint8_t flow_id);
/** Deduplica por (nodo, SeqNum): `out_is_new` = false si ya se entregó. */
API status_t et_frm_note_rx(et_frm_t* frm, uint16_t node_id,
                                                          uint32_t seq_num, bool duplicate_flag,
                                                          bool* out_is_new);
/** Conmuta el flujo del nodo al camino alternativo tras un fallo de enlace. */
API status_t et_frm_fail_path(et_frm_t* frm, uint16_t node_id);
API uint8_t et_frm_active_path(const et_frm_t* frm, uint16_t node_id);
API status_t et_frm_stats(const et_frm_t* frm,
                                                        et_frm_stats_t* out);

/* ------------------------------------------------------------------ */
/* ID local determinista: MAC LAA derivada del ID (2 niveles)          */
/*                                                                     */
/* Nivel 1 (camino crítico): `et_aalid_candidate()` calcula   */
/* el ID candidato con CRC-32C + plegado XOR. Es una función sin ramas  */
/* ni bucles, de tiempo constante, y NO garantiza unicidad: con 16     */
/* bits la paradoja del cumpleaños aparece hacia los ~300 nodos.        */
/*                                                                     */
/* Nivel 2 (solo admisión): el maestro es la autoridad final. Mantiene  */
/* una tabla {MAC de fábrica -> ID} y un bitmap de IDs ocupados;        */
/* resuelve colisiones con un cursor secuencial determinista, fuera del */
/* superframe. El resultado es único en régimen permanente.             */
/* ------------------------------------------------------------------ */

#define ET_LOCAL_ID_MAX  0xFFFFu   /* 65.536 identificadores */
#define ET_LOCAL_ID_NONE 0xFFFFu

/**
 * MAC local (LAA) del ID: `02:00:00:00:ID[15:8]:ID[7:0]`. El bit U/L queda
 * a 1 (dirección administrada localmente) y el I/G a 0 (unicast).
 */
API uint32_t et_aalid_mac(uint16_t id, uint8_t out[6]);

/** ID codificado en una MAC local; `ET_LOCAL_ID_NONE` si no lo es. */
API uint16_t et_aalid_id_from_mac(const uint8_t mac[6]);

/** Nivel 1: ID candidato (CRC-32C de la MAC plegado a 16 bits). */
API uint16_t et_aalid_candidate(const uint8_t mac[6]);

/** Nivel 1 alternativo sin CRC: hash multiplicativo (fallback portable). */
API uint16_t et_aalid_candidate_mul(const uint8_t mac[6]);

/**
 * Backoff determinista derivado de la MAC: entero en `[0, slots)`. Reproducible
 * entre ejecuciones (a diferencia de `rand()`) y distinto para MACs distintas.
 * `slots` debe ser potencia de 2.
 */
API uint32_t et_aalid_backoff_slots(const uint8_t mac[6], uint32_t slots);

/** Resultado de una solicitud de admisión. */
typedef enum {
    ET_AALID_OK = 0,           /* ID candidato libre            */
    ET_AALID_COLLISION = 1,    /* candidato ocupado: se resolvió */
    ET_AALID_DUPLICATE = 2,    /* la MAC ya tenía ID: se devuelve */
    ET_AALID_CLONE = 3,        /* misma MAC, distinto nonce      */
    ET_AALID_FULL = 4,         /* no quedan IDs libres           */
    ET_AALID_INVALID = 5
} et_aalid_result_t;

typedef struct {
    uint16_t id_max;        /* IDs asignables - 1 (def. 65535)             */
    uint32_t grace_ms;      /* tiempo de gracia sin heartbeat (def. 600000)*/
    uint32_t table_slots;   /* potencia de 2 (def. 1024; 0 => 1024)        */
} et_aalid_config_t;

typedef struct {
    uint64_t admissions;    /* IDs concedidos                              */
    uint64_t collisions;    /* candidatos ocupados que hubo que reasignar  */
    uint64_t duplicates;    /* reclamaciones de una MAC ya registrada      */
    uint64_t released;      /* liberaciones explícitas                     */
    uint64_t expired;       /* IDs liberados por timeout de heartbeat      */
    uint64_t clone_mac;     /* solicitudes con MAC clonada (rechazadas)    */
    uint32_t occupied;      /* IDs en uso                                  */
} et_aalid_stats_t;

typedef struct et_aalid et_aalid_t;

API void et_aalid_default_config(et_aalid_config_t* cfg);
API et_aalid_t* et_aalid_create(const et_aalid_config_t* cfg);
API void et_aalid_destroy(et_aalid_t* aalid);

/**
 * Resuelve el ID final de `mac` (Nivel 2). Determinista: la misma secuencia de
 * llegadas produce siempre la misma asignación. Devuelve SUCCESS con el ID en
 * `out_id` (también en duplicados y colisiones) o `ERROR_STATE` si la
 * MAC está clonada, `ERROR_FULL` si no quedan IDs.
 */
API status_t et_aalid_admit(et_aalid_t* aalid,
                                                          const uint8_t mac[6],
                                                          uint32_t nonce,
                                                          uint64_t now_ms,
                                                          uint16_t* out_id,
                                                          et_aalid_result_t* out_result);

API status_t et_aalid_release(et_aalid_t* aalid,
                                                           uint16_t id);
API status_t et_aalid_lookup(const et_aalid_t* aalid,
                                                           const uint8_t mac[6],
                                                           uint16_t* out_id);
/** Refresca el temporizador de vida de un ID (mantiene viva la entrada CAM). */
API status_t et_aalid_heartbeat(et_aalid_t* aalid,
                                                              uint16_t id, uint64_t now_ms);
/** Libera los IDs sin heartbeat dentro del tiempo de gracia. Devuelve cuántos. */
API uint32_t et_aalid_expire(et_aalid_t* aalid, uint64_t now_ms);
API status_t et_aalid_stats(const et_aalid_t* aalid,
                                                          et_aalid_stats_t* out);
API bool et_aalid_is_occupied(const et_aalid_t* aalid, uint16_t id);

/**
 * Persistencia {MAC -> ID} para conservar la identidad tras un reinicio.
 * `save` escribe de forma atómica (fichero temporal + `rename`).
 */
API status_t et_aalid_save(const et_aalid_t* aalid,
                                                         const char* path);
API status_t et_aalid_load(et_aalid_t* aalid,
                                                         const char* path);

/* ------------------------------------------------------------------ */
/* Nodos y maestro                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t id;
    uint8_t  mac[6];
    int64_t  last_seen_ns;
    int64_t  clock_offset_ns;   /* offset estimado por RTT (ns)      */
    uint32_t rtt_ns;
    uint32_t missed_cycles;
    bool     lost;
} et_node_t;

typedef struct {
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint64_t dropped;
    uint64_t backpressure;
    uint64_t fec_blocks;
    uint64_t frame_errors;
    /* --- v0.1.0 --- */
    uint64_t duplicates_tx;     /* tramas duplicadas por FRM               */
    uint64_t wkc_mismatches;    /* discrepancias de ET-WKC observadas      */
    uint64_t sync_out_of_sync;  /* nodos marcados fuera de sincronización  */
    uint64_t mbx_retries;       /* reintentos de ET-MBX                    */
    uint64_t local_id_dropped;  /* descartadas por destino ajeno (ID local) */
} et_master_stats_t;

/* ------------------------------------------------------------------
/* Modo de operación EtherTux (Master/Slave)
 * ------------------------------------------------------------------ */

#define ET_OP_SLAVE  0u
#define ET_OP_MASTER 1u

typedef struct et_master et_master_t;
typedef struct et_slave et_slave_t;

/* ------------------------------------------------------------------
 * Estadísticas del esclavo EtherTux
 * ------------------------------------------------------------------ */

/**
 * Estadísticas completas del esclavo EtherTux.
 */
typedef struct {
    uint64_t frames_tx;      /* Tramas transmitidas exitosas */
    uint64_t frames_rx;      /* Tramas recibidas (después de filtro) */
    uint64_t bytes_tx;       /* Bytes transmitidos */
    uint64_t bytes_rx;       /* Bytes recibidos (después de filtro) */
    uint64_t dropped;        /* Tramas descartadas (filtro destino) */
    uint64_t frame_errors;   /* Errores de formato de trama */
} et_slave_stats_t;

/* ------------------------------------------------------------------
 * API Master EtherTux (controla el bus)
 * ------------------------------------------------------------------ */

API et_master_t* et_master_create(void);
API void et_master_destroy(et_master_t* master);

/** Abre `ifname` como EtherTux (AF_PACKET, EtherType 0x88B5). 1 Gbps / 1 ms. */
API status_t et_master_open(et_master_t* master,
                                                           const char* ifname);
/** Igual, declarando la tasa del enlace y el superframe. */
API status_t et_master_open_ex(et_master_t* master,
                                                             const char* ifname,
                                                             uint64_t link_bps,
                                                             uint32_t cycle_ns);
API void et_master_close(et_master_t* master);
API bool et_master_is_open(const et_master_t* master);
API const char* et_master_interface(const et_master_t* master);

API status_t et_master_set_qos(et_master_t* master,
                                                             const et_qos_config_t* cfg);
API status_t et_master_set_fec(et_master_t* master,
                                                             et_fec_mode_t mode);
/** Asocia (sin tomar propiedad) un planificador al maestro. */
API status_t et_master_attach_scheduler(et_master_t* master,
                                                                     et_sched_t* sched);

/* Asociaciones opcionales (sin propiedad) de los módulos v0.1.0. */
API status_t et_master_attach_topology(et_master_t* master,
                                                                    et_topology_t* topo);
API status_t et_master_attach_sync(et_master_t* master,
                                                                et_sync_t* sync);
API status_t et_master_attach_esm(et_master_t* master,
                                                               et_esm_t* esm);
API status_t et_master_attach_mbx(et_master_t* master,
                                                               et_mbx_t* mbx);
API status_t et_master_attach_frm(et_master_t* master,
                                                               et_frm_t* frm);

/* --- ID local determinista en el maestro ---------------------------- */

/**
 * Activa el esquema de ID local (Nivel 2) en el maestro. A partir de aquí la
 * recepción descarta las tramas cuyo destino no sea la MAC local ni difusión, y
 * los nodos se identifican con su ID (O(1)) en lugar de buscar por MAC.
 */
API status_t et_master_enable_local_ids(
        et_master_t* master, const et_aalid_config_t* cfg);

/** Fija el ID local del maestro y deriva de él su MAC de origen (LAA). */
API status_t et_master_set_local_id(et_master_t* master,
                                                                  uint16_t id);

/** ID local del maestro (`ET_LOCAL_ID_NONE` si no hay). */
API uint16_t et_master_local_id(const et_master_t* master);

/** Autoridad de admisión: resuelve el ID final de una MAC (Nivel 2). */
API status_t et_master_admit_node(et_master_t* master,
                                                                const uint8_t mac[6],
                                                                uint32_t nonce,
                                                                uint64_t now_ms,
                                                                uint16_t* out_id,
                                                                et_aalid_result_t* out_result);

/** Libera los IDs sin heartbeat dentro del tiempo de gracia. Devuelve cuántos. */
API uint32_t et_master_expire_local_ids(et_master_t* master,
                                                            uint64_t now_ms);

/**
 * `true` si una trama dirigida a `mac` interesa a este maestro (difusión o la
 * propia MAC local). Es el filtro del camino de recepción, en tiempo constante.
 */
API bool et_master_accepts_destination(const et_master_t* master,
                                                           const uint8_t mac[6]);

/** Estado de admisión asociado (NULL si no está activado). */
API et_aalid_t* et_master_aalid(et_master_t* master);

/**
 * Envía a un nodo concreto usando su ID local (destino unicast `02:...:ID`), en
 * lugar de difusión: evita el *flooding* del switch y el tráfico parásito en el
 * resto de puertos. `dest_id == ET_LOCAL_ID_NONE` equivale a difusión.
 */
API status_t et_master_send_to(et_master_t* master,
                                                             uint16_t dest_id,
                                                             uint8_t priority,
                                                             et_type_t type,
                                                             uint8_t flow_id,
                                                             const void* payload, uint16_t len);

/** ET-WKC de la última trama recibida. */
API uint16_t et_master_last_wkc(const et_master_t* master);

/** Velocidad del enlace en bps según sysfs (`/sys/class/net/<if>/speed`); 0 si no se puede leer. */
API uint64_t et_link_speed_bps(const char* ifname);

/**
 * Abre la interfaz detectando la velocidad real del enlace (1G/2.5G/5G/10G) y
 * ajustando el superframe al presupuesto de bytes correspondiente. Si no se
 * puede leer la velocidad, usa 1 Gbps.
 */
API status_t et_master_open_auto(et_master_t* master,
                                                               const char* ifname);

/**
 * Envía sin copiar la carga útil: cabecera + payload en dos iovecs (`sendmsg`).
 * La carga debe permanecer válida hasta que la llamada retorne.
 */
API status_t et_master_send_zerocopy(et_master_t* master,
                                                                   uint8_t priority,
                                                                   et_type_t type,
                                                                   uint8_t flow_id,
                                                                   const void* payload,
                                                                   uint16_t len);

/** Elemento de un envío por lotes. */
typedef struct {
    uint8_t              priority;
    et_type_t  type;
    uint8_t              flow_id;
    const void*          payload;
    uint16_t             len;
} et_batch_item_t;

/**
 * Envía hasta 16 tramas en **una sola llamada al sistema** (`sendmmsg`), o en
 * una sola llamada si el anillo TX está activo. Devuelve el número de tramas
 * entregadas al kernel, o -1 si falla el envío completo.
 */
API int et_master_send_batch(et_master_t* master,
                                                 const et_batch_item_t* items,
                                                 uint32_t count);

/* ------------------------------------------------------------------ */
/* Modo de transmisión (llamada al sistema vs. anillo en user space)   */
/* ------------------------------------------------------------------ */

typedef enum {
    ET_TX_AUTO    = 0,  /* anillo TX si el kernel lo permite     */
    ET_TX_SYSCALL = 1,  /* sendmsg / sendmmsg                    */
    ET_TX_RING    = 2   /* PACKET_TX_RING obligatorio             */
} et_tx_mode_t;

/**
 * Selecciona el camino de transmisión. Con el anillo (`PACKET_TX_RING`) la
 * trama se escribe en una ranura mapeada en user space y un único `send()`
 * vacía el lote completo: se elimina la llamada al sistema por trama, que es
 * el coste dominante con tramas pequeñas. `AUTO` lo intenta y, si el kernel o
 * la interfaz no lo permiten, continúa con llamadas al sistema.
 */
API status_t et_master_set_tx_mode(et_master_t* master,
                                                                 et_tx_mode_t mode);

/** `true` si el anillo de transmisión está activo en esta interfaz. */
API bool et_master_tx_ring_active(const et_master_t* master);

/** Envía una PDU (aplica QoS, CRC-32C y, si procede, marca FEC). */
API status_t et_master_send(et_master_t* master,
                                                          uint8_t priority,
                                                          et_type_t type,
                                                          uint8_t flow_id,
                                                          const void* payload, uint16_t len);

/** Envía con pacing por hardware (SO_TXTIME) si la NIC lo soporta. */
API status_t et_master_send_at(et_master_t* master,
                                                             uint8_t priority,
                                                             et_type_t type,
                                                             uint8_t flow_id,
                                                             const void* payload, uint16_t len,
                                                             uint64_t tx_deadline_ns);

/**
 * Envía varios flujos agregados en una sola trama (`FlowCount > 1`, §7.6).
 * La carga útil se serializa como lista de entradas (FlowID, longitud, datos).
 */
API status_t et_master_send_aggregated(et_master_t* master,
                                                                     uint8_t priority,
                                                                     uint8_t flow_id,
                                                                     const et_flow_entry_t* entries,
                                                                     uint32_t count);

/**
 * Envía un segmento de ET-MBX (§9): prioridad P4, o P5 si la ocupación de P4
 * supera el umbral configurado. Requiere un mailbox asociado.
 */
API status_t et_master_send_mailbox(et_master_t* master,
                                                                  uint8_t channel,
                                                                  const void* data, uint16_t len,
                                                                  uint8_t p4_occupancy_percent);

/**
 * Envía un bloque FEC completo: `count` PDUs de datos (count debe ser K) más
 * los N-K bloques de paridad. Los datos deben tener todos `len` bytes.
 */
API status_t et_master_send_fec_block(et_master_t* master,
                                                                    uint8_t priority,
                                                                    uint8_t flow_id,
                                                                    const uint8_t* const* data,
                                                                    uint32_t count,
                                                                    uint32_t len);

/** Recibe una PDU. Devuelve los bytes de carga útil, o -1 en timeout/error. */
API int et_master_recv(et_master_t* master,
                                           et_header_t* hdr,
                                           void* payload, uint32_t bufsize,
                                           uint32_t timeout_us);

/** Marca caídos los nodos sin heartbeat en 3 superframes. Devuelve cuántos. */
API uint32_t et_master_heartbeat(et_master_t* master,
                                                     uint64_t now_ns);
API uint32_t et_master_node_count(const et_master_t* master);
API bool et_master_node(const et_master_t* master, uint32_t index,
                                            et_node_t* out);
/** Sincroniza el reloj de un nodo por RTT (offset ≈ -RTT/2). */
API status_t et_master_rtt_sync(et_master_t* master,
                                                              const uint8_t mac[6],
                                                              uint32_t rtt_ns);

API status_t et_master_stats(const et_master_t* master,
                                                           et_master_stats_t* out);
API void et_master_reset_stats(et_master_t* master);

/* ------------------------------------------------------------------
 * API Slave EtherTux (responde como esclavo del master)
 * ------------------------------------------------------------------ */

/**
 * Crea un nuevo contexto de esclavo EtherTux.
 * - `ifname`: nombre de la interfaz de red (ej. "eth0")
 * - `link_bps`: velocidad del enlace (0 = 1 Gbps por defecto)
 * - `cycle_ns`: periodo del superframe en ns (0 = 1 ms por defecto)
 * Retorna NULL si falla la asignación.
 */
API et_slave_t* et_slave_create(const char* ifname,
                                                uint64_t link_bps,
                                                uint32_t cycle_ns);

/** Destruye un contexto de esclavo y libera todos los recursos. */
API void et_slave_destroy(et_slave_t* slave);

/**
 * Abre la interfaz EtherTux para el modo esclavo.
 * El esclavo no requiere configuración de QoS, scheduler ni topología.
 */
API status_t et_slave_open(et_slave_t* slave,
                                               const char* ifname);

/** Cierra la interfaz y libera los recursos del esclavo. */
API void et_slave_close(et_slave_t* slave);

/** Retorna el modo actual (SLAVE). */
API uint8_t et_slave_mode(const et_slave_t* slave);

/** Retorna las estadísticas de operación del esclavo. */
API status_t et_slave_stats(const et_slave_t* slave,
                                              et_slave_stats_t* out);

/**
 * Envía una trama EtherTux sin destino específico (difusión).
 */
API status_t et_slave_send(et_slave_t* slave,
                                            uint8_t priority,
                                            et_type_t type,
                                            uint8_t flow_id,
                                            const void* payload,
                                            uint16_t len);

/**
 * Envía una trama con pacing por hardware (SO_TXTIME) si la NIC lo soporta.
 */
API status_t et_slave_send_at(et_slave_t* slave,
                                               uint8_t priority,
                                               et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload,
                                               uint16_t len,
                                               uint64_t tx_deadline_ns);

/**
 * Envía una trama a un destino unicast usando ID local.
 * `dest_id == ET_LOCAL_ID_NONE` equivale a difusión.
 */
API status_t et_slave_send_to(et_slave_t* slave,
                                               uint16_t dest_id,
                                               uint8_t priority,
                                               et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload,
                                               uint16_t len);

/**
 * Recibe una trama EtherTux.
 * El esclavo filtra por destino: acepta difusión y tramas dirigidas a
 * su propia MAC local. Devuelve los bytes de carga útil, o -1 en timeout/error.
 */
API int et_slave_recv(et_slave_t* slave,
                             et_header_t* hdr,
                             void* payload,
                             uint32_t bufsize,
                             uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* ETHERTUX_H */
