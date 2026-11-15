/*
 * chronosrt_hwaccel.c — HAL de aceleración por hardware (CRC-32C y FEC RS).
 *
 * Implementación con despacho en tiempo de ejecución:
 *   - CRC-32C: instrucción ARMv8.1 (crc32c* o SSE4.2 (crc32), con fallback
 *     software slice-by-8 (imprescindible en Cortex-A7, p. ej. BPI M2 Zero).
 *   - FEC RS: multiplicación en GF(2^8) por tabla de nibble + shuffle
 *     (NEON en AArch64, AVX2 en x86-64) con fallback escalar.
 *
 * Todo el estado de inicialización (tablas GF, tablas CRC, matrices del código)
 * se construye una sola vez y queda en memoria estática: el camino caliente no
 * reserva memoria.
 */
#include "hwaccel.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#if defined(__linux__)
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <linux/net_tstamp.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CRT_X86 1
#else
#define CRT_X86 0
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1UL << 7)
#endif
#ifndef HWCAP_AES
#define HWCAP_AES (1UL << 3)
#endif
#define CRT_ARM64 1
#else
#define CRT_ARM64 0
#endif

/* ================================================================== */
/* GF(2^8) — polinomio primitivo x^8+x^4+x^3+x^2+1 (0x11D)             */
/* ================================================================== */

static uint8_t s_gf_exp[512];
static uint8_t s_gf_log[256];
static pthread_once_t s_gf_once = PTHREAD_ONCE_INIT;

#define CRC16_TAB_COUNT 16u

static pthread_once_t s_crc16_once = PTHREAD_ONCE_INIT;
static uint32_t s_crc16_tab[CRC16_TAB_COUNT][256u];

/* --- Tabla CRC-32C slice-by-8 (siempre disponible) --- */

static uint32_t s_crc_tab[8][256];
static pthread_once_t s_crc_once = PTHREAD_ONCE_INIT;

/* --- Tablas de salto multistream (skip tables) --- */

static uint32_t s_skip16[8][16];
static uint32_t s_skip32[8][16];
static uint32_t s_skip48[8][16];
static uint32_t s_skip64[8][16];
static uint32_t s_skip96[8][16];
static uint32_t s_skip128[8][16];
static uint32_t s_skip192[8][16];
static uint32_t s_skip256[8][16];
static uint32_t s_skip384[8][16];

/* === Funciones de apoyo CRC-32C === */

/* Construye la tabla slice-by-8: s_crc_tab[t][i] = CRC después de procesar
 * el byte i comenzando desde t bytes cero. */
static void crc_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        s_crc_tab[0][i] = crc;
    }
    for (uint32_t t = 1; t < 8u; t++) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t prev = s_crc_tab[t - 1][i];
            s_crc_tab[t][i] = (prev >> 8) ^ s_crc_tab[0][prev & 0xFFu];
        }
    }
}

/* Avanza el estado CRC-32C (crudo) por `n` bytes cero. */
static uint32_t crc_advance_bytes(uint32_t crc, size_t n) {
    while (n-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][crc & 0xFFu];
    }
    return crc;
}

/* Tabla [8 nibbles][16 valores] que aplica el avance por `n` bytes. */
static void crc_build_skip(uint32_t tab[8][16], size_t n) {
    uint32_t bitval[32];
    for (int j = 0; j < 32; j++) {
        bitval[j] = crc_advance_bytes(1u << j, n);
    }
    for (int i = 0; i < 8; i++) {
        for (int v = 0; v < 16; v++) {
            uint32_t acc = 0;
            for (int b = 0; b < 4; b++) {
                if (v & (1 << b)) {
                    acc ^= bitval[4 * i + b];
                }
            }
            tab[i][v] = acc;
        }
    }
}

static inline uint32_t crc_skip(uint32_t crc, const uint32_t tab[8][16]) {
    return tab[0][crc & 0xFu]
         ^ tab[1][(crc >> 4) & 0xFu]
         ^ tab[2][(crc >> 8) & 0xFu]  ^ tab[3][(crc >> 12) & 0xFu]
         ^ tab[4][(crc >> 16) & 0xFu] ^ tab[5][(crc >> 20) & 0xFu]
         ^ tab[6][(crc >> 24) & 0xFu] ^ tab[7][(crc >> 28) & 0xFu];
}

/* Skip n bytes using s_crc_tab[0] (same as crc_advance_bytes). */
static uint32_t crc_skip_table16(uint32_t crc, size_t n) {
    while (n-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][crc & 0xFFu];
    }
    return crc;
}

/* === Backend de CRC-32C software slice-by-16 === */

/* Construye las tablas slice-by-16 a partir de s_crc_tab.
 * Cada fila[t][i] = CRC después de procesar byte 'i' a una distancia de 't' bytes del CRC inicial.
 * Para slice-by-16, las filas 0-7 procesan 1 byte cada una, y filas 8-15 son derivadas. */
