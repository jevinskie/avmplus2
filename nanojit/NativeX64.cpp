/* -*- Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 4 -*- */
/* vi: set ts=4 sw=4 expandtab: (add to ~/.vimrc: set modeline modelines=5) */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nanojit.h"

// uncomment this to enable _vprof/_nvprof macros
//#define DOPROF
#include "../vprof/vprof.h"

#if defined FEATURE_NANOJIT && defined NANOJIT_X64

/*
completion
- 64bit branch offsets
- finish cmov/qcmov with other conditions
- validate asm_cond with other conditions

better code
- put R12 back in play as a base register
- no-disp addr modes (except RBP/R13)
- disp64 branch/call
- spill gp values to xmm registers?
- prefer xmm registers for copies since gprs are in higher demand?
- stack based LIR_paramp

tracing
- nFragExit

*/

namespace nanojit
{
#ifdef _WIN64
    const Register RegAlloc::argRegs[] = { RCX, RDX, R8, R9 };
    const static int maxArgRegs = 4;
    const Register RegAlloc::savedRegs[] = { RBX, RSI, RDI, R12, R13, R14, R15 };
#else
    const Register RegAlloc::argRegs[] = { RDI, RSI, RDX, RCX, R8, R9 };
    const static int maxArgRegs = 6;
    const Register RegAlloc::savedRegs[] = { RBX, R12, R13, R14, R15 };
#endif

    const char *regNames[] = {
        "rax",  "rcx",  "rdx",   "rbx",   "rsp",   "rbp",   "rsi",   "rdi",
        "r8",   "r9",   "r10",   "r11",   "r12",   "r13",   "r14",   "r15",
        "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
    };

    const char *gpRegNames32[] = {
        "eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
    };

    const char *gpRegNames8[] = {
        "al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
        "r8l", "r9l", "r10l", "r11l", "r12l", "r13l", "r14l", "r15l"
    };

    const char *gpRegNames8hi[] = {
        "ah", "ch", "dh", "bh"
    };

    const char *gpRegNames16[] = {
        "ax",  "cx",  "dx",   "bx",   "spx",  "bpx",  "six",  "dix",
        "r8x", "r9x", "r10x", "r11x", "r12x", "r13x", "r14x", "r15x"
    };

#ifdef _DEBUG
    #define TODO(x) todo(#x)
    static void todo(const char *s) {
        verbose_only( avmplus::AvmLog("%s",s); )
        NanoAssertMsgf(false, "%s", s);
    }
#else
    #define TODO(x)
#endif


    // MODRM and SIB restrictions:
    // memory access modes != 11 require SIB if base&7 == 4 (RSP or R12)
    // mode 00 with base == x101 means RIP+disp32 (RBP or R13), use mode 01 disp8=0 instead
    // mode 01 or 11 with base = x101 means disp32 + EBP or R13, not RIP relative
    // base == x100 means SIB byte is present, so using ESP|R12 as base requires SIB
    // rex prefix required to use RSP-R15 as 8bit registers in mod/rm8 modes.

    // take R12 out of play as a base register because using ESP or R12 as base requires the SIB byte
    const RegisterMask BaseRegs = GpRegs & ~rmask(R12);

    static inline int oplen(uint64_t op) {
        return op & 255;
    }

    // encode 2-register rex prefix.  dropped if none of its bits are set.
    static inline uint64_t rexrb(uint64_t op, Register r, Register b) {
        int shift = 64 - 8*oplen(op);
        NanoAssert( (((op>>shift)&255) == 0x40) || 
                    (((op>>shift)&255) == 0x48) ); // Make sure rexrb is properly used
        uint64_t rex = ((op >> shift) & 255) | ((REGNUM(r)&8)>>1) | ((REGNUM(b)&8)>>3);
        return rex != 0x40 ? op | rex << shift : op - 1;
    }

    // encode 3-register rex prefix.  dropped if none of its bits are set.
    static inline uint64_t rexrxb(uint64_t op, Register r, Register x, Register b) {
        int shift = 64 - 8*oplen(op);
        uint64_t rex = ((op >> shift) & 255) | ((REGNUM(r)&8)>>1) | ((REGNUM(x)&8)>>2) | ((REGNUM(b)&8)>>3);
        return rex != 0x40 ? op | rex << shift : op - 1;
    }

    // encode 2-register rex prefix.  dropped if none of its bits are set, but
    // keep REX if b >= rsp, to allow uniform use of all 16 8bit registers
    static inline uint64_t rexrb8(uint64_t op, Register r, Register b) {
        int shift = 64 - 8*oplen(op);
        uint64_t rex = ((op >> shift) & 255) | ((REGNUM(r)&8)>>1) | ((REGNUM(b)&8)>>3);
        return ((rex | (REGNUM(b) & ~3)) != 0x40) ? (op | (rex << shift)) : op - 1;
    }

    // encode 2-register rex prefix that follows a manditory prefix (66,F2,F3)
    // [prefix][rex][opcode]
    static inline uint64_t rexprb(uint64_t op, Register r, Register b) {
        int shift = 64 - 8*oplen(op) + 8;
#ifdef _DEBUG
        int mandatory_prefix = (op >> (shift-8)) & 255;
        NanoAssert(mandatory_prefix!=0);
        NanoAssert(mandatory_prefix==0x66 || mandatory_prefix == 0xF2
                   || mandatory_prefix == 0xF3 );
#endif
        uint64_t rex = ((op >> shift) & 255) | ((REGNUM(r)&8)>>1) | ((REGNUM(b)&8)>>3);
        // to drop rex, we replace rex with manditory prefix, and decrement length
        return rex != 0x40 ? op | rex << shift :
            ((op & ~(255LL<<shift)) | (op>>(shift-8)&255) << shift) - 1;
    }

    // [rex][opcode][mod-rr]
    static inline uint64_t mod_rr(uint64_t op, Register r, Register b) {
        return op | uint64_t((REGNUM(r)&7)<<3 | (REGNUM(b)&7))<<56;
    }

    // [rex][opcode][modrm=r][sib=xb]
    static inline uint64_t mod_rxb(uint64_t op, Register r, Register x, Register b) {
        return op | /*modrm*/uint64_t((REGNUM(r)&7)<<3)<<48 | /*sib*/uint64_t((REGNUM(x)&7)<<3|(REGNUM(b)&7))<<56;
    }

    static inline uint64_t mod_disp32(uint64_t op, Register r, Register b, int32_t d) {
        NanoAssert(IsGpReg(r) && IsGpReg(b));
        NanoAssert((REGNUM(b) & 7) != 4); // using RSP or R12 as base requires SIB
        uint64_t mod = (((op>>24)&255)>>6); // mod bits in addressing mode: 0,1,2, or 3
        if (mod == 2 && isS8(d)) {
            // op is:  0x[disp32=0][mod=2:r:b][op][rex][len]
            int len = oplen(op);
            op = (op & ~0xff000000LL) | (0x40 | (REGNUM(r)&7)<<3 | (REGNUM(b)&7))<<24; // replace mod
            return op<<24 | int64_t(d)<<56 | (len-3); // shrink disp, add disp8
        } else {
            // op is: 0x[disp32][mod][op][rex][len]
            return op | int64_t(d)<<32 | uint64_t((REGNUM(r)&7)<<3 | (REGNUM(b)&7))<<24;
        }
    }

    // All the emit() functions should only be called from within codegen
    // functions PUSHR(), SHR(), etc.

    void Assembler::emit(uint64_t op) {
        int len = oplen(op);
        // we will only move nIns by -len bytes, but we write 8
        // bytes.  so need to protect 8 so we dont stomp the page
        // header or the end of the preceding page (might segf)
        underrunProtect(8);
        ((int64_t*)_nIns)[-1] = op;
        _nIns -= len; // move pointer by length encoded in opcode
        _nvprof("x64-bytes", len);
    }

    void Assembler::emit8(uint64_t op, int64_t v) {
        NanoAssert(isS8(v));
        emit(op | uint64_t(v)<<56);
    }

    void Assembler::emit_target8(size_t underrun, uint64_t op, NIns* target) {
        underrunProtect(underrun); // must do this before calculating offset
        // Nb: see emit_target32() for why we use _nIns here.
        int64_t offset = target - _nIns;
        NanoAssert(isS8(offset));
        emit(op | uint64_t(offset)<<56);
    }

    void Assembler::emit_target32(size_t underrun, uint64_t op, NIns* target) {
        underrunProtect(underrun); // must do this before calculating offset
        // Nb: at this point in time, _nIns points to the most recently
        // written instruction, ie. the jump's successor.  So why do we use it
        // to compute the offset, rather than the jump's address?  Because in
        // x86/x64-64 the offset in a relative jump is not from the jmp itself
        // but from the following instruction.  Eg. 'jmp $0' will jump to the
        // next instruction.
        int64_t offset = target ? target - _nIns : 0;
        if (!isS32(offset))
            setError(BranchTooFar);
        emit(op | uint64_t(uint32_t(offset))<<32);
    }

    void Assembler::emit_target64(size_t underrun, uint64_t op, NIns* target) {
        NanoAssert(underrun >= 16);
        underrunProtect(underrun); // must do this before calculating offset
        // Nb: at this point in time, _nIns points to the most recently
        // written instruction, ie. the jump's successor.
        ((uint64_t*)_nIns)[-1] = (uint64_t) target;
        _nIns -= 8;
        emit(op);
    }

    // 3-register modrm32+sib form
    void Assembler::emitrxb(uint64_t op, Register r, Register x, Register b) {
        emit(rexrxb(mod_rxb(op, r, x, b), r, x, b));
    }

    // 2-register modrm32 form
    void Assembler::emitrr(uint64_t op, Register r, Register b) {
        emit(rexrb(mod_rr(op, r, b), r, b));
    }

    // 2-register modrm8 form (8 bit operand size)
    void Assembler::emitrr8(uint64_t op, Register r, Register b) {
        emit(rexrb8(mod_rr(op, r, b), r, b));
    }

    // same as emitrr, but with a prefix byte
    void Assembler::emitprr(uint64_t op, Register r, Register b) {
        emit(rexprb(mod_rr(op, r, b), r, b));
    }

    // disp32 modrm8 form, when the disp fits in the instruction (opcode is 1-3 bytes)
    void Assembler::emitrm8(uint64_t op, Register r, int32_t d, Register b) {
        emit(rexrb8(mod_disp32(op, r, b, d), r, b));
    }

    // disp32 modrm form, when the disp fits in the instruction (opcode is 1-3 bytes)
    void Assembler::emitrm(uint64_t op, Register r, int32_t d, Register b) {
        emit(rexrb(mod_disp32(op, r, b, d), r, b));
    }

    // disp32 modrm form when the disp must be written separately (opcode is 4+ bytes)
#define emit_disp32_sib(o,d) (emit_disp32( ((o)&0x00FFFFFFFFFFFFFFLL)<<8, d) >>8) | ((o)&0xFF00000000000000LL)
    uint64_t Assembler::emit_disp32(uint64_t op, int32_t d) {
        if (isS8(d)) {
            NanoAssert(((op>>56)&0xC0) == 0x80); // make sure mod bits == 2 == disp32 mode
            underrunProtect(1+8);
            *(--_nIns) = (NIns) d;
            _nvprof("x64-bytes", 1);
            op ^= 0xC000000000000000LL; // change mod bits to 1 == disp8 mode
        } else {
            underrunProtect(4+8); // room for displ plus fullsize op
            *((int32_t*)(_nIns -= 4)) = d;
            _nvprof("x64-bytes", 4);
        }
        return op;
    }

    // disp32 modrm form when the disp must be written separately (opcode is 4+ bytes)
    void Assembler::emitrm_wide(uint64_t op, Register r, int32_t d, Register b) {
        op = emit_disp32(op, d);
        emitrr(op, r, b);
    }

    // disp32 modrm form when the disp must be written separately (opcode is 4+ bytes)
    // p = prefix -- opcode must have a 66, F2, or F3 prefix
    void Assembler::emitprm(uint64_t op, Register r, int32_t d, Register b) {
        op = emit_disp32(op, d);
        emitprr(op, r, b);
    }

    // disp32 modrm form with 32-bit immediate value
    void Assembler::emitrm_imm32(uint64_t op, Register b, int32_t d, int32_t imm) {
        NanoAssert(IsGpReg(b));
        NanoAssert((REGNUM(b) & 7) != 4); // using RSP or R12 as base requires SIB
        underrunProtect(4+4+8); // room for imm plus disp plus fullsize op
        *((int32_t*)(_nIns -= 4)) = imm;
        _nvprof("x86-bytes", 4);
        emitrm_wide(op, RZero, d, b);
    }

    // disp32 modrm form with 16-bit immediate value
    // p = prefix -- opcode must have a 66, F2, or F3 prefix
    void Assembler::emitprm_imm16(uint64_t op, Register b, int32_t d, int32_t imm) {
        NanoAssert(IsGpReg(b));
        NanoAssert((REGNUM(b) & 7) != 4); // using RSP or R12 as base requires SIB
        underrunProtect(2+4+8); // room for imm plus disp plus fullsize op
        *((int16_t*)(_nIns -= 2)) = (int16_t) imm;
        _nvprof("x86-bytes", 2);
        emitprm(op, RZero, d, b);
    }

    // disp32 modrm form with 8-bit immediate value
    void Assembler::emitrm_imm8(uint64_t op, Register b, int32_t d, int32_t imm) {
        NanoAssert(IsGpReg(b));
        NanoAssert((REGNUM(b) & 7) != 4); // using RSP or R12 as base requires SIB
        underrunProtect(1+4+8); // room for imm plus disp plus fullsize op
        *((int8_t*)(_nIns -= 1)) = (int8_t) imm;
        _nvprof("x86-bytes", 1);
        emitrm_wide(op, RZero, d, b);
    }

    void Assembler::emitrr_imm(uint64_t op, Register r, Register b, int32_t imm) {
        NanoAssert(IsGpReg(r) && IsGpReg(b));
        underrunProtect(4+8); // room for imm plus fullsize op
        *((int32_t*)(_nIns -= 4)) = imm;
        _nvprof("x86-bytes", 4);
        emitrr(op, r, b);
    }

    void Assembler::emitrr_imm8(uint64_t op, Register r, Register b, uint8_t imm) {
        NanoAssert(IsFpReg(r) && IsFpReg(b));
        underrunProtect(1+8); // room for imm plus fullsize op
        *((uint8_t*)(_nIns -= 1)) = imm;
        _nvprof("x86-bytes", 1);
        emitrr(op, r, b);
    }

    void Assembler::emitprr_imm8(uint64_t op, Register r, Register b, uint8_t imm) {
        NanoAssert((IsGpReg(r) && IsGpReg(b)) || (IsFpReg(r) && IsFpReg(b)));
        underrunProtect(1+8); // room for imm plus fullsize op
        *((uint8_t*)(_nIns -= 1)) = imm;
        _nvprof("x86-bytes", 1);
        emitprr(op, r, b);
    }

    void Assembler::emitr_imm64(uint64_t op, Register r, uint64_t imm64) {
        underrunProtect(8+8); // imm64 + worst case instr len
        *((uint64_t*)(_nIns -= 8)) = imm64;
        _nvprof("x64-bytes", 8);
        emitr(op, r);
    }

    void Assembler::emitrxb_imm(uint64_t op, Register r, Register x, Register b, int32_t imm) {
        NanoAssert(IsGpReg(r) && IsGpReg(x) && IsGpReg(b));
        underrunProtect(4+8); // room for imm plus fullsize op
        *((int32_t*)(_nIns -= 4)) = imm;
        _nvprof("x86-bytes", 4);
        emitrxb(op, r, x, b);
    }

    // op = [rex][opcode][modrm][imm8]
    void Assembler::emitr_imm8(uint64_t op, Register b, int32_t imm8) {
        NanoAssert(IsGpReg(b) && isS8(imm8));
        op |= uint64_t(imm8)<<56 | uint64_t(REGNUM(b)&7)<<48;  // modrm is 2nd to last byte
        emit(rexrb(op, RZero, b));
    }


    void Assembler::emitxm_abs(uint64_t op, Register r, int32_t addr32)
    {
        underrunProtect(4+8);
        *((int32_t*)(_nIns -= 4)) = addr32;
        _nvprof("x64-bytes", 4);
        op = op | uint64_t((REGNUM(r)&7)<<3)<<48; // put rr[0:2] into mod/rm byte
        op = rexrb(op, r, RZero);         // put rr[3] into rex byte
        emit(op);
    }

    void Assembler::emitxm_rel(uint64_t op, Register r, NIns* addr64)
    {
        underrunProtect(4+8);
        int32_t d = (int32_t)(addr64 - _nIns);
        *((int32_t*)(_nIns -= 4)) = d;
        _nvprof("x64-bytes", 4);
        emitrr(op, r, RZero);
    }

    // Succeeds if 'target' is within a signed 8-bit offset from the current
    // instruction's address.
    bool Assembler::isTargetWithinS8(NIns* target)
    {
        NanoAssert(target);
        // First call underrunProtect().  Without it, we might compute the
        // difference just before starting a new code chunk.
        underrunProtect(8);
        if (_config.force_long_branch)
            return false;
        return isS8(target - _nIns);
    }

    // Like isTargetWithinS8(), but for signed 32-bit offsets.
    bool Assembler::isTargetWithinS32(NIns* target,int32_t maxInstSize)
    {
        NanoAssert(target);
        // some instructions with S32 offsets take more than 8 bytes (e.g. packed float loads like movaps/movups)
        underrunProtect(maxInstSize);
        if (_config.force_long_branch)
            return false;
        return isS32(target - _nIns);
    }

#define RB(r)       gpRegNames8[(REGNUM(r))]
#define RS(r)       gpRegNames16[(REGNUM(r))]
#define RBhi(r)     gpRegNames8hi[(REGNUM(r))]
#define RL(r)       gpRegNames32[(REGNUM(r))]
#define RQ(r)       gpn(r)

    typedef Register R;
    typedef int      I;
    typedef int32_t  I32;
    typedef uint64_t U64;
    typedef size_t   S;

