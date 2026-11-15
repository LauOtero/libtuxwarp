/*
 * et_master.c — Maestro EtherTux: ciclo de vida, envío/recepción y nodos.
 *
 * Camino de un paquete (spec §8.2):
 *
 *   generación -> clasificación P0–P5 -> control de admisión (QoS)
 *     -> ventana time-triggered (planificador) -> construcción de trama
 *     -> CRC-32C (HAL) -> anillo de socket -> pacing SO_TXTIME -> NIC
 *
 * Todo el estado del camino caliente está pre-asignado en el maestro: los
 * búferes de TX/RX son miembros de la estructura y el envío no reserva memoria.
 * La única ruta que asigna es `send_fec_block` (los bloques de paridad se
 * calculan por bloques, no por trama) y la decodificación FEC de recuperación.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/* Nodos                                                               */
/* ------------------------------------------------------------------ */

int et_master_touch_node(et_master_t* master, const uint8_t mac[6],
                         uint64_t now_ns) {
    if (master == NULL || mac == NULL) {
        return -1;
    }

    /*
     * Índice hash {MAC -> nodo} con sondeo lineal acotado: sustituye el barrido
     * de hasta 64 nodos por trama (≈100–200 ns) por 1–2 comparaciones.
     */
    const uint32_t h0 = (uint32_t)et_aalid_candidate(mac) & (ET_NODE_HASH_SLOTS - 1u);
    int32_t free_slot = -1;

    for (uint32_t p = 0; p < ET_NODE_HASH_PROBES; p++) {
        const uint32_t h = (h0 + p) & (ET_NODE_HASH_SLOTS - 1u);
        const uint8_t slot = master->node_hash[h];
        if (slot == 0u) {
            if (free_slot < 0) {
                free_slot = (int32_t)h;
            }
            break;
        }
        et_node_t* n = &master->nodes[slot - 1u];
        if (memcmp(n->mac, mac, 6u) == 0) {
            n->last_seen_ns = (int64_t)now_ns;
            n->missed_cycles = 0u;
            n->lost = false;
            return (int)(slot - 1u);
        }
    }

    if (master->node_count >= ET_MAX_NODES) {
        return -1;
    }
    if (free_slot < 0) {
        /* Cadena de sondeo llena: se busca un hueco de forma lineal (raro). */
        for (uint32_t i = 0; i < ET_NODE_HASH_SLOTS; i++) {
            if (master->node_hash[i] == 0u) {
                free_slot = (int32_t)i;
                break;
            }
        }
        if (free_slot < 0) {
            return -1;
        }
    }

    et_node_t* n = &master->nodes[master->node_count];
    memset(n, 0, sizeof(*n));
    memcpy(n->mac, mac, 6u);
    n->id = (uint16_t)(master->node_count + 1u);
    n->last_seen_ns = (int64_t)now_ns;
    master->node_hash[free_slot] = (uint8_t)(master->node_count + 1u);
    master->node_count++;
    return (int)(master->node_count - 1u);
}

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

et_master_t* et_master_create(void) {
    et_master_t* m = (et_master_t*)calloc(1u, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->port.fd = -1;
    m->link_bps = 1000000000ull;
    m->cycle_ns = 1000000u;
    m->local_id = ET_LOCAL_ID_NONE;

    m->qos = et_qos_create();
    if (m->qos == NULL) {
        free(m);
        return NULL;
    }
    return m;
}

void et_master_destroy(et_master_t* master) {
    if (master == NULL) {
        return;
    }
    et_master_close(master);
    et_aalid_destroy(master->aalid);
    et_qos_destroy(master->qos);
    et_fec_destroy(master->fec);
    free(master);
}

status_t et_master_open_ex(et_master_t* master,
                                               const char* ifname,
                                               uint64_t link_bps,
                                               uint32_t cycle_ns) {
    if (master == NULL || ifname == NULL || ifname[0] == '\0') {
        return ERROR_INVALID;
    }
    if (master->is_open) {
        return ERROR_STATE;
    }

    status_t st = et_socket_open(&master->port, ifname);
    if (st != SUCCESS) {
        return st;
    }
    /* AUTO: se intenta el anillo de transmisión; si el kernel no lo permite,
     * se continúa con llamadas al sistema sin alterar el comportamiento. */
    (void)et_socket_tx_ring_enable(&master->port, 64u);

    master->link_bps = (link_bps != 0u) ? link_bps : 1000000000ull;
    master->cycle_ns = (cycle_ns != 0u) ? cycle_ns : 1000000u;

    /* Si ya se fijó un ID local antes de abrir, la MAC de origen es su LAA. */
    if (master->local_id != ET_LOCAL_ID_NONE) {
        (void)et_aalid_mac(master->local_id, master->port.mac);
    }

    master->is_open = true;
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* ID local determinista                                               */
/* ------------------------------------------------------------------ */

status_t et_master_enable_local_ids(et_master_t* master,
                                                        const et_aalid_config_t* cfg) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    if (master->aalid != NULL) {
        return SUCCESS;
    }
    master->aalid = et_aalid_create(cfg);
    return (master->aalid != NULL) ? SUCCESS : ERROR_MEMORY;
}

status_t et_master_set_local_id(et_master_t* master, uint16_t id) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->local_id = id;
    if (id != ET_LOCAL_ID_NONE) {
        (void)et_aalid_mac(id, master->port.mac);
    }
    return SUCCESS;
}