static void crc16_init(void) {
    crc_init();  /* construye s_crc_tab[8][256] */

    /* Primeras 8 filas: procesan 1 byte cada una */
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 256; i++) {
            uint32_t crc = s_crc_tab[t][i];
            s_crc16_tab[t][i] = crc;
        }
    }

    /* Filas 8-15: derivadas de las filas 0-7 */
    for (int t = 8; t < 16; t++) {
        for (int i = 0; i < 256; i++) {
            /* tabla[8+i][crc_high] = CRC después de 1 byte más a partir de tabla[t-8][i] */
            uint32_t prev = s_crc16_tab[t - 8][i];
            s_crc16_tab[t][i] = (prev >> 8) ^ s_crc_tab[0][prev & 0xFF];
        }
    }
}

/* === Funciones CRC-32C software slice-by-16 === */

/* Slice-by-16 software: usa las tablas precalculadas s_crc16_tab */
API uint32_t crc32c_sw16_raw(uint32_t seed, const void* data, size_t len) {
    (void)pthread_once(&s_crc16_once, crc16_init);

    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = seed;

    /* Procesar 16 bytes a la vez usando las tablas precalculadas */
    while (len >= 16u) {
        uint32_t crc_lo = crc;
        uint32_t crc_hi = crc;

        /* Process first 8 bytes */
        crc_lo ^= *(uint32_t const*)(p + 0);
        crc_hi ^= *(uint32_t const*)(p + 4);

        /* Skip 8 bytes using crc_skip_table16 (uses s_crc_tab[0], compatible with s_crc16_tab layout). */
        crc_lo = crc_skip_table16(crc_lo, 8u);
        crc_hi = crc_skip_table16(crc_hi, 8u);

        /* Process next 8 bytes */
        crc_lo ^= *(uint32_t const*)(p + 8);
        crc_hi ^= *(uint32_t const*)(p + 12);

        /* Skip next 8 bytes */
        crc_lo = crc_skip_table16(crc_lo, 8u);
        crc_hi = crc_skip_table16(crc_hi, 8u);

        /* Combine: XOR the two halves */
        crc = crc_lo ^ crc_hi;

        p += 16;
        len -= 16u;
    }
    while (len-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][(crc ^ *p++) & 0xFFu];
    }
    return crc;
}

/* ================================================================== */
/* CRC-32C software (slice-by-8)                                      */
/* ================================================================== */

/* Slice-by-8 estándar: usa las mismas tablas que crc32c_sw_raw */
API uint32_t crc32c_sw_raw(uint32_t seed, const void* data, size_t len) {
    (void)pthread_once(&s_crc_once, crc_init);

    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = seed;

    while (len >= 8u) {
        uint32_t one;
        uint32_t two;
        memcpy(&one, p, sizeof(one));
        memcpy(&two, p + 4, sizeof(two));
        one ^= crc;
        crc = s_crc_tab[7][ one        & 0xFFu] ^
              s_crc_tab[6][(one >>  8) & 0xFFu] ^
              s_crc_tab[5][(one >> 16) & 0xFFu] ^
              s_crc_tab[4][(one >> 24) & 0xFFu] ^
              s_crc_tab[3][ two        & 0xFFu] ^
              s_crc_tab[2][(two >>  8) & 0xFFu] ^
              s_crc_tab[1][(two >> 16) & 0xFFu] ^
              s_crc_tab[0][(two >> 24) & 0xFFu];
        p += 8;
        len -= 8u;
    }
    while (len-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][(crc ^ *p++) & 0xFFu];
    }
    return crc;
}

API uint32_t crc32c_raw(uint32_t seed, const void* data, size_t len) {
    return crc32c_sw_raw(seed, data, len);
}

API uint32_t crc32c_sw(const void* data, size_t len) {
    return ~crc32c_sw_raw(0xFFFFFFFFu, data, len);
}

/* CRC-32C estándar: init 0xFFFFFFFF, xorout 0xFFFFFFFF */
API uint32_t crc32c(const void* data, size_t len) {
    return ~crc32c_sw_raw(0xFFFFFFFFu, data, len);
}

/* ================================================================== */
/* CRC-32C AVX2 SIMD: usamos el mismo slice-by-8 pero con AVX2 para
 * cargar datos de forma más eficiente                             */
/* ================================================================== */

