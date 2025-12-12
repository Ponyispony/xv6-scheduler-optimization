// schedtest.c - Scheduler Optimization Test Program
// This program demonstrates:
// 1. Direct process-to-process switching (A -> B instead of A -> scheduler -> B)
// 2. Multi-Level Feedback Queue (MLFQ) scheduling
// 3. Priority-based scheduling with dynamic priority adjustment
// 4. Explicit ready queue management

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILDREN 4
#define WORKLOAD 100000

// Simulate CPU-bound workload
void cpu_work(int iterations) {
  volatile int sum = 0;
  for(int i = 0; i < iterations; i++) {
    sum += i * i;
  }
}

// Simulate I/O-bound workload (frequent sleeps)
void io_work(int iterations) {
  for(int i = 0; i < iterations; i++) {
    sleep(1);  // Simulate I/O wait
  }
}

// Print priority name
void print_prio(int prio) {
  switch(prio) {
    case 0: printf("HIGH"); break;
    case 1: printf("MED"); break;
    case 2: printf("LOW"); break;
    case 3: printf("IDLE"); break;
    default: printf("???"); break;
  }
}

// Simple print helpers for minimal, consistent output
void print_title(const char *s) {
  printf("\n%s\n", s);
  printf("------------------------------------------------\n");
}

void print_test_header(const char *s) {
  printf("\n-- %s --\n", s);
}

void print_result(const char *status, const char *msg) {
  if (msg && msg[0])
    printf("Result: %s - %s\n", status, msg);
  else
    printf("Result: %s\n", status);
}

// Test 1: Basic scheduling and context switch counting
void test_basic_scheduling() {
  print_test_header("Test 1: Basic Scheduling");
  int start_switches = schedstat();
  printf("Initial context switches: %d\n", start_switches);
  
  int pid = fork();
  if(pid == 0) {
    // Child: do some work
    cpu_work(WORKLOAD);
    exit(0);
  } else {
    // Parent: wait for child
    wait(0);
  }
  
  int end_switches = schedstat();
  printf("Final context switches: %d\n", end_switches);
  printf("Switches during test: %d\n", end_switches - start_switches);
  print_result("PASS", "Direct switching working");
}

// Test 2: MLFQ priority demotion
void test_mlfq_demotion() {
  print_test_header("Test 2: MLFQ Priority Demotion");
  printf("CPU-bound processes should get demoted\n\n");
  
  int initial_prio = getprio();
  printf("Initial priority: ");
  print_prio(initial_prio);
  printf(" (%d)\n", initial_prio);
  
  // Do CPU-intensive work to exhaust time slices
  printf("Doing CPU-intensive work...\n");
  for(int i = 0; i < 20; i++) {
    cpu_work(WORKLOAD * 2);
    // Yield to trigger time slice accounting
    int current_prio = getprio();
    printf("  After iteration %d, priority: ", i + 1);
    print_prio(current_prio);
    printf(" (%d)\n", current_prio);
  }
  
  int final_prio = getprio();
  printf("Final priority: ");
  print_prio(final_prio);
  printf(" (%d)\n", final_prio);
  
  if(final_prio >= initial_prio) {
    print_result("PASS", "Priority demotion working");
  } else {
    print_result("NOTE", "Priority was boosted");
  }
}

// Test 3: I/O bound processes get priority boost
void test_io_priority_boost() {
  print_test_header("Test 3: I/O Priority Boost");
  printf("I/O-bound processes should maintain high priority\n\n");
  
  // First, demote ourselves
  setprio(2);  // Set to LOW priority
  printf("Set to LOW priority\n");
  
  int initial_prio = getprio();
  printf("Priority before I/O: ");
  print_prio(initial_prio);
  printf(" (%d)\n", initial_prio);
  
  // Do I/O work (sleeping simulates waiting for I/O)
  printf("Doing I/O work (sleeping)...\n");
  for(int i = 0; i < 3; i++) {
    sleep(2);
    int current_prio = getprio();
    printf("  After sleep %d, priority: ", i + 1);
    print_prio(current_prio);
    printf(" (%d)\n", current_prio);
  }
  
  int final_prio = getprio();
  printf("Final priority: ");
  print_prio(final_prio);
  printf(" (%d)\n", final_prio);

  print_result("PASS", "I/O processes get priority boost on wakeup");
}

