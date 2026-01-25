
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <execinfo.h>
#include <fcntl.h>


#define MAX_FUNCTIONS 1000
#define MAX_CALL_STACK 100
#define PROFILING_INTERVAL 10000 //sample interval 10ms


typedef unsigned long long time_stamp;

typedef struct {
    char name[256];
    time_stamp total_time;      // Wall clock time (microseconds)
    time_stamp self_time;       // Sampling time (microseconds)
    time_stamp user_time;       // User mode CPU time (microseconds)
    time_stamp sys_time;        // Kernel mode CPU time (microseconds)
    time_stamp wait_time;       // Wait time = wall - (user + sys) (microseconds)
    time_stamp call_count;
    int is_active;
    struct timespec start_wall_time; // Wall clock time at function entry (nanosecond precision)
    struct rusage start_rusage;      // Resource usage at function entry
} function_info_t;


static function_info_t functions[MAX_FUNCTIONS];
static time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
static struct itimerval  timer;
static int function_count=0;
static int profiling_enabled=0;

typedef struct {
    int stack[MAX_CALL_STACK];
    int top;
} call_stack_t;

static call_stack_t call_stack={.top=-1};


void profiling_handler(int sig){
    if (!profiling_enabled) return;

    static int init=0;
    static struct timeval last_sample;
    struct timeval current_time;

    gettimeofday(&current_time,NULL);

    if (!init){
        last_sample=current_time;
        init=1;
        return;
    }

    //count time interval
    long interval_us=(current_time.tv_sec-last_sample.tv_sec)*1000000 +
    (current_time.tv_usec-last_sample.tv_usec);

    if (call_stack.top>=0){
        // 純取樣自有時間：只累計堆疊頂端函式的 self_time
        int current_func = call_stack.stack[call_stack.top];
        functions[current_func].self_time += interval_us;
    }

    last_sample=current_time;
}

void init_profilier(){
    //set timer
    timer.it_interval.tv_sec=0;
    timer.it_interval.tv_usec=PROFILING_INTERVAL;
    timer.it_value=timer.it_interval;

    //set signal handler
    struct sigaction sa;
    sa.sa_handler=profiling_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=0;
    sigaction(SIGPROF,&sa,NULL);
    
}


void start_profiling() {
    profiling_enabled=1;
    setitimer(ITIMER_PROF,&timer,NULL);
}

void stop_profiling() {
    profiling_enabled=0;
    timer.it_value.tv_sec=0;
    timer.it_value.tv_usec=0;
    setitimer(ITIMER_PROF,&timer,NULL);

}

int register_function(const char *name) {
    if (function_count >= MAX_FUNCTIONS) {
        return -1;
    }

    strncpy(functions[function_count].name,name,255);
    functions[function_count].total_time=0;
    functions[function_count].self_time = 0;
    functions[function_count].user_time = 0;
    functions[function_count].sys_time = 0;
    functions[function_count].wait_time = 0;
    functions[function_count].call_count = 0;
    functions[function_count].is_active = 0;

    return function_count++;

}

void enter_function(int func_id) {
    if (func_id < 0 || func_id>=function_count) return;

    functions[func_id].call_count++;
    functions[func_id].is_active=1;

    // Record wall clock time with nanosecond precision (Phase 2)
    clock_gettime(CLOCK_MONOTONIC, &functions[func_id].start_wall_time);

    // Record resource usage at function entry (for user/system time tracking)
    // Note: Using RUSAGE_SELF for now (single-threaded). Will switch to RUSAGE_THREAD in Phase 3.
    getrusage(RUSAGE_SELF, &functions[func_id].start_rusage);

    if (call_stack.top >= 0) {
        int caller_id = call_stack.stack[call_stack.top];
        if (caller_id >= 0 && caller_id < function_count) {
            caller_counts[caller_id][func_id]++;
        }
    }

    if (call_stack.top < MAX_CALL_STACK-1){
        call_stack.stack[++call_stack.top]=func_id;
    }
}

