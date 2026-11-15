# Guía de Portación — libtuxwarp

**Versión:** 0.1.0 · **Lenguaje:** C11 · **Build:** CMake ≥ 3.16

## 1. Requisitos del sistema

### 1.1 Hardware mínimo

| Componente | Requisito |
|---|---|
| CPU | ARM64 (AArch64) con CRC32 por instrucción, ARMv7 con fallback *slice-by-16*, o x86-64 con SIMD |
| RAM | 256 MB |
| Almacenamiento | 50 MB |
| Red | 1 Gbps recomendado; NIC dedicada para EtherCAT/EtherTux |

### 1.2 Software

| Software | Mínimo | Recomendado |
|---|---|---|
| Compilador | GCC 11 / Clang 15 | GCC 13+ / Clang 17+ |
| CMake | 3.16 | 3.25+ |
| Linux | 4.15+ (kernel_probe detecta soporte) | 7.x con PREEMPT_RT |
| Bibliotecas | `pthread`, `m`, `dl` | glibc 2.31+ |

Dependencias de ejecución de los protocolos: **solo** `glibc` + `liblibtuxwarp.so`.
No se usa SOEM, IgH, DPDK ni libbpf.

---

## 2. Perfiles de plataforma

`scripts/lib/sbc.sh` es la **fuente única de verdad** con **29 perfiles**:
arquitectura, núcleos, CRC32 por hardware, motor criptográfico, PHC, flags
`-march/-mcpu/-mfpu`, `cmdline` recomendada y notas operativas.

```bash
./scripts/build.sh --list
./scripts/build.sh --detect
```

Familias cubiertas:

- **Raspberry Pi:** Zero 2 W, 3, 4, 5, CM4, CM5
- **Orange Pi:** Zero2, Zero3, 3 LTS, 4 LTS, 5, 5 Plus
- **BigTreeTech / MKS:** CB1, CB2, PI, Skipr
- **Radxa / Rock:** Rock Pi 4, Rock 3 y Rock 5
- **NVIDIA:** Jetson Nano, Jetson Orin
- **x86-64:** genérico y con PREEMPT_RT

### 2.1 Raspberry Pi 4/5

- RPi 4: BCM2711 (Cortex-A72), 4× ARMv8.
- RPi 5: BCM2712 (Cortex-A76), 4× ARMv8, extensiones criptográficas ARMv8.
- RPi 5 puede usar **PHC/Launch Time** con una NIC PCIe Intel i226.
- Interfaz de red habitual: `end0` (device-tree) o `enp1s0` (PCIe).

```bash
./scripts/build.sh --sbc rpi5 --release
```

### 2.2 Jetson Nano / Orin

- Nano: Cortex-A57; Orin: Cortex-A78AE.
- SVE en Orin si el compilador lo soporta (`HAVE_SVE`).
- Contador de ciclos vía `perf_event_open` (no `pmccntr_el0` directo).

```bash
./scripts/build.sh --sbc jetson-orin --release
```

### 2.3 Orange Pi 5 / ROCK 5B

- RK3588: 4× Cortex-A76 + 4× Cortex-A55.
- NEON y CRC32 por instrucción (Cortex-A53+).
- `rk2_crypto` aún en parcheo: **no** usar en camino crítico.

```bash
./scripts/build.sh --sbc opi-5 --release
```

### 2.4 Intel NUC / x86-64 RT

- AVX2 o AVX-512 según CPU; CRC-32C SSE4.2.
- PHC/Launch Time en Intel i210/i225/i226.

```bash
./scripts/build.sh --sbc x86-64 --native
```

---

## 3. Compilación

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 3.1 Opciones de CMake

| Opción | Defecto | Descripción |
|---|---|---|
| `BUILD_PROTOCOLS` | `ON` | Compila EtherCAT y EtherTux |
| `BUILD_TESTS` | `ON` | 10 suites de `ctest` |
| `BUILD_BENCHMARK` | `ON` | Ejecutable `bench` |
| `BUILD_TOOLS` | `ON` | Instala scripts de ayuda |
| `USE_SIMD` | `ON` | AVX-512 / AVX2 / NEON / SVE |
| `USE_HUGEPAGES` | `ON` | Soporte de *hugepages* |
| `ENABLE_LTO` | `ON` | *Link-time optimization* |
| `ENABLE_NATIVE` | `OFF` | `-march=native` (no portable) |
| `TUXRT_PROVIDER_MODULE_DIR` | autodetectado | Directorio con `TuxRTProvider.cmake` |