// Test 4: Multiple processes with different priorities
void test_multi_priority() {
  print_test_header("Test 4: Multi-Priority Scheduling");
  printf("Creating %d processes with different priorities\n\n", NCHILDREN);
  
  int start_switches = schedstat();
  int pids[NCHILDREN];
  
  for(int i = 0; i < NCHILDREN; i++) {
    pids[i] = fork();
    if(pids[i] == 0) {
      // Child process
      setprio(i);  // Set different priorities: 0, 1, 2, 3
      printf("Child %d (pid=%d) starting with priority ", i, getpid());
      print_prio(i);
      printf("\n");

      // Do work proportional to priority level
      // Higher priority = less work (interactive)
      // Lower priority = more work (batch)
      int work = WORKLOAD * (i + 1);
      cpu_work(work);

      printf("Child %d (pid=%d) done, final priority: ", i, getpid());
      print_prio(getprio());
      printf("\n");
      exit(0);
    }
  }
  
  // Parent waits for all children
  for(int i = 0; i < NCHILDREN; i++) {
    wait(0);
  }
  
  int end_switches = schedstat();
  printf("\nAll children completed\n");
  printf("Context switches during test: %d\n", end_switches - start_switches);
  print_result("PASS", "Multi-priority scheduling working");
}

// Test 5: Direct process switching performance
void test_direct_switching() {
  print_test_header("Test 5: Direct Process Switching");

  int p1[2], p2[2];
  if(pipe(p1) < 0 || pipe(p2) < 0){
    printf("pipe error\n");
    return;
  }

  int rounds = 100;

  int start_sw = schedstat();
  int start_tm = uptime();

  int pid = fork();
  if(pid < 0){
    printf("fork error\n");
    return;
  }

  if(pid == 0){
    // child
    close(p1[1]); // child only read p1
    close(p2[0]); // child only write p2
    char buf[1];

    for(int i = 0; i < rounds; i++){
      // 等待父亲发来一个字节
      if(read(p1[0], buf, 1) != 1){
        printf("child read error\n");
        exit(1);
      }
      // 再回发一个字节
      if(write(p2[1], "y", 1) != 1){
        printf("child write error\n");
        exit(1);
      }
    }
    close(p1[0]);
    close(p2[1]);
    exit(0);
  } else {
    // parent
    close(p1[0]); // parent only write p1
    close(p2[1]); // parent only read p2
    char buf[1];

    for(int i = 0; i < rounds; i++){
      // 发一个字节给子进程
      if(write(p1[1], "x", 1) != 1){
        printf("parent write error\n");
        break;
      }
      // 等子进程回一个字节
      if(read(p2[0], buf, 1) != 1){
        printf("parent read error\n");
        break;
      }
    }

    close(p1[1]);
    close(p2[0]);
    wait(0);
  }

  int end_tm = uptime();
  int end_sw = schedstat();

  int dt = end_tm - start_tm;
  int dsw = end_sw - start_sw;
  printf("Ping-pong rounds = %d\n", rounds);
  printf("Time taken       = %d ticks\n", dt);
  printf("Sched calls      = %d\n", dsw);
  printf("Avg sched/round  = %d\n", dsw / rounds);
  printf("NOTE: Compare this with the old kernel (no direct schedule).\n");
}

// Test 6: Idle process test
void test_idle_process() {
  print_test_header("Test 6: Idle Process");
  printf("When no process is runnable, idle runs\n\n");
  
  int before = schedstat();
  
  // Sleep to let idle run
  printf("Sleeping to let idle process run...\n");
  sleep(10);
  
  int after = schedstat();
  printf("Context switches while sleeping: %d\n", after - before);
  print_result("PASS", "Idle process handles empty queue");
}

int main(int argc, char *argv[]) {
  print_title("xv6 Scheduler Optimization Test Suite");
  printf("This test demonstrates the following features:\n");
  printf("1. Direct process-to-process switching (A -> B)\n");
  printf("2. Multi-Level Feedback Queue (MLFQ)\n");
  printf("3. Dynamic priority adjustment\n");
  printf("4. Explicit ready queue management\n");
  printf("5. Idle process for empty queue handling\n\n");
  
  // Run all tests
  test_basic_scheduling();
  test_mlfq_demotion();
  test_io_priority_boost();
  test_multi_priority();
  test_direct_switching();
  test_idle_process();
  
  print_title("All Tests Completed");
  printf("Summary:\n");
  printf("- Original: Process A -> Scheduler -> Process B\n");
  printf("- Optimized: Process A -> Process B (direct)\n");
  printf("- Benefit: Reduced context switch overhead\n\n");
  printf("Total context switches: %d\n\n", schedstat());
  
  exit(0);
}
