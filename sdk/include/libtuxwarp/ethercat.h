/*
 * ethercat.h — Maestro EtherCAT de libtuxwarp (API pública, C11).
 *
 * Maestro EtherCAT AUTÓNOMO implementado sobre Ethernet crudo (AF_PACKET,
 * EtherType 0x88A4) conforme a IEC 61158 / ETG.1000, SIN dependencias externas
 * (ni SOEM ni el maestro IgH). El diseño prioriza las cuatro propiedades del
 * proyecto:
 *
 *   - TIEMPO REAL     : sin asignaciones dinámicas en el camino caliente, todo
 *                       pre-asignado; el ciclo de proceso no bloquea ni reserva.
 *   - DETERMINISMO    : temporización monotónica (CLOCK_MONOTONIC), timeouts
 *                       acotados, sin esperas indefinidas.
 *   - RESILIENCIA     : validación por contador de trabajo, detección de
 *                       esclavas perdidas, contadores de error del ESC, reintentos
 *                       acotados y redundancia de red (doble NIC).
 *   - ULTRA RENDIMIENTO: varios datagramas por trama (bit M), buffers indexados
 *                       pre-asignados y cero copias superfluas.
 *
 * Cualquier aplicación puede enumerar las interfaces del sistema, abrir una
 * (p. ej. enp2s0 / end0, según los nombres predecibles de udev) como maestro
 * EtherCAT y comunicarse con las esclavas.
 *
 * libtuxwarp ofrece DOS protocolos de red seleccionables (nunca simultáneos sobre
 * la misma interfaz): este maestro EtherCAT (hardware EtherCAT dedicado) y
 * EtherTux (Ethernet comercial determinista, `ethertux.h`). La
 * elección se resuelve en configuración con `protocol.h`.
 */
#ifndef ETHERCAT_H
#define ETHERCAT_H

#include "abi.h"
#include "libtuxwarp.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EtherType de EtherCAT (IEC 61158 tipo 0x88A4). */
#define EC_ETHERTYPE 0x88A4

/* Límites del maestro. */
#define EC_MAX_SLAVES        64     /* esclavas soportadas por bus   */
#define EC_MAX_FRAME         1518   /* MTU Ethernet estándar         */
#define EC_MAX_DATAGRAM_DATA 1024   /* datos máximos por datagrama   */
#define EC_MAX_SM            8      /* Sync Managers por esclava     */
#define EC_MAX_FMMU          4      /* FMMU por esclava              */
#define EC_MAX_MAILBOX       256    /* bytes del buzón (SM0/SM1)     */
#define EC_MAX_INTERFACES    16     /* interfaces enumerables        */

/* Dirección de difusión (broadcast). */
#define EC_BROADCAST_ADDR 0x0000u

/* Dirección de posición base: la primera esclava recibe EC_NODEOFFSET. */
#define EC_NODE_OFFSET 0x1000u

/* Comandos EtherCAT (campo `command` del datagrama, ETG.1000). */
typedef enum {
    EC_CMD_NOP  = 0,
    EC_CMD_APRD = 1,   /* auto-increment read  (posición)      */
    EC_CMD_APWR = 2,   /* auto-increment write (posición)      */
    EC_CMD_APRW = 3,   /* auto-increment read/write            */
    EC_CMD_FPRD = 4,   /* configured address read              */
    EC_CMD_FPWR = 5,   /* configured address write             */
    EC_CMD_FPRW = 6,   /* configured address read/write        */
    EC_CMD_BRD  = 7,   /* broadcast read                       */
    EC_CMD_BWR  = 8,   /* broadcast write                      */
    EC_CMD_BRW  = 9,   /* broadcast read/write                 */
    EC_CMD_LRD  = 10,  /* logical read                         */
    EC_CMD_LWR  = 11,  /* logical write                        */
    EC_CMD_LRW  = 12,  /* logical read/write (PDO)             */
    EC_CMD_ARMW = 13,  /* auto-inc read, multiple write (DC)   */
    EC_CMD_FRMW = 14   /* configured read, multiple write (DC) */
} ec_cmd_t;

