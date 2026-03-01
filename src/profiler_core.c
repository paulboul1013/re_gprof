#include "profiler_core.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

function_registry_entry_t global_function_registry[MAX_GLOBAL_FUNCTIONS];
int global_function_count = 0;
pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

thread_data_snapshot_t* thread_snapshots[MAX_THREADS];
int thread_snapshot_count = 0;
pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;

__thread hash_table_t* functions = NULL;
__thread caller_counts_hash_t* caller_counts = NULL;
__thread pid_t current_thread_id = 0;
__thread call_stack_t call_stack = {.top = -1};

static struct itimerval timer;
static int profiling_enabled = 0;

/* Computes a stable bucket index input for string-keyed hash tables. */
static unsigned long hash_string(const char* str) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }

    return hash;
}

/* Allocates one empty function-statistics hash table. */
hash_table_t* create_hash_table(int capacity) {
    hash_table_t* ht = (hash_table_t*)malloc(sizeof(hash_table_t));

    if (!ht) {
        return NULL;
    }

    ht->capacity = capacity;
    ht->size = 0;
    ht->buckets = (hash_entry_t**)calloc(capacity, sizeof(hash_entry_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }

    return ht;
}

/* Allocates one empty caller->callee counter table. */
caller_hash_table_t* create_caller_hash_table(int capacity) {
    caller_hash_table_t* ht = (caller_hash_table_t*)malloc(sizeof(caller_hash_table_t));

    if (!ht) {
        return NULL;
    }

    ht->capacity = capacity;
    ht->size = 0;
    ht->buckets = (caller_entry_t**)calloc(capacity, sizeof(caller_entry_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }

    return ht;
}

/* Allocates the top-level map from caller name to callee table. */
caller_counts_hash_t* create_caller_counts_hash(void) {
    caller_counts_hash_t* ht = (caller_counts_hash_t*)malloc(sizeof(caller_counts_hash_t));

    if (!ht) {
        return NULL;
    }

    ht->capacity = 128;
    ht->size = 0;
    ht->buckets = (caller_map_entry_t**)calloc(ht->capacity, sizeof(caller_map_entry_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }

    return ht;
}

/* Finds one function entry by name in a hash table chain. */
function_info_t* hash_find(hash_table_t* ht, const char* key) {
    unsigned long hash;
    hash_entry_t* entry;

    if (!ht || !key) {
        return NULL;
    }

    hash = hash_string(key) % ht->capacity;
    entry = ht->buckets[hash];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return &entry->value;
        }
        entry = entry->next;
    }

    return NULL;
}

/* Inserts a new function entry when the name has not been seen yet. */
function_info_t* hash_insert(hash_table_t* ht, const char* key) {
    unsigned long hash;
    hash_entry_t* entry;
    function_info_t* existing;

    if (!ht || !key) {
        return NULL;
    }

    existing = hash_find(ht, key);
    if (existing) {
        return existing;
    }

    hash = hash_string(key) % ht->capacity;
    entry = (hash_entry_t*)malloc(sizeof(hash_entry_t));
    if (!entry) {
        return NULL;
    }

    strncpy(entry->key, key, 255);
    entry->key[255] = '\0';
    memset(&entry->value, 0, sizeof(function_info_t));
    strncpy(entry->value.name, key, 255);
    entry->value.name[255] = '\0';
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->size++;

    return &entry->value;
}

/* Finds the invocation counter for one callee under one caller. */
time_stamp* caller_hash_find(caller_hash_table_t* ht, const char* key) {
    unsigned long hash;
    caller_entry_t* entry;

    if (!ht || !key) {
        return NULL;
    }

    hash = hash_string(key) % ht->capacity;
    entry = ht->buckets[hash];

    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            return &entry->count;
        }
        entry = entry->next;
    }

    return NULL;
}

/* Inserts a new callee counter when a caller sees that callee for the first time. */
time_stamp* caller_hash_insert(caller_hash_table_t* ht, const char* key) {
    unsigned long hash;
    caller_entry_t* entry;
    time_stamp* existing;

    if (!ht || !key) {
        return NULL;
    }

    existing = caller_hash_find(ht, key);
    if (existing) {
        return existing;
    }

    hash = hash_string(key) % ht->capacity;
    entry = (caller_entry_t*)malloc(sizeof(caller_entry_t));
    if (!entry) {
        return NULL;
    }

    strncpy(entry->key, key, 255);
    entry->key[255] = '\0';
    entry->count = 0;
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->size++;

    return &entry->count;
}

/* Returns the callee table for a caller, creating it when needed. */
caller_hash_table_t* caller_counts_find_or_create(caller_counts_hash_t* ht, const char* caller) {
    unsigned long hash;
    caller_map_entry_t* entry;

    if (!ht || !caller) {
        return NULL;
    }

    hash = hash_string(caller) % ht->capacity;
    entry = ht->buckets[hash];

    while (entry) {
        if (strcmp(entry->key, caller) == 0) {
            return entry->callees;
        }
        entry = entry->next;
    }

    entry = (caller_map_entry_t*)malloc(sizeof(caller_map_entry_t));
    if (!entry) {
        return NULL;
    }

    strncpy(entry->key, caller, 255);
    entry->key[255] = '\0';
    entry->callees = create_caller_hash_table(64);
    entry->next = ht->buckets[hash];
    ht->buckets[hash] = entry;
    ht->size++;

    return entry->callees;
}

