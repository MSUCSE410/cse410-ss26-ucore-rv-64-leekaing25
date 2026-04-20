#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "file.h"
#include "fcntl.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

static uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

static uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
	return -1;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
	return -1;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	(void)_tz;
	if (val == 0)
		return -1;
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	if (copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal)) < 0)
		return -1;
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	(void)flag;
	(void)fd;
	if (len == 0)
		return 0;
	if (!PGALIGNED(start))
		return -1;
	if (len > (1ULL << 30))
		return -1;
	if ((port & ~0x7) != 0 || (port & 0x7) == 0)
		return -1;

	uint64 map_len = PGROUNDUP(len);
	if (start >= MAXVA || start + map_len < start || start + map_len > MAXVA)
		return -1;

	int perm = PTE_U;
	if (port & 0x1)
		perm |= PTE_R;
	if (port & 0x2)
		perm |= PTE_W;
	if (port & 0x4)
		perm |= PTE_X;

	struct proc *p = curr_proc();
	for (uint64 va = start; va < start + map_len; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte != 0 && (*pte & PTE_V))
			return -1;
	}
	for (uint64 va = start; va < start + map_len; va += PGSIZE) {
		char *pa = kalloc();
		if (pa == 0)
			return -1;
		if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) < 0)
			return -1;
	}
	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	if (len == 0)
		return 0;
	if (!PGALIGNED(start))
		return -1;

	uint64 unmap_len = PGROUNDUP(len);
	if (start >= MAXVA || start + unmap_len < start ||
	    start + unmap_len > MAXVA)
		return -1;

	struct proc *p = curr_proc();
	for (uint64 va = start; va < start + unmap_len; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte == 0 || (*pte & PTE_V) == 0)
			return -1;
	}
	uvmunmap(p->pagetable, start, unmap_len / PGSIZE, 1);
	return 0;
}

static inline uint64 cycles_to_ms(uint64 cycles)
{
	return (cycles * 1000) / CPU_FREQ;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_task_info(uint64 ti_va)
{
	if (ti_va == 0)
		return -1;
	struct proc *p = curr_proc();
	TaskInfo info;

	switch (p->state) {
	case UNUSED:
		info.status = UnInit;
		break;
	case RUNNABLE:
	case USED:
	case SLEEPING:
		info.status = Ready;
		break;
	case RUNNING:
		info.status = Running;
		break;
	case ZOMBIE:
		info.status = Exited;
		break;
	default:
		info.status = UnInit;
		break;
	}
	memmove(info.syscall_times, p->syscall_times, sizeof(info.syscall_times));
	if (p->start_cycle == 0) {
		info.time = 0;
	} else {
		uint64 now = get_cycle();
		uint64 total_cycles =
			(now > p->start_cycle) ? (now - p->start_cycle) : 0;
		info.time = (int)cycles_to_ms(total_cycles);
	}
	if (copyout(p->pagetable, ti_va, (char *)&info, sizeof(info)) < 0)
		return -1;
	return 0;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	(void)uargv;
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];

	if (copyinstr(p->pagetable, name, path, sizeof(name)) < 0)
		return -1;
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];

	if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0)
		return -1;
	return spawn(name);
}

uint64 sys_set_priority(long long prio)
{
	if (prio < 2)
		return -1;
	struct proc *p = curr_proc();
	p->priority = prio;
	p->pass = BIG_STRIDE / p->priority;
	return prio;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	(void)_flags;
	struct proc *p = curr_proc();
	char path[200];

	if (copyinstr(p->pagetable, path, va, sizeof(path)) < 0)
		return -1;
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd, uint64 stat)
{
	struct proc *p = curr_proc();
	struct file *f;
	Stat st;

	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	f = p->files[fd];
	if (f == 0 || f->type != FD_INODE)
		return -1;

	// fstat reports inode metadata for regular file descriptors only.
	ivalid(f->ip);
	st.dev = 0;
	st.ino = f->ip->inum;
	st.mode = (f->ip->type == T_DIR) ? DIR : FILE;
	st.nlink = f->ip->nlink;
	memset(st.pad, 0, sizeof(st.pad));

	// Marshal the kernel-side Stat to user memory; never dereference the user
	// pointer directly inside the kernel.
	if (copyout(p->pagetable, stat, (char *)&st, sizeof(st)) < 0)
		return -1;
	return 0;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath,
	       uint64 flags)
{
	struct proc *p = curr_proc();
	struct inode *dp, *ip;
	char old_name[200];
	char new_name[200];

	(void)olddirfd;
	(void)newdirfd;
	(void)flags;

	if (copyinstr(p->pagetable, old_name, oldpath, sizeof(old_name)) < 0)
		return -1;
	if (copyinstr(p->pagetable, new_name, newpath, sizeof(new_name)) < 0)
		return -1;
	// This lab's filesystem is flat, so linking a file to the same name is a
	// no-op that we reject as an error.
	if (strncmp(old_name, new_name, sizeof(old_name)) == 0)
		return -1;

	ip = namei(old_name);
	if (ip == 0)
		return -1;
	ivalid(ip);
	if (ip->type != T_FILE) {
		iput(ip);
		return -1;
	}

	// First make the inode durable with its incremented nlink. If dirlink
	// later fails, we roll the increment back before returning.
	ip->nlink++;
	iupdate(ip);

	dp = root_dir();
	if (dirlink(dp, new_name, ip->inum) < 0) {
		ip->nlink--;
		iupdate(ip);
		iput(dp);
		iput(ip);
		return -1;
	}

	iput(dp);
	iput(ip);
	return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	struct proc *p = curr_proc();
	struct inode *dp, *ip;
	struct dirent empty;
	char path[200];
	uint off;

	(void)dirfd;
	(void)flags;

	if (copyinstr(p->pagetable, path, name, sizeof(path)) < 0)
		return -1;

	dp = root_dir();
	ip = dirlookup(dp, path, &off);
	if (ip == 0) {
		iput(dp);
		return -1;
	}

	ivalid(ip);
	memset(&empty, 0, sizeof(empty));
	// Remove the directory entry first so future lookups stop reaching this
	// inode even if the inode itself still has open-file references.
	if (writei(dp, 0, (uint64)&empty, off, sizeof(empty)) != sizeof(empty)) {
		iput(dp);
		iput(ip);
		return -1;
	}

	if (ip->nlink < 1)
		panic("unlinkat: nlink");
	// Dropping the last hard link does not necessarily free the inode right
	// here; iput() performs reclamation once no in-memory references remain.
	ip->nlink--;
	iupdate(ip);

	iput(dp);
	iput(ip);
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	if (id >= 0 && id < MAX_SYSCALL_NUM)
		curr_proc()->syscall_times[id]++;
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_task_info:
		ret = sys_task_info(args[0]);
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone:
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_fstat:
		ret = sys_fstat(args[0], args[1]);
		break;
	case SYS_linkat:
		ret = sys_linkat(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_unlinkat:
		ret = sys_unlinkat(args[0], args[1], args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