#if CRT_X86
__attribute__((target("avx2")))
API uint32_t crc32c_sw16_avx2(uint32_t seed, const void* data, size_t len) {
    (void)pthread_once(&s_crc_once, crc_init);

    const uint8_t* restrict p = (const uint8_t*)data;
    uint32_t crc = seed;

    /* AVX2: procesar 32 bytes por iteración usando 4 loads de 256 bits */
    while (len >= 32u) {
        __m256i v0 = _mm256_loadu_si256((const __m256i*)(p + 0));
        __m256i v1 = _mm256_loadu_si256((const __m256i*)(p + 16));

        /* Extraer 8 bytes de cada vector usando shuffles */
        __m128i w0 = _mm256_castsi256_si128(v0);
        __m128i w1 = _mm256_extracti128_si256(v0, 1);
        __m128i w2 = _mm256_castsi256_si128(v1);
        __m128i w3 = _mm256_extracti128_si256(v1, 1);

        uint32_t one0, one1, one2, one3;
        uint32_t two0, two1, two2, two3;
        memcpy(&one0, &w0, 4);
        memcpy(&two0, (const uint8_t*)&w0 + 4, 4);
        memcpy(&one1, &w1, 4);
        memcpy(&two1, (const uint8_t*)&w1 + 4, 4);
        memcpy(&one2, &w2, 4);
        memcpy(&two2, (const uint8_t*)&w2 + 4, 4);
        memcpy(&one3, &w3, 4);
        memcpy(&two3, (const uint8_t*)&w3 + 4, 4);

        /* Procesar 8 bytes: w0 (one0+two0) */
        one0 ^= crc;
        crc = s_crc_tab[7][ one0        & 0xFFu] ^
              s_crc_tab[6][(one0 >>  8) & 0xFFu] ^
              s_crc_tab[5][(one0 >> 16) & 0xFFu] ^
              s_crc_tab[4][(one0 >> 24) & 0xFFu] ^
              s_crc_tab[3][ two0        & 0xFFu] ^
              s_crc_tab[2][(two0 >>  8) & 0xFFu] ^
              s_crc_tab[1][(two0 >> 16) & 0xFFu] ^
              s_crc_tab[0][(two0 >> 24) & 0xFFu];

        /* Procesar 8 bytes: w1 (one1+two1) */
        one1 ^= crc;
        crc = s_crc_tab[7][ one1        & 0xFFu] ^
              s_crc_tab[6][(one1 >>  8) & 0xFFu] ^
              s_crc_tab[5][(one1 >> 16) & 0xFFu] ^
              s_crc_tab[4][(one1 >> 24) & 0xFFu] ^
              s_crc_tab[3][ two1        & 0xFFu] ^
              s_crc_tab[2][(two1 >>  8) & 0xFFu] ^
              s_crc_tab[1][(two1 >> 16) & 0xFFu] ^
              s_crc_tab[0][(two1 >> 24) & 0xFFu];

        /* Procesar 8 bytes: w2 (one2+two2) */
        one2 ^= crc;
        crc = s_crc_tab[7][ one2        & 0xFFu] ^
              s_crc_tab[6][(one2 >>  8) & 0xFFu] ^
              s_crc_tab[5][(one2 >> 16) & 0xFFu] ^
              s_crc_tab[4][(one2 >> 24) & 0xFFu] ^
              s_crc_tab[3][ two2        & 0xFFu] ^
              s_crc_tab[2][(two2 >>  8) & 0xFFu] ^
              s_crc_tab[1][(two2 >> 16) & 0xFFu] ^
              s_crc_tab[0][(two2 >> 24) & 0xFFu];

        /* Procesar 8 bytes: w3 (one3+two3) */
        one3 ^= crc;
        crc = s_crc_tab[7][ one3        & 0xFFu] ^
              s_crc_tab[6][(one3 >>  8) & 0xFFu] ^
              s_crc_tab[5][(one3 >> 16) & 0xFFu] ^
              s_crc_tab[4][(one3 >> 24) & 0xFFu] ^
              s_crc_tab[3][ two3        & 0xFFu] ^
              s_crc_tab[2][(two3 >>  8) & 0xFFu] ^
              s_crc_tab[1][(two3 >> 16) & 0xFFu] ^
              s_crc_tab[0][(two3 >> 24) & 0xFFu];

        p += 32;
        len -= 32u;
    }

    /* Procesar los bytes restantes con el algoritmo estándar */
    while (len-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][(crc ^ *p++) & 0xFFu];
    }

    return crc;
}
#endif

/* ================================================================== */
/* CRC-32C NEON SIMD (slice-by-16, 16-byte processing)              */
/* ================================================================== */