uint16_t et_master_local_id(const et_master_t* master) {
    return (master != NULL) ? master->local_id : ET_LOCAL_ID_NONE;
}

et_aalid_t* et_master_aalid(et_master_t* master) {
    return (master != NULL) ? master->aalid : NULL;
}

status_t et_master_admit_node(et_master_t* master,
                                                  const uint8_t mac[6], uint32_t nonce,
                                                  uint64_t now_ms, uint16_t* out_id,
                                                  et_aalid_result_t* out_result) {
    if (master == NULL || master->aalid == NULL) {
        return ERROR_STATE;
    }
    return et_aalid_admit(master->aalid, mac, nonce, now_ms, out_id, out_result);
}

uint32_t et_master_expire_local_ids(et_master_t* master, uint64_t now_ms) {
    if (master == NULL || master->aalid == NULL) {
        return 0u;
    }
    return et_aalid_expire(master->aalid, now_ms);
}

bool et_master_accepts_destination(const et_master_t* master,
                                             const uint8_t mac[6]) {
    if (master == NULL || mac == NULL) {
        return false;
    }
    /* Difusión y multicast (bit I/G) siempre se aceptan. */
    if ((mac[0] & 0x01u) != 0u) {
        return true;
    }
    if (master->local_id == ET_LOCAL_ID_NONE) {
        return true;   /* sin ID local no se filtra por destino */
    }
    uint8_t self[6];
    (void)et_aalid_mac(master->local_id, self);
    return memcmp(mac, self, 6u) == 0;
}

status_t et_master_set_tx_mode(et_master_t* master,
                                                   et_tx_mode_t mode) {
    if (master == NULL || !master->is_open) {
        return ERROR_STATE;
    }
    switch (mode) {
        case ET_TX_SYSCALL:
            et_socket_tx_ring_disable(&master->port);
            return SUCCESS;
        case ET_TX_AUTO:
            if (!et_socket_tx_ring_active(&master->port)) {
                (void)et_socket_tx_ring_enable(&master->port, 64u);
            }
            return SUCCESS;
        case ET_TX_RING:
            if (et_socket_tx_ring_active(&master->port)) {
                return SUCCESS;
            }
            return et_socket_tx_ring_enable(&master->port, 64u)
                   ? SUCCESS : ERROR_NOTSUP;
        default:
            return ERROR_INVALID;
    }
}

bool et_master_tx_ring_active(const et_master_t* master) {
    return (master != NULL) && et_socket_tx_ring_active(&master->port);
}

status_t et_master_open(et_master_t* master,
                                            const char* ifname) {
    return et_master_open_ex(master, ifname, 0u, 0u);
}

void et_master_close(et_master_t* master) {
    if (master == NULL) {
        return;
    }
    et_socket_close(&master->port);
    master->is_open = false;
}

bool et_master_is_open(const et_master_t* master) {
    return (master != NULL) && master->is_open;
}

const char* et_master_interface(const et_master_t* master) {
    if (master == NULL || !master->is_open) {
        return NULL;
    }
    return master->port.ifname;
}

status_t et_master_set_qos(et_master_t* master,
                                               const et_qos_config_t* cfg) {
    if (master == NULL || cfg == NULL) {
        return ERROR_INVALID;
    }
    return et_qos_configure(master->qos, cfg);
}

status_t et_master_set_fec(et_master_t* master,
                                               et_fec_mode_t mode) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    et_fec_destroy(master->fec);
    master->fec = NULL;

    if (mode == ET_FEC_OFF) {
        return SUCCESS;
    }
    master->fec = et_fec_create_mode(mode);
    return (master->fec != NULL) ? SUCCESS : ERROR_INVALID;
}

status_t et_master_attach_scheduler(et_master_t* master,
                                                        et_sched_t* sched) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->sched = sched;   /* sin propiedad */
    return SUCCESS;
}

status_t et_master_attach_topology(et_master_t* master,
                                                       et_topology_t* topo) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->topo = topo;
    return SUCCESS;
}

status_t et_master_attach_sync(et_master_t* master,
                                                   et_sync_t* sync) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->sync = sync;
    return SUCCESS;
}