/* Releases one function hash table and all entries stored inside it. */
void free_hash_table(hash_table_t* ht) {
    int i;

    if (!ht) {
        return;
    }

    for (i = 0; i < ht->capacity; i++) {
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

/* Releases one caller->callee hash table and its entries. */
void free_caller_hash_table(caller_hash_table_t* ht) {
    int i;

    if (!ht) {
        return;
    }

    for (i = 0; i < ht->capacity; i++) {
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

/* Releases the nested caller-count structure used for call graph edges. */
void free_caller_counts_hash(caller_counts_hash_t* ht) {
    int i;

    if (!ht) {
        return;
    }

    for (i = 0; i < ht->capacity; i++) {
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

/* Copies one function hash table so finished threads can be reported later. */
static hash_table_t* deep_copy_hash_table(hash_table_t* src) {
    int i;
    hash_table_t* dst;

    if (!src) {
        return NULL;
    }

    dst = create_hash_table(src->capacity);
    if (!dst) {
        return NULL;
    }

    for (i = 0; i < src->capacity; i++) {
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

/* Copies one caller-count graph so finished threads can be reported later. */
static caller_counts_hash_t* deep_copy_caller_counts(caller_counts_hash_t* src) {
    int i;
    caller_counts_hash_t* dst;

    if (!src) {
        return NULL;
    }

    dst = create_caller_counts_hash();
    if (!dst) {
        return NULL;
    }

    for (i = 0; i < src->capacity; i++) {
        caller_map_entry_t* entry = src->buckets[i];
        while (entry) {
            caller_hash_table_t* dst_callees = caller_counts_find_or_create(dst, entry->key);
            if (dst_callees && entry->callees) {
                int j;
                for (j = 0; j < entry->callees->capacity; j++) {
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

/* Converts periodic SIGPROF samples into self-time on the current stack top. */
void profiling_handler(int sig) {
    static int init = 0;
    static struct timeval last_sample;
    struct timeval current_time;
    long interval_us;

    (void)sig;

    if (!profiling_enabled) {
        return;
    }

    gettimeofday(&current_time, NULL);

    if (!init) {
        last_sample = current_time;
        init = 1;
        return;
    }

    interval_us = (current_time.tv_sec - last_sample.tv_sec) * 1000000L
        + (current_time.tv_usec - last_sample.tv_usec);

    if (call_stack.top >= 0 && functions) {
        const char* current_func_name = call_stack.stack[call_stack.top];
        function_info_t* func = hash_find(functions, current_func_name);
        if (func) {
            func->self_time += interval_us;
        }
    }

    last_sample = current_time;
}

/* Installs the SIGPROF handler and prepares the repeating timer interval. */
void init_profiler(void) {
    struct sigaction sa;

    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = PROFILING_INTERVAL;
    timer.it_value = timer.it_interval;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = profiling_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
}

/* Enables sampling by arming the profiling interval timer. */
void start_profiling(void) {
    profiling_enabled = 1;
    setitimer(ITIMER_PROF, &timer, NULL);
}

/* Disables sampling by clearing the profiling interval timer. */
void stop_profiling(void) {
    profiling_enabled = 0;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    setitimer(ITIMER_PROF, &timer, NULL);
}

/* Registers a function name globally and lazily creates thread-local storage. */
const char* register_function(const char* name) {
    function_info_t* func;

    if (current_thread_id == 0) {
        current_thread_id = syscall(SYS_gettid);
    }

    if (!functions) {
        functions = create_hash_table(512);
    }
    if (!caller_counts) {
        caller_counts = create_caller_counts_hash();
    }

    pthread_mutex_lock(&registry_mutex);
    {
        int found = 0;
        int i;
        for (i = 0; i < global_function_count; i++) {
            if (strcmp(global_function_registry[i].name, name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found && global_function_count < MAX_GLOBAL_FUNCTIONS) {
            strncpy(global_function_registry[global_function_count].name, name, 255);
            global_function_registry[global_function_count].name[255] = '\0';
            global_function_registry[global_function_count].id = global_function_count;
            global_function_count++;
        }
    }
    pthread_mutex_unlock(&registry_mutex);

    func = hash_insert(functions, name);
    if (func && func->call_count == 0) {
        func->thread_id = current_thread_id;
        if (func->addr == NULL) {
            void* ret_addr = __builtin_return_address(0);
            Dl_info dl_info;
            if (dladdr(ret_addr, &dl_info) && dl_info.dli_saddr != NULL) {
                func->addr = dl_info.dli_saddr;
            }
        }
    }

    return name;
}

/* Records function entry timing and caller->callee edges for one call. */
void enter_function(const char* func_name) {
    function_info_t* func;

    if (!func_name || !functions) {
        return;
    }

    func = hash_find(functions, func_name);
    if (!func) {
        func = hash_insert(functions, func_name);
        if (!func) {
            return;
        }
        func->thread_id = current_thread_id;
    }

    func->call_count++;
    func->is_active = 1;

    clock_gettime(CLOCK_MONOTONIC, &func->start_wall_time);
#ifdef __linux__
    getrusage(RUSAGE_THREAD, &func->start_rusage);
#else
    getrusage(RUSAGE_SELF, &func->start_rusage);
#endif

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

    if (call_stack.top < MAX_CALL_STACK - 1) {
        strncpy(call_stack.stack[call_stack.top + 1], func_name, 255);
        call_stack.stack[call_stack.top + 1][255] = '\0';
        call_stack.top++;
    }
}

/* Records function exit timing and pops the matching stack frame. */
void leave_function(const char* func_name) {
    function_info_t* func;
    struct timespec end_wall_time;
    struct rusage end_rusage;
    long long wall_delta;
    long long user_delta;
    long long sys_delta;
    long long wait_delta;

    if (!func_name || !functions) {
        return;
    }

    func = hash_find(functions, func_name);
    if (!func) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_wall_time);
#ifdef __linux__
    getrusage(RUSAGE_THREAD, &end_rusage);
#else
    getrusage(RUSAGE_SELF, &end_rusage);
#endif

    wall_delta = (end_wall_time.tv_sec - func->start_wall_time.tv_sec) * 1000000LL
        + (end_wall_time.tv_nsec - func->start_wall_time.tv_nsec) / 1000;
    user_delta = (end_rusage.ru_utime.tv_sec - func->start_rusage.ru_utime.tv_sec) * 1000000LL
        + (end_rusage.ru_utime.tv_usec - func->start_rusage.ru_utime.tv_usec);
    sys_delta = (end_rusage.ru_stime.tv_sec - func->start_rusage.ru_stime.tv_sec) * 1000000LL
        + (end_rusage.ru_stime.tv_usec - func->start_rusage.ru_stime.tv_usec);
    wait_delta = wall_delta - (user_delta + sys_delta);
    if (wait_delta < 0) {
        wait_delta = 0;
    }

    func->total_time += wall_delta;
    func->user_time += user_delta;
    func->sys_time += sys_delta;
    func->wait_time += wait_delta;
    func->is_active = 0;

    if (call_stack.top >= 0 && strcmp(call_stack.stack[call_stack.top], func_name) == 0) {
        call_stack.top--;
    }
}

/* Deep-copies the current thread state into the global snapshot list. */
void register_thread_data(void) {
    pid_t tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);
    thread_data_snapshot_t* snapshot;

    if (!functions || functions->size == 0) {
        return;
    }

    pthread_mutex_lock(&snapshot_mutex);

    if (thread_snapshot_count >= MAX_THREADS) {
        pthread_mutex_unlock(&snapshot_mutex);
        fprintf(stderr, "Warning: Max threads exceeded, cannot register thread data\n");
        return;
    }

    snapshot = (thread_data_snapshot_t*)malloc(sizeof(thread_data_snapshot_t));
    if (!snapshot) {
        pthread_mutex_unlock(&snapshot_mutex);
        fprintf(stderr, "Error: Failed to allocate thread snapshot\n");
        return;
    }

    snapshot->thread_id = tid;
    snapshot->functions = deep_copy_hash_table(functions);
    snapshot->caller_counts = deep_copy_caller_counts(caller_counts);
    thread_snapshots[thread_snapshot_count++] = snapshot;

    pthread_mutex_unlock(&snapshot_mutex);
}

/* Builds a temporary merged function lookup table for cross-thread symbol reports. */
hash_table_t* build_merged_function_lookup(void) {
    hash_table_t* merged_ht = create_hash_table(512);

    if (!merged_ht) {
        return NULL;
    }

    pthread_mutex_lock(&snapshot_mutex);
    for (int t = 0; t < thread_snapshot_count; t++) {
        hash_table_t* ht = thread_snapshots[t]->functions;
        if (!ht) {
            continue;
        }
        for (int i = 0; i < ht->capacity; i++) {
            hash_entry_t* entry = ht->buckets[i];
            while (entry) {
                function_info_t* dst = hash_insert(merged_ht, entry->key);
                if (dst && dst->call_count == 0) {
                    dst->addr = entry->value.addr;
                    strncpy(dst->name, entry->value.name, 255);
                    dst->name[255] = '\0';
                    dst->call_count = 1;
                }
                entry = entry->next;
            }
        }
    }
    pthread_mutex_unlock(&snapshot_mutex);

    return merged_ht;
}

/* Frees all captured thread snapshots after reporting finishes. */
void cleanup_thread_snapshots(void) {
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

/* Frees the current thread's local tables so worker threads exit cleanly. */
void cleanup_current_thread_data(void) {
    if (functions) {
        free_hash_table(functions);
        functions = NULL;
    }
    if (caller_counts) {
        free_caller_counts_hash(caller_counts);
        caller_counts = NULL;
    }
}

/* Cleanup attribute target that automatically closes one profiling scope. */
void profile_cleanup_str(void* p) {
    const char* name = *(const char**)p;
    leave_function(name);
}
