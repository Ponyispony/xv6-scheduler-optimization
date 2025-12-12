#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

#ifdef SCHED_DIRECT_MLFQ
// Multi-Level Feedback Queue: explicit ready queues for each priority
struct ready_queue ready_queues[NPRIO];
struct spinlock queue_lock;

// Statistics for testing
uint64 context_switches = 0;
uint64 last_boost_tick = 0;
#endif


// --- 链表辅助函数 ---
#if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ) || defined(SCHED_PERCPU_PRIO)
// 链表辅助函数的实现
void list_init(struct proc_list *list) {
  list->head = 0;
  list->tail = 0;
}
int list_empty(struct proc_list *list) {
  return list->head == 0;
}
void list_push(struct proc_list *list, struct proc *p) {
  p->next = 0;
  p->prev = list->tail;
  if (list_empty(list)) {
    list->head = p;
  } else {
    list->tail->next = p;
  }
  list->tail = p;
}
struct proc *list_pop(struct proc_list *list) {
  if (list_empty(list)) {
    return 0;
  }
  struct proc *p = list->head;
  list->head = p->next;
  if (list->head) {
    list->head->prev = 0;
  } else {
    list->tail = 0;
  }
  p->next = 0;
  p->prev = 0;
  return p;
}
// 从列表中移除特定进程
struct proc *list_remove(struct proc_list *list, struct proc *p) {
  if (p->prev) {
    p->prev->next = p->next;
  } else {
    list->head = p->next;
  }
  if (p->next) {
    p->next->prev = p->prev;
  } else {
    list->tail = p->prev;
  }
  p->next = 0;
  p->prev = 0;
  return p;
}
#endif

// --- 全局 MLFQ 调度器数据 ---
#ifdef SCHED_GLOBAL_MLFQ
struct proc_list mlfq_queues[NUM_MLFQ_QUEUES]; // 全局MLFQ队列
struct spinlock mlfq_lock; // 保护全局MLFQ队列的锁
uint last_boost_time; // 上次全局提升的时间
#endif

// --- 每 CPU MLFQ 调度器数据 ---
#ifdef SCHED_PERCPU_MLFQ
static struct spinlock boost_lock;
static uint last_boost_time_percpu = 0;
#endif

// --- 全局静态优先级调度器数据 ---
#ifdef SCHED_GLOBAL_PRIO
struct spinlock scheduler_lock; // 单一全局调度锁
#endif
// ---


// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  #ifdef SCHED_DIRECT_MLFQ
  initlock(&queue_lock, "ready_queue");
  
  // Initialize ready queues for MLFQ
  for(int i = 0; i < NPRIO; i++) {
    ready_queues[i].head = 0;
    ready_queues[i].tail = 0;
    ready_queues[i].count = 0;
  }
  
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
      p->priority = PRIO_HIGH;
      p->time_slice = TIME_SLICE_HIGH;
      p->total_ticks = 0;
      p->start_tick = 0;
      p->is_idle = 0;
      p->queue_next = 0;
      p->queue_prev = 0;
  }
  #else
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
  #endif

  // --- 调度器初始化 ---
  #ifdef SCHED_GLOBAL_PRIO
  initlock(&scheduler_lock, "scheduler");
  #endif

  #ifdef SCHED_GLOBAL_MLFQ
  initlock(&mlfq_lock, "mlfq");
  for (int i = 0; i < NUM_MLFQ_QUEUES; i++) {
    list_init(&mlfq_queues[i]);
  }
  last_boost_time = 0;
  #endif

  #ifdef SCHED_PERCPU_MLFQ
  for(struct cpu *c = cpus; c < &cpus[NCPU]; c++) {
    initlock(&c->mlfq_lock, "mlfq_cpu");
    for (int i = 0; i < NUM_MLFQ_QUEUES; i++) {
      list_init(&c->mlfq_queues[i]);
    }
  }
  initlock(&boost_lock, "boost_lock"); 
  #endif

  #ifdef SCHED_PERCPU_PRIO
  for(struct cpu *c = cpus; c < &cpus[NCPU]; c++) {
    initlock(&c->prio_lock, "prio_cpu");
    for (int i = 0; i < NUM_PRIO_QUEUES; i++) {
      list_init(&c->prio_queues[i]);
    }
  }
  #endif
  // ---
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  // --- 调度器字段初始化 ---
  #if defined(SCHED_GLOBAL_PRIO) || defined(SCHED_PERCPU_PRIO)
  p->priority = DEFAULT_PRIORITY; // 新进程使用默认静态优先级
  #endif

  #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ)
  p->priority = MLFQ_DEFAULT_PRIORITY; // 新进程进入最高优先级队列
  p->ticks_in_queue = 0;
  acquire(&tickslock);
  p->creation_time = ticks; // 记录创建时间
  release(&tickslock);
  #endif

  #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ) || defined(SCHED_PERCPU_PRIO)
  p->next = 0;
  p->prev = 0;
  #endif

  #ifdef SCHED_DIRECT_MLFQ
  // Initialize MLFQ fields: new processes start at highest priority
  p->priority = PRIO_HIGH;
  p->time_slice = TIME_SLICE_HIGH;
  p->total_ticks = 0;
  p->is_idle = 0;
  p->queue_next = 0;
  p->queue_prev = 0;
  #endif
  // ---

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// ============================================================================
// Ready Queue Management (Explicit Queue) - SCHED_DIRECT_MLFQ
// ============================================================================
#ifdef SCHED_DIRECT_MLFQ