void leave_function(int func_id) {
    if (func_id < 0 || func_id>=function_count) return;

    struct timespec end_wall_time;
    struct rusage end_rusage;

    // Get end timestamps
    clock_gettime(CLOCK_MONOTONIC, &end_wall_time);
    getrusage(RUSAGE_SELF, &end_rusage);

    // Calculate wall time delta (convert timespec to microseconds)
    long long wall_delta = (end_wall_time.tv_sec - functions[func_id].start_wall_time.tv_sec) * 1000000LL +
                           (end_wall_time.tv_nsec - functions[func_id].start_wall_time.tv_nsec) / 1000;

    // Calculate user time delta (user mode CPU time)
    long long user_delta = (end_rusage.ru_utime.tv_sec - functions[func_id].start_rusage.ru_utime.tv_sec) * 1000000LL +
                           (end_rusage.ru_utime.tv_usec - functions[func_id].start_rusage.ru_utime.tv_usec);

    // Calculate system time delta (kernel mode CPU time)
    long long sys_delta = (end_rusage.ru_stime.tv_sec - functions[func_id].start_rusage.ru_stime.tv_sec) * 1000000LL +
                          (end_rusage.ru_stime.tv_usec - functions[func_id].start_rusage.ru_stime.tv_usec);

    // Calculate wait time: Wall time - (User time + System time)
    // This represents I/O wait, sleep, lock contention, etc.
    long long wait_delta = wall_delta - (user_delta + sys_delta);

    // Handle potential negative values due to measurement precision
    if (wait_delta < 0) wait_delta = 0;

    // Accumulate all times
    functions[func_id].total_time += wall_delta;
    functions[func_id].user_time += user_delta;
    functions[func_id].sys_time += sys_delta;
    functions[func_id].wait_time += wait_delta;

    functions[func_id].is_active=0;

    if (call_stack.top >=0 && call_stack.stack[call_stack.top]==func_id){
        call_stack.top--;
    }
}


