#ifndef PROFILER_CORE_H
#define PROFILER_CORE_H

#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_FUNCTIONS 1000
#define MAX_CALL_STACK 100
#define PROFILING_INTERVAL 10000
#define MAX_GLOBAL_FUNCTIONS 1000
#define MAX_THREADS 64

typedef unsigned long long time_stamp;

typedef struct {
    char name[256];
    void* addr;
    time_stamp total_time;
    time_stamp self_time;
    time_stamp user_time;
    time_stamp sys_time;
    time_stamp wait_time;
    time_stamp call_count;
    int is_active;
    pid_t thread_id;
    struct timespec start_wall_time;
    struct rusage start_rusage;
} function_info_t;

typedef struct hash_entry {
    char key[256];
    function_info_t value;
    struct hash_entry* next;
} hash_entry_t;

typedef struct {
    hash_entry_t** buckets;
    int capacity;
    int size;
} hash_table_t;

typedef struct caller_entry {
    char key[256];
    time_stamp count;
    struct caller_entry* next;
} caller_entry_t;

typedef struct {
    caller_entry_t** buckets;
    int capacity;
    int size;
} caller_hash_table_t;

typedef struct caller_map_entry {
    char key[256];
    caller_hash_table_t* callees;
    struct caller_map_entry* next;
} caller_map_entry_t;

typedef struct {
    caller_map_entry_t** buckets;
    int capacity;
    int size;
} caller_counts_hash_t;

typedef struct {
    char name[256];
    int id;
} function_registry_entry_t;

typedef struct {
    pid_t thread_id;
    hash_table_t* functions;
    caller_counts_hash_t* caller_counts;
} thread_data_snapshot_t;

typedef struct {
    char stack[MAX_CALL_STACK][256];
    int top;
} call_stack_t;

extern function_registry_entry_t global_function_registry[MAX_GLOBAL_FUNCTIONS];
extern int global_function_count;
extern pthread_mutex_t registry_mutex;

extern thread_data_snapshot_t* thread_snapshots[MAX_THREADS];
extern int thread_snapshot_count;
extern pthread_mutex_t snapshot_mutex;

extern __thread hash_table_t* functions;
extern __thread caller_counts_hash_t* caller_counts;
extern __thread pid_t current_thread_id;
extern __thread call_stack_t call_stack;

/* Creates a function-statistics hash table with the requested bucket count. */
hash_table_t* create_hash_table(int capacity);

/* Creates a caller->callee hash table with the requested bucket count. */
caller_hash_table_t* create_caller_hash_table(int capacity);

/* Creates the top-level caller-count map used by each thread. */
caller_counts_hash_t* create_caller_counts_hash(void);

/* Finds one function record by name inside a function hash table. */
function_info_t* hash_find(hash_table_t* ht, const char* key);

/* Inserts a function record when missing and returns the stored entry. */
function_info_t* hash_insert(hash_table_t* ht, const char* key);

/* Finds one caller->callee counter by callee name. */
time_stamp* caller_hash_find(caller_hash_table_t* ht, const char* key);

/* Inserts a caller->callee counter when missing and returns it. */
time_stamp* caller_hash_insert(caller_hash_table_t* ht, const char* key);

/* Looks up or creates the callee map for one caller function. */
caller_hash_table_t* caller_counts_find_or_create(caller_counts_hash_t* ht, const char* caller);

/* Releases one function hash table and all chained entries. */
void free_hash_table(hash_table_t* ht);

/* Releases one caller hash table and all chained entries. */
void free_caller_hash_table(caller_hash_table_t* ht);

/* Releases the top-level caller-count map and nested callee tables. */
void free_caller_counts_hash(caller_counts_hash_t* ht);

/* Signal handler that turns periodic samples into self-time accounting. */
void profiling_handler(int sig);

/* Configures the SIGPROF handler and sampling timer. */
void init_profiler(void);

/* Starts interval-based sampling for the current process. */
void start_profiling(void);

/* Stops interval-based sampling for the current process. */
void stop_profiling(void);

/* Registers a function name in thread-local and global profiler state. */
const char* register_function(const char* name);

/* Marks the entry of one profiled function and records timing baselines. */
void enter_function(const char* func_name);

/* Marks the exit of one profiled function and accumulates timing deltas. */
void leave_function(const char* func_name);

/* Stores a deep copy of the current thread's profiling data for later reports. */
void register_thread_data(void);

/* Builds a temporary merged view of all thread function tables. */
hash_table_t* build_merged_function_lookup(void);

/* Frees every stored thread snapshot after reporting is complete. */
void cleanup_thread_snapshots(void);

/* Frees the current thread's thread-local profiler data. */
void cleanup_current_thread_data(void);

/* Cleanup hook used by PROFILE_FUNCTION and PROFILE_SCOPE guards. */
void profile_cleanup_str(void* p);

#ifdef AUTO_PROFILE
#define PROFILE_FUNCTION()                                                     \
    static __thread const char* __func_name = NULL;                            \
    if (__func_name == NULL) {                                                 \
        __func_name = register_function(__func__);                             \
    }                                                                          \
    enter_function(__func_name);                                               \
    __attribute__((cleanup(profile_cleanup_str))) const char* __profile_scope_guard = __func_name

#define PROFILE_SCOPE(name)                                                    \
    static __thread const char* __scope_name_##__LINE__ = NULL;                \
    if (__scope_name_##__LINE__ == NULL) {                                     \
        __scope_name_##__LINE__ = register_function(name);                     \
    }                                                                          \
    enter_function(__scope_name_##__LINE__);                                   \
    __attribute__((cleanup(profile_cleanup_str))) const char* __profile_scope_guard_##__LINE__ = __scope_name_##__LINE__
#else
#define PROFILE_FUNCTION()
#define PROFILE_SCOPE(name)
#endif

#endif
