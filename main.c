
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
#include <pthread.h>
#include <sys/syscall.h>  // For syscall(SYS_gettid)


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
    pid_t thread_id;            // Thread ID (Phase 3)
    struct timespec start_wall_time; // Wall clock time at function entry (nanosecond precision)
    struct rusage start_rusage;      // Resource usage at function entry
} function_info_t;

// Global function name registry (shared across all threads)
// Protected by mutex for thread-safe registration
typedef struct {
    char name[256];
    int id;
} function_registry_entry_t;

#define MAX_GLOBAL_FUNCTIONS 1000
static function_registry_entry_t global_function_registry[MAX_GLOBAL_FUNCTIONS];
static int global_function_count = 0;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
// Phase 4: Thread Data Collection
// ============================================================

// Thread profiling data snapshot (copy of TLS data)
typedef struct {
    pid_t thread_id;
    int function_count;
    function_info_t functions[MAX_FUNCTIONS];
    time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
} thread_data_snapshot_t;

#define MAX_THREADS 64
static thread_data_snapshot_t* thread_snapshots[MAX_THREADS];
static int thread_snapshot_count = 0;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;


// Thread-local storage (Phase 3): Each thread has its own profiling data
__thread function_info_t functions[MAX_FUNCTIONS];
__thread time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
__thread int function_count = 0;
__thread pid_t current_thread_id = 0;  // Cached thread ID

// Global profiling control (shared across threads)
static struct itimerval timer;
static int profiling_enabled = 0;

typedef struct {
    int stack[MAX_CALL_STACK];
    int top;
} call_stack_t;

// Thread-local call stack
__thread call_stack_t call_stack = {.top = -1};


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

// Thread-safe function registration (Phase 3)
// Step 1: Register in global registry (mutex-protected)
// Step 2: Create thread-local entry
int register_function(const char *name) {
    if (function_count >= MAX_FUNCTIONS) {
        return -1;
    }

    // Initialize thread ID on first registration
    if (current_thread_id == 0) {
        current_thread_id = syscall(SYS_gettid);
    }

    // Thread-safe: Lock global registry to find or create function ID
    pthread_mutex_lock(&registry_mutex);

    int global_id = -1;
    // Check if function already registered globally
    for (int i = 0; i < global_function_count; i++) {
        if (strcmp(global_function_registry[i].name, name) == 0) {
            global_id = global_function_registry[i].id;
            break;
        }
    }

    // If not found, create new global entry
    if (global_id == -1) {
        if (global_function_count >= MAX_GLOBAL_FUNCTIONS) {
            pthread_mutex_unlock(&registry_mutex);
            return -1;
        }
        global_id = global_function_count;
        strncpy(global_function_registry[global_function_count].name, name, 255);
        global_function_registry[global_function_count].id = global_id;
        global_function_count++;
    }

    pthread_mutex_unlock(&registry_mutex);

    // Create thread-local entry
    int local_id = function_count;
    strncpy(functions[local_id].name, name, 255);
    functions[local_id].total_time = 0;
    functions[local_id].self_time = 0;
    functions[local_id].user_time = 0;
    functions[local_id].sys_time = 0;
    functions[local_id].wait_time = 0;
    functions[local_id].call_count = 0;
    functions[local_id].is_active = 0;
    functions[local_id].thread_id = current_thread_id;

    function_count++;
    return local_id;
}

void enter_function(int func_id) {
    if (func_id < 0 || func_id >= function_count) return;

    functions[func_id].call_count++;
    functions[func_id].is_active = 1;

    // Record wall clock time with nanosecond precision (Phase 2)
    clock_gettime(CLOCK_MONOTONIC, &functions[func_id].start_wall_time);

    // Phase 3: Use RUSAGE_THREAD for per-thread CPU time tracking
    // RUSAGE_THREAD requires _GNU_SOURCE and Linux 2.6.26+
    #ifdef __linux__
        getrusage(RUSAGE_THREAD, &functions[func_id].start_rusage);
    #else
        getrusage(RUSAGE_SELF, &functions[func_id].start_rusage);
    #endif

    if (call_stack.top >= 0) {
        int caller_id = call_stack.stack[call_stack.top];
        if (caller_id >= 0 && caller_id < function_count) {
            caller_counts[caller_id][func_id]++;
        }
    }

    if (call_stack.top < MAX_CALL_STACK - 1) {
        call_stack.stack[++call_stack.top] = func_id;
    }
}

