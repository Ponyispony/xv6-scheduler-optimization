#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILD 8

void spin_loop(int prio, int idx) {
  printf("Child %d: PID %d (Priority %d) starting...\n", idx, getpid(), prio);
  volatile long long counter = 0;
  for (long long i = 0; i < 100000000; i++) {
    counter++;
    if (i % 25000000 == 0 && i != 0) {
      printf("Child %d: PID %d (Priority %d): tick %lld\n", idx, getpid(), prio, i/25000000);
    }
  }
  printf("Child %d: PID %d (Priority %d) finished.\n", idx, getpid(), prio);
}

int main(int argc, char *argv[])
{
  printf("Starting static priority test with %d children...\n", NCHILD);

  // int pids[NCHILD];
  int prios[NCHILD];

  // 设置父进程优先级为中等
  setpriority(DEFAULT_PRIORITY);

  // 分配优先级，均匀分布在 MIN_PRIORITY ~ MAX_PRIORITY
  for (int i = 0; i < NCHILD; i++) {
    prios[i] = MIN_PRIORITY + (MAX_PRIORITY - MIN_PRIORITY) * i / (NCHILD - 1);
  }

  // 创建子进程
  for (int i = 0; i < NCHILD; i++) {
    int pid = fork();
    if (pid == 0) {
      setpriority(prios[i]);
      spin_loop(prios[i], i);
      exit(0);
    } else if (pid > 0) {
      // pids[i] = pid;
    } else {
      printf("fork failed for child %d\n", i);
      exit(1);
    }
  }

  // 父进程等待所有子进程
  for (int i = 0; i < NCHILD; i++) {
    int wpid = wait(0);
    printf("Parent: child PID %d finished.\n", wpid);
  }

  printf("Static priority test finished.\n");
  exit(0);
}