// Enqueue a process to its priority queue
// Must hold queue_lock
static void
enqueue(struct proc *p)
{
  int prio = p->priority;
  struct ready_queue *q = &ready_queues[prio];
  
  p->queue_next = 0;
  p->queue_prev = q->tail;
  
  if(q->tail) {
    q->tail->queue_next = p;
  } else {
    q->head = p;
  }
  q->tail = p;
  q->count++;
}

// Dequeue a process from its priority queue
// Must hold queue_lock
static void
dequeue(struct proc *p)
{
  int prio = p->priority;
  struct ready_queue *q = &ready_queues[prio];
  
  if(p->queue_prev) {
    p->queue_prev->queue_next = p->queue_next;
  } else {
    q->head = p->queue_next;
  }
  
  if(p->queue_next) {
    p->queue_next->queue_prev = p->queue_prev;
  } else {
    q->tail = p->queue_prev;
  }
  
  p->queue_next = 0;
  p->queue_prev = 0;
  q->count--;
}

// Find the highest priority runnable process (excluding idle)
// Must hold queue_lock, removes from queue
static struct proc*
find_and_remove_runnable(void)
{
  for(int prio = 0; prio < NPRIO; prio++) {
    struct proc *p = ready_queues[prio].head;
    while(p) {
      if(p->state == RUNNABLE && !p->is_idle) {
        dequeue(p);
        return p;
      }
      p = p->queue_next;
    }
  }
  return 0;
}

// Get time slice for a priority level
static int
get_time_slice(int priority)
{
  switch(priority) {
    case PRIO_HIGH: return TIME_SLICE_HIGH;
    case PRIO_MED:  return TIME_SLICE_MED;
    case PRIO_LOW:  return TIME_SLICE_LOW;
    default:        return TIME_SLICE_IDLE;
  }
}

// Demote process priority (MLFQ: process used full time slice)
static void
demote_priority(struct proc *p)
{
  if(p->priority < PRIO_IDLE - 1 && !p->is_idle) {
    p->priority++;
    p->time_slice = get_time_slice(p->priority);
    p->total_ticks = 0;
  }
}

// Boost all process priorities periodically (prevent starvation)
void
priority_boost(void)
{
  acquire(&queue_lock);
  
  // Move all processes to highest priority queue
  for(int prio = 1; prio < NPRIO; prio++) {
    struct proc *p = ready_queues[prio].head;
    while(p) {
      struct proc *next = p->queue_next;
      if(!p->is_idle && p->state == RUNNABLE) {
        dequeue(p);
        p->priority = PRIO_HIGH;
        p->time_slice = TIME_SLICE_HIGH;
        p->total_ticks = 0;
        enqueue(p);
      }
      p = next;
    }
  }
  
  release(&queue_lock);
}

// release_prev_proc() - Release the previous process's lock after direct switch
// Called by the newly running process after context switch
static void
release_prev_proc(void)
{
  struct cpu *c = mycpu();
  struct proc *prev = c->prev_proc;
  
  if(prev) {
    release(&prev->lock);
    c->prev_proc = 0;
  }
}

// Forward declaration
static void idle_loop(void);

// Idle loop for idle process - just loop and yield
static void
idle_loop(void)
{
  struct cpu *c = mycpu();
  struct proc *p = myproc();
  
  // Release previous process's lock (from direct switch)
  if(c->prev_proc) {
    release(&c->prev_proc->lock);
    c->prev_proc = 0;
  }
  
  // Release our own lock
  release(&p->lock);
  
  for(;;) {
    intr_on();
    // Wait for interrupt
    asm volatile("wfi");
    yield();
  }
}