    void Assembler::PUSHR(R r)  { emitr(X64_pushr,r); asm_output("push %s", RQ(r)); }
    void Assembler::POPR( R r)  { emitr(X64_popr, r); asm_output("pop %s",  RQ(r)); }
    void Assembler::NOT(  R r)  { emitr(X64_not,  r); asm_output("notl %s", RL(r)); }
    void Assembler::NEG(  R r)  { emitr(X64_neg,  r); asm_output("negl %s", RL(r)); }
    void Assembler::IDIV( R r)  { emitr(X64_idiv, r); asm_output("idivl edx:eax, %s",RL(r)); }

    void Assembler::SHR( R r)   { emitr(X64_shr,  r); asm_output("shrl %s, ecx", RL(r)); }
    void Assembler::SAR( R r)   { emitr(X64_sar,  r); asm_output("sarl %s, ecx", RL(r)); }
    void Assembler::SHL( R r)   { emitr(X64_shl,  r); asm_output("shll %s, ecx", RL(r)); }
    void Assembler::SHRQ(R r)   { emitr(X64_shrq, r); asm_output("shrq %s, ecx", RQ(r)); }
    void Assembler::SARQ(R r)   { emitr(X64_sarq, r); asm_output("sarq %s, ecx", RQ(r)); }
    void Assembler::SHLQ(R r)   { emitr(X64_shlq, r); asm_output("shlq %s, ecx", RQ(r)); }

    void Assembler::SHRI( R r, I i)   { emit8(rexrb(X64_shri  | U64(REGNUM(r)&7)<<48, RZero, r), i); asm_output("shrl %s, %d", RL(r), i); }
    void Assembler::SARI( R r, I i)   { emit8(rexrb(X64_sari  | U64(REGNUM(r)&7)<<48, RZero, r), i); asm_output("sarl %s, %d", RL(r), i); }
    void Assembler::SHLI( R r, I i)   { emit8(rexrb(X64_shli  | U64(REGNUM(r)&7)<<48, RZero, r), i); asm_output("shll %s, %d", RL(r), i); }
    void Assembler::SHRQI(R r, I i)   { emit8(rexrb(X64_shrqi | U64(REGNUM(r)&7)<<48, RZero, r), i); asm_output("shrq %s, %d", RQ(r), i); }
    void Assembler::SARQI(R r, I i)   { emit8(rexrb(X64_sarqi | U64(REGNUM(r)&7)<<48, RZero, r), i); asm_output("sarq %s, %d", RQ(r), i); }
    void Assembler::SHLQI(R r, I i)   { emit8(rexrb(X64_shlqi | U64(REGNUM(r)&7)<<48, RZero, r), i); asm_output("shlq %s, %d", RQ(r), i); }

    void Assembler::SETE( R r)  { emitr8(X64_sete, r); asm_output("sete %s", RB(r)); }
    void Assembler::SETL( R r)  { emitr8(X64_setl, r); asm_output("setl %s", RB(r)); }
    void Assembler::SETLE(R r)  { emitr8(X64_setle,r); asm_output("setle %s",RB(r)); }
    void Assembler::SETG( R r)  { emitr8(X64_setg, r); asm_output("setg %s", RB(r)); }
    void Assembler::SETGE(R r)  { emitr8(X64_setge,r); asm_output("setge %s",RB(r)); }
    void Assembler::SETB( R r)  { emitr8(X64_setb, r); asm_output("setb %s", RB(r)); }
    void Assembler::SETBE(R r)  { emitr8(X64_setbe,r); asm_output("setbe %s",RB(r)); }
    void Assembler::SETA( R r)  { emitr8(X64_seta, r); asm_output("seta %s", RB(r)); }
    void Assembler::SETAE(R r)  { emitr8(X64_setae,r); asm_output("setae %s",RB(r)); }
    void Assembler::SETO( R r)  { emitr8(X64_seto, r); asm_output("seto %s", RB(r)); }

    void Assembler::ADDRR(R l, R r)     { emitrr(X64_addrr,l,r); asm_output("addl %s, %s", RL(l),RL(r)); }
    void Assembler::SUBRR(R l, R r)     { emitrr(X64_subrr,l,r); asm_output("subl %s, %s", RL(l),RL(r)); }
    void Assembler::ANDRR(R l, R r)     { emitrr(X64_andrr,l,r); asm_output("andl %s, %s", RL(l),RL(r)); }
    void Assembler::ORLRR(R l, R r)     { emitrr(X64_orlrr,l,r); asm_output("orl %s, %s",  RL(l),RL(r)); }
    void Assembler::XORRR(R l, R r)     { emitrr(X64_xorrr,l,r); asm_output("xorl %s, %s", RL(l),RL(r)); }
    void Assembler::IMUL( R l, R r)     { emitrr(X64_imul, l,r); asm_output("imull %s, %s",RL(l),RL(r)); }
    void Assembler::CMPLR(R l, R r)     { emitrr(X64_cmplr,l,r); asm_output("cmpl %s, %s", RL(l),RL(r)); }
    void Assembler::MOVLR(R l, R r)     { emitrr(X64_movlr,l,r); asm_output("movl %s, %s", RL(l),RL(r)); }

