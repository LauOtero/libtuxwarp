cmake_minimum_required(VERSION 3.16)
project(libtuxwarp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_TESTS     "Build test suite" ON)
option(BUILD_BENCHMARK "Build benchmark executable" ON)
option(BUILD_TOOLS     "Install helper tools" ON)
option(BUILD_PROTOCOLS "Build the network protocols (EtherCAT + EtherTux)" ON)
option(USE_SIMD        "Enable SIMD optimizations" ON)
option(USE_HUGEPAGES   "Enable hugepages support" ON)
option(ENABLE_LTO      "Enable link-time optimization (PF-6)" ON)
option(ENABLE_NATIVE   "Build with -march=native (PF-6, no portátil)" OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(DEBUG)
endif()

# glibc extensions (CPU_ZERO/CPU_SET, sched_setcpu, thread affinity, memfd)

find_package(Threads REQUIRED)

# --- SIMD detection ---------------------------------------------------------
# Las pruebas AVX-512/AVX2 se compilan *y ejecutan* para detectar correctamente
# la capacidad del host (p. ej. Zen2 tiene AVX2 sin AVX-512) y evitar SIGILL.
include(CheckCSourceCompiles)
include(CheckCSourceRuns)

if(NOT CMAKE_CROSSCOMPILING)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")

        set(CMAKE_REQUIRED_FLAGS "-mavx512f")
        check_c_source_runs("
            #include <immintrin.h>
            int main(void) {
                __m512i a = _mm512_set1_epi32(7);
                __m512i b = _mm512_add_epi32(a, a);
                int buf[16];
                _mm512_storeu_si512((void*)buf, b);
                return buf[0] == 14 ? 0 : 1;
            }
        " HAVE_AVX512)
        set(CMAKE_REQUIRED_FLAGS)

        set(CMAKE_REQUIRED_FLAGS "-mavx2")
        check_c_source_runs("
            #include <immintrin.h>
            int main(void) {
                __m256i a = _mm256_set1_epi32(7);
                __m256i b = _mm256_add_epi32(a, a);
                int buf[8];
                _mm256_storeu_si256((__m256i*)buf, b);
                return buf[0] == 14 ? 0 : 1;
            }
        " HAVE_AVX2)
        set(CMAKE_REQUIRED_FLAGS)
    endif()
endif()

check_c_source_compiles("
    #if defined(__aarch64__)
    #include <arm_neon.h>
    int main(void) { uint64x2_t x = vdupq_n_u64(0); (void)x; return 0; }
    #else
    #error not aarch64
    #endif
" HAVE_NEON)

# SVE (PF-4): vectorización escalable en ARM64 (Jetson Orin y posteriores).
check_c_source_compiles("
    #if defined(__aarch64__) && defined(__ARM_FEATURE_SVE)
    #include <arm_sve.h>
    int main(void) { return (int)svcntb(); }
    #else
    #error no sve
    #endif
" HAVE_SVE)

# --- Fuentes y cabeceras ----------------------------------------------------
set(PUBLIC_HEADERS
    include/libtuxwarp/libtuxwarp.h
    include/libtuxwarp/abi.h
    include/libtuxwarp/ring.h
    include/libtuxwarp/mpmc.h
    include/libtuxwarp/bounded.h
    include/libtuxwarp/simd.h
    include/libtuxwarp/mem.h
    include/libtuxwarp/kernel.h
    include/libtuxwarp/tuxwarp_sched.h
    include/libtuxwarp/rt.h
    include/libtuxwarp/resilience.h
    include/libtuxwarp/obs.h
    include/libtuxwarp/tuxrt.h
    include/libtuxwarp/ethercat.h
    include/libtuxwarp/ethertux.h
    include/libtuxwarp/hwaccel.h
    include/libtuxwarp/protocol.h
    include/libtuxwarp/transport.h
)

set(SOURCES
    # Runtime core: eventos, memoria, planificación RT, resiliencia, observabilidad
    src/core/core.c
    src/core/mem.c
    src/core/sched.c
    src/core/rt.c
    src/core/resilience.c
    src/core/obs.c
    # Plataforma: detección y validación del kernel
    src/platform/kernel.c
    src/platform/net_probe.c
    # HAL de aceleración por hardware (CRC-32C, GF(2^8), Reed-Solomon)
    src/hwaccel/hwaccel.c
    # Adaptador de transporte (USB, UART, CAN, Ethernet, SPI, I2C, EtherCAT, EtherTux)
    src/transport/transport.c
    src/transport/transport_ethercat.c
    src/transport/transport_ethertux.c
    # Selección de protocolo de red (EtherCAT / EtherTux)
    src/protocols/protocol.c
    # SIMD genérico, puente con libtuxrt_core y benchmark
    src/simd/simd.c
    src/integration/bridge.c
    src/benchmark/benchmark.c
)

if(BUILD_PROTOCOLS)
    list(APPEND SOURCES
        # Maestro EtherCAT (hardware dedicado)
        src/protocols/ethercat/ec_frame.c
        src/protocols/ethercat/ec_socket.c
        src/protocols/ethercat/ec_master.c
        src/protocols/ethercat/ec_slave.c
        src/protocols/ethercat/ec_dc.c
        src/protocols/ethercat/ec_mailbox.c
        # EtherTux (Ethernet comercial determinista)
        src/protocols/ethertux/et_frame.c
        src/protocols/ethertux/et_fec.c
        src/protocols/ethertux/et_qos.c
        src/protocols/ethertux/et_sched.c
        src/protocols/ethertux/et_esm.c
        src/protocols/ethertux/et_sync.c
        src/protocols/ethertux/et_topology.c
        src/protocols/ethertux/et_mbx.c
        src/protocols/ethertux/et_flowagg.c
        src/protocols/ethertux/et_frm.c
        src/protocols/ethertux/et_aalid.c
        src/protocols/ethertux/et_socket.c
        src/protocols/ethertux/et_master.c
        # Modo esclave EtherTux
        src/protocols/ethertux/et_slave.c
    )
endif()

# --- Biblioteca compartida (ABI C estable, coexistencia con libtuxrt_core) ---
add_library(libtuxwarp SHARED ${SOURCES} ${PUBLIC_HEADERS})

set_target_properties(libtuxwarp PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PREFIX    ""
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_compile_definitions(libtuxwarp PRIVATE BUILDING _GNU_SOURCE)

target_include_directories(libtuxwarp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/libtuxwarp>
    $<INSTALL_INTERFACE:include/libtuxwarp>
)

# --- Enlace universal de proveedores (TuxRT Provider ABI) -------------------
# libtuxwarp se publica como PROVEEDOR del ecosistema exportando el símbolo
# descriptor `tuxrt_provider_descriptor`. La dependencia es UNIDIRECCIONAL
# (consumidor -> proveedor): libtuxrt_core lo descubre con dlopen. libtuxwarp NO
# enlaza ni conoce a libtuxrt_core.
#
# `TuxRTProvider.cmake` es un módulo reutilizable por cualquier proyecto; se
# localiza por `TUXRT_PROVIDER_MODULE_DIR` o, por defecto, en el proyecto
# hermano TuxedoRT-Mojo.
set(TUXRT_PROVIDER_MODULE_DIR "" CACHE PATH
    "Directorio con TuxRTProvider.cmake (contrato de proveedor TuxRT)")

# El módulo vive en el proyecto anfitrión. Se resuelve partiendo del directorio
# REAL del proyecto (sin '..' ni enlaces) para que include() funcione también en
# montajes anidados, donde '..' no se resuelve de forma consistente.
set(_tuxrt_module "")
if(TUXRT_PROVIDER_MODULE_DIR)
    if(EXISTS "${TUXRT_PROVIDER_MODULE_DIR}/TuxRTProvider.cmake")
        set(_tuxrt_module "${TUXRT_PROVIDER_MODULE_DIR}/TuxRTProvider.cmake")
    endif()
endif()

if(NOT _tuxrt_module)
    get_filename_component(_src_real "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    get_filename_component(_src_parent "${_src_real}" DIRECTORY)
    file(GLOB _tuxrt_candidates
         "${_src_parent}/TuxedoRT-Mojo/cmake/TuxRTProvider.cmake"
         "${_src_parent}/cmake/TuxRTProvider.cmake"
         "${_src_parent}/*/cmake/TuxRTProvider.cmake")
    if(_tuxrt_candidates)
        list(GET _tuxrt_candidates 0 _tuxrt_module)
    endif()
endif()

if(_tuxrt_module)
    get_filename_component(_tuxrt_dir "${_tuxrt_module}" DIRECTORY)
    set(TUXRT_PROVIDER_MODULE_DIR "${_tuxrt_dir}" CACHE PATH
        "Directorio con TuxRTProvider.cmake (contrato de proveedor TuxRT)" FORCE)
    include("${_tuxrt_module}")
endif()

if(COMMAND tuxrt_provide)
    tuxrt_provide(libtuxwarp
        NAME         "libtuxwarp"
        VERSION      "${PROJECT_VERSION}"
        CAPABILITIES "ethercat,ethertux,rt-scheduler,recovery,observability,simd,lockfree")
else()
    message(STATUS "libtuxwarp: TuxRTProvider.cmake no encontrado; descriptor de "
                   "proveedor omitido (defina -DTUXRT_PROVIDER_MODULE_DIR=<dir>)")
endif()

if(NOT CMAKE_CROSSCOMPILING)
    if(HAVE_SVE)
        target_compile_definitions(libtuxwarp PRIVATE SIMD_SVE)
    endif()
endif()

if(USE_SIMD)
    target_compile_definitions(libtuxwarp PRIVATE SIMD_ENABLED)

    if(HAVE_AVX512)
        target_compile_options(libtuxwarp PRIVATE -mavx512f -mavx512dq -mavx512bw)
        target_compile_definitions(libtuxwarp PRIVATE SIMD_AVX512)
    elseif(HAVE_AVX2)
        target_compile_options(libtuxwarp PRIVATE -mavx2)
        target_compile_definitions(libtuxwarp PRIVATE SIMD_AVX2)
    elseif(HAVE_NEON)
        target_compile_definitions(libtuxwarp PRIVATE SIMD_NEON)
    endif()
endif()

if(USE_HUGEPAGES)
    target_compile_definitions(libtuxwarp PRIVATE HUGEPAGES_ENABLED)
endif()

if(ENABLE_NATIVE)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-march=native" HAVE_MARCH_NATIVE)
    if(HAVE_MARCH_NATIVE)
        target_compile_options(libtuxwarp PRIVATE -march=native)
    endif()
endif()

if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_OK OUTPUT IPO_MSG)
    if(IPO_OK)
        set_property(TARGET libtuxwarp PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "libtuxwarp: LTO no disponible (${IPO_MSG})")
    endif()
endif()

target_link_libraries(libtuxwarp PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})

# --- Benchmark executable ---------------------------------------------------
if(BUILD_BENCHMARK)
    add_executable(bench tools/benchmark.c)
    target_link_libraries(bench PRIVATE libtuxwarp)
    if(ENABLE_LTO AND IPO_OK)
        set_property(TARGET bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# --- Test suite -------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()

    set(TEST_SOURCES
        tests/test_core.c
        tests/test_lockfree.c
        tests/test_rt.c
        tests/test_resilience.c
        tests/test_integration.c
        tests/test_network.c
        tests/test_hwaccel.c
        tests/test_ethertux.c
        tests/test_ethertux_v010.c
        tests/test_aalid.c
        tests/test_multi_nic.c
    )

    set(TEST_NAMES
        test_core test_lockfree test_rt test_resilience test_integration
        test_network test_hwaccel test_ethertux test_ethertux_v010
        test_aalid test_multi_nic
    )

    list(LENGTH TEST_SOURCES _n)
    set(_i 0)
    foreach(_src ${TEST_SOURCES})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
            list(GET TEST_NAMES ${_i} _tname)
            add_executable(${_tname} ${_src})
            target_link_libraries(${_tname} PRIVATE libtuxwarp)
            add_test(NAME ${_tname} COMMAND ${_tname})
        endif()
        math(EXPR _i "${_i} + 1")
    endforeach()
endif()

# --- Instalación ------------------------------------------------------------
install(TARGETS libtuxwarp
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${PUBLIC_HEADERS}
    DESTINATION include/libtuxwarp
)

install(DIRECTORY cmake/
    DESTINATION share/libtuxwarp/cmake
)

if(BUILD_TOOLS)
    install(DIRECTORY scripts/
        DESTINATION share/libtuxwarp/scripts
        USE_SOURCE_PERMISSIONS
    )
endif()
