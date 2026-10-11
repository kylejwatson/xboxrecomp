/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The MMIO hook is an x86-64 instruction decoder run from a vectored
 * exception handler: Win32 VEH on Windows, the sigaction-backed emulation in
 * src/platform/win32_compat.c elsewhere. Both hand it the same CONTEXT. */
#include "platform/xbox_winnt.h"
#include "platform/mmio_decode.h"
/* getenv: without <stdlib.h> its pointer is truncated to int. */
#include <stdlib.h>

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

/* ============================================================
 * Instruction decoder for APU MMIO access
 * ============================================================ */

static uint64_t apu_rd(void *dev, uint32_t off, int size)
{
    g_apu_mmio_read_count++;
    return mcpx_apu_mmio_read((MCPXAPUState *)dev, off, size);
}

static void apu_wr(void *dev, uint32_t off, uint64_t val, int size)
{
    g_apu_mmio_write_count++;
    mcpx_apu_mmio_write((MCPXAPUState *)dev, off, val, size);
}

/* The faulting instruction against the APU's registers, through the shared
 * decoder (platform/mmio_decode.h), which knows the read-modify-write forms a
 * compiler may fuse a volatile |= into. */
static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;

    (void)is_write;
    if (!g_apu_state)
        return false;
    if (mmio_emulate(ctx, mmio_offset, g_apu_state, apu_rd, apu_wr))
        return true;
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * AC'97 page: writes trap, reads do not
 * ============================================================ */

/* The AC'97 bus-master control registers, as offsets from the MCPX base.
 *
 * These are NABM DMA-engine register blocks at 0xFEC00100 and 0xFEC00160:
 * the guest writes the buffer-descriptor base at engine+0x00 and reads status
 * at +0x06 and the position counter at +0x08, so +0x0B is CR, the control
 * register, and bit 1 is RR -- Reset Registers.
 *
 * RR is architecturally self-clearing: software sets it, the hardware clears
 * it when the reset finishes. So masking it out on the way in is the correct
 * model of the part, not a workaround. DirectSound relies on that exactly,
 * reading the byte back four instructions later and spinning on the register
 * copy, so it never sees a later change:
 *
 *     mov cl, byte [dsp];  and cl, 2
 *   loop:
 *     test cl, cl;  jne loop
 *
 * On hardware the write latches a command and the flag drops with it, so the
 * read already sees zero. A thread clearing the byte from outside cannot win
 * that race, and measurably does not. Catching the write is what makes it
 * deterministic: the page is mapped PAGE_READONLY, so a write faults here and
 * a read stays plain memory at full speed.
 *
 * Dropping the bit is the whole model. There is no DSP to run the command,
 * and the title reads this byte only to find out whether it may proceed.
 */
#define MCPX_AC97_BM0_CR     0x0040011Bu   /* 0xFEC0011B, engine 0 */
#define MCPX_AC97_BM1_CR     0x0040017Bu   /* 0xFEC0017B, engine 1 */
#define MCPX_AC97_CR_RR      0x02u         /* Reset Registers, self-clearing */

static uint64_t ac97_apply_mask(uint32_t mcpx_offset, uint64_t value)
{
    if (mcpx_offset == MCPX_AC97_BM0_CR || mcpx_offset == MCPX_AC97_BM1_CR)
        return value & ~(uint64_t)MCPX_AC97_CR_RR;
    return value;
}

/* Put the value in the page, which is read-only to everyone including us.
 *
 * Three things here are deliberate, and the first two were wrong before.
 *
 * The restore is to PAGE_READONLY by name, not to whatever VirtualProtect
 * handed back. Two threads in here at once and the second captures
 * PAGE_READWRITE as the "old" protection, so its restore leaves the page
 * writable and the trap is silently dead for the rest of the run.
 *
 * The lock serialises the window. Guest threads are real OS threads
 * (PsCreateSystemThreadEx creates them), so two can reach this together, and
 * during any writable window a second thread's write to the control register
 * lands unmasked -- which is exactly the hang this exists to prevent.
 *
 * And the protection is applied to the whole containing page, because that is
 * the granularity the API works at regardless of the size asked for. A store
 * that would straddle the end of the page is refused rather than dragging the
 * neighbouring page's protection with it.
 */