    void Assembler::ADDQRR( R l, R r)   { emitrr(X64_addqrr, l,r); asm_output("addq %s, %s",  RQ(l),RQ(r)); }
    void Assembler::SUBQRR( R l, R r)   { emitrr(X64_subqrr, l,r); asm_output("subq %s, %s",  RQ(l),RQ(r)); }
    void Assembler::ANDQRR( R l, R r)   { emitrr(X64_andqrr, l,r); asm_output("andq %s, %s",  RQ(l),RQ(r)); }
    void Assembler::ORQRR(  R l, R r)   { emitrr(X64_orqrr,  l,r); asm_output("orq %s, %s",   RQ(l),RQ(r)); }
    void Assembler::XORQRR( R l, R r)   { emitrr(X64_xorqrr, l,r); asm_output("xorq %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMPQR(  R l, R r)   { emitrr(X64_cmpqr,  l,r); asm_output("cmpq %s, %s",  RQ(l),RQ(r)); }
    void Assembler::MOVQR(  R l, R r)   { emitrr(X64_movqr,  l,r); asm_output("movq %s, %s",  RQ(l),RQ(r)); }
    void Assembler::MOVAPSR(R l, R r)   { emitrr(X64_movapsr,l,r); asm_output("movaps %s, %s",RQ(l),RQ(r)); }
    void Assembler::UNPCKLPS(R l, R r)  { emitrr(X64_unpcklps,l,r);asm_output("unpcklps %s, %s",RQ(l),RQ(r));}

    void Assembler::CMOVNO( R l, R r)   { emitrr(X64_cmovno, l,r); asm_output("cmovlno %s, %s",  RL(l),RL(r)); }
    void Assembler::CMOVNE( R l, R r)   { emitrr(X64_cmovne, l,r); asm_output("cmovlne %s, %s",  RL(l),RL(r)); }
    void Assembler::CMOVNL( R l, R r)   { emitrr(X64_cmovnl, l,r); asm_output("cmovlnl %s, %s",  RL(l),RL(r)); }
    void Assembler::CMOVNLE(R l, R r)   { emitrr(X64_cmovnle,l,r); asm_output("cmovlnle %s, %s", RL(l),RL(r)); }
    void Assembler::CMOVNG( R l, R r)   { emitrr(X64_cmovng, l,r); asm_output("cmovlng %s, %s",  RL(l),RL(r)); }
    void Assembler::CMOVNGE(R l, R r)   { emitrr(X64_cmovnge,l,r); asm_output("cmovlnge %s, %s", RL(l),RL(r)); }
    void Assembler::CMOVNB( R l, R r)   { emitrr(X64_cmovnb, l,r); asm_output("cmovlnb %s, %s",  RL(l),RL(r)); }
    void Assembler::CMOVNBE(R l, R r)   { emitrr(X64_cmovnbe,l,r); asm_output("cmovlnbe %s, %s", RL(l),RL(r)); }
    void Assembler::CMOVNA( R l, R r)   { emitrr(X64_cmovna, l,r); asm_output("cmovlna %s, %s",  RL(l),RL(r)); }
    void Assembler::CMOVNAE(R l, R r)   { emitrr(X64_cmovnae,l,r); asm_output("cmovlnae %s, %s", RL(l),RL(r)); }

    void Assembler::CMOVQNO( R l, R r)  { emitrr(X64_cmovqno, l,r); asm_output("cmovqno %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMOVQNE( R l, R r)  { emitrr(X64_cmovqne, l,r); asm_output("cmovqne %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMOVQNL( R l, R r)  { emitrr(X64_cmovqnl, l,r); asm_output("cmovqnl %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMOVQNLE(R l, R r)  { emitrr(X64_cmovqnle,l,r); asm_output("cmovqnle %s, %s", RQ(l),RQ(r)); }
    void Assembler::CMOVQNG( R l, R r)  { emitrr(X64_cmovqng, l,r); asm_output("cmovqng %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMOVQNGE(R l, R r)  { emitrr(X64_cmovqnge,l,r); asm_output("cmovqnge %s, %s", RQ(l),RQ(r)); }
    void Assembler::CMOVQNB( R l, R r)  { emitrr(X64_cmovqnb, l,r); asm_output("cmovqnb %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMOVQNBE(R l, R r)  { emitrr(X64_cmovqnbe,l,r); asm_output("cmovqnbe %s, %s", RQ(l),RQ(r)); }
    void Assembler::CMOVQNA( R l, R r)  { emitrr(X64_cmovqna, l,r); asm_output("cmovqna %s, %s",  RQ(l),RQ(r)); }
    void Assembler::CMOVQNAE(R l, R r)  { emitrr(X64_cmovqnae,l,r); asm_output("cmovqnae %s, %s", RQ(l),RQ(r)); }

    void Assembler::MOVSXDR(R l, R r)   { emitrr(X64_movsxdr,l,r); asm_output("movsxd %s, %s",RQ(l),RL(r)); }

    void Assembler::MOVZX8(R l, R r)    { emitrr8(X64_movzx8,l,r); asm_output("movzx %s, %s",RQ(l),RB(r)); }

// XORPS is a 4x32f vector operation, we use it instead of the more obvious
// XORPD because it's one byte shorter.  This is ok because it's only used for
// zeroing an XMM register;  hence the single argument.
// Also note that (unlike most SSE2 instructions) XORPS does not have a prefix, thus emitrr() should be used.
    void Assembler::XORPS(        R r)  { emitrr(X64_xorps,    r,r); asm_output("xorps %s, %s",   RQ(r),RQ(r)); }
    void Assembler::XORPS(   R l, R r)  { emitrr(X64_xorps,    l,r); asm_output("xorps %s, %s",   RQ(l),RQ(r)); }
    void Assembler::DIVSD(   R l, R r)  { emitprr(X64_divsd,   l,r); asm_output("divsd %s, %s",   RQ(l),RQ(r)); }
    void Assembler::MULSD(   R l, R r)  { emitprr(X64_mulsd,   l,r); asm_output("mulsd %s, %s",   RQ(l),RQ(r)); }
    void Assembler::ADDSD(   R l, R r)  { emitprr(X64_addsd,   l,r); asm_output("addsd %s, %s",   RQ(l),RQ(r)); }
    void Assembler::SUBSD(   R l, R r)  { emitprr(X64_subsd,   l,r); asm_output("subsd %s, %s",   RQ(l),RQ(r)); }
    void Assembler::DIVSS(   R l, R r)  { emitprr(X64_divss,   l,r); asm_output("divss %s, %s",   RQ(l),RQ(r)); }
    void Assembler::MULSS(   R l, R r)  { emitprr(X64_mulss,   l,r); asm_output("mulss %s, %s",   RQ(l),RQ(r)); }
    void Assembler::ADDSS(   R l, R r)  { emitprr(X64_addss,   l,r); asm_output("addss %s, %s",   RQ(l),RQ(r)); }
    void Assembler::SUBSS(   R l, R r)  { emitprr(X64_subss,   l,r); asm_output("subss %s, %s",   RQ(l),RQ(r)); }
    void Assembler::DIVPS(   R l, R r)  { emitrr(X64_divps,   l,r); asm_output("divps %s, %s",   RQ(l),RQ(r)); }
    void Assembler::MULPS(   R l, R r)  { emitrr(X64_mulps,   l,r); asm_output("mulps %s, %s",   RQ(l),RQ(r)); }
    void Assembler::ADDPS(   R l, R r)  { emitrr(X64_addps,   l,r); asm_output("addps %s, %s",   RQ(l),RQ(r)); }
    void Assembler::SUBPS(   R l, R r)  { emitrr(X64_subps,   l,r); asm_output("subps %s, %s",   RQ(l),RQ(r)); }
    void Assembler::CVTSQ2SD(R l, R r)  { emitprr(X64_cvtsq2sd,l,r); asm_output("cvtsq2sd %s, %s",RQ(l),RQ(r)); }
    void Assembler::CVTSQ2SS(R l, R r)  { emitprr(X64_cvtsq2ss,l,r); asm_output("cvtsq2ss %s, %s",RQ(l),RQ(r)); }
    void Assembler::CVTSI2SD(R l, R r)  { emitprr(X64_cvtsi2sd,l,r); asm_output("cvtsi2sd %s, %s",RQ(l),RL(r)); }
    void Assembler::CVTSI2SS(R l, R r)  { emitprr(X64_cvtsi2ss,l,r); asm_output("cvtsi2ss %s, %s",RQ(l),RL(r)); }
    void Assembler::CVTSS2SD(R l, R r)  { emitprr(X64_cvtss2sd,l,r); asm_output("cvtss2sd %s, %s",RQ(l),RL(r)); }
    void Assembler::CVTSD2SS(R l, R r)  { emitprr(X64_cvtsd2ss,l,r); asm_output("cvtsd2ss %s, %s",RQ(l),RQ(r)); }
    void Assembler::CVTSD2SI(R l, R r)  { emitprr(X64_cvtsd2si,l,r); asm_output("cvtsd2si %s, %s",RL(l),RQ(r)); }
    void Assembler::CVTTSS2SI(R l, R r) { emitprr(X64_cvttss2si,l,r);asm_output("cvttss2si %s, %s",RL(l),RQ(r));}
    void Assembler::CVTTSD2SI(R l, R r) { emitprr(X64_cvttsd2si,l,r);asm_output("cvttsd2si %s, %s",RL(l),RQ(r));}
    void Assembler::UCOMISS( R l, R r)  { emitrr(X64_ucomiss, l,r);  asm_output("ucomiss %s, %s", RQ(l),RQ(r)); }
    void Assembler::UCOMISD( R l, R r)  { emitprr(X64_ucomisd, l,r); asm_output("ucomisd %s, %s", RQ(l),RQ(r)); }
    void Assembler::MOVQRX(  R l, R r)  { emitprr(X64_movqrx,  r,l); asm_output("movq %s, %s",    RQ(l),RQ(r)); } // Nb: r and l are deliberately reversed within the emitprr() call.
    void Assembler::MOVQXR(  R l, R r)  { emitprr(X64_movqxr,  l,r); asm_output("movq %s, %s",    RQ(l),RQ(r)); }
    void Assembler::MOVDXR(  R l, R r)  { emitprr(X64_movdxr,  l,r); asm_output("movd %s, %s",    RQ(l),RQ(r)); }
    void Assembler::MOVLHPS( R l, R r)  { emitrr(X64_movlhps, l,r);  asm_output("movlhps %s, %s", RQ(l),RQ(r)); }
    void Assembler::PMOVMSKB(R l, R r)  { emitprr(X64_pmovmskb,l,r); asm_output("pmovmskb %s, %s",RQ(l),RQ(r)); }
    void Assembler::CMPNEQPS(R l, R r)  { emitrr_imm8(X64_cmppsr,l,r,4); asm_output("cmpneqps %s, %s", RL(l),RL(r)); }

    inline uint8_t PSHUFD_MASK(int x, int y, int z, int w) { 
        NanoAssert(x>=0 && x<=3);
        NanoAssert(y>=0 && y<=3);
        NanoAssert(z>=0 && z<=3);
        NanoAssert(w>=0 && w<=3);
        return (uint8_t) (x | (y<<2) | (z<<4) | (w<<6)); 
    }
    void Assembler::PSHUFD(R l, R r, I m) {
        NanoAssert(isU8(m));
        uint8_t mode = uint8_t(m);
        emitprr_imm8(X64_pshufd, l, r, mode);
        asm_output("pshufd  %s, %s, %x", RQ(l), RQ(r), m);
    }
    void Assembler::SHUFPD(R l, R r, I m)  {
        NanoAssert(m>=0 && m<256);
        NanoAssert(isU8(m));
        uint8_t mode = (uint8_t) m;
        emitprr_imm8(X64_shufpd,l,r,mode);
        asm_output("shufpd  %s, %s, %x", RQ(l), RQ(r), m);
    }

    // MOVI must not affect condition codes!
    void Assembler::MOVI(  R r, I32 i32)    { emitr_imm(X64_movi,  r,i32); asm_output("movl %s, %d",RL(r),i32); }
    void Assembler::ADDLRI(R r, I32 i32)    { emitr_imm(X64_addlri,r,i32); asm_output("addl %s, %d",RL(r),i32); }
    void Assembler::SUBLRI(R r, I32 i32)    { emitr_imm(X64_sublri,r,i32); asm_output("subl %s, %d",RL(r),i32); }
    void Assembler::ANDLRI(R r, I32 i32)    { emitr_imm(X64_andlri,r,i32); asm_output("andl %s, %d",RL(r),i32); }
    void Assembler::ORLRI( R r, I32 i32)    { emitr_imm(X64_orlri, r,i32); asm_output("orl %s, %d", RL(r),i32); }
    void Assembler::XORLRI(R r, I32 i32)    { emitr_imm(X64_xorlri,r,i32); asm_output("xorl %s, %d",RL(r),i32); }
    void Assembler::CMPLRI(R r, I32 i32)    { emitr_imm(X64_cmplri,r,i32); asm_output("cmpl %s, %d",RL(r),i32); }

    void Assembler::ADDQRI( R r, I32 i32)   { emitr_imm(X64_addqri, r,i32); asm_output("addq %s, %d",   RQ(r),i32); }
    void Assembler::SUBQRI( R r, I32 i32)   { emitr_imm(X64_subqri, r,i32); asm_output("subq %s, %d",   RQ(r),i32); }
    void Assembler::ANDQRI( R r, I32 i32)   { emitr_imm(X64_andqri, r,i32); asm_output("andq %s, %d",   RQ(r),i32); }
    void Assembler::ORQRI(  R r, I32 i32)   { emitr_imm(X64_orqri,  r,i32); asm_output("orq %s, %d",    RQ(r),i32); }
    void Assembler::XORQRI( R r, I32 i32)   { emitr_imm(X64_xorqri, r,i32); asm_output("xorq %s, %d",   RQ(r),i32); }
    void Assembler::CMPQRI( R r, I32 i32)   { emitr_imm(X64_cmpqri, r,i32); asm_output("cmpq %s, %d",   RQ(r),i32); }
    void Assembler::MOVQI32(R r, I32 i32)   { emitr_imm(X64_movqi32,r,i32); asm_output("movqi32 %s, %d",RQ(r),i32); }

    void Assembler::ADDLR8(R r, I32 i8)     { emitr_imm8(X64_addlr8,r,i8); asm_output("addl %s, %d", RL(r),i8); }
    void Assembler::SUBLR8(R r, I32 i8)     { emitr_imm8(X64_sublr8,r,i8); asm_output("subl %s, %d", RL(r),i8); }
    void Assembler::ANDLR8(R r, I32 i8)     { emitr_imm8(X64_andlr8,r,i8); asm_output("andl %s, %d", RL(r),i8); }
    void Assembler::ORLR8( R r, I32 i8)     { emitr_imm8(X64_orlr8, r,i8); asm_output("orl %s, %d",  RL(r),i8); }
    void Assembler::XORLR8(R r, I32 i8)     { emitr_imm8(X64_xorlr8,r,i8); asm_output("xorl %s, %d", RL(r),i8); }
    void Assembler::CMPLR8(R r, I32 i8)     { emitr_imm8(X64_cmplr8,r,i8); asm_output("cmpl %s, %d", RL(r),i8); }

    void Assembler::ADDQR8(R r, I32 i8)     { emitr_imm8(X64_addqr8,r,i8); asm_output("addq %s, %d",RQ(r),i8); }
    void Assembler::SUBQR8(R r, I32 i8)     { emitr_imm8(X64_subqr8,r,i8); asm_output("subq %s, %d",RQ(r),i8); }
    void Assembler::ANDQR8(R r, I32 i8)     { emitr_imm8(X64_andqr8,r,i8); asm_output("andq %s, %d",RQ(r),i8); }
    void Assembler::ORQR8( R r, I32 i8)     { emitr_imm8(X64_orqr8, r,i8); asm_output("orq %s, %d", RQ(r),i8); }
    void Assembler::XORQR8(R r, I32 i8)     { emitr_imm8(X64_xorqr8,r,i8); asm_output("xorq %s, %d",RQ(r),i8); }
    void Assembler::CMPQR8(R r, I32 i8)     { emitr_imm8(X64_cmpqr8,r,i8); asm_output("cmpq %s, %d",RQ(r),i8); }

    void Assembler::IMULI(R l, R r, I32 i32)    { emitrr_imm(X64_imuli,l,r,i32); asm_output("imuli %s, %s, %d",RL(l),RL(r),i32); }

    void Assembler::MOVQI(R r, U64 u64)         { emitr_imm64(X64_movqi,r,u64); asm_output("movq %s, %p",RQ(r),(void*)u64); }

    void Assembler::LEARIP(R r, I32 d)          { emitrm(X64_learip,r,d,RZero); asm_output("lea %s, %d(rip)",RQ(r),d); }

    void Assembler::LEALRM(R r, I d, R b)       { emitrm(X64_lealrm,r,d,b); asm_output("leal %s, %d(%s)",RL(r),d,RL(b)); }
    void Assembler::LEAQRM(R r, I d, R b)       { emitrm(X64_leaqrm,r,d,b); asm_output("leaq %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVLRM(R r, I d, R b)       { emitrm(X64_movlrm,r,d,b); asm_output("movl %s, %d(%s)",RL(r),d,RQ(b)); }
    void Assembler::MOVQRM(R r, I d, R b)       { emitrm(X64_movqrm,r,d,b); asm_output("movq %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVBMR(R r, I d, R b)       { emitrm8(X64_movbmr,r,d,b); asm_output("movb %d(%s), %s",d,RQ(b),RB(r)); }
    void Assembler::MOVSMR(R r, I d, R b)       { emitprm(X64_movsmr,r,d,b); asm_output("movs %d(%s), %s",d,RQ(b),RS(r)); }
    void Assembler::MOVLMR(R r, I d, R b)       { emitrm(X64_movlmr,r,d,b); asm_output("movl %d(%s), %s",d,RQ(b),RL(r)); }
    void Assembler::MOVQMR(R r, I d, R b)       { emitrm(X64_movqmr,r,d,b); asm_output("movq %d(%s), %s",d,RQ(b),RQ(r)); }

    void Assembler::MOVZX8M( R r, I d, R b)     { emitrm_wide(X64_movzx8m, r,d,b); asm_output("movzxb %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVZX16M(R r, I d, R b)     { emitrm_wide(X64_movzx16m,r,d,b); asm_output("movzxs %s, %d(%s)",RQ(r),d,RQ(b)); }

    void Assembler::MOVSX8M( R r, I d, R b)     { emitrm_wide(X64_movsx8m, r,d,b); asm_output("movsxb %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVSX16M(R r, I d, R b)     { emitrm_wide(X64_movsx16m,r,d,b); asm_output("movsxs %s, %d(%s)",RQ(r),d,RQ(b)); }

    void Assembler::MOVSDRM(R r, I d, R b)      { emitprm(X64_movsdrm,r,d,b); asm_output("movsd %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVSDMR(R r, I d, R b)      { emitprm(X64_movsdmr,r,d,b); asm_output("movsd %d(%s), %s",d,RQ(b),RQ(r)); }
    void Assembler::MOVSSRM(R r, I d, R b)      { emitprm(X64_movssrm,r,d,b); asm_output("movss %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVSSMR(R r, I d, R b)      { emitprm(X64_movssmr,r,d,b); asm_output("movss %d(%s), %s",d,RQ(b),RQ(r)); }
    void Assembler::MOVUPSRM(R r, I d, R b)     { emitrm_wide(X64_movupsrm,r,d,b); asm_output("movups %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVUPSMR(R r, I d, R b)     { emitrm_wide(X64_movupsmr,r,d,b); asm_output("movups %d(%s), %s",d,RQ(b),RQ(r)); }
    void Assembler::MOVUPSRMRIP(R r, I d)       { emitrm_wide(X64_movupsrip,r,d,RZero); asm_output("movups %s, %d(rip)",RQ(r),d); }
    void Assembler::MOVAPSRM(R r, I d, R b)     { emitrm_wide(X64_movapsrm,r,d,b); asm_output("movaps %s, %d(%s)",RQ(r),d,RQ(b)); }
    void Assembler::MOVAPSRMRIP(R r, I d)       { emitrm_wide(X64_movapsrip,r,d,RZero); asm_output("movaps %s, %d(rip)",RQ(r),d); }
    void Assembler::MOVSSSPR(R r, I d)          { 
                                                  uint64_t op = emit_disp32_sib(X64_movssspr,d); 
                                                  emit( op | U64((REGNUM(r)&7)<<3) << 48 | U64((REGNUM(r)&8)>>1) << 24);
                                                  asm_output("movss %s, %d(RSP)",RQ(r),d); 
                                                }
    void Assembler::MOVSDSPR(R r, I d)          { 
                                                  uint64_t op = emit_disp32_sib(X64_movsdspr,d); 
                                                  emit( op | U64((REGNUM(r)&7)<<3) << 48 | U64((REGNUM(r)&8)>>1) << 24);
                                                  asm_output("movsd %s, %d(RSP)",RQ(r),d); 
                                                }
    void Assembler::MOVUPSSPR(R r, I d)         { 
                                                  uint64_t op = emit_disp32_sib(X64_movupspr,d); 
                                                  emit( op | U64((REGNUM(r)&7)<<3) << 48 | U64((REGNUM(r)&8)>>1) << 24);
                                                  asm_output("movups %s, %d(RSP)",RQ(r),d); 
                                                }
    

    void Assembler::JMP8( S n, NIns* t)    { emit_target8(n, X64_jmp8,t); asm_output("jmp %p", t); }

    void Assembler::JMP32(S n, NIns* t)    { emit_target32(n,X64_jmp, t); asm_output("jmp %p", t); }
    void Assembler::JMP64(S n, NIns* t)    { emit_target64(n,X64_jmpi, t); asm_output("jmp %p", t); }

    void Assembler::JMPX(R indexreg, NIns** table)
    {
        Register R5 = { 5 };
        emitrxb_imm(X64_jmpx, RZero, indexreg, R5, (int32_t)(uintptr_t)table);
        asm_output("jmpq [%s*8 + %p]", RQ(indexreg), (void*)table);
    }

    void Assembler::JMPXB(R indexreg, R tablereg)   { emitxb(X64_jmpxb, indexreg, tablereg); asm_output("jmp [%s*8 + %s]", RQ(indexreg), RQ(tablereg)); }

    void Assembler::JO( S n, NIns* t)      { emit_target32(n,X64_jo,  t); asm_output("jo %p", t); }
    void Assembler::JE( S n, NIns* t)      { emit_target32(n,X64_je,  t); asm_output("je %p", t); }
    void Assembler::JL( S n, NIns* t)      { emit_target32(n,X64_jl,  t); asm_output("jl %p", t); }
    void Assembler::JLE(S n, NIns* t)      { emit_target32(n,X64_jle, t); asm_output("jle %p",t); }
    void Assembler::JG( S n, NIns* t)      { emit_target32(n,X64_jg,  t); asm_output("jg %p", t); }
    void Assembler::JGE(S n, NIns* t)      { emit_target32(n,X64_jge, t); asm_output("jge %p",t); }
    void Assembler::JB( S n, NIns* t)      { emit_target32(n,X64_jb,  t); asm_output("jb %p", t); }
    void Assembler::JBE(S n, NIns* t)      { emit_target32(n,X64_jbe, t); asm_output("jbe %p",t); }
    void Assembler::JA( S n, NIns* t)      { emit_target32(n,X64_ja,  t); asm_output("ja %p", t); }
    void Assembler::JAE(S n, NIns* t)      { emit_target32(n,X64_jae, t); asm_output("jae %p",t); }
    void Assembler::JP( S n, NIns* t)      { emit_target32(n,X64_jp,  t); asm_output("jp  %p",t); }

    void Assembler::JNO( S n, NIns* t)     { emit_target32(n,X64_jo ^X64_jneg, t); asm_output("jno %p", t); }
    void Assembler::JNE( S n, NIns* t)     { emit_target32(n,X64_je ^X64_jneg, t); asm_output("jne %p", t); }
    void Assembler::JNL( S n, NIns* t)     { emit_target32(n,X64_jl ^X64_jneg, t); asm_output("jnl %p", t); }
    void Assembler::JNLE(S n, NIns* t)     { emit_target32(n,X64_jle^X64_jneg, t); asm_output("jnle %p",t); }
    void Assembler::JNG( S n, NIns* t)     { emit_target32(n,X64_jg ^X64_jneg, t); asm_output("jng %p", t); }
    void Assembler::JNGE(S n, NIns* t)     { emit_target32(n,X64_jge^X64_jneg, t); asm_output("jnge %p",t); }
    void Assembler::JNB( S n, NIns* t)     { emit_target32(n,X64_jb ^X64_jneg, t); asm_output("jnb %p", t); }
    void Assembler::JNBE(S n, NIns* t)     { emit_target32(n,X64_jbe^X64_jneg, t); asm_output("jnbe %p",t); }
    void Assembler::JNA( S n, NIns* t)     { emit_target32(n,X64_ja ^X64_jneg, t); asm_output("jna %p", t); }
    void Assembler::JNAE(S n, NIns* t)     { emit_target32(n,X64_jae^X64_jneg, t); asm_output("jnae %p",t); }

    void Assembler::JO8( S n, NIns* t)     { emit_target8(n,X64_jo8,  t); asm_output("jo %p", t); }
    void Assembler::JE8( S n, NIns* t)     { emit_target8(n,X64_je8,  t); asm_output("je %p", t); }
    void Assembler::JL8( S n, NIns* t)     { emit_target8(n,X64_jl8,  t); asm_output("jl %p", t); }
    void Assembler::JLE8(S n, NIns* t)     { emit_target8(n,X64_jle8, t); asm_output("jle %p",t); }
    void Assembler::JG8( S n, NIns* t)     { emit_target8(n,X64_jg8,  t); asm_output("jg %p", t); }
    void Assembler::JGE8(S n, NIns* t)     { emit_target8(n,X64_jge8, t); asm_output("jge %p",t); }
    void Assembler::JB8( S n, NIns* t)     { emit_target8(n,X64_jb8,  t); asm_output("jb %p", t); }
    void Assembler::JBE8(S n, NIns* t)     { emit_target8(n,X64_jbe8, t); asm_output("jbe %p",t); }
    void Assembler::JA8( S n, NIns* t)     { emit_target8(n,X64_ja8,  t); asm_output("ja %p", t); }
    void Assembler::JAE8(S n, NIns* t)     { emit_target8(n,X64_jae8, t); asm_output("jae %p",t); }
    void Assembler::JP8( S n, NIns* t)     { emit_target8(n,X64_jp8,  t); asm_output("jp  %p",t); }

    void Assembler::JNO8( S n, NIns* t)    { emit_target8(n,X64_jo8 ^X64_jneg8, t); asm_output("jno %p", t); }
    void Assembler::JNE8( S n, NIns* t)    { emit_target8(n,X64_je8 ^X64_jneg8, t); asm_output("jne %p", t); }
    void Assembler::JNL8( S n, NIns* t)    { emit_target8(n,X64_jl8 ^X64_jneg8, t); asm_output("jnl %p", t); }
    void Assembler::JNLE8(S n, NIns* t)    { emit_target8(n,X64_jle8^X64_jneg8, t); asm_output("jnle %p",t); }
    void Assembler::JNG8( S n, NIns* t)    { emit_target8(n,X64_jg8 ^X64_jneg8, t); asm_output("jng %p", t); }
    void Assembler::JNGE8(S n, NIns* t)    { emit_target8(n,X64_jge8^X64_jneg8, t); asm_output("jnge %p",t); }
    void Assembler::JNB8( S n, NIns* t)    { emit_target8(n,X64_jb8 ^X64_jneg8, t); asm_output("jnb %p", t); }
    void Assembler::JNBE8(S n, NIns* t)    { emit_target8(n,X64_jbe8^X64_jneg8, t); asm_output("jnbe %p",t); }
    void Assembler::JNA8( S n, NIns* t)    { emit_target8(n,X64_ja8 ^X64_jneg8, t); asm_output("jna %p", t); }
    void Assembler::JNAE8(S n, NIns* t)    { emit_target8(n,X64_jae8^X64_jneg8, t); asm_output("jnae %p",t); }
    void Assembler::JNP8( S n, NIns* t)    { emit_target8(n,X64_jp8 ^X64_jneg8, t); asm_output("jnp  %p",t); }

    void Assembler::CALL( S n, NIns* t)    { emit_target32(n,X64_call,t); asm_output("call %p",t); }

    void Assembler::CALLRAX()       { emit(X64_callrax); asm_output("call (rax)"); }
    void Assembler::RET()           { emit(X64_ret);     asm_output("ret");        }

    void Assembler::MOVQMI(R r, I d, I32 imm) { emitrm_imm32(X64_movqmi,r,d,imm); asm_output("movq %d(%s), %d",d,RQ(r),imm); }
    void Assembler::MOVLMI(R r, I d, I32 imm) { emitrm_imm32(X64_movlmi,r,d,imm); asm_output("movl %d(%s), %d",d,RQ(r),imm); }
    void Assembler::MOVSMI(R r, I d, I32 imm) { emitprm_imm16(X64_movsmi,r,d,imm); asm_output("movs %d(%s), %d",d,RQ(r),imm); }
    void Assembler::MOVBMI(R r, I d, I32 imm) { emitrm_imm8(X64_movbmi,r,d,imm); asm_output("movb %d(%s), %d",d,RQ(r),imm); }

    void Assembler::MOVQSPR(I d, R r)   { emit(X64_movqspr | U64(d) << 56 | U64((REGNUM(r)&7)<<3) << 40 | U64((REGNUM(r)&8)>>1) << 24); asm_output("movq %d(rsp), %s", d, RQ(r)); }    // insert r into mod/rm and rex bytes
    void Assembler::MOVQSPX(I d, R r)   { emit(rexprb(X64_movqspx,RSP,r) | U64(d) << 56 | U64((REGNUM(r)&7)<<3) << 40); asm_output("movq %d(rsp), %s", d, RQ(r)); }

    void Assembler::XORPSA(R r, I32 i32)    { emitxm_abs(X64_xorpsa, r, i32); asm_output("xorps %s, (0x%x)",RQ(r), i32); }
    void Assembler::XORPSM(R r, NIns* a64)  { emitxm_rel(X64_xorpsm, r, a64); asm_output("xorps %s, (%p)",  RQ(r), a64); }

    void Assembler::X86_AND8R(R r)  { emit(X86_and8r | U64(REGNUM(r)<<3|(REGNUM(r)|4))<<56); asm_output("andb %s, %s", RB(r), RBhi(r)); }
    void Assembler::X86_SETNP(R r)  { emit(X86_setnp | U64(REGNUM(r)|4)<<56); asm_output("setnp %s", RBhi(r)); }
    void Assembler::X86_SETE(R r)   { emit(X86_sete  | U64(REGNUM(r))<<56);   asm_output("sete %s", RB(r)); }

#undef R
#undef I
#undef I32
#undef U64
#undef S

    void Assembler::MR(Register d, Register s) {
        NanoAssert(IsGpReg(d) && IsGpReg(s));
        MOVQR(d, s);
    }

    // This is needed for guards;  we must be able to patch the jmp later and
    // we cannot do that if an 8-bit relative jump is used, so we can't use
    // JMP().
    void Assembler::JMPl(NIns* target) {
        if (target && isTargetWithinS32(target)) {
            JMP32(8, target);
        } else {
            JMP64(16, target);
        }
    }

	// If target address is unknown, i.e., a backward branch, allow for 64-bit address.
	// Formerly, a 32-bit displacement would do, but layout randomization may result in
	// larger displacements if a function is split between two randomly-placed regions.
	
    void Assembler::JMP(NIns *target) {
        if (target && isTargetWithinS8(target)) {
			JMP8(8, target);
		} else if (target && isTargetWithinS32(target)) {
			JMP32(8, target);
        } else {
            JMP64(16, target);
        }
    }

    void Assembler::asm_qbinop(LIns *ins) {
        asm_arith(ins);
    }

    void Assembler::asm_shift(LIns *ins) {
        // Shift requires rcx for shift count.
        LIns *a = ins->oprnd1();
        LIns *b = ins->oprnd2();
        // Immediate shift counts are masked to six bits, and thus are not blinded.
        // In principle, there is a test for 'shouldBlind()', but it will always succeed.
        if (b->isImmI()) {
            asm_shift_imm(ins);
            return;
        }

        Register rr, ra;
        if (a != b) {
            findSpecificRegFor(b, RCX);
            beginOp1Regs(ins, GpRegs & ~rmask(RCX), rr, ra);
        } else {
            // Nb: this is just like beginOp1Regs() except that it asserts
            // that ra is in GpRegs instead of rmask(RCX)) -- this is
            // necessary for the a==b case because 'a' might not be in RCX
            // (which is ok, the MR(rr, ra) below will move it into RCX).
            rr = prepareResultReg(ins, rmask(RCX));

            // If 'a' isn't in a register, it can be clobbered by 'ins'.
            ra = a->isInReg() ? a->getReg() : rr;
            NanoAssert(rmask(ra) & GpRegs);
        }

        switch (ins->opcode()) {
        default:
            TODO(asm_shift);
        case LIR_rshuq: SHRQ(rr);   break;
        case LIR_rshq:  SARQ(rr);   break;
        case LIR_lshq:  SHLQ(rr);   break;
        case LIR_rshui: SHR( rr);   break;
        case LIR_rshi:  SAR( rr);   break;
        case LIR_lshi:  SHL( rr);   break;
        }
        if (rr != ra)
            MR(rr, ra);

        endOpRegs(ins, rr, ra);
    }

    void Assembler::asm_shift_imm(LIns *ins) {
        Register rr, ra;
        beginOp1Regs(ins, GpRegs, rr, ra);

        int shift = ins->oprnd2()->immI() & 63;
        switch (ins->opcode()) {
        default: TODO(shiftimm);
        case LIR_rshuq: SHRQI(rr, shift);   break;
        case LIR_rshq:  SARQI(rr, shift);   break;
        case LIR_lshq:  SHLQI(rr, shift);   break;
        case LIR_rshui: SHRI( rr, shift);   break;
        case LIR_rshi:  SARI( rr, shift);   break;
        case LIR_lshi:  SHLI( rr, shift);   break;
        }
        if (rr != ra)
            MR(rr, ra);

        endOpRegs(ins, rr, ra);
    }

    static bool isImm32(LIns *ins) {
        return ins->isImmI() || (ins->isImmQ() && isS32(ins->immQ()));
    }
    static int32_t getImm32(LIns *ins) {
        return ins->isImmI() ? ins->immI() : int32_t(ins->immQ());
    }

    // Binary op, integer regs, rhs is int32 constant.
    void Assembler::asm_arith_imm(LIns *ins) {
        LIns *b = ins->oprnd2();
        int32_t imm = getImm32(b);
        LOpcode op = ins->opcode();
        Register rr, ra;

        if (op == LIR_muli || op == LIR_muljovi || op == LIR_mulxovi) {
            // Special case: imul-by-imm has true 3-addr form.  So we don't
            // need the MR(rr, ra) after the IMULI.
            beginOp1Regs(ins, GpRegs, rr, ra);
            IMULI(rr, ra, imm);
            endOpRegs(ins, rr, ra);
            return;
        }

        beginOp1Regs(ins, GpRegs, rr, ra);
        if (isS8(imm)) {
            switch (ins->opcode()) {
            default: TODO(arith_imm8);
            case LIR_addi:
            case LIR_addjovi:
            case LIR_addxovi:    ADDLR8(rr, imm);   break;   // XXX: bug 547125: could use LEA for LIR_addi
            case LIR_andi:       ANDLR8(rr, imm);   break;
            case LIR_ori:        ORLR8( rr, imm);   break;
            case LIR_subi:
            case LIR_subjovi:
            case LIR_subxovi:    SUBLR8(rr, imm);   break;
            case LIR_xori:       XORLR8(rr, imm);   break;
            case LIR_addq:
            case LIR_addjovq:    ADDQR8(rr, imm);   break;
            case LIR_subq:
            case LIR_subjovq:    SUBQR8(rr, imm);   break;
            case LIR_andq:       ANDQR8(rr, imm);   break;
            case LIR_orq:        ORQR8( rr, imm);   break;
            case LIR_xorq:       XORQR8(rr, imm);   break;
            }
        } else {
            switch (ins->opcode()) {
            default: TODO(arith_imm);
            case LIR_addi:
            case LIR_addjovi:
            case LIR_addxovi:    ADDLRI(rr, imm);   break;   // XXX: bug 547125: could use LEA for LIR_addi
            case LIR_andi:       ANDLRI(rr, imm);   break;
            case LIR_ori:        ORLRI( rr, imm);   break;
            case LIR_subi:
            case LIR_subjovi:
            case LIR_subxovi:    SUBLRI(rr, imm);   break;
            case LIR_xori:       XORLRI(rr, imm);   break;
            case LIR_addq:
            case LIR_addjovq:    ADDQRI(rr, imm);   break;
            case LIR_subq:
            case LIR_subjovq:    SUBQRI(rr, imm);   break;
            case LIR_andq:       ANDQRI(rr, imm);   break;
            case LIR_orq:        ORQRI( rr, imm);   break;
            case LIR_xorq:       XORQRI(rr, imm);   break;
            }
        }
        if (rr != ra)
            MR(rr, ra);

        endOpRegs(ins, rr, ra);
    }

    bool Assembler::asm_arith_imm_blind(LIns *ins) {

        switch (ins->opcode()) {
        case LIR_addi:
        case LIR_andi:
        case LIR_ori: 
        case LIR_subi:
        case LIR_xori:
        case LIR_addq:
        case LIR_subq:
        case LIR_andq:
        case LIR_orq:
        case LIR_xorq:
            break;

        default:
            return false;
        }

        LIns *b = ins->oprnd2();
        int32_t imm = getImm32(b);
        Register rr, ra;

        // We do not encode the short immediates here, so for efficiency's sake,
        // we should not be asked to handle cases where they would be appropriate.
        NanoAssert(!isS8(imm));

        beginOp1Regs(ins, GpRegs, rr, ra);

        // These operations allow for blinding of a constant RHS without
        // allocating an extra register.  The technique was borrowed from
        // JavaScriptCore (WebKit).  We may set CCs that would not be set
        // in the non-blinded case, so we must be careful to use this function
        // only in cases where the CC's are not required.

        switch (ins->opcode()) {

        case LIR_addi:  ADDLRI(rr, _blindMask32);  ADDLRI(rr, imm - _blindMask32);  break;
        case LIR_addq:  ADDQRI(rr, _blindMask32);  ADDQRI(rr, imm - _blindMask32);  break;

        case LIR_subi:  SUBLRI(rr, _blindMask32);  SUBLRI(rr, imm - _blindMask32);  break;
        case LIR_subq:  SUBQRI(rr, _blindMask32);  SUBQRI(rr, imm - _blindMask32);  break;

        case LIR_andi:  ANDLRI(rr, (imm & _blindMask32) | ~_blindMask32);  ANDLRI(rr, (imm & ~_blindMask32) | _blindMask32);  break;
        case LIR_andq:  ANDQRI(rr, (imm & _blindMask32) | ~_blindMask32);  ANDQRI(rr, (imm & ~_blindMask32) | _blindMask32);  break;

        case LIR_ori:   ORLRI(rr, imm & _blindMask32);  ORLRI(rr, imm & ~_blindMask32);  break;
        case LIR_orq:   ORQRI(rr, imm & _blindMask32);  ORQRI(rr, imm & ~_blindMask32);  break;

        case LIR_xori:  XORLRI(rr, _blindMask32);  XORLRI(rr, imm ^ _blindMask32);  break;
        case LIR_xorq:  XORQRI(rr, _blindMask32);  XORQRI(rr, imm ^ _blindMask32);  break;

        default:
            NanoAssert(false);
        }

        if (rr != ra)
            MR(rr, ra);

        endOpRegs(ins, rr, ra);

        return true;
    }

    // Generates code for a LIR_divi that doesn't have a subsequent LIR_modi.
    void Assembler::asm_div(LIns *div) {
        NanoAssert(div->isop(LIR_divi));
        LIns *a = div->oprnd1();
        LIns *b = div->oprnd2();

        evictIfActive(RDX);
        prepareResultReg(div, rmask(RAX));

        Register rb = findRegFor(b, GpRegs & ~(rmask(RAX)|rmask(RDX)));
        Register ra = a->isInReg() ? a->getReg() : RAX;

        IDIV(rb);
        SARI(RDX, 31);
        MR(RDX, RAX);
        if (RAX != ra)
            MR(RAX, ra);

        freeResourcesOf(div);
        if (!a->isInReg()) {
            NanoAssert(ra == RAX);
            findSpecificRegForUnallocated(a, RAX);
        }
    }

    // Generates code for a LIR_modi(LIR_divi(divL, divR)) sequence.
    void Assembler::asm_div_mod(LIns *mod) {
        LIns *div = mod->oprnd1();

        NanoAssert(mod->isop(LIR_modi));
        NanoAssert(div->isop(LIR_divi));

        LIns *divL = div->oprnd1();
        LIns *divR = div->oprnd2();

        prepareResultReg(mod, rmask(RDX));
        prepareResultReg(div, rmask(RAX));

        Register rDivR = findRegFor(divR, GpRegs & ~(rmask(RAX)|rmask(RDX)));
        Register rDivL = divL->isInReg() ? divL->getReg() : RAX;

        IDIV(rDivR);
        SARI(RDX, 31);
        MR(RDX, RAX);
        if (RAX != rDivL)
            MR(RAX, rDivL);

        freeResourcesOf(mod);
        freeResourcesOf(div);
        if (!divL->isInReg()) {
            NanoAssert(rDivL == RAX);
            findSpecificRegForUnallocated(divL, RAX);
        }
    }

    // binary op with integer registers
    void Assembler::asm_arith(LIns *ins) {
        Register rr, ra, rb = UnspecifiedReg;   // init to shut GCC up

        switch (ins->opcode()) {
        case LIR_lshi:  case LIR_lshq:
        case LIR_rshi:  case LIR_rshq:
        case LIR_rshui: case LIR_rshuq:
            asm_shift(ins);
            return;
        case LIR_modi:
            asm_div_mod(ins);
            return;
        case LIR_divi:
            // Nb: if the div feeds into a mod it will be handled by
            // asm_div_mod() rather than here.
            asm_div(ins);
            return;
        default:
            break;
        }

        LIns *b = ins->oprnd2();
        if (isImm32(b)) {
            int32_t val = getImm32(b);
            if (b->isTainted() && shouldBlind(val)) {
                if (asm_arith_imm_blind(ins))
                    return;                
                // else fall through to non-immediate case
            } else {
                asm_arith_imm(ins);
                return;
            }
        }

        beginOp2Regs(ins, GpRegs, rr, ra, rb);
        switch (ins->opcode()) {
        default:           TODO(asm_arith);
        case LIR_ori:      ORLRR(rr, rb);  break;
        case LIR_subi:
        case LIR_subjovi:
        case LIR_subxovi:  SUBRR(rr, rb);  break;
        case LIR_addi:
        case LIR_addjovi:
        case LIR_addxovi:  ADDRR(rr, rb);  break;  // XXX: bug 547125: could use LEA for LIR_addi
        case LIR_andi:     ANDRR(rr, rb);  break;
        case LIR_xori:     XORRR(rr, rb);  break;
        case LIR_muli:
        case LIR_muljovi:
        case LIR_mulxovi:  IMUL(rr, rb);   break;
        case LIR_xorq:     XORQRR(rr, rb); break;
        case LIR_orq:      ORQRR(rr, rb);  break;
        case LIR_andq:     ANDQRR(rr, rb); break;
        case LIR_addq:
        case LIR_addjovq:  ADDQRR(rr, rb); break;
        case LIR_subq:
        case LIR_subjovq:  SUBQRR(rr, rb); break;
        }
        if (rr != ra)
            MR(rr, ra);

        endOpRegs(ins, rr, ra);
    }

    // Binary op with fp registers.
    void Assembler::asm_fop(LIns *ins) {
        Register rr, ra, rb = UnspecifiedReg;   // init to shut GCC up
        beginOp2Regs(ins, FpRegs, rr, ra, rb);
        switch (ins->opcode()) {
        default:        TODO(asm_fop);
        case LIR_divd:  DIVSD(rr, rb); break;
        case LIR_muld:  MULSD(rr, rb); break;
        case LIR_addd:  ADDSD(rr, rb); break;
        case LIR_subd:  SUBSD(rr, rb); break;
        case LIR_divf:  DIVSS(rr, rb); break;
        case LIR_mulf:  MULSS(rr, rb); break;
        case LIR_addf:  ADDSS(rr, rb); break;
        case LIR_subf:  SUBSS(rr, rb); break;
        case LIR_divf4: DIVPS(rr, rb); break;
        case LIR_mulf4: MULPS(rr, rb); break;
        case LIR_addf4: ADDPS(rr, rb); break;
        case LIR_subf4: SUBPS(rr, rb); break;
        }
        if (rr != ra) {
            asm_nongp_copy(rr, ra);
        }

        endOpRegs(ins, rr, ra);
    }

    void Assembler::asm_neg_not(LIns *ins) {
        Register rr, ra;
        beginOp1Regs(ins, GpRegs, rr, ra);

        if (ins->isop(LIR_noti))
            NOT(rr);
        else
            NEG(rr);
        if (rr != ra)
            MR(rr, ra);

        endOpRegs(ins, rr, ra);
    }

    void Assembler::asm_call(LIns *ins) {
        if (!ins->isop(LIR_callv)) {
            Register rr = (ins->isop(LIR_calld) || ins->isop(LIR_callf) || ins->isop(LIR_callf4)) ? XMM0 : RAX;
            prepareResultReg(ins, rmask(rr));
            evictScratchRegsExcept(rmask(rr));
        } else {
            evictScratchRegsExcept(0);
        }

        const CallInfo *call = ins->callInfo();
        ArgType argTypes[MAXARGS];
        int argc = call->getArgTypes(argTypes);

        if (!call->isIndirect()) {
            verbose_only(if (_logc->lcbits & LC_Native)
                outputf("        %p:", _nIns);
            )
            NIns *target = (NIns*)call->_address;
            if (isTargetWithinS32(target)) {
                CALL(8, target);
            } else {
                // can't reach target from here, load imm64 and do an indirect jump
                CALLRAX();
                asm_immq(RAX, (uint64_t)target, /*canClobberCCs*/true, /*blind*/false);
            }
            // Call this now so that the arg setup can involve 'rr'.
            freeResourcesOf(ins);
        } else {
            // Indirect call: we assign the address arg to RAX since it's not
            // used for regular arguments, and is otherwise scratch since it's
            // clobberred by the call.
            CALLRAX();
            // Call this now so that the arg setup can involve 'rr'.
            freeResourcesOf(ins);

            // Assign the call address to RAX.  Must happen after freeResourcesOf()
            // since RAX is usually the return value and will be allocated until that point.
            asm_regarg(ARGTYPE_P, ins->arg(--argc), RAX);
        }

    #ifdef _WIN64
        int stk_used = 32; // always reserve 32byte shadow area
    #else
        int stk_used = 0;
        Register fr = XMM0;
    #endif
        int arg_index = 0;
        for (int i = 0; i < argc; i++) {
            int j = argc - i - 1;
            ArgType ty = argTypes[j];
            LIns* arg = ins->arg(j);
            if ((ty == ARGTYPE_I || ty == ARGTYPE_UI || ty == ARGTYPE_Q) && arg_index < NumArgRegs) {
                // gp arg
                asm_regarg(ty, arg, RegAlloc::argRegs[arg_index]);
                arg_index++;
            }
        #if defined(_WIN64)
            else if ((ty == ARGTYPE_D || ty == ARGTYPE_F) && arg_index < NumArgRegs) {
                // double and float go in XMM register # based on overall arg_index
                Register rxi = XMM0 + arg_index;
                asm_regarg(ty, arg, rxi);
                arg_index++;
            }
            else if ((ty == ARGTYPE_F4) && arg_index < NumArgRegs) {
                // first 4 parameters passed as pointers
                asm_ptrarg(ty, arg, RegAlloc::argRegs[arg_index]);
                arg_index++;
            }
        #else
            else if ((ty == ARGTYPE_D || ty == ARGTYPE_F || ty == ARGTYPE_F4) && fr < XMM8) {
                // double, float, and float4 go in next available XMM register
                asm_regarg(ty, arg, fr);
                fr = fr + 1;
            }
        #endif
            else {
                asm_stkarg(ty, arg, stk_used);
                /* float4 is passed as a pointer to the value, so it still takes up as much space as "void*" */
                stk_used += sizeof(void*);
            }
        }

        if (stk_used > max_stk_used)
            max_stk_used = stk_used;
    }

    void Assembler::asm_ptrarg(ArgType ty, LIns *p, Register r) {
        NanoAssert(ty==ARGTYPE_F4);(void)ty;
        NanoAssert(IsGpReg(r));
        if(p->isImmF4()){
            // No need to blind constant, as we load from pool.
            const float4_t* vaddr = findImmF4FromPool(p->immF4());
            if( isTargetWithinS32((NIns*)vaddr) ) {
                int32_t d = int32_t(int64_t(vaddr)-int64_t(_nIns));
                LEARIP(r, d);
            } else {
                MOVQI(r, (U64) vaddr);
            }
        } else {
            int d = findMemFor(p);
            LEAQRM(r, d, FP);
        }
    }

    void Assembler::asm_regarg(ArgType ty, LIns *p, Register r) {
        if (ty == ARGTYPE_I) {
            NanoAssert(p->isI());
            if (p->isImmI()) {
                asm_immq(r, int64_t(p->immI()), /*canClobberCCs*/true, p->isTainted());
                return;
            }
            // sign extend int32 to int64
            MOVSXDR(r, r);
        } else if (ty == ARGTYPE_UI) {
            NanoAssert(p->isI());
            if (p->isImmI()) {
                asm_immq(r, uint64_t(uint32_t(p->immI())), /*canClobberCCs*/true, p->isTainted());
                return;
            }
            // zero extend with 32bit mov, auto-zeros upper 32bits
            MOVLR(r, r);
        } else {
            // Do nothing.
        }
        /* there is no point in folding an immediate here, because
         * the argument register must be a scratch register and we're
         * just before a call.  Just reserving the register will cause
         * the constant to be rematerialized nearby in asm_restore(),
         * which is the same instruction we would otherwise emit right
         * here, and moving it earlier in the stream provides more scheduling
         * freedom to the cpu. */
        findSpecificRegFor(p, r);
    }

    void Assembler::asm_stkarg(ArgType ty, LIns *p, int stk_off) {
        NanoAssert(isS8(stk_off));
        if (ty == ARGTYPE_I || ty == ARGTYPE_UI || ty == ARGTYPE_Q) {
            Register r = findRegFor(p, GpRegs);
            MOVQSPR(stk_off, r);    // movq [rsp+d8], r
            if (ty == ARGTYPE_I) {
                // sign extend int32 to int64
                NanoAssert(p->isI());
                MOVSXDR(r, r);
            } else if (ty == ARGTYPE_UI) {
                // zero extend uint32 to uint64
                NanoAssert(p->isI());
                MOVLR(r, r);
            } else {
                NanoAssert(ty == ARGTYPE_Q);
                // Do nothing.
            }
        } else if (ty == ARGTYPE_D) {
            Register r = findRegFor(p, FpRegs);
            // TODO!! MOVQSPX(stk_off, r);    // movsd [rsp+d8], xmm
            MOVSDSPR(r, stk_off);
        } else if (ty == ARGTYPE_F) {
            Register r = findRegFor(p, FpRegs);
            MOVSSSPR(r, stk_off);
        } else if (ty == ARGTYPE_F4) {
            // We need to pass on stack a pointer to the float4 value
            Register r = _allocator.allocTempReg(GpRegs);
            MOVQSPR(stk_off, r);    // movq [rsp+d8], r
            asm_ptrarg(ty,p,r);
        } else {
            verbose_only({
                char str[1000];sprintf(str,"ArgType %x Opcode %x\n",ty, p->opcode());
                avmplus::AvmLog(str);
            })
            TODO(asm_stkarg_non_int);
        }
    }

    void Assembler::asm_q2i(LIns *ins) {
        Register rr, ra;
        beginOp1Regs(ins, GpRegs, rr, ra);
        NanoAssert(IsGpReg(ra));
        // If ra==rr we do nothing.  This is valid because we don't assume the
        // upper 32-bits of a 64-bit GPR are zero when doing a 32-bit
        // operation.  More specifically, we widen 32-bit to 64-bit in three
        // places, all of which explicitly sign- or zero-extend: asm_ui2uq(),
        // asm_regarg() and asm_stkarg().  For the first this is required, for
        // the latter two it's unclear if this is required, but it can't hurt.
        if (ra != rr)
            MOVLR(rr, ra);
        endOpRegs(ins, rr, ra);
    }

    void Assembler::asm_ui2uq(LIns *ins) {
        Register rr, ra;
        beginOp1Regs(ins, GpRegs, rr, ra);
        NanoAssert(IsGpReg(ra));
        if (ins->isop(LIR_ui2uq)) {
            MOVLR(rr, ra);      // 32bit mov zeros the upper 32bits of the target
        } else {
            NanoAssert(ins->isop(LIR_i2q));
            MOVSXDR(rr, ra);    // sign extend 32->64
        }
        endOpRegs(ins, rr, ra);
    }

    void Assembler::asm_dasq(LIns *ins) {
        Register rr = prepareResultReg(ins, GpRegs);
        Register ra = findRegFor(ins->oprnd1(), FpRegs);
        asm_nongp_copy(rr, ra);
        freeResourcesOf(ins);
    }

    void Assembler::asm_qasd(LIns *ins) {
        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(ins->oprnd1(), GpRegs);
        asm_nongp_copy(rr, ra);
        freeResourcesOf(ins);
    }

    // The CVTSI2SD instruction only writes to the low 64bits of the target
    // XMM register, which hinders register renaming and makes dependence
    // chains longer.  So we precede with XORPS to clear the target register.

    void Assembler::asm_i2d(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isD() && a->isI());

        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, GpRegs);
        CVTSI2SD(rr, ra);   // cvtsi2sd xmmr, b  only writes xmm:0:64
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        freeResourcesOf(ins);
    }

    // As for i2d

    void Assembler::asm_q2d(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isD() && a->isQ());
        
        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, GpRegs);
        CVTSQ2SD(rr, ra);   // cvtsq2sd xmmr, b  only writes xmm:0:64
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        freeResourcesOf(ins);
    }
    
    void Assembler::asm_ui2d(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isD() && a->isI());

        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, GpRegs);
        // Because oprnd1 is 32bit, it's ok to zero-extend it without worrying about clobbering.
        CVTSQ2SD(rr, ra);   // convert int64 to double
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        MOVLR(ra, ra);      // zero extend u32 to int64
        freeResourcesOf(ins);
    }

    void Assembler::asm_ui2f(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isF() && a->isI());

        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, GpRegs);
        // Because oprnd1 is 32bit, it's ok to zero-extend it without worrying about clobbering.
        CVTSQ2SS(rr, ra);   // convert int64 to double
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        MOVLR(ra, ra);      // zero extend u32 to int64
        freeResourcesOf(ins);
    }
    
    void Assembler::asm_i2f(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isF() && a->isI());

        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, GpRegs);
        CVTSI2SS(rr, ra);   // cvtsi2ss xmmr, b  only writes xmm:0:32
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        freeResourcesOf(ins);
    }
    
    void Assembler::asm_f2i(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isI() && a->isF());

        Register rr = prepareResultReg(ins, GpRegs);
        Register rb = findRegFor(a, FpRegs);
        CVTTSS2SI(rr, rb); 
        freeResourcesOf(ins);
    }

    void Assembler::asm_f2f4(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isF4() && a->isF());

        Register rr = prepareResultReg(ins, FpRegs);
        Register rb = findRegFor(a, FpRegs);
        PSHUFD(rr, rb, PSHUFD_MASK(0, 0, 0, 0)); 
        freeResourcesOf(ins);
    }

    void Assembler::asm_ffff2f4(LIns *ins) {
        LIns *x = ins->oprnd1();
        LIns *y = ins->oprnd2();
        LIns *z = ins->oprnd3();
        LIns *w = ins->oprnd4();
        NanoAssert(ins->isF4() && x->isF() && y->isF() && z->isF() && w->isF());
        
        /* we need a temp register because we're not supposed to change the values
        of the registers associated with input operands. */
        Register rr = prepareResultReg(ins, FpRegs);
        Register rt = _allocator.allocTempReg(FpRegs & ~rmask(rr) );
        UNPCKLPS(rr,rt);// x y z w
        Register rw = findRegFor(w, FpRegs & ~(rmask(rt) | rmask(rr)));
        UNPCKLPS(rt,rw);// y w y w
        Register ry = findRegFor(y, FpRegs & ~(rmask(rt) | rmask(rr)));
        MOVAPSR(rt, ry);
        Register rz = findRegFor(z, FpRegs & ~rmask(rr));
        UNPCKLPS(rr,rz);// x z x z
        freeResourcesOf(ins);
        Register rx = x->isInReg() ? findRegFor(x, FpRegs) : findSpecificRegForUnallocated(x, rr);
        if (rx != rr) 
            MOVAPSR(rr, rx);
    }
    
    void Assembler::asm_f4comp(LIns *ins) {
        NanoAssert(ins->isop(LIR_swzf4) ? ins->isF4() : ins->isF());
        NanoAssert(ins->oprnd1()->isF4());
        LIns *a = ins->oprnd1();
        Register rr = prepareResultReg(ins, FpRegs);
        Register rb = findRegFor(a, FpRegs);
        switch (ins->opcode()) {
        default: NanoAssert(!"bad opcode for asm_f4comp()"); break;
        case LIR_f4x: PSHUFD(rr, rb, PSHUFD_MASK(0, 0, 0, 0)); break;
        case LIR_f4y: PSHUFD(rr, rb, PSHUFD_MASK(1, 1, 1, 1)); break;
        case LIR_f4z: PSHUFD(rr, rb, PSHUFD_MASK(2, 2, 2, 2)); break;
        case LIR_f4w: PSHUFD(rr, rb, PSHUFD_MASK(3, 3, 3, 3)); break;
        case LIR_swzf4: {
            uint8_t mask = ins->mask();
            PSHUFD(rr, rb, mask);
        }
        }
        freeResourcesOf(ins);
    }

    void Assembler::asm_f2d(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isD() && a->isF());

        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, FpRegs);
        CVTSS2SD(rr, ra);   // cvtss2sd xmmr, b  only writes xmm:0:64
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        freeResourcesOf(ins);
    }
    
    void Assembler::asm_d2f(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isF() && a->isD());

        Register rr = prepareResultReg(ins, FpRegs);
        Register ra = findRegFor(a, FpRegs);
        CVTSD2SS(rr, ra);   // cvtsd2ss xmmr, b  only writes xmm:0:32
        XORPS(rr);          // xorps xmmr,xmmr to break dependency chains
        freeResourcesOf(ins);
    }

    void Assembler::asm_d2i(LIns *ins) {
        LIns *a = ins->oprnd1();
        NanoAssert(ins->isI() && a->isD());

        Register rr = prepareResultReg(ins, GpRegs);
        Register rb = findRegFor(a, FpRegs);
        CVTTSD2SI(rr, rb); 
        freeResourcesOf(ins);
    }

    void Assembler::asm_cmov(LIns *ins) {
        LIns* cond    = ins->oprnd1();
        LIns* iftrue  = ins->oprnd2();
        LIns* iffalse = ins->oprnd3();
        NanoAssert(cond->isCmp());
        NanoAssert((ins->isop(LIR_cmovi) && iftrue->isI() && iffalse->isI())  ||
                   (ins->isop(LIR_cmovq) && iftrue->isQ() && iffalse->isQ())  ||
                   (ins->isop(LIR_cmovf) && iftrue->isF() && iffalse->isF())  ||
                   (ins->isop(LIR_cmovf4)&& iftrue->isF4()&& iffalse->isF4()) ||
                   (ins->isop(LIR_cmovd) && iftrue->isD() && iffalse->isD())  );

        bool isFloatOp = ins->isD() || ins->isF() || ins->isF4();
        RegisterMask allow = isFloatOp ? FpRegs : GpRegs;

        Register rr = prepareResultReg(ins, allow);

        Register rf = findRegFor(iffalse, allow & ~rmask(rr));

        if ( isFloatOp ) {
            // See Nativei386.cpp:asm_cmov() for an explanation of the subtleties here.
            NIns* target = _nIns;
            asm_nongp_copy(rr, rf);
            asm_branch_helper(false, cond, target);

            // If 'iftrue' isn't in a register, it can be clobbered by 'ins'.
            Register rt = iftrue->isInReg() ? iftrue->getReg() : rr;

            if (rr != rt)
                asm_nongp_copy(rr, rt);

            freeResourcesOf(ins);
            if (!iftrue->isInReg()) {
                NanoAssert(rt == rr);
                findSpecificRegForUnallocated(iftrue, rr);
            }

            asm_cmp(cond);
            return;
        }

        // If 'iftrue' isn't in a register, it can be clobbered by 'ins'.
        Register rt = iftrue->isInReg() ? iftrue->getReg() : rr;

        // WARNING: We cannot generate any code that affects the condition
        // codes between the MRcc generation here and the asm_cmpi() call
        // below.  See asm_cmpi() for more details.
        LOpcode condop = cond->opcode();
        if (ins->isop(LIR_cmovi)) {
            switch (condop) {
            case LIR_eqi:  case LIR_eqq:    CMOVNE( rr, rf);  break;
            case LIR_lti:  case LIR_ltq:    CMOVNL( rr, rf);  break;
            case LIR_gti:  case LIR_gtq:    CMOVNG( rr, rf);  break;
            case LIR_lei:  case LIR_leq:    CMOVNLE(rr, rf);  break;
            case LIR_gei:  case LIR_geq:    CMOVNGE(rr, rf);  break;
            case LIR_ltui: case LIR_ltuq:   CMOVNB( rr, rf);  break;
            case LIR_gtui: case LIR_gtuq:   CMOVNA( rr, rf);  break;
            case LIR_leui: case LIR_leuq:   CMOVNBE(rr, rf);  break;
            case LIR_geui: case LIR_geuq:   CMOVNAE(rr, rf);  break;
            default:                        NanoAssert(0);    break;
            }
        } else {
            NanoAssert(ins->isop(LIR_cmovq));
            switch (condop) {
            case LIR_eqi:  case LIR_eqq:    CMOVQNE( rr, rf); break;
            case LIR_lti:  case LIR_ltq:    CMOVQNL( rr, rf); break;
            case LIR_gti:  case LIR_gtq:    CMOVQNG( rr, rf); break;
            case LIR_lei:  case LIR_leq:    CMOVQNLE(rr, rf); break;
            case LIR_gei:  case LIR_geq:    CMOVQNGE(rr, rf); break;
            case LIR_ltui: case LIR_ltuq:   CMOVQNB( rr, rf); break;
            case LIR_gtui: case LIR_gtuq:   CMOVQNA( rr, rf); break;
            case LIR_leui: case LIR_leuq:   CMOVQNBE(rr, rf); break;
            case LIR_geui: case LIR_geuq:   CMOVQNAE(rr, rf); break;
            default:                        NanoAssert(0);    break;
            }
        }
        if (rr != rt)
            MR(rr, rt);

        freeResourcesOf(ins);
        if (!iftrue->isInReg()) {
            NanoAssert(rt == rr);
            findSpecificRegForUnallocated(iftrue, rr);
        }

        asm_cmpi(cond);
    }

    Branches Assembler::asm_branch(bool onFalse, LIns* cond, NIns* target) {
        Branches branches = asm_branch_helper(onFalse, cond, target);
        asm_cmp(cond);
        return branches;
    }

	// If target address is unknown, i.e., a backward branch, allow for 64-bit address.
	// Formerly, a 32-bit displacement would do, but layout randomization may result in
	// larger displacements if a function is split between two randomly-placed regions.
	
    Branches Assembler::asm_branch_helper(bool onFalse, LIns *cond, NIns *target) {

        // Float or double. Float4 is handled by the integer path.
        if (isCmpDOpcode(cond->opcode()) || isCmpFOpcode(cond->opcode())) {
			return asm_branchd_helper(onFalse, cond, target);
		}

		// Integer
		NIns* patch = NULL;
        if (target && isTargetWithinS8(target)) {
			patch = asm_branchi_S8(onFalse, cond, target);
		} else if (target && isTargetWithinS32(target)) {
			patch = asm_branchi_S32(onFalse, cond, target);
		} else {
			// A conditional jump beyond 32-bit range, so invert the
			// branch/compare and emit an unconditional jump to the target:
			//         j(inverted) B1
			//         jmp target
			//     B1:
			underrunProtect(22);	// 14 bytes for JMP64 + 8 bytes (incl. overhang) for branchi helper
			NIns* skip = _nIns;
			JMP64(16, target);		// 6 + 8 bytes (16)
			patch = _nIns;
			// Generate an 8-bit branch to a target that does not need to be patched.  Needs 8 bytes max.
			asm_branchi_S8(!onFalse, cond, skip);
		}
        return Branches(patch);
    }

    NIns* Assembler::asm_branchi_S8(bool onFalse, LIns *cond, NIns *target) {
		LOpcode condop = cond->opcode();
		if (onFalse) {
			switch (condop) {
			case LIR_eqf4:
			case LIR_eqi:  case LIR_eqq:    JNE8( 8, target); break;
			case LIR_lti:  case LIR_ltq:    JNL8( 8, target); break;
			case LIR_gti:  case LIR_gtq:    JNG8( 8, target); break;
			case LIR_lei:  case LIR_leq:    JNLE8(8, target); break;
			case LIR_gei:  case LIR_geq:    JNGE8(8, target); break;
			case LIR_ltui: case LIR_ltuq:   JNB8( 8, target); break;
			case LIR_gtui: case LIR_gtuq:   JNA8( 8, target); break;
			case LIR_leui: case LIR_leuq:   JNBE8(8, target); break;
			case LIR_geui: case LIR_geuq:   JNAE8(8, target); break;
			default:                        NanoAssert(0);    break;
			}
		} else {
			switch (condop) {
			case LIR_eqf4:
			case LIR_eqi:  case LIR_eqq:    JE8( 8, target);  break;
			case LIR_lti:  case LIR_ltq:    JL8( 8, target);  break;
			case LIR_gti:  case LIR_gtq:    JG8( 8, target);  break;
			case LIR_lei:  case LIR_leq:    JLE8(8, target);  break;
			case LIR_gei:  case LIR_geq:    JGE8(8, target);  break;
			case LIR_ltui: case LIR_ltuq:   JB8( 8, target);  break;
			case LIR_gtui: case LIR_gtuq:   JA8( 8, target);  break;
			case LIR_leui: case LIR_leuq:   JBE8(8, target);  break;
			case LIR_geui: case LIR_geuq:   JAE8(8, target);  break;
			default:                        NanoAssert(0);    break;
			}
		}
		return _nIns;
	}

    NIns* Assembler::asm_branchi_S32(bool onFalse, LIns *cond, NIns *target) {
		LOpcode condop = cond->opcode();
		if (onFalse) {
			switch (condop) {
			case LIR_eqf4:
			case LIR_eqi:  case LIR_eqq:    JNE( 8, target);  break;
			case LIR_lti:  case LIR_ltq:    JNL( 8, target);  break;
			case LIR_gti:  case LIR_gtq:    JNG( 8, target);  break;
			case LIR_lei:  case LIR_leq:    JNLE(8, target);  break;
			case LIR_gei:  case LIR_geq:    JNGE(8, target);  break;
			case LIR_ltui: case LIR_ltuq:   JNB( 8, target);  break;
			case LIR_gtui: case LIR_gtuq:   JNA( 8, target);  break;
			case LIR_leui: case LIR_leuq:   JNBE(8, target);  break;
			case LIR_geui: case LIR_geuq:   JNAE(8, target);  break;
			default:                        NanoAssert(0);    break;
			}
		} else {
			switch (condop) {
			case LIR_eqf4:
			case LIR_eqi:  case LIR_eqq:    JE( 8, target);   break;
			case LIR_lti:  case LIR_ltq:    JL( 8, target);   break;
			case LIR_gti:  case LIR_gtq:    JG( 8, target);   break;
			case LIR_lei:  case LIR_leq:    JLE(8, target);   break;
			case LIR_gei:  case LIR_geq:    JGE(8, target);   break;
			case LIR_ltui: case LIR_ltuq:   JB( 8, target);   break;
			case LIR_gtui: case LIR_gtuq:   JA( 8, target);   break;
			case LIR_leui: case LIR_leuq:   JBE(8, target);   break;
			case LIR_geui: case LIR_geuq:   JAE(8, target);   break;
			default:                        NanoAssert(0);    break;
			}
		}
		return _nIns;
    }

    NIns* Assembler::asm_branch_ov(LOpcode, NIns* target) {
        // We must ensure there's room for the instr before calculating
        // the offset.  And the offset determines the opcode (8bit or 32bit).
        if (target && isTargetWithinS8(target)) {
            JO8(8, target);
		} else if (target && isTargetWithinS32(target)) {
            JO(8, target);
		} else {
			underrunProtect(22);
            NIns* skip = _nIns;
            JMP64(16, target);				// 6 + 8 bytes (16)
			JNO8(8, skip);					// 2 bytes (8)
		}
		return _nIns;
    }

    void Assembler::asm_pushstate()
    {
        SUBQRI(RSP, 32);
        PUSHR(R15);
        PUSHR(R14);
        PUSHR(R13);
        PUSHR(R12);
        PUSHR(R11);
        PUSHR(R10);
        PUSHR(R9);
        PUSHR(R8);
        PUSHR(RDI);
        PUSHR(RSI);
        PUSHR(RBP);
        PUSHR(RBX); // RSP
        PUSHR(RBX);
        PUSHR(RDX);
        PUSHR(RCX);
        PUSHR(RAX);
    }
    
    void Assembler::asm_popstate()
    {
        POPR(RAX);
        POPR(RCX);
        POPR(RDX);
        POPR(RBX);
        POPR(RBX); // RSP
        POPR(RBP);
        POPR(RSI);
        POPR(RDI);
        POPR(R8);
        POPR(R9);
        POPR(R10);
        POPR(R11);
        POPR(R12);
        POPR(R13);
        POPR(R14);
        POPR(R15);
        ADDQRI(RSP, 32);
    }

    void Assembler::asm_brsavpc_impl(LIns* flag, NIns* target)
    {
        Register r = findRegFor(flag, GpRegs);
        underrunProtect(37);
    
        // discard pc
        ADDQR8(RSP, 16);  				// 4 bytes (8)
        
        // handle interrupt call
        // The length of this instruction sequence (16) is hardcoded into asm_restorepc below!
        NIns* skip = _nIns;
        JMP64(16, target);				// 6 + 8 bytes (16)
        JE8(8, skip);					// 2 bytes (8)
        
        // save pc
        emit(X64_call);					// call with displacement 0, 5 bytes (8)
        
        CMPQR8(r, 0);					// 4 bytes (8)
        SUBQR8(RSP, 8);					// 4 bytes (8)
    }

    void Assembler::asm_restorepc()
    {
        underrunProtect(11);
        // jmp dword ptr [rsp]
        emit(0x2424FF0000000003LL);   // 3 bytes (8)
        // add qword ptr [rsp], 16
        // The constant '16' is the size of the branch to the interrupt label
        // in the epilogue, which is emitted in asm_brsavpc_impl above.
        emit(0x1000244483480006LL);   // 6 bytes (8)
    }

	void Assembler::asm_memfence()
	{
		// no fencing necessary on x64
	}

    void Assembler::asm_cmp(LIns *cond) {
      if (isCmpF4Opcode(cond->opcode()))
          asm_cmpf4(cond);
      else if (isCmpFOpcode(cond->opcode()) || isCmpDOpcode(cond->opcode()))
          asm_cmpd(cond);
      else
          asm_cmpi(cond);
    }

    // WARNING: this function cannot generate code that will affect the
    // condition codes prior to the generation of the test/cmp.  See
    // Nativei386.cpp:asm_cmpi() for details.
    void Assembler::asm_cmpi(LIns *cond) {
        LIns *b = cond->oprnd2();
        if (isImm32(b) && !(b->isTainted() && shouldBlind(getImm32(b)))) {
            asm_cmpi_imm(cond);
            return;
        }
        LIns *a = cond->oprnd1();
        Register ra, rb;
        if (a != b) {
            findRegFor2(GpRegs, a, ra, GpRegs, b, rb);
        } else {
            // optimize-me: this will produce a const result!
            ra = rb = findRegFor(a, GpRegs);
        }

        LOpcode condop = cond->opcode();
        if (isCmpQOpcode(condop)) {
            CMPQR(ra, rb);
        } else {
            NanoAssert(isCmpIOpcode(condop));
            CMPLR(ra, rb);
        }
    }

    void Assembler::asm_cmpi_imm(LIns *cond) {
        LOpcode condop = cond->opcode();
        LIns *a = cond->oprnd1();
        LIns *b = cond->oprnd2();
        Register ra = findRegFor(a, GpRegs);
        int32_t imm = getImm32(b);
        if (isCmpQOpcode(condop)) {
            if (isS8(imm))
                CMPQR8(ra, imm);
            else
                CMPQRI(ra, imm);
        } else {
            NanoAssert(isCmpIOpcode(condop));
            if (isS8(imm))
                CMPLR8(ra, imm);
            else
                CMPLRI(ra, imm);
        }
    }

    // Compiling floating point branches.
    // Discussion in https://bugzilla.mozilla.org/show_bug.cgi?id=443886.
    //
    //  fucom/p/pp: c3 c2 c0   jae ja    jbe jb je jne
    //  ucomisd:     Z  P  C   !C  !C&!Z C|Z C  Z  !Z
    //              -- -- --   --  ----- --- -- -- --
    //  unordered    1  1  1             T   T  T
    //  greater >    0  0  0   T   T               T
    //  less    <    0  0  1             T   T     T
    //  equal   =    1  0  0   T         T      T
    //
    //  Here are the cases, using conditionals:
    //
    //  branch  >=  >   <=       <        =
    //  ------  --- --- ---      ---      ---
    //  LIR_jt  jae ja  swap+jae swap+ja  jp over je
    //  LIR_jf  jb  jbe swap+jb  swap+jbe jne+jp

    Branches Assembler::asm_branchd_helper(bool onFalse, LIns *cond, NIns *target) {
        LOpcode condop = cond->opcode();
        NIns *patch1 = NULL;
        NIns *patch2 = NULL;
        if (isCmpFOpcode(condop))
            condop = getCmpDOpcode(condop);
        NanoAssert(condop != LIR_eqf4); // handled in asm_branchi_helper
        if (condop == LIR_eqd) {
            if (onFalse) {
                // branch if unordered or !=
				underrunProtect(14);		// ensure we have space for entire 32-bit branch sequence with overhang
				if (target && isTargetWithinS32(target)) {
					JP(8, target);			// 6 bytes (8)
					patch1 = _nIns;
					JNE(8, target);			// 6 bytes (8)
					patch2 = _nIns;
				} else {
					underrunProtect(38); 	// ensure we have space for entire 64-bit branch sequence with overhang
					NIns* skip1 = _nIns;
					JMP64(16, target);		// 6 + 8 bytes (16)
					patch1 = _nIns;
					JNP8(8, skip1);			// 2 bytes (8)
					NIns* skip2 = _nIns;
					JMP64(16, target);		// 6 + 8 bytes (16)
					patch2 = _nIns;
					JE8(8, skip2);			// 2 bytes (8)
				}
            } else {
				underrunProtect(14);		// ensure we have space for entire 32-bit branch sequence with overhang
				if (target && isTargetWithinS32(target)) {
					NIns *skip = _nIns;
					JE(8, target);			// 6 bytes (8)
					patch1 = _nIns;
					JP8(8, skip);			// 2 bytes (8)
				} else {
					underrunProtect(28); 	// ensure we have space for entire 64-bit branch sequence with overhang
					NIns *skip = _nIns;
					JMP64(16, target);		// 6 + 8 bytes (16)
					patch1 = _nIns;
					JNE8(8, skip);			// 6 bytes (8)
					JP8(8, skip);			// 2 bytes (8)
				}
            }
        }
        else if (target && isTargetWithinS32(target)) {
            // TODO: Use 8-bit branches where possible for branch to known target.
            // LIR_ltd and LIR_gtd are handled by the same case because
            // asm_cmpd() converts LIR_ltd(a,b) to LIR_gtd(b,a).  Likewise for
            // LIR_led/LIR_ged.
            switch (condop) {
            case LIR_ltd:
            case LIR_gtd: if (onFalse) JBE(8, target); else JA(8, target);  break;
            case LIR_led:
            case LIR_ged: if (onFalse) JB(8, target);  else JAE(8, target); break;
            default:      NanoAssert(0);                                    break;
            }
            patch1 = _nIns;
        } else {
            // Skip over long branch on inverted sense of comparison.
            underrunProtect(22);  // 14 bytes of JMP64 + 8 bytes Jcc (incl. overhang)
			NIns* skip = _nIns;
            JMP64(16, target);
			patch1 = _nIns;
            switch (condop) {
            case LIR_ltd:
            case LIR_gtd: if (onFalse) JA8(8, skip); else JBE8(8, skip);  break;
            case LIR_led:
            case LIR_ged: if (onFalse) JAE8(8, skip);  else JB8(8, skip); break;
            default:      NanoAssert(0);                                  break;
            }
        }
        return Branches(patch1, patch2);
    }

    void Assembler::asm_condd(LIns *ins) {
        LOpcode op = ins->opcode();
        if (isCmpFOpcode(op))
          op = getCmpDOpcode(op); // the only difference between float/double is in asm_cmpd
        if (op == LIR_eqd) {
            // result = ZF & !PF, must do logic on flags
            // r = al|bl|cl|dl, can only use rh without rex prefix
            Register r = prepareResultReg(ins, 1<<REGNUM(RAX) | 1<<REGNUM(RCX) |
                                               1<<REGNUM(RDX) | 1<<REGNUM(RBX));
            MOVZX8(r, r);       // movzx8   r,rl     r[8:63] = 0
            X86_AND8R(r);       // and      rl,rh    rl &= rh
            X86_SETNP(r);       // setnp    rh       rh = !PF
            X86_SETE(r);        // sete     rl       rl = ZF
        } else {
            // LIR_ltd and LIR_gtd are handled by the same case because
            // asm_cmpd() converts LIR_ltd(a,b) to LIR_gtd(b,a).  Likewise for
            // LIR_led/LIR_ged.
            Register r = prepareResultReg(ins, GpRegs); // x64 can use any GPR as setcc target
            MOVZX8(r, r);
            switch (op) {
            case LIR_ltd:
            case LIR_gtd: SETA(r);       break;
            case LIR_led:
            case LIR_ged: SETAE(r);      break;
            default:      NanoAssert(0); break;
            }
        }

        freeResourcesOf(ins);

        asm_cmpd(ins);
    }

    // WARNING: This function cannot generate any code that will affect the
    // condition codes prior to the generation of the ucomisd/ucomiss.  
    // See asm_cmpi() for more details.
    void Assembler::asm_cmpd(LIns *cond) {
        LOpcode opcode = cond->opcode();
        bool singlePrecision = isCmpFOpcode(opcode);
        if (singlePrecision)
           opcode = getCmpDOpcode(opcode);
        LIns* a = cond->oprnd1();
        LIns* b = cond->oprnd2();
        // First, we convert (a < b) into (b > a), and (a <= b) into (b >= a).
        // For float4, we don't have these ops, at all.
        if (opcode == LIR_ltd) {
            opcode = LIR_gtd;
            LIns* t = a; a = b; b = t;
        } else if (opcode == LIR_led) {
            opcode = LIR_ged;
            LIns* t = a; a = b; b = t;
        }
        Register ra, rb;
        findRegFor2(FpRegs, a, ra, FpRegs, b, rb);
        if (singlePrecision)
            UCOMISS(ra, rb);
        else 
            UCOMISD(ra, rb);
    }

    void Assembler::asm_condf4(LIns *cond) {
        NanoAssert(cond->opcode() == LIR_eqf4);
        // unlike x86-32, with a rex prefix we can use any GP register as an 8bit target
        Register r = prepareResultReg(cond, GpRegs);

        // SETcc only sets low 8 bits, so extend
        MOVZX8(r, r);
        SETE(r);
        freeResourcesOf(cond);

        asm_cmpf4(cond);
    }

    // WARNING: This function cannot generate any code that will affect the
    // condition codes prior to the generation of the ucomisd.  See asm_cmpi()
    // for more details.
    void Assembler::asm_cmpf4(LIns *cond) {
        NanoAssert((cond->opcode()==LIR_eqf4) );
        LIns* a = cond->oprnd1();
        LIns* b = cond->oprnd2();

        Register gt = _allocator.allocTempReg(GpRegs);
        /*
            CMPNEQPS ra, rb  // CMPPS ra,rb,4
            // we could use PTEST but it's SSE4_1, we don't want to assume SSE4 support yet 
            PMOVMSKB gp, ra
            CMP      gp, 0
        */
       CMPLR8(gt, 0);
       Register rt = _allocator.allocTempReg(FpRegs);
       PMOVMSKB(gt,rt);
       Register ra, rb;
       
       findRegFor2(FpRegs & ~rmask(rt), a, ra, FpRegs & ~rmask(rt), b, rb);
       CMPNEQPS(rt,rb);
       if(ra!=rt)
           asm_nongp_copy(rt,ra);

    }

    // Return true if we can generate code for this instruction that neither
    // sets CCs nor clobbers any input register.
    // LEA is the only native instruction that fits those requirements.
    bool canRematLEA(LIns* ins)
    {
        // We cannot rematerialize tainted (blinded) integer literals, as the XOR
        // instruction used to synthesize the constant value may alter the CCs.
        switch (ins->opcode()) {
        case LIR_addi:
            return ins->oprnd1()->isInRegMask(BaseRegs) && ins->oprnd2()->isImmI() &&
                   !(ins->oprnd2()->isTainted() && shouldBlind(ins->oprnd2()->immI()));
        case LIR_addq: {
            LIns* rhs;
            return ins->oprnd1()->isInRegMask(BaseRegs) &&
                   (rhs = ins->oprnd2())->isImmQ() &&
                   isS32(rhs->immQ()) &&
                   !(rhs->isTainted() && shouldBlind(rhs->immQ()));
        }
        // Subtract and some left-shifts could be rematerialized using LEA,
        // but it hasn't shown to help in real code yet.  Noting them anyway:
        // maybe sub? R = subl/q rL, const  =>  leal/q R, [rL + -const]
        // maybe lsh? R = lshl/q rL, 1/2/3  =>  leal/q R, [rL * 2/4/8]
        default:
            ;
        }
        return false;
    }

    bool RegAlloc::canRemat(LIns* ins) {
        // We cannot rematerialize tainted (blinded) integer literals, as the XOR instruction
        // used to synthesize the constant value may alter the CCs.  See asm_restore() below.
        return (ins->isImmAny() &&
                !(ins->isImmI() && ins->isTainted() && shouldBlind(ins->immI())) &&
                !(ins->isImmQ() && ins->isTainted() && shouldBlind(ins->immQ())))
               || ins->isop(LIR_allocp) || canRematLEA(ins);
    }

    // WARNING: the code generated by this function must not affect the
    // condition codes.  See asm_cmpi() for details.
    void Assembler::asm_restore(LIns *ins, Register r) {
        if (ins->isop(LIR_allocp)) {
            int d = arDisp(ins);
            LEAQRM(r, d, FP);
        }
        else if (ins->isImmI() && !(ins->isTainted() && shouldBlind(ins->immI()))) {
            // We cannot rematerialize most tainted (blinded) literals, as the XOR
            // instruction used to synthesize the constant value may alter the CCs.
            // Float4 literals may be loaded from a constant pool, so they are not
            // subject to the restriction. We could likewise provide a pool for the
            // scalar types, which would be more efficient than a spill/reload.
            asm_immi(r, ins->immI(), /*canClobberCCs*/false, /*blind*/false);
        }
        else if (ins->isImmQ() && !(ins->isTainted() && shouldBlind(ins->immQ()))) {
            asm_immq(r, ins->immQ(), /*canClobberCCs*/false, /*blind*/false);
        }
        else if (ins->isImmD() && !ins->isTainted()) {
            asm_immd(r, ins->immDasQ(), /*canClobberCCs*/false, /*blind*/false);
        }
        else if (ins->isImmF() && !ins->isTainted()) {
            asm_immf(r, ins->immFasI(), /*canClobberCCs*/false, /*blind*/false);
        }
        else if (ins->isImmF4()) {
            asm_immf4(r, ins->immF4(), /*canClobberCCs*/false, ins->isTainted());
        }
        else if (canRematLEA(ins)) {
            Register lhsReg = ins->oprnd1()->getReg();
            if (ins->isop(LIR_addq))
                LEAQRM(r, (int32_t)ins->oprnd2()->immQ(), lhsReg);
            else // LIR_addi
                LEALRM(r, ins->oprnd2()->immI(), lhsReg);
        }
        else {
            int d = findMemFor(ins);
            if (ins->isD()) {
                NanoAssert(IsFpReg(r));
                MOVSDRM(r, d, FP);
            } else if (ins->isQ()) {
                NanoAssert(IsGpReg(r));
                MOVQRM(r, d, FP);
            } else if (ins->isF()) {
                NanoAssert(IsFpReg(r));
                MOVSSRM(r, d, FP);
            } else if (ins->isF4()) {
                NanoAssert(IsFpReg(r));
                MOVUPSRM(r, d, FP);
            } else {
                NanoAssert(ins->isI());
                MOVLRM(r, d, FP);
            }
        }
    }

    void Assembler::asm_cond(LIns *ins) {
        LOpcode op = ins->opcode();

        // unlike x86-32, with a rex prefix we can use any GP register as an 8bit target
        Register r = prepareResultReg(ins, GpRegs);

        // SETcc only sets low 8 bits, so extend
        MOVZX8(r, r);
        switch (op) {
        default:
            TODO(cond);
        case LIR_eqq:
        case LIR_eqi:    SETE(r);    break;
        case LIR_ltq:
        case LIR_lti:    SETL(r);    break;
        case LIR_leq:
        case LIR_lei:    SETLE(r);   break;
        case LIR_gtq:
        case LIR_gti:    SETG(r);    break;
        case LIR_geq:
        case LIR_gei:    SETGE(r);   break;
        case LIR_ltuq:
        case LIR_ltui:   SETB(r);    break;
        case LIR_leuq:
        case LIR_leui:   SETBE(r);   break;
        case LIR_gtuq:
        case LIR_gtui:   SETA(r);    break;
        case LIR_geuq:
        case LIR_geui:   SETAE(r);   break;
        }
        freeResourcesOf(ins);

        asm_cmpi(ins);
    }

    void Assembler::asm_ret(LIns *ins) {
        genEpilogue();

        // Restore RSP from RBP, undoing SUB(RSP,amt) in the prologue
        MR(RSP,FP);

        releaseRegisters();
        assignSavedRegs();
        LIns *value = ins->oprnd1();
        Register r = ins->isop(LIR_retd) || ins->isop(LIR_retf) || ins->isop(LIR_retf4) ? XMM0 : RAX;
        findSpecificRegFor(value, r);
    }

    RegisterMask RegAlloc::nRegCopyCandidates(Register r, RegisterMask allow) {
        (void) r;
        return allow; // can freely transfer registers among different classes
    }
    
    void Assembler::asm_nongp_copy(Register d, Register s) {
        if (!IsFpReg(d) && IsFpReg(s)) {
            // gpr <- xmm: use movq r/m64, xmm (66 REX.W 0F 7E /r)
            MOVQRX(d, s);
        } else if (IsFpReg(d) && IsFpReg(s)) {
            // xmm <- xmm: use movaps. movsd r,r causes partial register stall
            MOVAPSR(d, s);
        } else {
            NanoAssert(IsFpReg(d) && !IsFpReg(s));
            // xmm <- gpr: use movq xmm, r/m64 (66 REX.W 0F 6E /r)
            MOVQXR(d, s);
        }
    }

    // Register setup for load ops.  Pairs with endLoadRegs().
    void Assembler::beginLoadRegs(LIns *ins, RegisterMask allow, Register &rr, int32_t &dr, Register &rb, Register &orb) {
        dr = ins->disp();
        LIns *base = ins->oprnd1();
		bool force = forceDisplacementBlinding(ins->isTainted());
		// Allocation of r must precede getBaseRegWithBlinding(), as the latter may allocate a temporary register.
		// Once a temporary has been allocated, no other allocations (within an overlapping regclass) may occur until the temporary is dead.
        rr = prepareResultReg(ins, allow);
        rb = getBaseRegWithBlinding(base, dr, BaseRegs & ~rmask(rr), ins->isTainted(), force, &orb);
    }

    // Register clean-up for load ops.  Pairs with beginLoadRegs().
    void Assembler::endLoadRegs(LIns* ins, Register rb, Register orb) {
		adjustBaseRegForBlinding(rb, orb);
        freeResourcesOf(ins);
    }

    void Assembler::asm_load64(LIns *ins) {
        Register rr, rb, orb;
        int32_t dr;
        switch (ins->opcode()) {
            case LIR_ldq:
                beginLoadRegs(ins, GpRegs, rr, dr, rb, orb);
                NanoAssert(IsGpReg(rr));
                MOVQRM(rr, dr, rb);     // general 64bit load, 32bit const displacement
                break;
            case LIR_ldd:
                beginLoadRegs(ins, FpRegs, rr, dr, rb, orb);
                NanoAssert(IsFpReg(rr));
                MOVSDRM(rr, dr, rb);    // load 64bits into XMM
                break;
            case LIR_ldf:
                beginLoadRegs(ins, FpRegs, rr, dr, rb, orb);
                NanoAssert(IsFpReg(rr));
                MOVSSRM(rr, dr, rb);
                break;
            case LIR_ldf2d:
                beginLoadRegs(ins, FpRegs, rr, dr, rb, orb);
                NanoAssert(IsFpReg(rr));
                CVTSS2SD(rr, rr);
                MOVSSRM(rr, dr, rb);
                break;
            default:
                NanoAssertMsg(0, "asm_load64 should never receive this LIR opcode");
                break;
        }
        endLoadRegs(ins, rb, orb);
    }

    void Assembler::asm_load128(LIns *ins) {
        Register rr, rb, orb;
        int32_t dr;
        NanoAssert(ins->opcode() == LIR_ldf4);
        
        beginLoadRegs(ins, FpRegs, rr, dr, rb, orb);
        NanoAssert(IsFpReg(rr));
        MOVUPSRM(rr,dr,rb);
        endLoadRegs(ins, rb, orb);
    }

    void Assembler::asm_load32(LIns *ins) {
        NanoAssert(ins->isI());
        Register r, b, ob;
        int32_t d;
        beginLoadRegs(ins, GpRegs, r, d, b, ob);
        LOpcode op = ins->opcode();
        switch (op) {
            case LIR_lduc2ui:
                MOVZX8M( r, d, b);
                break;
            case LIR_ldus2ui:
                MOVZX16M(r, d, b);
                break;
            case LIR_ldi:
                MOVLRM(  r, d, b);
                break;
            case LIR_ldc2i:
                MOVSX8M( r, d, b);
                break;
            case LIR_lds2i:
                MOVSX16M( r, d, b);
                break;
            default:
                NanoAssertMsg(0, "asm_load32 should never receive this LIR opcode");
                break;
        }
        endLoadRegs(ins, b, ob);
    }

    void Assembler::asm_immf(Register r, uint32_t v, bool canClobberCCs, bool blind) {
        NanoAssert(IsFpReg(r));
        if (v == 0 && canClobberCCs) {
            XORPS(r);
        } else {
            // There's no general way to load an immediate into an XMM reg.
            // For non-zero floats the best thing is to put the equivalent
            // 64-bit integer into a scratch GpReg and then move it into the
            // appropriate FpReg.
            // TODO: When blinding is enabled, asm_immq() can be rather costly.
            // We may be better off loading from a pool here.
            Register rt = _allocator.allocTempReg(GpRegs);
            MOVDXR(r, rt);
            asm_immi(rt, v, canClobberCCs, blind);
        }
    }
    
    void Assembler::asm_immf4(Register r, float4_t v, bool canClobberCCs, bool blind) {
        NanoAssert(IsFpReg(r));
        union{
            float4_t f4;
            int64_t bits64[2];
        }fval; fval.f4 = v; 

        int64_t v0= fval.bits64[0], v1=fval.bits64[1];
        if (v0 == 0 && v1 == 0 && canClobberCCs) {
            XORPS(r);
        }   else {
            if(v1==0 && !blind){
                asm_immd(r, v0, canClobberCCs, /*blind*/false);
            } else {
                const float4_t* vaddr = findImmF4FromPool(v);
                bool is_aligned = ( ((uintptr_t)vaddr) & 0xf ) == 0;
                /* 
                    We must be sure that MOVAPSRMRIP/MOVAPSRMRIP does NOT cross into a new page.
                    Hence - isTargetWithinS32 has to make room for 12 bytes (not 8), because
                    emit_disp32() makes room for displacement (4 bytes) + full-size op (8 bytes)
                */
                if( isTargetWithinS32((NIns*)vaddr, 12 ) ) {
                    int32_t d = int32_t(int64_t(vaddr)-int64_t(_nIns));
                    is_aligned? MOVAPSRMRIP(r, d):MOVUPSRMRIP(r, d);
                } else {
                    Register gp = _allocator.allocTempReg(GpRegs);
                    is_aligned? MOVAPSRM(r, 0, gp): MOVUPSRM(r,0,gp);
                    asm_immq(gp, (uint64_t) vaddr, canClobberCCs, /*blind*/false);
                }
            }
        }
    }

    void Assembler::asm_store128(LOpcode op, LIns *value, int d, LIns *base, bool tainted) {
        NanoAssert((value->isF4() && (op==LIR_stf4)) ); (void) op;

		bool force = forceDisplacementBlinding(tainted);
		Register ob;
		// NOTE: fpRegs are disjoint from BaseRegs
        Register r = findRegFor(value, FpRegs);
		Register b = getBaseRegWithBlinding(base, d, BaseRegs, tainted, force, &ob);
        MOVUPSMR(r, d, b);
		adjustBaseRegForBlinding(b, ob);
    }

    void Assembler::asm_store64(LOpcode op, LIns *value, int d, LIns *base, bool tainted) {
        // This function also handles stf (store-float-32) because its more
        // convenient to do it here than asm_store32, which only handles GP registers.
        NanoAssert(op == LIR_stf ? value->isF() : value->isQorD());
		bool force = forceDisplacementBlinding(tainted);
        switch (op) {
            case LIR_stq: {
                uint64_t c;
                if (value->isImmQ() && (c = value->immQ(), isS32(c)) && !(value->isTainted() && shouldBlind(c))) {
					force = force || tainted; // If the store is tainted, and we are not going to blind the immediate, then blind the displacement.
                    uint64_t c = value->immQ();
					Register orb;
                    Register rb = getBaseRegWithBlinding(base, d, BaseRegs, tainted, force, &orb);
                    // MOVQMI takes a 32-bit integer that gets signed extended to a 64-bit value.
                    MOVQMI(rb, d, int32_t(c));
					adjustBaseRegForBlinding(rb, orb);
                } else {
                    Register rr, rb, orb;
                    getBaseReg2WithBlinding(GpRegs, value, rr, BaseRegs, base, rb, d, tainted, force, &orb);
                    MOVQMR(rr, d, rb);    // gpr store
					adjustBaseRegForBlinding(rb, orb);
                }
                break;
            }
            case LIR_std: {
				Register ob;
                Register r = findRegFor(value, FpRegs);
                Register b = getBaseRegWithBlinding(base, d, BaseRegs, tainted, force, &ob);
                MOVSDMR(r, d, b);   // xmm store
				adjustBaseRegForBlinding(b, ob);
                break;
            }
            case LIR_stf:{
				Register ob;
                Register r = findRegFor(value, FpRegs);
                Register b = getBaseRegWithBlinding(base, d, BaseRegs, tainted, force, &ob);
                MOVSSMR(r, d, b);   // store
				adjustBaseRegForBlinding(b, ob);
                break;
            }
            case LIR_std2f: {
				Register ob;
                Register r = findRegFor(value, FpRegs);
                Register b = getBaseRegWithBlinding(base, d, BaseRegs, tainted, force, &ob);
                Register t = _allocator.allocTempReg(FpRegs & ~rmask(r));
                MOVSSMR(t, d, b);   // store
                CVTSD2SS(t, r);     // cvt to single-precision
                XORPS(t);           // break dependency chains
				adjustBaseRegForBlinding(b, ob);
                break;
            }
            default:
                NanoAssertMsg(0, "asm_store64 should never receive this LIR opcode");
                break;
        }
    }

    void Assembler::asm_store32(LOpcode op, LIns *value, int d, LIns *base, bool tainted) {
		bool force = forceDisplacementBlinding(tainted);
        if (value->isImmI() && !(value->isTainted() && shouldBlind(value->immI()))) {
			force = force || tainted; // If the store is tainted, and we are not going to blind the immediate, then blind the displacement.
            int c = value->immI();
			Register orb;
			Register rb = getBaseRegWithBlinding(base, d, BaseRegs, tainted, force, &orb);
            switch (op) {
                case LIR_sti2c: MOVBMI(rb, d, c); break;
                case LIR_sti2s: MOVSMI(rb, d, c); break;
                case LIR_sti:   MOVLMI(rb, d, c); break;
                default:        NanoAssert(0);    break;
            }
			adjustBaseRegForBlinding(rb, orb);
        } else {
            // Quirk of x86-64: reg cannot appear to be ah/bh/ch/dh for
            // single-byte stores with REX prefix.
            const RegisterMask SrcRegs = (op == LIR_sti2c) ? SingleByteStoreRegs : GpRegs;

            NanoAssert(value->isI());
			Register ob;
			// Allocation of r must precede getBaseRegWithBlinding(), as the latter may allocate a temporary register.
			// Once a temporary has been allocated, no other allocations (within an overlapping regclass) may occur until the temporary is dead.
            Register r = findRegFor(value, SrcRegs);
			Register b = getBaseRegWithBlinding(base, d, BaseRegs & ~rmask(r), tainted, force, &ob);
            switch (op) {
                case LIR_sti2c: MOVBMR(r, d, b); break;
                case LIR_sti2s: MOVSMR(r, d, b); break;
                case LIR_sti:   MOVLMR(r, d, b); break;
                default:        NanoAssert(0);   break;
            }
			adjustBaseRegForBlinding(b, ob);
        }
    }

    void Assembler::asm_immi(LIns *ins) {
        Register rr = prepareResultReg(ins, GpRegs);
        asm_immi(rr, ins->immI(), /*canClobberCCs*/true, ins->isTainted());
        freeResourcesOf(ins);
    }

    void Assembler::asm_immq(LIns *ins) {
        Register rr = prepareResultReg(ins, GpRegs);
        asm_immq(rr, ins->immQ(), /*canClobberCCs*/true, ins->isTainted());
        freeResourcesOf(ins);
    }

    void Assembler::asm_immd(LIns *ins) {
        Register r = prepareResultReg(ins, FpRegs);
        asm_immd(r, ins->immDasQ(), /*canClobberCCs*/true, ins->isTainted());
        freeResourcesOf(ins);
    }

    void Assembler::asm_immf(LIns *ins) {
        Register r = prepareResultReg(ins, FpRegs);
        asm_immf(r, ins->immFasI(), /*canClobberCCs*/true, ins->isTainted());
        freeResourcesOf(ins);
    }

    void Assembler::asm_immf4(LIns *ins) {
        Register r = prepareResultReg(ins, FpRegs);
        asm_immf4(r, ins->immF4(), /*canClobberCCs*/true, ins->isTainted());
        freeResourcesOf(ins);
    }

    void Assembler::asm_immi(Register r, int32_t v, bool canClobberCCs, bool blind) {
        NanoAssert(IsGpReg(r));
        if (v == 0 && canClobberCCs) {
            XORRR(r, r);
        } else if (blind && shouldBlind(v)) {
            NanoAssert(canClobberCCs);
            XORLRI(r, _blindMask32);
            MOVI(r, v ^ _blindMask32);
        } else {
            MOVI(r, v);
        }
    }

    void Assembler::asm_immq(Register r, uint64_t v, bool canClobberCCs, bool blind) {
        NanoAssert(IsGpReg(r));
        if (isU32(v)) {
            asm_immi(r, int32_t(v), canClobberCCs, blind);
        } else if (isS32(v)) {
            // safe for sign-extension 32->64
            if (blind && shouldBlind(int32_t(v))) {
                NanoAssert(canClobberCCs);
                // Might it be better to load blinded constants from a pool?
                Register t = _allocator.allocTempReg(GpRegs & ~rmask(r));
                XORQRR(r, t);
                MOVQI32(t, _blindMask32);
                MOVQI32(r, int32_t(v) ^ _blindMask32);
            } else {
                MOVQI32(r, int32_t(v));
            }
        } else if (isTargetWithinS32((NIns*)v) && !(blind && shouldBlind(v))) {
            // Value is within +/- 2GB from RIP, thus we can use LEA with RIP-relative disp32.
            // Don't use this pattern for blinded constants, as an attacker might know where
            // the code is loaded.
            int32_t d = int32_t(int64_t(v)-int64_t(_nIns));
            LEARIP(r, d);
        } else {
            if (blind && shouldBlind(v)) {
                NanoAssert(canClobberCCs);
                // Might it be better to load blinded constants from a pool?
                Register t = _allocator.allocTempReg(GpRegs & ~rmask(r));
                XORQRR(r, t);
                MOVQI(t, _blindMask64);
                MOVQI(r, v ^ _blindMask64);
            } else {
                MOVQI(r, v);
            }
        }
    }

    void Assembler::asm_immd(Register r, uint64_t v, bool canClobberCCs, bool blind) {
        NanoAssert(IsFpReg(r));
        if (v == 0 && canClobberCCs) {
            XORPS(r);
        } else {
            // There's no general way to load an immediate into an XMM reg.
            // For non-zero floats the best thing is to put the equivalent
            // 64-bit integer into a scratch GpReg and then move it into the
            // appropriate FpReg.
            // QUERY: When blinding is enabled, asm_immq() can be rather costly.
            // We may be better off loading from a pool here.
            Register rt = _allocator.allocTempReg(GpRegs);
            MOVQXR(r, rt);
            asm_immq(rt, v, canClobberCCs, blind);
        }
    }

    void Assembler::asm_param(LIns *ins) {
        uint32_t a = ins->paramArg();
        uint32_t kind = ins->paramKind();
        if (kind == 0) {
            // Ordinary param.  First four or six args always in registers for x86_64 ABI.
            if (a < (uint32_t)NumArgRegs) {
                // incoming arg in register
                prepareResultReg(ins, rmask(RegAlloc::argRegs[a]));
                // No code to generate.
            } else {
                // todo: support stack based args, arg 0 is at [FP+off] where off
                // is the # of regs to be pushed in genProlog()
                TODO(asm_param_stk);
            }
        }
        else {
            // Saved param.
            prepareResultReg(ins, rmask(RegAlloc::savedRegs[a]));
            // No code to generate.
        }
        freeResourcesOf(ins);
    }

    // Register setup for 2-address style unary ops of the form R = (op) R.
    // Pairs with endOpRegs().
    void Assembler::beginOp1Regs(LIns* ins, RegisterMask allow, Register &rr, Register &ra) {
        LIns* a = ins->oprnd1();

        rr = prepareResultReg(ins, allow);

        // If 'a' isn't in a register, it can be clobbered by 'ins'.
        ra = a->isInReg() ? a->getReg() : rr;
        NanoAssert(rmask(ra) & allow);
    }

    // Register setup for 2-address style binary ops of the form R = R (op) B.
    // Pairs with endOpRegs().
    void Assembler::beginOp2Regs(LIns *ins, RegisterMask allow, Register &rr, Register &ra,
                                 Register &rb) {
        LIns *a = ins->oprnd1();
        LIns *b = ins->oprnd2();
        if (a != b) {
            rb = findRegFor(b, allow);
            allow &= ~rmask(rb);
        }
        rr = prepareResultReg(ins, allow);

        // If 'a' isn't in a register, it can be clobbered by 'ins'.
        ra = a->isInReg() ? a->getReg() : rr;
        NanoAssert(rmask(ra) & allow);

        if (a == b) {
            rb = ra;
        }
    }

    // Register clean-up for 2-address style unary ops of the form R = (op) R.
    // Pairs with beginOp1Regs() and beginOp2Regs().
    void Assembler::endOpRegs(LIns* ins, Register rr, Register ra) {
        (void) rr; // quell warnings when NanoAssert is compiled out.

        LIns* a = ins->oprnd1();

        // We're finished with 'ins'.
        NanoAssert(ins->getReg() == rr);
        freeResourcesOf(ins);

        // If 'a' isn't in a register yet, that means it's clobbered by 'ins'.
        if (!a->isInReg()) {
            NanoAssert(ra == rr);
            findSpecificRegForUnallocated(a, ra);
         }
    }

    static const AVMPLUS_ALIGN16(int64_t) negateMaskD[]  = { 0x8000000000000000LL, 0 };
    static const AVMPLUS_ALIGN16(int32_t) negateMaskF[]  = { 0x80000000, 0, 0, 0 };
    static const AVMPLUS_ALIGN16(int32_t) negateMaskF4[] = { 0x80000000, 0x80000000, 0x80000000, 0x80000000 };

    void Assembler::asm_neg_abs(LIns *ins) {
        Register rr, ra;
        NanoAssert(ins->isop(LIR_negf) || ins->isop(LIR_negf4) || ins->isop(LIR_negd));
        beginOp1Regs(ins, FpRegs, rr, ra);
        uintptr_t mask;

        switch (ins->opcode()) {
        default: NanoAssert(!"bad opcode for asm_neg_abs"); mask = 0; break;
        case LIR_negf:    mask = (uintptr_t) negateMaskF;  break;
        case LIR_negf4:   mask = (uintptr_t) negateMaskF4; break;
        case LIR_negd:    mask = (uintptr_t) negateMaskD;     break;
        }

        if (isS32(mask)) {
            // builtin code is in bottom or top 2GB addr space, use absolute addressing
            XORPSA(rr, (int32_t)mask);
        } else if (isTargetWithinS32((NIns*)mask)) {
            // jit code is within +/-2GB of builtin code, use rip-relative
            XORPSM(rr, (NIns*)mask);
        } else {
            // This is just hideous - can't use RIP-relative load, can't use
            // absolute-address load, and cant move imm64 const to XMM.
            // Solution: move negateMaskD into a temp GP register, then copy to
            // a temp XMM register.
            // Nb: we don't want any F64 values to end up in a GpReg, nor any
            // I64 values to end up in an FpReg.
            //
            //   # 'gt' and 'ga' are temporary GpRegs.
            //   # ins->oprnd1() is in 'rr' (FpRegs)
            //   mov   gt, 0x8000000000000000
            //   mov   rt, gt
            //   xorps rr, rt

            // NOTE: we can use allocTempReg, since we allocate from different classes,
            // AND all the called functions (asm_immq, asm_immi) don't alloc/inspect the regstate
            // But this is arguably dangerous (some called function may change in the future), 
            Register rt = _allocator.allocTempReg(FpRegs & ~(rmask(ra)|rmask(rr)));
            Register gt = _allocator.allocTempReg(GpRegs);
            XORPS(rr, rt);

            if (ins->isop(LIR_negf) || ins->isop(LIR_negf4)) {
                if (ins->isop(LIR_negf4))
                    PSHUFD(rt,rt,PSHUFD_MASK(0, 0, 0, 0));    // copy mask in all 4 components of the float4 vector 
                MOVDXR(rt, gt);
                asm_immi(gt, negateMaskF[0], /*canClobberCCs*/true, /*blind*/false); 
            } else {
                // LIR_negd
                MOVQXR(rt, gt);
                asm_immq(gt, negateMaskD[0], /*canClobberCCs*/true, /*blind*/false);
            }
        }
        if (ra != rr)
            asm_nongp_copy(rr,ra);
        endOpRegs(ins, rr, ra);
    }

    void Assembler::asm_recip_sqrt(LIns*) {
        NanoAssert(!"not implemented");
    }

    void Assembler::asm_spill(Register rr, int d, int8_t nWords) {
        NanoAssert(d);
        if (!IsFpReg(rr)) {
            NanoAssert(nWords == 1 || nWords == 2);
            if (nWords == 2)
                MOVQMR(rr, d, FP);
            else
                MOVLMR(rr, d, FP);
        } else {
            NanoAssert(nWords == 1 || nWords == 2 || nWords == 4);
            switch (nWords) {
            default: NanoAssert(!"bad nWords");
            case 1:  // single-precision float: store 32bits from XMM to memory
                MOVSSMR(rr, d, FP);
                break;
            case 2:  // double: store 64bits from XMM to memory
                MOVSDMR(rr, d, FP);
                break;
            case 4:  // float4: store 128bits from XMM to memory
                MOVUPSMR(rr, d, FP);
                break;
            }
        }
    }

    NIns* Assembler::genPrologue() {
        // activation frame is 4 bytes per entry even on 64bit machines
        uint32_t stackNeeded = max_stk_used + _activation.stackSlotsNeeded() * 4;

        uint32_t stackPushed =
            sizeof(void*) + // returnaddr
            sizeof(void*); // ebp
        uint32_t aligned = alignUp(stackNeeded + stackPushed, NJ_ALIGN_STACK);
        uint32_t amt = aligned - stackPushed;

#ifdef _WIN64
        // Windows uses a single guard page for extending the stack, so
        // new stack pages must be first touched in stack-growth order.
        // We touch each whole page that will be allocated to the frame
        // (following the saved FP) to cause the OS to commit the page if
        // necessary.  Since we don't calculate page boundaries, but just
        // probe at intervals of the pagesize, it is possible that the
        // last page of the frame will be touched unnecessarily.  Note that
        // we must generate the probes in the reverse order of their execution.
        // We require that the page size be a power of 2.
        uint32_t pageSize = uint32_t(VMPI_getVMPageSize());
        NanoAssert((pageSize & (pageSize-1)) == 0);
        uint32_t pageRounded = amt & ~(pageSize-1);
        for (int32_t d = pageRounded; d > 0; d -= pageSize) {
            MOVLMI(RBP, -d, 0);
        }
#endif

        // Reserve stackNeeded bytes, padded
        // to preserve NJ_ALIGN_STACK-byte alignment.
        if (amt) {
            if (isS8(amt))
                SUBQR8(RSP, amt);
            else
                SUBQRI(RSP, amt);
        }

#if defined(NANOJIT_WIN_CFG)
		// Do 16 bytes alignment
		// Function entry address is going to be "_nIns - 4" at this point.
		// The function entry should be 16 bytes aligned so we check with "_nIns - 4".
		// Alternatively, we can add this NOPs in front of function prologue but if we do,
		// a debugger (i.e Visueal Studio) would get lost callstack information so it would make us difficult to debug.
		// ToDo:
		//    There may be something to optimaize, such as using 15 NOPs vs. using JMP
		//    Leave it as follow-up action item.
		while ((((uintptr_t)_nIns - 4) & (uintptr_t)0x0F))
			emit(X64_nop1);
#endif

        verbose_only( asm_output("[patch entry]"); )
        NIns *patchEntry = _nIns;
        MR(FP, RSP);    // Establish our own FP.
        PUSHR(FP);      // Save caller's FP.

#if defined(NANOJIT_WIN_CFG)
		NanoAssert(((uintptr_t)_nIns & (uintptr_t)0x0F) == 0);
#endif

        return patchEntry;
    }

    NIns* Assembler::genEpilogue() {
        // pop rbp
        // ret
        RET();
        POPR(RBP);
        return _nIns;
    }

    RegisterMask RegAlloc::nInitManagedRegisters() {
        // add scratch registers to our free list for the allocator
#ifdef _WIN64
        return 0x001fffcf; // rax-rbx, rsi, rdi, r8-r15, xmm0-xmm5
#else
        return 0xffffffff & ~(1<<REGNUM(RSP) | 1<<REGNUM(RBP));
#endif
    }

    void Assembler::nPatchBranch(NIns *patch, NIns *target) {
        NIns *next = 0;
        if (patch[0] == 0xE9) {
            // jmp disp32
            next = patch+5;
        } else if (patch[0] == 0x0F && (patch[1] & 0xF0) == 0x80) {
            // jcc disp32
            next = patch+6;
        } else if ((patch[0] == 0xFF) && (patch[1] == 0x25)) {
            // jmp 64bit target
            // This uses RIP-relative addressing, the 4 bytes after FF 25 is an offset of 0.
            next = patch+6;
            ((int64_t*)next)[0] = int64_t(target);
            return;
        } else {
            next = 0;
            TODO(unknown_patch);
        }
        // Guards can result in a valid branch being patched again later, so don't assert
        // that the old value is poison.
        if (!isS32(target - next)) {
            setError(BranchTooFar);
            return;         // don't patch
        }
        ((int32_t*)next)[-1] = int32_t(target - next);
    }

    void Assembler::nFragExit(LIns *guard) {
        SideExit *exit = guard->record()->exit;
        Fragment *frag = exit->target;
        GuardRecord *lr = 0;
        bool destKnown = (frag && frag->fragEntry);
        // Generate jump to epilog and initialize lr.
        // If the guard already exists, use a simple jump.
        if (destKnown) {
            JMP(frag->fragEntry);
            lr = 0;
        } else {  // target doesn't exist. Use 0 jump offset and patch later
            if (!_epilogue)
                _epilogue = genEpilogue();
            lr = guard->record();
            JMPl(_epilogue);
            lr->jmp = _nIns;
        }

        // profiling for the exit
        verbose_only(
           if (_logc->lcbits & LC_FragProfile) {
              asm_inc_m32( &guard->record()->profCount );
           }
        )

        MR(RSP, RBP);

        // return value is GuardRecord*
        asm_immq(RAX, uintptr_t(lr), /*canClobberCCs*/true, /*blind*/false);
    }

    const RegisterMask PREFER_SPECIAL = ~ ((RegisterMask)0);

    // Init per-opcode register hint table.  
    static bool nHintsInit(RegisterMask Hints[]) {
        VMPI_memset(Hints,0,sizeof(RegisterMask)*LIR_sentinel );
        Hints[LIR_calli]  = rmask(RAX);
        Hints[LIR_calld]  = rmask(XMM0);
        Hints[LIR_callf]  = rmask(XMM0);
        Hints[LIR_callf4] = rmask(XMM0);
        Hints[LIR_paramp] = PREFER_SPECIAL;
        return true;
    }

    void Assembler::nBeginAssembly() {
        max_stk_used = 0;
    }

    // This should only be called from within emit() et al.
    void Assembler::underrunProtect(ptrdiff_t bytes) {
        NanoAssertMsg(bytes<=LARGEST_UNDERRUN_PROT, "constant LARGEST_UNDERRUN_PROT is too small");
        NIns *pc = _nIns;
        NIns *top = codeStart;  // this may be in a normal code chunk or an exit code chunk

    #if PEDANTIC
        // pedanticTop is based on the last call to underrunProtect; any time we call
        // underrunProtect and would use more than what's already protected, then insert
        // a page break jump.  Sometimes, this will be to a new page, usually it's just
        // the next instruction

        NanoAssert(pedanticTop >= top);
        if (pc - bytes < pedanticTop) {
            // no page break required, but insert a far branch anyway just to be difficult
            const int br_size = 8; // opcode + 32bit addr
            if (pc - bytes - br_size < top) {
                // really do need a page break
                verbose_only(if (_logc->lcbits & LC_Native) outputf("newpage %p:", pc);)
                // This may be in a normal code chunk or an exit code chunk.
                codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));
            }
            // now emit the jump, but make sure we won't need another page break.
            // we're pedantic, but not *that* pedantic.
            pedanticTop = _nIns - br_size;
            JMP(pc);
            pedanticTop = _nIns - bytes;
        }
    #else
        if (pc - bytes < top) {
            verbose_only(if (_logc->lcbits & LC_Native) outputf("newpage %p:", pc);)
            // This may be in a normal code chunk or an exit code chunk.
            codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));
            // This jump will call underrunProtect again, but since we're on a new
            // page, nothing will happen.
            JMP(pc);
        }
    #endif
    }

    RegisterMask RegAlloc::nHint(LIns* ins)
    {
        static RegisterMask  Hints[LIR_sentinel+1]; // effectively const, save for the initialization
        static bool initialized = nHintsInit(Hints); (void)initialized; 

        RegisterMask prefer = Hints[ins->opcode()];

        if (prefer != PREFER_SPECIAL)
          return prefer;

        NanoAssert(ins->isop(LIR_paramp));
        uint8_t arg = ins->paramArg();
        if (ins->paramKind() == 0) {
            if (arg < maxArgRegs)
                prefer = rmask(argRegs[arg]);
        } else {
            if (arg < NumSavedRegs)
                prefer = rmask(savedRegs[arg]);
        }
        return prefer;
    }

    void Assembler::nativePageSetup() {
        NanoAssert(!_inExit);
        if (!_nIns) {
            codeAlloc(codeStart, codeEnd, _nIns verbose_only(, codeBytes));
            IF_PEDANTIC( pedanticTop = _nIns; )
        }
    }

    void Assembler::nativePageReset()
    {}

    // Increment the 32-bit profiling counter at pCtr, without
    // changing any registers.
    verbose_only(
    void Assembler::asm_inc_m32(uint32_t* pCtr)
    {
        // Not as simple as on x86.  We need to temporarily free up a
        // register into which to generate the address, so just push
        // it on the stack.  This assumes that the scratch area at
        // -8(%rsp) .. -1(%esp) isn't being used for anything else
        // at this point.
        emitr(X64_popr, RAX);                                                   // popq    %rax
        emit(X64_inclmRAX);                                                     // incl    (%rax)
        asm_immq(RAX, (uint64_t)pCtr, /*canClobberCCs*/true, /*blind*/false);   // movabsq $pCtr, %rax
        emitr(X64_pushr, RAX);                                                  // pushq   %rax
    }
    )

    void Assembler::asm_jtbl(NIns** table, Register indexreg)
    {
        if (isS32((intptr_t)table)) {
            // table is in low 2GB or high 2GB, can use absolute addressing
            // jmpq [indexreg*8 + table]
            JMPX(indexreg, table);
        } else {
            // don't use R13 for base because we want to use mod=00, i.e. [index*8+base + 0]
            Register tablereg =  _allocator.allocTempReg(GpRegs & ~(rmask(indexreg)|rmask(R13)));
            // jmp [indexreg*8 + tablereg]
            JMPXB(indexreg, tablereg);
            // tablereg <- #table
            asm_immq(tablereg, (uint64_t)table, /*canClobberCCs*/true, /*blind*/false);
        }
    }

    void Assembler::swapCodeChunks() {
        if (!_nExitIns) {
            codeAlloc(exitStart, exitEnd, _nExitIns verbose_only(, exitBytes));
        }
        SWAP(NIns*, _nIns, _nExitIns);
        SWAP(NIns*, codeStart, exitStart);
        SWAP(NIns*, codeEnd, exitEnd);
        verbose_only( SWAP(size_t, codeBytes, exitBytes); )
    }

    void Assembler::asm_insert_random_nop() {
        NanoAssert(0); // not supported
    }

    void Assembler::asm_label() {
        // do nothing right now
    }

} // namespace nanojit

#endif // FEATURE_NANOJIT && NANOJIT_X64