// Create idle process for each CPU
void
idle_init(void)
{
  struct cpu *c = mycpu();
  struct proc *p;
  
  // Find an unused proc slot for idle
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    }
    release(&p->lock);
  }
  panic("idle_init: no free proc");

found:
  p->pid = 0;  // Idle processes have pid 0
  p->state = RUNNABLE;
  p->is_idle = 1;
  p->priority = PRIO_IDLE;
  p->time_slice = TIME_SLICE_IDLE;
  
  // Set up context to run idle_loop
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)idle_loop;
  p->context.sp = p->kstack + PGSIZE;
  safestrcpy(p->name, "idle", sizeof(p->name));
  
  c->idle = p;
  // when no other process is runnable
  
  release(&p->lock);
}

// Get scheduler statistics for user space
uint64
get_sched_stats(void)
{
  return context_switches;
}

// Get current process priority
int
get_proc_priority(void)
{
  struct proc *p = myproc();
  return p->priority;
}

// Set current process priority
int
set_proc_priority(int new_prio)
{
  struct proc *p = myproc();
  
  if(new_prio < PRIO_HIGH || new_prio > PRIO_LOW)
    return -1;
  
  acquire(&p->lock);
  p->priority = new_prio;
  p->time_slice = get_time_slice(new_prio);
  p->total_ticks = 0;
  release(&p->lock);
  
  return 0;
}

#endif // SCHED_DIRECT_MLFQ

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc(); // p->lock is HELD
  initproc = p;
  
  p->cwd = namei("/");

  release(&p->lock); // 释放 allocproc 持有的锁

  #if defined(SCHED_GLOBAL_MLFQ)
    acquire(&mlfq_lock); // 1. 获取全局锁
    acquire(&p->lock);   // 2. 获取进程锁
    p->state = RUNNABLE;
    list_push(&mlfq_queues[p->priority], p);
    release(&p->lock);
    release(&mlfq_lock);

  #elif defined(SCHED_PERCPU_MLFQ)
    // 放在启动CPU（CPU 0）的队列中
    struct cpu *c = &cpus[0];
    acquire(&c->mlfq_lock); // 1. 获取CPU队列锁
    acquire(&p->lock);      // 2. 获取进程锁
    p->state = RUNNABLE;
    list_push(&c->mlfq_queues[p->priority], p);
    release(&p->lock);
    release(&c->mlfq_lock);

  #elif defined(SCHED_PERCPU_PRIO)
    // 放在启动CPU（CPU 0）的队列中
    struct cpu *c = &cpus[0];
    acquire(&c->prio_lock); // 1. 获取CPU队列锁
    acquire(&p->lock);      // 2. 获取进程锁
    p->state = RUNNABLE;
    list_push(&c->prio_queues[p->priority], p);
    release(&p->lock);
    release(&c->prio_lock);

  #elif defined(SCHED_DIRECT_MLFQ)
    // Direct MLFQ: Add to ready queue
    acquire(&p->lock);
    p->state = RUNNABLE;
    acquire(&queue_lock);
    enqueue(p);
    release(&queue_lock);
    release(&p->lock);

  #else
    // 原始 xv6 或 全局静态优先级 逻辑
    acquire(&p->lock);
    p->state = RUNNABLE;
    release(&p->lock);
  #endif
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){ // np->lock is HELD
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // --- 调度器：初始化子进程的调度字段 ---
  #if defined(SCHED_GLOBAL_PRIO) || defined(SCHED_PERCPU_PRIO)
  np->priority = p->priority; // 子进程继承父进程的静态优先级
  #endif

  #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ)
  np->priority = MLFQ_DEFAULT_PRIORITY; // 新进程总是进入最高优先级队列
  np->ticks_in_queue = 0;
  acquire(&tickslock);
  np->creation_time = ticks;
  release(&tickslock);
  #endif

  #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ) || defined(SCHED_PERCPU_PRIO)
  np->next = 0;
  np->prev = 0;
  #endif
  // ---

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock); // 释放 allocproc 持有的锁

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  #if defined(SCHED_GLOBAL_MLFQ)
    acquire(&mlfq_lock); // 1. 获取全局锁
    acquire(&np->lock);  // 2. 获取进程锁
    np->state = RUNNABLE;
    list_push(&mlfq_queues[np->priority], np);
    release(&np->lock);
    release(&mlfq_lock);

  #elif defined(SCHED_PERCPU_MLFQ)
    // 放在当前CPU的队列中
    struct cpu *c = mycpu();
    acquire(&c->mlfq_lock); // 1. 获取CPU队列锁
    acquire(&np->lock);     // 2. 获取进程锁
    np->state = RUNNABLE;
    list_push(&c->mlfq_queues[np->priority], np);
    release(&np->lock);
    release(&c->mlfq_lock);

  #elif defined(SCHED_PERCPU_PRIO)
    // 放在当前CPU的队列中
    struct cpu *c = mycpu();
    acquire(&c->prio_lock); // 1. 获取CPU队列锁
    acquire(&np->lock);     // 2. 获取进程锁
    np->state = RUNNABLE;
    list_push(&c->prio_queues[np->priority], np);
    release(&np->lock);
    release(&c->prio_lock);

  #elif defined(SCHED_DIRECT_MLFQ)
    // Direct MLFQ: Add new process to ready queue at high priority
    acquire(&np->lock);
    np->state = RUNNABLE;
    acquire(&queue_lock);
    enqueue(np);
    release(&queue_lock);
    release(&np->lock);

  #else
    // 原始 xv6 或 全局静态优先级 逻辑
    acquire(&np->lock);
    np->state = RUNNABLE;
    release(&np->lock);
  #endif
  
  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// --- MLFQ 全局优先级提升 ---