/* Registros ESC de uso habitual. */
#define EC_REG_TYPE           0x0000u  /* tipo/revisión del ESC      */
#define EC_REG_REVISION       0x0008u  /* revisión del ESC           */
#define EC_REG_STATION_ADDR   0x0010u  /* dirección de estación      */
#define EC_REG_STATION_ALIAS  0x0012u  /* alias de estación          */
#define EC_REG_DL_STATUS      0x0110u  /* estado del enlace (DL)     */
#define EC_REG_AL_CONTROL     0x0120u  /* control AL                 */
#define EC_REG_AL_STATUS      0x0130u  /* estado AL                  */
#define EC_REG_AL_STATUS_CODE 0x0134u  /* código de estado AL        */
#define EC_REG_RX_ERROR       0x0300u  /* contadores de error RX     */
#define EC_REG_CRC_ERROR      0x0308u  /* contador de error CRC      */
#define EC_REG_WD_COUNTER     0x0442u  /* contador de watchdog       */
#define EC_REG_DC_SYSTEM_TIME 0x0910u  /* tiempo de sistema (DC)     */
#define EC_REG_DC_SYNC_ACT    0x0981u  /* activación SYNC (DC)       */
#define EC_REG_DC_START0      0x0990u  /* tiempo de arranque SYNC0   */
#define EC_REG_DC_CYCLE0      0x09A0u  /* periodo de SYNC0           */

/* Estados de la máquina AL (Application Layer). */
typedef enum {
    EC_STATE_NONE  = 0x00,
    EC_STATE_INIT  = 0x01,
    EC_STATE_PREOP = 0x02,
    EC_STATE_BOOT  = 0x03,
    EC_STATE_SAFEOP= 0x04,
    EC_STATE_OP    = 0x08,
    EC_STATE_ACK   = 0x10,   /* ACK de error (se OR-ea al estado) */
    EC_STATE_ERROR = 0x10
} ec_state_t;

/* Protocolos de buzón (mailbox) soportados por una esclava. */
#define EC_MBX_AOE  0x0001u
#define EC_MBX_EOE  0x0002u
#define EC_MBX_COE  0x0004u
#define EC_MBX_FOE  0x0008u
#define EC_MBX_SOE  0x0010u
#define EC_MBX_VOE  0x0020u

/* Interfaces de red del sistema. */
typedef struct {
    char     name[32];      /* nombre de la interfaz (p. ej. "enp2s0") */
    uint8_t  mac[6];        /* dirección MAC                          */
    int      ifindex;       /* índice del kernel                      */
    bool     is_up;         /* interfaz administrativamente activa    */
} ec_interface_t;

/* Datagrama EtherCAT deserializado. */
typedef struct {
    uint8_t        cmd;             /* ec_cmd_t                  */
    uint8_t        idx;             /* índice de correlación               */
    uint16_t       adp;             /* dirección de posición/estación      */
    uint16_t       ado;             /* offset (o dirección lógica baja)    */
    uint32_t       address;         /* ADP | (ADO << 16), tal cual on-wire  */
    uint16_t       data_len;        /* longitud del área de datos          */
    uint16_t       irq;             /* campo IRQ (normalmente 0)           */
    uint16_t       working_counter; /* contador de trabajo del datagrama   */
    const uint8_t* data;            /* puntero al área de datos (no propio)*/
    bool           more;            /* bit M: hay más datagramas           */
    uint32_t       next_offset;     /* offset del siguiente datagrama      */
} ec_datagram_t;

/* Esclava EtherCAT descubierta y configurada. */
typedef struct {
    uint16_t position;        /* posición en el anillo (auto-incremento 1..N) */
    uint16_t station_addr;    /* dirección de estación configurada           */
    uint16_t alias;           /* alias de estación                           */
    uint16_t config_addr;     /* dirección efectiva para FPRD/FPWR           */
    uint32_t vendor_id;       /* fabricante (SII)                            */
    uint32_t product_code;    /* producto (SII)                              */
    uint32_t revision;        /* revisión (SII)                              */
    uint32_t serial;          /* número de serie (SII)                       */

    uint8_t  al_state;        /* estado AL actual                            */
    uint16_t al_status_code;  /* código de estado AL                         */
    uint16_t dl_status;       /* estado de la capa de enlace (DL)            */

    uint8_t  ports;           /* número de puertos del ESC                   */
    bool     has_dc;          /* soporta relojes distribuidos                */
    bool     is_lost;         /* esclava no responde en el último ciclo      */

    uint16_t mbx_proto;       /* protocolos de buzón soportados              */

    uint32_t i_bytes;         /* bytes de entrada (TxPDO)                    */
    uint32_t o_bytes;         /* bytes de salida (RxPDO)                     */
    uint32_t i_offset;        /* offset de entradas en el IOMap              */
    uint32_t o_offset;        /* offset de salidas en el IOMap               */

    int64_t  dc_offset;       /* offset de reloj (ns)                        */
    int32_t  dc_delay;        /* retardo de propagación (ns)                 */
} ec_slave_t;

/* Maestro EtherCAT (tipo opaco). */
typedef struct ec_master ec_master_t;

