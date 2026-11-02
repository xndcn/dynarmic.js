// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Minimal WebAssembly binary module emitter.
//
// This is a self-contained WASM writer used by the WASM backend to emit
// executable modules for translated guest basic blocks at runtime. It
// deliberately supports only the features the backend needs:
//   - type section (function signatures, deduplicated)
//   - import section (functions, memory, table, globals)
//   - function/table/memory/global/export/code sections
//   - element section (declarative, for declaring function refs)
// No name section, no data section, no start section.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <mcl/stdint.hpp>

namespace Dynarmic::Backend::Wasm {

// ---------------------------------------------------------------------------
// LEB128 encoding
// ---------------------------------------------------------------------------

void EncodeULEB(std::vector<u8>& out, u64 value);
void EncodeSLEB(std::vector<u8>& out, s64 value);

// ---------------------------------------------------------------------------
// Core types
// ---------------------------------------------------------------------------

enum class ValType : u8 {
    I32 = 0x7F,
    I64 = 0x7E,
    F32 = 0x7D,
    F64 = 0x7C,
    V128 = 0x7B,
    FuncRef = 0x70,
    ExternRef = 0x6F,
};

enum class ExternalKind : u8 {
    Function = 0x00,
    Table = 0x01,
    Memory = 0x02,
    Global = 0x03,
};

// Limits flag: bit0 = has_max, bit1 = shared, bit2 = memory64
struct Limits {
    u64 min = 0;
    std::optional<u64> max;
    bool shared = false;
    bool memory64 = false;
};

struct FuncType {
    std::vector<ValType> params;
    std::vector<ValType> results;