#if defined(SCHED_GLOBAL_MLFQ)
void global_priority_boost() {
  struct proc *p;

  acquire(&mlfq_lock); // 1. 获取全局锁
  // 1. 清空所有队列
  for (int i = 0; i < NUM_MLFQ_QUEUES; i++) {
    list_init(&mlfq_queues[i]);
  }

  // 2. 遍历所有进程
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock); // 2. 获取进程锁
    // 进程饿死：将所有进程移到最高优先级队列
    if(p->state == SLEEPING || p->state == RUNNABLE) {
      p->priority = MLFQ_DEFAULT_PRIORITY;
      p->ticks_in_queue = 0;
    }
    
    // 3. 如果进程是 RUNNABLE，将其添加到新队列中
    if(p->state == RUNNABLE) {
      list_push(&mlfq_queues[p->priority], p);
    }
    release(&p->lock);
  }
  release(&mlfq_lock);
}
#endif

#if defined(SCHED_PERCPU_MLFQ)
// 每CPU MLFQ的全局提升
void global_priority_boost() {
  struct proc *p;

  // 1. 清空所有CPU队列
  // 必须按顺序获取所有锁以避免死锁
  for(struct cpu *c = cpus; c < &cpus[NCPU]; c++) {
    acquire(&c->mlfq_lock);
  }

  for(struct cpu *c = cpus; c < &cpus[NCPU]; c++) {
    for (int i = 0; i < NUM_MLFQ_QUEUES; i++) {
      list_init(&c->mlfq_queues[i]);
    }
  }

  // 2. 遍历所有进程
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == SLEEPING || p->state == RUNNABLE) {
      p->priority = MLFQ_DEFAULT_PRIORITY;
      p->ticks_in_queue = 0;
    }
    
    // 3. 如果是 RUNNABLE, 重新分配到 CPU 0 的队列中
    if(p->state == RUNNABLE) {
      list_push(&cpus[0].mlfq_queues[p->priority], p);
    }
    release(&p->lock);
  }

  // 4. 释放所有CPU锁
  for(struct cpu *c = cpus; c < &cpus[NCPU]; c++) {
    release(&c->mlfq_lock);
  }
}
#endif
// ---

