#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, char *str, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, str, len);
	if (fd != STDOUT)
		return -1;
	for (int i = 0; i < len; ++i) {
		console_putchar(str[i]);
	}
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

uint64 sys_gettimeofday(TimeVal *val, int _tz)
{
	uint64 cycle = get_cycle();
	val->sec = cycle / CPU_FREQ;
	val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
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

uint64 sys_task_info(TaskInfo *ti)
{
	if (ti == 0)
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
	memmove(info.syscall_times, p->syscall_times,
		sizeof(info.syscall_times));
	if (p->start_cycle == 0) {
		info.time = 0;
	} else {
		uint64 now = get_cycle();
		uint64 total_cycles =
			(now > p->start_cycle) ? (now - p->start_cycle) : 0;
		info.time = (int)cycles_to_ms(total_cycles);
	}
	memmove(ti, &info, sizeof(info));
	return 0;
}

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
		ret = sys_write(args[0], (char *)args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
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