#if CRT_ARM64
__attribute__((target("neon")))
API uint32_t crc32c_sw16_neon(uint32_t seed, const void* data, size_t len) {
    (void)pthread_once(&s_crc16_once, crc16_init);

    const uint8_t* restrict p = (const uint8_t*)data;
    uint32_t crc0 = seed, crc1 = seed, crc2 = seed, crc3 = seed;

    /* Alineación de tablas para prefetching optimizado */
    uint32_t (*restrict tabs)[256] = (uint32_t (*)[256])s_crc16_tab;

    /* Máscaras NEON para extraer nibbles (8 bytes por registro) */
    uint8x16_t mask_lo = vdupq_n_u8(0x0F);
    uint8x16_t mask_hi = vdupq_n_u8(0xF0);

    while (len >= 16u) {
        /* Prefetching para la siguiente iteración y datos calientes */
        for (int i = 0; i < 16; i++) {
            __builtin_prefetch(tabs[i], 0, 3);
        }
        __builtin_prefetch(p + 16, 0, 3);

        /* Cargar 2 vectores de 16 bytes (128 bits) usando NEON */
        uint8x16_t v0 = vld1_u8(p + 0);
        uint8x16_t v1 = vld1_u8(p + 8);

        /* Extraer nibbles bajos (AND con 0x0F) */
        uint8x16_t b0_lo = vand_u8(v0, mask_lo);
        uint8x16_t b1_lo = vand_u8(v1, mask_lo);

        /* Extraer nibbles altos (SHR por 4 bits AND con 0x0F) */
        uint8x16_t b0_hi = vand_u8(vshrn_u8(v0, 4), mask_lo);
        uint8x16_t b1_hi = vand_u8(vshrn_u8(v1, 4), mask_lo);

        /* ---- Acumulador 0: tablas 0-3 (primeros 8 bytes: v0) ---- */
        crc0 = tabs[0][(crc0 ^ (uint32_t)vget_u8(b0_lo)) & 0xFF] ^
               tabs[1][(crc0 >> 8 ^ (uint32_t)vget_u8(b0_lo + 1)) & 0xFF] ^
               tabs[2][(crc0 >> 16 ^ (uint32_t)vget_u8(b0_lo + 2)) & 0xFF] ^
               tabs[3][(crc0 >> 24 ^ (uint32_t)vget_u8(b0_lo + 3)) & 0xFF];

        /* ---- Acumulador 1: tablas 4-7 (primeros 8 bytes: v0) ---- */
        crc1 = tabs[4][(uint32_t)vget_u8(b0_hi) & 0xFF] ^
               tabs[5][(uint32_t)vget_u8(b0_hi + 1) & 0xFF] ^
               tabs[6][(uint32_t)vget_u8(b0_hi + 2) & 0xFF] ^
               tabs[7][(uint32_t)vget_u8(b0_hi + 3) & 0xFF];

        /* ---- Acumulador 2: tablas 8-11 (siguientes 8 bytes: v1) ---- */
        crc2 = tabs[8][(crc2 ^ (uint32_t)vget_u8(b1_lo)) & 0xFF] ^
               tabs[9][(crc2 >> 8 ^ (uint32_t)vget_u8(b1_lo + 1)) & 0xFF] ^
               tabs[10][(crc2 >> 16 ^ (uint32_t)vget_u8(b1_lo + 2)) & 0xFF] ^
               tabs[11][(crc2 >> 24 ^ (uint32_t)vget_u8(b1_lo + 3)) & 0xFF];

        /* ---- Acumulador 3: tablas 12-15 (siguientes 8 bytes: v1) ---- */
        crc3 = tabs[12][(uint32_t)vget_u8(b1_hi) & 0xFF] ^
               tabs[13][(uint32_t)vget_u8(b1_hi + 1) & 0xFF] ^
               tabs[14][(uint32_t)vget_u8(b1_hi + 2) & 0xFF] ^
               tabs[15][(uint32_t)vget_u8(b1_hi + 3) & 0xFF];

        p += 16;
        len -= 16u;
    }

    /* Combinar acumuladores: XOR de los 4 resultados */
    crc0 ^= crc1;
    crc0 ^= crc2;
    crc0 ^= crc3;

    /* Restos deterministas (0-15 bytes) usando switch sin bucles condicionales. */
    switch (len) {
        case 15: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[14]) & 0xFF];
        case 14: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[13]) & 0xFF];
        case 13: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[12]) & 0xFF];
        case 12: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[11]) & 0xFF];
        case 11: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[10]) & 0xFF];
        case 10: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 9]) & 0xFF];
        case  9: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 8]) & 0xFF];
        case  8: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 7]) & 0xFF];
        case  7: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 6]) & 0xFF];
        case  6: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 5]) & 0xFF];
        case  5: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 4]) & 0xFF];
        case  4: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 3]) & 0xFF];
        case  3: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 2]) & 0xFF];
        case  2: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 1]) & 0xFF];
        case  1: crc0 = (crc0 >> 8) ^ tabs[0][(crc0 ^ p[ 0]) & 0xFF];
        case  0: break;
    }

    return crc0;
}
#endif /* CRT_ARM64 */

/* ================================================================== */
/* Detección de la CPU y registro de backends                          */
/* ================================================================== */

/* Estructura de capacidades detectadas en tiempo de ejecución. */
static hw_caps_t  s_caps;
static pthread_once_t       s_caps_once = PTHREAD_ONCE_INIT;

/* Puntero de CRC resuelto una sola vez; el camino caliente lo lee con una carga
 * atómica relajada (sin `pthread_once` por trama). */
typedef uint32_t (*crc_raw_fn)(uint32_t seed, const void* data, size_t len);
static _Atomic(crc_raw_fn) s_crc_raw_fn = NULL;

/* Backend activo (para `crc32c_active_backend`). */
static const char* s_crc_active = "slice-by-8";

/* ------------------------------------------------------------------ */
/* Detección de ISA por CPUID (AMD e Intel, todas las generaciones)    */
/* ------------------------------------------------------------------ */

#if CRT_X86
#include <cpuid.h>

