/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the read-modify-write and flag-setting forms a poll loop is built from.
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>

#include "xbox_winnt.h"
#if !defined(_WIN32)
#include "win32_compat.h"
#endif

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline uint64_t *mmio_ctx_reg(PCONTEXT c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;                    /* SIB    */
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* Flags after a compare-shaped operation, so a poll loop branches correctly.
 * ZF, SF and CF only: those are what jz/jnz, js and jb/jae read, and inventing
 * an overflow flag nothing here sets is worse than leaving it alone. */
static inline void mmio_set_flags(PCONTEXT ctx, uint64_t result, int size,
                                  int carry)
{
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (size < 8)
        result &= (1ULL << (size * 8)) - 1;
    if (result == 0)
        ctx->EFlags |= 0x0040u;                                   /* ZF */
    if (result & (1ULL << (size * 8 - 1)))
        ctx->EFlags |= 0x0080u;                                   /* SF */
    if (carry)
        ctx->EFlags |= 0x0001u;                                   /* CF */
}

/* An arithmetic result's flags in full: ZF, SF, CF and OF. */
static inline void mmio_arith_flags(PCONTEXT ctx, uint64_t a, uint64_t b, uint64_t r,
                                    int size, int sub, int keep_cf)
{
    uint64_t mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
    uint64_t sign = 1ULL << (size * 8 - 1);
    uint32_t cf = ctx->EFlags & 0x0001u;
    int carry, of;

    a &= mask; b &= mask; r &= mask;
    carry = sub ? (a < b) : (r < a);
    of = sub ? (((a ^ b) & (a ^ r) & sign) != 0) : (((a ^ r) & (b ^ r) & sign) != 0);
    mmio_set_flags(ctx, r, size, carry);
    if (keep_cf)
        ctx->EFlags = (ctx->EFlags & ~0x0001u) | cf;
    if (of)
        ctx->EFlags |= 0x0800u;                                   /* OF */
}

/* A general register as an operand of `size` bytes. Without a REX prefix,
 * byte registers 4..7 are AH, CH, DH and BH. */
static inline uint64_t mmio_reg_get(PCONTEXT ctx, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && (reg & 7) >= 4 && reg < 8)
        return (*mmio_ctx_reg(ctx, reg - 4) >> 8) & 0xFF;
    return *mmio_ctx_reg(ctx, reg);
}

/* Writing a register as the CPU does: a 32-bit write clears the top half,
 * narrower ones keep the rest. */
