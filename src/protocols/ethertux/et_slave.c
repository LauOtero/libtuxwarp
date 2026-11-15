/*
 * et_slave.c — Implementación del modo esclavo EtherTux.
 *
 * El esclavo EtherTux:
 *   - Recibe tramas de difusión y unicast (destino = su MAC local/LAA).
 *   - Responde a Discovery, Sync, Heartbeat y StateChange.
 *   - Filtra tramas por destino y valida CRC-32C.
 *   - Usa el mismo datapath que el maestro (anillo PACKET_TX_RING opcional).
 *
 * Diseño: el esclavo es un estado finito simple (INIT, CONFIG, SAFE, OP, FAULT)
 * y no mantiene vista del planificador (se sincroniza con el maestro).
 */
#include "et_internal.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/* Helpers internos                                                    */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* API esclavo                                                          */
/* ------------------------------------------------------------------ */

et_slave_t* et_slave_create(const char* ifname,
                            uint64_t link_bps,
                            uint32_t cycle_ns) {
    (void)link_bps;
    (void)cycle_ns;

    et_slave_t* slave = calloc(1, sizeof(et_slave_t));
    if (slave == NULL) {
        return NULL;
    }

    if (ifname != NULL && ifname[0] != '\0') {
        strncpy(slave->port.ifname, ifname, sizeof(slave->port.ifname) - 1);
        slave->port.ifname[sizeof(slave->port.ifname) - 1] = '\0';
    }

    slave->port.txtime_enabled = false;
    slave->port.fd = -1;
    slave->is_open = false;
    slave->esm_state = ET_STATE_INIT;

    return slave;
}

void et_slave_destroy(et_slave_t* slave) {
    if (slave == NULL) {
        return;
    }
    et_slave_close(slave);
    free(slave);
}

status_t et_slave_open(et_slave_t* slave, const char* ifname) {
    if (slave == NULL || ifname == NULL || ifname[0] == '\0') {
        return ERROR_INVALID;
    }

    /* Abrir socket EtherTux */
    status_t st = et_socket_open(&slave->port, ifname);
    if (st != SUCCESS) {
        return st;
    }

    slave->is_open = true;

    /* Leer velocidad del enlace */
    slave->link_bps = et_socket_link_bps(ifname);
    if (slave->link_bps == 0) {
        slave->link_bps = 1000000000ull;  /* 1 Gbps por defecto */
    }

    return SUCCESS;
}

void et_slave_close(et_slave_t* slave) {
    if (slave == NULL) {
        return;
    }
    if (slave->is_open) {
        et_socket_close(&slave->port);
        slave->is_open = false;
    }
}

uint8_t et_slave_mode(const et_slave_t* slave) {
    return (slave != NULL && slave->is_open) ? ET_OP_SLAVE : 0;
}