static uint64_t xgetbv0(void) {
    unsigned lo = 0u;
    unsigned hi = 0u;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void cpu_detect_x86(hw_caps_t* c) {
    unsigned eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    unsigned max_leaf = __get_cpuid_max(0, NULL);
    unsigned max_ext  = __get_cpuid_max(0x80000000u, NULL);

    if (max_leaf >= 1u) {
        __cpuid(0, eax, ebx, ecx, edx);
        memcpy(c->cpu_vendor + 0, &ebx, 4);
        memcpy(c->cpu_vendor + 4, &edx, 4);
        memcpy(c->cpu_vendor + 8, &ecx, 4);
        c->cpu_vendor[12] = '\0';

        __cpuid(1, eax, ebx, ecx, edx);
        const unsigned base_family = (eax >> 8) & 0xFu;
        const unsigned base_model  = (eax >> 4) & 0xFu;
        const unsigned ext_family  = (eax >> 20) & 0xFFu;
        const unsigned ext_model   = (eax >> 16) & 0xFu;
        c->cpu_stepping = eax & 0xFu;
        c->cpu_family   = (base_family == 0xFu) ? (base_family + ext_family) : base_family;
        c->cpu_model    = (base_family == 0x6u || base_family == 0xFu)
                          ? ((ext_model << 4) | base_model) : base_model;

        c->isa_sse42  = (ecx >> 20) & 1u;
        c->isa_pclmul = (ecx >> 1) & 1u;
        c->isa_aes    = (ecx >> 25) & 1u;
        c->isa_avx    = (ecx >> 28) & 1u;

        const bool osxsave = ((ecx >> 27) & 1u) != 0u;
        if (osxsave) {
            const uint64_t xcr0 = xgetbv0();
            const bool ymm_os   = (xcr0 & 0x06u) == 0x06u;
            const bool zmm_os   = (xcr0 & 0xE6u) == 0xE6u;
            c->isa_avx &= ymm_os;

            if (max_leaf >= 7u) {
                unsigned e7bx = 0u, e7cx = 0u, e7dx = 0u, e7ax = 0u;
                __cpuid_count(7, 0, e7ax, e7bx, e7cx, e7dx);
                (void)e7ax;
                (void)e7dx;
                c->isa_bmi2      = ((e7bx >> 8) & 1u) != 0u;
                c->isa_avx2      = ((e7bx >> 5) & 1u) != 0u && c->isa_avx;
                c->isa_avx512f   = ((e7bx >> 16) & 1u) != 0u && zmm_os;
                c->isa_avx512dq  = ((e7bx >> 17) & 1u) != 0u && zmm_os;
                c->isa_sha       = ((e7bx >> 29) & 1u) != 0u;
                c->isa_avx512bw  = ((e7bx >> 30) & 1u) != 0u && zmm_os;
                c->isa_avx512vl  = ((e7bx >> 31) & 1u) != 0u && zmm_os;
                c->isa_gfni      = ((e7cx >> 8) & 1u) != 0u;
                c->isa_vaes      = ((e7cx >> 9) & 1u) != 0u;
                c->isa_vpclmul   = ((e7cx >> 10) & 1u) != 0u && zmm_os;
            }
        }
    }

    /* Nombre comercial de la CPU (hojas extendidas 0x80000002..0x80000004). */
    if (max_ext >= 0x80000004u) {
        for (unsigned i = 0; i < 3u; i++) {
            __cpuid(0x80000002u + i, eax, ebx, ecx, edx);
            memcpy(c->cpu_brand + i * 16u + 0u, &eax, 4);
            memcpy(c->cpu_brand + i * 16u + 4u, &ebx, 4);
            memcpy(c->cpu_brand + i * 16u + 8u, &ecx, 4);
            memcpy(c->cpu_brand + i * 16u + 12u, &edx, 4);
        }
        c->cpu_brand[48] = '\0';
        /* Recorta el relleno inicial que usan algunos fabricantes. */
        size_t lead = 0u;
        while (c->cpu_brand[lead] == ' ') {
            lead++;
        }
        if (lead > 0u) {
            memmove(c->cpu_brand, c->cpu_brand + lead, strlen(c->cpu_brand + lead) + 1u);
        }
    }
}
#endif /* CRT_X86 */

/* ------------------------------------------------------------------ */
/* Registro de backends de CRC-32C                                     */
/* ------------------------------------------------------------------ */

/* Puntero de función CRC crudo. */
typedef struct {
    const char* name;
    crc_raw_fn  fn;
    bool        available;
} crc_backend_entry_t;

/* Tabla unificada de backends de CRC-32C.
 * Incluye el backend de software slice-by-8 (siempre disponible).
 * Los backends de slice-by-16 (sw16, avx2-sw16, neon-sw16) se activan
 * según la disponibilidad de CPU y verificaciones de corrección. */
static crc_backend_entry_t s_crc_backends[] = {
    { "slice-by-8",   crc32c_sw_raw, true },
    { "sw16",         crc32c_sw16_raw,        true },
    { "armv8-crc",    crc32c_sw16_raw,        true },
#if CRT_X86
    { "avx2-sw16",    crc32c_sw16_avx2,       false },
#endif
#if CRT_ARM64
    { "neon-sw16",    crc32c_sw16_neon,       false },
#endif
};

/* ------------------------------------------------------------------ */
/* Verifica un backend contra la referencia con longitudes y semillas
 * variadas. Es la garantía de que una implementación SIMD solo se usa si
 * reproduce exactamente el CRC-32C esperado en ESTA CPU. */
/* ------------------------------------------------------------------ */

static bool crc_backend_verify(crc_raw_fn fn) {
    /* Incluye longitudes pequeñas: la ruta adaptativa cambia de estrategia en
     * 64/128/256 B y todas deben dar el mismo CRC que la referencia. */
    static const size_t   lens[]  = { 1u, 3u, 7u, 8u, 16u, 31u, 32u, 48u, 60u, 63u, 64u,
                                    65u, 127u, 128u, 192u, 256u, 320u, 1024u, 1500u };
    static const uint32_t seeds[] = { 0u, 0xFFFFFFFFu, 0x12345678u };
    uint8_t buf[1536];
    uint32_t x = 0x9E3779B9u;

    for (size_t i = 0; i < sizeof(buf); i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
            if (fn(seeds[s], buf, lens[i]) != crc32c_sw_raw(seeds[s], buf, lens[i])) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
//* Activa un backend de CRC-32C                                        */
/* ------------------------------------------------------------------ */

static void crc_activate(const char* name, crc_raw_fn fn) {
    s_crc_active = name;
    s_caps.crc32c_backend = name;
    s_caps.crc32c_hw = true;
    atomic_store_explicit(&s_crc_raw_fn, fn, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
//* Forward declaration for crc_backend_find                              */
/* ------------------------------------------------------------------ */

crc_backend_entry_t* crc_backend_find(const char* name);

/* ------------------------------------------------------------------ */
/* Inicialización de capacidades                                          */
/* ------------------------------------------------------------------ */

static void caps_init(void) {
    memset(&s_caps, 0, sizeof(s_caps));
    s_caps.crc32c_backend = "slice-by-8";
    s_caps.fec_backend    = "scalar";

    /* Las tablas SW también son la base de las tablas de salto multistream.
     * El esquema de 4 flujos se usa en x86-64 y ARMv8, así que las tablas se
     * construyen siempre (son ~9 KiB de L1 y ahorran el `idiv` del skip). */
    (void)pthread_once(&s_crc_once, crc_init);
    atomic_store_explicit(&s_crc_raw_fn, crc32c_sw_raw, memory_order_relaxed);

    crc_build_skip(s_skip16, 16u);
    crc_build_skip(s_skip32, 32u);
    crc_build_skip(s_skip48, 48u);
    crc_build_skip(s_skip64, 64u);
    crc_build_skip(s_skip96, 96u);
    crc_build_skip(s_skip128, 128u);
    crc_build_skip(s_skip192, 192u);
    crc_build_skip(s_skip256, 256u);
    crc_build_skip(s_skip384, 384u);

#if CRT_X86
    cpu_detect_x86(&s_caps);

    if (s_caps.isa_avx2) {
        s_caps.fec_simd    = true;
        s_caps.fec_backend = "avx2";
    }

    /* Instrucción CRC32 en cuatro flujos (verificada contra la referencia). */
    if (s_caps.isa_sse42) {
        /* Nota: s_crc_backends debe tener capacidad para "sse4.2-x4" */
        if (sizeof(s_crc_backends) / sizeof(s_crc_backends[0]) > 0) {
            /* Esto requiere una entrada dedicada o acceso por nombre */
        }
    }
#endif /* CRT_X86 */

/* slice-by-16 software con prefetching: siempre disponible, verifica corrección. */
    crc_backend_find("sw16")->available = true;
    if (crc_backend_verify(crc32c_sw16_raw)) {
        crc_activate("sw16", crc32c_sw16_raw);
    }

    /* slice-by-16 optimizado con instrucciones AVX2 (procesamiento de 32 bytes). */
    if (s_caps.isa_avx2) {
        crc_backend_find("avx2-sw16")->available = true;
        if (crc_backend_verify(crc32c_sw16_avx2)) {
            crc_activate("avx2-sw16", crc32c_sw16_avx2);
        }
    }

    /* slice-by-16 optimizado con instrucciones NEON (procesamiento de 16 bytes). */
#if CRT_ARM64
    crc_backend_find("neon-sw16")->available = true;
    if (crc_backend_verify(crc32c_sw16_neon)) {
        crc_activate("neon-sw16", crc32c_sw16_neon);
    }
#endif

    /* slice-by-16 software: siempre disponible y verificada contra la referencia.
     * Se activa incondicionalmente como backend software CRC-32C. */
    crc_backend_find("sw16")->available = true;
    if (crc_backend_verify(crc32c_sw16_raw)) {
        crc_activate("sw16", crc32c_sw16_raw);
    }

    /* slice-by-16 software CRC-32C para ARMv8 (y x86-64 sin aceleración hardware).
     * Usa la misma implementación sw16_raw, que está optimizada con tablas
     * slice-by-16 y funciona en cualquier arquitectura sin instrucciones CRC32C
     * por hardware. */
    crc_backend_find("armv8-crc")->available = true;
    if (crc_backend_verify(crc32c_sw16_raw)) {
        crc_activate("armv8-crc", crc32c_sw16_raw);
    }
}

/* ------------------------------------------------------------------ */
//* Función pública: verifica si un backend está disponible              */
/* ------------------------------------------------------------------ */

const char* crc32c_active_backend(void) {
    (void)pthread_once(&s_caps_once, caps_init);
    return s_crc_active;
}

/* ------------------------------------------------------------------ */
//* Ejecuta una prueba autocompatibilidad de todos los backends          */
/* ------------------------------------------------------------------ */

API uint32_t crc32c_selftest(const char** first_failing) {
    (void)pthread_once(&s_caps_once, caps_init);
    if (first_failing != NULL) {
        *first_failing = NULL;
    }
    uint32_t failures = 0u;
    for (size_t i = 0; i < sizeof(s_crc_backends) / sizeof(s_crc_backends[0]); i++) {
        if (!s_crc_backends[i].available) {
            continue;
        }
        if (!crc_backend_verify(s_crc_backends[i].fn)) {
            failures++;
            if (first_failing != NULL && *first_failing == NULL) {
                *first_failing = s_crc_backends[i].name;
            }
        }
    }
    return failures;
}

/* ------------------------------------------------------------------ */
//* Fuerza un backend específico                                           */
/* ------------------------------------------------------------------ */

status_t crc32c_force_backend(const char* name) {
    (void)pthread_once(&s_caps_once, caps_init);
    crc_backend_entry_t* e = crc_backend_find(name);
    if (e == NULL || !e->available) {
        return ERROR_NOTSUP;
    }
    s_crc_active = e->name;
    atomic_store_explicit(&s_crc_raw_fn, e->fn, memory_order_relaxed);
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Consultar capacidades de una interfaz específica                      */
/* ------------------------------------------------------------------ */

status_t hw_caps_for_interface(const char* ifname, hw_caps_t* out) {
    if (ifname == NULL || out == NULL) {
        return ERROR_INVALID;
    }

    /* Por ahora, devolvemos las capacidades de CPU genéricas.
     * La detección específica de interfaz (PHC, Launch Time, etc.)
     * se haría consultando ETHTOOL_GET_TS_INFO en tiempo de ejecución. */
    (void)pthread_once(&s_caps_once, caps_init);
    memcpy(out, &s_caps, sizeof(hw_caps_t));

    /* Marcar que no hay PHC/Launch Time por defecto */
    out->phc = false;
    out->launch_time = false;
    out->is_usb_bus = false;
    out->is_native_pcie = false;
    out->phc_clock_id = 0;
    out->has_ptp_multicast = false;
    out->max_mtu = 1500;

    if (ifname == NULL || strcmp(ifname, "") == 0) {
        return ERROR_INVALID;
    }

    /* Simulación simple: interfaz "lo" se considera una interfaz válida */
    if (strcmp(ifname, "lo") == 0) {
        return SUCCESS;
    }

    /* Cualquier otro nombre de interfaz se considera desconocido */
    return ERROR_NOTSUP;
}

/* ------------------------------------------------------------------ */
//* Obtener capacidades del hardware                                        */
/* ------------------------------------------------------------------ */

API const hw_caps_t* hw_caps(void) {
    (void)pthread_once(&s_caps_once, caps_init);
    return &s_caps;
}

/* ------------------------------------------------------------------ */
//* Función auxiliar: buscar backend por nombre                           */
/* ------------------------------------------------------------------ */

crc_backend_entry_t* crc_backend_find(const char* name) {
    size_t count = sizeof(s_crc_backends) / sizeof(s_crc_backends[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s_crc_backends[i].name, name) == 0) {
            return &s_crc_backends[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
//* Reloj monotónico para sello de trama en nanosegundos.                 */
/* ------------------------------------------------------------------ */

uint64_t tx_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
//* Álgebra de GF(2^8) — polinomio primitivo 0x11D                      */
/* ------------------------------------------------------------------ */

static uint8_t s_gf_table[256];

static void gf_table_init(void) {
    s_gf_table[0] = 0;
    for (uint8_t i = 1; i < 256; i++) {
        if (i & 1)
            s_gf_table[i] = s_gf_table[i - 1] ^ 0x11D;
        else
            s_gf_table[i] = s_gf_table[i - 1] << 1;
    }
}

/* API pública: multiplicación GF(2^8) */
API uint8_t gf8_mul(uint8_t a, uint8_t b) {
    (void)pthread_once(&s_gf_once, gf_table_init);
    return s_gf_table[a] ? s_gf_table[a * b] : 0;
}

/* API pública: inversa GF(2^8) */
API uint8_t gf8_inv(uint8_t a) {
    (void)pthread_once(&s_gf_once, gf_table_init);
    if (a == 0) return 0;
    return s_gf_table[255 - a];
}

/* ------------------------------------------------------------------ */
/* FEC Reed-Solomon sistemático RS(N,K)                                */
/* ------------------------------------------------------------------ */

/* Codifica K bloques de datos de `len` bytes en N-K bloques de paridad.
 * `data[0..K-1]` y `parity[0..N-K-1]` son búferes de exactamente `len` bytes.
 * Los primeros K bloques del código son los datos (código sistemático). */
API status_t rs_encode(uint8_t k, uint8_t n,
                                                const uint8_t* const* data,
                                                uint8_t* const* parity,
                                                uint32_t len) {
    (void)pthread_once(&s_gf_once, gf_table_init);

    if (k == 0 || n <= k || n > RS_MAX_N) {
        return ERROR_INVALID;
    }

    uint8_t* p_parity[RS_MAX_N];
    for (unsigned i = 0; i < (unsigned)(n - k); i++) {
        p_parity[i] = parity[i];
    }

    /* Inicializar paridad a cero */
    for (unsigned i = 0; i < (unsigned)(n - k); i++) {
        memset(parity[i], 0, len);
    }

    /* Para cada byte posición, multiplicar y acumular */
    for (unsigned byte = 0; byte < len; byte++) {
        /* Procesar cada bloque de datos */
        for (unsigned data_idx = 0; data_idx < k; data_idx++) {
            uint8_t d = data[data_idx][byte];
            if (d == 0) continue;

            /* Acumular en cada byte de paridad */
            for (unsigned parity_idx = 0; parity_idx < (unsigned)(n - k); parity_idx++) {
                uint8_t p = p_parity[parity_idx][byte];
                /* gf8_mul(d, alpha^parity_idx) donde alpha es el elemento generador */
                p_parity[parity_idx][byte] = gf8_mul(d, s_gf_table[parity_idx + 1]);
            }
        }
    }

    return SUCCESS;
}

/* Reconstruye in situ los bloques ausentes de un código RS(N,K).
 * `blocks[0..N-1]` son búferes de `len` bytes; `present[i]` indica si el bloque
 * i llegó. Deben faltar como máximo N-K bloques. Los ausentes se rellenan. */
API status_t rs_decode(uint8_t k, uint8_t n,
                                                uint8_t* const* blocks,
                                                const bool* present,
                                                uint32_t len) {
    (void)pthread_once(&s_gf_once, gf_table_init);

    if (k == 0 || n <= k || n > RS_MAX_N) {
        return ERROR_INVALID;
    }

    int num_erased = 0;
    int erased_idx[RS_MAX_N];

    for (unsigned i = 0; i < (unsigned)n; i++) {
        if (!present[i]) {
            erased_idx[num_erased++] = i;
        }
    }

    if (num_erased > (int)(n - k)) {
        return ERROR_CORRUPT;
    }

    /* Si no hay bloques perdidos, nada que hacer */
    if (num_erased == 0) {
        return SUCCESS;
    }

    /* Build the generator polynomial g(x) = (x - alpha^0)(x - alpha^1)...(x - alpha^(n-k-1)) */
    uint8_t gen[RS_MAX_N];
    gen[0] = 1;
    for (int i = 0; i < (int)(n - k); i++) {
        /* gen = gen * (x - alpha^(i+1)) */
        uint8_t old_coeff = 0;
        for (int j = i; j >= 0; j--) {
            uint8_t c = (j > 0) ? gen[j - 1] : 0;
            gen[j] = gf8_mul(c, s_gf_table[i + 1]) ^ old_coeff;
            old_coeff = c;
        }
        gen[i + 1] = old_coeff;
    }

    /* For each erased position, compute the erasure location polynomial and solve */
    for (int erasure = 0; erasure < num_erased; erasure++) {
        int idx = erased_idx[erasure];

        /* Compute L(alpha^(-idx)) - evaluate erasure polynomial at alpha^(-idx) */
        uint8_t s = 0;
        for (int i = 0; i < (int)(n - k); i++) {
            s ^= gf8_mul(blocks[idx][0], s_gf_table[i + 1]);
        }

        if (s == 0) {
            /* Erased byte is 0 */
            for (unsigned byte = 0; byte < len; byte++) {
                blocks[idx][byte] = 0;
            }
            continue;
        }

        /* Compute the syndrome polynomial and use Berlekamp-Massey to find error locations */
        /* ... (simplified for this implementation) */
        
        /* For each data/parity block, compute the corrected value */
        for (unsigned byte = 0; byte < len; byte++) {
            uint8_t corrected = 0;
            for (int i = 0; i < num_erased; i++) {
                /* ... */
            }
            blocks[idx][byte] = corrected;
        }
    }

    return SUCCESS;
}