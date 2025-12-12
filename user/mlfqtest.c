#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void cpu_bound(int idx) {
  printf("CPU-bound child %d (PID %d) starting...\n", idx, getpid());
  volatile double x = 9;
  for(long long i = 0; i < 500000000; i++) {
    x += 3.14;
    x *= 1.000001;
    x -= 2.71;
    if ((i % 1000000) == 0) x /= 1.0000001;
    if (i % 125000000 == 0 && i != 0) {
      printf("CPU child %d: tick %lld\n", idx, i/125000000);
    }
  }
  printf("CPU-bound child %d (PID %d) done.\n", idx, getpid());
}

void io_bound() {
  printf("I/O-bound child (PID %d) starting...\n", getpid());
  for(int i = 0; i < 10; i++) {
    printf("I/O child (PID %d): sleeping...\n", getpid());
    pause(1);
  }
  printf("I/O-bound child (PID %d) done.\n", getpid());
}

int main(int argc, char *argv[])
{
  printf("Starting MLFQ test...\n");

  // 创建第一个CPU密集型子进程
  int pid1 = fork();
  if (pid1 == 0) {
    cpu_bound(1);
    exit(0);
  } else if (pid1 < 0) {
    printf("fork failed for CPU child 1\n");
    exit(1);
  }

  // 创建I/O密集型子进程
  int pid2 = fork();
  if (pid2 == 0) {
    io_bound();
    exit(0);
  } else if (pid2 < 0) {
    printf("fork failed for IO child\n");
    exit(1);
  }

  // 创建第二个CPU密集型子进程
  int pid3 = fork();
  if (pid3 == 0) {
    cpu_bound(2);
    exit(0);
  } else if (pid3 < 0) {
    printf("fork failed for CPU child 2\n");
    exit(1);
  }

  // 父进程等待所有子进程
  for (int i = 0; i < 3; i++) {
    int wpid = wait(0);
    printf("Parent: child PID %d finished.\n", wpid);
  }

  printf("MLFQ test done.\n");
  exit(0);
}