Detección de SIMD:
- **AVX-512 / AVX2**: se activa mediante `check_csource_runs` (compilación y ejecución), no solo comprobando flags del compilador (en Zen 2 hay AVX2 sin AVX-512).
- **NEON**: se activa mediante `check_csource_compiles` (solo compilación), obligatorio en AArch64.
- **SVE**: se activa mediante `check_csource_compiles` con `__ARM_FEATURE_SVE`, disponible en Orin y posteriores.

La tabla resultante se establece en tiempo de compilación y se lee en tiempo de ejecución a través de `hw_caps()`.

### 3.2 Cross-compilación

```bash
# Listar perfiles
./scripts/build.sh --list

# Cross-compilar
./scripts/build.sh --sbc rpi5 --release
./scripts/build.sh --sbc opi-5 --release

# Ver flags y cmdline sin compilar
./scripts/build.sh --sbc rpi5 --print-config

# Sysroot explícito
./scripts/build.sh --sbc rpi4 --release --sysroot /usr/aarch64-linux-gnu
```

Toolchains incluidas: `cmake/aarch64-linux-gnu.cmake` y `cmake/jetson.cmake`.
Estas definen `CMAKE_SYSTEM_PROCESSOR`, `CMAKE_C_COMPILER` y flags `-march=armv8-a` (aarch64) o `-march=armv7-a+neon-vfpv4` (armv7).

**Importante:** En cross-compilación, la detección de SIMD por `check_csource_runs` fallará (no hay CPU objetivo). En su lugar, se debe especificar manualmente:
- `aarch64` con CRC32: `-march=armv8-a+crc` + backend `armv8-crc`
- `aarch64` sin CRC32 HW (SBCs sin Ethernet nativo): `-march=armv7-a+neon-vfpv4` + fallback `slice-by-16`
- `x86_64`: flags SSE4.2/AVX2/AVX-512 según CPU

---

## 4. Kernel de tiempo real

### 4.1 PREEMPT_RT en kernel 7.x

Desde Linux 6.12, PREEMPT_RT está en mainline. Para activarlo:

```
# /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=2-7 nohz_full=2-7 rcu_nocbs=2-7 \
    preempt=full rcu_nocb_poll tsc=reliable skew_tick=1 \
    nosoftlockup mce=ignore_ce idle=poll"
```

Verificación:

```bash
uname -r
cat /proc/cmdline
cat /sys/kernel/debug/sched/preempt 2>/dev/null
cat /sys/kernel/realtime 2>/dev/null
```

En kernels antiguos, instale el kernel `-rt` de su distribución o compile el
parche. libtuxwarp **detecta** el estado y avisa; no modifica el `cmdline`.

### 4.2 Ajuste del sistema

```bash
# /etc/sysctl.d/99-libtuxwarp.conf
vm.swappiness = 0
vm.zone_reclaim_mode = 0
kernel.numa_balancing = 0
kernel.sched_autogroup_enabled = 0
```

- *Governor* `performance`.
- C-states limitados (`processor.max_cstate=1`, `intel_idle.max_cstate=0`).
- IRQ de la NIC fijadas a los núcleos aislados.
- Hugepages si se usan páginas explícitas:

```bash
sudo sysctl -w vm.nr_hugepages=64
grep -i huge /proc/meminfo
```

---

## 5. Preparación de la red

libtuxwarp tiene **dos backends mutuamente excluyentes**. Elija uno por interfaz:

```bash
export PROTOCOL=ethercat   # defecto
# o
export PROTOCOL=ethertux
```

### 5.1 Interfaz

```bash
ip -br link                 # nombre real: enp2s0, eno1, end0, enx<MAC>
ls /sys/class/net
sudo ip link set enp2s0 up
cat /sys/class/net/enp2s0/carrier      # 1 = cable conectado
```

### 5.2 Permisos

`AF_PACKET` requiere `CAP_NET_RAW` (y `CAP_NET_ADMIN` para promiscuo):

```bash
sudo setcap cap_net_raw,cap_net_admin+ep ./build/bench
# o ejecutar como root
```

### 5.3 EtherCAT

- NIC **dedicada** al segmento EtherCAT.
- Cable a la entrada IN de la primera esclava.
- No se requiere módulo del kernel ni DPDK.

### 5.4 EtherTux

- NIC y switch **comerciales**; red exclusiva para el lazo de control.
- Si la NIC soporta PHC/Launch Time, `hw_caps_for_interface()` lo
  reporta y `send_at()` aplica *pacing* por hardware.
- `PACKET_TX_RING` se activa en modo `AUTO` si el kernel lo permite.
- El ID local determinista añade **≈ 14,4 KB** de RAM (tabla de 256 nodos +
  bitmap de 65 536 IDs) y no requiere privilegios; la persistencia necesita un
  directorio escribible (p. ej. `/var/lib/libtuxwarp/`).