void print_profiling_results() {
    printf("\n=== Profiling Results (Phase 2: with Wait Time) ===\n");
    printf("%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Total(ms)", "Self(ms)", "User(s)", "Sys(s)", "Wait(s)", "Self%", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    time_stamp total_self_time=0;

    for (int i=0;i<function_count;i++){
        total_self_time+=functions[i].self_time;
    }

    for (int i=0;i<function_count;i++){
        if (functions[i].call_count > 0){
            double total_ms=functions[i].total_time/1000.0;
            double self_ms=functions[i].self_time/1000.0;
            double user_s=functions[i].user_time/1000000.0;   // Convert microseconds to seconds
            double sys_s=functions[i].sys_time/1000000.0;     // Convert microseconds to seconds
            double wait_s=functions[i].wait_time/1000000.0;   // Convert microseconds to seconds
            double self_percent=(total_self_time > 0) ?
            (functions[i].self_time * 100.0 / total_self_time) : 0;
            double avg_total = (functions[i].call_count > 0) ? (total_ms / (double)functions[i].call_count) : 0.0;

            printf("%-30s %10llu %10.2f %10.2f %10.4f %10.4f %10.4f %9.2f%% %10.3f\n",
                functions[i].name,
                functions[i].call_count,
                total_ms,
                self_ms,
                user_s,
                sys_s,
                wait_s,
                self_percent,
                avg_total);
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    // Show caller counts for each function
    printf("\n--- Callers (counts) ---\n");
    for (int callee = 0; callee < function_count; callee++) {
        if (functions[callee].call_count == 0) continue;
        int has_caller = 0;
        printf("%-30s <- ", functions[callee].name);
        for (int caller = 0; caller < function_count; caller++) {
            if (caller_counts[caller][callee] > 0) {
                has_caller = 1;
                printf("%s(%llu) ", functions[caller].name, caller_counts[caller][callee]);
            }
        }
        if (!has_caller) {
            printf("[none]");
        }
        printf("\n");
    }
}

static inline void __profile_cleanup_int(void *p) {
    int id = *(int *)p;
    leave_function(id);
}

#ifdef AUTO_PROFILE
#define PROFILE_FUNCTION()                                                     \
    static int __func_id = -1;                                                 \
    if (__func_id == -1) {                                                     \
        __func_id = register_function(__func__);                               \
    }                                                                          \
    enter_function(__func_id);                                                 \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard = __func_id;

#define PROFILE_SCOPE(name)                                                    \
    static int __scope_id_##__LINE__ = -1;                                     \
    if (__scope_id_##__LINE__ == -1) {                                         \
        __scope_id_##__LINE__ = register_function(name);                       \
    }                                                                          \
    enter_function(__scope_id_##__LINE__);                                     \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard_##__LINE__ = __scope_id_##__LINE__;

#else
#define PROFILE_FUNCTION()
#define PROFILE_SCOPE(name)
#endif


void function_a() {
    PROFILE_FUNCTION();
    for (volatile int  i=0;i<1000000;i++);
}

void function_b() {
    PROFILE_FUNCTION();
    for (volatile int  i=0;i<500000;i++);
    function_a();
}

void function_c() {
    PROFILE_FUNCTION();


    for(volatile int i=0;i<2000000;i++);
    function_b();
}

// I/O intensive function - should show high system time
void function_io_heavy() {
    PROFILE_FUNCTION();

    // Write to actual file with sync (forces real kernel I/O)
    int fd = open("test_io.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
    if (fd < 0) return;

    char buffer[4096];
    memset(buffer, 'A', sizeof(buffer));

    // Perform many write system calls with O_SYNC (forces kernel mode time)
    for (int i = 0; i < 1000; i++) {
        write(fd, buffer, sizeof(buffer));
    }

    fsync(fd);  // Force flush to disk
    close(fd);
    unlink("test_io.tmp");  // Clean up
}

// System call intensive function - many small syscalls
void function_syscall_heavy() {
    PROFILE_FUNCTION();

    // Lots of system calls: getpid is cheap but still goes to kernel
    for (int i = 0; i < 100000; i++) {
        getpid();  // Each call triggers kernel mode switch
    }
}

// CPU intensive function - should show high user time
void function_cpu_heavy() {
    PROFILE_FUNCTION();

    volatile double result = 0.0;
    // Pure CPU computation (user mode only)
    for (int i = 0; i < 2000000; i++) {
        result += i * 3.14159;
        result /= (i + 1.0);
    }
}

// Sleep test - should show high wait time, low user/sys time
void function_sleep_test() {
    PROFILE_FUNCTION();

    // Sleep for 100ms - this should appear as wait time
    struct timespec sleep_time = {0, 100000000}; // 0 sec, 100ms in nanoseconds
    nanosleep(&sleep_time, NULL);
}

// Mixed workload: CPU + I/O + Sleep
void function_mixed() {
    PROFILE_FUNCTION();

    // Some CPU work
    volatile int sum = 0;
    for (int i = 0; i < 100000; i++) {
        sum += i;
    }

    // Some I/O
    int fd = open("test_mixed.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[256] = "test";
        write(fd, buf, sizeof(buf));
        close(fd);
        unlink("test_mixed.tmp");
    }

    // Some sleep (wait time)
    struct timespec ts = {0, 50000000}; // 50ms
    nanosleep(&ts, NULL);
}

int main(){


    init_profilier();
    
#ifndef AUTO_PROFILE
    register_function("main");
    register_function("function_a");
    register_function("function_b");
    register_function("function_c");
#endif

    printf("Start  profiling...\n");
    printf("Testing User/System time separation...\n");
    start_profiling();

    // Original CPU-intensive tests
    for (int i=0;i<3;i++){
        PROFILE_SCOPE("main_loop");
        function_a();
        function_b();
        function_c();

        for (volatile int j=0;j<1000000;j++);
    }

    // Phase 1 tests: User vs System time
    printf("Running CPU-heavy test...\n");
    function_cpu_heavy();

    printf("Running I/O-heavy test (real file with O_SYNC)...\n");
    function_io_heavy();

    printf("Running syscall-heavy test (100k getpid calls)...\n");
    function_syscall_heavy();

    // Phase 2 tests: Wait time measurement
    printf("Running sleep test (100ms sleep - should show wait time)...\n");
    function_sleep_test();

    printf("Running mixed workload test (CPU + I/O + Sleep)...\n");
    function_mixed();

    stop_profiling();
    

    print_profiling_results();

    printf("profiling  stopped.\n");

    return 0;
}