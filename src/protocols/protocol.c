/*
 * protocol.c — Resolución del protocolo de red (EtherCAT / EtherTux).
 */
#include "protocol.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

protocol_t protocol_default(void) {
    return PROTOCOL_ETHERCAT;
}

protocol_t protocol_from_string(const char* name) {
    if (name == NULL) {
        return protocol_default();
    }

    /* Normaliza a minúsculas descartando espacios y tabuladores. */
    char buf[16];
    size_t len = 0u;
    for (const char* p = name; *p != '\0' && len < sizeof(buf) - 1u; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') {
            continue;
        }
        buf[len++] = (char)tolower(c);
    }
    buf[len] = '\0';

    if (strcmp(buf, "ethertux") == 0) {
        return PROTOCOL_ETHERTUX;
    }
    if (strcmp(buf, "ethercat") == 0) {
        return PROTOCOL_ETHERCAT;
    }
    return protocol_default();
}

const char* protocol_name(protocol_t protocol) {
    return (protocol == PROTOCOL_ETHERTUX) ? "ethertux" : "ethercat";
}

protocol_t protocol_from_env(void) {
    const char* env = getenv("PROTOCOL");
    if (env == NULL || env[0] == '\0') {
        return protocol_default();
    }
    return protocol_from_string(env);
}
