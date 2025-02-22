#include <cpu.h>
#include <string.h>
#include <debug.h>
#include <sched/sched.h>

struct syscall_handle {
	void (*handler)(struct registers*);
	const char *name;
};

extern void syscall_openat(struct registers*);
extern void syscall_close(struct registers*);
extern void syscall_read(struct registers*);
extern void syscall_write(struct registers*);
extern void syscall_seek(struct registers*);
extern void syscall_mmap(struct registers*);
extern void syscall_munmap(struct registers*);
extern void syscall_stat(struct registers*);
extern void syscall_statat(struct registers*);
extern void syscall_getpid(struct registers*);
extern void syscall_getppid(struct registers*);
extern void syscall_gettid(struct registers*);
extern void syscall_dup(struct registers*);
extern void syscall_dup2(struct registers*);
extern void syscall_fcntl(struct registers*);
extern void syscall_fork(struct registers*);
extern void syscall_exit(struct registers*);
extern void syscall_waitpid(struct registers*);
extern void syscall_execve(struct registers*);
extern void syscall_waitpid(struct registers*);
extern void syscall_readdir(struct registers*);
extern void syscall_chdir(struct registers*);
extern void syscall_getcwd(struct registers*);
extern void syscall_faccessat(struct registers*);
extern void syscall_pipe(struct registers*);
extern void syscall_ioctl(struct registers*);
extern void syscall_umask(struct registers*);
extern void syscall_getuid(struct registers*);
extern void syscall_geteuid(struct registers*);
extern void syscall_setuid(struct registers*);
extern void syscall_seteuid(struct registers*);
extern void syscall_getgid(struct registers*);
extern void syscall_getegid(struct registers*);
extern void syscall_setgid(struct registers*);
extern void syscall_setegid(struct registers*);
extern void syscall_fchmod(struct registers*);
extern void syscall_fchmodat(struct registers*);
extern void syscall_fchownat(struct registers*);
extern void syscall_sigaction(struct registers*);
extern void syscall_sigpending(struct registers*);
extern void syscall_sigprocmask(struct registers*);
extern void syscall_kill(struct registers*);
extern void syscall_setpgid(struct registers*);
extern void syscall_getpgid(struct registers*);
extern void syscall_setsid(struct registers*);
extern void syscall_getsid(struct registers*);

static void syscall_set_fs_base(struct registers *regs) {
	uint64_t addr = regs->rdi;

	CURRENT_THREAD->user_fs_base = addr;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] set_fs_base: addr {%x}\n", CORE_LOCAL->pid, addr);
#endif

	set_user_fs(addr);

	regs->rax = 0;
}

static void syscall_get_fs_base(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] get_fs_base\n", CORE_LOCAL->pid);
#endif

	regs->rax = get_user_fs();
}

static void syscall_set_gs_base(struct registers *regs) {
	uint64_t addr = regs->rdi;

	CURRENT_THREAD->user_gs_base = addr;

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] set_gs_base: addr {%x}\n", CORE_LOCAL->pid, addr);
#endif

	set_user_gs(addr);

	regs->rax = 0;
}

static void syscall_get_gs_base(struct registers *regs) {
#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] get_gs_base\n", CORE_LOCAL->pid);
#endif

	regs->rax = get_user_gs();
}

static void syscall_syslog(struct registers *regs) {
	const char *str = (void*)regs->rdi;
	print("%s\n", str);
}

