from pathlib import Path
p = Path('src/dynarmic/backend/wasm/emit_wasm_data_processing.cpp')
s = p.read_text()
for sub in [False, True]:
    marker = '    // Full path: t = zext(a)' if sub else '    // Full path: compute in i64'
    begin = s.index(marker)
    end = s.index('    const u32 overflow = ctx.Temp(ValType::I32);',begin)
    old = s[begin:end]
    old = old.replace('    const u32 carry = ctx.Temp(ValType::I32);\n', '')
    fast = '''    // With fixed carry-in and live carry flags, stay in i32. The no-flags
    // path above and dynamic ADC/SBC below keep their existing emission.
    const u32 carry = ctx.Temp(ValType::I32);
    if (args[2].IsImmediate() && args[2].GetImmediateU1() == FIXED) {
        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(OP);
        code.LocalSet(dst);
CARRY
        code.LocalSet(carry);
    } else {
'''
    c = '''        EmitPush(code, ctx.reg_alloc, args[0]);
        EmitPush(code, ctx.reg_alloc, args[1]);
        code.U8(Op::I32GeU);''' if sub else '''        code.LocalGet(dst);
        EmitPush(code, ctx.reg_alloc, args[0]);
        code.U8(Op::I32LtU);'''
    fast = fast.replace('FIXED','true' if sub else 'false').replace('OP','Op::I32Sub' if sub else 'Op::I32Add').replace('CARRY',c)
    s=s[:begin]+fast+''.join('    '+line if line.strip() else line for line in old.splitlines(keepends=True))+'    }\n\n'+s[end:]
p.write_text(s)
