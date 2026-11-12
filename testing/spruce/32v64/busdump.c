#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <unistd.h>
static void h(int s, siginfo_t* si, void* uc_) {
	ucontext_t* uc = uc_;
	unsigned long pc = uc->uc_mcontext.arm_pc, cpsr = uc->uc_mcontext.arm_cpsr;
	Dl_info di; dladdr((void*)pc, &di);
	fprintf(stderr, "SIGBUS addr=%p pc=%lx thumb=%lu insn=%08x lib=%s+%lx sym=%s\n", si->si_addr, pc, (cpsr>>5)&1,
		(cpsr>>5)&1 ? (unsigned)(*(unsigned short*)pc<<16 | *(unsigned short*)(pc+2)) : *(unsigned*)pc,
		di.dli_fname, pc-(unsigned long)di.dli_fbase, di.dli_sname?di.dli_sname:"?");
	_exit(77);
}
__attribute__((constructor)) static void init(void) { struct sigaction sa = {0}; sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO; sigaction(SIGBUS, &sa, 0); }
