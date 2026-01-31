
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

// ============================================================
// Phase 5: Hash Table Implementation
// ============================================================

// Hash table entry (chaining for collision resolution)
typedef struct hash_entry {
    char key[256];              // Function name
    function_info_t value;      // Function profiling data
    struct hash_entry* next;    // Next entry in chain
} hash_entry_t;

// Hash table for functions
typedef struct {
    hash_entry_t** buckets;
    int capacity;
    int size;
} hash_table_t;

// Hash table entry for caller counts
typedef struct caller_entry {
    char key[256];              // Callee function name
    time_stamp count;
    struct caller_entry* next;
} caller_entry_t;

// Hash table for caller->callee relationships
typedef struct {
    caller_entry_t** buckets;
    int capacity;
    int size;
} caller_hash_table_t;

// Hash table entry for caller_counts[caller][callee]
typedef struct caller_map_entry {
    char key[256];              // Caller function name
    caller_hash_table_t* callees; // Hash table of callees
    struct caller_map_entry* next;
} caller_map_entry_t;

// Top-level caller counts hash table
typedef struct {
    caller_map_entry_t** buckets;
    int capacity;
    int size;
} caller_counts_hash_t;

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

// Thread profiling data snapshot (Phase 5: use hash tables)
typedef struct {
    pid_t thread_id;
    hash_table_t* functions;             // Deep copy of function hash table
    caller_counts_hash_t* caller_counts; // Deep copy of caller counts
} thread_data_snapshot_t;

#define MAX_THREADS 64
static thread_data_snapshot_t* thread_snapshots[MAX_THREADS];
static int thread_snapshot_count = 0;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;


// Thread-local storage (Phase 5): Each thread has its own hash tables
__thread hash_table_t* functions = NULL;
__thread caller_counts_hash_t* caller_counts = NULL;
__thread pid_t current_thread_id = 0;  // Cached thread ID

// Global profiling control (shared across threads)
static struct itimerval timer;
static int profiling_enabled = 0;

// Thread-local call stack (Phase 5: stores function names instead of IDs)
typedef struct {
    char stack[MAX_CALL_STACK][256];  // Function names
    int top;
} call_stack_t;

__thread call_stack_t call_stack = {.top = -1};

// ============================================================
// Phase 5: Hash Table Functions
// ============================================================