/* ------------------------------------------------------------------ */
/* Enumeración de interfaces del sistema                               */
/* ------------------------------------------------------------------ */

/** Enumera las interfaces disponibles. Devuelve el número de interfaces. */
API uint32_t ec_interface_count(void);
/** Devuelve la interfaz en `index`, o NULL si el índice está fuera de rango. */
API const ec_interface_t* ec_interface(uint32_t index);
/** Devuelve el índice de `name` (coincidencia por prefijo exacto) o -1. */
API int ec_interface_index(const char* name);

/* ------------------------------------------------------------------ */
/* Helpers puros de trama (testeables sin hardware)                    */
/* ------------------------------------------------------------------ */

/**
 * Serializa UN datagrama EtherCAT (cabecera de 10 B + datos + WC de 2 B).
 * `address` = ADP | (ADO << 16). `more` activa el bit M (quedan datagramas).
 * Devuelve los bytes escritos o 0 si el búfer es insuficiente.
 */
API uint32_t ec_datagram_build(uint8_t* out, uint32_t out_size,
                                                   uint8_t cmd, uint8_t idx,
                                                   uint32_t address,
                                                   const void* data, uint16_t len,
                                                   bool more);

/**
 * Deserializa el datagrama que comienza en `in`. Devuelve los bytes consumidos
 * (para avanzar al siguiente) o 0 si el búfer es insuficiente/mal formado.
 */
API uint32_t ec_datagram_parse(const uint8_t* in, uint32_t in_len,
                                                   ec_datagram_t* out);

/**
 * Escribe la cabecera EtherCAT de 2 B (longitud de datagramas en 11 bits |
 * tipo=1 en 4 bits) y devuelve su valor de 16 bits en little-endian.
 */
API uint16_t ec_frame_header(uint8_t out[2], uint16_t datagram_bytes);

/* ------------------------------------------------------------------ */
/* Ciclo de vida del maestro                                           */
/* ------------------------------------------------------------------ */

API ec_master_t* ec_master_create(void);
/** Abre `ifname` como maestro EtherCAT (AF_PACKET, EtherType 0x88A4). */
API status_t ec_master_open(ec_master_t* master,
                                                          const char* ifname);
/** Abre el maestro con redundancia de red (NIC primaria + secundaria). */
API status_t ec_master_open_redundant(ec_master_t* master,
                                                                    const char* ifname,
                                                                    const char* redundant_ifname);