// Per-CPU process scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;

  #ifdef SCHED_DIRECT_MLFQ
  // Direct MLFQ: Bootstrap scheduler - only runs once per CPU
  extern uint ticks;
  c->prev_proc = 0;
  
  // Create idle process for this CPU
  idle_init();
  
  // Keep trying to find a process to run
  for(;;) {
    // Enable interrupts to avoid deadlock
    intr_on();
    
    p = 0;
    
    // Find a runnable process
    acquire(&queue_lock);
    p = find_and_remove_runnable();
    release(&queue_lock);
    
    // If no runnable process, use idle
    if(p == 0) {
      p = c->idle;
    }
    
    // Try to acquire the process's lock
    acquire(&p->lock);
    
    // Double-check state after acquiring lock
    if(p->state == RUNNABLE || p->is_idle) {
      // Start this process - this is the ONLY time scheduler does swtch
      p->state = RUNNING;
      p->start_tick = ticks;
      c->proc = p;
      c->prev_proc = 0;  // No previous process on first switch
      context_switches++;

      // Switch to process - SCHEDULER NEVER RETURNS FROM HERE
      // All future scheduling happens via direct sched() calls
      swtch(&c->context, &p->context);

      // We should never get here after direct switching is implemented
      panic("scheduler returned");
    }
    
    release(&p->lock);
    // Try again
  }
  #else
  // Non-direct MLFQ schedulers
  for(;;){
    intr_on();
    intr_off();

    // =================================================================
    // --- 默认轮转调度 (SCHED_RR) ---
    #if !defined(SCHED_GLOBAL_PRIO) && !defined(SCHED_PERCPU_PRIO) && !defined(SCHED_GLOBAL_MLFQ) && !defined(SCHED_PERCPU_MLFQ)
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      asm volatile("wfi");
    }
    #endif
    // =================================================================


    // =================================================================
    // --- 全局静态优先级调度 (SCHED_GLOBAL_PRIO) ---
    #ifdef SCHED_GLOBAL_PRIO
    acquire(&scheduler_lock);

    struct proc *best_p = 0;
    int best_priority = MAX_PRIORITY + 1;

    // 1. 查找全局优先级最高的 RUNNABLE 进程
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state == RUNNABLE) {
        if(p->priority < best_priority) {
          best_priority = p->priority;
          best_p = p;
        }
      }
    }

    // 2. 如果找到，尝试运行它
    if(best_p) {
      acquire(&best_p->lock);
      
      if(best_p->state == RUNNABLE) {
        p = best_p;
        p->state = RUNNING;
        c->proc = p;
        
        release(&scheduler_lock); // 运行前释放全局锁
        
        swtch(&c->context, &p->context);
        c->proc = 0;
        
        release(&p->lock); // 运行后释放进程锁
      } else {
        release(&best_p->lock);
        release(&scheduler_lock);
      }
    } else {
      release(&scheduler_lock);
      asm volatile("wfi"); // 没有进程可运行
    }
    #endif
    // =================================================================


    // =================================================================
    // --- 每 CPU 静态优先级调度 (SCHED_PERCPU_PRIO) ---
    #ifdef SCHED_PERCPU_PRIO
    p = 0;
    // 1. 尝试从自己的队列中获取任务
    acquire(&c->prio_lock);
    for (int i = 0; i < NUM_PRIO_QUEUES; i++) { // 从最高优先级 0 开始
      if (!list_empty(&c->prio_queues[i])) {
        p = list_pop(&c->prio_queues[i]);
        break;
      }
    }
    release(&c->prio_lock);
    
    // 2. 如果自己的队列为空，尝试“工作窃取”
    if (p == 0) {
      for (int i = 1; i < NCPU; i++) {
        struct cpu *victim_c = &cpus[(cpuid() + i) % NCPU];
        
        if(holding(&victim_c->prio_lock))
            continue;
        acquire(&victim_c->prio_lock);
        
        // 从受害者的最高优先级队列中窃取
        for (int j = 0; j < NUM_PRIO_QUEUES; j++) {
          if (!list_empty(&victim_c->prio_queues[j])) {
            p = list_pop(&victim_c->prio_queues[j]);
            break;
          }
        }
        release(&victim_c->prio_lock);
        
        if (p) {
          break; // 窃取成功
        }
      }
    }
    
    // 3. 如果找到了进程（自己的或窃取的），运行它
    if(p) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;
      }
      release(&p->lock);
    } else {
      asm volatile("wfi"); // 没有工作可做
    }
    #endif
    // =================================================================
    
    
    // =================================================================
    // --- 全局 MLFQ 调度 (SCHED_GLOBAL_MLFQ) ---
    #ifdef SCHED_GLOBAL_MLFQ
    // 1. 检查是否需要全局优先级提升
    acquire(&tickslock);
    uint current_ticks = ticks;
    release(&tickslock);
    
    if (current_ticks - last_boost_time >= PRIORITY_BOOST_TICKS) {
      global_priority_boost(); // boost 内部会获取 mlfq_lock
      last_boost_time = current_ticks;
    }
    
    // 2. 查找要运行的进程
    p = 0;
    acquire(&mlfq_lock); // 获取全局锁
    
    // 从最高优先级队列 (0) 开始查找
    for (int i = 0; i < NUM_MLFQ_QUEUES; i++) {
      if (!list_empty(&mlfq_queues[i])) {
        p = list_pop(&mlfq_queues[i]);
        break;
      }
    }
    
    if (p) {
      acquire(&p->lock); // 获取进程锁
      release(&mlfq_lock); // 释放全局队列锁
      
      if (p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;
      }
      release(&p->lock);
      
    } else {
      release(&mlfq_lock);
      asm volatile("wfi");
    }
    #endif
    // =================================================================
    
    
    // =================================================================
    // --- 每 CPU MLFQ 调度 (SCHED_PERCPU_MLFQ) ---
    #ifdef SCHED_PERCPU_MLFQ
    // 1. 检查全局优先级提升
    acquire(&tickslock);
    uint current_ticks = ticks;
    release(&tickslock);
    
    // 仅 CPU 0 负责触发全局提升
    if (c == &cpus[0]) {
        acquire(&boost_lock);
        if(current_ticks - last_boost_time_percpu >= PRIORITY_BOOST_TICKS) {
            global_priority_boost();
            last_boost_time_percpu = current_ticks;
        }
        release(&boost_lock);
    }
    
    // 2. 尝试从自己的队列中获取任务
    p = 0;
    acquire(&c->mlfq_lock); // 获取本地 CPU 锁
    for (int i = 0; i < NUM_MLFQ_QUEUES; i++) {
      if (!list_empty(&c->mlfq_queues[i])) {
        p = list_pop(&c->mlfq_queues[i]);
        break;
      }
    }
    release(&c->mlfq_lock);
    
    // 3. 如果自己的队列为空，尝试“工作窃取”
    if (p == 0) {
      for (int i = 1; i < NCPU; i++) {
        struct cpu *victim_c = &cpus[(cpuid() + i) % NCPU];
        
        if(holding(&victim_c->mlfq_lock))
            continue;
        acquire(&victim_c->mlfq_lock); // 获取受害者 CPU 锁
        
        // 从受害者的最低优先级队列中窃取
        for (int j = MLFQ_MAX_PRIORITY; j >= 0; j--) {
          if (!list_empty(&victim_c->mlfq_queues[j])) {
            p = list_pop(&victim_c->mlfq_queues[j]);
            break;
          }
        }
        release(&victim_c->mlfq_lock);
        
        if (p) {
          break; // 窃取成功
        }
      }
    }
    
    // 4. 如果找到了进程（自己的或窃取的），运行它
    if(p) {
      acquire(&p->lock); // 获取进程锁
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        c->proc = 0;
      }
      release(&p->lock);
    } else {
      asm volatile("wfi"); // 没有工作可做
    }
    #endif
    // =================================================================
  }
  #endif // !SCHED_DIRECT_MLFQ
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;

  // 必须释放 p->lock，然后以正确的顺序（队列锁 -> 进程锁）
  // 重新获取锁，以避免与 scheduler() 死锁。

  #if defined(SCHED_GLOBAL_MLFQ)
  if(p->state == RUNNABLE) {
    release(&p->lock); // 释放锁
    
    acquire(&mlfq_lock);   // 1. 获取全局锁
    acquire(&p->lock);     // 2. 获取进程锁
    
    // 重新检查状态，因为锁已释放
    if(p->state == RUNNABLE) {
      list_push(&mlfq_queues[p->priority], p);
    }
    
    release(&mlfq_lock);
    // 保持 p->lock 持有状态进入 swtch
  }
  
  #elif defined(SCHED_PERCPU_MLFQ)
  if(p->state == RUNNABLE) {
    struct cpu *c = mycpu();
    release(&p->lock); // 释放锁
    
    acquire(&c->mlfq_lock); // 1. 获取CPU锁
    acquire(&p->lock);      // 2. 获取进程锁
    
    if(p->state == RUNNABLE) {
      list_push(&c->mlfq_queues[p->priority], p);
    }
    
    release(&c->mlfq_lock);
    // 保持 p->lock 持有状态进入 swtch
  }
  
  #elif defined(SCHED_PERCPU_PRIO)
  if(p->state == RUNNABLE) {
    struct cpu *c = mycpu();
    release(&p->lock); // 释放锁
    
    acquire(&c->prio_lock); // 1. 获取CPU锁
    acquire(&p->lock);      // 2. 获取进程锁
    
    if(p->state == RUNNABLE) {
      list_push(&c->prio_queues[p->priority], p);
    }
    
    release(&c->prio_lock);
    // 保持 p->lock 持有状态进入 swtch
  }

  #elif defined(SCHED_DIRECT_MLFQ)
  // Direct MLFQ: Direct process-to-process switching
  {
    struct proc *next;
    struct cpu *c = mycpu();
    extern uint ticks;

    acquire(&queue_lock);
    
    // Periodic priority boost to prevent starvation
    if(ticks - last_boost_tick >= BOOST_INTERVAL) {
      last_boost_tick = ticks;
      for(int prio = 1; prio < NPRIO - 1; prio++) {
        struct proc *curr = ready_queues[prio].head;
        while(curr) {
          struct proc *tmp = curr->queue_next;
          if(!curr->is_idle && curr->state == RUNNABLE) {
            dequeue(curr);
            curr->priority = PRIO_HIGH;
            curr->time_slice = TIME_SLICE_HIGH;
            enqueue(curr);
          }
          curr = tmp;
        }
      }
    }
    
    // IMPORTANT: Find next process BEFORE adding current to queue
    // This prevents selecting ourselves (which would cause double-lock)
    next = find_and_remove_runnable();
    
    // Now add current process back to queue if it's runnable
    if(p->state == RUNNABLE && !p->is_idle) {
      enqueue(p);
    }
    
    // If no other runnable process, use idle
    if(next == 0) {
      next = c->idle;
    }
    
    // Special case: if next is current process (e.g., idle switching to idle)
    // Just return without switching
    if(next == p) {
      release(&queue_lock);
      c->intena = intena;
      return;
    }
    
    release(&queue_lock);
    
    // Acquire the next process's lock
    acquire(&next->lock);
    
    // Set up for the switch
    next->state = RUNNING;
    next->start_tick = ticks;
    c->proc = next;
    c->prev_proc = p;  // Save current proc for lock release after switch
    context_switches++;
   
    // ========================================
    // DIRECT SWITCH: A -> B (one swtch call)
    // ========================================
    swtch(&p->context, &next->context);
    
    // ========================================
    // We're back! Another process switched to us.
    // Release the previous process's lock.
    // ========================================
    release_prev_proc();
    
    c->intena = intena;
    return;
  }

  #endif
  // --- 修复结束 ---

  #ifndef SCHED_DIRECT_MLFQ
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
  #endif
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);

  #ifdef SCHED_DIRECT_MLFQ
  // MLFQ: Track time slice usage
  extern uint ticks;
  uint64 used = ticks - p->start_tick;
  p->total_ticks += used;
  p->time_slice--;
  
  // If time slice exhausted, demote priority
  if(p->time_slice <= 0 && !p->is_idle) {
    demote_priority(p);
  }
  #endif

  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  #ifdef SCHED_DIRECT_MLFQ
  // In direct switching mode, we need to release the previous process's lock
  // if we were switched to directly from another process
  struct cpu *c = mycpu();
  if(c->prev_proc) {
    release(&c->prev_proc->lock);
    c->prev_proc = 0;
  }
  #endif

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  #if defined(SCHED_GLOBAL_MLFQ)
    acquire(&mlfq_lock); // 1. 全局锁
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p != myproc()){
        acquire(&p->lock); // 2. 进程锁
        if(p->state == SLEEPING && p->chan == chan) {
          
          p->priority = MLFQ_DEFAULT_PRIORITY; 
          p->ticks_in_queue = 0; 
          list_push(&mlfq_queues[p->priority], p);
          
          p->state = RUNNABLE;
        }
        release(&p->lock);
      }
    }
    release(&mlfq_lock);

  #elif defined(SCHED_PERCPU_MLFQ)
    struct cpu *c = mycpu();
    acquire(&c->mlfq_lock); // 1. CPU锁
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p != myproc()){
        acquire(&p->lock); // 2. 进程锁
        if(p->state == SLEEPING && p->chan == chan) {

          p->priority = MLFQ_DEFAULT_PRIORITY;
          p->ticks_in_queue = 0; 
          list_push(&c->mlfq_queues[p->priority], p);

          p->state = RUNNABLE;
        }
        release(&p->lock);
      }
    }
    release(&c->mlfq_lock);

  #elif defined(SCHED_PERCPU_PRIO)
    struct cpu *c = mycpu();
    acquire(&c->prio_lock); // 1. CPU锁
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p != myproc()){
        acquire(&p->lock); // 2. 进程锁
        if(p->state == SLEEPING && p->chan == chan) {
          p->state = RUNNABLE;
          list_push(&c->prio_queues[p->priority], p);
        }
        release(&p->lock);
      }
    }
    release(&c->prio_lock);

  #elif defined(SCHED_DIRECT_MLFQ)
    // Direct MLFQ: Waking processes get priority boost (I/O bound behavior)
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p != myproc()){
        acquire(&p->lock);
        if(p->state == SLEEPING && p->chan == chan) {
          p->state = RUNNABLE;
          // MLFQ: Waking processes get priority boost (I/O bound behavior)
          if(!p->is_idle && p->priority > PRIO_HIGH) {
            p->priority = PRIO_HIGH;
            p->time_slice = TIME_SLICE_HIGH;
          }
          // Add to ready queue
          acquire(&queue_lock);
          enqueue(p);
          release(&queue_lock);
        }
        release(&p->lock);
      }
    }

  #else
    // 原始 xv6 或 全局静态优先级 逻辑
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p != myproc()){
        acquire(&p->lock);
        if(p->state == SLEEPING && p->chan == chan) {
          p->state = RUNNABLE;
        }
        release(&p->lock);
      }
    }
  #endif
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  #if defined(SCHED_GLOBAL_MLFQ)
    acquire(&mlfq_lock); // 1. 全局锁
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock); // 2. 进程锁
      if(p->pid == pid){
        p->killed = 1;
        if(p->state == SLEEPING){
          
          p->priority = MLFQ_DEFAULT_PRIORITY;
          p->ticks_in_queue = 0;
          list_push(&mlfq_queues[p->priority], p);
          
          p->state = RUNNABLE;
        }
        release(&p->lock);
        release(&mlfq_lock);
        return 0;
      }
      release(&p->lock);
    }
    release(&mlfq_lock);

  #elif defined(SCHED_PERCPU_MLFQ)
    struct cpu *c = mycpu();
    acquire(&c->mlfq_lock); // 1. CPU锁
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock); // 2. 进程锁
      if(p->pid == pid){
        p->killed = 1;
        if(p->state == SLEEPING){

          p->priority = MLFQ_DEFAULT_PRIORITY;
          p->ticks_in_queue = 0;
          list_push(&c->mlfq_queues[p->priority], p);
          
          p->state = RUNNABLE;
        }
        release(&p->lock);
        release(&c->mlfq_lock);
        return 0;
      }
      release(&p->lock);
    }
    release(&c->mlfq_lock);

  #elif defined(SCHED_PERCPU_PRIO)
    struct cpu *c = mycpu();
    acquire(&c->prio_lock); // 1. CPU锁
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock); // 2. 进程锁
      if(p->pid == pid){
        p->killed = 1;
        if(p->state == SLEEPING){
          p->state = RUNNABLE;
          list_push(&c->prio_queues[p->priority], p);
        }
        release(&p->lock);
        release(&c->prio_lock);
        return 0;
      }
      release(&p->lock);
    }
    release(&c->prio_lock);

  #elif defined(SCHED_DIRECT_MLFQ)
    // Direct MLFQ kill
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->pid == pid){
        p->killed = 1;
        if(p->state == SLEEPING){
          p->state = RUNNABLE;
          // Add to ready queue
          acquire(&queue_lock);
          enqueue(p);
          release(&queue_lock);
        }
        release(&p->lock);
        return 0;
      }
      release(&p->lock);
    }

  #else
    // 原始 xv6 或 全局静态优先级 逻辑
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->pid == pid){
        p->killed = 1;
        if(p->state == SLEEPING){
          p->state = RUNNABLE;
        }
        release(&p->lock);
        return 0;
      }
      release(&p->lock);
    }
  #endif
  
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  #ifdef SCHED_DIRECT_MLFQ
  static char *prio_names[] = {
    "HIGH", "MED", "LOW", "IDLE"
  };
  #endif
  struct proc *p;
  char *state;

  #ifdef SCHED_DIRECT_MLFQ
  printf("\n=== Process Dump (MLFQ Scheduler) ===\n");
  printf("Context switches: %ld\n", context_switches);
  printf("PID\tSTATE\tPRIO\tNAME\n");
  #else
  printf("\n");
  #endif

  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    #ifdef SCHED_DIRECT_MLFQ
    printf("%d\t%s\t%s\t%s", p->pid, state, prio_names[p->priority], p->name);
    if(p->is_idle)
      printf(" [idle]");
    printf("\n");
    #else
    printf("%d %s %s", p->pid, state, p->name);

    // // 打印调度信息
    // #if defined(SCHED_GLOBAL_PRIO) || defined(SCHED_PERCPU_PRIO)
    // printf(" (priority %d)", p->priority);
    // #endif
    // #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ)
    // printf(" (mlfq_prio %d, ticks %d)", p->priority, p->ticks_in_queue);
    // #endif

    printf("\n");
    #endif
  }
  #ifdef SCHED_DIRECT_MLFQ
  printf("=====================================\n");
  #endif
}