static struct syscall_handle syscall_list[] = {
	{ .handler = syscall_openat, .name = "open" }, // 0
	{ .handler = syscall_close, .name = "close" }, // 1
	{ .handler = syscall_read, .name = "read" }, // 2
	{ .handler = syscall_write, .name = "write" }, // 3
	{ .handler = syscall_seek, .name = "seek" }, // 4
	{ .handler = syscall_dup, .name = "dup" }, // 5
	{ .handler = syscall_dup2, .name = "dup2" }, // 6
	{ .handler = syscall_mmap, .name = "mmap" }, // 7
	{ .handler = syscall_munmap, .name = "munamp" }, // 8
	{ .handler = syscall_set_fs_base, .name = "set_fs_base" }, // 9
	{ .handler = syscall_set_gs_base, .name = "set_gs_base" }, // 10
	{ .handler = syscall_get_fs_base, .name = "get_fs_base" }, // 11
	{ .handler = syscall_get_gs_base, .name = "get_gs_base" }, // 12
	{ .handler = syscall_syslog, .name = "syslog" }, // 13
	{ .handler = syscall_exit, .name = "exit" }, // 14
	{ .handler = syscall_getpid, .name = "getpid" }, // 15
	{ .handler = syscall_gettid, .name = "gettid" }, // 16
	{ .handler = syscall_getppid, .name = "getppid" }, // 17
	{ .handler = NULL, .name = "isatty" }, // 18
	{ .handler = syscall_fcntl, .name = "fcntl" }, // 19
	{ .handler = syscall_stat, .name = "fstat" }, // 20
	{ .handler = syscall_statat, .name = "fstatat" }, // 21
	{ .handler = syscall_ioctl, .name = "ioctl" }, // 22
	{ .handler = syscall_fork, .name = "fork" }, // 23
	{ .handler = syscall_waitpid, .name = "waitpid" }, // 24
	{ .handler = syscall_readdir, .name = "readdir" }, // 25
	{ .handler = syscall_execve, .name = "execve" }, // 26
	{ .handler = syscall_getcwd, .name = "getcwd" }, // 27
	{ .handler = syscall_chdir, .name = "chdir" }, // 28
	{ .handler = syscall_faccessat, .name = "faccessat" }, // 29
	{ .handler = syscall_pipe, .name = "pipe" }, // 30
	{ .handler = syscall_umask, .name = "umask" }, // 31
	{ .handler = syscall_getuid, .name = "getuid" }, // 32
	{ .handler = syscall_geteuid, .name = "geteuid" }, // 33
	{ .handler = syscall_setuid, .name = "setuid" }, // 34
	{ .handler = syscall_seteuid, .name = "seteuid" }, // 35
	{ .handler = syscall_getgid, .name = "getgid" }, // 36
	{ .handler = syscall_getegid, .name = "getegid" }, // 37
	{ .handler = syscall_setgid, .name = "setgid" }, // 38
	{ .handler = syscall_setegid, .name = "setegid" }, // 39
	{ .handler = syscall_fchmod, .name = "fchmod" }, // 40
	{ .handler = syscall_fchmodat, .name = "fchmodat" }, // 41
	{ .handler = syscall_fchownat, .name = "fchownat" }, // 42
	{ .handler = syscall_sigaction, .name = "sigaction" }, // 43
	{ .handler = syscall_sigpending, .name = "sigpending" }, // 44
	{ .handler = syscall_sigprocmask, .name = "sigprocmask" }, // 45
	{ .handler = syscall_kill, .name = "kill" }, // 46
	{ .handler = syscall_setpgid, .name = "setpgid" }, // 47
	{ .handler = syscall_getpgid, .name = "getpgid" }, // 48
	{ .handler = syscall_setsid, .name = "setsid" }, // 49
	{ .handler = syscall_getsid, .name = "getsid" } // 50
};

extern void syscall_handler(struct registers *regs) {
	uint64_t syscall_number = regs->rax;

	if(syscall_number >= LENGTHOF(syscall_list)) {
		print("SYSCALL: unknown syscall number %d\n", syscall_number);
		return;
	}

	if(syscall_list[syscall_number].handler != NULL) {
		syscall_list[syscall_number].handler(regs);
	} else {
		panic("null syscall %s", syscall_list[syscall_number].name);
	}

	if(regs->rax != -1) {
		set_errno(0);
	}

#ifndef SYSCALL_DEBUG
	print("syscall: [pid %x] %s returning %x with errno %d\n", CORE_LOCAL->pid, syscall_list[syscall_number].name, regs->rax, get_errno());
#endif
}
