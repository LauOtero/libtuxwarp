# =============================================================================
# TuxRTProvider.cmake — Enlace universal de proveedores (TuxRT Provider ABI)
# =============================================================================
# Módulo reutilizable para PUBLICAR y CONSUMIR proveedores del ecosistema TuxRT.
# El contrato C vive en `tuxrt_provider.h` (símbolo `tuxrt_provider_descriptor`).
#
# PROVEEDOR (ChronosRT o cualquier otro proyecto):
#     include(TuxRTProvider.cmake)
#     tuxrt_provide(<target>
#         NAME         <identidad>     # p. ej. "ChronosRT"
#         VERSION      <versión>       # p. ej. "0.1.0"
#         [CAPABILITIES <csv>]         # p. ej. "ethercat,rt-scheduler,simd")
#
# CONSUMIDOR (libtuxrt_core u otro):
#     include(TuxRTProvider.cmake)
#     tuxrt_require_provider(<Name> SONAME <soname> [VERSION <v>])
#
# El enlace real es DINÁMICO (dlopen en tuxrt_provider_open/query): la
# dependencia es UNIDIRECCIONAL (consumidor -> proveedor) y su ausencia nunca
# rompe el proceso. Añadir un proveedor nuevo sólo exige exportar el símbolo.
# =============================================================================

# Directorio que contiene `tuxrt_provider.h` (contrato de proveedor).
set(TUXRT_PROVIDER_INCLUDE_DIR "" CACHE PATH
    "Directorio con tuxrt_provider.h (contrato de proveedor TuxRT)")
if(NOT TUXRT_PROVIDER_INCLUDE_DIR)
    get_filename_component(_tuxrt_provider_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(TUXRT_PROVIDER_INCLUDE_DIR "${_tuxrt_provider_root}/sdk/include")
endif()

# -----------------------------------------------------------------------------
# tuxrt_provide(<target> NAME <n> VERSION <v> [CAPABILITIES <csv>])
#
# Genera y compila en <target> el descriptor del proveedor, exportándolo con
# visibilidad por defecto (compatible con -fvisibility=hidden).
# -----------------------------------------------------------------------------
function(tuxrt_provide target)
    cmake_parse_arguments(ARG "" "NAME;VERSION;CAPABILITIES" "" ${ARGN})

    if(NOT TARGET ${target})
        message(FATAL_ERROR "tuxrt_provide(): el target '${target}' no existe")
    endif()
    if(NOT ARG_NAME OR NOT ARG_VERSION)
        message(FATAL_ERROR "tuxrt_provide(${target}): NAME y VERSION son obligatorios")
    endif()
    if(NOT ARG_CAPABILITIES)
        set(ARG_CAPABILITIES "")
    endif()
    if(NOT EXISTS "${TUXRT_PROVIDER_INCLUDE_DIR}/tuxrt_provider.h")
        message(FATAL_ERROR
            "tuxrt_provide(): no se encontró tuxrt_provider.h en "
            "'${TUXRT_PROVIDER_INCLUDE_DIR}'. Defina -DTUXRT_PROVIDER_INCLUDE_DIR=<dir>.")
    endif()

    set(_tuxrt_desc "${CMAKE_CURRENT_BINARY_DIR}/tuxrt_provider_descriptor_${target}.c")
    file(WRITE "${_tuxrt_desc}"
"/* Generado por TuxRTProvider.cmake — NO EDITAR.
 * Exporta el descriptor del contrato TuxRT Provider ABI. */
#include <tuxrt_provider.h>

TUXRT_PROVIDER_EXPORT(\"${ARG_NAME}\", \"${ARG_VERSION}\", \"${ARG_CAPABILITIES}\");
")

    target_sources(${target} PRIVATE "${_tuxrt_desc}")
    target_include_directories(${target} PRIVATE "${TUXRT_PROVIDER_INCLUDE_DIR}")

    message(STATUS "TuxRTProvider: '${target}' publica el proveedor "
                   "${ARG_NAME} ${ARG_VERSION} (capacidades: '${ARG_CAPABILITIES}')")
endfunction()

# -----------------------------------------------------------------------------
# tuxrt_require_provider(<Name> SONAME <soname> [VERSION <v>])
#
# Registra y comprueba en configure-time un proveedor requerido. La ausencia
# sólo se avisa (el consumidor lo resuelve con dlopen y degrada con elegancia).
# -----------------------------------------------------------------------------
function(tuxrt_require_provider name)
    cmake_parse_arguments(ARG "" "SONAME;VERSION" "" ${ARGN})

    if(NOT ARG_SONAME)
        string(TOLOWER "${name}" _tuxrt_lower)
        set(ARG_SONAME "lib${_tuxrt_lower}.so")
    endif()

    find_library(_tuxrt_prov_lib_${name} NAMES "${ARG_SONAME}")

    if(_tuxrt_prov_lib_${name})
        message(STATUS "TuxRTProvider: dependencia ${name} (${ARG_SONAME}) "
                       "encontrada en ${_tuxrt_prov_lib_${name}}")
    else()
        message(STATUS "TuxRTProvider: dependencia ${name} (${ARG_SONAME}) no "
                       "encontrada; el consumidor la resolverá con dlopen en "
                       "tiempo de ejecución y la reportará sin romper el proceso")
    endif()
endfunction()
