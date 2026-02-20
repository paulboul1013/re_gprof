
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
#include <dlfcn.h>        // Phase 6: dladdr() for function address lookup
#include <stdint.h>       // Phase 6: uintptr_t
#include <elf.h>          // Phase 7: ELF format structures
#include <sys/mman.h>     // Phase 7: mmap for reading ELF file
#include <sys/stat.h>     // Phase 7: fstat for file size

// Phase 6: gmon.out format constants
#define GMON_MAGIC      "gmon"
#define GMON_VERSION    1
#define GMON_TAG_TIME_HIST  0
#define GMON_TAG_CG_ARC     1

// ============================================================
// Phase 7: ELF Symbol Resolution
// ============================================================

// One entry in our symbol table: start address, byte size, and name
typedef struct {
    uintptr_t addr;
    uintptr_t size;
    char name[256];
} elf_sym_t;

// Sorted array of function symbols loaded from ELF or System.map
typedef struct {
    elf_sym_t* entries;
    int count;
    int capacity;
} elf_sym_table_t;

static elf_sym_table_t* elf_sym_table_create(void) {
    elf_sym_table_t* t = (elf_sym_table_t*)malloc(sizeof(elf_sym_table_t));
    if (!t) return NULL;
    t->capacity = 256;
    t->count = 0;
    t->entries = (elf_sym_t*)malloc(t->capacity * sizeof(elf_sym_t));
    if (!t->entries) { free(t); return NULL; }
    return t;
}

static int elf_sym_table_add(elf_sym_table_t* t, uintptr_t addr, uintptr_t size, const char* name) {
    if (!t || !t->entries) return -1;
    if (t->count >= t->capacity) {
        int new_cap = t->capacity * 2;
        elf_sym_t* new_entries = (elf_sym_t*)realloc(t->entries, new_cap * sizeof(elf_sym_t));
        if (!new_entries) return -1;  // t->capacity unchanged, t->entries unchanged
        t->entries = new_entries;
        t->capacity = new_cap;
    }
    t->entries[t->count].addr = addr;
    t->entries[t->count].size = size;
    strncpy(t->entries[t->count].name, name, 255);
    t->entries[t->count].name[255] = '\0';
    t->count++;
    return 0;
}

static int elf_sym_cmp(const void* a, const void* b) {
    const elf_sym_t* sa = (const elf_sym_t*)a;
    const elf_sym_t* sb = (const elf_sym_t*)b;
    if (sa->addr < sb->addr) return -1;
    if (sa->addr > sb->addr) return 1;
    return 0;
}

static void elf_free_sym_table(elf_sym_table_t* t) {
    if (!t) return;
    free(t->entries);
    free(t);
}

