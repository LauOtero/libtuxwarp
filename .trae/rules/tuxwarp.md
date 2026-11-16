# Reglas del Proyecto: Ingeniería de Sistemas en Tiempo Real

## Idioma y Comunicación
- **Idioma Obligatorio**: Responde SIEMPRE en español en todas tus explicaciones, análisis de código, diagnósticos, advertencias y comentarios dentro del código fuente.

## Activación Obligatoria de la Skill
- Siempre que vayas a crear, editar, depurar o revisar cualquier archivo `.c` o `.h`, o cuando se discuta arquitectura en C, diseño de memoria, concurrencia o rendimiento, DEBES cargar de inmediato y aplicar estrictamente la skill `c-deterministic-realtime` ubicada en `.trae/skills/c-realtime-engine/SKILL.md`.
- No generes ni una sola línea de código en C sin haber aplicado antes las directrices definidas en `c-deterministic-realtime`.

## Restricciones Innegociables
- Nunca introduzcas asignación dinámica de memoria (`malloc`, `free`, etc.) en rutas de ejecución (*runtime*).
- Nunca dejes advertencias del compilador sin resolver; todo el código en C generado debe compilar limpio bajo `-Wall -Wextra -Werror -Wpedantic`.
- Indica siempre de forma explícita si un cambio propuesto afecta al determinismo, a la alineación de caché o al tiempo de ejecución en el peor caso (*WCET*).