static inline void mmio_reg_set(PCONTEXT ctx, int reg, int size, int has_rex, uint64_t v)
{
    uint64_t *r;

    if (size == 1 && !has_rex && (reg & 7) >= 4 && reg < 8) {
        r = mmio_ctx_reg(ctx, reg - 4);
        *r = (*r & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    r = mmio_ctx_reg(ctx, reg);
    if (size == 1)      *r = (*r & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *r = (*r & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *r = v & 0xFFFFFFFFULL;
    else                *r = v;
}

static inline uint64_t mmio_sext(uint64_t v, int from_bytes)
{
    int shift = 64 - from_bytes * 8;
    return (uint64_t)(((int64_t)(v << shift)) >> shift);
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it.
 *
 * The forms: MOV both ways and with an immediate, MOVZX/MOVSX, TEST and CMP,
 * and every read-modify-write a compiler can fuse a `|=` or `+=` on volatile
 * memory into -- ADD/OR/AND/SUB/XOR with a register or an immediate, INC,
 * DEC, NOT, NEG, XCHG, XADD, CMPXCHG -- with a LOCK prefix or without. MSVC
 * keeps a volatile read and write apart, so the Windows build only ever met
 * the MOV forms; GCC and Clang fuse them, and a decoder that knew only MOV
 * failed on the first `or [reg], 1` against a trapped register. The flags an
 * ALU form sets are set, so a branch on them goes the right way. */
static inline int mmio_emulate(PCONTEXT ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg, ext;
    uint64_t m, r, v, mask;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3 || b == 0xF0) { prefix++; }   /* rep, lock */
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) { prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = has66 ? 2 : (rex_w ? 8 : 4);

    /* The classic ALU block: 00-03 ADD, 08-0B OR, 20-23 AND, 28-2B SUB,
     * 30-33 XOR, 38-3B CMP. Even opcodes are bytes; 0..1 store to memory,
     * 2..3 load into the register. */
    if (op[0] < 0x40 && (op[0] & 7) < 4 &&
        (op[0] >> 3) != 2 && (op[0] >> 3) != 3) {     /* not ADC (10-13) / SBB (18-1B) */
        int alu = op[0] >> 3, to_reg = (op[0] & 2) != 0;
        if (!(op[0] & 1)) size = 1;
        mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m = rd(dev, off, size) & mask;
        r = mmio_reg_get(ctx, reg, size, has_rex) & mask;
        {
            uint64_t a = to_reg ? r : m, b = to_reg ? m : r;
            switch (alu) {
            case 0: v = a + b; mmio_arith_flags(ctx, a, b, v, size, 0, 0); break;
            case 1: v = a | b; mmio_set_flags(ctx, v, size, 0); break;
            case 4: v = a & b; mmio_set_flags(ctx, v, size, 0); break;
            case 5: v = a - b; mmio_arith_flags(ctx, a, b, v, size, 1, 0); break;
            case 6: v = a ^ b; mmio_set_flags(ctx, v, size, 0); break;
            case 7: v = a - b; mmio_arith_flags(ctx, a, b, v, size, 1, 0); break;
            default: return 0;
            }
        }
        if (alu != 7) {
            if (to_reg) mmio_reg_set(ctx, reg, size, has_rex, v);
            else        wr(dev, off, v & mask, size);
        }
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, mmio_reg_get(ctx, reg, size, has_rex), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0xC7: {                                     /* MOV r/m, imm16/32    */
        int imm = has66 ? 2 : 4;
        mlen = mmio_modrm_len(op + 1, rex_b);
        v = has66 ? *(const uint16_t *)(op + 1 + mlen) : *(const uint32_t *)(op + 1 + mlen);
        if (rex_w)
            v = mmio_sext(v, 4);
        wr(dev, off, v, size);
        ctx->Rip += prefix + 1 + mlen + imm;
        return 1;
    }

    case 0xC6:                                       /* MOV r/m8, imm8       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, op[1 + mlen], 1);
        ctx->Rip += prefix + 1 + mlen + 1;
        return 1;

    case 0x8A: case 0x8B:                            /* MOV r, r/m   (read)  */
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_reg_set(ctx, reg, size, has_rex, rd(dev, off, size));
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x84: case 0x85:                            /* TEST r/m, r          */
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = mmio_reg_get(ctx, reg, size, has_rex);
        mmio_set_flags(ctx, m & r, size, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x86: case 0x87:                            /* XCHG r/m, r          */
        if (op[0] == 0x86) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        wr(dev, off, mmio_reg_get(ctx, reg, size, has_rex), size);
        mmio_reg_set(ctx, reg, size, has_rex, m);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x80: case 0x81: case 0x83: {               /* group 1, immediate   */
        int imm;
        if (op[0] == 0x80) size = 1;
        mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
        mlen = mmio_modrm_len(op + 1, rex_b);
        ext  = (op[1] >> 3) & 7;
        if (op[0] == 0x81) {
            imm = has66 ? 2 : 4;
            r = has66 ? *(const uint16_t *)(op + 1 + mlen) : *(const uint32_t *)(op + 1 + mlen);
            if (!has66)
                r = mmio_sext(r, 4);
        } else {
            imm = 1;
            r = op[1 + mlen];
            if (op[0] == 0x83)
                r = mmio_sext(r, 1);
        }
        r &= mask;
        m = rd(dev, off, size) & mask;
        switch (ext) {
        case 0: v = m + r; mmio_arith_flags(ctx, m, r, v, size, 0, 0); break;   /* ADD */
        case 1: v = m | r; mmio_set_flags(ctx, v, size, 0); break;               /* OR  */
        case 4: v = m & r; mmio_set_flags(ctx, v, size, 0); break;               /* AND */
        case 5: v = m - r; mmio_arith_flags(ctx, m, r, v, size, 1, 0); break;   /* SUB */
        case 6: v = m ^ r; mmio_set_flags(ctx, v, size, 0); break;               /* XOR */
        case 7: v = m - r; mmio_arith_flags(ctx, m, r, v, size, 1, 0); break;   /* CMP */
        default: return 0;                          /* ADC/SBB: not emitted here */
        }
        if (ext != 7)
            wr(dev, off, v & mask, size);
        ctx->Rip += prefix + 1 + mlen + imm;
        return 1;
    }

    case 0xF6: case 0xF7: {                          /* TEST imm, NOT, NEG   */
        if (op[0] == 0xF6) size = 1;
        mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
        mlen = mmio_modrm_len(op + 1, rex_b);
        ext  = (op[1] >> 3) & 7;
        m = rd(dev, off, size) & mask;
        if (ext == 0) {
            int imm = size == 1 ? 1 : (has66 ? 2 : 4);
            r = size == 1 ? op[1 + mlen] : has66 ? *(const uint16_t *)(op + 1 + mlen)
                                                 : *(const uint32_t *)(op + 1 + mlen);
            if (size == 8)
                r = mmio_sext(r, 4);
            mmio_set_flags(ctx, m & r, size, 0);
            ctx->Rip += prefix + 1 + mlen + imm;
            return 1;
        }
        if (ext == 2) {                              /* NOT: no flags        */
            wr(dev, off, ~m & mask, size);
        } else if (ext == 3) {                       /* NEG                  */
            v = (0 - m) & mask;
            mmio_arith_flags(ctx, 0, m, v, size, 1, 0);
            wr(dev, off, v, size);
        } else {
            return 0;
        }
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0xFE: case 0xFF:                            /* INC, DEC             */
        if (op[0] == 0xFE) size = 1;
        mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
        mlen = mmio_modrm_len(op + 1, rex_b);
        ext  = (op[1] >> 3) & 7;
        if (ext > 1)
            return 0;                                /* call/jmp/push        */
        m = rd(dev, off, size) & mask;
        v = (ext == 0 ? m + 1 : m - 1) & mask;
        mmio_arith_flags(ctx, m, 1, v, size, ext == 1, 1);   /* CF unchanged */
        wr(dev, off, v, size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7 || op[1] == 0xBE || op[1] == 0xBF) {
            int s = (op[1] & 1) ? 2 : 1;             /* MOVZX / MOVSX        */
            int dst = rex_w ? 8 : (has66 ? 2 : 4);
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            v = rd(dev, off, s) & ((1ULL << (s * 8)) - 1);
            if (op[1] >= 0xBE)
                v = mmio_sext(v, s);
            mmio_reg_set(ctx, reg, dst, has_rex, v);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        if (op[1] == 0xC0 || op[1] == 0xC1) {        /* XADD r/m, r          */
            if (op[1] == 0xC0) size = 1;
            mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
            mlen = mmio_modrm_len(op + 2, rex_b);
            reg  = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            m = rd(dev, off, size) & mask;
            r = mmio_reg_get(ctx, reg, size, has_rex) & mask;
            v = (m + r) & mask;
            mmio_arith_flags(ctx, m, r, v, size, 0, 0);
            wr(dev, off, v, size);
            mmio_reg_set(ctx, reg, size, has_rex, m);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        if (op[1] == 0xB0 || op[1] == 0xB1) {        /* CMPXCHG r/m, r       */
            uint64_t acc;
            if (op[1] == 0xB0) size = 1;
            mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
            mlen = mmio_modrm_len(op + 2, rex_b);
            reg  = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            m   = rd(dev, off, size) & mask;
            acc = ctx->Rax & mask;
            mmio_arith_flags(ctx, acc, m, (acc - m) & mask, size, 1, 0);
            if (acc == m)
                wr(dev, off, mmio_reg_get(ctx, reg, size, has_rex) & mask, size);
            else
                mmio_reg_set(ctx, 0, size, 1, m);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

#endif /* MMIO_DECODE_H */