    bool operator==(const FuncType&) const = default;
};

struct GlobalType {
    ValType type;
    bool mutable_;
};

// ---------------------------------------------------------------------------
// Opcode constants (subset). Prefix opcodes are emitted as
// (prefix byte, ULEB subopcode).
// ---------------------------------------------------------------------------

namespace Op {

// Control
constexpr u8 Unreachable = 0x00;
constexpr u8 Nop = 0x01;
constexpr u8 Block = 0x02;
constexpr u8 Loop = 0x03;
constexpr u8 If = 0x04;
constexpr u8 Else = 0x05;
constexpr u8 End = 0x0B;
constexpr u8 Br = 0x0C;
constexpr u8 BrIf = 0x0D;
constexpr u8 BrTable = 0x0E;
constexpr u8 Return = 0x0F;
constexpr u8 Call = 0x10;
constexpr u8 CallIndirect = 0x11;
// Tail-call proposal (enabled by default in V8 >= 11.2 and WebKit).
constexpr u8 ReturnCallIndirect = 0x13;

// Reference
constexpr u8 RefNull = 0xD0;
constexpr u8 RefIsNull = 0xD1;
constexpr u8 RefFunc = 0xD2;

// Parametric
constexpr u8 Drop = 0x1A;
constexpr u8 Select = 0x1B;
constexpr u8 SelectT = 0x1C;

// Variable
constexpr u8 LocalGet = 0x20;
constexpr u8 LocalSet = 0x21;
constexpr u8 LocalTee = 0x22;
constexpr u8 GlobalGet = 0x23;
constexpr u8 GlobalSet = 0x24;

// Table
constexpr u8 TableGet = 0x25;
constexpr u8 TableSet = 0x26;

// Memory (each followed by memarg: align ULEB, offset ULEB)
constexpr u8 I32Load = 0x28;
constexpr u8 I64Load = 0x29;
constexpr u8 F32Load = 0x2A;
constexpr u8 F64Load = 0x2B;
constexpr u8 I32Load8S = 0x2C;
constexpr u8 I32Load8U = 0x2D;
constexpr u8 I32Load16S = 0x2E;
constexpr u8 I32Load16U = 0x2F;
constexpr u8 I64Load8S = 0x30;
constexpr u8 I64Load8U = 0x31;
constexpr u8 I64Load16S = 0x32;
constexpr u8 I64Load16U = 0x33;
constexpr u8 I64Load32S = 0x34;
constexpr u8 I64Load32U = 0x35;
constexpr u8 I32Store = 0x36;
constexpr u8 I64Store = 0x37;
constexpr u8 F32Store = 0x38;
constexpr u8 F64Store = 0x39;
constexpr u8 I32Store8 = 0x3A;
constexpr u8 I32Store16 = 0x3B;
constexpr u8 I64Store8 = 0x3C;
constexpr u8 I64Store16 = 0x3D;
constexpr u8 I64Store32 = 0x3E;
constexpr u8 MemorySize = 0x3F;  // followed by 0x00
constexpr u8 MemoryGrow = 0x40;  // followed by 0x00

// Numeric constants
constexpr u8 I32Const = 0x41;  // SLEB i32
constexpr u8 I64Const = 0x42;  // SLEB i64
constexpr u8 F32Const = 0x43;  // 4 raw bytes
constexpr u8 F64Const = 0x44;  // 8 raw bytes

// Comparisons i32
constexpr u8 I32Eqz = 0x45;
constexpr u8 I32Eq = 0x46;
constexpr u8 I32Ne = 0x47;
constexpr u8 I32LtS = 0x48;
constexpr u8 I32LtU = 0x49;
constexpr u8 I32GtS = 0x4A;
constexpr u8 I32GtU = 0x4B;
constexpr u8 I32LeS = 0x4C;
constexpr u8 I32LeU = 0x4D;
constexpr u8 I32GeS = 0x4E;
constexpr u8 I32GeU = 0x4F;

// Comparisons i64
constexpr u8 I64Eqz = 0x50;
constexpr u8 I64Eq = 0x51;
constexpr u8 I64Ne = 0x52;
constexpr u8 I64LtS = 0x53;
constexpr u8 I64LtU = 0x54;
constexpr u8 I64GtS = 0x55;
constexpr u8 I64GtU = 0x56;
constexpr u8 I64LeS = 0x57;
constexpr u8 I64LeU = 0x58;
constexpr u8 I64GeS = 0x59;
constexpr u8 I64GeU = 0x5A;

// Comparisons f32/f64
constexpr u8 F32Eq = 0x5B;
constexpr u8 F32Ne = 0x5C;
constexpr u8 F32Lt = 0x5D;
constexpr u8 F32Gt = 0x5E;
constexpr u8 F32Le = 0x5F;
constexpr u8 F32Ge = 0x60;
constexpr u8 F64Eq = 0x61;
constexpr u8 F64Ne = 0x62;
constexpr u8 F64Lt = 0x63;
constexpr u8 F64Gt = 0x64;
constexpr u8 F64Le = 0x65;
constexpr u8 F64Ge = 0x66;

// i32 arithmetic
constexpr u8 I32Clz = 0x67;
constexpr u8 I32Ctz = 0x68;
constexpr u8 I32Popcnt = 0x69;
constexpr u8 I32Add = 0x6A;
constexpr u8 I32Sub = 0x6B;
constexpr u8 I32Mul = 0x6C;
constexpr u8 I32DivS = 0x6D;
constexpr u8 I32DivU = 0x6E;
constexpr u8 I32RemS = 0x6F;
constexpr u8 I32RemU = 0x70;
constexpr u8 I32And = 0x71;
constexpr u8 I32Or = 0x72;
constexpr u8 I32Xor = 0x73;
constexpr u8 I32Shl = 0x74;
constexpr u8 I32ShrS = 0x75;
constexpr u8 I32ShrU = 0x76;
constexpr u8 I32Rotl = 0x77;
constexpr u8 I32Rotr = 0x78;

// i64 arithmetic
constexpr u8 I64Clz = 0x79;
constexpr u8 I64Ctz = 0x7A;
constexpr u8 I64Popcnt = 0x7B;
constexpr u8 I64Add = 0x7C;
constexpr u8 I64Sub = 0x7D;
constexpr u8 I64Mul = 0x7E;
constexpr u8 I64DivS = 0x7F;
constexpr u8 I64DivU = 0x80;
constexpr u8 I64RemS = 0x81;
constexpr u8 I64RemU = 0x82;
constexpr u8 I64And = 0x83;
constexpr u8 I64Or = 0x84;
constexpr u8 I64Xor = 0x85;
constexpr u8 I64Shl = 0x86;
constexpr u8 I64ShrS = 0x87;
constexpr u8 I64ShrU = 0x88;
constexpr u8 I64Rotl = 0x89;
constexpr u8 I64Rotr = 0x8A;

// f32 arithmetic
constexpr u8 F32Abs = 0x8B;
constexpr u8 F32Neg = 0x8C;
constexpr u8 F32Ceil = 0x8D;
constexpr u8 F32Floor = 0x8E;
constexpr u8 F32Trunc = 0x8F;
constexpr u8 F32Nearest = 0x90;
constexpr u8 F32Sqrt = 0x91;
constexpr u8 F32Add = 0x92;
constexpr u8 F32Sub = 0x93;
constexpr u8 F32Mul = 0x94;
constexpr u8 F32Div = 0x95;
constexpr u8 F32Min = 0x96;
constexpr u8 F32Max = 0x97;
constexpr u8 F32Copysign = 0x98;

// f64 arithmetic
constexpr u8 F64Abs = 0x99;
constexpr u8 F64Neg = 0x9A;
constexpr u8 F64Ceil = 0x9B;
constexpr u8 F64Floor = 0x9C;
constexpr u8 F64Trunc = 0x9D;
constexpr u8 F64Nearest = 0x9E;
constexpr u8 F64Sqrt = 0x9F;
constexpr u8 F64Add = 0xA0;
constexpr u8 F64Sub = 0xA1;
constexpr u8 F64Mul = 0xA2;
constexpr u8 F64Div = 0xA3;
constexpr u8 F64Min = 0xA4;
constexpr u8 F64Max = 0xA5;
constexpr u8 F64Copysign = 0xA6;

// Conversions
constexpr u8 I32WrapI64 = 0xA7;
constexpr u8 I32TruncF32S = 0xA8;
constexpr u8 I32TruncF32U = 0xA9;
constexpr u8 I32TruncF64S = 0xAA;
constexpr u8 I32TruncF64U = 0xAB;
constexpr u8 I64ExtendI32S = 0xAC;
constexpr u8 I64ExtendI32U = 0xAD;
constexpr u8 I64TruncF32S = 0xAE;
constexpr u8 I64TruncF32U = 0xAF;
constexpr u8 I64TruncF64S = 0xB0;
constexpr u8 I64TruncF64U = 0xB1;
constexpr u8 F32ConvertI32S = 0xB2;
constexpr u8 F32ConvertI32U = 0xB3;
constexpr u8 F32ConvertI64S = 0xB4;
constexpr u8 F32ConvertI64U = 0xB5;
constexpr u8 F32DemoteF64 = 0xB6;
constexpr u8 F64ConvertI32S = 0xB7;
constexpr u8 F64ConvertI32U = 0xB8;
constexpr u8 F64ConvertI64S = 0xB9;
constexpr u8 F64ConvertI64U = 0xBA;
constexpr u8 F64PromoteF32 = 0xBB;
constexpr u8 I32ReinterpretF32 = 0xBC;
constexpr u8 I64ReinterpretF64 = 0xBD;
constexpr u8 F32ReinterpretI32 = 0xBE;
constexpr u8 F64ReinterpretI64 = 0xBF;
constexpr u8 I32Extend8S = 0xC0;
constexpr u8 I32Extend16S = 0xC1;
constexpr u8 I64Extend8S = 0xC2;
constexpr u8 I64Extend16S = 0xC3;
constexpr u8 I64Extend32S = 0xC4;

// Prefixes
constexpr u8 PrefixMisc = 0xFC;   // saturating truncation, memory.*, table.*
constexpr u8 PrefixSimd = 0xFD;   // all v128 instructions

// Misc (0xFC) subopcodes
constexpr u32 I32TruncSatF32S = 0;
constexpr u32 I32TruncSatF32U = 1;
constexpr u32 I32TruncSatF64S = 2;
constexpr u32 I32TruncSatF64U = 3;
constexpr u32 I64TruncSatF32S = 4;
constexpr u32 I64TruncSatF32U = 5;
constexpr u32 I64TruncSatF64S = 6;
constexpr u32 I64TruncSatF64U = 7;
constexpr u32 MemoryInit = 8;
constexpr u32 DataDrop = 9;
constexpr u32 MemoryCopy = 10;   // two 0x00 bytes follow
constexpr u32 MemoryFill = 11;   // one 0x00 byte follows
constexpr u32 TableInit = 12;
constexpr u32 ElemDrop = 13;
constexpr u32 TableCopy = 14;
constexpr u32 TableGrow = 15;
constexpr u32 TableSize = 16;
constexpr u32 TableFill = 17;

// SIMD (0xFD) subopcodes used by the backend (subset)
constexpr u32 V128Load = 0;
constexpr u32 V128Store = 11;
constexpr u32 V128Const = 12;
constexpr u32 I8x16Shuffle = 13;
constexpr u32 I8x16Swizzle = 14;
constexpr u32 I8x16Splat = 15;
constexpr u32 I16x8Splat = 16;
constexpr u32 I32x4Splat = 17;
constexpr u32 I64x2Splat = 18;
constexpr u32 F32x4Splat = 19;
constexpr u32 F64x2Splat = 20;
constexpr u32 I8x16ExtractLaneS = 21;
constexpr u32 I8x16ExtractLaneU = 22;
constexpr u32 I8x16ReplaceLane = 23;
constexpr u32 I16x8ExtractLaneS = 24;
constexpr u32 I16x8ExtractLaneU = 25;
constexpr u32 I16x8ReplaceLane = 26;
constexpr u32 I32x4ExtractLane = 27;
constexpr u32 I32x4ReplaceLane = 28;
constexpr u32 I64x2ExtractLane = 29;
constexpr u32 I64x2ReplaceLane = 30;
constexpr u32 F32x4ExtractLane = 31;
constexpr u32 F32x4ReplaceLane = 32;
constexpr u32 F64x2ExtractLane = 33;
constexpr u32 F64x2ReplaceLane = 34;
constexpr u32 I8x16Eq = 35;
constexpr u32 I8x16Ne = 36;
constexpr u32 I8x16LtS = 37;
constexpr u32 I8x16LtU = 38;
constexpr u32 I8x16GtS = 39;
constexpr u32 I8x16GtU = 40;
constexpr u32 I8x16LeS = 41;
constexpr u32 I8x16LeU = 42;
constexpr u32 I8x16GeS = 43;
constexpr u32 I8x16GeU = 44;
constexpr u32 I16x8Eq = 45;
constexpr u32 I16x8GtS = 49;
constexpr u32 I16x8GtU = 50;
constexpr u32 I16x8LeS = 51;
constexpr u32 I16x8GeS = 53;
constexpr u32 I32x4Eq = 55;
constexpr u32 I32x4GtS = 59;
constexpr u32 I32x4GtU = 60;
constexpr u32 I32x4LeS = 61;
constexpr u32 I32x4GeS = 63;
constexpr u32 F32x4Eq = 65;
constexpr u32 F32x4Ne = 66;
constexpr u32 F32x4Lt = 67;
constexpr u32 F32x4Gt = 68;
constexpr u32 F32x4Le = 69;
constexpr u32 F32x4Ge = 70;
constexpr u32 F64x2Eq = 71;
constexpr u32 F64x2Ne = 72;
constexpr u32 F64x2Lt = 73;
constexpr u32 F64x2Gt = 74;
constexpr u32 F64x2Le = 75;
constexpr u32 F64x2Ge = 76;
constexpr u32 V128Not = 77;
constexpr u32 V128And = 78;
constexpr u32 V128AndNot = 79;
constexpr u32 V128Or = 80;
constexpr u32 V128Xor = 81;
constexpr u32 V128Bitselect = 82;
constexpr u32 V128AnyTrue = 83;
constexpr u32 V128Load32Zero = 92;
constexpr u32 V128Load64Zero = 93;
constexpr u32 F32x4DemoteF64x2Zero = 94;
constexpr u32 F64x2PromoteLowF32x4 = 95;
constexpr u32 I8x16Abs = 96;
constexpr u32 I8x16Neg = 97;
constexpr u32 I8x16Popcnt = 98;
constexpr u32 I8x16AllTrue = 99;
constexpr u32 I8x16Bitmask = 100;
constexpr u32 I8x16NarrowI16x8S = 101;
constexpr u32 I8x16NarrowI16x8U = 102;
constexpr u32 F32x4Ceil = 103;
constexpr u32 F32x4Floor = 104;
constexpr u32 F32x4Trunc = 105;
constexpr u32 F32x4Nearest = 106;
constexpr u32 I8x16Shl = 107;
constexpr u32 I8x16ShrS = 108;
constexpr u32 I8x16ShrU = 109;
constexpr u32 I8x16Add = 110;
constexpr u32 I8x16AddSatS = 111;
constexpr u32 I8x16AddSatU = 112;
constexpr u32 I8x16Sub = 113;
constexpr u32 I8x16SubSatS = 114;
constexpr u32 I8x16SubSatU = 115;
constexpr u32 F64x2Ceil = 116;
constexpr u32 F64x2Floor = 117;
constexpr u32 I8x16MinS = 118;
constexpr u32 I8x16MinU = 119;
constexpr u32 I8x16MaxS = 120;
constexpr u32 I8x16MaxU = 121;
constexpr u32 F64x2Trunc = 122;
constexpr u32 I8x16AvgrU = 123;
constexpr u32 I16x8ExtAddPairwiseI8x16S = 124;
constexpr u32 I16x8ExtAddPairwiseI8x16U = 125;
constexpr u32 I32x4ExtAddPairwiseI16x8S = 126;
constexpr u32 I32x4ExtAddPairwiseI16x8U = 127;
constexpr u32 I16x8Abs = 128;
constexpr u32 I16x8Neg = 129;
constexpr u32 I16x8Q15MulrSatS = 130;
constexpr u32 I16x8NarrowI32x4S = 133;
constexpr u32 I16x8NarrowI32x4U = 134;
constexpr u32 I16x8ExtendLowI8x16S = 135;
constexpr u32 I16x8ExtendHighI8x16S = 136;
constexpr u32 I16x8ExtendLowI8x16U = 137;
constexpr u32 I16x8ExtendHighI8x16U = 138;
constexpr u32 I16x8Shl = 139;
constexpr u32 I16x8ShrS = 140;
constexpr u32 I16x8ShrU = 141;
constexpr u32 I16x8Add = 142;
constexpr u32 I16x8AddSatS = 143;
constexpr u32 I16x8AddSatU = 144;
constexpr u32 I16x8Sub = 145;
constexpr u32 I16x8SubSatS = 146;
constexpr u32 I16x8SubSatU = 147;
constexpr u32 F64x2Nearest = 148;
constexpr u32 I16x8Mul = 149;
constexpr u32 I16x8MinS = 150;
constexpr u32 I16x8MinU = 151;
constexpr u32 I16x8MaxS = 152;
constexpr u32 I16x8MaxU = 153;
constexpr u32 I16x8AvgrU = 155;
constexpr u32 I16x8ExtMulLowI8x16S = 156;
constexpr u32 I16x8ExtMulHighI8x16S = 157;
constexpr u32 I16x8ExtMulLowI8x16U = 158;
constexpr u32 I16x8ExtMulHighI8x16U = 159;
constexpr u32 I32x4Abs = 160;
constexpr u32 I32x4Neg = 161;
constexpr u32 I32x4ExtendLowI16x8S = 167;
constexpr u32 I32x4ExtendHighI16x8S = 168;
constexpr u32 I32x4ExtendLowI16x8U = 169;
constexpr u32 I32x4ExtendHighI16x8U = 170;
constexpr u32 I32x4Shl = 171;
constexpr u32 I32x4ShrS = 172;
constexpr u32 I32x4ShrU = 173;
constexpr u32 I32x4Add = 174;
constexpr u32 I32x4Sub = 177;
constexpr u32 I32x4Mul = 181;
constexpr u32 I32x4MinS = 182;
constexpr u32 I32x4MinU = 183;
constexpr u32 I32x4MaxS = 184;
constexpr u32 I32x4MaxU = 185;
constexpr u32 I32x4ExtMulLowI16x8S = 188;
constexpr u32 I32x4ExtMulHighI16x8S = 189;
constexpr u32 I32x4ExtMulLowI16x8U = 190;
constexpr u32 I32x4ExtMulHighI16x8U = 191;
constexpr u32 I64x2Abs = 192;
constexpr u32 I64x2Neg = 193;
constexpr u32 I64x2ExtendLowI32x4S = 199;
constexpr u32 I64x2ExtendHighI32x4S = 200;
constexpr u32 I64x2ExtendLowI32x4U = 201;
constexpr u32 I64x2ExtendHighI32x4U = 202;
constexpr u32 I64x2Shl = 203;
constexpr u32 I64x2ShrS = 204;
constexpr u32 I64x2ShrU = 205;
constexpr u32 I64x2Add = 206;
constexpr u32 I64x2Sub = 209;
constexpr u32 I64x2Mul = 213;
constexpr u32 I64x2Eq = 214;
constexpr u32 I64x2Ne = 215;
constexpr u32 I64x2LtS = 216;
constexpr u32 I64x2GtS = 217;
constexpr u32 I64x2LeS = 218;
constexpr u32 I64x2GeS = 219;
constexpr u32 I64x2ExtMulLowI32x4S = 220;
constexpr u32 I64x2ExtMulHighI32x4S = 221;
constexpr u32 I64x2ExtMulLowI32x4U = 222;
constexpr u32 I64x2ExtMulHighI32x4U = 223;
constexpr u32 F32x4Abs = 224;
constexpr u32 F32x4Neg = 225;
constexpr u32 F32x4Sqrt = 227;
constexpr u32 F32x4Add = 228;
constexpr u32 F32x4Sub = 229;
constexpr u32 F32x4Mul = 230;
constexpr u32 F32x4Div = 231;
constexpr u32 F32x4Min = 232;
constexpr u32 F32x4Max = 233;
constexpr u32 F32x4PMin = 234;
constexpr u32 F32x4PMax = 235;
constexpr u32 F64x2Abs = 236;
constexpr u32 F64x2Neg = 237;
constexpr u32 F64x2Sqrt = 239;
constexpr u32 F64x2Add = 240;
constexpr u32 F64x2Sub = 241;
constexpr u32 F64x2Mul = 242;
constexpr u32 F64x2Div = 243;
constexpr u32 F64x2Min = 244;
constexpr u32 F64x2Max = 245;
constexpr u32 F64x2PMin = 246;
constexpr u32 F64x2PMax = 247;
constexpr u32 I32x4TruncSatF32x4S = 248;
constexpr u32 I32x4TruncSatF32x4U = 249;
constexpr u32 F32x4ConvertI32x4S = 250;
constexpr u32 F32x4ConvertI32x4U = 251;
constexpr u32 I32x4TruncSatF64x2SZero = 252;
constexpr u32 I32x4TruncSatF64x2UZero = 253;
constexpr u32 F64x2ConvertLowI32x4S = 254;
constexpr u32 F64x2ConvertLowI32x4U = 255;

}  // namespace Op

// Block type for block/loop/if: empty = 0x40, single valtype, or SLEB type index
struct BlockType {
    std::vector<ValType> results;  // empty or single-element for now
};

// ---------------------------------------------------------------------------
// Code body emitter: builds a function body (expr bytes) plus local decls
// ---------------------------------------------------------------------------

class FuncBody {
public:
    explicit FuncBody(u32 param_count = 0)
            : param_count(param_count) {}