---

## 6. Verificación de la portación

### 6.1 Suites

```bash
ctest --test-dir build --output-on-failure
# o
./scripts/test_runner.sh --sbc <id> --mode full
```

| Suite | Aserciones |
|---|---:|
| `test` | 20 |
| `test_lockfree` | 26 |
| `test_rt` | 30 |
| `test_resilience` | 29 |
| `test_integration` | 27 |
| `test_network` | 58 |
| `test_hwaccel` | 40 |
| `test_ethertux` | 103 |
| `test_ethertux_v010` | 152 |
| `test_aalid` | 62 |
| **Total** | **547** |

### 6.2 HAL

```bash
./build/bench --mode crc --duration 1
```

Debe mostrar el backend esperado:

- x86-64 con SSE4.2: `sse4.2-x4`.
- ARMv8.1+: `armv8-crc`.
- ARMv7 / Cortex-A7: `slice-by-16`.

### 6.3 Kernel

```bash
./build/bench --mode latency --duration 1 | head -n 12
```

Debe indicar la versión, PREEMPT_RT, aislamiento y nivel de soporte (`0..3`).

### 6.4 Protocolo (requiere hardware)

```bash
sudo ./build/bench --mode latency --duration 5
```

Para una prueba real de bus, use la API de `ethercat.h` o
`ethertux.h` (los tests no abren NIC porque requieren `CAP_NET_RAW`).

---

## 7. Solución de problemas

### 7.1 Latencia alta o jitter elevado

1. Confirme PREEMPT_RT (`support_level >= 2`).
2. Aísle núcleos (`isolcpus`, `nohz_full`, `rcu_nocbs`) y fije afinidad.
3. *Governor* `performance`; desactive C-states/turbo variable.
4. Fije las IRQ de la NIC a los núcleos aislados.
5. Evite *swapping*; use `mlockall`/hugepages.

```bash
mpstat -P ALL 1
cat /proc/interrupts | grep -i enp
rtla timerlat top -d 30s
```

### 7.2 No se abre la interfaz

- Nombre incorrecto (`eth0` ya no es habitual).
- Interfaz `DOWN` o sin *carrier*.
- Falta `CAP_NET_RAW`/`CAP_NET_ADMIN`.

```bash
ip -br link
cat /sys/class/net/enp2s0/carrier
sudo setcap cap_net_raw,cap_net_admin+ep ./build/bench
```

### 7.3 No se detectan esclavas EtherCAT

- Cable en el puerto correcto (IN de la primera esclava).
- Interfaz dedicada, no compartida con el tráfico del sistema.
- Compruebe `frames_lost`, `is_lost` y el WKC.

### 7.4 `BACKPRESSURE` constante en EtherTux

- La ventana actual no corresponde a esa prioridad: revise el planificador.
- Clase saturada: reduzca la tasa o aumente `rate_bytes_per_sec`.

### 7.5 Pérdidas de paquetes

- Active FEC (`LIGHT` o `ROBUST`).
- Revise `storm-control` y buffers del switch.
- Verifique que no hay duplicación de tráfico fuera de FRM.

### 7.6 Fallo de compilación SIMD

```bash
# x86 sin AVX-512: deshabilitar SIMD o dejar la detección automática
cmake -S . -B build -DUSE_SIMD=OFF

# ARM64 sin toolchain
sudo apt install gcc-aarch64-linux-gnu
```

### 7.7 `SCHED_FIFO`/`SCHED_DEADLINE` denegados

```bash
sudo setcap cap_sys_nice=eip ./build/bench
# o
echo "@user - rtprio 99" | sudo tee -a /etc/security/limits.conf
```

---

## 8. Checklist de portación

- [ ] Kernel 5.15+ (ideal 7.x) con PREEMPT_RT o `preempt=full`.
- [ ] `cmdline` con `isolcpus`, `nohz_full`, `rcu_nocbs`.
- [ ] *Governor* `performance`; C-states controlados.
- [ ] Hugepages configuradas si se usan.
- [ ] IRQ de la NIC en núcleos aislados.
- [ ] `CAP_NET_RAW`/`CAP_NET_ADMIN` para el binario.
- [ ] Nombre real de la interfaz verificado (`ip -br link`).
- [ ] Protocolo elegido (`PROTOCOL`).
- [ ] `ctest` 10/10 y 547 aserciones.
- [ ] `--mode aalid` con candidato < 10 ns y admisión < 100 ns (si usa ID local).
- [ ] `--mode crc` con backend HW (o *fallback* aceptado).
- [ ] `kernel_probe()` con `support_level >= 2`.
- [ ] Benchmark de referencia guardado para comparar regresiones.
