#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"

#define NPROC (512)
#define FD_BUFFER_SIZE (16)
// Chapter 5 asks us to report per-syscall usage back to user space.
// The syscall ID space is sparse, but a fixed-size array keeps lookup
// simple inside the hot syscall path.
#define MAX_SYSCALL_NUM (500)
// Stride scheduling uses a large constant divided by priority.
// A higher priority therefore produces a smaller "pass" value,
// which means the process accumulates virtual time more slowly and
// gets chosen more often.
#define BIG_STRIDE (65536)

struct file;

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

typedef enum {
	// Mirrors the task status values expected by the user tests.
	UnInit,
	Ready,
	Running,
	Exited,
} TaskStatus;

typedef struct {
	// Snapshot of a process that can be copied out by sys_task_info().
	TaskStatus status;
	unsigned int syscall_times[MAX_SYSCALL_NUM];
	// Runtime in milliseconds since the task first got CPU time.
	int time;
} TaskInfo;

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	pagetable_t pagetable; // User page table
	uint64 ustack; // Virtual address of kernel stack
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	uint64 max_page;
	// Cycle counter captured the first time the scheduler actually runs
	// this process. We use it to report elapsed execution time.
	uint64 start_cycle;
	// Bookkeeping for sys_task_info(): incremented in syscall().
	unsigned int syscall_times[MAX_SYSCALL_NUM];
	struct proc *parent; // Parent process
	uint64 exit_code;
	// Stride scheduler state:
	// - priority is user-visible via set_priority()
	// - stride stores the task's accumulated virtual runtime
	// - pass is the amount added after each time slice
	int priority;
	uint64 stride;
	uint64 pass;
	struct file *files[FD_BUFFER_SIZE];
};

int cpuid();
struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
int fork();
int exec(char *);
int wait(int, int *);
int spawn(char *);
void add_task(struct proc *);
struct proc *allocproc();
int fdalloc(struct file *);
// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H
