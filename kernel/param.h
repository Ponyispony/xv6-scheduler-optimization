// --- 调度器选择 ---
// 取消注释（uncomment）以下五行中的一个来选择调度策略。
// 如果全部注释，将使用默认的xv6轮转调度。
// #define SCHED_GLOBAL_PRIO
// #define SCHED_PERCPU_PRIO
// #define SCHED_GLOBAL_MLFQ
// #define SCHED_PERCPU_MLFQ
#define SCHED_DIRECT_MLFQ    // 直接切换优化的MLFQ调度器

// --- 直接切换MLFQ调度常量 ---
#ifdef SCHED_DIRECT_MLFQ
#define NPRIO       4           // Number of priority levels
#define PRIO_HIGH   0           // Highest priority (interactive)
#define PRIO_MED    1           // Medium priority
#define PRIO_LOW    2           // Low priority
#define PRIO_IDLE   3           // Idle priority (background)

#define TIME_SLICE_HIGH  1      // Time slices for each priority
#define TIME_SLICE_MED   2
#define TIME_SLICE_LOW   4
#define TIME_SLICE_IDLE  8

#define BOOST_INTERVAL  100     // Ticks before priority boost
#endif

// --- 静态优先级调度常量 ---
#define MIN_PRIORITY 0  // 最小（高）优先级
#define MAX_PRIORITY 20 // 最大（低）优先级
#define DEFAULT_PRIORITY 10 //p默认静态优先级
#define NUM_PRIO_QUEUES (MAX_PRIORITY + 1) // 静态优先级队列数

// --- MLFQ 调度常量 ---
#define NUM_MLFQ_QUEUES 5 // 队列级别数 (0-4)
#define MLFQ_MAX_PRIORITY (NUM_MLFQ_QUEUES - 1) // 4 (最低优先级)
#define MLFQ_DEFAULT_PRIORITY 0 // 新进程的默认（最高）优先级
#define PRIORITY_BOOST_TICKS 100 // 全局优先级提升的周期（ticks）

// MLFQ 时间片长度 (in ticks)
// 高优先级队列时间片短，低优先级队列时间片长
#define TIME_SLICE_Q0 2
#define TIME_SLICE_Q1 4
#define TIME_SLICE_Q2 8
#define TIME_SLICE_Q3 16
#define TIME_SLICE_Q4 32

#ifdef LAB_FS
#define NPROC        10  // maximum number of processes
#else
#define NPROC        64  // maximum number of processes (speedsup bigfile)
#endif
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#ifdef LAB_FS
#define FSSIZE       200000  // size of file system in blocks
#else
#ifdef LAB_LOCK
#define FSSIZE       10000  // size of file system in blocks
#else
#define FSSIZE       2000   // size of file system in blocks
#endif
#endif
#define MAXPATH      128   // maximum file path name

#ifdef LAB_UTIL
#define USERSTACK    2     // user stack pages
#else
#define USERSTACK    1     // user stack pages
#endif