// Phase 7: Load function symbols from ELF binary (.symtab section).
// Uses mmap to read the file, finds .symtab + linked .strtab,
// and returns a sorted elf_sym_table_t* (or NULL on failure).
static elf_sym_table_t* elf_load_symbols(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("elf_load_symbols: open");
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    void* base = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        perror("elf_load_symbols: mmap");
        return NULL;
    }

    // Validate ELF magic
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)base;
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        fprintf(stderr, "elf_load_symbols: not an ELF file: %s\n", path);
        munmap(base, file_size);
        return NULL;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "elf_load_symbols: only 64-bit ELF supported\n");
        munmap(base, file_size);
        return NULL;
    }

    // Get section header table
    Elf64_Shdr* shdrs = (Elf64_Shdr*)((char*)base + ehdr->e_shoff);

    // Get .shstrtab (section name string table)
    if (ehdr->e_shstrndx == SHN_UNDEF || ehdr->e_shstrndx >= ehdr->e_shnum) {
        fprintf(stderr, "elf_load_symbols: invalid section name table index\n");
        munmap(base, file_size);
        return NULL;
    }
    Elf64_Shdr* shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
    const char* shstrtab = (const char*)base + shstrtab_hdr->sh_offset;

    // Find .symtab and its associated .strtab (via sh_link)
    Elf64_Shdr* symtab_hdr = NULL;
    Elf64_Shdr* strtab_hdr = NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char* sec_name = shstrtab + shdrs[i].sh_name;
        if (shdrs[i].sh_type == SHT_SYMTAB && strcmp(sec_name, ".symtab") == 0) {
            symtab_hdr = &shdrs[i];
            if (shdrs[i].sh_link < (unsigned)ehdr->e_shnum) {
                strtab_hdr = &shdrs[shdrs[i].sh_link];
            }
        }
    }

    if (!symtab_hdr || !strtab_hdr) {
        fprintf(stderr, "elf_load_symbols: no .symtab found in %s (stripped?)\n", path);
        munmap(base, file_size);
        return NULL;
    }

    // Read function symbols
    Elf64_Sym* syms = (Elf64_Sym*)((char*)base + symtab_hdr->sh_offset);
    int num_syms = (int)(symtab_hdr->sh_size / sizeof(Elf64_Sym));
    const char* strtab = (const char*)base + strtab_hdr->sh_offset;

    elf_sym_table_t* table = elf_sym_table_create();
    if (!table) {
        munmap(base, file_size);
        return NULL;
    }

    for (int i = 0; i < num_syms; i++) {
        if (ELF64_ST_TYPE(syms[i].st_info) == STT_FUNC && syms[i].st_value != 0) {
            const char* sym_name = strtab + syms[i].st_name;
            elf_sym_table_add(table, (uintptr_t)syms[i].st_value,
                              (uintptr_t)syms[i].st_size, sym_name);
        }
    }

    munmap(base, file_size);

    // Sort by address for binary search
    qsort(table->entries, table->count, sizeof(elf_sym_t), elf_sym_cmp);

    printf("[ELF] Loaded %d function symbols from %s\n", table->count, path);
    return table;
}

// Phase 7: Resolve an address to the nearest function symbol using binary search.
// Returns the symbol entry whose address is <= query_addr and query_addr < addr+size.
// If the symbol has size 0 (unknown), returns the closest symbol with addr <= query_addr.
// Returns NULL if no matching symbol found.
static const elf_sym_t* elf_resolve_addr(const elf_sym_table_t* table, uintptr_t query_addr) {
    if (!table || table->count == 0) return NULL;

    int lo = 0, hi = table->count - 1;
    const elf_sym_t* best = NULL;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (table->entries[mid].addr <= query_addr) {
            best = &table->entries[mid];
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (!best) return NULL;

    // If symbol has known size, verify addr is within range
    if (best->size > 0 && query_addr >= best->addr + best->size) {
        return NULL;  // Address is past this symbol's end
    }

    return best;
}

// Phase 7: Load symbols from System.map format file.
// Format: "<hex_addr> <type> <name>" one per line.
// Only loads type 'T' or 't' (text/code section symbols).
// Returns a sorted elf_sym_table_t*, or NULL on failure.
static elf_sym_table_t* sysmap_load_symbols(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        perror("sysmap_load_symbols: fopen");
        return NULL;
    }

    elf_sym_table_t* table = elf_sym_table_create();
    if (!table) { fclose(fp); return NULL; }

    char line[512];
    int loaded = 0;
    while (fgets(line, sizeof(line), fp)) {
        char addr_str[32], type_str[4], name[256];
        if (sscanf(line, "%31s %3s %255s", addr_str, type_str, name) != 3) continue;

        // Only text (code) symbols: T (global) or t (local)
        if (type_str[0] != 'T' && type_str[0] != 't') continue;

        char* endptr = NULL;
        uintptr_t addr = (uintptr_t)strtoull(addr_str, &endptr, 16);
        if (endptr == addr_str || addr == 0) continue;  // parse error or zero address

        if (elf_sym_table_add(table, addr, 0, name) == 0) {
            loaded++;
        }
    }
    fclose(fp);

    // Sort by address for binary search
    if (table->count > 0) {
        qsort(table->entries, table->count, sizeof(elf_sym_t), elf_sym_cmp);
    }

    printf("[ELF] Loaded %d symbols from System.map: %s\n", loaded, path);
    return table;
}