status_t et_master_attach_esm(et_master_t* master,
                                                  et_esm_t* esm) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->esm = esm;
    return SUCCESS;
}

status_t et_master_attach_mbx(et_master_t* master,
                                                  et_mbx_t* mbx) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->mbx = mbx;
    return SUCCESS;
}

status_t et_master_attach_frm(et_master_t* master,
                                                  et_frm_t* frm) {
    if (master == NULL) {
        return ERROR_INVALID;
    }
    master->frm = frm;
    return SUCCESS;
}

uint16_t et_master_last_wkc(const et_master_t* master) {
    return (master != NULL) ? master->last_wkc : 0u;
}

/* ------------------------------------------------------------------ */
/* Envío                                                               */
/* ------------------------------------------------------------------ */

/* Construye la trama (Ethernet II + PDU) en el búfer del maestro y la envía. */
static status_t et_transmit_pdu(et_master_t* master,
                                          uint16_t dest_id,
                                          const et_header_t* h,
                                          const void* payload, uint16_t len,
                                          bool paced, uint64_t deadline_ns) {
    /*
     * I-4: con anillo TX activo se construye la trama DIRECTAMENTE en la ranura
     * (cabecera Ethernet + PDU + CRC), sin el memcpy trama→anillo. Con pacing
     * (`SO_TXTIME`) no se puede: el instante de envío viaja en el `cmsghdr` de
     * `sendmsg`, así que se usa el búfer propio.
     */
    uint8_t* frame = master->tx_frame;
    bool in_ring = false;

    if (!paced && master->port.tx_ring_active) {
        uint8_t* slot = et_socket_tx_reserve(&master->port);
        if (slot != NULL) {
            frame = slot;
            in_ring = true;
        }
        /* Si el anillo está lleno se degrada al búfer propio: no se pierde trama. */
    }

    /*
     * Cabecera Ethernet II. Por defecto, difusión (el segmento EtherTux se
     * comporta como un bus). Con ID local y un nodo concreto, el destino es su
     * MAC LAA: el switch conmuta solo a ese puerto y se elimina el flooding.
     */
    if (dest_id == ET_LOCAL_ID_NONE) {
        memset(frame, 0xFF, 6);
    } else {
        (void)et_aalid_mac(dest_id, frame);
    }
    memcpy(frame + 6, master->port.mac, 6);
    uint16_t eth_type = htons((uint16_t)ET_ETHERTYPE);
    memcpy(frame + 12, &eth_type, sizeof(eth_type));

    uint32_t body = et_pdu_build(frame + ET_ETH_HDR_SIZE,
                                           ET_MAX_FRAME - ET_ETH_HDR_SIZE,
                                           h, payload, len);
    if (body == 0u) {
        if (in_ring) {
            et_socket_tx_cancel(&master->port);   /* libera la ranura sin enviar */
        }
        return ERROR_INVALID;
    }

    uint32_t total = ET_ETH_HDR_SIZE + body;
    int rc;
    if (in_ring) {
        et_socket_tx_commit(&master->port, total);
        (void)et_socket_tx_ring_kick(&master->port);
        rc = (int)total;
    } else {
        rc = paced ? et_socket_send_at(&master->port, frame, total, deadline_ns)
                   : et_socket_send(&master->port, frame, total);
    }
    if (rc < 0) {
        return ERROR_IO;
    }

    master->stats.frames_tx++;
    master->stats.bytes_tx += (uint64_t)len;
    return SUCCESS;
}

/* Rellena la cabecera EtherTux con las reglas comunes de flags y secuencia. */
static void et_fill_header(et_master_t* master, et_header_t* h,
                           uint8_t priority, et_type_t type, uint8_t flow_id,
                           et_state_t state, uint64_t now,
                           uint8_t flow_count, uint16_t block_id) {
    memset(h, 0, sizeof(*h));
    h->type = (uint8_t)type;
    h->priority = et_qos_effective_priority(master->qos, priority);
    h->state = (uint8_t)state;
    h->flow_id = flow_id;
    h->seq_num = ++master->seq[flow_id];
    h->tx_timestamp = now;
    h->wkc = 0u;              /* cada nodo que la procese lo incrementa */
    h->block_id = block_id;
    h->flow_count = flow_count;

    if (priority == ET_P0) {
        h->flags |= ET_FLAG_EXPRESS;
    } else {
        h->flags |= ET_FLAG_PREEMPTABLE;
    }
    if (type == ET_TYPE_FEC) {
        h->flags |= ET_FLAG_FEC;
    }
    if (type == ET_TYPE_DISCOVERY) {
        h->flags |= ET_FLAG_DISCOVERY;
    }
}