status_t et_slave_stats(const et_slave_t* slave, et_slave_stats_t* out) {
    if (slave == NULL || out == NULL) {
        return ERROR_INVALID;
    }

    out->frames_tx = slave->port.frames_tx;
    out->frames_rx = slave->port.frames_rx;
    out->bytes_tx = slave->port.bytes_tx;
    out->bytes_rx = slave->port.bytes_rx;
    out->dropped = 0; /* El esclavo no descarta por filtro de PU */
    out->frame_errors = 0; /* Se inicializa en 0, se actualiza en recv si hay error */

    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Construcción de cabecera EtherTux                                    */
/* ------------------------------------------------------------------ */

static status_t et_slave_build_frame(et_slave_t* slave,
                                     uint8_t priority,
                                     et_type_t type,
                                     uint8_t flow_id,
                                     const void* payload,
                                     uint16_t len,
                                     uint64_t deadline_ns,
                                     bool with_deadline) {
    if (slave == NULL || !slave->is_open) {
        return ERROR_STATE;
    }

    et_header_t h;
    memset(&h, 0, sizeof(h));

    h.type = (uint8_t)type;
    h.priority = priority;
    h.state = (uint8_t)slave->esm_state;
    h.flow_id = flow_id;

    uint64_t tx_ns = now_ns();
    h.tx_timestamp = tx_ns;
    h.wkc = 0; /* El esclavo no lleva contador de tramas enviadas */

    /* Calcular CRC-32C */
    uint32_t crc = et_crc(&h, payload, len);
    h.crc32 = crc;

    /* Construir PDU completa */
    uint8_t frame[ET_MAX_FRAME + 64];
    uint16_t frame_len = (uint16_t)et_pdu_build(frame, sizeof(frame), &h, payload, len);

    if (frame_len == 0) {
        return ERROR_INVALID;
    }

    /* Enviar */
    if (with_deadline && deadline_ns > tx_ns && et_socket_txtime_enabled(&slave->port)) {
        int sent = et_socket_send_at(&slave->port, frame, frame_len, deadline_ns);
        if (sent < 0) {
            slave->stats.frame_errors++;
            return ERROR_IO;
        }
    } else {
        int sent = et_socket_send(&slave->port, frame, frame_len);
        if (sent < 0) {
            slave->stats.frame_errors++;
            return ERROR_IO;
        }
    }

    return SUCCESS;
}

status_t et_slave_send(et_slave_t* slave,
                       uint8_t priority,
                       et_type_t type,
                       uint8_t flow_id,
                       const void* payload,
                       uint16_t len) {
    return et_slave_build_frame(slave, priority, type, flow_id, payload, len, 0, false);
}

status_t et_slave_send_at(et_slave_t* slave,
                          uint8_t priority,
                          et_type_t type,
                          uint8_t flow_id,
                          const void* payload,
                          uint16_t len,
                          uint64_t tx_deadline_ns) {
    return et_slave_build_frame(slave, priority, type, flow_id, payload, len, tx_deadline_ns, true);
}

status_t et_slave_send_to(et_slave_t* slave,
                          uint16_t dest_id,
                          uint8_t priority,
                          et_type_t type,
                          uint8_t flow_id,
                          const void* payload,
                          uint16_t len) {
    (void)dest_id; /* El esclavo solo envía difusión */
    return et_slave_send(slave, priority, type, flow_id, payload, len);
}

/* ------------------------------------------------------------------ */
/* Recepción                                                            */
/* ------------------------------------------------------------------ */

int et_slave_recv(et_slave_t* slave,
                  et_header_t* hdr,
                  void* payload,
                  uint32_t bufsize,
                  uint32_t timeout_us) {
    if (slave == NULL || !slave->is_open || hdr == NULL) {
        return -1;
    }

    /* Buffer para Ethernet header + EtherTux header */
    uint8_t frame[ET_MAX_FRAME];
    uint8_t* payload_ptr = frame + ET_ETH_HDR_SIZE;

    int n = et_socket_recv(&slave->port, frame, sizeof(frame), timeout_us, NULL);
    if (n < 0) {
        return -1;
    }

    /* Validar EtherType */
    if (n < (int)(ET_ETH_HDR_SIZE + ET_HEADER_SIZE)) {
        slave->stats.frame_errors++;
        return -1;
    }

    uint16_t eth_type = (uint16_t)((frame[12] << 8) | frame[13]);
    if (eth_type != ET_ETHERTYPE) {
        slave->stats.frame_errors++;
        return -1;
    }

    /* Parsear PDU (valida CRC) */
    et_header_t local_hdr;
    const uint8_t* pld_ptr;
    uint32_t parsed = et_pdu_parse(frame, (uint32_t)n, &local_hdr, &pld_ptr);

    if (parsed == 0) {
        slave->stats.frame_errors++;
        return -1;
    }

    /* Filtrar por destino: difusión (FF:FF:FF:FF:FF:FF) o unicast a nuestra MAC */
    uint8_t src_mac[6];
    memcpy(src_mac, frame + 6, 6);
    uint8_t dst_mac[6];
    memcpy(dst_mac, frame, 6);

    bool is_broadcast = (dst_mac[0] == 0xFF && dst_mac[1] == 0xFF);
    bool is_unicast_to_me = false;

    if (!is_broadcast) {
        /* Verificar si el destino es nuestra MAC */
        is_unicast_to_me = memcmp(dst_mac, slave->port.mac, 6) == 0;
    }

    if (!is_broadcast && !is_unicast_to_me) {
        slave->stats.dropped++;
        return 0; /* No se procesó, pero no es error */
    }

    /* Copiar header */
    memcpy(hdr, &local_hdr, sizeof(et_header_t));

    /* Copiar payload si hay espacio */
    uint32_t payload_len = parsed - ET_ETH_HDR_SIZE - ET_HEADER_SIZE;
    if (payload_len > 0 && payload != NULL && bufsize >= payload_len) {
        memcpy(payload, pld_ptr, payload_len);
    }

    slave->stats.frames_rx++;
    slave->stats.bytes_rx += n;

    return (int)payload_len;
}

/* ------------------------------------------------------------------
/* Estado esclavo (ET-ESM simple)
 * ------------------------------------------------------------------ */

static status_t et_esm_slave_transition(et_slave_t* slave, et_state_t next) {
    if (slave == NULL) {
        return ERROR_INVALID;
    }

    et_state_t current = (et_state_t)slave->esm_state;

    /* Transiciones válidas:
     * INIT -> CONFIG (tras Discovery)
     * CONFIG -> SAFE (tras mailbox vacío y sin Discovery pendiente)
     * SAFE -> OP (transición del maestro via StateChange)
     * OP -> FAULT (error crítico)
     * FAULT -> INIT (reset manual o timeout)
     * Cualquier estado -> FAULT (error)
     */

    if (current == next) {
        return SUCCESS;
    }

    if (current == ET_STATE_INIT && next == ET_STATE_CONFIG) {
        slave->esm_state = ET_STATE_CONFIG;
        return SUCCESS;
    }

    if (current == ET_STATE_CONFIG && next == ET_STATE_SAFE) {
        slave->esm_state = ET_STATE_SAFE;
        return SUCCESS;
    }

    if (current == ET_STATE_SAFE && next == ET_STATE_OP) {
        slave->esm_state = ET_STATE_OP;
        return SUCCESS;
    }

    if (current == ET_STATE_OP && next == ET_STATE_FAULT) {
        slave->esm_state = ET_STATE_FAULT;
        return SUCCESS;
    }

    if (current == ET_STATE_FAULT && next == ET_STATE_INIT) {
        slave->esm_state = ET_STATE_INIT;
        return SUCCESS;
    }

    return ERROR_STATE;
}