    // Declare `count` locals of `type`. Returns the absolute index of the
    // first (i.e. including parameters). Locals are coalesced into runs.
    u32 AddLocals(u32 count, ValType type);

    const std::vector<u8>& Body() const { return body; }
    std::vector<u8>& Body() { return body; }

    // Emit helpers
    void U8(u8 v) { body.push_back(v); }
    void ULEB(u64 v) { EncodeULEB(body, v); }
    void SLEB(s64 v) { EncodeSLEB(body, v); }
    void Bytes(const void* data, size_t size);

    void I32Const(s32 v) { U8(Op::I32Const); SLEB(v); }
    void I64Const(s64 v) { U8(Op::I64Const); SLEB(v); }
    void F32Const(u32 bits) { U8(Op::F32Const); Raw32(bits); }
    void F64Const(u64 bits) { U8(Op::F64Const); Raw64(bits); }

    void LocalGet(u32 i) { U8(Op::LocalGet); ULEB(i); }
    void LocalSet(u32 i) { U8(Op::LocalSet); ULEB(i); }
    void LocalTee(u32 i) { U8(Op::LocalTee); ULEB(i); }
    void GlobalGet(u32 i) { U8(Op::GlobalGet); ULEB(i); }
    void GlobalSet(u32 i) { U8(Op::GlobalSet); ULEB(i); }

