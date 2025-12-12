#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// 仅在启用静态优先级调度时编译此函数
#if defined(SCHED_GLOBAL_PRIO) || defined(SCHED_PERCPU_PRIO)
uint64
sys_setpriority(void)
{
  int priority;
  
  argint(0, &priority);

  if (priority < MIN_PRIORITY || priority > MAX_PRIORITY)
    return -1;

  struct proc *p = myproc();
  acquire(&p->lock);
  p->priority = priority;
  release(&p->lock);

  // 当进程设置了新的优先级时，它应该主动放弃CPU，
  // 以便调度器可以立即选出优先级最高的进程。
  yield();

  return 0;
}
#else
// 如果未使用静态优先级，则提供一个空实现
uint64
sys_setpriority(void)
{
  return -1; // Or 0, to indicate not supported
}
#endif

// ============================================================================
// Direct MLFQ Scheduler System Calls
// ============================================================================

#ifdef SCHED_DIRECT_MLFQ
// Get current process priority
uint64
sys_getprio(void)
{
  return get_proc_priority();
}

// Set current process priority
uint64
sys_setprio(void)
{
  int new_prio;
  argint(0, &new_prio);
  return set_proc_priority(new_prio);
}

// Get scheduler statistics (context switch count)
uint64
sys_schedstat(void)
{
  return get_sched_stats();
}

#else
// Stub implementations when SCHED_DIRECT_MLFQ is not enabled
uint64
sys_getprio(void)
{
  return -1;
}

uint64
sys_setprio(void)
{
  return -1;
}

uint64
sys_schedstat(void)
{
  return 0;
}
#endif