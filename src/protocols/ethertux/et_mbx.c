/*
 * et_mbx.c — ET-MBX: mailbox de gestión no crítica (§9).
 *
 * Canal fiable sobre el plano de gestión:
 *   - `MailboxData` (0x09): [canal 1B][seq 2B][índice 2B][total 2B][flags 1B][datos]
 *   - `MailboxAck`  (0x0A): [canal 1B][seq 2B][estado 1B]
 *
 * Reglas (§9.2):
 *   - Solo se usa en P4 (rate-constrained) o P5 (best-effort).
 *   - Si la ocupación de P4 supera `p4_degrade_percent` (70 % por defecto), el
 *     mailbox se degrada automáticamente a P5.
 *   - Reintentos acotados (3 por defecto) con backoff exponencial no bloqueante.
 *   - Segmentos de 512 B para no degradar el jitter.
 *
 * Reintento: si `et_mbx_retry_due()` es cierto, la siguiente llamada a
 * `et_mbx_next_segment()` **reemite el mismo segmento** (no avanza el
 * índice) y consume el reintento pendiente.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

/* Backoff base y factor: 200 µs, 400 µs, 800 µs… */
#define ET_MBX_BACKOFF_BASE_NS 200000ull

typedef struct {
    et_mbx_state_t state;
    bool     active;
    bool     firmware;
    uint16_t seq;
    uint16_t index;          /* segmento actual (0-based)     */
    uint16_t total;
    uint8_t  retries;
    uint16_t next_seq;
    uint32_t total_bytes;
    uint64_t next_retry_ns;
    bool     retry_pending;
} et_mbx_chan_t;

struct et_mbx {
    et_mbx_config_t cfg;
    et_mbx_chan_t             ch[ET_MBX_CHANNELS];
};

static inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void et_mbx_default_config(et_mbx_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->max_retries = (uint8_t)ET_MBX_MAX_RETRIES;
    cfg->segment_bytes = (uint16_t)ET_MBX_SEGMENT_BYTES;
    cfg->p4_degrade_percent = 70u;
}

