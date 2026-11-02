import {createEmulator} from 'dynarmic.js';

const arm = await createEmulator();
const a32Value: number = arm.getRegister(0);
arm.setRegister(0, a32Value);
arm.setPC(4096);
// @ts-expect-error A32 registers are numbers.
arm.setRegister(0, 1n);
const a64 = await createEmulator({architecture: 'a64', memorySize: 65536});
const a64Value: bigint = a64.getRegister(0);
a64.setRegister(0, a64Value);
a64.setPC(4096n);
// @ts-expect-error A64 registers retain 64 bits through bigint.
a64.setRegister(0, 1);
// @ts-expect-error Only supported architectures are accepted.
await createEmulator({architecture: 'x86'});
const architecture = Math.random() ? 'a32' : 'a64';
const cpu = await createEmulator({architecture});
if (cpu.architecture === 'a64') {
  const value: bigint = cpu.getPC();
  cpu.setPC(value);
} else {
  const value: number = cpu.getPC();
  cpu.setPC(value);
}
cpu.writeMemory(0, new Uint8Array(4));
const bytes: Uint8Array = cpu.readMemory(0, 4);
void bytes;
for (const instance of [cpu, arm, a64]) instance.dispose();