#define MAX_FUNCTIONS 1000
#define MAX_CALL_STACK 100
#define PROFILING_INTERVAL 10000 //sample interval 10ms

typedef unsigned long long time_stamp;

typedef struct {
    char name[256];
    void* addr;                 // Phase 6: Function start address (from dladdr)
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

// Phase 7: Global ELF symbol table (loaded after profiling, freed after report)
static elf_sym_table_t* g_sym_table = NULL;


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

        // Phase 6: Capture function start address using __builtin_return_address
        // __builtin_return_address(0) points into the calling function's body,
        // allowing dladdr() to resolve the symbol's start address.
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
// Phase 6: gmon.out Binary Format Export
// ============================================================

// Write gmon.out for current data.
// use_merged=0: single-threaded (uses TLS functions/caller_counts)
// use_merged=1: multi-threaded merged (uses thread_snapshots)
void export_gmon_out(const char* filename, int use_merged) {
    // -------- Collect function data --------
    // We aggregate all functions into a temporary list for address scan
    // and histogram building.

    // Build a flat array of (addr, self_time_us) for histogram
    // and iterate caller_counts for arc records.

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        perror("export_gmon_out: fopen");
        return;
    }

    // -------- Step 1: Write header (20 bytes) --------
    // magic: "gmon" (4 bytes)
    fwrite(GMON_MAGIC, 4, 1, fp);
    // version: 1 (4 bytes, little-endian native)
    uint32_t version = GMON_VERSION;
    fwrite(&version, sizeof(uint32_t), 1, fp);
    // spare: 12 bytes of zeros
    char spare[12] = {0};
    fwrite(spare, 12, 1, fp);

    // -------- Step 2: Find PC range and collect function info --------
    uintptr_t low_pc = UINTPTR_MAX;
    uintptr_t high_pc = 0;

    // Collect addresses from appropriate source
    if (use_merged && thread_snapshot_count > 0) {
        pthread_mutex_lock(&snapshot_mutex);
        for (int t = 0; t < thread_snapshot_count; t++) {
            hash_table_t* ht = thread_snapshots[t]->functions;
            if (!ht) continue;
            for (int i = 0; i < ht->capacity; i++) {
                hash_entry_t* entry = ht->buckets[i];
                while (entry) {
                    if (entry->value.addr) {
                        uintptr_t a = (uintptr_t)entry->value.addr;
                        if (a < low_pc) low_pc = a;
                        if (a > high_pc) high_pc = a;
                    }
                    entry = entry->next;
                }
            }
        }
        pthread_mutex_unlock(&snapshot_mutex);
    } else if (!use_merged && functions) {
        for (int i = 0; i < functions->capacity; i++) {
            hash_entry_t* entry = functions->buckets[i];
            while (entry) {
                if (entry->value.addr) {
                    uintptr_t a = (uintptr_t)entry->value.addr;
                    if (a < low_pc) low_pc = a;
                    if (a > high_pc) high_pc = a;
                }
                entry = entry->next;
            }
        }
    }

    if (low_pc == UINTPTR_MAX || high_pc == 0 || high_pc <= low_pc) {
        // No valid addresses: skip histogram, write empty file
        fprintf(stderr, "export_gmon_out: no valid function addresses found\n");
        fclose(fp);
        return;
    }

    // Add padding at end of address range to cover last function's body
    high_pc += 0x1000;

    // -------- Step 3: Build histogram --------
    // Standard gmon uses HISTFRACTION = 2 bytes per bin
    const int BIN_BYTES = 2;
    const uint32_t PROF_RATE = 100;  // 100 Hz = 10ms interval
    uintptr_t addr_range = high_pc - low_pc;

    // Cap number of bins to avoid excessive memory
    int num_bins = (int)(addr_range / BIN_BYTES);
    if (num_bins > 65536) {
        num_bins = 65536;
    }
    if (num_bins <= 0) num_bins = 1;

    // NOTE: gmon.out stores hist_size as the COUNT of bins, not byte size
    uint32_t hist_size = (uint32_t)num_bins;

    uint16_t* hist = (uint16_t*)calloc(num_bins, sizeof(uint16_t));
    if (!hist) {
        fprintf(stderr, "export_gmon_out: out of memory for histogram\n");
        fclose(fp);
        return;
    }

    // Distribute self_time samples across histogram bins
    // Each sample = PROFILING_INTERVAL us = 10000 us
    double actual_bin_bytes = (double)addr_range / num_bins;

    if (use_merged && thread_snapshot_count > 0) {
        pthread_mutex_lock(&snapshot_mutex);
        for (int t = 0; t < thread_snapshot_count; t++) {
            hash_table_t* ht = thread_snapshots[t]->functions;
            if (!ht) continue;
            for (int i = 0; i < ht->capacity; i++) {
                hash_entry_t* entry = ht->buckets[i];
                while (entry) {
                    if (entry->value.addr && entry->value.self_time > 0) {
                        uintptr_t a = (uintptr_t)entry->value.addr;
                        int bin = (int)((a - low_pc) / actual_bin_bytes);
                        if (bin < 0) bin = 0;
                        if (bin >= num_bins) bin = num_bins - 1;
                        // Convert self_time (microseconds) to sample count
                        long long samples = (long long)(entry->value.self_time / PROFILING_INTERVAL);
                        if (samples > 65535) samples = 65535;
                        hist[bin] = (uint16_t)(hist[bin] + samples > 65535 ? 65535 : hist[bin] + samples);
                    }
                    entry = entry->next;
                }
            }
        }
        pthread_mutex_unlock(&snapshot_mutex);
    } else if (!use_merged && functions) {
        for (int i = 0; i < functions->capacity; i++) {
            hash_entry_t* entry = functions->buckets[i];
            while (entry) {
                if (entry->value.addr && entry->value.self_time > 0) {
                    uintptr_t a = (uintptr_t)entry->value.addr;
                    int bin = (int)((a - low_pc) / actual_bin_bytes);
                    if (bin < 0) bin = 0;
                    if (bin >= num_bins) bin = num_bins - 1;
                    long long samples = (long long)(entry->value.self_time / PROFILING_INTERVAL);
                    if (samples > 65535) samples = 65535;
                    hist[bin] = (uint16_t)(hist[bin] + samples > 65535 ? 65535 : hist[bin] + samples);
                }
                entry = entry->next;
            }
        }
    }

    // -------- Step 4: Write histogram record --------
    uint8_t tag = GMON_TAG_TIME_HIST;
    fwrite(&tag, 1, 1, fp);

    // low_pc and high_pc (pointer-size, platform native)
    fwrite(&low_pc, sizeof(uintptr_t), 1, fp);
    fwrite(&high_pc, sizeof(uintptr_t), 1, fp);

    // hist_size (bytes of histogram data)
    fwrite(&hist_size, sizeof(uint32_t), 1, fp);

    // prof_rate (samples per second)
    fwrite(&PROF_RATE, sizeof(uint32_t), 1, fp);

    // dimen: "seconds" padded to 15 bytes
    char dimen[15] = "seconds        ";
    fwrite(dimen, 15, 1, fp);

    // dimen_abbrev: 's'
    char abbrev = 's';
    fwrite(&abbrev, 1, 1, fp);

    // Histogram bin data
    fwrite(hist, sizeof(uint16_t), num_bins, fp);
    free(hist);

    // -------- Step 5: Write call graph arc records --------
    // helper lambda-style: write one arc
    // For each caller -> callee pair with both valid addresses
    tag = GMON_TAG_CG_ARC;

    if (use_merged && thread_snapshot_count > 0) {
        pthread_mutex_lock(&snapshot_mutex);
        for (int t = 0; t < thread_snapshot_count; t++) {
            caller_counts_hash_t* cc = thread_snapshots[t]->caller_counts;
            hash_table_t* ht = thread_snapshots[t]->functions;
            if (!cc || !ht) continue;

            for (int i = 0; i < cc->capacity; i++) {
                caller_map_entry_t* cme = cc->buckets[i];
                while (cme) {
                    function_info_t* caller_fi = hash_find(ht, cme->key);
                    void* from_pc = caller_fi ? caller_fi->addr : NULL;

                    if (from_pc && cme->callees) {
                        for (int j = 0; j < cme->callees->capacity; j++) {
                            caller_entry_t* ce = cme->callees->buckets[j];
                            while (ce) {
                                function_info_t* callee_fi = hash_find(ht, ce->key);
                                void* self_pc = callee_fi ? callee_fi->addr : NULL;

                                if (self_pc && ce->count > 0) {
                                    fwrite(&tag, 1, 1, fp);
                                    fwrite(&from_pc, sizeof(void*), 1, fp);
                                    fwrite(&self_pc, sizeof(void*), 1, fp);
                                    uint32_t cnt = (ce->count > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)ce->count;
                                    fwrite(&cnt, sizeof(uint32_t), 1, fp);
                                }
                                ce = ce->next;
                            }
                        }
                    }
                    cme = cme->next;
                }
            }
        }
        pthread_mutex_unlock(&snapshot_mutex);
    } else if (!use_merged && caller_counts && functions) {
        for (int i = 0; i < caller_counts->capacity; i++) {
            caller_map_entry_t* cme = caller_counts->buckets[i];
            while (cme) {
                function_info_t* caller_fi = hash_find(functions, cme->key);
                void* from_pc = caller_fi ? caller_fi->addr : NULL;

                if (from_pc && cme->callees) {
                    for (int j = 0; j < cme->callees->capacity; j++) {
                        caller_entry_t* ce = cme->callees->buckets[j];
                        while (ce) {
                            function_info_t* callee_fi = hash_find(functions, ce->key);
                            void* self_pc = callee_fi ? callee_fi->addr : NULL;

                            if (self_pc && ce->count > 0) {
                                fwrite(&tag, 1, 1, fp);
                                fwrite(&from_pc, sizeof(void*), 1, fp);
                                fwrite(&self_pc, sizeof(void*), 1, fp);
                                uint32_t cnt = (ce->count > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)ce->count;
                                fwrite(&cnt, sizeof(uint32_t), 1, fp);
                            }
                            ce = ce->next;
                        }
                    }
                }
                cme = cme->next;
            }
        }
    }

    fclose(fp);
    printf("gmon.out exported to %s\n", filename);
    printf("Analyze with: gprof ./main %s\n", filename);
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

// Phase 7: Print ELF symbol resolution report.
// Compares profiler-captured function addresses against ELF/System.map symbols.
static void print_elf_symbol_report(elf_sym_table_t* sym_table, hash_table_t* ht) {
    printf("\n================================================================================\n");
    printf("ELF Symbol Resolution Report (Phase 7)\n");
    printf("================================================================================\n");
    printf("%-40s %-18s %-18s %s\n",
           "Function (profiler)", "Profiler Addr", "ELF Addr", "Match?");
    printf("%-40s %-18s %-18s %s\n",
           "----------------------------------------",
           "------------------", "------------------", "-------");

    if (!ht) {
        printf("(no profiling data)\n");
        return;
    }

    int matched = 0, total = 0;
    for (int i = 0; i < ht->capacity; i++) {
        hash_entry_t* entry = ht->buckets[i];
        while (entry) {
            function_info_t* func = &entry->value;
            total++;

            const char* match_str = "-";
            const char* elf_name = "(not found)";
            char elf_addr_str[32] = "(none)";

            if (func->addr && sym_table) {
                const elf_sym_t* sym = elf_resolve_addr(sym_table, (uintptr_t)func->addr);
                if (sym) {
                    snprintf(elf_addr_str, sizeof(elf_addr_str), "0x%016lx", (unsigned long)sym->addr);
                    elf_name = sym->name;
                    if (strcmp(sym->name, func->name) == 0) {
                        match_str = "OK";
                        matched++;
                    } else {
                        match_str = "MISMATCH";
                    }
                }
            }

            printf("%-40s 0x%016lx %-18s %s (%s)\n",
                   func->name,
                   func->addr ? (unsigned long)(uintptr_t)func->addr : 0UL,
                   elf_addr_str,
                   match_str,
                   elf_name);

            entry = entry->next;
        }
    }

    printf("\nSummary: %d/%d functions matched ELF symbols\n", matched, total);

    // Print all ELF symbols (sorted by address)
    if (sym_table && sym_table->count > 0) {
        printf("\n--- All ELF Function Symbols (%d total) ---\n", sym_table->count);
        printf("%-18s %-10s %s\n", "Address", "Size", "Name");
        for (int i = 0; i < sym_table->count; i++) {
            printf("0x%016lx %-10lu %s\n",
                   (unsigned long)sym_table->entries[i].addr,
                   (unsigned long)sym_table->entries[i].size,
                   sym_table->entries[i].name);
        }
    }
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
    int export_dot = 0;   // Phase 8: Export call graph to DOT format
    int export_gmon = 0;  // Phase 6: Export gmon.out binary file
    char resolve_symbols_path[512] = "";  // Phase 7: path to ELF or System.map
    int use_sysmap = 0;                   // Phase 7: use System.map format instead of ELF
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
        } else if (strcmp(argv[i], "--export-gmon") == 0) {
            export_gmon = 1;  // Phase 6: Enable gmon.out export
        } else if (strncmp(argv[i], "--resolve-symbols=", 18) == 0) {
            strncpy(resolve_symbols_path, argv[i] + 18, 511);
            resolve_symbols_path[511] = '\0';
        } else if (strcmp(argv[i], "--resolve-symbols") == 0) {
            strncpy(resolve_symbols_path, "/proc/self/exe", 511);
        } else if (strcmp(argv[i], "--sysmap") == 0) {
            use_sysmap = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("\nUsage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --multi-threaded         Run multi-threaded tests\n");
            printf("  --shared-test            Run shared function test (multiple threads call same functions)\n");
            printf("  --report-mode=MODE       Report mode: per-thread, merged, or both (default: per-thread)\n");
            printf("  --export-dot             Export call graph to Graphviz DOT format (Phase 8)\n");
            printf("  --dot-mode=MODE          DOT export mode: per-thread or merged (default: merged)\n");
            printf("  --export-gmon            Export gmon.out binary file for gprof analysis (Phase 6)\n");
            printf("  --resolve-symbols        Resolve addresses via ELF .symtab (Phase 7)\n");
            printf("  --resolve-symbols=PATH   Use specified ELF file or System.map\n");
            printf("  --sysmap                 Treat --resolve-symbols path as System.map format\n");
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

    // Phase 7: Load ELF/System.map symbols if requested
    if (resolve_symbols_path[0] != '\0') {
        if (use_sysmap) {
            g_sym_table = sysmap_load_symbols(resolve_symbols_path);
        } else {
            g_sym_table = elf_load_symbols(resolve_symbols_path);
        }
    }

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

        // Phase 6: Export gmon.out (merged mode for multi-threaded)
        if (export_gmon) {
            printf("\n");
            printf("================================================================================\n");
            printf("Exporting gmon.out (Phase 6) - merged mode\n");
            printf("================================================================================\n");
            export_gmon_out("gmon.out", 1);
        }

        // Phase 7: ELF symbol resolution report
        if (g_sym_table) {
            print_elf_symbol_report(g_sym_table, functions);
            elf_free_sym_table(g_sym_table);
            g_sym_table = NULL;
        }

        cleanup_thread_snapshots();
    } else {
        // Single-threaded: report already printed inside run_single_threaded_tests()
        // Phase 7: ELF symbol resolution report
        if (g_sym_table) {
            print_elf_symbol_report(g_sym_table, functions);
            elf_free_sym_table(g_sym_table);
            g_sym_table = NULL;
        }

        // Phase 6: Export gmon.out using current thread's TLS data
        if (export_gmon) {
            printf("\n");
            printf("================================================================================\n");
            printf("Exporting gmon.out (Phase 6) - single-threaded mode\n");
            printf("================================================================================\n");
            export_gmon_out("gmon.out", 0);
        }
    }

    printf("\nProfiling stopped.\n");

    // Phase 5: Cleanup main thread's TLS memory
    cleanup_thread_local();

    return 0;
}