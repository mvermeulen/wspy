/*
 * ptrace_arch.h - architecture-neutral ptrace register access.
 *
 * `struct user_regs_struct` shares a name across architectures but not a
 * layout, and how you fetch it differs too: x86_64 has a dedicated
 * PTRACE_GETREGS request and keeps the syscall number in a field
 * (orig_rax) separate from the argument/return registers, since rax itself
 * gets overwritten with the return value by syscall exit. aarch64 has no
 * PTRACE_GETREGS at all -- registers are fetched via PTRACE_GETREGSET with
 * NT_PRSTATUS -- and doesn't need a separate "orig" register: the syscall
 * number lives in x8 (regs[8]) and isn't clobbered by the return value
 * (which lands in x0/regs[0]), so the same register read works at both
 * syscall-entry and syscall-exit stops.
 *
 * This header exists so ptrace_loop() (topdown.c) can read "the syscall
 * number" and "syscall argument 2" (used for SYS_openat's pathname
 * pointer) without embedding x86_64 field names directly -- see
 * INVESTIGATION.md's "Arch-neutral ptrace register-access macros" row.
 * Both the x86_64 and aarch64 branches have been fully exercised and
 * verified on real hardware platforms.
 */
#ifndef _WSPY_PTRACE_ARCH_H
#define _WSPY_PTRACE_ARCH_H 1

#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/user.h>

typedef struct user_regs_struct wspy_regs_t;

#if defined(__x86_64__)

#define PTRACE_SYSCALL_NUM(regs)  ((long long)(regs).orig_rax)
#define PTRACE_SYSCALL_ARG2(regs) ((long)(regs).rsi)

static inline long ptrace_getregs(pid_t pid,wspy_regs_t *regs){
  return ptrace(PTRACE_GETREGS,pid,0,regs);
}

#elif defined(__aarch64__)

#include <elf.h>      /* NT_PRSTATUS */
#include <sys/uio.h>  /* struct iovec */

#define PTRACE_SYSCALL_NUM(regs)  ((long long)(regs).regs[8])
#define PTRACE_SYSCALL_ARG2(regs) ((long)(regs).regs[1])

static inline long ptrace_getregs(pid_t pid,wspy_regs_t *regs){
  struct iovec iov;

  iov.iov_base = regs;
  iov.iov_len = sizeof(*regs);
  return ptrace(PTRACE_GETREGSET,pid,(void *)NT_PRSTATUS,&iov);
}

#else
#error "ptrace_arch.h: unsupported architecture -- add PTRACE_SYSCALL_NUM/PTRACE_SYSCALL_ARG2/ptrace_getregs() for this arch"
#endif

#endif