    void Load(u8 op, u32 offset, u32 align_log2 = 0) { U8(op); ULEB(align_log2); ULEB(offset); }
    void Store(u8 op, u32 offset, u32 align_log2 = 0) { U8(op); ULEB(align_log2); ULEB(offset); }

    // Optional hooks wrapped around call / return instructions. They must emit
    // stack-neutral code (the call's operands / result are on the stack); the
    // emitter uses them to flush and reload guest registers that are cached in
    // locals across a self-loop. Hooks never see calls they emit themselves.
    std::function<void(u32 func)> before_call;
    std::function<void(u32 func)> after_call;
    std::function<void()> before_exit;  // return / return_call_indirect

    void Call(u32 func) {
        if (before_call) before_call(func);
        U8(Op::Call);
        ULEB(func);
        if (after_call) after_call(func);
    }
    void CallIndirect(u32 type, u32 table = 0) {
        if (before_call) before_call(~0u);
        U8(Op::CallIndirect);
        ULEB(type);
        ULEB(table);
        if (after_call) after_call(~0u);
    }
    void ReturnCallIndirect(u32 type, u32 table = 0) {
        if (before_exit) before_exit();
        U8(Op::ReturnCallIndirect);
        ULEB(type);
        ULEB(table);
    }

    // Structured-control nesting depth (number of open block/loop/if frames),
    // used to compute relative branch depths to an enclosing loop.
    u32 Depth() const { return depth; }

