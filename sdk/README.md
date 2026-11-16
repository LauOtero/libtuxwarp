# libtuxwarp SDK

Biblioteca compartida TuxedoRT Mojo - libtuxwarp para procesamiento en tiempo real y aceleración CRC-32C.

## Versión
0.1.0

## Instalación

### Prerrequisitos
- Sistema Linux (x86_64 o aarch64/ARM64)
- Node.js >= 14.0.0 (para enlaces TypeScript/JavaScript)
- CMake >= 3.16 (si se recompila desde fuente)

### Instalación desde npm (TypeScript/JavaScript)
```bash
npm install libtuxwarp
```

### Uso en TypeScript/JavaScript
```typescript
import {
  tuxwarp_init,
  tuxwarp_shutdown,
  tuxwarp_crc32c,
  tuxwarp_has_simd,
  VERSION_STRING
} from 'libtuxwarp';

// Inicializar la biblioteca
const initResult = tuxwarp_init({
  cpu_affinity: { cpu_id: 0, cpu_set_size: 16 },
  simd_level: 'AVX2',
  crc32c_config: { use_hw_accel: true, slice_size: 'slice-by-16', hugepages_enabled: true }
});

if (initResult !== TuxWarpError.SUCCESS) {
  console.error('Error initializing libtuxwarp');
  process.exit(1);
}

// Verificar capacidades SIMD
const simdCaps = tuxwarp_get_simd_capabilities();
console.log(`SIMD level: ${simdCaps.level}`);
console.log(`AVX2 supported: ${simdCaps.avx2_supported}`);

// Calcular CRC-32C
const data = new Uint8Array([0x01, 0x02, 0x03, 0x04]);
const crc = tuxwarp_crc32c(data);
console.log(`CRC-32C: ${crc}`);

// Obtener versión
console.log(`libtuxwarp version: ${VERSION_STRING}`);

// Apagar cuando termine
tuxwarp_shutdown();
```

### Compilación desde fuente (C)
```bash
cd libtuxwarp
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=OFF -DBUILD_BENCHMARK=OFF -DBUILD_TOOLS=OFF
make -j$(nproc)
cmake --install . --prefix /usr/local
```

### Verificación de instalación
```bash
node -e "const { tuxwarp_init, tuxwarp_shutdown, VERSION_STRING } = require('libtuxwarp'); console.log('Version:', VERSION_STRING); tuxwarp_init({}); tuxwarp_shutdown();"
```

## Puntos de entrada de la librería

### Funciones principales
- `tuxwarp_init(config)` - Inicializar la biblioteca con configuración
- `tuxwarp_shutdown()` - Apagar y liberar recursos
- `tuxwarp_crc32c(data)` - Calcular CRC-32C (software fallback)
- `tuxwarp_crc32c_hw(data)` - Calcular CRC-32C (aceleración hardware si disponible)
- `tuxwarp_set_affinity(affinity)` - Afinidad de CPU
- `tuxwarp_get_cpu_id()` - ID de CPU actual
- `tuxwarp_has_simd(level)` - Verificar disponibilidad SIMD
- `tuxwarp_get_simd_capabilities()` - Obtener capacidades SIMD

### Estructuras y tipos
- `TuxWarpError` - Códigos de error (SUCCESS, ERROR_INIT, etc.)
- `SimdLevel` - Niveles SIMD (NONE, AVX2, AVX512F, NEON, SVE)
- `Crc32cSliceMode` - Modos slice (slice-by-8, slice-by-16)
- `ObservabilityMetrics` - Métricas de observabilidad

### Versión
- `VERSION_STRING` - Cadena con versión de la biblioteca ("libtuxwarp 0.1.0")

## Capacidades de hardware

### CRC-32C
- **slice-by-16**: Modo recomendado, ~40% más throughput en ARMv7, ×1.4 general
- **slice-by-8**: Modo legacy, compatible con hardware older
- Soporte automático de aceleración hardware (GF(2^8) cuando está disponible)

### SIMD
- AVX2/AVX512F en x86_64
- NEON en ARM32/ARM64
- SVE en ARM64 (Jetson Orin y posteriores)

### Giant Pages
- Soporte opcional para hugepages para reducir overhead de TLB

## Ejemplos mínimos

### Ejemplo 1: Inicialización básica
```typescript
import { tuxwarp_init, tuxwarp_shutdown } from 'libtuxwarp';

tuxwarp_init({});
tuxwarp_shutdown();
```

### Ejemplo 2: CRC-32C con aceleración hardware
```typescript
import { tuxwarp_crc32c_hw, tuxwarp_has_simd } from 'libtuxwarp';

if (tuxwarp_has_simd('AVX2')) {
  const data = new Uint8Array([0x11, 0x22, 0x33, 0x44]);
  const crc = tuxwarp_crc32c_hw(data);
  console.log(`CRC-32C hardware: 0x${crc.toString(16)}`);
} else {
  console.log('SIMD AVX2 no disponible, usando software');
}
```

## Licencia
MIT - Ver archivo LICENSE para detalles.

## Soporte
- Issues: https://github.com/TuxedoRT-Mojo/libtuxwarp/issues
- Documentación completa: Ver docs/ en el repositorio