/*
 * et_esm.c — Máquina de estados del nodo EtherTux (ET-ESM, §6.1).
 *
 *   INIT → CONFIG → SAFE → OP
 *     └──────┴───────┴──────┴──→ FAULT (cuarentena)
 *   FAULT → INIT (reintroducción tras reparación, vía Discovery)
 *
 * El estado del emisor viaja en el campo `State` de cada trama y determina qué
 * tráfico puede generarse: un nodo en FAULT solo responde a Discovery, de modo
 * que un nodo defectuoso no consume ancho de banda ni provoca bloqueos.
 */
#include "et_internal.h"

#include <stdlib.h>

struct et_esm {
    et_state_t state;
};

et_esm_t* et_esm_create(void) {
    et_esm_t* esm = (et_esm_t*)calloc(1u, sizeof(*esm));
    if (esm != NULL) {
        esm->state = ET_STATE_INIT;
    }
    return esm;
}

void et_esm_destroy(et_esm_t* esm) {
    free(esm);
}

void et_esm_reset(et_esm_t* esm) {
    if (esm != NULL) {
        esm->state = ET_STATE_INIT;
    }
}

et_state_t et_esm_state(const et_esm_t* esm) {
    return (esm != NULL) ? esm->state : ET_STATE_FAULT;
}

bool et_esm_can_transition(et_state_t from,
                                     et_state_t to) {
    switch (from) {
        case ET_STATE_INIT:
            return to == ET_STATE_CONFIG || to == ET_STATE_FAULT;
        case ET_STATE_CONFIG:
            return to == ET_STATE_SAFE || to == ET_STATE_FAULT ||
                   to == ET_STATE_INIT;
        case ET_STATE_SAFE:
            return to == ET_STATE_OP || to == ET_STATE_FAULT ||
                   to == ET_STATE_CONFIG;
        case ET_STATE_OP:
            return to == ET_STATE_SAFE || to == ET_STATE_FAULT;
        case ET_STATE_FAULT:
            return to == ET_STATE_INIT;
        default:
            return false;
    }
}

status_t et_esm_request(et_esm_t* esm,
                                            et_state_t next) {
    if (esm == NULL) {
        return ERROR_INVALID;
    }
    if (!et_esm_can_transition(esm->state, next)) {
        return ERROR_STATE;
    }
    esm->state = next;
    return SUCCESS;
}

bool et_esm_allows_traffic(et_state_t state,
                                     uint8_t type, uint8_t priority) {
    if (priority >= ET_PRIORITIES) {
        return false;
    }
    const bool mailbox = (type == (uint8_t)ET_TYPE_MAILBOX_DATA ||
                          type == (uint8_t)ET_TYPE_MAILBOX_ACK);
    const bool discovery = (type == (uint8_t)ET_TYPE_DISCOVERY);

    switch (state) {
        case ET_STATE_INIT:
        case ET_STATE_FAULT:
            return discovery;
        case ET_STATE_CONFIG:
            return discovery || mailbox;
        case ET_STATE_SAFE:
            return mailbox || priority == (uint8_t)ET_P0 ||
                   priority == (uint8_t)ET_P1;
        case ET_STATE_OP:
            return true;
        default:
            return false;
    }
}

uint32_t et_esm_build_state_change(et_state_t target,
                                             uint8_t reason,
                                             uint8_t out[2]) {
    if (out == NULL || (uint8_t)target > (uint8_t)ET_STATE_FAULT) {
        return 0u;
    }
    out[0] = (uint8_t)target;
    out[1] = reason;
    return 2u;
}

status_t et_esm_parse_state_change(const uint8_t* in, uint32_t len,
                                                       et_state_t* target,
                                                       uint8_t* reason) {
    if (in == NULL || len < 2u) {
        return ERROR_INVALID;
    }
    if (in[0] > (uint8_t)ET_STATE_FAULT) {
        return ERROR_INVALID;
    }
    if (target != NULL) {
        *target = (et_state_t)in[0];
    }
    if (reason != NULL) {
        *reason = in[1];
    }
    return SUCCESS;
}

bool et_esm_state_from_wire(uint8_t raw, et_state_t* out) {
    if (raw > (uint8_t)ET_STATE_FAULT) {
        return false;
    }
    if (out != NULL) {
        *out = (et_state_t)raw;
    }
    return true;
}
