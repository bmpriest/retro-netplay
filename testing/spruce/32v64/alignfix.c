/* LD_PRELOAD for 32-bit ARM processes on a kernel that does not fix up
 * misaligned multi-word accesses (arm64 compat, qemu-user). Emulates ARM-mode
 * LDM/STM and LDRD/STRD from SIGBUS the way arch/arm/mm/alignment.c does on a
 * 32-bit kernel (e.g. the Miyoo A30), so a core behaves as it would there.
 * Anything else is reported and the process exits 77. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static unsigned long fixups;

static uint32_t rd32(uintptr_t a) { uint32_t v; memcpy(&v, (void*)a, 4); return v; }
static void wr32(uintptr_t a, uint32_t v) { memcpy((void*)a, &v, 4); }

static int cond_pass(uint32_t insn, uint32_t cpsr) {
	unsigned c = insn >> 28, N = cpsr >> 31 & 1, Z = cpsr >> 30 & 1, C = cpsr >> 29 & 1, V = cpsr >> 28 & 1;
	switch (c) {
	case 0: return Z; case 1: return !Z; case 2: return C; case 3: return !C;
	case 4: return N; case 5: return !N; case 6: return V; case 7: return !V;
	case 8: return C && !Z; case 9: return !C || Z; case 10: return N == V; case 11: return N != V;
	case 12: return !Z && N == V; case 13: return Z || N != V; default: return 1;
	}
}

static void h(int s, siginfo_t* si, void* uc_) {
	(void)s;
	ucontext_t* uc = uc_;
	unsigned long* r = &uc->uc_mcontext.arm_r0;   /* r0..r15 are contiguous */
	uint32_t cpsr = uc->uc_mcontext.arm_cpsr;
	uintptr_t pc = uc->uc_mcontext.arm_pc;
	if (cpsr & (1u << 5)) goto bad;               /* Thumb: not handled */
	uint32_t insn = *(uint32_t*)pc;
	if (!cond_pass(insn, cpsr)) { uc->uc_mcontext.arm_pc += 4; return; }

	if ((insn & 0x0e000000) == 0x08000000) {      /* LDM/STM */
		unsigned P = insn >> 24 & 1, U = insn >> 23 & 1, W = insn >> 21 & 1, L = insn >> 20 & 1;
		unsigned rn = insn >> 16 & 15, list = insn & 0xffff;
		if (insn >> 22 & 1) goto bad;             /* S bit */
		if (list & (1u << 15)) goto bad;          /* pc in list */
		unsigned n = __builtin_popcount(list);
		uintptr_t base = r[rn], start;
		if (U) start = P ? base + 4 : base;
		else   start = P ? base - 4 * n : base - 4 * n + 4;
		uintptr_t a = start;
		for (unsigned i = 0; i < 15; i++) {
			if (!(list & (1u << i))) continue;
			if (L) r[i] = rd32(a); else wr32(a, (uint32_t)r[i]);
			a += 4;
		}
		if (W && !(L && (list & (1u << rn)))) r[rn] = U ? base + 4 * n : base - 4 * n;
	} else if ((insn & 0x0e0000d0) == 0x000000d0 && (insn & 0x60) && !(insn >> 20 & 1)) {
		/* LDRD (op2=10) / STRD (op2=11), immediate or register offset */
		unsigned P = insn >> 24 & 1, U = insn >> 23 & 1, I = insn >> 22 & 1, W = insn >> 21 & 1;
		unsigned rn = insn >> 16 & 15, rt = insn >> 12 & 15, store = (insn >> 5 & 3) == 3;
		if ((insn >> 5 & 3) != 2 && !store) goto bad;
		if (rt & 1 || rt == 14) goto bad;
		uint32_t off = I ? ((insn >> 4 & 0xf0) | (insn & 0xf)) : (uint32_t)r[insn & 15];
		uintptr_t base = rn == 15 ? pc + 8 : r[rn];
		uintptr_t addr = U ? base + off : base - off;
		uintptr_t a = P ? addr : base;
		if (store) { wr32(a, (uint32_t)r[rt]); wr32(a + 4, (uint32_t)r[rt + 1]); }
		else { r[rt] = rd32(a); r[rt + 1] = rd32(a + 4); }
		if (!P || W) r[rn] = addr;
	} else goto bad;
	fixups++;
	uc->uc_mcontext.arm_pc += 4;
	return;
bad:
	fprintf(stderr, "SIGBUS unhandled addr=%p pc=%lx thumb=%u insn=%08x\n", si->si_addr,
		(unsigned long)pc, (unsigned)(cpsr >> 5 & 1), *(uint32_t*)(pc & ~1ul));
	_exit(77);
}

static void report(void) {
	if (getenv("ALIGNFIX_REPORT")) fprintf(stderr, "alignfix: %lu fixups\n", fixups);
}

__attribute__((constructor)) static void init(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = h;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, 0);
	atexit(report);
}
