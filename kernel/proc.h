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

// Ready queue structure for each priority level (Direct MLFQ)
#ifdef SCHED_DIRECT_MLFQ
struct ready_queue {
  struct proc *head;           // First process in queue
  struct proc *tail;           // Last process in queue
  int count;                   // Number of processes in queue
};
#endif

// MLFQ 队列的辅助结构
#if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ) || defined(SCHED_PERCPU_PRIO)
struct proc_list {
  struct proc *head;
  struct proc *tail;
};
void list_push(struct proc_list *list, struct proc *p);
struct proc *list_pop(struct proc_list *list);
int list_empty(struct proc_list *list);
struct proc *list_remove(struct proc_list *list, struct proc *p);
void list_init(struct proc_list *list);
#endif

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?

  #ifdef SCHED_DIRECT_MLFQ
  struct proc *idle;          // This CPU's idle process
  struct proc *prev_proc;     // Previous process (for direct switching - release its lock)
  #endif

  #ifdef SCHED_PERCPU_MLFQ
  // 每个CPU维护一套本地MLFQ
  struct proc_list mlfq_queues[NUM_MLFQ_QUEUES];
  struct spinlock mlfq_lock; // 保护此CPU的MLFQ队列
  #endif

  // --- CHED_PERCPU_PRIO 的每CPU队列 ---
  #ifdef SCHED_PERCPU_PRIO
  struct proc_list prio_queues[NUM_PRIO_QUEUES];
  struct spinlock prio_lock; // 保护此CPU的静态优先级队列
  #endif
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
struct trapframe {
  /* 0 */ uint64 kernel_satp;   // kernel page table
  /* 8 */ uint64 kernel_sp;     // top of process's kernel stack
  /* 16 */ uint64 kernel_trap;   // usertrap()
  /* 24 */ uint64 epc;           // saved user program counter
  /* 32 */ uint64 kernel_hartid; // saved kernel tp
  /* 40 */ uint64 ra;
  /* 48 */ uint64 sp;
  /* 56 */ uint64 gp;
  /* 64 */ uint64 tp;
  /* 72 */ uint64 t0;
  /* 80 */ uint64 t1;
  /* 88 */ uint64 t2;
  /* 96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  
  // --- 调度器字段 ---
  #if defined(SCHED_GLOBAL_PRIO) || defined(SCHED_PERCPU_PRIO)
  int priority; // 静态优先级
  #endif

  #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ)
  int priority;           // 当前MLFQ优先级
  int ticks_in_queue;     // 在当前队列中已运行的ticks
  uint creation_time;     // 进程创建时间 (用于MLFQ中打破平局)
  #endif

  #ifdef SCHED_DIRECT_MLFQ
  // Scheduler optimization: MLFQ fields
  int priority;                // Current priority level (0 = highest)
  int time_slice;              // Remaining time slice
  int total_ticks;             // Total ticks used at current priority
  uint64 start_tick;           // When the process started running
  int is_idle;                 // 1 if this is an idle process

  // Ready queue linkage (explicit queue instead of scanning proc table)
  struct proc *queue_next;     // Next process in ready queue
  struct proc *queue_prev;     // Previous process in ready queue
  #endif

  // --- 共享的链表指针 ---
  #if defined(SCHED_GLOBAL_MLFQ) || defined(SCHED_PERCPU_MLFQ) || defined(SCHED_PERCPU_PRIO)
  struct proc *next;      // 用于链表
  struct proc *prev;      // 用于链表
  #endif
};

#ifdef SCHED_DIRECT_MLFQ
extern struct ready_queue ready_queues[NPRIO];
extern struct spinlock queue_lock;
#endif