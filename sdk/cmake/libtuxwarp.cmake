# ============================================================================
# libtuxwarp.cmake — Módulo de find_package para libtuxwarp
# ============================================================================
# Proporciona el paquete CMake `libtuxwarp` para que otros proyectos lo
# puedan descubrir y enlazar contra la biblioteca compartida.
#
# Uso en el proyecto consumidor:
#   cmake_minimum_required(VERSION 3.16)
#   project(MyApp C)
#   find_package(libtuxwarp REQUIRED)
#   target_link_libraries(MyApp PRIVATE libtuxwarp)
#
# La instalación se realizó con:
#   cmake --install . --prefix /usr/local
# ============================================================================

include(GNUInstallDirs)

set(_libtuxwarp_PREFIX "${CMAKE_INSTALL_PREFIX}" CACHE PATH "Directorio de instalación")

# Ubicaciones por defecto (matching la instalación del CMakeLists.txt principal)
list(APPEND _libtuxwarp_SEARCH_DIRS
    "${_libtuxwarp_PREFIX}/include/libtuxwarp"
    "${_libtuxwarp_PREFIX}/lib"
    "${_libtuxwarp_PREFIX}/share/libtuxwarp/cmake"
)

# Buscar la biblioteca compartida
find_library(_libtuxwarp_LIB
    NAMES libtuxwarp
    PATHS ${_libtuxwarp_SEARCH_DIRS}
    NO_DEFAULT_PATH
)

# Buscar los headers públicos
find_file(_libtuxwarp_HEADER
    NAMES libtuxwarp.h
    PATHS ${_libtuxwarp_SEARCH_DIRS}
    NO_DEFAULT_PATH
)

# Buscar el módulo TuxRTProvider
find_file(_libtuxwarp_PROVIDER
    NAMES TuxRTProvider.cmake
    PATHS "${_libtuxwarp_PREFIX}/share/libtuxwarp/cmake"
    NO_DEFAULT_PATH
)

# Determinar si encontramos todo lo necesario
set(_libtuxwarp_FOUND TRUE)
if(NOT _libtuxwarp_LIB)
    set(_libtuxwarp_FOUND FALSE)
    message(STATUS "libtuxwarp: biblioteca compartida no encontrada")
endif()

if(NOT _libtuxwarp_HEADER)
    set(_libtuxwarp_FOUND FALSE)
    message(STATUS "libtuxwarp: headers públicos no encontrados")
endif()

if(NOT _libtuxwarp_PROVIDER)
    set(_libtuxwarp_FOUND FALSE)
    message(STATUS "libtuxwarp: TuxRTProvider.cmake no encontrado")
endif()

# Exportar variables de CMake
mark_as_advanced(_libtuxwarp_LIB _libtuxwarp_HEADER _libtuxwarp_PROVIDER)

if(_libtuxwarp_FOUND)
    include(UseLibDirStandards)

    # Biblioteca compartida
    set(LIBTUXWARP_LIBRARY ${_libtuxwarp_LIB} CACHE FILE "Ruta a libtuxwarp.so")

    # Directorio de includes
    set(LIBTUXWARP_INCLUDE_DIR "${_libtuxwarp_PREFIX}/include/libtuxwarp" CACHE PATH "Directorio de headers")

    # Marcar como encontrados
    set(LIBTUXWARP_FOUND TRUE)
else()
    set(LIBTUXWARP_FOUND FALSE)
endif()

# Proporcionar el paquete
if(_libtuxwarp_FOUND)
    set(PACKAGE_LIBTUXWARP_FOUND TRUE)
else()
    set(PACKAGE_LIBTUXWARP_FOUND FALSE)
endif()

# Definir interface para link_libraries / target_link_libraries
if(LIBTUXWARP_FOUND)
    add_library(libtuxwarp INTERFACE)
    target_include_directories(libtuxwarp INTERFACE ${LIBTUXWARP_INCLUDE_DIR})
    target_link_libraries(libtuxwarp INTERFACE ${LIBTUXWARP_LIBRARY})
    set_property(INTERNAL TARGET libtuxwarp PROPERTY ALLOWDUPLICATEOFFSET TRUE)
endif()

mark_as_advanced(LIBTUXWARP_FOUND LIBTUXWARP_LIBRARY LIBTUXWARP_INCLUDE_DIR)