API void ec_master_close(ec_master_t* master);
API void ec_master_destroy(ec_master_t* master);
API bool ec_master_is_open(const ec_master_t* master);
API const char* ec_master_interface(const ec_master_t* master);
/** Ajusta el timeout por defecto de las operaciones (µs). */
API void ec_master_set_timeout(ec_master_t* master, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Escaneo y configuración del bus                                     */
/* ------------------------------------------------------------------ */

/** Enumera las esclavas presentes (auto-incremento). Devuelve el nº o -1. */
API int ec_master_scan(ec_master_t* master);
/** Configura las esclavas: dirección de estación, SII/EEPROM y buzón. */
API int ec_master_config_init(ec_master_t* master);
/** Configura el mapa de proceso: SM/FMMU y direcciones lógicas (IOMap). */
API int ec_master_config_map(ec_master_t* master);

API uint32_t ec_master_slave_count(const ec_master_t* master);
API const ec_slave_t* ec_master_slave(const ec_master_t* master,
                                                                    uint32_t index);
/** Puntero al mapa de proceso (IOMap) y su tamaño. */
API const uint8_t* ec_master_iomap(const ec_master_t* master);
API uint32_t ec_master_iomap_size(const ec_master_t* master);

/* ------------------------------------------------------------------ */
/* Acceso a registros ESC                                              */
/* ------------------------------------------------------------------ */

/* `adp` = auto-incremento (posición) o dirección de estación. `ado` = offset. */
API int ec_master_read_reg(ec_master_t* master,
                                               uint16_t adp, uint16_t ado,
                                               void* out, uint16_t len);
API int ec_master_write_reg(ec_master_t* master,
                                                uint16_t adp, uint16_t ado,
                                                const void* in, uint16_t len);
/* Direccionamiento por dirección de estación configurada (FPRD/FPWR). */
API int ec_master_fp_read(ec_master_t* master,
                                              uint16_t adp, uint16_t ado,
                                              void* out, uint16_t len);
API int ec_master_fp_write(ec_master_t* master,
                                               uint16_t adp, uint16_t ado,
                                               const void* in, uint16_t len);
API int ec_master_broadcast_read(ec_master_t* master,
                                                     uint16_t ado, void* out, uint16_t len);
API int ec_master_broadcast_write(ec_master_t* master,
                                                      uint16_t ado, const void* in, uint16_t len);

/* Direccionamiento lógico (datos de proceso). */
API int ec_master_lrw(ec_master_t* master, uint32_t logical_addr,
                                          void* buf, uint16_t len);
API int ec_master_lrd(ec_master_t* master, uint32_t logical_addr,
                                          void* out, uint16_t len);
API int ec_master_lwr(ec_master_t* master, uint32_t logical_addr,
                                          const void* in, uint16_t len);

/* Lectura + escritura múltiple (relojes distribuidos). */
API int ec_master_armw(ec_master_t* master,
                                           uint16_t adp, uint16_t ado, void* buf, uint16_t len);
API int ec_master_frmw(ec_master_t* master,
                                           uint16_t adp, uint16_t ado, void* buf, uint16_t len);

/* ------------------------------------------------------------------ */
/* Máquina de estados AL                                               */
/* ------------------------------------------------------------------ */

API int ec_master_read_state(ec_master_t* master, uint16_t adp,
                                                 uint8_t* state, uint16_t* status_code);
/** Solicita un estado y espera a alcanzarlo (con ACK de errores). */
API int ec_master_request_state(ec_master_t* master, uint16_t adp,
                                                    uint8_t state, uint32_t timeout_us);
/** Comprueba que la esclava `adp` está en `req_state` (con timeout). */
API int ec_master_statecheck(ec_master_t* master, uint16_t adp,
                                                 uint8_t req_state, uint32_t timeout_us);
/** Escribe el registro de control AL con ACK de error. */
API int ec_master_write_state(ec_master_t* master, uint16_t adp,
                                                  uint8_t state);
/** Lleva TODAS las esclavas a `state` (BWR) y espera (degradación elegante). */
API int ec_master_request_all_states(ec_master_t* master,
                                                         uint8_t state, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Relojes distribuidos (DC)                                           */
/* ------------------------------------------------------------------ */

/** Mide retardos de propagación y compensa el offset de reloj del bus. */
API status_t ec_master_config_dc(ec_master_t* master);
/** Activa/desactiva SYNC0 en la esclava `index` (1..N). */
API int ec_master_sync0(ec_master_t* master, uint32_t index,
                                            bool act, uint32_t cycle_ns, int32_t shift_ns);
/** Activa/desactiva SYNC0 y SYNC1 en la esclava `index`. */
API int ec_master_sync01(ec_master_t* master, uint32_t index,
                                             bool act, uint32_t cycle0_ns, int32_t shift0_ns,
                                             uint32_t cycle1_ns, int32_t shift1_ns);
/** Último tiempo de sistema DC leído del bus (ns, epoch 2000-01-01). */
API int64_t ec_master_dc_time(const ec_master_t* master);

/* ------------------------------------------------------------------ */
/* Ciclo de datos de proceso (PDO)                                     */
/* ------------------------------------------------------------------ */

/** Envía una trama de proceso (LRW) con las salidas del IOMap. */
API int ec_master_send_processdata(ec_master_t* master);
/** Recibe la trama de proceso y valida el contador de trabajo esperado. */
API int ec_master_receive_processdata(ec_master_t* master,
                                                          uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Buzón / CoE (SDO)                                                   */
/* ------------------------------------------------------------------ */

/** Lee un objeto CoE (SDO upload). `*size` entra con el tamaño del búfer y sale con el leído. */
API status_t ec_master_sdo_read(ec_master_t* master,
                                                              uint32_t slave_index,
                                                              uint16_t index, uint8_t subindex,
                                                              void* out, uint32_t* size,
                                                              uint32_t timeout_us);
/** Escribe un objeto CoE (SDO download). */
API status_t ec_master_sdo_write(ec_master_t* master,
                                                               uint32_t slave_index,
                                                               uint16_t index, uint8_t subindex,
                                                               const void* in, uint32_t size,
                                                               uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Diagnóstico                                                         */
/* ------------------------------------------------------------------ */

API uint16_t ec_master_working_counter(const ec_master_t* master);
API uint16_t ec_master_expected_wkc(const ec_master_t* master);
API uint64_t ec_master_frames_sent(const ec_master_t* master);
API uint64_t ec_master_frames_lost(const ec_master_t* master);

/** Envío/recepción de datagramas EtherCAT crudos (protocolo avanzado). */
API int ec_master_send(ec_master_t* master,
                                           const uint8_t* datagrams, uint32_t len);
API int ec_master_recv(ec_master_t* master,
                                           uint8_t* buffer, uint32_t bufsize,
                                           uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* ETHERCAT_H */