    void Block(const BlockType& bt = {}) { U8(Op::Block); EmitBlockType(bt); depth++; }
    void Loop(const BlockType& bt = {}) { U8(Op::Loop); EmitBlockType(bt); depth++; }
    void If(const BlockType& bt = {}) { U8(Op::If); EmitBlockType(bt); depth++; }
    void Else() { U8(Op::Else); }
    void End() { U8(Op::End); depth--; }
    void Br(u32 label) { U8(Op::Br); ULEB(label); }
    void BrIf(u32 label) { U8(Op::BrIf); ULEB(label); }
    void BrTable(const std::vector<u32>& targets, u32 default_target);
    void Return() {
        if (before_exit) before_exit();
        U8(Op::Return);
    }

    void Simd(u32 subop) { U8(Op::PrefixSimd); ULEB(subop); }
    void Misc(u32 subop) { U8(Op::PrefixMisc); ULEB(subop); }

    void Raw32(u32 v);
    void Raw64(u64 v);

    // Serializes: local decl vec + body + trailing End
    std::vector<u8> Serialize() const;

private:
    void EmitBlockType(const BlockType& bt);

    u32 param_count;
    u32 depth = 0;
    std::vector<u8> body;
    // Local declarations as (count, type) runs
    std::vector<std::pair<u32, ValType>> local_runs;
};

// ---------------------------------------------------------------------------
// Module builder
// ---------------------------------------------------------------------------

struct ImportFunc {
    std::string module;
    std::string name;
    u32 type_index;
};

class ModuleBuilder {
public:
    // Types (deduplicated). Returns type index.
    u32 AddType(const FuncType& type);

