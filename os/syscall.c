#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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
	// The timer hardware exposes cycles, so convert to the user-visible
	// seconds + microseconds layout expected by gettimeofday().
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
	// This Chapter 5 mmap is the minimal anonymous mapping interface used
	// by the tests: no file backing, just allocate pages and insert them
	// into the caller's page table.
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
	// First pass: validate the whole range before changing anything. This
	// prevents partially overlapping mappings from being accepted.
	for (uint64 va = start; va < start + map_len; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte != 0 && (*pte & PTE_V))
			return -1;
	}
	// Second pass: allocate physical pages and map them one page at a time.
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
	if (start >= MAXVA || start + unmap_len < start || start + unmap_len > MAXVA)
		return -1;

	struct proc *p = curr_proc();
	// As with mmap(), validate the entire range before mutating the page
	// table so an invalid address does not produce a partial unmap.
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
	// task_info reports coarse-grained runtime in milliseconds.
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
	// Translate the kernel's internal process state into the smaller API
	// contract used by the user-space tests.
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
		// The process has never been scheduled yet, so from the user's
		// perspective it has consumed no runtime.
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

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
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

	// Copy the user-provided program name into a kernel buffer before
	// resolving it in the built-in app table.
	if (copyinstr(p->pagetable, name, va, sizeof(name)) < 0)
		return -1;
	return spawn(name);
}

uint64 sys_set_priority(long long prio){
	// The tests require priorities >= 2. Smaller values would either make
	// BIG_STRIDE / priority invalid or give a process effectively infinite
	// scheduling weight.
	if (prio < 2)
		return -1;
	struct proc *p = curr_proc();
	p->priority = prio;
	// Recompute the stride increment immediately so future scheduling
	// decisions use the new weight.
	p->pass = BIG_STRIDE / p->priority;
	return prio;
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
	// Chapter 5's task_info syscall reports how many times each syscall was
	// invoked, so we count the dispatch here before entering the handler.
	if (id >= 0 && id < MAX_SYSCALL_NUM)
		curr_proc()->syscall_times[id]++;
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
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
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
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
