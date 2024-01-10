CC=gcc
CFLAGS=-g
PROG = wspy
SRCS = cpu_info.c error.c proctree.c topdown.c
OBJS = cpu_info.o error.o proctree.o topdown.o
LIBS = -lpthread -lm

all:	wspy cpu_info proctree

wspy:	topdown.o error.o cpu_info.c cpu_info.h
	$(CC) -o wspy $(CFLAGS) topdown.o cpu_info.c error.o

proctree:	proctree.o error.o
	$(CC) -o proctree proctree.o error.o

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info -DTEST_CPU_INFO cpu_info.c error.o

topdown.o:	topdown.c
	$(CC) -c $(CFLAGS) topdown.c

proctree.o:	proctree.c
	$(CC) -c $(CFLAGS) proctree.c

depend:
	-makedepend -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy wspy cpu_info proctree

# DO NOT DELETE

cpu_info.o: /usr/include/stdio.h /usr/include/bits/libc-header-start.h
cpu_info.o: /usr/include/features.h /usr/include/features-time64.h
cpu_info.o: /usr/include/bits/wordsize.h /usr/include/bits/timesize.h
cpu_info.o: /usr/include/stdc-predef.h /usr/include/sys/cdefs.h
cpu_info.o: /usr/include/bits/long-double.h /usr/include/gnu/stubs.h
cpu_info.o: /usr/include/bits/types.h /usr/include/bits/typesizes.h
cpu_info.o: /usr/include/bits/time64.h /usr/include/bits/types/__fpos_t.h
cpu_info.o: /usr/include/bits/types/__mbstate_t.h
cpu_info.o: /usr/include/bits/types/__fpos64_t.h
cpu_info.o: /usr/include/bits/types/__FILE.h /usr/include/bits/types/FILE.h
cpu_info.o: /usr/include/bits/types/struct_FILE.h
cpu_info.o: /usr/include/bits/types/cookie_io_functions_t.h
cpu_info.o: /usr/include/bits/stdio_lim.h /usr/include/bits/floatn.h
cpu_info.o: /usr/include/bits/floatn-common.h /usr/include/stdlib.h
cpu_info.o: /usr/include/bits/waitflags.h /usr/include/bits/waitstatus.h
cpu_info.o: /usr/include/bits/types/locale_t.h
cpu_info.o: /usr/include/bits/types/__locale_t.h /usr/include/sys/types.h
cpu_info.o: /usr/include/bits/types/clock_t.h
cpu_info.o: /usr/include/bits/types/clockid_t.h
cpu_info.o: /usr/include/bits/types/time_t.h
cpu_info.o: /usr/include/bits/types/timer_t.h /usr/include/bits/stdint-intn.h
cpu_info.o: /usr/include/endian.h /usr/include/bits/endian.h
cpu_info.o: /usr/include/bits/endianness.h /usr/include/bits/byteswap.h
cpu_info.o: /usr/include/bits/uintn-identity.h /usr/include/sys/select.h
cpu_info.o: /usr/include/bits/select.h /usr/include/bits/types/sigset_t.h
cpu_info.o: /usr/include/bits/types/__sigset_t.h
cpu_info.o: /usr/include/bits/types/struct_timeval.h
cpu_info.o: /usr/include/bits/types/struct_timespec.h
cpu_info.o: /usr/include/bits/pthreadtypes.h
cpu_info.o: /usr/include/bits/thread-shared-types.h
cpu_info.o: /usr/include/bits/pthreadtypes-arch.h
cpu_info.o: /usr/include/bits/atomic_wide_counter.h
cpu_info.o: /usr/include/bits/struct_mutex.h
cpu_info.o: /usr/include/bits/struct_rwlock.h /usr/include/alloca.h
cpu_info.o: /usr/include/bits/stdlib-float.h /usr/include/string.h
cpu_info.o: /usr/include/strings.h /usr/include/sched.h
cpu_info.o: /usr/include/bits/sched.h
cpu_info.o: /usr/include/bits/types/struct_sched_param.h
cpu_info.o: /usr/include/bits/cpu-set.h /usr/include/unistd.h
cpu_info.o: /usr/include/bits/posix_opt.h /usr/include/bits/environments.h
cpu_info.o: /usr/include/bits/confname.h /usr/include/bits/getopt_posix.h
cpu_info.o: /usr/include/bits/getopt_core.h /usr/include/bits/unistd_ext.h
cpu_info.o: /usr/include/sys/sysinfo.h /usr/include/linux/kernel.h
cpu_info.o: /usr/include/linux/sysinfo.h /usr/include/linux/types.h
cpu_info.o: /usr/include/asm/types.h /usr/include/asm-generic/types.h
cpu_info.o: /usr/include/asm-generic/int-ll64.h
cpu_info.o: /usr/include/asm/bitsperlong.h
cpu_info.o: /usr/include/asm-generic/bitsperlong.h
cpu_info.o: /usr/include/linux/posix_types.h /usr/include/linux/stddef.h
cpu_info.o: /usr/include/asm/posix_types.h /usr/include/asm/posix_types_64.h
cpu_info.o: /usr/include/asm-generic/posix_types.h /usr/include/linux/const.h
cpu_info.o: /usr/include/sys/stat.h /usr/include/bits/stat.h
cpu_info.o: /usr/include/bits/struct_stat.h /usr/include/bits/statx.h
cpu_info.o: /usr/include/bits/statx-generic.h
cpu_info.o: /usr/include/bits/types/struct_statx_timestamp.h
cpu_info.o: /usr/include/bits/types/struct_statx.h cpu_info.h
cpu_info.o: /usr/include/linux/perf_event.h /usr/include/linux/ioctl.h
cpu_info.o: /usr/include/asm/ioctl.h /usr/include/asm-generic/ioctl.h
cpu_info.o: /usr/include/asm/byteorder.h
cpu_info.o: /usr/include/linux/byteorder/little_endian.h
cpu_info.o: /usr/include/linux/swab.h /usr/include/asm/swab.h error.h
error.o: /usr/include/stdlib.h /usr/include/bits/libc-header-start.h
error.o: /usr/include/features.h /usr/include/features-time64.h
error.o: /usr/include/bits/wordsize.h /usr/include/bits/timesize.h
error.o: /usr/include/stdc-predef.h /usr/include/sys/cdefs.h
error.o: /usr/include/bits/long-double.h /usr/include/gnu/stubs.h
error.o: /usr/include/bits/waitflags.h /usr/include/bits/waitstatus.h
error.o: /usr/include/bits/floatn.h /usr/include/bits/floatn-common.h
error.o: /usr/include/bits/types/locale_t.h
error.o: /usr/include/bits/types/__locale_t.h /usr/include/sys/types.h
error.o: /usr/include/bits/types.h /usr/include/bits/typesizes.h
error.o: /usr/include/bits/time64.h /usr/include/bits/types/clock_t.h
error.o: /usr/include/bits/types/clockid_t.h /usr/include/bits/types/time_t.h
error.o: /usr/include/bits/types/timer_t.h /usr/include/bits/stdint-intn.h
error.o: /usr/include/endian.h /usr/include/bits/endian.h
error.o: /usr/include/bits/endianness.h /usr/include/bits/byteswap.h
error.o: /usr/include/bits/uintn-identity.h /usr/include/sys/select.h
error.o: /usr/include/bits/select.h /usr/include/bits/types/sigset_t.h
error.o: /usr/include/bits/types/__sigset_t.h
error.o: /usr/include/bits/types/struct_timeval.h
error.o: /usr/include/bits/types/struct_timespec.h
error.o: /usr/include/bits/pthreadtypes.h
error.o: /usr/include/bits/thread-shared-types.h
error.o: /usr/include/bits/pthreadtypes-arch.h
error.o: /usr/include/bits/atomic_wide_counter.h
error.o: /usr/include/bits/struct_mutex.h /usr/include/bits/struct_rwlock.h
error.o: /usr/include/alloca.h /usr/include/bits/stdlib-float.h
error.o: /usr/include/string.h /usr/include/strings.h error.h
error.o: /usr/include/stdio.h /usr/include/bits/types/__fpos_t.h
error.o: /usr/include/bits/types/__mbstate_t.h
error.o: /usr/include/bits/types/__fpos64_t.h
error.o: /usr/include/bits/types/__FILE.h /usr/include/bits/types/FILE.h
error.o: /usr/include/bits/types/struct_FILE.h
error.o: /usr/include/bits/types/cookie_io_functions_t.h
error.o: /usr/include/bits/stdio_lim.h
proctree.o: /usr/include/stdio.h /usr/include/bits/libc-header-start.h
proctree.o: /usr/include/features.h /usr/include/features-time64.h
proctree.o: /usr/include/bits/wordsize.h /usr/include/bits/timesize.h
proctree.o: /usr/include/stdc-predef.h /usr/include/sys/cdefs.h
proctree.o: /usr/include/bits/long-double.h /usr/include/gnu/stubs.h
proctree.o: /usr/include/bits/types.h /usr/include/bits/typesizes.h
proctree.o: /usr/include/bits/time64.h /usr/include/bits/types/__fpos_t.h
proctree.o: /usr/include/bits/types/__mbstate_t.h
proctree.o: /usr/include/bits/types/__fpos64_t.h
proctree.o: /usr/include/bits/types/__FILE.h /usr/include/bits/types/FILE.h
proctree.o: /usr/include/bits/types/struct_FILE.h
proctree.o: /usr/include/bits/types/cookie_io_functions_t.h
proctree.o: /usr/include/bits/stdio_lim.h /usr/include/bits/floatn.h
proctree.o: /usr/include/bits/floatn-common.h /usr/include/unistd.h
proctree.o: /usr/include/bits/posix_opt.h /usr/include/bits/environments.h
proctree.o: /usr/include/bits/confname.h /usr/include/bits/getopt_posix.h
proctree.o: /usr/include/bits/getopt_core.h /usr/include/bits/unistd_ext.h
proctree.o: /usr/include/getopt.h /usr/include/bits/getopt_ext.h
proctree.o: /usr/include/string.h /usr/include/bits/types/locale_t.h
proctree.o: /usr/include/bits/types/__locale_t.h /usr/include/strings.h
proctree.o: /usr/include/stdlib.h /usr/include/bits/waitflags.h
proctree.o: /usr/include/bits/waitstatus.h /usr/include/sys/types.h
proctree.o: /usr/include/bits/types/clock_t.h
proctree.o: /usr/include/bits/types/clockid_t.h
proctree.o: /usr/include/bits/types/time_t.h
proctree.o: /usr/include/bits/types/timer_t.h /usr/include/bits/stdint-intn.h
proctree.o: /usr/include/endian.h /usr/include/bits/endian.h
proctree.o: /usr/include/bits/endianness.h /usr/include/bits/byteswap.h
proctree.o: /usr/include/bits/uintn-identity.h /usr/include/sys/select.h
proctree.o: /usr/include/bits/select.h /usr/include/bits/types/sigset_t.h
proctree.o: /usr/include/bits/types/__sigset_t.h
proctree.o: /usr/include/bits/types/struct_timeval.h
proctree.o: /usr/include/bits/types/struct_timespec.h
proctree.o: /usr/include/bits/pthreadtypes.h
proctree.o: /usr/include/bits/thread-shared-types.h
proctree.o: /usr/include/bits/pthreadtypes-arch.h
proctree.o: /usr/include/bits/atomic_wide_counter.h
proctree.o: /usr/include/bits/struct_mutex.h
proctree.o: /usr/include/bits/struct_rwlock.h /usr/include/alloca.h
proctree.o: /usr/include/bits/stdlib-float.h error.h
topdown.o: /usr/include/stdio.h /usr/include/bits/libc-header-start.h
topdown.o: /usr/include/features.h /usr/include/features-time64.h
topdown.o: /usr/include/bits/wordsize.h /usr/include/bits/timesize.h
topdown.o: /usr/include/stdc-predef.h /usr/include/sys/cdefs.h
topdown.o: /usr/include/bits/long-double.h /usr/include/gnu/stubs.h
topdown.o: /usr/include/bits/types.h /usr/include/bits/typesizes.h
topdown.o: /usr/include/bits/time64.h /usr/include/bits/types/__fpos_t.h
topdown.o: /usr/include/bits/types/__mbstate_t.h
topdown.o: /usr/include/bits/types/__fpos64_t.h
topdown.o: /usr/include/bits/types/__FILE.h /usr/include/bits/types/FILE.h
topdown.o: /usr/include/bits/types/struct_FILE.h
topdown.o: /usr/include/bits/types/cookie_io_functions_t.h
topdown.o: /usr/include/bits/stdio_lim.h /usr/include/bits/floatn.h
topdown.o: /usr/include/bits/floatn-common.h /usr/include/string.h
topdown.o: /usr/include/bits/types/locale_t.h
topdown.o: /usr/include/bits/types/__locale_t.h /usr/include/strings.h
topdown.o: /usr/include/stdlib.h /usr/include/bits/waitflags.h
topdown.o: /usr/include/bits/waitstatus.h /usr/include/sys/types.h
topdown.o: /usr/include/bits/types/clock_t.h
topdown.o: /usr/include/bits/types/clockid_t.h
topdown.o: /usr/include/bits/types/time_t.h /usr/include/bits/types/timer_t.h
topdown.o: /usr/include/bits/stdint-intn.h /usr/include/endian.h
topdown.o: /usr/include/bits/endian.h /usr/include/bits/endianness.h
topdown.o: /usr/include/bits/byteswap.h /usr/include/bits/uintn-identity.h
topdown.o: /usr/include/sys/select.h /usr/include/bits/select.h
topdown.o: /usr/include/bits/types/sigset_t.h
topdown.o: /usr/include/bits/types/__sigset_t.h
topdown.o: /usr/include/bits/types/struct_timeval.h
topdown.o: /usr/include/bits/types/struct_timespec.h
topdown.o: /usr/include/bits/pthreadtypes.h
topdown.o: /usr/include/bits/thread-shared-types.h
topdown.o: /usr/include/bits/pthreadtypes-arch.h
topdown.o: /usr/include/bits/atomic_wide_counter.h
topdown.o: /usr/include/bits/struct_mutex.h /usr/include/bits/struct_rwlock.h
topdown.o: /usr/include/alloca.h /usr/include/bits/stdlib-float.h
topdown.o: /usr/include/stdint.h /usr/include/bits/wchar.h
topdown.o: /usr/include/bits/stdint-uintn.h /usr/include/unistd.h
topdown.o: /usr/include/bits/posix_opt.h /usr/include/bits/environments.h
topdown.o: /usr/include/bits/confname.h /usr/include/bits/getopt_posix.h
topdown.o: /usr/include/bits/getopt_core.h /usr/include/bits/unistd_ext.h
topdown.o: /usr/include/signal.h /usr/include/bits/signum-generic.h
topdown.o: /usr/include/bits/signum-arch.h
topdown.o: /usr/include/bits/types/sig_atomic_t.h
topdown.o: /usr/include/bits/types/siginfo_t.h
topdown.o: /usr/include/bits/types/__sigval_t.h
topdown.o: /usr/include/bits/siginfo-arch.h
topdown.o: /usr/include/bits/siginfo-consts.h
topdown.o: /usr/include/bits/siginfo-consts-arch.h
topdown.o: /usr/include/bits/types/sigval_t.h
topdown.o: /usr/include/bits/types/sigevent_t.h
topdown.o: /usr/include/bits/sigevent-consts.h /usr/include/bits/sigaction.h
topdown.o: /usr/include/bits/sigcontext.h /usr/include/bits/types/stack_t.h
topdown.o: /usr/include/sys/ucontext.h /usr/include/bits/sigstack.h
topdown.o: /usr/include/bits/sigstksz.h /usr/include/bits/ss_flags.h
topdown.o: /usr/include/bits/types/struct_sigstack.h
topdown.o: /usr/include/bits/sigthread.h /usr/include/bits/signal_ext.h
topdown.o: /usr/include/getopt.h /usr/include/bits/getopt_ext.h
topdown.o: /usr/include/time.h /usr/include/bits/time.h
topdown.o: /usr/include/bits/timex.h /usr/include/bits/types/struct_tm.h
topdown.o: /usr/include/bits/types/struct_itimerspec.h /usr/include/fcntl.h
topdown.o: /usr/include/bits/fcntl.h /usr/include/bits/fcntl-linux.h
topdown.o: /usr/include/bits/types/struct_iovec.h /usr/include/linux/falloc.h
topdown.o: /usr/include/bits/stat.h /usr/include/bits/struct_stat.h
topdown.o: /usr/include/sys/wait.h /usr/include/sys/sysinfo.h
topdown.o: /usr/include/linux/kernel.h /usr/include/linux/sysinfo.h
topdown.o: /usr/include/linux/types.h /usr/include/asm/types.h
topdown.o: /usr/include/asm-generic/types.h
topdown.o: /usr/include/asm-generic/int-ll64.h /usr/include/asm/bitsperlong.h
topdown.o: /usr/include/asm-generic/bitsperlong.h
topdown.o: /usr/include/linux/posix_types.h /usr/include/linux/stddef.h
topdown.o: /usr/include/asm/posix_types.h /usr/include/asm/posix_types_64.h
topdown.o: /usr/include/asm-generic/posix_types.h /usr/include/linux/const.h
topdown.o: /usr/include/sys/syscall.h /usr/include/asm/unistd.h
topdown.o: /usr/include/asm/unistd_64.h /usr/include/bits/syscall.h
topdown.o: /usr/include/sys/ioctl.h /usr/include/bits/ioctls.h
topdown.o: /usr/include/asm/ioctls.h /usr/include/asm-generic/ioctls.h
topdown.o: /usr/include/linux/ioctl.h /usr/include/asm/ioctl.h
topdown.o: /usr/include/asm-generic/ioctl.h /usr/include/bits/ioctl-types.h
topdown.o: /usr/include/sys/ttydefaults.h /usr/include/sys/resource.h
topdown.o: /usr/include/bits/resource.h
topdown.o: /usr/include/bits/types/struct_rusage.h /usr/include/sys/user.h
topdown.o: /usr/include/sys/stat.h /usr/include/bits/statx.h
topdown.o: /usr/include/bits/statx-generic.h
topdown.o: /usr/include/bits/types/struct_statx_timestamp.h
topdown.o: /usr/include/bits/types/struct_statx.h /usr/include/sys/ptrace.h
topdown.o: /usr/include/bits/ptrace-shared.h /usr/include/linux/perf_event.h
topdown.o: /usr/include/asm/byteorder.h
topdown.o: /usr/include/linux/byteorder/little_endian.h
topdown.o: /usr/include/linux/swab.h /usr/include/asm/swab.h
topdown.o: /usr/include/errno.h /usr/include/bits/errno.h
topdown.o: /usr/include/linux/errno.h /usr/include/asm/errno.h
topdown.o: /usr/include/asm-generic/errno.h
topdown.o: /usr/include/asm-generic/errno-base.h
topdown.o: /usr/include/bits/types/error_t.h error.h cpu_info.h