et_mbx_t* et_mbx_create(void) {
    et_mbx_t* m = (et_mbx_t*)calloc(1u, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    et_mbx_default_config(&m->cfg);
    for (uint32_t i = 0; i < ET_MBX_CHANNELS; i++) {
        m->ch[i].state = ET_MBX_IDLE;
    }
    return m;
}

void et_mbx_destroy(et_mbx_t* mbx) {
    free(mbx);
}

status_t et_mbx_configure(et_mbx_t* mbx,
                                              const et_mbx_config_t* cfg) {
    if (mbx == NULL || cfg == NULL) {
        return ERROR_INVALID;
    }
    if (cfg->segment_bytes == 0u ||
        cfg->segment_bytes > (uint16_t)(ET_MAX_PAYLOAD - 8u) ||
        cfg->max_retries == 0u || cfg->p4_degrade_percent > 100u) {
        return ERROR_INVALID;
    }
    mbx->cfg = *cfg;
    return SUCCESS;
}

status_t et_mbx_begin(et_mbx_t* mbx, uint8_t channel,
                                          uint32_t total_bytes, bool firmware) {
    if (mbx == NULL || channel >= ET_MBX_CHANNELS) {
        return ERROR_INVALID;
    }
    et_mbx_chan_t* c = &mbx->ch[channel];

    uint32_t total = (total_bytes + (uint32_t)mbx->cfg.segment_bytes - 1u) /
                     (uint32_t)mbx->cfg.segment_bytes;
    if (total == 0u) {
        total = 1u;                    /* un segmento vacío es válido */
    }
    if (total > UINT16_MAX) {
        return ERROR_INVALID;
    }

    c->active = true;
    c->firmware = firmware;
    c->total_bytes = total_bytes;
    c->index = 0u;
    c->total = (uint16_t)total;
    c->retries = 0u;
    c->retry_pending = false;
    c->next_retry_ns = 0u;
    c->seq = ++c->next_seq;
    if (c->seq == 0u) {
        c->seq = ++c->next_seq;        /* 0 reservado */
    }
    c->state = ET_MBX_SENDING;
    return SUCCESS;
}

status_t et_mbx_status(const et_mbx_t* mbx,
                                           uint8_t channel,
                                           et_mbx_channel_status_t* out) {
    if (mbx == NULL || out == NULL || channel >= ET_MBX_CHANNELS) {
        return ERROR_INVALID;
    }
    const et_mbx_chan_t* c = &mbx->ch[channel];
    out->state = c->state;
    out->seq = c->seq;
    out->segment = c->index;
    out->total_segments = c->total;
    out->retries = c->retries;
    out->firmware = c->firmware;
    return SUCCESS;
}

status_t et_mbx_next_segment(et_mbx_t* mbx,
                                                 uint8_t channel,
                                                 const uint8_t* data, uint32_t len,
                                                 uint8_t* out, uint32_t out_size,
                                                 uint32_t* out_len) {
    if (mbx == NULL || out == NULL || out_len == NULL ||
        channel >= ET_MBX_CHANNELS) {
        return ERROR_INVALID;
    }
    et_mbx_chan_t* c = &mbx->ch[channel];
    const bool resend = c->retry_pending;

    if (!c->active || c->state == ET_MBX_DONE ||
        c->state == ET_MBX_FAILED) {
        return ERROR_STATE;
    }
    if (!resend && c->state != ET_MBX_SENDING) {
        return ERROR_STATE;
    }
    if (len > (uint32_t)mbx->cfg.segment_bytes) {
        return ERROR_INVALID;
    }
    if ((data == NULL && len > 0u) || out_size < (8u + len)) {
        return ERROR_INVALID;
    }
    if (c->index > c->total) {
        return ERROR_STATE;
    }

    /* En un reintento se reemite el último segmento emitido (índice - 1). */
    uint16_t emit_index = c->index;
    if (resend) {
        if (c->index == 0u) {
            return ERROR_STATE;
        }
        emit_index = (uint16_t)(c->index - 1u);
    }

    out[0] = channel;
    put_le16(out + 1u, c->seq);
    put_le16(out + 3u, emit_index);
    put_le16(out + 5u, c->total);
    out[7] = c->firmware ? 0x01u : 0x00u;
    if (len > 0u) {
        memcpy(out + 8u, data, len);
    }
    *out_len = 8u + len;

    if (!resend) {
        c->index++;
    }
    c->retry_pending = false;
    c->state = ET_MBX_WAIT_ACK;
    return SUCCESS;
}

status_t et_mbx_on_ack(et_mbx_t* mbx, uint8_t channel,
                                           uint16_t seq, bool accepted,
                                           uint64_t now_ns) {
    if (mbx == NULL || channel >= ET_MBX_CHANNELS) {
        return ERROR_INVALID;
    }
    et_mbx_chan_t* c = &mbx->ch[channel];
    if (!c->active || c->state != ET_MBX_WAIT_ACK || seq != c->seq) {
        return ERROR_STATE;
    }

    if (accepted) {
        c->retries = 0u;
        c->retry_pending = false;
        c->state = (c->index >= c->total) ? ET_MBX_DONE
                                          : ET_MBX_SENDING;
        return SUCCESS;
    }

    c->retries++;
    if (c->retries >= mbx->cfg.max_retries) {
        c->state = ET_MBX_FAILED;
        c->retry_pending = false;
        return ERROR_TIMEOUT;
    }

    /* Backoff exponencial no bloqueante: 200 µs, 400 µs, 800 µs… */
    uint64_t backoff = ET_MBX_BACKOFF_BASE_NS << (c->retries - 1u);
    c->next_retry_ns = now_ns + backoff;
    c->retry_pending = true;
    return ERROR_BACKPRESSURE;   /* reintento programado (NACK) */
}

bool et_mbx_retry_due(const et_mbx_t* mbx, uint8_t channel,
                                uint64_t now_ns) {
    if (mbx == NULL || channel >= ET_MBX_CHANNELS) {
        return false;
    }
    const et_mbx_chan_t* c = &mbx->ch[channel];
    return c->retry_pending && now_ns >= c->next_retry_ns;
}

uint8_t et_mbx_effective_priority(const et_mbx_t* mbx,
                                            uint8_t p4_occupancy_percent) {
    if (mbx == NULL) {
        return (uint8_t)ET_P5;
    }
    if (p4_occupancy_percent > mbx->cfg.p4_degrade_percent) {
        return (uint8_t)ET_P5;   /* degradación automática §9.2 */
    }
    return (uint8_t)ET_P4;
}

uint32_t et_mbx_build_ack(uint8_t channel, uint16_t seq, bool accepted,
                                    uint8_t out[ET_MBX_ACK_SIZE]) {
    if (out == NULL || channel >= ET_MBX_CHANNELS) {
        return 0u;
    }
    out[0] = channel;
    put_le16(out + 1u, seq);
    out[3] = accepted ? 0x01u : 0x00u;
    return ET_MBX_ACK_SIZE;
}

status_t et_mbx_parse_ack(const uint8_t* in, uint32_t len,
                                              uint8_t* channel, uint16_t* seq,
                                              bool* accepted) {
    if (in == NULL || len < ET_MBX_ACK_SIZE) {
        return ERROR_INVALID;
    }
    uint8_t ch = in[0];
    if (ch >= ET_MBX_CHANNELS) {
        return ERROR_INVALID;
    }
    if (channel != NULL) {
        *channel = ch;
    }
    if (seq != NULL) {
        *seq = get_le16(in + 1u);
    }
    if (accepted != NULL) {
        *accepted = (in[3] & 0x01u) != 0u;
    }
    return SUCCESS;
}

status_t et_mbx_parse_data(const uint8_t* in, uint32_t len,
                                               uint8_t* channel, uint16_t* seq,
                                               uint16_t* index, uint16_t* total,
                                               const uint8_t** data, uint16_t* data_len) {
    if (in == NULL || len < 8u) {
        return ERROR_INVALID;
    }
    uint8_t ch = in[0];
    uint16_t total_segments = get_le16(in + 5u);
    if (ch >= ET_MBX_CHANNELS || total_segments == 0u) {
        return ERROR_INVALID;
    }
    uint16_t idx = get_le16(in + 3u);
    if (idx >= total_segments) {
        return ERROR_INVALID;
    }

    if (channel != NULL) {
        *channel = ch;
    }
    if (seq != NULL) {
        *seq = get_le16(in + 1u);
    }
    if (index != NULL) {
        *index = idx;
    }
    if (total != NULL) {
        *total = total_segments;
    }
    if (data != NULL) {
        *data = in + 8u;
    }
    if (data_len != NULL) {
        *data_len = (uint16_t)(len - 8u);
    }
    return SUCCESS;
}