void leave_function(int func_id) {
    if (func_id < 0 || func_id >= function_count) return;

    struct timespec end_wall_time;
    struct rusage end_rusage;

    // Get end timestamps
    clock_gettime(CLOCK_MONOTONIC, &end_wall_time);

    // Phase 3: Use RUSAGE_THREAD for per-thread CPU time
    #ifdef __linux__
        getrusage(RUSAGE_THREAD, &end_rusage);
    #else
        getrusage(RUSAGE_SELF, &end_rusage);
    #endif

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


// Phase 4: Register current thread's profiling data to global collection
void register_thread_data() {
    pid_t tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);

    // Skip if no profiling data
    if (function_count == 0) {
        return;
    }

    pthread_mutex_lock(&snapshot_mutex);

    if (thread_snapshot_count >= MAX_THREADS) {
        pthread_mutex_unlock(&snapshot_mutex);
        fprintf(stderr, "Warning: Max threads exceeded, cannot register thread data\n");
        return;
    }

    // Allocate snapshot
    thread_data_snapshot_t* snapshot = malloc(sizeof(thread_data_snapshot_t));
    if (!snapshot) {
        pthread_mutex_unlock(&snapshot_mutex);
        fprintf(stderr, "Error: Failed to allocate thread snapshot\n");
        return;
    }

    // Copy TLS data to snapshot
    snapshot->thread_id = tid;
    snapshot->function_count = function_count;
    memcpy(snapshot->functions, functions, sizeof(functions));
    memcpy(snapshot->caller_counts, caller_counts, sizeof(caller_counts));

    // Add to global collection
    thread_snapshots[thread_snapshot_count++] = snapshot;

    pthread_mutex_unlock(&snapshot_mutex);
}

void print_profiling_results() {
    // Get current thread ID for display
    pid_t tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);

    printf("\n=== Profiling Results (Phase 4: Thread %d) ===\n", tid);
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
// Phase 4 fix: Use __thread for function ID to support multi-threading
#define PROFILE_FUNCTION()                                                     \
    static __thread int __func_id = -1;                                        \
    if (__func_id == -1) {                                                     \
        __func_id = register_function(__func__);                               \
    }                                                                          \
    enter_function(__func_id);                                                 \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard = __func_id;

#define PROFILE_SCOPE(name)                                                    \
    static __thread int __scope_id_##__LINE__ = -1;                            \
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

// ============================================================
// Phase 4: Report Generation Functions
// ============================================================