static CRITICAL_SECTION s_ac97_cs;
static LONG s_ac97_cs_ready;

static void ac97_lock_init(void)
{
    if (InterlockedCompareExchange(&s_ac97_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&s_ac97_cs);
        InterlockedExchange(&s_ac97_cs_ready, 2);
    }
    while (InterlockedCompareExchange(&s_ac97_cs_ready, 2, 2) != 2)
        Sleep(0);
}

static int ac97_store(void *dst, uint64_t value, int size)
{
    void *page = (void *)((uintptr_t)dst & ~(uintptr_t)0xFFF);
    size_t off  = (uintptr_t)dst & 0xFFF;
    DWORD old;
    int ok = 1;

    if (off + (size_t)size > 0x1000)
        return 0;                          /* straddles the page: not ours */

    ac97_lock_init();
    EnterCriticalSection(&s_ac97_cs);

    if (VirtualProtect(page, 0x1000, PAGE_READWRITE, &old)) {
        switch (size) {
        case 1: *(volatile uint8_t  *)dst = (uint8_t)value;  break;
        case 2: *(volatile uint16_t *)dst = (uint16_t)value; break;
        case 8: *(volatile uint64_t *)dst = value;           break;
        default: *(volatile uint32_t *)dst = (uint32_t)value; break;
        }
        VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
    } else {
        ok = 0;
    }

    LeaveCriticalSection(&s_ac97_cs);
    return ok;
}

/* ============================================================
 * NV2A interrupt status: write-1-to-clear
 * ============================================================ */

/* A write-trapped page reads as memory: the decoder's reads come straight
 * from the page, and its one write goes to the handler's semantics. */
typedef struct {
    uintptr_t host_addr;        /* where the faulting access lands */
    uint32_t  offset;           /* the register it lands on */
    uintptr_t aperture;         /* NV2A: the register window's host base */
    int       ok;               /* the semantic store succeeded */
} TrappedWrite;

static uint64_t trapped_rd(void *dev, uint32_t off, int size)
{
    const TrappedWrite *t = (const TrappedWrite *)dev;
    (void)off;
    switch (size) {
    case 1:  return *(const volatile uint8_t  *)t->host_addr;
    case 2:  return *(const volatile uint16_t *)t->host_addr;
    case 8:  return *(const volatile uint64_t *)t->host_addr;
    default: return *(const volatile uint32_t *)t->host_addr;
    }
}


/* The registers a guest ISR acknowledges through, and the summary bit.
 *
 * PMC_INTR_0 bit 24 is not storage: it reports that the CRTC has something
 * pending, and it goes away when PCRTC_INTR_0 does. A driver acknowledges by
 * writing the bits it handled to PCRTC_INTR_0 -- write-1-to-clear -- and then
 * waits for the summary to drop.
 *
 * Burnout 2's D3D8 does exactly that, and against plain memory it cannot ever
 * finish:
 *
 *     0x0021D0D0:  mov  [edi + 0x600100], ecx      ; ecx = 1, the vblank bit
 *     0x0021D0D6:  test [edi + 0x100], 0x1000000
 *     0x0021D0DC:  jne  0x21d0d0                   ; spin
 *
 * The store put 1 into PCRTC_INTR_0 rather than clearing bit 0, PMC_INTR_0
 * kept its summary bit, and the loop never exited. That loop runs on the DPC,
 * which this runtime dispatches from the timer thread -- the same thread that
 * raises the next vblank. So one frame was delivered, the thread died inside
 * the acknowledgement, and nothing ever ticked again. The title's registered
 * vertical-blank callback never ran, and its asset loader waits on a counter
 * that callback feeds.
 *
 * Implementing the real semantics is what fixes it, and it is less code than
 * the workarounds: clear the written bits, then recompute the summary.
 */