static status_t et_send_frame_ex(et_master_t* master,
                                           uint16_t dest_id,
                                           uint8_t priority,
                                           et_type_t type,
                                           uint8_t flow_id,
                                           const void* payload, uint16_t len,
                                           uint8_t flow_count,
                                           bool paced, uint64_t deadline_ns,
                                           uint16_t block_id) {
    if (master == NULL || !master->is_open) {
        return ERROR_STATE;
    }
    if (len > ET_MAX_PAYLOAD || flow_count == 0u) {
        return ERROR_INVALID;
    }
    if (priority >= ET_PRIORITIES) {
        return ERROR_INVALID;
    }

    /* ET-ESM: el estado del nodo emisor limita el tráfico permitido (§6.1). */
    et_state_t state = (master->esm != NULL) ? et_esm_state(master->esm)
                                                       : ET_STATE_OP;
    if (!et_esm_allows_traffic(state, (uint8_t)type, priority)) {
        return ERROR_STATE;
    }

    uint64_t now = tx_timestamp_ns();
    uint32_t wire = ET_ETH_HDR_SIZE + ET_HEADER_SIZE + len;

    status_t st = et_qos_admit(master->qos, priority, wire, now);
    if (st != SUCCESS) {
        if (st == ERROR_DROPPED) {
            master->stats.dropped++;
        } else {
            master->stats.backpressure++;
        }
        return st;
    }

    /* Ventana time-triggered (si hay planificador asociado). P0 siempre pasa. */
    if (master->sched != NULL) {
        /* RS-10: usar timestamp único y ventanas precomputadas.
         * Llamada única a tx_timestamp_ns() en lugar de dos llamadas duplicadas.
         * El cálculo de deadline usa ventanas precomputadas en sched->window_ns[]
         * sin división costosa en el camino caliente. */
        uint64_t now = tx_timestamp_ns();
        bool paced = true;
        if (paced) {
            uint8_t window = et_sched_window_at(master->sched, now);
            /* Determinar el inicio de la ventana actual usando valores precomputados. */
            uint64_t window_start = master->sched->window_ns[window];
            /* Si ya estamos past la ventana actual, programar la próxima.
             * Estrategia: window_start + n * cycle_ns donde n es el número de ciclo. */
            if (now >= window_start) {
                /* Calcular el próximo inicio de ventana para esta prioridad.
                 * Usar la fórmula: si now está en la ventana actual,
                 * la próxima ventana es la siguiente en la secuencia. */
                uint64_t next_window_start;
                if (priority == ET_P0) {
                    /* P0 puede transmitir en cualquier ventana; next_window_start = window actual + cycle */
                    next_window_start = window_start + master->sched->cycle_ns;
                } else {
                    /* Para P1-P5, determinar la ventana por prioridad */
                    uint8_t target_window;
                    if (priority == ET_P1) {
                        target_window = ET_WINDOW_CONTROL;
                    } else if (priority == ET_P2) {
                        target_window = ET_WINDOW_DETERMINISTIC;
                    } else if (priority <= 4) {
                        target_window = ET_WINDOW_RATE;
                    } else {
                        target_window = ET_WINDOW_BEST_EFFORT;
                    }
                    /* Calcular next_window_start basándose en la ventana objetivo.
                     * Si now ya pasó la ventana objetivo en el cycle actual,
                     * usar el siguiente cycle. */
                    next_window_start = master->sched->window_ns[target_window];
                    if (now >= next_window_start) {
                        next_window_start += master->sched->cycle_ns;
                    }
                }
                deadline_ns = next_window_start;
            }
        }
        if (!et_sched_can_transmit(master->sched, priority, now)) {
            master->stats.backpressure++;
            return ERROR_BACKPRESSURE;
        }
    } else if (!et_sched_can_transmit(master->sched, priority, now)) {
        master->stats.backpressure++;
        return ERROR_BACKPRESSURE;
    }

    et_header_t h;
    et_fill_header(master, &h, priority, type, flow_id, state, now, flow_count, block_id);

    st = et_transmit_pdu(master, dest_id, &h, payload, len, paced, deadline_ns);
    if (st != SUCCESS) {
        return st;
    }

    /*
     * FRM (§8.2): los flujos P0/P1 se duplican por el camino alternativo. El
     * duplicado conserva el SeqNum y marca `Flags[4]`; el receptor lo descarta
     * con `et_frm_note_rx`.
     */
    if (master->frm != NULL && flow_count == 1u &&
        et_frm_should_duplicate(master->frm, priority)) {
        et_header_t dup = h;
        dup.flow_id = et_frm_alternate_flow(flow_id);
        dup.flags = (uint8_t)(h.flags | ET_FLAG_FRM);
        if (et_transmit_pdu(master, dest_id, &dup, payload, len, paced, deadline_ns) ==
            SUCCESS) {
            master->stats.duplicates_tx++;
        }
    }

    return SUCCESS;
}