    // Imports. Function imports occupy the low function indices.
    u32 ImportFunc(std::string module, std::string name, u32 type_index);
    u32 ImportMemory(std::string module, std::string name, const Limits& limits);
    u32 ImportTable(std::string module, std::string name, ValType elem_type, const Limits& limits);
    u32 ImportGlobal(std::string module, std::string name, ValType type, bool mutable_);

    // Definitions
    u32 AddFunction(u32 type_index);  // returns function index (imports first)
    u32 AddMemory(const Limits& limits);
    u32 AddTable(ValType elem_type, const Limits& limits);
    u32 AddGlobal(ValType type, bool mutable_, const std::vector<u8>& init_expr);

    void AddExport(std::string name, ExternalKind kind, u32 index);

    // Declarative element segment declaring a function index (for ref.func).
    void DeclareFuncElem(u32 func_index);
    // Active element segment: fills table `table_index` at `offset_expr` with funcs.
    void AddActiveElem(u32 table_index, u32 offset, const std::vector<u32>& func_indices);

    // Returns the stored serialized body size, including locals and End.
    size_t SetCode(u32 func_index, const FuncBody& body);

    // Emits a "compilationHints" custom section applying `hint` to every
    // defined function. V8 byte layout: bits 0-1 strategy (2 = eager),
    // bits 2-3 baseline tier / bits 4-5 top tier (2 = optimized). 0x2A skips
    // Liftoff and compiles with TurboFan up front -- needed for hot loops,
    // since wasm has no OSR and a loop entered once never tiers up.
    void SetCompilationHint(u8 hint) { compilation_hint = hint; }

