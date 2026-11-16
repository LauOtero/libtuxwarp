// TypeScript type definitions for libtuxwarp C library
// Generated for SDK integration - libtuxwarp 0.1.0

/**
 * Error codes returned by libtuxwarp functions
 */
export enum TuxWarpError {
  SUCCESS = 0,
  ERROR_INIT = -1,
  ERROR_MEMORY = -2,
  ERROR_CPU = -3,
  ERROR_TIMEOUT = -4,
  ERROR_INVALID = -5,
  ERROR_BUSY = -6,
  ERROR_NOT_CONFIGURED = -7
}

/**
 * CPU affinity configuration
 */
export interface CpuAffinity {
  cpu_id: number;
  cpu_set_size: number;
}

/**
 * Memory region descriptor
 */
export interface MemoryRegion {
  base_addr: number;
  length: number;
  flags: number;
}

/**
 * Ring buffer configuration
 */
export interface RingBufferConfig {
  size: number;
  producer_id: number;
  consumer_id: number;
}

/**
 * SIMD capability detection
 */
export enum SimdLevel {
  NONE = 0,
  AVX2 = 1,
  AVX512F = 2,
  NEON = 3,
  SVE = 4
}

/**
 * SIMD capabilities info
 */
export interface SimdCapabilities {
  level: SimdLevel;
  avx2_supported: boolean;
  avx512_supported: boolean;
  neon_supported: boolean;
  sve_supported: boolean;
}

/**
 * CRC-32C hardware acceleration config
 */
export interface Crc32cConfig {
  use_hw_accel: boolean;
  slice_size: 'slice-by-8' | 'slice-by-16';
  hugepages_enabled: boolean;
}

/**
 * Main initialization options
 */
export interface TuxWarpConfig {
  cpu_affinity?: CpuAffinity;
  simd_level?: SimdLevel;
  crc32c_config?: Crc32cConfig;
  enable_hugepages?: boolean;
  max_cpus?: number;
}

/**
 * Version information
 */
export const VERSION_STRING: string;

/**
 * Initialize the libtuxwarp library
 * @param config Configuration options
 * @returns Error code (SUCCESS on success)
 */
export function tuxwarp_init(config: TuxWarpConfig): TuxWarpError;

/**
 * Clean up and shutdown the library
 * @returns Error code (SUCCESS on success)
 */
export function tuxwarp_shutdown(): TuxWarpError;

/**
 * Calculate CRC-32C checksum using software fallback
 * @param data Input data buffer (ArrayBuffer or uint8Array)
 * @param length Number of bytes to process
 * @returns CRC-32C value
 */
export function tuxwarp_crc32c(data: ArrayBufferView, length?: number): number;

/**
 * Calculate CRC-32C checksum using hardware acceleration if available
 * @param data Input data buffer (ArrayBuffer or uint8Array)
 * @param length Number of bytes to process
 * @returns CRC-32C value
 */
export function tuxwarp_crc32c_hw(data: ArrayBufferView, length?: number): number;

/**
 * Set CPU affinity for the current thread
 * @param affinity CPU affinity configuration
 * @returns Error code (SUCCESS on success)
 */
export function tuxwarp_set_affinity(affinity: CpuAffinity): TuxWarpError;

/**
 * Get current CPU identifier
 * @returns CPU identifier number
 */
export function tuxwarp_get_cpu_id(): number;

/**
 * Check if a specific SIMD level is available
 * @param level SIMD level to check
 * @returns Boolean indicating availability
 */
export function tuxwarp_has_simd(level: SimdLevel): boolean;

/**
 * Get current SIMD capabilities
 * @returns SimdCapabilities object
 */
export function tuxwarp_get_simd_capabilities(): SimdCapabilities;

/**
 * Ring buffer operations
 */

/**
 * Create a new ring buffer
 * @param config Ring buffer configuration
 * @returns Error code (SUCCESS on success)
 */
export function tuxwarp_ring_create(config: RingBufferConfig): TuxWarpError;

/**
 * Write data to ring buffer (producer)
 * @param buf Data buffer to write
 * @param length Number of bytes to write
 * @returns Number of bytes actually written
 */
export function tuxwarp_ring_write(buf: ArrayBufferView, length: number): number;

/**
 * Read data from ring buffer (consumer)
 * @param buf Data buffer to fill
 * @param length Maximum number of bytes to read
 * @returns Number of bytes actually read
 */
export function tuxwarp_ring_read(buf: ArrayBufferView, length: number): number;

/**
 * Ring buffer status
 * @returns Object with fill level and status info
 */
export function tuxwarp_ring_status(): {
  fill_level: number;
  capacity: number;
  producer_index: number;
  consumer_index: number;
};

/**
 * CRC-32C slice configuration
 */
export enum Crc32cSliceMode {
  SLICE_BY_8 = 8,
  SLICE_BY_16 = 16
}

/**
 * Set CRC-32C slice mode
 * @param mode Slice mode ('slice-by-8' or 'slice-by-16')
 * @returns Error code (SUCCESS on success)
 */
export function tuxwarp_crc32c_set_slice_mode(mode: Crc32cSliceMode): TuxWarpError;

/**
 * Get current CRC-32C slice mode
 * @returns Current slice mode
 */
export function tuxwarp_crc32c_get_slice_mode(): Crc32cSliceMode;

/**
 * EtherCAT protocol operations (simplified types)
 */
export interface EthercatFrame {
  frame_type: number;
  vendor_id: number;
  command_code: number;
  data: ArrayBufferView;
}

/**
 * EtherTux protocol operations (simplified types)
 */
export interface EthertuxFrame {
  frame_type: number;
  source_port: number;
  destination_port: number;
  data: ArrayBufferView;
}

/**
 * Recovery configuration
 */
export interface RecoveryConfig {
  max_retries: number;
  recovery_timeout_ns: number;
  enable_backoff: boolean;
}

/**
 * Observability metrics
 */
export interface ObservabilityMetrics {
  packets_processed: number;
  cycles_per_packet: number;
  average_latency_ns: number;
  wcet_ns: number;
  throughput_mps: number;
}

/**
 * Recovery status
 */
export interface RecoveryStatus {
  last_error: TuxWarpError;
  consecutive_failures: number;
  last_recovery_ns: number;
  recovery_enabled: boolean;
}

/**
 * Public API exports
 */
export {
  tuxwarp_init,
  tuxwarp_shutdown,
  tuxwarp_crc32c,
  tuxwarp_crc32c_hw,
  tuxwarp_set_affinity,
  tuxwarp_get_cpu_id,
  tuxwarp_has_simd,
  tuxwarp_get_simd_capabilities,
  tuxwarp_ring_create,
  tuxwarp_ring_write,
  tuxwarp_ring_read,
  tuxwarp_ring_status,
  tuxwarp_crc32c_set_slice_mode,
  tuxwarp_crc32c_get_slice_mode,
  TuxWarpError,
  SimdLevel,
  SimdCapabilities,
  Crc32cSliceMode,
  EthercatFrame,
  EthertuxFrame,
  RecoveryConfig,
  ObservabilityMetrics,
  RecoveryStatus,
  VERSION_STRING
};