status_t et_master_send(et_master_t* master,
                                            uint8_t priority,
                                            et_type_t type,
                                            uint8_t flow_id,
                                            const void* payload, uint16_t len) {
    return et_send_frame_ex(master, ET_LOCAL_ID_NONE, priority, type, flow_id,
                            payload, len, 1u, false, 0u, 0u);
}

status_t et_master_send_to(et_master_t* master,
                                               uint16_t dest_id,
                                               uint8_t priority,
                                               et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload, uint16_t len) {
    return et_send_frame_ex(master, dest_id, priority, type, flow_id,
                            payload, len, 1u, false, 0u, 0u);
}

status_t et_master_send_at(et_master_t* master,
                                               uint8_t priority,
                                               et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload, uint16_t len,
                                               uint64_t tx_deadline_ns) {
    return et_send_frame_ex(master, ET_LOCAL_ID_NONE, priority, type, flow_id,
                            payload, len, 1u, true, tx_deadline_ns, 0u);
}

status_t et_master_send_aggregated(et_master_t* master,
                                                       uint8_t priority,
                                                       uint8_t flow_id,
                                                       const et_flow_entry_t* entries,
                                                       uint32_t count) {
    if (master == NULL || entries == NULL || count < 2u || count > UINT8_MAX) {
        return ERROR_INVALID;
    }
    uint32_t plen = 0u;
    status_t st = et_flowagg_build(entries, count, master->aux,
                                                       sizeof(master->aux), &plen);
    if (st != SUCCESS) {
        return st;
    }
    return et_send_frame_ex(master, ET_LOCAL_ID_NONE, priority,
                            ET_TYPE_DATA, flow_id,
                            master->aux, (uint16_t)plen, (uint8_t)count,
                            false, 0u, 0u);
}

status_t et_master_send_mailbox(et_master_t* master,
                                                    uint8_t channel,
                                                    const void* data, uint16_t len,
                                                    uint8_t p4_occupancy_percent) {
    if (master == NULL || master->mbx == NULL || data == NULL || len == 0u) {
        return ERROR_INVALID;
    }
    uint32_t plen = 0u;
    status_t st = et_mbx_next_segment(master->mbx, channel,
                                                          (const uint8_t*)data, len,
                                                          master->aux, sizeof(master->aux),
                                                          &plen);
    if (st != SUCCESS) {
        return st;
    }
    uint8_t prio = et_mbx_effective_priority(master->mbx, p4_occupancy_percent);
    return et_send_frame_ex(master, ET_LOCAL_ID_NONE, prio,
                            ET_TYPE_MAILBOX_DATA, 255u,
                            master->aux, (uint16_t)plen, 1u, false, 0u, 0u);
}

