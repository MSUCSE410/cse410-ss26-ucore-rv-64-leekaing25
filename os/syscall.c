#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
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

uint64 sys_gettimeofday(uint64 val_va, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	(void)_tz;
	if (val_va == 0)
		return -1;
	TimeVal val;
	uint64 cycle = get_cycle();
	val.sec = cycle / CPU_FREQ;
	val.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	if (copyout(curr_proc()->pagetable, val_va, (char *)&val, sizeof(val)) < 0)
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
	if (start >= MAXVA || start + unmap_len < start || start + unmap_len > MAXVA)
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

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
		curr_proc()->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
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
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