    // Names a defined function in a "name" custom section (shown by engine
    // profilers and stack traces).
    void SetFunctionName(u32 func_index, std::string name) { function_names.emplace_back(func_index, std::move(name)); }

    std::vector<u8> Finalize() const;

    u32 NumImportedFuncs() const { return num_imported_funcs; }

private:
    struct FuncImport { std::string module, name; u32 type; };
    struct MemImport { std::string module, name; Limits limits; };
    struct TableImport { std::string module, name; ValType elem; Limits limits; };
    struct GlobalImport { std::string module, name; ValType type; bool mut; };
    struct Export { std::string name; ExternalKind kind; u32 index; };
    struct CodeEntry { std::vector<u8> bytes; };

    std::vector<FuncType> types;
    std::vector<FuncImport> func_imports;
    std::vector<MemImport> mem_imports;
    std::vector<TableImport> table_imports;
    std::vector<GlobalImport> global_imports;
    std::vector<u32> funcs;  // type indices of defined functions
    std::vector<Limits> memories;
    std::vector<std::pair<ValType, Limits>> tables;
    struct GlobalDef { ValType type; bool mut; std::vector<u8> init; };
    std::vector<GlobalDef> globals;
    std::vector<Export> exports;
    std::vector<u32> declarative_elems;
    struct ActiveElem { u32 table; u32 offset; std::vector<u32> funcs; };
    std::vector<ActiveElem> active_elems;
    std::vector<CodeEntry> codes;  // parallel to funcs
    u32 num_imported_funcs = 0;
    std::optional<u8> compilation_hint;
    std::vector<std::pair<u32, std::string>> function_names;
};

}  // namespace Dynarmic::Backend::Wasm
