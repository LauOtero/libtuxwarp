/*
 * abi.h — Contrato ABI estable de libtuxwarp (C11)
 *
 * Regla: una vez publicado, NUNCA romper el ABI. Solo añadir al final.
 * Cambios incompatibles -> nueva versión mayor + cabecera versionada.
 *
 * Convenciones de símbolos (coexistencia con libtuxrt_core):
 *   - libtuxwarp exporta únicamente símbolos con prefijo ``
 *     (más las funciones `*` en minúscula y `*` de módulos).
 *   - libtuxrt_core exporta símbolos con prefijo `tuxrt_`.
 *   - Ambos espacios de nombres son disjuntos => pueden convivir en el mismo
 *     proceso y enlazarse simultáneamente sin colisión.
 */
#ifndef ABI_H
#define ABI_H

#define VERSION_MAJOR 0
#define VERSION_MINOR 1
#define VERSION_PATCH 0
#define VERSION_STRING "0.1.0"

/* ABI numérico (major<<16 | minor<<8 | patch) para verificación en runtime. */
#define ABI_VERSION 0x000100UL

/* ------------------------------------------------------------------------- */
/* Macro de visibilidad: con -fvisibility=hidden solo se exporta lo marcado.  */
/* ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(BUILDING)
    #define API __declspec(dllexport)
  #else
    #define API __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define API __attribute__((visibility("default")))
  #else
    #define API
  #endif
#endif

/* Alineación de línea de caché para estructuras de camino caliente. */
#define CACHELINE 64

#endif /* ABI_H */
