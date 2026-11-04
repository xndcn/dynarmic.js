/** ARM32 (including Thumb) or AArch64 guest instruction set. */
export type Architecture = 'a32' | 'a64';
export type RegisterValue<A extends Architecture> = A extends 'a64' ? bigint : number;

export interface OptimizationOptions {
  /** Share ARM/Thumb/A64 region entries to reduce repeated compilation; may reduce warm throughput. Default: false. */
  regionMultiEntry?: boolean;
}

export interface CreateEmulatorOptions<A extends Architecture = Architecture> {
  /** Default: 'a32'. Select Thumb by setting bit 5 of the A32 status register. */
  architecture?: A;
  /** Zero-initialized guest RAM, in bytes. 64 KiB–256 MiB, a multiple of 4096. Default: 16 MiB. */
  memorySize?: number;
  /** Compilation policy, fixed when creating the emulator. */
  optimizations?: OptimizationOptions;
  /** Absolute URL for runtime.wasm. file: URLs work in Node.js; browsers require a fetchable URL. */
  wasmURL?: string | URL;
  /** Supply Wasm bytes instead of loading an asset. Mutually exclusive with wasmURL. */
  wasmBinary?: Uint8Array | ArrayBuffer;
}

export interface RunOptions {
  /** Cooperative tick budget, 0–2147483647. Default: 1,000,000. May overshoot by a translated block. */
  maxTicks?: number;
}

interface StopState<A extends Architecture> {
  /** PC at return. After SVC, this is the next instruction; other faults need host-directed recovery. */
  pc: RegisterValue<A>;
  /** Raw Dynarmic HaltReason bitmask, for diagnostics. Prefer the reason discriminator. */
  haltReason: number;
}

export type RunResult<A extends Architecture = Architecture> = StopState<A> & (
  | {reason: 'halted' | 'budget' | 'step'}
  | {reason: 'svc'; svc: number}
  | {reason: 'memory-fault'; address: RegisterValue<A>; access: 'read' | 'write' | 'execute'; size: number}
  | {reason: 'unsupported-instruction'; address: RegisterValue<A>; instructions: number}
  | {reason: 'exception'; address: RegisterValue<A>; exception: number}
);

interface EmulatorInstance<A extends Architecture> {
  readonly architecture: A;
  readonly memorySize: number;
  readonly disposed: boolean;
  /** Read r0–r15 (A32) or x0–x30 (A64). A64 values retain all 64 bits as unsigned bigint. */
  getRegister(index: number): RegisterValue<A>;
  setRegister(index: number, value: RegisterValue<A>): void;
  getPC(): RegisterValue<A>;
  /** Alignment is checked when executing. Setting an A32 odd PC does not automatically select Thumb. */
  setPC(value: RegisterValue<A>): void;
  getSP(): RegisterValue<A>;
  setSP(value: RegisterValue<A>): void;
  /** A32 CPSR or A64 PSTATE, represented as an unsigned 32-bit number. */
  getStatus(): number;
  setStatus(value: number): void;
  /** Returns a copy. Addresses are number offsets into guest RAM for either architecture. */
  readMemory(address: number, size: number): Uint8Array;
  /** Copies bytes into RAM and invalidates translated code and exclusive reservations for host edits. */
  writeMemory(address: number, data: Uint8Array): void;
  /** Invalidate compiled guest code after guest self-modification, while the CPU is stopped. */
  invalidateCache(address: number, size: number): void;
  clearCache(): void;
  /** Synchronous execution. In browsers use a Worker; budgets are cooperative, not wall-clock deadlines. */
  run(options?: RunOptions): RunResult<A>;
  step(): RunResult<A>;
  /** Release the CPU and translated code. Safe to call more than once; other methods then throw. */
  dispose(): void;
}

/** A union of architecture-specific instances, narrowed by the architecture property. */
export type Emulator<A extends Architecture = Architecture> = A extends Architecture ? EmulatorInstance<A> : never;

/** Initialize an independent CPU and Wasm module. Importing the package alone does not initialize Wasm. */
export function createEmulator(options?: CreateEmulatorOptions<'a32'>): Promise<Emulator<'a32'>>;
export function createEmulator(options: CreateEmulatorOptions<'a64'> & {architecture: 'a64'}): Promise<Emulator<'a64'>>;
export function createEmulator<A extends Architecture>(options: CreateEmulatorOptions<A> & {architecture: A}): Promise<Emulator<A>>;
export function createEmulator(options: CreateEmulatorOptions): Promise<Emulator>;