status_t et_master_send_fec_block(et_master_t* master,
                                                      uint8_t priority,
                                                      uint8_t flow_id,
                                                      const uint8_t* const* data,
                                                      uint32_t count,
                                                      uint32_t len) {
    if (master == NULL || data == NULL) {
        return ERROR_INVALID;
    }
    if (master->fec == NULL) {
        return ERROR_NOTSUP;
    }
    if (count != (uint32_t)master->fec->k || len == 0u ||
        len > ET_MAX_PAYLOAD) {
        return ERROR_INVALID;
    }

    uint8_t k = master->fec->k;
    uint8_t n = master->fec->n;

    /* Usar buffer pre-asignado en lugar de malloc/free (c elimination en el camino caliente). */
    uint8_t* parity_store = master->fec_parity;

    uint8_t* par_ptrs[RS_MAX_N];
    for (uint32_t j = 0; j < (uint32_t)(n - k); j++) {
        par_ptrs[j] = parity_store + (size_t)j * len;
    }

    status_t st = et_fec_encode(master->fec, data, par_ptrs, len);
    if (st != SUCCESS) {
        return st;
    }

    uint16_t base = (uint16_t)(((uint32_t)(master->block_seq & 0x00FFu)) << 8);

    for (uint32_t i = 0; i < k; i++) {
        st = et_send_frame_ex(master, ET_LOCAL_ID_NONE, priority,
                              ET_TYPE_DATA, flow_id,
                              data[i], (uint16_t)len, 1u, false, 0u,
                              (uint16_t)(base | (uint16_t)i));
        if (st != SUCCESS) {
            return st;
        }
    }
    for (uint32_t j = 0; j < (uint32_t)(n - k); j++) {
        st = et_send_frame_ex(master, ET_LOCAL_ID_NONE, priority,
                              ET_TYPE_FEC, flow_id,
                              par_ptrs[j], (uint16_t)len, 1u, false, 0u,
                              (uint16_t)(base | (uint16_t)(k + j)));
        if (st != SUCCESS) {
            return st;
        }
    }

    master->block_seq++;
    master->stats.fec_blocks++;
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Camino rápido: cero copias, lotes y velocidad de enlace             */
/* ------------------------------------------------------------------ */

uint64_t et_link_speed_bps(const char* ifname) {
    return et_socket_link_bps(ifname);
}

status_t et_master_open_auto(et_master_t* master,
                                                 const char* ifname) {
    if (master == NULL || ifname == NULL || ifname[0] == '\0') {
        return ERROR_INVALID;
    }
    uint64_t bps = et_socket_link_bps(ifname);
    if (bps == 0u) {
        bps = 1000000000ull;   /* enlace caído o no legible: 1 Gbps */
    }
    return et_master_open_ex(master, ifname, bps, 0u);
}

/* Cabecera Ethernet + EtherTux contiguas (14 + 26 B) con el CRC ya calculado. */
static uint32_t et_build_wire_header(et_master_t* master, uint8_t* out,
                                     const et_header_t* h,
                                     const void* payload, uint16_t len) {
    memset(out, 0xFF, 6);
    memcpy(out + 6, master->port.mac, 6);
    uint16_t eth_type = htons((uint16_t)ET_ETHERTYPE);
    memcpy(out + 12, &eth_type, sizeof(eth_type));
    return et_pdu_build_header(out + ET_ETH_HDR_SIZE, h, payload, len);
}

status_t et_master_send_zerocopy(et_master_t* master,
                                                     uint8_t priority,
                                                     et_type_t type,
                                                     uint8_t flow_id,
                                                     const void* payload, uint16_t len) {
    if (master == NULL || !master->is_open) {
        return ERROR_STATE;
    }
    if (len > ET_MAX_PAYLOAD || (payload == NULL && len > 0u)) {
        return ERROR_INVALID;
    }
    if (priority >= ET_PRIORITIES) {
        return ERROR_INVALID;
    }

    et_state_t state = (master->esm != NULL) ? et_esm_state(master->esm)
                                                       : ET_STATE_OP;
    if (!et_esm_allows_traffic(state, (uint8_t)type, priority)) {
        return ERROR_STATE;
    }

    uint64_t now = tx_timestamp_ns();
    uint32_t wire = ET_ETH_HDR_SIZE + ET_HEADER_SIZE + len;
    status_t st = et_qos_admit(master->qos, priority, wire, now);
    if (st != SUCCESS) {
        if (st == ERROR_DROPPED) {
            master->stats.dropped++;
        } else {
            master->stats.backpressure++;
        }
        return st;
    }
    if (master->sched != NULL &&
        !et_sched_can_transmit(master->sched, priority, now)) {
        master->stats.backpressure++;
        return ERROR_BACKPRESSURE;
    }

    et_header_t h;
    et_fill_header(master, &h, priority, type, flow_id, state, now, 1u, 0u);

    uint8_t* hdr = master->tx_frame;   /* solo los primeros 40 B */
    if (et_build_wire_header(master, hdr, &h, payload, len) == 0u) {
        return ERROR_INVALID;
    }

    uint32_t hdr_len = ET_ETH_HDR_SIZE + ET_HEADER_SIZE;
    if (et_socket_send_iov(&master->port, hdr, hdr_len, payload, len) < 0) {
        return ERROR_IO;
    }
    master->stats.frames_tx++;
    master->stats.bytes_tx += (uint64_t)len;
    return SUCCESS;
}

int et_master_send_batch(et_master_t* master,
                                   const et_batch_item_t* items,
                                   uint32_t count) {
    enum { ET_BATCH_MAX = 16u };

    if (master == NULL || !master->is_open || items == NULL || count == 0u) {
        return -1;
    }
    if (count > ET_BATCH_MAX) {
        count = ET_BATCH_MAX;
    }

    /* Estado pre-asignado del lote (el maestro no es thread-safe). */
    static struct mmsghdr msgs[ET_BATCH_MAX];
    static struct iovec   iovs[ET_BATCH_MAX][2];
    static uint8_t        hdrs[ET_BATCH_MAX][ET_ETH_HDR_SIZE +
                                            ET_HEADER_SIZE];

    const et_state_t state = (master->esm != NULL)
                                           ? et_esm_state(master->esm)
                                           : ET_STATE_OP;
    uint64_t now = tx_timestamp_ns();
    uint64_t bytes = 0u;
    uint32_t n = 0u;

    for (uint32_t i = 0; i < count; i++) {
        const et_batch_item_t* it = &items[i];
        if (it->priority >= ET_PRIORITIES ||
            it->len > ET_MAX_PAYLOAD ||
            (it->payload == NULL && it->len > 0u)) {
            continue;
        }
        if (!et_esm_allows_traffic(state, (uint8_t)it->type, it->priority)) {
            continue;
        }

        uint32_t wire = ET_ETH_HDR_SIZE + ET_HEADER_SIZE + it->len;
        status_t st = et_qos_admit(master->qos, it->priority, wire, now);
        if (st != SUCCESS) {
            if (st == ERROR_DROPPED) {
                master->stats.dropped++;
            } else {
                master->stats.backpressure++;
            }
            continue;
        }

        et_header_t h;
        et_fill_header(master, &h, it->priority, it->type, it->flow_id, state,
                       now, 1u, 0u);
        if (et_build_wire_header(master, hdrs[n], &h, it->payload, it->len) == 0u) {
            continue;
        }

        iovs[n][0].iov_base = hdrs[n];
        iovs[n][0].iov_len = ET_ETH_HDR_SIZE + ET_HEADER_SIZE;
        iovs[n][1].iov_base = (void*)it->payload;
        iovs[n][1].iov_len = it->len;

        memset(&msgs[n], 0, sizeof(msgs[n]));
        msgs[n].msg_hdr.msg_iov = iovs[n];
        msgs[n].msg_hdr.msg_iovlen = (it->len > 0u) ? 2u : 1u;

        bytes += wire;
        n++;
    }

    if (n == 0u) {
        return 0;
    }
    int sent = et_socket_send_batch(&master->port, msgs, n);
    if (sent < 0) {
        return -1;
    }
    master->stats.frames_tx += (uint64_t)sent;
    master->stats.bytes_tx += bytes;
    return sent;
}

/* ------------------------------------------------------------------ */
/* Recepción                                                           */
/* ------------------------------------------------------------------ */

int et_master_recv(et_master_t* master,
                             et_header_t* hdr,
                             void* payload, uint32_t bufsize,
                             uint32_t timeout_us) {
    if (master == NULL || !master->is_open) {
        return -1;
    }

    /* Se leen hasta 8 tramas para descartar duplicados FRM sin bloquear. */
    for (unsigned attempt = 0; attempt < 8u; attempt++) {
        int n = et_socket_recv(&master->port, master->rx_frame,
                               ET_MAX_FRAME,
                               (attempt == 0u) ? timeout_us : 0u, NULL);
        if (n < 0) {
            return -1;
        }

        if (n < (int)ET_ETH_HDR_SIZE) {
            master->stats.frame_errors++;
            return -1;
        }

        /*
 * Filtro de destino por ID local: en cuanto el maestro tiene su propia
 * MAC LAA, cualquier trama unicast ajena se descarta aquí (coste
 * constante, una comparación) en lugar de parsearla y validar su CRC.
 * Este check se realiza DESPUÉS de confirmar que la trama es EtherTux
 * y tiene tamaño mínimo, ANTES de parsear la PDU y el CRC.
 */
        if (master->local_id != ET_LOCAL_ID_NONE) {
            uint8_t da0 = master->rx_frame[0];
            if ((da0 & 0x01u) == 0u) {
                /* Unicast: comparar con la MAC local LAA */
                if (!master->port.mac_laa_valid) {
                    (void)et_aalid_mac(master->local_id, master->port.mac_laa);
                    master->port.mac_laa_valid = true;
                }
                if (memcmp(master->rx_frame + 0, master->port.mac_laa, 6u) != 0) {
                    master->stats.local_id_dropped++;
                    continue;
                }
            }
            /* Difusión (da0 & 0x01 != 0) siempre se acepta */
        }
        /* Sin ID local: aceptar todo (comportamiento legacy) */

        uint32_t body_len = (uint32_t)n - ET_ETH_HDR_SIZE;
        et_header_t h;
        const uint8_t* pl = NULL;
        uint32_t used = et_pdu_parse(master->rx_frame + ET_ETH_HDR_SIZE,
                                               body_len, &h, &pl);
        if (used == 0u) {
            master->stats.frame_errors++;
            return -1;
        }

        uint32_t pl_len = used - ET_HEADER_SIZE;
        if (pl_len > bufsize) {
            master->stats.frame_errors++;
            return -1;
        }

        const uint8_t* src_mac = master->rx_frame + 6;
        uint64_t now = tx_timestamp_ns();

        /* Si el origen ya usa una MAC local, su ID mantiene viva la entrada CAM
         * y el temporizador de admisión. El heartbeat tiene granularidad de
         * segundos (60 s), así que muestrear 1 de cada 64 tramas basta y evita
         * un hash por trama en el camino de recepción. */
        if (master->aalid != NULL && (master->stats.frames_rx & 63u) == 0u) {
            const uint16_t src_id = et_aalid_id_from_mac(src_mac);
            if (src_id != ET_LOCAL_ID_NONE) {
                (void)et_aalid_heartbeat(master->aalid, src_id, now / 1000000ull);
            }
        }

        int idx = et_master_touch_node(master, src_mac, now);
        uint16_t node_id = (idx >= 0) ? (uint16_t)(idx + 1) : 0u;

        master->last_wkc = h.wkc;

        /* FRM (§8.2): la segunda copia de un flujo crítico se descarta. */
        if (master->frm != NULL && node_id != 0u) {
            bool is_new = true;
            (void)et_frm_note_rx(master->frm, node_id, h.seq_num,
                                           (h.flags & ET_FLAG_FRM) != 0u,
                                           &is_new);
            if (!is_new) {
                continue;   /* duplicado: no se entrega a la aplicación */
            }
        }

        /* ET-WKC (§6.2): comparación con el valor esperado del registro. */
        if (master->topo != NULL) {
            if (et_topology_on_frame(master->topo, src_mac, &h, now) ==
                ERROR_CORRUPT) {
                master->stats.wkc_mismatches++;
            }
        }

        /* ET-Sync (§5): reporte de offset del nodo y clasificación. */
        if (master->sync != NULL && node_id != 0u &&
            h.type == (uint8_t)ET_TYPE_SYNC_REPORT) {
            int64_t off = 0;
            if (et_sync_parse_report(pl, pl_len, &off) == SUCCESS) {
                et_sync_status_t stt = ET_SYNC_OK;
                (void)et_sync_process_report(master->sync, node_id, off, now, &stt);
                if (stt == ET_SYNC_OUT_OF_SYNC) {
                    master->stats.sync_out_of_sync++;
                }
            }
        }

        /* ET-MBX (§9): ACK/NACK del mailbox. */
        if (master->mbx != NULL && h.type == (uint8_t)ET_TYPE_MAILBOX_ACK) {
            uint8_t ch = 0u;
            uint16_t seq = 0u;
            bool accepted = false;
            if (et_mbx_parse_ack(pl, pl_len, &ch, &seq, &accepted) ==
                SUCCESS) {
                (void)et_mbx_on_ack(master->mbx, ch, seq, accepted, now);
            }
        }

        if (payload != NULL && pl_len > 0u) {
            memcpy(payload, pl, pl_len);
        }
        if (hdr != NULL) {
            *hdr = h;
        }

        master->stats.frames_rx++;
        master->stats.bytes_rx += (uint64_t)pl_len;
        return (int)pl_len;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Heartbeat, nodos y sincronización                                   */
/* ------------------------------------------------------------------ */

uint32_t et_master_heartbeat(et_master_t* master, uint64_t now_ns) {
    if (master == NULL) {
        return 0u;
    }

    /* Con registro de nodos asociado, él es la autoridad del ciclo de vida. */
    if (master->topo != NULL) {
        return et_topology_tick(master->topo, now_ns, master->cycle_ns);
    }

    uint64_t limit = (uint64_t)master->cycle_ns * 3ull;
    uint32_t newly_lost = 0u;

    for (uint32_t i = 0; i < master->node_count; i++) {
        et_node_t* n = &master->nodes[i];
        if (n->lost || n->last_seen_ns <= 0) {
            continue;
        }
        uint64_t seen = (uint64_t)n->last_seen_ns;
        if (now_ns > seen && (now_ns - seen) > limit) {
            n->lost = true;
            newly_lost++;
            if (master->sched != NULL) {
                (void)et_sched_remove_node(master->sched, n->id);
            }
        }
    }
    return newly_lost;
}

uint32_t et_master_node_count(const et_master_t* master) {
    return (master != NULL) ? master->node_count : 0u;
}

bool et_master_node(const et_master_t* master, uint32_t index,
                              et_node_t* out) {
    if (master == NULL || out == NULL || index >= master->node_count) {
        return false;
    }
    *out = master->nodes[index];
    return true;
}

status_t et_master_rtt_sync(et_master_t* master,
                                                const uint8_t mac[6],
                                                uint32_t rtt_ns) {
    if (master == NULL || mac == NULL) {
        return ERROR_INVALID;
    }
    for (uint32_t i = 0; i < master->node_count; i++) {
        if (memcmp(master->nodes[i].mac, mac, 6) == 0) {
            master->nodes[i].rtt_ns = rtt_ns;
            /* Hipótesis de simetría: el reloj del nodo va RTT/2 por detrás. */
            master->nodes[i].clock_offset_ns = -(int64_t)(rtt_ns / 2u);
            return SUCCESS;
        }
    }
    return ERROR_INVALID;
}

status_t et_master_stats(const et_master_t* master,
                                             et_master_stats_t* out) {
    if (master == NULL || out == NULL) {
        return ERROR_INVALID;
    }
    *out = master->stats;
    return SUCCESS;
}

void et_master_reset_stats(et_master_t* master) {
    if (master != NULL) {
        memset(&master->stats, 0, sizeof(master->stats));
    }
}