// Print report for a single thread snapshot
void print_thread_report(thread_data_snapshot_t* snapshot) {
    printf("\n=== Thread %d Report ===\n", snapshot->thread_id);
    printf("%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Total(ms)", "Self(ms)", "User(s)", "Sys(s)", "Wait(s)", "Self%", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    time_stamp total_self_time = 0;
    for (int i = 0; i < snapshot->function_count; i++) {
        total_self_time += snapshot->functions[i].self_time;
    }

    for (int i = 0; i < snapshot->function_count; i++) {
        if (snapshot->functions[i].call_count > 0) {
            double total_ms = snapshot->functions[i].total_time / 1000.0;
            double self_ms = snapshot->functions[i].self_time / 1000.0;
            double user_s = snapshot->functions[i].user_time / 1000000.0;
            double sys_s = snapshot->functions[i].sys_time / 1000000.0;
            double wait_s = snapshot->functions[i].wait_time / 1000000.0;
            double self_percent = (total_self_time > 0) ?
                (snapshot->functions[i].self_time * 100.0 / total_self_time) : 0;
            double avg_total = (snapshot->functions[i].call_count > 0) ?
                (total_ms / (double)snapshot->functions[i].call_count) : 0.0;

            printf("%-30s %10llu %10.2f %10.2f %10.4f %10.4f %10.4f %9.2f%% %10.3f\n",
                snapshot->functions[i].name,
                snapshot->functions[i].call_count,
                total_ms, self_ms, user_s, sys_s, wait_s,
                self_percent, avg_total);
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");
}

// Print per-thread reports
void print_per_thread_reports() {
    printf("\n");
    printf("================================================================================\n");
    printf("Per-Thread Profiling Reports (Phase 4)\n");
    printf("================================================================================\n");
    printf("Total threads: %d\n", thread_snapshot_count);

    for (int i = 0; i < thread_snapshot_count; i++) {
        print_thread_report(thread_snapshots[i]);
    }
}

// Print merged report (aggregate all threads)
void print_merged_report() {
    printf("\n");
    printf("================================================================================\n");
    printf("Merged Profiling Report (All Threads - Phase 4)\n");
    printf("================================================================================\n");
    printf("Total threads: %d\n", thread_snapshot_count);

    if (thread_snapshot_count == 0) {
        printf("No thread data collected.\n");
        return;
    }

    // Merged data structure (use global function registry for mapping)
    typedef struct {
        char name[256];
        time_stamp total_time;
        time_stamp self_time;
        time_stamp user_time;
        time_stamp sys_time;
        time_stamp wait_time;
        time_stamp call_count;
        int thread_count;  // How many threads called this function
    } merged_function_t;

    merged_function_t merged[MAX_GLOBAL_FUNCTIONS];
    memset(merged, 0, sizeof(merged));

    // Initialize function names from global registry
    for (int i = 0; i < global_function_count; i++) {
        strncpy(merged[i].name, global_function_registry[i].name, 255);
    }

    // Aggregate data from all threads
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];

        for (int f = 0; f < snapshot->function_count; f++) {
            if (snapshot->functions[f].call_count == 0) continue;

            // Find function in global registry
            int global_id = -1;
            for (int g = 0; g < global_function_count; g++) {
                if (strcmp(snapshot->functions[f].name, global_function_registry[g].name) == 0) {
                    global_id = g;
                    break;
                }
            }

            if (global_id >= 0) {
                merged[global_id].total_time += snapshot->functions[f].total_time;
                merged[global_id].self_time += snapshot->functions[f].self_time;
                merged[global_id].user_time += snapshot->functions[f].user_time;
                merged[global_id].sys_time += snapshot->functions[f].sys_time;
                merged[global_id].wait_time += snapshot->functions[f].wait_time;
                merged[global_id].call_count += snapshot->functions[f].call_count;
                merged[global_id].thread_count++;
            }
        }
    }

    // Print merged report
    printf("\n%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Threads", "Total(ms)", "User(s)", "Sys(s)", "Wait(s)", "Avg/call", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    time_stamp total_self_time = 0;
    for (int i = 0; i < global_function_count; i++) {
        total_self_time += merged[i].self_time;
    }

    for (int i = 0; i < global_function_count; i++) {
        if (merged[i].call_count > 0) {
            double total_ms = merged[i].total_time / 1000.0;
            double user_s = merged[i].user_time / 1000000.0;
            double sys_s = merged[i].sys_time / 1000000.0;
            double wait_s = merged[i].wait_time / 1000000.0;
            double avg_per_call = (merged[i].call_count > 0) ?
                (total_ms / (double)merged[i].call_count) : 0.0;
            double total_per_call = total_ms / (double)merged[i].call_count;

            printf("%-30s %10llu %10d %10.2f %10.4f %10.4f %10.4f %10.3f %10.3f\n",
                merged[i].name,
                merged[i].call_count,
                merged[i].thread_count,
                total_ms, user_s, sys_s, wait_s,
                avg_per_call, total_per_call);
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");
}

// Cleanup thread snapshots
void cleanup_thread_snapshots() {
    for (int i = 0; i < thread_snapshot_count; i++) {
        free(thread_snapshots[i]);
        thread_snapshots[i] = NULL;
    }
    thread_snapshot_count = 0;
}

// ============================================================
// Phase 3: Multi-threaded Test Functions
// ============================================================

// Thread worker 1: CPU-intensive
void* thread_worker_cpu(void* arg) {
    PROFILE_SCOPE("thread_worker_cpu");

    printf("Thread %ld: Starting CPU-intensive work\n", syscall(SYS_gettid));

    // Perform CPU-heavy work
    for (int i = 0; i < 3; i++) {
        function_cpu_heavy();
    }

    printf("Thread %ld: CPU work done\n", syscall(SYS_gettid));

    // Phase 4: Register thread data before exiting
    register_thread_data();

    return NULL;
}

// Thread worker 2: I/O-intensive
void* thread_worker_io(void* arg) {
    PROFILE_SCOPE("thread_worker_io");

    printf("Thread %ld: Starting I/O-intensive work\n", syscall(SYS_gettid));

    // Perform I/O-heavy work
    function_io_heavy();

    printf("Thread %ld: I/O work done\n", syscall(SYS_gettid));

    // Phase 4: Register thread data before exiting
    register_thread_data();

    return NULL;
}

// Thread worker 3: Sleep-heavy
void* thread_worker_sleep(void* arg) {
    PROFILE_SCOPE("thread_worker_sleep");

    printf("Thread %ld: Starting sleep work\n", syscall(SYS_gettid));

    // Multiple sleeps
    for (int i = 0; i < 5; i++) {
        function_sleep_test();
    }

    printf("Thread %ld: Sleep work done\n", syscall(SYS_gettid));

    // Phase 4: Register thread data before exiting
    register_thread_data();

    return NULL;
}

// Thread worker 4: Mixed workload
void* thread_worker_mixed(void* arg) {
    PROFILE_SCOPE("thread_worker_mixed");

    printf("Thread %ld: Starting mixed work\n", syscall(SYS_gettid));

    // Call various functions
    function_a();
    function_b();
    function_c();
    function_mixed();

    printf("Thread %ld: Mixed work done\n", syscall(SYS_gettid));

    // Phase 4: Register thread data before exiting
    register_thread_data();

    return NULL;
}

// Run single-threaded tests (Phase 0, 1, 2)
void run_single_threaded_tests() {
    printf("\n========================================\n");
    printf("Single-threaded Tests (Phase 0, 1, 2)\n");
    printf("========================================\n");

    // Original CPU-intensive tests
    for (int i = 0; i < 3; i++) {
        PROFILE_SCOPE("main_loop");
        function_a();
        function_b();
        function_c();

        for (volatile int j = 0; j < 1000000; j++);
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

    print_profiling_results();
}

// Phase 4: Test with multiple threads calling same function
void* thread_worker_shared(void* arg) {
    long thread_num = (long)arg;  // Use long to avoid pointer size issues

    // Each thread calls the same functions multiple times
    for (int i = 0; i < thread_num + 1; i++) {
        function_a();
        function_cpu_heavy();
    }

    // Register thread data
    register_thread_data();
    return NULL;
}

void run_shared_function_test() {
    printf("\n========================================\n");
    printf("Shared Function Test (Phase 4)\n");
    printf("========================================\n");
    printf("Testing multiple threads calling same functions...\n\n");

    pthread_t threads[4];

    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, thread_worker_shared, (void*)(long)(i + 1));
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads completed!\n");
}

// Run multi-threaded tests (Phase 3/4)
void run_multi_threaded_tests() {
    printf("\n========================================\n");
    printf("Multi-threaded Tests (Phase 3/4)\n");
    printf("========================================\n");
    printf("Creating 4 threads with different workloads...\n\n");

    pthread_t threads[4];

    // Create 4 threads with different workloads
    pthread_create(&threads[0], NULL, thread_worker_cpu, NULL);
    pthread_create(&threads[1], NULL, thread_worker_io, NULL);
    pthread_create(&threads[2], NULL, thread_worker_sleep, NULL);
    pthread_create(&threads[3], NULL, thread_worker_mixed, NULL);

    // Wait for all threads to complete
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n========================================\n");
    printf("All threads completed!\n");
    printf("========================================\n");
}

int main(int argc, char* argv[]) {
    init_profilier();

#ifndef AUTO_PROFILE
    register_function("main");
    register_function("function_a");
    register_function("function_b");
    register_function("function_c");
#endif

    printf("==============================================\n");
    printf("simple_gprof - Multi-threaded Profiler Demo\n");
    printf("==============================================\n");

    // Phase 4: Parse command line arguments
    int multi_threaded = 0;
    int shared_test = 0;
    char report_mode[32] = "per-thread";  // default: per-thread, options: merged, both

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi-threaded") == 0) {
            multi_threaded = 1;
        } else if (strcmp(argv[i], "--shared-test") == 0) {
            shared_test = 1;
            multi_threaded = 1;  // Shared test requires multi-threading
        } else if (strncmp(argv[i], "--report-mode=", 14) == 0) {
            strncpy(report_mode, argv[i] + 14, 31);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("\nUsage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --multi-threaded         Run multi-threaded tests\n");
            printf("  --shared-test            Run shared function test (multiple threads call same functions)\n");
            printf("  --report-mode=MODE       Report mode: per-thread, merged, or both (default: per-thread)\n");
            printf("  --help                   Show this help message\n\n");
            printf("Examples:\n");
            printf("  %s                                    # Single-threaded test\n", argv[0]);
            printf("  %s --multi-threaded                   # Multi-threaded test, per-thread reports\n", argv[0]);
            printf("  %s --multi-threaded --report-mode=merged  # Multi-threaded test, merged report\n", argv[0]);
            printf("  %s --shared-test --report-mode=both   # Shared function test, both reports\n", argv[0]);
            printf("\n");
            return 0;
        }
    }

    start_profiling();

    if (shared_test) {
        // Phase 4: Shared function test
        run_shared_function_test();
    } else if (multi_threaded) {
        // Phase 3/4: Multi-threaded tests
        run_multi_threaded_tests();
    } else {
        // Default: Single-threaded tests
        run_single_threaded_tests();
    }

    stop_profiling();

    // Phase 4: Generate reports based on mode
    if (multi_threaded || shared_test) {
        // Add main thread data if it has profiling data
        if (function_count > 0) {
            register_thread_data();
        }

        if (strcmp(report_mode, "per-thread") == 0) {
            print_per_thread_reports();
        } else if (strcmp(report_mode, "merged") == 0) {
            print_merged_report();
        } else if (strcmp(report_mode, "both") == 0) {
            print_per_thread_reports();
            print_merged_report();
        } else {
            printf("Unknown report mode: %s\n", report_mode);
            printf("Using default: per-thread\n");
            print_per_thread_reports();
        }

        cleanup_thread_snapshots();
    }

    printf("\nProfiling stopped.\n");

    return 0;
}