// djb2 hash function
static unsigned long hash_string(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

// Create hash table for functions
static hash_table_t* create_hash_table(int capacity) {
    hash_table_t* ht = (hash_table_t*)malloc(sizeof(hash_table_t));
    if (!ht) return NULL;

    ht->capacity = capacity;
    ht->size = 0;
    ht->buckets = (hash_entry_t**)calloc(capacity, sizeof(hash_entry_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    return ht;
}

// Create caller hash table
static caller_hash_table_t* create_caller_hash_table(int capacity) {
    caller_hash_table_t* ht = (caller_hash_table_t*)malloc(sizeof(caller_hash_table_t));
    if (!ht) return NULL;

    ht->capacity = capacity;
    ht->size = 0;
    ht->buckets = (caller_entry_t**)calloc(capacity, sizeof(caller_entry_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    return ht;
}

// Create caller counts hash table
static caller_counts_hash_t* create_caller_counts_hash() {
    caller_counts_hash_t* ht = (caller_counts_hash_t*)malloc(sizeof(caller_counts_hash_t));
    if (!ht) return NULL;

    ht->capacity = 128;
    ht->size = 0;
    ht->buckets = (caller_map_entry_t**)calloc(128, sizeof(caller_map_entry_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    return ht;
}

// Find function in hash table
static function_info_t* hash_find(hash_table_t* ht, const char* key) {
    if (!ht || !key) return NULL;

    unsigned long hash = hash_string(key) % ht->capacity;
    hash_entry_t* entry = ht->buckets[hash];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return &entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

// Insert or update function in hash table
static function_info_t* hash_insert(hash_table_t* ht, const char* key) {
    if (!ht || !key) return NULL;

    // Check if already exists
    function_info_t* existing = hash_find(ht, key);
    if (existing) return existing;

    // Create new entry
    unsigned long hash = hash_string(key) % ht->capacity;
    hash_entry_t* entry = (hash_entry_t*)malloc(sizeof(hash_entry_t));
    if (!entry) return NULL;

    strncpy(entry->key, key, 255);
    entry->key[255] = '\0';
    memset(&entry->value, 0, sizeof(function_info_t));
    strncpy(entry->value.name, key, 255);
    entry->value.name[255] = '\0';

    // Insert at head of chain
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->size++;

    return &entry->value;
}

// Find caller count
static time_stamp* caller_hash_find(caller_hash_table_t* ht, const char* key) {
    if (!ht || !key) return NULL;

    unsigned long hash = hash_string(key) % ht->capacity;
    caller_entry_t* entry = ht->buckets[hash];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return &entry->count;
        }
        entry = entry->next;
    }
    return NULL;
}

// Insert or update caller count
static time_stamp* caller_hash_insert(caller_hash_table_t* ht, const char* key) {
    if (!ht || !key) return NULL;

    // Check if already exists
    time_stamp* existing = caller_hash_find(ht, key);
    if (existing) return existing;

    // Create new entry
    unsigned long hash = hash_string(key) % ht->capacity;
    caller_entry_t* entry = (caller_entry_t*)malloc(sizeof(caller_entry_t));
    if (!entry) return NULL;

    strncpy(entry->key, key, 255);
    entry->key[255] = '\0';
    entry->count = 0;

    // Insert at head of chain
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->size++;

    return &entry->count;
}

// Find or create caller map entry
static caller_hash_table_t* caller_counts_find_or_create(caller_counts_hash_t* ht, const char* caller) {
    if (!ht || !caller) return NULL;

    unsigned long hash = hash_string(caller) % ht->capacity;
    caller_map_entry_t* entry = ht->buckets[hash];

    // Search for existing
    while (entry) {
        if (strcmp(entry->key, caller) == 0) {
            return entry->callees;
        }
        entry = entry->next;
    }

    // Create new entry
    entry = (caller_map_entry_t*)malloc(sizeof(caller_map_entry_t));
    if (!entry) return NULL;

    strncpy(entry->key, caller, 255);
    entry->key[255] = '\0';
    entry->callees = create_caller_hash_table(64);

    // Insert at head
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->size++;

    return entry->callees;
}

// Free hash table
static void free_hash_table(hash_table_t* ht) {
    if (!ht) return;

    for (int i = 0; i < ht->capacity; i++) {
        hash_entry_t* entry = ht->buckets[i];
        while (entry) {
            hash_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(ht->buckets);
    free(ht);
}

// Free caller hash table
static void free_caller_hash_table(caller_hash_table_t* ht) {
    if (!ht) return;

    for (int i = 0; i < ht->capacity; i++) {
        caller_entry_t* entry = ht->buckets[i];
        while (entry) {
            caller_entry_t* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(ht->buckets);
    free(ht);
}

// Free caller counts hash
static void free_caller_counts_hash(caller_counts_hash_t* ht) {
    if (!ht) return;

    for (int i = 0; i < ht->capacity; i++) {
        caller_map_entry_t* entry = ht->buckets[i];
        while (entry) {
            caller_map_entry_t* next = entry->next;
            free_caller_hash_table(entry->callees);
            free(entry);
            entry = next;
        }
    }
    free(ht->buckets);
    free(ht);
}

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

    if (call_stack.top>=0 && functions){
        // Phase 5: Use function name from call stack to find function in hash table
        const char* current_func_name = call_stack.stack[call_stack.top];
        function_info_t* func = hash_find(functions, current_func_name);
        if (func) {
            func->self_time += interval_us;
        }
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

// Phase 5: Thread-safe function registration using hash tables
// Returns function name pointer for use in enter/leave
const char* register_function(const char *name) {
    // Initialize thread ID on first registration
    if (current_thread_id == 0) {
        current_thread_id = syscall(SYS_gettid);
    }

    // Initialize hash tables if not already done
    if (!functions) {
        functions = create_hash_table(512);
    }
    if (!caller_counts) {
        caller_counts = create_caller_counts_hash();
    }

    // Thread-safe: Register in global registry (for merging reports)
    pthread_mutex_lock(&registry_mutex);
    int found = 0;
    for (int i = 0; i < global_function_count; i++) {
        if (strcmp(global_function_registry[i].name, name) == 0) {
            found = 1;
            break;
        }
    }
    if (!found && global_function_count < MAX_GLOBAL_FUNCTIONS) {
        strncpy(global_function_registry[global_function_count].name, name, 255);
        global_function_registry[global_function_count].id = global_function_count;
        global_function_count++;
    }
    pthread_mutex_unlock(&registry_mutex);

    // Find or create in thread-local hash table
    function_info_t* func = hash_insert(functions, name);
    if (func && func->call_count == 0) {
        // Initialize new function entry
        func->thread_id = current_thread_id;
    }

    return name;
}

// Phase 5: Enter function using function name
void enter_function(const char* func_name) {
    if (!func_name || !functions) return;

    function_info_t* func = hash_find(functions, func_name);
    if (!func) {
        func = hash_insert(functions, func_name);
        if (!func) return;
        func->thread_id = current_thread_id;
    }

    func->call_count++;
    func->is_active = 1;

    // Record wall clock time with nanosecond precision (Phase 2)
    clock_gettime(CLOCK_MONOTONIC, &func->start_wall_time);

    // Phase 3: Use RUSAGE_THREAD for per-thread CPU time tracking
    #ifdef __linux__
        getrusage(RUSAGE_THREAD, &func->start_rusage);
    #else
        getrusage(RUSAGE_SELF, &func->start_rusage);
    #endif

    // Track caller-callee relationship
    if (call_stack.top >= 0 && caller_counts) {
        const char* caller_name = call_stack.stack[call_stack.top];
        caller_hash_table_t* callees = caller_counts_find_or_create(caller_counts, caller_name);
        if (callees) {
            time_stamp* count = caller_hash_insert(callees, func_name);
            if (count) {
                (*count)++;
            }
        }
    }

    // Push function name onto call stack
    if (call_stack.top < MAX_CALL_STACK - 1) {
        strncpy(call_stack.stack[call_stack.top + 1], func_name, 255);
        call_stack.stack[call_stack.top + 1][255] = '\0';
        call_stack.top++;
    }
}

// Phase 5: Leave function using function name
void leave_function(const char* func_name) {
    if (!func_name || !functions) return;

    function_info_t* func = hash_find(functions, func_name);
    if (!func) return;

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
    long long wall_delta = (end_wall_time.tv_sec - func->start_wall_time.tv_sec) * 1000000LL +
                           (end_wall_time.tv_nsec - func->start_wall_time.tv_nsec) / 1000;

    // Calculate user time delta (user mode CPU time)
    long long user_delta = (end_rusage.ru_utime.tv_sec - func->start_rusage.ru_utime.tv_sec) * 1000000LL +
                           (end_rusage.ru_utime.tv_usec - func->start_rusage.ru_utime.tv_usec);

    // Calculate system time delta (kernel mode CPU time)
    long long sys_delta = (end_rusage.ru_stime.tv_sec - func->start_rusage.ru_stime.tv_sec) * 1000000LL +
                          (end_rusage.ru_stime.tv_usec - func->start_rusage.ru_stime.tv_usec);

    // Calculate wait time: Wall time - (User time + System time)
    long long wait_delta = wall_delta - (user_delta + sys_delta);
    if (wait_delta < 0) wait_delta = 0;

    // Accumulate all times
    func->total_time += wall_delta;
    func->user_time += user_delta;
    func->sys_time += sys_delta;
    func->wait_time += wait_delta;

    func->is_active = 0;

    // Pop from call stack
    if (call_stack.top >= 0 && strcmp(call_stack.stack[call_stack.top], func_name) == 0) {
        call_stack.top--;
    }
}


// Phase 5: Deep copy hash table for thread snapshot
static hash_table_t* deep_copy_hash_table(hash_table_t* src) {
    if (!src) return NULL;

    hash_table_t* dst = create_hash_table(src->capacity);
    if (!dst) return NULL;

    // Copy all entries
    for (int i = 0; i < src->capacity; i++) {
        hash_entry_t* entry = src->buckets[i];
        while (entry) {
            function_info_t* dst_func = hash_insert(dst, entry->key);
            if (dst_func) {
                *dst_func = entry->value;
            }
            entry = entry->next;
        }
    }
    return dst;
}

// Phase 5: Deep copy caller counts hash table
static caller_counts_hash_t* deep_copy_caller_counts(caller_counts_hash_t* src) {
    if (!src) return NULL;

    caller_counts_hash_t* dst = create_caller_counts_hash();
    if (!dst) return NULL;

    // Copy all caller->callee relationships
    for (int i = 0; i < src->capacity; i++) {
        caller_map_entry_t* entry = src->buckets[i];
        while (entry) {
            caller_hash_table_t* dst_callees = caller_counts_find_or_create(dst, entry->key);
            if (dst_callees && entry->callees) {
                // Copy all callees
                for (int j = 0; j < entry->callees->capacity; j++) {
                    caller_entry_t* callee = entry->callees->buckets[j];
                    while (callee) {
                        time_stamp* dst_count = caller_hash_insert(dst_callees, callee->key);
                        if (dst_count) {
                            *dst_count = callee->count;
                        }
                        callee = callee->next;
                    }
                }
            }
            entry = entry->next;
        }
    }
    return dst;
}

// Phase 5: Register current thread's profiling data (deep copy hash tables)
void register_thread_data() {
    pid_t tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);

    // Skip if no profiling data
    if (!functions || functions->size == 0) {
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

    // Deep copy hash tables
    snapshot->thread_id = tid;
    snapshot->functions = deep_copy_hash_table(functions);
    snapshot->caller_counts = deep_copy_caller_counts(caller_counts);

    // Add to global collection
    thread_snapshots[thread_snapshot_count++] = snapshot;

    pthread_mutex_unlock(&snapshot_mutex);
}

// Phase 5: Print profiling results using hash table iteration
void print_profiling_results() {
    if (!functions) return;

    pid_t tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);

    printf("\n=== Profiling Results (Phase 5: Thread %d) ===\n", tid);
    printf("%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Total(ms)", "Self(ms)", "User(s)", "Sys(s)", "Wait(s)", "Self%", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    // First pass: calculate total self time
    time_stamp total_self_time = 0;
    for (int i = 0; i < functions->capacity; i++) {
        hash_entry_t* entry = functions->buckets[i];
        while (entry) {
            total_self_time += entry->value.self_time;
            entry = entry->next;
        }
    }

    // Second pass: print function stats
    for (int i = 0; i < functions->capacity; i++) {
        hash_entry_t* entry = functions->buckets[i];
        while (entry) {
            function_info_t* func = &entry->value;
            if (func->call_count > 0) {
                double total_ms = func->total_time / 1000.0;
                double self_ms = func->self_time / 1000.0;
                double user_s = func->user_time / 1000000.0;
                double sys_s = func->sys_time / 1000000.0;
                double wait_s = func->wait_time / 1000000.0;
                double self_percent = (total_self_time > 0) ?
                    (func->self_time * 100.0 / total_self_time) : 0;
                double avg_total = (func->call_count > 0) ?
                    (total_ms / (double)func->call_count) : 0.0;

                printf("%-30s %10llu %10.2f %10.2f %10.4f %10.4f %10.4f %9.2f%% %10.3f\n",
                    func->name, func->call_count, total_ms, self_ms,
                    user_s, sys_s, wait_s, self_percent, avg_total);
            }
            entry = entry->next;
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    // Show caller counts
    if (caller_counts) {
        printf("\n--- Callers (counts) ---\n");
        for (int i = 0; i < functions->capacity; i++) {
            hash_entry_t* callee_entry = functions->buckets[i];
            while (callee_entry) {
                if (callee_entry->value.call_count == 0) {
                    callee_entry = callee_entry->next;
                    continue;
                }
                printf("%-30s <- ", callee_entry->key);
                int has_caller = 0;

                // Find callers in caller_counts hash
                for (int j = 0; j < caller_counts->capacity; j++) {
                    caller_map_entry_t* caller_entry = caller_counts->buckets[j];
                    while (caller_entry) {
                        if (caller_entry->callees) {
                            time_stamp* count = caller_hash_find(caller_entry->callees, callee_entry->key);
                            if (count && *count > 0) {
                                has_caller = 1;
                                printf("%s(%llu) ", caller_entry->key, *count);
                            }
                        }
                        caller_entry = caller_entry->next;
                    }
                }
                if (!has_caller) {
                    printf("[none]");
                }
                printf("\n");
                callee_entry = callee_entry->next;
            }
        }
    }
}

// Phase 5: Cleanup function using function name pointer
static inline void __profile_cleanup_str(void *p) {
    const char* name = *(const char **)p;
    leave_function(name);
}

#ifdef AUTO_PROFILE
// Phase 5: Use function name pointers instead of IDs
#define PROFILE_FUNCTION()                                                     \
    static __thread const char* __func_name = NULL;                            \
    if (__func_name == NULL) {                                                 \
        __func_name = register_function(__func__);                             \
    }                                                                          \
    enter_function(__func_name);                                               \
    __attribute__((cleanup(__profile_cleanup_str))) const char* __profile_scope_guard = __func_name;

#define PROFILE_SCOPE(name)                                                    \
    static __thread const char* __scope_name_##__LINE__ = NULL;                \
    if (__scope_name_##__LINE__ == NULL) {                                     \
        __scope_name_##__LINE__ = register_function(name);                     \
    }                                                                          \
    enter_function(__scope_name_##__LINE__);                                   \
    __attribute__((cleanup(__profile_cleanup_str))) const char* __profile_scope_guard_##__LINE__ = __scope_name_##__LINE__;

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
// ============================================================
// Phase 5: Report Generation Functions (Hash Table Version)
// ============================================================

// Print report for a single thread snapshot (Phase 5)
void print_thread_report(thread_data_snapshot_t* snapshot) {
    if (!snapshot || !snapshot->functions) return;

    printf("\n=== Thread %d Report ===\n", snapshot->thread_id);
    printf("%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Total(ms)", "Self(ms)", "User(s)", "Sys(s)", "Wait(s)", "Self%", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    // Calculate total self time
    time_stamp total_self_time = 0;
    for (int i = 0; i < snapshot->functions->capacity; i++) {
        hash_entry_t* entry = snapshot->functions->buckets[i];
        while (entry) {
            total_self_time += entry->value.self_time;
            entry = entry->next;
        }
    }

    // Print function stats
    for (int i = 0; i < snapshot->functions->capacity; i++) {
        hash_entry_t* entry = snapshot->functions->buckets[i];
        while (entry) {
            function_info_t* func = &entry->value;
            if (func->call_count > 0) {
                double total_ms = func->total_time / 1000.0;
                double self_ms = func->self_time / 1000.0;
                double user_s = func->user_time / 1000000.0;
                double sys_s = func->sys_time / 1000000.0;
                double wait_s = func->wait_time / 1000000.0;
                double self_percent = (total_self_time > 0) ?
                    (func->self_time * 100.0 / total_self_time) : 0;
                double avg_total = (func->call_count > 0) ?
                    (total_ms / (double)func->call_count) : 0.0;

                printf("%-30s %10llu %10.2f %10.2f %10.4f %10.4f %10.4f %9.2f%% %10.3f\n",
                    func->name, func->call_count, total_ms, self_ms,
                    user_s, sys_s, wait_s, self_percent, avg_total);
            }
            entry = entry->next;
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

// Print merged report (Phase 5: aggregate all threads using hash tables)
void print_merged_report() {
    printf("\n");
    printf("================================================================================\n");
    printf("Merged Profiling Report (All Threads - Phase 5)\n");
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

    // Aggregate data from all threads (Phase 5: iterate hash tables)
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->functions) continue;

        // Iterate through hash table
        for (int b = 0; b < snapshot->functions->capacity; b++) {
            hash_entry_t* entry = snapshot->functions->buckets[b];
            while (entry) {
                if (entry->value.call_count == 0) {
                    entry = entry->next;
                    continue;
                }

                // Find function in global registry
                int global_id = -1;
                for (int g = 0; g < global_function_count; g++) {
                    if (strcmp(entry->value.name, global_function_registry[g].name) == 0) {
                        global_id = g;
                        break;
                    }
                }

                if (global_id >= 0) {
                    merged[global_id].total_time += entry->value.total_time;
                    merged[global_id].self_time += entry->value.self_time;
                    merged[global_id].user_time += entry->value.user_time;
                    merged[global_id].sys_time += entry->value.sys_time;
                    merged[global_id].wait_time += entry->value.wait_time;
                    merged[global_id].call_count += entry->value.call_count;
                    merged[global_id].thread_count++;
                }
                entry = entry->next;
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

// ============================================================
// Phase 8: Graphviz DOT Format Output
// ============================================================

// Generate color based on percentage (hot = red, cold = blue)
static const char* get_color_for_percentage(double percent) {
    if (percent > 20.0) return "#FF0000";      // Red - very hot
    if (percent > 10.0) return "#FF8800";      // Orange - hot
    if (percent > 5.0) return "#FFFF00";       // Yellow - warm
    if (percent > 1.0) return "#88FF88";       // Light green - cool
    return "#AAAAFF";                          // Light blue - cold
}

// Phase 5: Export call graph to DOT format (per-thread, hash table version)
void export_dot_per_thread(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create %s\n", filename);
        return;
    }

    fprintf(fp, "digraph CallGraph {\n");
    fprintf(fp, "    rankdir=LR;\n");
    fprintf(fp, "    node [shape=box, style=filled];\n");
    fprintf(fp, "    \n");

    // Calculate total self time for percentage
    time_stamp total_self_time = 0;
    for (int t = 0; t < thread_snapshot_count; t++) {
        if (!thread_snapshots[t] || !thread_snapshots[t]->functions) continue;
        hash_table_t* ht = thread_snapshots[t]->functions;
        for (int i = 0; i < ht->capacity; i++) {
            hash_entry_t* entry = ht->buckets[i];
            while (entry) {
                total_self_time += entry->value.self_time;
                entry = entry->next;
            }
        }
    }

    // Add nodes (functions) for each thread
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->functions) continue;

        fprintf(fp, "    // Thread %d\n", snapshot->thread_id);
        fprintf(fp, "    subgraph cluster_%d {\n", snapshot->thread_id);
        fprintf(fp, "        label=\"Thread %d\";\n", snapshot->thread_id);
        fprintf(fp, "        style=dashed;\n");

        // Iterate hash table
        for (int i = 0; i < snapshot->functions->capacity; i++) {
            hash_entry_t* entry = snapshot->functions->buckets[i];
            while (entry) {
                if (entry->value.call_count > 0) {
                    double percent = (total_self_time > 0) ?
                        (entry->value.self_time * 100.0 / total_self_time) : 0;
                    const char* color = get_color_for_percentage(percent);

                    fprintf(fp, "        \"T%d_%s\" [label=\"%s\\n%.1f%%\\n%llu calls\", fillcolor=\"%s\"];\n",
                        snapshot->thread_id,
                        entry->key,
                        entry->key,
                        percent,
                        entry->value.call_count,
                        color);
                }
                entry = entry->next;
            }
        }

        fprintf(fp, "    }\n\n");
    }

    // Add edges (caller -> callee)
    fprintf(fp, "    // Call relationships\n");
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->caller_counts) continue;

        // Iterate caller_counts hash table
        for (int i = 0; i < snapshot->caller_counts->capacity; i++) {
            caller_map_entry_t* caller_entry = snapshot->caller_counts->buckets[i];
            while (caller_entry) {
                if (caller_entry->callees) {
                    // Iterate callees for this caller
                    for (int j = 0; j < caller_entry->callees->capacity; j++) {
                        caller_entry_t* callee = caller_entry->callees->buckets[j];
                        while (callee) {
                            if (callee->count > 0) {
                                fprintf(fp, "    \"T%d_%s\" -> \"T%d_%s\" [label=\"%llu\"];\n",
                                    snapshot->thread_id,
                                    caller_entry->key,
                                    snapshot->thread_id,
                                    callee->key,
                                    callee->count);
                            }
                            callee = callee->next;
                        }
                    }
                }
                caller_entry = caller_entry->next;
            }
        }
    }

    fprintf(fp, "}\n");
    fclose(fp);

    printf("Call graph exported to %s\n", filename);
    printf("Generate image with: dot -Tpng %s -o callgraph.png\n", filename);
}

// Phase 5: Export merged call graph to DOT format (hash table version)
void export_dot_merged(const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create %s\n", filename);
        return;
    }

    fprintf(fp, "digraph MergedCallGraph {\n");
    fprintf(fp, "    rankdir=LR;\n");
    fprintf(fp, "    node [shape=box, style=filled];\n");
    fprintf(fp, "    \n");

    // Merged function data
    typedef struct {
        char name[256];
        time_stamp self_time;
        time_stamp call_count;
        int thread_count;
    } merged_func_t;

    merged_func_t merged[MAX_GLOBAL_FUNCTIONS];
    memset(merged, 0, sizeof(merged));

    // Initialize from global registry
    for (int i = 0; i < global_function_count; i++) {
        strncpy(merged[i].name, global_function_registry[i].name, 255);
    }

    // Aggregate data from hash tables
    time_stamp total_self_time = 0;
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->functions) continue;

        // Iterate hash table
        for (int b = 0; b < snapshot->functions->capacity; b++) {
            hash_entry_t* entry = snapshot->functions->buckets[b];
            while (entry) {
                if (entry->value.call_count == 0) {
                    entry = entry->next;
                    continue;
                }

                // Find in global registry
                int global_id = -1;
                for (int g = 0; g < global_function_count; g++) {
                    if (strcmp(entry->value.name, global_function_registry[g].name) == 0) {
                        global_id = g;
                        break;
                    }
                }

                if (global_id >= 0) {
                    merged[global_id].self_time += entry->value.self_time;
                    merged[global_id].call_count += entry->value.call_count;
                    merged[global_id].thread_count++;
                    total_self_time += entry->value.self_time;
                }
                entry = entry->next;
            }
        }
    }

    // Add nodes
    fprintf(fp, "    // Functions (merged from all threads)\n");
    for (int i = 0; i < global_function_count; i++) {
        if (merged[i].call_count == 0) continue;

        double percent = (total_self_time > 0) ?
            (merged[i].self_time * 100.0 / total_self_time) : 0;
        const char* color = get_color_for_percentage(percent);

        fprintf(fp, "    \"%s\" [label=\"%s\\n%.1f%%\\n%llu calls\\n%d threads\", fillcolor=\"%s\"];\n",
            merged[i].name,
            merged[i].name,
            percent,
            merged[i].call_count,
            merged[i].thread_count,
            color);
    }

    // Aggregate caller-callee relationships from hash tables
    typedef struct {
        char caller[256];
        char callee[256];
        time_stamp count;
    } call_edge_t;

    call_edge_t edges[10000];  // Assume max 10k edges
    int edge_count = 0;

    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->caller_counts) continue;

        // Iterate caller_counts hash table
        for (int i = 0; i < snapshot->caller_counts->capacity; i++) {
            caller_map_entry_t* caller_entry = snapshot->caller_counts->buckets[i];
            while (caller_entry) {
                if (caller_entry->callees) {
                    // Iterate callees
                    for (int j = 0; j < caller_entry->callees->capacity; j++) {
                        caller_entry_t* callee = caller_entry->callees->buckets[j];
                        while (callee) {
                            if (callee->count > 0) {
                                // Find or create edge
                                int found = -1;
                                for (int e = 0; e < edge_count; e++) {
                                    if (strcmp(edges[e].caller, caller_entry->key) == 0 &&
                                        strcmp(edges[e].callee, callee->key) == 0) {
                                        found = e;
                                        break;
                                    }
                                }

                                if (found >= 0) {
                                    edges[found].count += callee->count;
                                } else if (edge_count < 10000) {
                                    strncpy(edges[edge_count].caller, caller_entry->key, 255);
                                    strncpy(edges[edge_count].callee, callee->key, 255);
                                    edges[edge_count].count = callee->count;
                                    edge_count++;
                                }
                            }
                            callee = callee->next;
                        }
                    }
                }
                caller_entry = caller_entry->next;
            }
        }
    }

    // Add edges
    fprintf(fp, "\n    // Call relationships\n");
    for (int i = 0; i < edge_count; i++) {
        fprintf(fp, "    \"%s\" -> \"%s\" [label=\"%llu\"];\n",
            edges[i].caller,
            edges[i].callee,
            edges[i].count);
    }

    fprintf(fp, "}\n");
    fclose(fp);

    printf("Merged call graph exported to %s\n", filename);
    printf("Generate image with: dot -Tpng %s -o callgraph_merged.png\n", filename);
}

// ============================================================
// Phase 5: Memory Cleanup Functions
// ============================================================

// Free all thread snapshots
void cleanup_thread_snapshots() {
    pthread_mutex_lock(&snapshot_mutex);
    for (int i = 0; i < thread_snapshot_count; i++) {
        if (thread_snapshots[i]) {
            free_hash_table(thread_snapshots[i]->functions);
            free_caller_counts_hash(thread_snapshots[i]->caller_counts);
            free(thread_snapshots[i]);
            thread_snapshots[i] = NULL;
        }
    }
    thread_snapshot_count = 0;
    pthread_mutex_unlock(&snapshot_mutex);
}

// Free current thread's TLS hash tables
static void cleanup_thread_local() {
    if (functions) {
        free_hash_table(functions);
        functions = NULL;
    }
    if (caller_counts) {
        free_caller_counts_hash(caller_counts);
        caller_counts = NULL;
    }
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

    // Phase 5: Cleanup thread-local memory
    cleanup_thread_local();

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

    // Phase 5: Cleanup thread-local memory
    cleanup_thread_local();

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

    // Phase 5: Cleanup thread-local memory
    cleanup_thread_local();

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

    // Phase 5: Cleanup thread-local memory
    cleanup_thread_local();

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

    // Phase 5: Cleanup thread-local memory
    cleanup_thread_local();

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

    // Phase 4/8: Parse command line arguments
    int multi_threaded = 0;
    int shared_test = 0;
    int export_dot = 0;  // Phase 8: Export call graph to DOT format
    char report_mode[32] = "per-thread";  // default: per-thread, options: merged, both
    char dot_mode[32] = "merged";  // Phase 8: per-thread or merged

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi-threaded") == 0) {
            multi_threaded = 1;
        } else if (strcmp(argv[i], "--shared-test") == 0) {
            shared_test = 1;
            multi_threaded = 1;  // Shared test requires multi-threading
        } else if (strncmp(argv[i], "--report-mode=", 14) == 0) {
            strncpy(report_mode, argv[i] + 14, 31);
        } else if (strcmp(argv[i], "--export-dot") == 0) {
            export_dot = 1;  // Phase 8: Enable DOT export
        } else if (strncmp(argv[i], "--dot-mode=", 11) == 0) {
            strncpy(dot_mode, argv[i] + 11, 31);  // Phase 8: per-thread or merged
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("\nUsage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --multi-threaded         Run multi-threaded tests\n");
            printf("  --shared-test            Run shared function test (multiple threads call same functions)\n");
            printf("  --report-mode=MODE       Report mode: per-thread, merged, or both (default: per-thread)\n");
            printf("  --export-dot             Export call graph to Graphviz DOT format (Phase 8)\n");
            printf("  --dot-mode=MODE          DOT export mode: per-thread or merged (default: merged)\n");
            printf("  --help                   Show this help message\n\n");
            printf("Examples:\n");
            printf("  %s                                    # Single-threaded test\n", argv[0]);
            printf("  %s --multi-threaded                   # Multi-threaded test, per-thread reports\n", argv[0]);
            printf("  %s --multi-threaded --report-mode=merged  # Multi-threaded test, merged report\n", argv[0]);
            printf("  %s --shared-test --report-mode=both   # Shared function test, both reports\n", argv[0]);
            printf("  %s --multi-threaded --export-dot      # Export merged call graph to DOT\n", argv[0]);
            printf("  %s --multi-threaded --export-dot --dot-mode=per-thread  # Export per-thread call graphs\n", argv[0]);
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

    // Phase 5: Generate reports based on mode
    if (multi_threaded || shared_test) {
        // Add main thread data if it has profiling data
        if (functions && functions->size > 0) {
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

        // Phase 8: Export call graph to DOT format
        if (export_dot) {
            printf("\n");
            printf("================================================================================\n");
            printf("Exporting Call Graph (Phase 8)\n");
            printf("================================================================================\n");

            if (strcmp(dot_mode, "per-thread") == 0) {
                export_dot_per_thread("callgraph_per_thread.dot");
            } else if (strcmp(dot_mode, "merged") == 0) {
                export_dot_merged("callgraph_merged.dot");
            } else {
                printf("Unknown DOT mode: %s, using merged\n", dot_mode);
                export_dot_merged("callgraph_merged.dot");
            }
        }

        cleanup_thread_snapshots();
    }

    printf("\nProfiling stopped.\n");

    // Phase 5: Cleanup main thread's TLS memory
    cleanup_thread_local();

    return 0;
}