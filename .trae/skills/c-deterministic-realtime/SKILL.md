---
name: c-deterministic-realtime
description: Impone programación en C de tiempo real determinista, seguridad de memoria sin asignación dinámica, concurrencia lock-free y optimización extrema a nivel de hardware. Usar al escribir, refactorizar o auditar módulos en C de bajo nivel, tareas RTOS, drivers o bucles de alta frecuencia.
---

# Skill de C Determinista, Resiliente y de Ultra Alto Rendimiento

## 1. Memoria y Determinismo (Cero Asignación Dinámica)
- **Prohibición de Memoria Dinámica**: Queda estrictamente prohibido usar `malloc`, `calloc`, `realloc` y `free` después de la fase de inicialización del sistema. Utiliza pools de memoria estáticos, *arena allocators* o *ring buffers* preasignados.
- **Ejecución Acotada**: Todo bucle debe tener un límite superior demostrable estáticamente. Evita bucles `while` infinitos (salvo el bucle principal supervisado) y prohíbe la recursión.
- **Ramificación Predecible**: Minimiza los fallos de predicción de saltos (*branch mispredictions*) en rutas críticas usando aritmética sin saltos (*branchless*), tablas de búsqueda (*LUTs*) o `__builtin_expect`.

## 2. Resiliencia Absoluta y Seguridad (Estándar MISRA / Power of 10)
- **Tipado Estricto**: Usa exclusivamente enteros de ancho fijo de `<stdint.h>` (`uint32_t`, `int64_t`) y `size_t` para tamaños. Nunca dependas de tipos variables como `int` o `long`.
- **Seguridad de Punteros y Búferes**: Valida todos los punteros y límites de arreglos en las fronteras de cada módulo. Usa los calificadores `const` y `restrict` de forma agresiva para garantizar ausencia de *aliasing*.
- **Manejo de Errores**: Verifica absolutamente todos los valores de retorno. Evita `setjmp`/`longjmp` y manejadores de señales dinámicos. Ante un fallo crítico, transiciona de inmediato a un estado seguro y determinista.

## 3. Ultra Alto Rendimiento y Alineación con el Hardware
- **Localidad de Caché**: Diseña las estructuras de datos usando *Structure of Arrays* (SoA) en lugar de *Array of Structures* (AoS) para bucles críticos. Alinea las estructuras clave a líneas de caché (`alignas(64)`).
- **Concurrencia Lock-Free**: Evita el uso de *mutex* en hilos de tiempo real para prevenir la inversión de prioridades. Utiliza operaciones atómicas de C11 (`<stdatomic.h>`) con ordenamiento de memoria explícito (`memory_order_acquire`/`release`) y colas circulares SPSC/MPMC.
- **SIMD y Pistas al Compilador**: Escribe bucles autovectorizables y aplica `static inline` en funciones pequeñas dentro de rutas críticas.

## 4. Flujo de Verificación Obligatorio
Antes de dar por finalizada cualquier generación o refactorización de código en C:
1. Audita el código en busca de asignaciones ocultas, promociones implícitas de tipos o comportamiento indefinido (*Undefined Behavior*).
2. Sugiere siempre las banderas de compilación estrictas: `-O3 -march=native -Wall -Wextra -Werror -Wpedantic -Wconversion -fsanitize=undefined,address` (para pruebas en host).
3. Evalúa y documenta el impacto en el tiempo de ejecución en el peor caso (*WCET*) de las secciones críticas.