#define NV2A_PMC_INTR_0_OFF      0x000100u
#define NV2A_PMC_INTR_PCRTC_BIT  0x01000000u
#define NV2A_PCRTC_INTR_0_OFF    0x600100u

/* The semantics, given the value and width the instruction stores. */
static void nv2a_intr_wr(void *dev, uint32_t off, uint64_t written, int size)
{
    TrappedWrite *t = (TrappedWrite *)dev;
    volatile uint32_t *pmc =
        (volatile uint32_t *)(t->aperture + NV2A_PMC_INTR_0_OFF);
    volatile uint32_t *pcrtc =
        (volatile uint32_t *)(t->aperture + NV2A_PCRTC_INTR_0_OFF);

    (void)off;
    if (t->offset == NV2A_PCRTC_INTR_0_OFF) {
        uint32_t remaining = *pcrtc & ~(uint32_t)written;
        if (!ac97_store((void *)t->host_addr, remaining, 4))
            return;
        /* The summary follows the unit, and nothing else owns that bit. */
        if (remaining)
            *pmc |= NV2A_PMC_INTR_PCRTC_BIT;
        else
            *pmc &= ~NV2A_PMC_INTR_PCRTC_BIT;
        t->ok = 1;
        return;
    }
    if (t->offset == NV2A_PMC_INTR_0_OFF) {
        /* PMC_INTR_0 is read-only status on hardware. A driver that writes it
         * is acknowledging, so drop the written bits rather than storing
         * them. */
        uint32_t remaining = *pmc & ~(uint32_t)written;
        t->ok = ac97_store((void *)t->host_addr, remaining, 4);
        return;
    }
    /* Any other register on the page: a plain store, so the page behaves as
     * memory for everything this does not model. */
    t->ok = ac97_store((void *)t->host_addr, written, size);
}

int nv2a_intr_handle_write(PCONTEXT ctx, uintptr_t host_addr,
                           uint32_t nv2a_offset, uintptr_t aperture)
{
    TrappedWrite t;
    CONTEXT saved = *ctx;

    t.host_addr = host_addr;
    t.offset = nv2a_offset;
    t.aperture = aperture;
    t.ok = 0;
    if (!mmio_emulate(ctx, nv2a_offset, &t, trapped_rd, nv2a_intr_wr))
        return 0;
    if (!t.ok) {
        *ctx = saved;               /* the store did not happen: let it fault */
        return 0;
    }
    return 1;
}

static void ac97_wr(void *dev, uint32_t off, uint64_t value, int size)
{
    TrappedWrite *t = (TrappedWrite *)dev;
    t->ok = ac97_store((void *)t->host_addr, ac97_apply_mask(off, value), size);
}

/* Any store form, read-modify-writes included (the guest clears the control
 * register with `and byte [cr], 0`): the page reads as memory, and the store
 * goes through the reset-bit mask. */
int mcpx_ac97_handle_write(PCONTEXT ctx, uintptr_t host_addr, uint32_t mcpx_offset)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    TrappedWrite t;
    CONTEXT saved = *ctx;

    t.host_addr = host_addr;
    t.offset = mcpx_offset;
    t.aperture = 0;
    t.ok = 0;
    if (mmio_emulate(ctx, mcpx_offset, &t, trapped_rd, ac97_wr) && t.ok)
        return 1;
    *ctx = saved;
    {
        static unsigned said;
        if (said++ < 20) {
            fprintf(stderr, "[AC97] write decode fail at RIP=%p offset=0x%X: "
                    "%02X %02X %02X %02X %02X %02X\n",
                    (void *)ctx->Rip, mcpx_offset,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
            fflush(stderr);
        }
    }
    return 0;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

int apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                         uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    int ok = apu_decode_and_handle(ctx, mmio_offset, is_write) ? 1 : 0;

    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    if (getenv("RECOMP_APU_TRACE")) {
        static unsigned n;
        if (n++ < 400) {
            /* The value as well as the offset: finding which register carries
             * the command-block address means recognising the address when it
             * goes past, and an offset alone never shows it. */
            uint64_t v = g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

