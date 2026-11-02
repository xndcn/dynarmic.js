// Shared acceptance cases, executed against an installed package in Node and
// a real browser module Worker. These bytes require no guest toolchain.
const equal = (actual, expected, label) => {
  if (actual !== expected) throw new Error(`${label}: expected ${String(expected)}, got ${String(actual)}`);
};
const throws = (fn, label) => {
  try { fn(); } catch { return; }
  throw new Error(`${label}: expected rejection`);
};
const words = (...values) => {
  const bytes = new Uint8Array(values.length * 4), view = new DataView(bytes.buffer);
  values.forEach((value, i) => view.setUint32(i * 4, value, true));
  return bytes;
};

export async function exercise(createEmulator, options = {}) {
  const checks = [];
  for (const architecture of ['a32', 'a64']) {
    const cpu = await createEmulator({...options, architecture, memorySize: 65536});
    const value = n => architecture === 'a64' ? BigInt(n) : n;
    const program = architecture === 'a32'
      ? words(0xe3a00028, 0xe2800002, 0xef000007, 0xe2800001, 0xef000008)
      : words(0xd2800500, 0x91000800, 0xd40000e1, 0x91000400, 0xd4000101);
    try {
      cpu.writeMemory(0x1000, program);
      cpu.setPC(value(0x1000));
      cpu.setSP(value(0xff00));
      equal(cpu.getSP(), value(0xff00), 'SP');
      equal(cpu.run({maxTicks: 0}).reason, 'budget', 'zero budget');
      equal(cpu.getPC(), value(0x1000), 'zero budget preserves PC');
      equal(cpu.step().reason, 'step', 'single step');
      equal(cpu.getRegister(0), value(40), 'single step value');
      equal(cpu.getPC(), value(0x1004), 'single step PC');
      const first = cpu.run({maxTicks: 100});
      equal(first.reason, 'svc', 'first SVC');
      equal(first.svc, 7, 'SVC immediate');
      equal(cpu.getRegister(0), value(42), 'guest result');
      equal(first.pc, value(0x100c), 'resume PC');
      cpu.setRegister(0, value(100));
      const second = cpu.run();
      equal(second.svc, 8, 'resume SVC');
      equal(cpu.getRegister(0), value(101), 'resume accepts host register changes');
      checks.push(`${architecture}: step, SVC, resume and zero budget`);

      cpu.writeMemory(4095, Uint8Array.of(1, 2, 3, 4));
      const copy = cpu.readMemory(4095, 4);
      equal(copy.join(','), '1,2,3,4', 'cross-page bytes');
      copy[0] = 99;
      equal(cpu.readMemory(4095, 1)[0], 1, 'read returns a copy');
      cpu.writeMemory(65535, Uint8Array.of(123));
      equal(cpu.readMemory(65535, 1)[0], 123, 'last RAM byte');
      equal(cpu.readMemory(65536, 0).length, 0, 'empty boundary read');
      for (const [address, size] of [[-1, 1], [0.5, 1], [0, -1], [0, NaN], [65536, 1], [65535, 2], [2 ** 32, 1]])
        throws(() => cpu.readMemory(address, size), 'invalid read');
      throws(() => cpu.writeMemory(65535, Uint8Array.of(1, 2)), 'out-of-bounds write');
      throws(() => cpu.setRegister(architecture === 'a32' ? 16 : 31, value(0)), 'invalid register');
      throws(() => cpu.setRegister(0, architecture === 'a32' ? 0x100000000 : 1n << 64n), 'register overflow');
      for (const maxTicks of [-1, 0.5, NaN, Infinity, 2 ** 31, null, '100'])
        throws(() => cpu.run({maxTicks}), 'invalid budget');
      throws(() => cpu.run(null), 'invalid run options');
      throws(() => cpu.writeMemory(0, new DataView(new ArrayBuffer(4))), 'invalid memory data');
      throws(() => cpu.setRegister(0, architecture === 'a32' ? 1n : 1), 'wrong register value type');
      checks.push(`${architecture}: memory copies, boundaries and input validation`);

      const replacement = architecture === 'a32'
        ? words(0xe3a00009, 0xef000007) : words(0xd2800120, 0xd40000e1);
      cpu.writeMemory(0x1000, replacement);
      cpu.setPC(value(0x1000));
      equal(cpu.run().svc, 7, 'patched code SVC');
      equal(cpu.getRegister(0), value(9), 'host writes invalidate compiled code');
      cpu.invalidateCache(0x1000, replacement.length);
      cpu.clearCache();
      checks.push(`${architecture}: code cache invalidation`);

      cpu.writeMemory(0x2000, architecture === 'a32'
        ? words(0xe2800001, 0xeafffffd) : words(0x91000400, 0x17ffffff));
      cpu.setPC(value(0x2000));
      cpu.setRegister(0, value(0));
      equal(cpu.run({maxTicks: 1}).reason, 'budget', 'infinite loop yields');
      const before = cpu.getRegister(0);
      equal(cpu.run({maxTicks: 100}).reason, 'budget', 'loop resumes then yields');
      if (!(cpu.getRegister(0) > before)) throw new Error('loop did not advance');
      checks.push(`${architecture}: cooperative budget interrupts a guest loop`);

      cpu.writeMemory(0x3000, architecture === 'a32'
        ? words(0xe5910000, 0xef000000) : words(0xf9400020, 0xd4000001));
      cpu.setRegister(1, value(65536));
      cpu.setPC(value(0x3000));
      const fault = cpu.run();
      equal(fault.reason, 'memory-fault', 'guest out-of-bounds load');
      equal(fault.address, value(65536), 'fault address');
      equal(fault.access, 'read', 'load access kind');
      equal(fault.size, architecture === 'a32' ? 4 : 8, 'load width');
      cpu.setRegister(1, value(65535));
      cpu.setPC(value(0x3000));
      equal(cpu.run().reason, 'memory-fault', 'load straddling RAM boundary');
      cpu.writeMemory(0x3000, architecture === 'a32'
        ? words(0xe5810000, 0xef000000) : words(0xf9000020, 0xd4000001));
      cpu.setPC(value(0x3000));
      const writeFault = cpu.run();
      equal(writeFault.reason, 'memory-fault', 'store straddling RAM boundary');
      equal(writeFault.access, 'write', 'store access kind');
      equal(cpu.readMemory(65535, 1)[0], 123, 'faulting store preserves RAM');
      cpu.writeMemory(0x3000, architecture === 'a32'
        ? words(0xe5d10000, 0xef000000) : words(0x39400020, 0xd4000001));
      cpu.setPC(value(0x3000));
      equal(cpu.run().reason, 'svc', 'recover after memory fault');
      equal(cpu.getRegister(0), value(123), 'guest reads last RAM byte');
      cpu.setPC(value(0x3001));
      const unaligned = cpu.run();
      equal(unaligned.reason, 'memory-fault', 'misaligned instruction fetch');
      equal(unaligned.access, 'execute', 'instruction access kind');
      cpu.setPC(value(65536));
      equal(cpu.run().reason, 'memory-fault', 'guest instruction fetch fault');
      checks.push(`${architecture}: data and instruction faults, boundary access and recovery`);

      if (architecture === 'a64') {
        cpu.setRegister(1, 0xfedcba9876543210n);
        cpu.writeMemory(0x4000, words(0x91000420, 0xd4000001)); // add x0,x1,#1; svc #0
        cpu.setPC(0x4000n);
        equal(cpu.run().svc, 0, '64-bit SVC');
        equal(cpu.getRegister(0), 0xfedcba9876543211n, '64-bit arithmetic');
        cpu.setPC(0x100000000n);
        const high = cpu.run();
        equal(high.reason, 'memory-fault', 'high address fault');
        equal(high.address, 0x100000000n, 'fault address retains high bits');
        checks.push('a64: BigInt arithmetic and fault addresses retain all 64 bits');
      } else {
        cpu.writeMemory(0x4000, Uint8Array.of(0x28, 0x20, 0x02, 0x30, 0x07, 0xdf, 0x01, 0x30, 0x08, 0xdf));
        cpu.setStatus(0x3f); // system mode, Thumb
        cpu.setPC(0x4000);
        equal(cpu.run().svc, 7, 'Thumb SVC');
        equal(cpu.getRegister(0), 42, 'Thumb result');
        equal(cpu.run().svc, 8, 'Thumb resume');
        equal(cpu.getRegister(0), 43, 'Thumb resumed result');
        cpu.writeMemory(65534, Uint8Array.of(0x09, 0xdf));
        cpu.setPC(65534);
        equal(cpu.run().svc, 9, 'Thumb instruction at last halfword');
        checks.push('a32: Thumb state and SVC resume');
      }
    } finally { cpu.dispose(); }
    cpu.dispose();
    equal(cpu.disposed, true, 'disposed state');
    throws(() => cpu.run(), 'run after disposal');
    throws(() => cpu.readMemory(0, 1), 'memory after disposal');
    throws(() => cpu.getRegister(0), 'register after disposal');
  }
  const a = await createEmulator({...options, memorySize: 65536});
  const b = await createEmulator({...options, memorySize: 65536});
  try {
    a.writeMemory(0x1000, words(0xe3a00001, 0xef000000));
    b.writeMemory(0x1000, words(0xe3a00002, 0xef000000));
    a.setPC(0x1000); b.setPC(0x1000);
    a.run(); b.run();
    equal(a.getRegister(0), 1, 'instance A');
    equal(b.getRegister(0), 2, 'instance B');
    a.dispose();
    b.setPC(0x1000); b.run();
    equal(b.getRegister(0), 2, 'surviving instance');
  } finally { a.dispose(); b.dispose(); }
  checks.push('independent live instances and idempotent disposal');
  return checks;
}
