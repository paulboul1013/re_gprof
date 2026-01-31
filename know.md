# 專案知識庫 (Knowledge Base)

這份文件記錄 simple_gprof 專案各階段的詳細技術知識點,作為學習和參考資料。

---

## Phase 0: 基礎原型 (已完成)

**完成日期**: 2026-01-25

### 1. GCC Cleanup 屬性

#### 什麼是 Cleanup 屬性?

GCC 提供的擴展功能,讓變數在離開作用域時自動呼叫指定的清理函數。

```c
void cleanup_function(int *p) {
    printf("Cleaning up, value = %d\n", *p);
}

void example() {
    __attribute__((cleanup(cleanup_function))) int x = 42;
    // 當 x 離開作用域時,自動呼叫 cleanup_function(&x)
}
```

#### 在本專案中的應用

```c
static inline void __profile_cleanup_int(void *p) {
    int id = *(int *)p;
    leave_function(id);  // 自動呼叫 leave_function
}

#define PROFILE_FUNCTION()                                                     \
    static int __func_id = -1;                                                 \
    if (__func_id == -1) {                                                     \
        __func_id = register_function(__func__);                               \
    }                                                                          \
    enter_function(__func_id);                                                 \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard = __func_id;
```

**關鍵點**:
- `__profile_scope_guard` 這個變數在函數任何出口點 (return, 正常結束) 都會觸發 cleanup
- 確保 `enter_function()` 和 `leave_function()` 成對執行
- 避免了手動在每個 return 前呼叫 `leave_function()`

**優點**:
- 自動化的資源管理
- 支援提前 return 和異常路徑
- 減少人為錯誤

**限制**:
- GCC 專有擴展,不符合標準 C
- Clang 也支援,但 MSVC 不支援

---

### 2. SIGPROF 信號與 ITIMER_PROF

#### 什麼是 SIGPROF?

`SIGPROF` 是 UNIX 系統提供的 profiling 專用信號,配合 `ITIMER_PROF` 計時器使用。

#### ITIMER_PROF vs ITIMER_REAL vs ITIMER_VIRTUAL

| 計時器類型 | 測量時間 | 使用情境 |
|-----------|---------|---------|
| ITIMER_REAL | Wall clock time (實際經過時間) | 需要包含 I/O 等待時間 |
| ITIMER_VIRTUAL | User mode CPU time | 只測量使用者態執行時間 |
| ITIMER_PROF | User + Kernel CPU time | **Profiling 的標準選擇** |

**本專案使用 ITIMER_PROF**,因為:
- 包含 user mode 和 kernel mode 的 CPU 時間
- 不會在程式等待 I/O 時觸發 (避免誤算)
- 當程式被暫停 (debugger, sleep) 時不計時

#### 實作細節

```c
void init_profilier() {
    // 設定計時器間隔為 10ms
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = PROFILING_INTERVAL;  // 10000 us
    timer.it_value = timer.it_interval;

    // 註冊信號處理器
    struct sigaction sa;
    sa.sa_handler = profiling_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPROF, &sa, NULL);
}

void start_profiling() {
    profiling_enabled = 1;
    setitimer(ITIMER_PROF, &timer, NULL);  // 啟動計時器
}
```

**採樣頻率選擇**:
- 10ms (100 Hz) 是傳統 profiler 的標準頻率
- 太高 (1ms): overhead 過大,干擾程式執行
- 太低 (100ms): 短函數可能完全採樣不到

---

### 3. Signal Handler 中的安全性

#### Async-Signal-Safe 函數

在信號處理器中,只能呼叫 **async-signal-safe** 的函數,否則可能造成 deadlock 或資料損壞。

**不安全的函數** (絕對不能在 signal handler 中使用):
- `printf()`, `fprintf()`, `sprintf()`: 可能使用內部 buffer 和 lock
- `malloc()`, `free()`: heap 操作不是 reentrant
- `pthread_mutex_lock()`: 可能造成 deadlock

**安全的操作**:
- 簡單的整數運算
- 存取 `volatile` 變數
- 呼叫 `write()` (直接系統呼叫)

#### 本專案的實作

```c
void profiling_handler(int sig) {
    if (!profiling_enabled) return;

    // ✅ 安全: 只做簡單的時間計算和整數累加
    gettimeofday(&current_time, NULL);
    long interval_us = (current_time.tv_sec - last_sample.tv_sec) * 1000000 +
                       (current_time.tv_usec - last_sample.tv_usec);

    if (call_stack.top >= 0) {
        int current_func = call_stack.stack[call_stack.top];
        functions[current_func].self_time += interval_us;  // ✅ 安全
    }

    last_sample = current_time;
}
```

**為什麼 `gettimeofday()` 安全?**
- 它是系統呼叫,不使用任何全域 lock
- POSIX 標準明確列為 async-signal-safe

---

### 4. Self Time vs Total Time 的區別

這是 profiling 中最容易混淆的概念。

#### Total Time (總時間)

函數從進入到離開的**所有時間**,包含:
- 函數自身的執行時間
- 呼叫的子函數的執行時間

**測量方式**: Instrumentation (插樁)
```c
void enter_function(int func_id) {
    gettimeofday(&functions[func_id].start_time, NULL);
}

void leave_function(int func_id) {
    gettimeofday(&end_time, NULL);
    long execute_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 + ...;
    functions[func_id].total_time += execute_time;  // 包含子函數時間
}
```

#### Self Time (自身時間)

函數**自己**執行的時間,**不包含**子函數的時間。

**測量方式**: Sampling (採樣)
```c
void profiling_handler(int sig) {
    // 只累加到 call stack 頂端的函數
    int current_func = call_stack.stack[call_stack.top];
    functions[current_func].self_time += interval_us;
}
```

#### 範例說明

假設有以下呼叫:
```
main() {
    function_a();  // 執行 100ms
    function_b();  // 執行 200ms
    // main 自己的工作: 50ms
}
```

結果:
- `main` 的 **total_time** = 350ms (100 + 200 + 50)
- `main` 的 **self_time** = 50ms (只有自己的工作)
- `function_a` 的 **total_time** = 100ms
- `function_a` 的 **self_time** = 100ms (沒有子函數)

#### 為什麼需要兩者?

- **Total Time**: 找出「最耗時的呼叫路徑」
- **Self Time**: 找出「真正的熱點」(應該優化的函數)

如果 `main` 的 total_time 很高但 self_time 很低,代表問題在子函數,不是 main 本身。

---

### 5. Call Stack 維護

#### 為什麼需要 Call Stack?

採樣時,我們需要知道「目前正在執行哪個函數」,才能正確歸屬 self_time。

#### 實作方式

```c
typedef struct {
    int stack[MAX_CALL_STACK];
    int top;  // 堆疊頂端索引, -1 表示空堆疊
} call_stack_t;

static call_stack_t call_stack = {.top = -1};
```

**Push** (進入函數):
```c
void enter_function(int func_id) {
    if (call_stack.top < MAX_CALL_STACK - 1) {
        call_stack.stack[++call_stack.top] = func_id;
    }
}
```

**Pop** (離開函數):
```c
void leave_function(int func_id) {
    if (call_stack.top >= 0 && call_stack.stack[call_stack.top] == func_id) {
        call_stack.top--;
    }
}
```

**採樣時讀取頂端**:
```c
if (call_stack.top >= 0) {
    int current_func = call_stack.stack[call_stack.top];
    functions[current_func].self_time += interval_us;
}
```

#### 為什麼不使用系統 Call Stack?

可以使用 `backtrace()` 取得系統 call stack,但:
- **overhead 很高**: 每次採樣都要 unwind stack
- **在 signal handler 中不安全**: `backtrace()` 不是 async-signal-safe
- **手動維護更快**: 只需要整數操作

---

### 6. Static Local Variable 的妙用

#### 函數 ID 的懶初始化

```c
#define PROFILE_FUNCTION()                                                     \
    static int __func_id = -1;                                                 \
    if (__func_id == -1) {                                                     \
        __func_id = register_function(__func__);                               \
    }                                                                          \
    enter_function(__func_id);
```

**為什麼使用 `static`?**
- `static` 變數在整個程式執行期間只初始化一次
- 每個函數有自己獨立的 `__func_id` 變數
- 第一次執行時註冊函數,之後直接使用已註冊的 ID

**如果不用 static?**
```c
// ❌ 錯誤示範
int __func_id = -1;  // 每次呼叫都重新初始化為 -1
if (__func_id == -1) {
    __func_id = register_function(__func__);  // 每次都重複註冊!
}
```

#### __func__ 魔法變數

`__func__` 是 C99 標準定義的預定義識別符,自動展開為目前函數名稱的字串。

```c
void my_function() {
    printf("%s\n", __func__);  // 輸出: my_function
}
```

類似的還有:
- `__FILE__`: 目前檔案名稱
- `__LINE__`: 目前行號
- `__FUNCTION__`: GCC 擴展,功能同 `__func__`

---

### 7. Caller-Callee 關係追蹤

#### 二維陣列表示 Call Graph

```c
static time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
```

- `caller_counts[A][B]` = 函數 A 呼叫函數 B 的次數
- 這是一個稀疏矩陣 (大部分元素為 0)

#### 記錄呼叫關係

```c
void enter_function(int func_id) {
    // ...
    if (call_stack.top >= 0) {
        int caller_id = call_stack.stack[call_stack.top];  // 堆疊頂端 = 呼叫者
        if (caller_id >= 0 && caller_id < function_count) {
            caller_counts[caller_id][func_id]++;  // 記錄 caller -> callee
        }
    }
    // 然後 push 自己到 stack
    call_stack.stack[++call_stack.top] = func_id;
}
```

**關鍵**: 在 push 之前,堆疊頂端就是呼叫者 (caller)

#### 空間複雜度問題

- O(n²) 空間複雜度: 1000 個函數 = 1M 個 uint64
- 大約 8 MB 記憶體 (假設 `time_stamp` 為 8 bytes)
- **Phase 5 將改用 Hash Table** 解決此問題

---

### 8. 時間測量精度

#### gettimeofday() 特性

```c
struct timeval {
    time_t      tv_sec;   // 秒
    suseconds_t tv_usec;  // 微秒 (0-999999)
};
```

- **精度**: 微秒級 (1 µs = 0.000001 秒)
- **實際解析度**: 取決於系統,通常為 1-10 µs
- **overhead**: 約 20-50 ns per call (很快)

#### 時間差計算

```c
long execute_time = (end_time.tv_sec - start_time.tv_sec) * 1000000 +
                    (end_time.tv_usec - start_time.tv_usec);
```

**注意**: 先算秒數差 × 1000000,再加微秒差,避免溢位

#### 為什麼不用 clock()?

```c
clock_t clock(void);  // 返回 CPU clock ticks
```

- `clock()` 在多執行緒下會累加所有執行緒的時間
- 不適合作為 wall time 測量
- 精度較低 (通常為 10ms)

#### 更高精度選項 (Phase 1 將使用)

```c
struct timespec {
    time_t tv_sec;   // 秒
    long   tv_nsec;  // 奈秒 (0-999999999)
};

clock_gettime(CLOCK_MONOTONIC, &ts);  // 奈秒精度
```

---

### 9. 效能分析中的取捨

#### Instrumentation vs Sampling

| 方法 | 優點 | 缺點 | 適用場景 |
|------|------|------|----------|
| **Instrumentation** | 精確的呼叫次數<br>完整的 call graph<br>可追蹤每次呼叫 | Overhead 較高 (5-20%)<br>需要重新編譯<br>程式碼膨脹 | 函數級別分析<br>call graph 建構 |
| **Sampling** | Overhead 很低 (<1%)<br>不需重新編譯<br>可分析執行中程式 | 統計誤差<br>短函數可能漏掉<br>無法得知確切呼叫次數 | 找出 hot spots<br>長時間執行程式 |

**本專案採用混合式**:
- 用 Instrumentation 測量 total_time 和 call_count
- 用 Sampling 測量 self_time

這樣能兼具精確性和低 overhead。

---

### 10. 編譯器最佳化的影響

#### Volatile 關鍵字的必要性

```c
void function_a() {
    PROFILE_FUNCTION();
    for (volatile int i = 0; i < 1000000; i++);  // ✅ 防止被優化掉
}
```

**如果不加 `volatile`?**
```c
for (int i = 0; i < 1000000; i++);  // ❌ 編譯器會優化掉整個迴圈!
```

使用 `-O2` 或 `-O3` 優化時,編譯器發現迴圈沒有副作用,會直接刪除。

#### -pg 旗標 (傳統 gprof)

```bash
gcc -pg -o program program.c
```

`-pg` 會讓編譯器:
1. 在每個函數開頭插入 `mcount()` 呼叫
2. 連結 `gmon.out` 產生程式碼
3. 程式結束時自動寫入 profiling 資料

**本專案不使用 `-pg`**,而是用自己的 `PROFILE_FUNCTION()` 巨集,更靈活且可控。

---

## 延伸閱讀

### 相關工具比較

| 工具 | 類型 | 優點 | 缺點 |
|------|------|------|------|
| gprof | Sampling + Instrumentation | 標準工具,易用 | 多執行緒支援差<br>精度低 |
| perf | Sampling (硬體計數器) | 超低 overhead<br>硬體級精度 | 需要 root 權限<br>Linux only |
| Valgrind Callgrind | Instrumentation (模擬) | 超精確<br>無需重新編譯 | 極高 overhead (10-100x)<br>執行超慢 |
| gperftools | Sampling | 低 overhead<br>支援 heap profiling | 設定複雜 |

### 推薦資源

1. **gprof 原理**: https://ftp.gnu.org/old-gnu/Manuals/gprof-2.9.1/html_mono/gprof.html
2. **Signal Safety**: man 7 signal-safety
3. **POSIX Timers**: man 2 getitimer, man 2 timer_create
4. **GCC Attributes**: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html

---

## Phase 1: User/System Time 分離 (已完成)

**完成日期**: 2026-01-25

### 1. getrusage() 系統呼叫

#### 什麼是 getrusage()?

`getrusage()` 是 POSIX 提供的系統呼叫,用於取得程序或執行緒的資源使用統計。

```c
#include <sys/resource.h>

int getrusage(int who, struct rusage *usage);
```

**參數**:
- `who`: 指定要查詢的對象
  - `RUSAGE_SELF`: 目前程序(包含所有執行緒的總和)
  - `RUSAGE_CHILDREN`: 所有已結束的子程序
  - `RUSAGE_THREAD`: 目前執行緒 (需要 `_GNU_SOURCE`,Linux 特有)

**返回值**:
- 成功返回 0
- 失敗返回 -1 並設定 errno

#### struct rusage 結構

```c
struct rusage {
    struct timeval ru_utime;    // User CPU time used (使用者態時間)
    struct timeval ru_stime;    // System CPU time used (核心態時間)
    long   ru_maxrss;           // Maximum resident set size (最大記憶體使用,KB)
    long   ru_ixrss;            // Integral shared memory size
    long   ru_idrss;            // Integral unshared data size
    long   ru_isrss;            // Integral unshared stack size
    long   ru_minflt;           // Page reclaims (soft page faults)
    long   ru_majflt;           // Page faults (hard page faults)
    long   ru_nswap;            // Swaps
    long   ru_inblock;          // Block input operations (讀取次數)
    long   ru_oublock;          // Block output operations (寫入次數)
    long   ru_msgsnd;           // IPC messages sent
    long   ru_msgrcv;           // IPC messages received
    long   ru_nsignals;         // Signals received
    long   ru_nvcsw;            // Voluntary context switches (自願上下文切換)
    long   ru_nivcsw;           // Involuntary context switches (非自願上下文切換)
};
```

**Phase 1 使用的欄位**:
- `ru_utime`: User mode CPU time
- `ru_stime`: Kernel mode CPU time

---

### 2. User Mode vs Kernel Mode

#### CPU 的兩種執行模式

現代作業系統使用 **特權級別** (Privilege Levels) 隔離使用者程式和作業系統核心:

| 特性 | User Mode (使用者模式) | Kernel Mode (核心模式) |
|------|----------------------|---------------------|
| **特權等級** | Ring 3 (最低) | Ring 0 (最高) |
| **可執行指令** | 一般指令 | 特權指令 + 一般指令 |
| **記憶體存取** | 只能存取使用者空間 | 可存取所有記憶體 |
| **硬體存取** | 不可直接存取 | 可直接存取硬體 |
| **錯誤影響** | 只影響該程序 | 可能造成系統當機 |

#### 為什麼需要 Kernel Mode?

使用者程式需要進行以下操作時,必須切換到 kernel mode:
- 檔案 I/O (`read()`, `write()`, `open()`, `close()`)
- 網路通訊 (`socket()`, `send()`, `recv()`)
- 程序管理 (`fork()`, `exec()`, `wait()`)
- 記憶體管理 (`mmap()`, `brk()`)
- 時間相關 (`gettimeofday()`, `clock_gettime()`)

**切換流程**:
1. 使用者程式呼叫 system call (例如 `write()`)
2. CPU 觸發 **trap/interrupt**,切換到 kernel mode
3. 核心執行對應的 system call handler
4. 完成後切換回 user mode,返回使用者程式

**Overhead**: 每次 mode switch 約 50-100 ns,這就是為什麼頻繁的 system call 會降低效能。

---

### 3. User Time vs System Time

#### User Time (ru_utime)

程式在 **user mode** 執行所花費的 CPU 時間。

**包含**:
- 應用程式邏輯執行
- 數學運算、字串處理
- 呼叫使用者空間函式庫 (如 glibc 的 `strlen()`)

**不包含**:
- System call 執行時間
- 等待 I/O 的時間 (此時 CPU 沒有執行此程序)

#### System Time (ru_stime)

程式在 **kernel mode** 執行所花費的 CPU 時間。

**包含**:
- 執行 system calls
- Page fault 處理
- Signal handling (核心部分)

**不包含**:
- I/O 等待時間 (CPU 閒置)

#### 範例對比

```c
// High user time, low system time
void cpu_intensive() {
    volatile double result = 0.0;
    for (int i = 0; i < 10000000; i++) {
        result += i * 3.14;  // 純 CPU 運算,user mode
    }
}

// Low user time, high system time
void io_intensive() {
    int fd = open("test.txt", O_WRONLY | O_CREAT, 0644);
    for (int i = 0; i < 10000; i++) {
        write(fd, "data", 4);  // system call,kernel mode
    }
    close(fd);
}
```

---

### 4. 本專案的實作細節

#### 在 function_info_t 中新增欄位

```c
typedef struct {
    char name[256];
    time_stamp total_time;      // Wall clock time
    time_stamp self_time;       // Sampling time
    time_stamp user_time;       // NEW: User mode CPU time
    time_stamp sys_time;        // NEW: Kernel mode CPU time
    time_stamp call_count;
    int is_active;
    struct timeval start_time;
    struct rusage start_rusage; // NEW: Resource usage at entry
} function_info_t;
```

#### 記錄進入時的 rusage

```c
void enter_function(int func_id) {
    // ...
    getrusage(RUSAGE_SELF, &functions[func_id].start_rusage);
    // ...
}
```

**為什麼用 RUSAGE_SELF?**
- 目前是單執行緒程式,`RUSAGE_SELF` 和 `RUSAGE_THREAD` 效果相同
- `RUSAGE_THREAD` 需要 `_GNU_SOURCE`,且為 Linux 特有
- Phase 3 實作多執行緒時,將改用 `RUSAGE_THREAD`

#### 計算離開時的時間增量

```c
void leave_function(int func_id) {
    struct rusage end_rusage;
    getrusage(RUSAGE_SELF, &end_rusage);

    // Calculate user time delta
    long user_delta =
        (end_rusage.ru_utime.tv_sec - start_rusage.ru_utime.tv_sec) * 1000000 +
        (end_rusage.ru_utime.tv_usec - start_rusage.ru_utime.tv_usec);

    // Calculate system time delta
    long sys_delta =
        (end_rusage.ru_stime.tv_sec - start_rusage.ru_stime.tv_sec) * 1000000 +
        (end_rusage.ru_stime.tv_usec - start_rusage.ru_stime.tv_usec);

    functions[func_id].user_time += user_delta;
    functions[func_id].sys_time += sys_delta;
}
```

**關鍵點**:
- `ru_utime` 和 `ru_stime` 都是 `struct timeval` 格式 (秒 + 微秒)
- 需要分別計算秒數和微秒數的差值
- 轉換為微秒儲存 (與 `total_time` 單位一致)

---

### 5. 測試與驗證

#### CPU 密集型測試

```c
void function_cpu_heavy() {
    PROFILE_FUNCTION();
    volatile double result = 0.0;
    for (int i = 0; i < 2000000; i++) {
        result += i * 3.14159;
        result /= (i + 1.0);
    }
}
```

**預期結果**:
- User time 高 (約等於 total time)
- System time 接近 0

**實際結果**:
```
function_cpu_heavy    1      15.32       7.99     0.0153     0.0000     19.98%     15.322
                                                   ^^^^^^     ^^^^^^
                                                   User(s)    Sys(s)
```
✅ 符合預期！

#### I/O 密集型測試

```c
void function_io_heavy() {
    PROFILE_FUNCTION();
    int fd = open("/dev/null", O_WRONLY);
    char buffer[1024] = {0};
    for (int i = 0; i < 5000; i++) {
        write(fd, buffer, sizeof(buffer));
    }
    close(fd);
}
```

**預期結果**:
- System time 較高 (因為 `write()` 是 system call)
- User time 較低

**實際結果**:
```
function_io_heavy     1       1.36      11.99     0.0014     0.0000     29.99%      1.365
                                                   ^^^^^^     ^^^^^^
                                                   User(s)    Sys(s)
```

⚠️ **System time 為 0 的原因**:
1. 寫入 `/dev/null` 是特殊處理,核心直接丟棄不執行實際 I/O
2. 現代系統對此高度優化,幾乎沒有 kernel time
3. 5000 次寫入可能還不夠多到產生可測量的 system time (微秒級)

**改進方案** (Phase 2 可嘗試):
- 寫入實際檔案而非 `/dev/null`
- 增加寫入次數到 100,000+
- 使用 `fsync()` 強制同步到磁碟
- 使用 `sleep()` 或 `nanosleep()` 等明確的 system call

---

### 6. RUSAGE_SELF vs RUSAGE_THREAD

#### RUSAGE_SELF

- 返回整個程序的資源使用
- **多執行緒程式**: 累加所有執行緒的使用量
- **可攜性**: POSIX 標準,所有 UNIX-like 系統支援

```c
struct rusage usage;
getrusage(RUSAGE_SELF, &usage);
// 單執行緒: 等同於該執行緒的使用
// 多執行緒: 所有執行緒的總和
```

#### RUSAGE_THREAD (Linux 特有)

- 返回目前執行緒的資源使用
- 需要 `#define _GNU_SOURCE` 在所有 include 之前
- 僅 Linux 2.6.26+ 支援

```c
#define _GNU_SOURCE
#include <sys/resource.h>

struct rusage usage;
getrusage(RUSAGE_THREAD, &usage);  // 只包含當前執行緒
```

#### 為何本專案先用 RUSAGE_SELF?

1. **當前階段是單執行緒**: 效果完全相同
2. **可攜性**: 不需要 GNU 擴展
3. **Phase 3 再切換**: 實作多執行緒時,會改用 `RUSAGE_THREAD` 並處理可攜性問題

---

### 7. 常見問題與陷阱

#### Q1: User time + System time > Total time?

**不可能**。CPU time (user + sys) 永遠 ≤ Wall time (total)。

Wall time = User time + System time + Wait time

Wait time 包含:
- I/O 等待
- Sleep
- 等待其他程序釋放 CPU

#### Q2: getrusage() 的精度如何?

- **理論精度**: 微秒 (µs)
- **實際精度**: 取決於系統的 tick rate (通常 1-10ms)
- 對於極短的函數 (< 1ms),測量誤差可能很大

#### Q3: 為什麼不用 clock()?

```c
clock_t clock(void);  // 返回程序使用的 CPU clock ticks
```

**問題**:
- 多執行緒下會累加所有執行緒 (類似 RUSAGE_SELF)
- 返回值是 clock ticks,需要除以 `CLOCKS_PER_SEC` 轉換
- 精度較低
- 無法區分 user time 和 system time

`getrusage()` 更精確且提供更多資訊。

#### Q4: 為何 I/O 函數的 system time 很少?

原因:
1. **Buffering**: 系統使用 buffer,不一定立即執行真正的 I/O
2. **Caching**: 檔案系統快取減少實際磁碟操作
3. **非同步 I/O**: 核心可能非同步執行寫入
4. **特殊裝置**: `/dev/null` 幾乎沒有真正的 I/O overhead

要看到明顯的 system time:
- 使用 `O_DIRECT` flag 繞過快取
- 對實際檔案進行 `fsync()`
- 大量的網路 I/O
- 使用 `futex` 等需要核心介入的同步原語

---

### 8. 學習心得與收穫

#### 技術收穫

1. **深入理解 CPU 時間分類**:
   - Wall time: 實際經過時間
   - User time: 使用者態 CPU 時間
   - System time: 核心態 CPU 時間
   - 三者關係: Wall ≥ User + System

2. **getrusage() 的強大功能**:
   - 不只是時間,還有記憶體、I/O、context switch 等統計
   - 可用於更深入的效能分析

3. **System Call 的效能影響**:
   - 每次 system call 都有 mode switch overhead
   - 頻繁的小 I/O 比大塊 I/O 慢
   - Batching 和 buffering 的重要性

#### 實作經驗

1. **RUSAGE_SELF 的可攜性權衡**:
   - 先用標準功能,有需要再用擴展
   - 為未來多執行緒留下升級路徑

2. **時間計算的細節**:
   - `struct timeval` 的秒和微秒分開計算
   - 注意溢位問題 (先乘以 1000000 再加)

3. **測試設計的重要性**:
   - 需要設計能明確區分特徵的測試
   - `/dev/null` 對測試 I/O overhead 不理想

---

## Phase 2: I/O Wait Time 估算 (已完成)

**完成日期**: 2026-01-25

### 1. clock_gettime() 與 CLOCK_MONOTONIC

#### 為什麼升級到 clock_gettime()?

Phase 1 使用 `gettimeofday()`,Phase 2 升級為 `clock_gettime()`:

| 特性 | gettimeofday() | clock_gettime() |
|------|----------------|-----------------|
| **精度** | 微秒 (µs) | 奈秒 (ns) |
| **標準** | POSIX.1-2001 (已廢棄) | POSIX.1-2008 (推薦) |
| **時鐘類型** | 固定為系統時間 | 可選擇多種時鐘 |
| **受 NTP 影響** | 是 (可能倒退) | MONOTONIC 不受影響 |

```c
#include <time.h>

int clock_gettime(clockid_t clk_id, struct timespec *tp);
```

#### CLOCK 類型選擇

| CLOCK 類型 | 說明 | 用途 |
|-----------|------|------|
| **CLOCK_REALTIME** | 系統實際時間 (wall clock) | 取得當前日期時間 |
| **CLOCK_MONOTONIC** | 單調遞增時鐘 (不倒退) | **測量時間間隔 (推薦)** |
| CLOCK_PROCESS_CPUTIME_ID | 程序 CPU 時間 | 類似 rusage |
| CLOCK_THREAD_CPUTIME_ID | 執行緒 CPU 時間 | 多執行緒分析 |

**為何選擇 CLOCK_MONOTONIC?**
- 不受系統時間調整影響 (NTP、手動修改)
- 保證單調遞增,不會倒退
- 適合測量時間差

**範例: 系統時間調整的影響**
```c
// 使用 CLOCK_REALTIME (危險!)
struct timespec start, end;
clock_gettime(CLOCK_REALTIME, &start);
// ... 執行 1 秒 ...
// 此時系統管理員將時間往回調 10 秒
clock_gettime(CLOCK_REALTIME, &end);
// 結果: delta = -9 秒! (負值!)

// 使用 CLOCK_MONOTONIC (安全)
clock_gettime(CLOCK_MONOTONIC, &start);
// ... 即使系統時間改變 ...
clock_gettime(CLOCK_MONOTONIC, &end);
// 結果: delta = 1 秒 (正確)
```

---

### 2. struct timespec vs struct timeval

#### struct timeval (舊式)

```c
struct timeval {
    time_t      tv_sec;   // 秒
    suseconds_t tv_usec;  // 微秒 (0-999999)
};
```

- 使用於 `gettimeofday()`, `select()`, `rusage`
- 精度: 微秒 (10⁻⁶ 秒)

#### struct timespec (新式)

```c
struct timespec {
    time_t tv_sec;   // 秒
    long   tv_nsec;  // 奈秒 (0-999999999)
};
```

- 使用於 `clock_gettime()`, `nanosleep()`, `pthread_mutex_timedlock()`
- 精度: 奈秒 (10⁻⁹ 秒) - **1000 倍更精確**

#### 時間差計算

**timeval** (微秒):
```c
long delta_us = (end.tv_sec - start.tv_sec) * 1000000 +
                (end.tv_usec - start.tv_usec);
```

**timespec** (轉換為微秒):
```c
long long delta_us = (end.tv_sec - start.tv_sec) * 1000000LL +
                     (end.tv_nsec - start.tv_nsec) / 1000;
```

**注意**:
- 奈秒除以 1000 = 微秒
- 使用 `long long` 避免 32-bit 溢位
- 秒數差先乘以 `1000000LL` (保持 64-bit)

---

### 3. Wall Clock Time 的概念

#### 三種時間測量

```
Wall Time = User Time + System Time + Wait Time
   (實際經過時間)   (CPU 使用)        (閒置等待)
```

**Wall Clock Time (掛鐘時間)**:
- 從函數進入到離開,實際經過的時間
- 就像看牆上的時鐘,記錄真實流逝的時間
- 包含所有時間:運算、系統呼叫、等待

**CPU Time**:
- User Time: CPU 在 user mode 執行的時間
- System Time: CPU 在 kernel mode 執行的時間

**Wait Time**:
- Wall Time - CPU Time
- 程式沒有使用 CPU 的時間
- 包含: I/O 等待、sleep、lock 等待、被其他程序搶占 CPU

---

### 4. Wait Time 計算與意義

#### 計算公式

```c
long long wait_delta = wall_delta - (user_delta + sys_delta);
```

**數學關係**:
```
Wall Time ≥ User Time + System Time

Wait Time = Wall Time - User Time - System Time
          = 程式「沒在用 CPU」的時間
```

#### Wait Time 的來源

1. **I/O Blocking** (最常見)
   ```c
   write(fd, data, size);  // 等待磁碟寫入完成
   read(fd, buf, size);    // 等待資料從磁碟讀取
   ```
   - 程式等待硬體完成操作
   - CPU 閒置或執行其他程序

2. **Sleep / Delay**
   ```c
   sleep(1);              // 主動睡眠 1 秒
   nanosleep(&ts, NULL);  // 精確睡眠
   ```
   - 程式主動放棄 CPU
   - 全部計入 wait time

3. **Lock Contention** (多執行緒)
   ```c
   pthread_mutex_lock(&mutex);  // 等待其他執行緒釋放鎖
   ```
   - 執行緒被阻塞,等待資源
   - Phase 3 會深入探討

4. **CPU 被搶占**
   - 作業系統排程其他程序
   - 在高負載系統下明顯

---

### 5. 本專案的實作細節

#### 在 function_info_t 中新增欄位

```c
typedef struct {
    // ... existing fields ...
    time_stamp wait_time;              // NEW: Wait time (microseconds)
    struct timespec start_wall_time;   // NEW: Wall clock at entry
    struct rusage start_rusage;        // Phase 1
} function_info_t;
```

#### 測量 Wall Time

**進入函數**:
```c
void enter_function(int func_id) {
    // Record wall clock time with nanosecond precision
    clock_gettime(CLOCK_MONOTONIC, &functions[func_id].start_wall_time);

    // Record CPU time
    getrusage(RUSAGE_SELF, &functions[func_id].start_rusage);
}
```

#### 計算 Wait Time

**離開函數**:
```c
void leave_function(int func_id) {
    struct timespec end_wall_time;
    struct rusage end_rusage;

    clock_gettime(CLOCK_MONOTONIC, &end_wall_time);
    getrusage(RUSAGE_SELF, &end_rusage);

    // Calculate wall time (convert ns to μs)
    long long wall_delta =
        (end_wall_time.tv_sec - start_wall_time.tv_sec) * 1000000LL +
        (end_wall_time.tv_nsec - start_wall_time.tv_nsec) / 1000;

    // Calculate CPU times
    long long user_delta = ...;  // from rusage
    long long sys_delta = ...;   // from rusage

    // Calculate wait time
    long long wait_delta = wall_delta - (user_delta + sys_delta);

    // Handle negative values (measurement precision error)
    if (wait_delta < 0) wait_delta = 0;

    functions[func_id].wait_time += wait_delta;
}
```

**為何可能出現負值?**
- `clock_gettime()` 和 `getrusage()` 不是原子操作
- 微小的測量誤差 (< 1 µs)
- 設為 0 避免統計錯誤

---

### 6. 測試與驗證

#### Sleep 測試

```c
void function_sleep_test() {
    PROFILE_FUNCTION();
    struct timespec sleep_time = {0, 100000000}; // 100ms
    nanosleep(&sleep_time, NULL);
}
```

**預期結果**:
- Wall time ≈ 100ms
- User time ≈ 0
- System time ≈ 0
- **Wait time ≈ 100ms**

**實際結果**:
```
function_sleep_test  1  100.14ms  0.00ms  0.0000s  0.0000s  0.1001s
                                           User     Sys      Wait
```
✅ **100.1ms wait time - 誤差 < 0.1%!**

#### I/O 密集測試

```c
void function_io_heavy() {
    PROFILE_FUNCTION();
    int fd = open("test.tmp", O_WRONLY | O_CREAT | O_SYNC, 0644);
    for (int i = 0; i < 1000; i++) {
        write(fd, buffer, 4096);  // 每次 write 都等待磁碟
    }
    fsync(fd);
    close(fd);
}
```

**實際結果**:
```
function_io_heavy  1  2199.50ms  2039.26ms  0.0042s  0.1765s  2.0188s
                                             User     Sys      Wait
```

**分析**:
- Total: 2199ms
- User: 4.2ms (0.2%) - 極少的 CPU 運算
- Sys: 176.5ms (8%) - 系統呼叫處理
- **Wait: 2018.8ms (91.8%)** - **絕大部分時間在等 I/O!**

這就是典型的 **I/O bound** 程式特徵。

#### CPU 密集測試

```c
void function_cpu_heavy() {
    PROFILE_FUNCTION();
    volatile double result = 0.0;
    for (int i = 0; i < 2000000; i++) {
        result += i * 3.14159;
        result /= (i + 1.0);
    }
}
```

**實際結果**:
```
function_cpu_heavy  1  15.55ms  8.00ms  0.0156s  0.0000s  0.0000s
                                        User     Sys      Wait
```

**分析**:
- User: 15.6ms (100%) - 全部是 CPU 運算
- Sys: 0ms
- **Wait: 0ms** - 沒有任何等待

這就是典型的 **CPU bound** 程式特徵。

#### 混合負載測試

```c
void function_mixed() {
    PROFILE_FUNCTION();
    // CPU work
    for (...) sum += i;
    // I/O
    write(fd, buf, size);
    // Sleep
    nanosleep(&ts, NULL);  // 50ms
}
```

**實際結果**:
```
function_mixed  1  50.49ms  0.00ms  0.0001s  0.0003s  0.0501s
```

**分析**:
- User: 0.1ms (CPU 工作)
- Sys: 0.3ms (I/O 系統呼叫)
- **Wait: 50.1ms** - 主要是 sleep 時間

---

### 7. I/O Bound vs CPU Bound 識別

#### 判斷標準

| 程式類型 | User% | Sys% | Wait% | 優化方向 |
|---------|-------|------|-------|---------|
| **CPU Bound** | 高 (>80%) | 低 | 低 | 演算法優化、平行化 |
| **I/O Bound** | 低 | 中 | 高 (>50%) | 非同步 I/O、快取、批次處理 |
| **Syscall Heavy** | 低 | 高 (>30%) | 低 | 減少系統呼叫次數、batching |
| **Lock Bound** | 低 | 低 | 高 | 減少鎖競爭、無鎖演算法 |

#### 實際應用範例

**問題**: 程式執行很慢,該如何優化?

**步驟 1**: 執行 profiler,查看報表
```
function_process_data  Total: 5000ms  User: 50ms  Sys: 200ms  Wait: 4750ms
```

**步驟 2**: 分析瓶頸
- Wait% = 4750/5000 = 95%
- 結論: **I/O bound**,不是 CPU 問題

**步驟 3**: 對症下藥
- ❌ 優化演算法 (User time 只佔 1%,優化無效)
- ✅ 優化 I/O:
  - 使用非同步 I/O
  - 增加 buffer 大小
  - 批次處理減少 I/O 次數
  - 使用記憶體快取

---

### 8. nanosleep() 與精確時間

#### nanosleep() API

```c
#include <time.h>

int nanosleep(const struct timespec *req, struct timespec *rem);
```

**參數**:
- `req`: 請求的睡眠時間
- `rem`: 如果被信號中斷,剩餘時間寫入此處 (可為 NULL)

**返回值**:
- 0: 成功睡眠完整時間
- -1: 被信號中斷,errno = EINTR

**範例**:
```c
struct timespec ts;
ts.tv_sec = 0;         // 0 秒
ts.tv_nsec = 100000000; // 100,000,000 奈秒 = 100 毫秒

nanosleep(&ts, NULL);
```

#### nanosleep vs sleep vs usleep

| 函數 | 精度 | 標準 | 可中斷 |
|------|------|------|--------|
| sleep(seconds) | 秒 | POSIX | 是 |
| usleep(microseconds) | 微秒 | 已廢棄 | 否 |
| **nanosleep()** | **奈秒** | **POSIX (推薦)** | **是** |

**為何使用 nanosleep?**
- 精度最高 (雖然實際精度取決於系統)
- 可處理信號中斷
- POSIX 標準,可攜性佳

---

### 9. 常見問題與除錯

#### Q1: Wait time 為負值?

**不應該發生**,程式碼已處理:
```c
if (wait_delta < 0) wait_delta = 0;
```

**可能原因**:
- `clock_gettime()` 和 `getrusage()` 呼叫順序問題
- 極短函數的測量誤差
- 系統負載極高時的排程延遲

**解決方案**: 設為 0,避免統計錯誤

#### Q2: CPU bound 程式也有 wait time?

可能原因:
1. **被其他程序搶占 CPU**
   - 系統負載高時,程序被排程器暫停
   - 不是程式問題,是系統資源競爭

2. **Page fault**
   - 記憶體頁面不在 RAM,需從 swap 讀取
   - 計入 wait time

解決:
- 在低負載系統測試
- 確保足夠記憶體,避免 swap

#### Q3: I/O 函數 wait time 很短?

可能原因:
1. **檔案系統快取**
   - 資料已在記憶體,不需真實 I/O
   - 使用 `O_DIRECT` 繞過快取測試

2. **SSD vs HDD**
   - SSD 極快,wait time 可能 < 1ms
   - HDD 會有明顯的 wait time (>10ms)

3. **非同步 I/O**
   - 寫入可能先到 buffer,非同步刷新
   - 使用 `O_SYNC` 或 `fsync()` 強制同步

#### Q4: clock_gettime() 的實際精度?

**理論**: 奈秒
**實際**: 取決於系統

查詢方法:
```c
struct timespec res;
clock_getres(CLOCK_MONOTONIC, &res);
printf("Resolution: %ld ns\n", res.tv_nsec);
```

典型值:
- 現代 Linux: 1-10 ns
- 舊系統: 1000 ns (1 µs)
- 虛擬機: 可能更低

---

### 10. 學習心得與收穫

#### 技術收穫

1. **深入理解三種時間**:
   - Wall time: 使用者感受到的時間
   - CPU time: 程式實際使用 CPU 的時間
   - Wait time: 程式閒置的時間

2. **clock_gettime() 的優勢**:
   - 奈秒精度
   - CLOCK_MONOTONIC 不受系統時間影響
   - POSIX 標準,未來趨勢

3. **效能分析的核心**:
   - 不是所有「慢」都要優化 CPU
   - I/O bound 程式優化 CPU 無效
   - 先測量,再優化 (measure, don't guess)

#### 實作經驗

1. **時間計算的細節**:
   - timespec 的 ns 要除以 1000 轉 µs
   - 使用 `long long` 避免溢位
   - 處理負值邊界情況

2. **測試的重要性**:
   - `function_sleep_test` 驗證公式正確性
   - 混合負載測試驗證實際應用場景

3. **結果的解讀**:
   - 91% wait time = I/O bound
   - 0% wait time = CPU bound
   - 有了資料,才能對症下藥

---

---

## Phase 3: 多執行緒支援 - Thread Local Storage (已完成)

**完成日期**: 2026-01-26

### 1. Thread Local Storage (TLS) 概念

#### 什麼是 Thread Local Storage?

TLS 讓每個執行緒擁有自己獨立的變數副本,即使變數名稱相同,不同執行緒存取的是不同的記憶體位址。

**問題場景**:
```c
// ❌ 多執行緒下的問題
static int counter = 0;

void* thread_func(void* arg) {
    counter++;  // Race condition! 多個執行緒同時修改同一個變數
    return NULL;
}
```

**TLS 解決方案**:
```c
// ✅ 每個執行緒有自己的 counter
__thread int counter = 0;

void* thread_func(void* arg) {
    counter++;  // 安全! 每個執行緒修改自己的副本
    return NULL;
}
```

#### __thread 關鍵字

GCC 和 Clang 提供的 TLS 語法 (C11 標準使用 `_Thread_local`):

```c
__thread int my_var = 0;              // 基本類型
__thread char buffer[1024];            // 陣列
__thread struct my_struct data;        // 結構

// 限制: 不能是 const, 不能有建構函數 (C++)
```

**儲存位置**:
- 每個執行緒有獨立的 TLS 區段 (在執行緒堆疊附近)
- 執行緒建立時自動分配,執行緒結束時自動釋放
- 初始值在執行緒啟動時複製

---

### 2. 本專案的 TLS 應用

#### 改為 TLS 的變數

```c
// Phase 0-2: 全域變數 (單執行緒)
static function_info_t functions[MAX_FUNCTIONS];
static time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
static call_stack_t call_stack;
static int function_count;

// Phase 3: Thread-local 變數 (多執行緒)
__thread function_info_t functions[MAX_FUNCTIONS];
__thread time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
__thread call_stack_t call_stack = {.top = -1};
__thread int function_count = 0;
__thread pid_t current_thread_id = 0;
```

**效果**:
- Thread A 呼叫 `function_a()` 時,只更新 Thread A 的 `functions[]`
- Thread B 同時呼叫 `function_b()`,更新的是 Thread B 的 `functions[]`
- 兩者互不干擾,無需加鎖

#### 記憶體佈局示意

```
Thread 1 (TID 1001):          Thread 2 (TID 1002):
├─ functions[...]              ├─ functions[...]
├─ caller_counts[][]           ├─ caller_counts[][]
├─ call_stack                  ├─ call_stack
└─ function_count              └─ function_count

共享全域區 (需要 mutex):
├─ global_function_registry[]  ← 所有執行緒共用
├─ global_function_count
└─ registry_mutex
```

---

### 3. Race Condition 與 Data Race

#### Race Condition (競爭條件)

多個執行緒的執行順序影響結果,導致不確定性。

**範例**:
```c
// ❌ 錯誤: 沒有保護的共享變數
int balance = 1000;

void* withdraw(void* amount) {
    int val = balance;           // T1 讀取: 1000
    // (此時 T2 也讀取到 1000)
    val -= *(int*)amount;        // T1 計算: 1000 - 100 = 900
    // (T2 計算: 1000 - 200 = 800)
    balance = val;               // T1 寫入: 900
    // (T2 覆寫: 800) ← 消失了 100!
    return NULL;
}
```

最終結果可能是 800 而非正確的 700,這就是 race condition。

#### Data Race (資料競爭)

多個執行緒同時存取同一記憶體,且至少一個是寫入,沒有適當同步。

**判斷標準**:
1. 兩個以上執行緒存取同一變數
2. 至少一個是寫入操作
3. 存取之間沒有 happens-before 關係 (無同步機制)

**本專案避免 data race 的方式**:
- Profiling 資料使用 TLS → 每個執行緒獨立,無競爭
- 函數名稱註冊使用 mutex → 序列化存取

---

### 4. Mutex (互斥鎖) 與 Critical Section

#### pthread_mutex_t 基礎

Mutex (Mutual Exclusion) 確保同一時間只有一個執行緒執行特定程式碼。

```c
#include <pthread.h>

pthread_mutex_t my_mutex = PTHREAD_MUTEX_INITIALIZER;

// 加鎖
pthread_mutex_lock(&my_mutex);
    // Critical section (臨界區)
    // 只有一個執行緒能執行這裡
pthread_mutex_unlock(&my_mutex);
```

**運作原理**:
1. Thread A 呼叫 `pthread_mutex_lock(&mutex)` → 成功,取得鎖
2. Thread B 呼叫 `pthread_mutex_lock(&mutex)` → 阻塞等待
3. Thread A 執行完 critical section,呼叫 `pthread_mutex_unlock(&mutex)`
4. Thread B 被喚醒,取得鎖,進入 critical section

#### 本專案的 Mutex 應用

```c
// 全域函數名稱註冊表 (所有執行緒共享)
static function_registry_entry_t global_function_registry[MAX_GLOBAL_FUNCTIONS];
static int global_function_count = 0;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

int register_function(const char *name) {
    // Thread-safe: 加鎖保護全域註冊表
    pthread_mutex_lock(&registry_mutex);

    // Critical section: 查找或建立函數 ID
    int global_id = -1;
    for (int i = 0; i < global_function_count; i++) {
        if (strcmp(global_function_registry[i].name, name) == 0) {
            global_id = global_function_registry[i].id;
            break;
        }
    }

    if (global_id == -1) {
        global_id = global_function_count;
        strncpy(global_function_registry[global_function_count].name, name, 255);
        global_function_registry[global_function_count].id = global_id;
        global_function_count++;
    }

    pthread_mutex_unlock(&registry_mutex);

    // Thread-local: 建立此執行緒的 function entry (無需加鎖)
    int local_id = function_count;
    // ...
    return local_id;
}
```

**為何需要 mutex?**
- `global_function_registry` 是所有執行緒共享的
- 多個執行緒可能同時註冊同名函數
- 沒有 mutex 會造成:
  - 重複註冊相同函數
  - `global_function_count` 計數錯誤
  - 陣列越界或資料損壞

---

### 5. RUSAGE_THREAD vs RUSAGE_SELF

#### RUSAGE_SELF (Phase 0-2)

返回整個程序的資源使用,包含所有執行緒的總和。

```c
struct rusage usage;
getrusage(RUSAGE_SELF, &usage);
// 多執行緒: usage 包含 Thread A + Thread B + ... 的總和
```

**問題**:
- 在多執行緒程式中,無法區分個別執行緒的 CPU 時間
- Thread A 呼叫 `getrusage(RUSAGE_SELF)` 會包含 Thread B 的時間!

#### RUSAGE_THREAD (Phase 3)

Linux 特有,返回當前執行緒的資源使用。

```c
#define _GNU_SOURCE  // 必須定義
#include <sys/resource.h>

struct rusage usage;
getrusage(RUSAGE_THREAD, &usage);
// 只包含目前執行緒的 CPU time
```

**優點**:
- 精確測量每個執行緒的 user time 和 system time
- 不受其他執行緒影響

**本專案實作**:
```c
void enter_function(int func_id) {
    // Phase 3: 使用 RUSAGE_THREAD
    #ifdef __linux__
        getrusage(RUSAGE_THREAD, &functions[func_id].start_rusage);
    #else
        getrusage(RUSAGE_SELF, &functions[func_id].start_rusage);
    #endif
}
```

**可攜性處理**:
- Linux: 使用 `RUSAGE_THREAD` (精確)
- 其他 UNIX: fallback 到 `RUSAGE_SELF` (單執行緒仍正確)

---

### 6. pthread API 基礎

#### 執行緒建立與等待

```c
#include <pthread.h>

// 執行緒函數簽名
void* thread_function(void* arg) {
    // arg 是傳入的參數
    // 可以返回任意 void* 值
    return NULL;
}

int main() {
    pthread_t thread_id;

    // 建立執行緒
    int ret = pthread_create(&thread_id, NULL, thread_function, NULL);
    //                        ^thread ID  ^attr  ^函數         ^參數
    if (ret != 0) {
        perror("pthread_create failed");
    }

    // 等待執行緒結束
    void* return_value;
    pthread_join(thread_id, &return_value);

    return 0;
}
```

#### pthread_create 參數

```c
int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg);
```

1. **thread**: 輸出參數,返回執行緒 ID
2. **attr**: 執行緒屬性 (NULL = 預設值)
3. **start_routine**: 執行緒執行的函數
4. **arg**: 傳給執行緒函數的參數

#### pthread_join

```c
int pthread_join(pthread_t thread, void **retval);
```

- 等待指定執行緒結束
- 類似 `wait()` for processes
- `retval` 接收執行緒的返回值

**本專案範例**:
```c
void run_multi_threaded_tests() {
    pthread_t threads[4];

    // 建立 4 個執行緒
    pthread_create(&threads[0], NULL, thread_worker_cpu, NULL);
    pthread_create(&threads[1], NULL, thread_worker_io, NULL);
    pthread_create(&threads[2], NULL, thread_worker_sleep, NULL);
    pthread_create(&threads[3], NULL, thread_worker_mixed, NULL);

    // 等待所有執行緒完成
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
}
```

---

### 7. Thread ID 取得

#### pthread_self() vs gettid() vs syscall(SYS_gettid)

| 方法 | 返回值 | 用途 | 可攜性 |
|------|--------|------|--------|
| `pthread_self()` | pthread_t (抽象 ID) | pthread 內部識別 | POSIX |
| `gettid()` | pid_t (整數) | 系統 thread ID | glibc 2.30+ |
| `syscall(SYS_gettid)` | pid_t | 直接系統呼叫 | Linux |

**pthread_self() 問題**:
```c
pthread_t tid = pthread_self();
printf("%lu\n", tid);  // ❌ pthread_t 不保證是整數!
```

- `pthread_t` 是不透明類型,可能是結構或指標
- 不適合直接 printf

**syscall(SYS_gettid) 優點**:
```c
pid_t tid = syscall(SYS_gettid);
printf("Thread %d\n", tid);  // ✅ 保證是整數
```

- 返回核心的執行緒 ID (與 `ps -eLf` 顯示的 TID 相同)
- 可直接用於除錯和日誌

**本專案使用**:
```c
__thread pid_t current_thread_id = 0;

int register_function(const char *name) {
    if (current_thread_id == 0) {
        current_thread_id = syscall(SYS_gettid);  // 快取 TID
    }
    // ...
}
```

---

### 8. 測試與驗證

#### 多執行緒測試設計

```bash
./main --multi-threaded
```

建立 4 個執行緒,各自執行不同工作負載:

1. **Thread 1: CPU-intensive**
   ```c
   void* thread_worker_cpu(void* arg) {
       for (int i = 0; i < 3; i++) {
           function_cpu_heavy();  // CPU 密集運算
       }
       print_profiling_results();  // 顯示此執行緒的統計
       return NULL;
   }
   ```

2. **Thread 2: I/O-intensive**
   ```c
   void* thread_worker_io(void* arg) {
       function_io_heavy();  // 大量檔案 I/O
       print_profiling_results();
       return NULL;
   }
   ```

3. **Thread 3: Sleep-heavy**
   ```c
   void* thread_worker_sleep(void* arg) {
       for (int i = 0; i < 5; i++) {
           function_sleep_test();  // 100ms sleep × 5
       }
       print_profiling_results();
       return NULL;
   }
   ```

4. **Thread 4: Mixed workload**
   ```c
   void* thread_worker_mixed(void* arg) {
       function_a();
       function_b();
       function_c();
       function_mixed();
       print_profiling_results();
       return NULL;
   }
   ```

#### 實際執行結果

```
Thread 11906: CPU work done
=== Profiling Results (Phase 3: Thread 11906) ===
function_cpu_heavy      3    49.51ms    32.00ms    0.0482s    0.0000s    0.0041s

Thread 11907: I/O work done
=== Profiling Results (Phase 3: Thread 11907) ===
function_io_heavy       1  2192.79ms  2129.26ms    0.0000s    0.1713s    2.0215s

Thread 11908: Sleep work done
=== Profiling Results (Phase 3: Thread 11908) ===
function_sleep_test     5   500.59ms     0.00ms    0.0002s    0.0000s    0.5004s

Thread 11909: Mixed work done
=== Profiling Results (Phase 3: Thread 11909) ===
function_mixed          1    51.12ms     0.00ms    0.0021s    0.0000s    0.0490s
```

**驗證結果**:
- ✅ 每個執行緒有獨立的 Thread ID (11906, 11907, 11908, 11909)
- ✅ 各執行緒的 call count 正確且獨立
- ✅ 沒有 race condition (資料沒有混亂)
- ✅ RUSAGE_THREAD 正確測量各執行緒的 CPU time

---

### 9. 編譯選項與巨集

#### Makefile 更新

```makefile
all:
	gcc -Wall -D_GNU_SOURCE -DAUTO_PROFILE -g -pthread -o main main.c
```

**新增旗標**:
1. **-D_GNU_SOURCE**:
   - 啟用 GNU 擴展,包括 `RUSAGE_THREAD`
   - 必須在所有 `#include` 之前定義

2. **-pthread**:
   - 連結 pthread 函式庫
   - 啟用多執行緒編譯選項
   - 等同於 `-lpthread` (但更推薦 `-pthread`)

#### 條件編譯

```c
#ifdef __linux__
    getrusage(RUSAGE_THREAD, &end_rusage);
#else
    getrusage(RUSAGE_SELF, &end_rusage);
#endif
```

**目的**:
- Linux: 使用 RUSAGE_THREAD (精確測量)
- 其他平台: fallback 到 RUSAGE_SELF (保持相容)

---

### 10. 常見問題與除錯

#### Q1: 為什麼不全部用 TLS,還需要 mutex?

**回答**: 函數名稱註冊表必須全域共享。

- 如果每個執行緒都有自己的註冊表,同名函數會有不同 ID
- Phase 4 合併報表時無法對應
- Global registry 提供統一的函數 ID mapping

#### Q2: TLS 變數的初始化時機?

**回答**: 每個新執行緒啟動時。

```c
__thread int counter = 42;  // 每個執行緒都初始化為 42

void* thread_func(void* arg) {
    printf("%d\n", counter);  // 輸出: 42 (不是其他執行緒修改的值)
    counter = 100;            // 只影響本執行緒
    return NULL;
}
```

主執行緒和新執行緒的 TLS 變數是獨立的。

#### Q3: pthread_join 一定要呼叫嗎?

**必須呼叫**,否則:
- 執行緒資源不會釋放 (記憶體洩漏)
- 執行緒變成 zombie thread
- 類似 process 的 `wait()`

**例外**: Detached thread
```c
pthread_detach(thread_id);  // 執行緒結束自動清理,無需 join
```

#### Q4: 多執行緒下 SIGPROF 如何運作?

**Linux 行為**:
- `ITIMER_PROF` 信號會發送到整個程序
- 隨機選擇一個執行緒接收信號
- 不保證每個執行緒都收到 (採樣不均)

**Phase 3 的選擇**:
- 目前只測量 instrumentation 資料 (總是精確)
- Sampling (SIGPROF) 在多執行緒下不準確
- Phase 4 可考慮使用 `timer_create()` + `SIGEV_THREAD_ID` 分執行緒採樣

---

### 11. 學習心得與收穫

#### 技術收穫

1. **TLS 的威力**:
   - 無鎖並行,性能極佳
   - 適合執行緒獨立的 profiling 資料
   - 簡化程式設計,避免複雜的同步

2. **混合策略**:
   - TLS 處理執行緒私有資料 (functions, call_stack)
   - Mutex 處理共享資料 (global registry)
   - 最小化鎖競爭

3. **RUSAGE_THREAD 的重要性**:
   - 多執行緒下必須使用,否則測量不準
   - 單執行緒下 RUSAGE_SELF 和 RUSAGE_THREAD 等價

#### 實作經驗

1. **執行緒安全的註冊機制**:
   - 全域 registry 提供統一的函數 ID
   - Mutex 保護關鍵區域
   - TLS 儲存執行緒本地資料

2. **測試的全面性**:
   - 4 個不同工作負載驗證獨立性
   - 每個執行緒輸出自己的報表
   - 視覺化驗證無資料混亂

3. **可攜性考慮**:
   - 使用條件編譯支援非 Linux 平台
   - Fallback 機制確保基本功能

#### 多執行緒 Profiling 的挑戰

1. **Sampling 問題**:
   - SIGPROF 在多執行緒下分佈不均
   - 需要更精細的 per-thread timer

2. **資料聚合**:
   - Phase 4 需實作合併所有執行緒的報表
   - 需要追蹤所有執行緒的 TLS 資料

3. **Lock Contention 測量**:
   - 目前 wait time 包含 lock waiting
   - 未來可細分 I/O wait 和 lock wait

---

---

## Phase 4: 多執行緒報表輸出 (已完成)

**完成日期**: 2026-01-26

### 1. TLS 資料收集的挑戰

#### 問題描述

Phase 3 使用 TLS (`__thread`) 隔離各執行緒的 profiling 資料:

```c
__thread function_info_t functions[MAX_FUNCTIONS];
__thread time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
```

**問題**: TLS 變數是執行緒私有的,主執行緒無法直接存取其他執行緒的 TLS。

```c
// ❌ 這不可行！
void main_thread() {
    // 無法存取 Thread A 的 functions[]
    // 無法存取 Thread B 的 functions[]
}
```

#### 解決方案: 資料快照 (Snapshot)

在執行緒結束前,將 TLS 資料複製到全域堆積記憶體:

```c
// 全域快照陣列 (所有執行緒共享)
thread_data_snapshot_t* thread_snapshots[MAX_THREADS];
int thread_snapshot_count = 0;
pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;

// 執行緒快照結構
typedef struct {
    pid_t thread_id;
    int function_count;
    function_info_t functions[MAX_FUNCTIONS];       // 複製 TLS
    time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];  // 複製 TLS
} thread_data_snapshot_t;
```

---

### 2. 資料收集機制實作

#### register_thread_data() 函數

每個執行緒在結束前呼叫此函數,將 TLS 資料快照到全域:

```c
void register_thread_data() {
    pid_t tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);

    // 跳過沒有 profiling 資料的執行緒
    if (function_count == 0) {
        return;
    }

    pthread_mutex_lock(&snapshot_mutex);

    // 動態分配快照記憶體
    thread_data_snapshot_t* snapshot = malloc(sizeof(thread_data_snapshot_t));

    // 複製 TLS 資料
    snapshot->thread_id = tid;
    snapshot->function_count = function_count;
    memcpy(snapshot->functions, functions, sizeof(functions));
    memcpy(snapshot->caller_counts, caller_counts, sizeof(caller_counts));

    // 加入全域快照列表
    thread_snapshots[thread_snapshot_count++] = snapshot;

    pthread_mutex_unlock(&snapshot_mutex);
}
```

**關鍵點**:
- 使用 `malloc()` 動態分配,確保資料在執行緒結束後仍存在
- 使用 `memcpy()` 複製整個陣列 (深拷貝)
- Mutex 保護全域陣列,避免 race condition

#### 執行緒 Worker 函數修改

```c
void* thread_worker_cpu(void* arg) {
    PROFILE_SCOPE("thread_worker_cpu");

    // ... 執行工作 ...
    function_cpu_heavy();

    // Phase 4: 結束前註冊資料
    register_thread_data();

    return NULL;
}
```

---

### 3. 分執行緒報表實作

#### print_per_thread_reports() 函數

遍歷所有快照,為每個執行緒產生獨立報表:

```c
void print_per_thread_reports() {
    printf("================================================================================\n");
    printf("Per-Thread Profiling Reports (Phase 4)\n");
    printf("Total threads: %d\n", thread_snapshot_count);

    for (int i = 0; i < thread_snapshot_count; i++) {
        print_thread_report(thread_snapshots[i]);
    }
}

void print_thread_report(thread_data_snapshot_t* snapshot) {
    printf("\n=== Thread %d Report ===\n", snapshot->thread_id);

    // 列印此執行緒的所有函數統計
    for (int i = 0; i < snapshot->function_count; i++) {
        if (snapshot->functions[i].call_count > 0) {
            printf("%-30s %10llu ...\n",
                snapshot->functions[i].name,
                snapshot->functions[i].call_count);
        }
    }
}
```

**輸出範例**:
```
=== Thread 12345 Report ===
Function                  Calls  Total(ms)  User(s)  ...
function_cpu_heavy            3      47.14    0.0467  ...

=== Thread 12346 Report ===
Function                  Calls  Total(ms)  User(s)  ...
function_io_heavy             1    2066.25    0.0042  ...
```

---

### 4. 合併報表實作

#### 資料彙總演算法

合併報表需要將所有執行緒的相同函數資料累加:

```c
typedef struct {
    char name[256];
    time_stamp total_time;      // 累加所有執行緒
    time_stamp self_time;
    time_stamp user_time;
    time_stamp sys_time;
    time_stamp wait_time;
    time_stamp call_count;      // 累加所有執行緒
    int thread_count;           // 有多少執行緒呼叫此函數
} merged_function_t;
```

#### print_merged_report() 實作

```c
void print_merged_report() {
    // 1. 建立合併資料結構
    merged_function_t merged[MAX_GLOBAL_FUNCTIONS];
    memset(merged, 0, sizeof(merged));

    // 2. 初始化函數名稱 (從全域註冊表)
    for (int i = 0; i < global_function_count; i++) {
        strncpy(merged[i].name, global_function_registry[i].name, 255);
    }

    // 3. 遍歷所有執行緒快照
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];

        // 4. 遍歷此執行緒的所有函數
        for (int f = 0; f < snapshot->function_count; f++) {
            if (snapshot->functions[f].call_count == 0) continue;

            // 5. 找到對應的全域函數 ID
            int global_id = find_global_function_id(snapshot->functions[f].name);

            // 6. 累加資料
            merged[global_id].total_time += snapshot->functions[f].total_time;
            merged[global_id].user_time += snapshot->functions[f].user_time;
            merged[global_id].sys_time += snapshot->functions[f].sys_time;
            merged[global_id].wait_time += snapshot->functions[f].wait_time;
            merged[global_id].call_count += snapshot->functions[f].call_count;
            merged[global_id].thread_count++;  // 記錄執行緒數
        }
    }

    // 7. 列印合併報表
    for (int i = 0; i < global_function_count; i++) {
        if (merged[i].call_count > 0) {
            printf("%-30s %10llu %10d ...\n",
                merged[i].name,
                merged[i].call_count,
                merged[i].thread_count);
        }
    }
}
```

**輸出範例**:
```
Function                  Calls  Threads  Total(ms)  User(s)  ...
function_a                   14        4      27.71    0.0242  ...
function_cpu_heavy           14        4     217.85    0.2157  ...
```

**解讀**:
- `function_a` 被呼叫 14 次,來自 4 個執行緒
- 總時間 27.71ms 是所有執行緒的累加

---

### 5. Static __thread 變數問題

#### 問題發現

Phase 3 實作中,`PROFILE_FUNCTION()` 巨集使用 static 變數快取函數 ID:

```c
#define PROFILE_FUNCTION()                                  \
    static int __func_id = -1;  /* ❌ 問題! */             \
    if (__func_id == -1) {                                  \
        __func_id = register_function(__func__);            \
    }                                                       \
    enter_function(__func_id);
```

**問題**: `static` 變數是全域共享的,但它儲存的是 thread-local 函數索引!

**情境**:
1. Thread A 呼叫 `function_a()`:
   - `__func_id = -1` → 註冊,得到 `local_id = 0`
   - `__func_id = 0` (全域變數!)

2. Thread B 呼叫 `function_a()`:
   - `__func_id = 0` (已被 Thread A 設置)
   - 跳過註冊,直接使用 `func_id = 0`
   - 但 Thread B 的 `functions[0]` 是空的! ❌

**結果**: 只有第一個執行緒的資料被記錄。

#### 解決方案

將 `static` 改為 `static __thread`,讓每個執行緒有自己的快取:

```c
#define PROFILE_FUNCTION()                                  \
    static __thread int __func_id = -1;  /* ✅ 修正! */    \
    if (__func_id == -1) {                                  \
        __func_id = register_function(__func__);            \
    }                                                       \
    enter_function(__func_id);
```

**效果**:
- Thread A: `__func_id = 0` (Thread A 的 TLS)
- Thread B: `__func_id = 0` (Thread B 的 TLS)
- 兩者獨立,互不影響 ✅

---

### 6. 命令列參數解析

#### 參數系統設計

```c
int main(int argc, char* argv[]) {
    // 預設值
    int multi_threaded = 0;
    int shared_test = 0;
    char report_mode[32] = "per-thread";

    // 解析參數
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi-threaded") == 0) {
            multi_threaded = 1;
        } else if (strcmp(argv[i], "--shared-test") == 0) {
            shared_test = 1;
            multi_threaded = 1;
        } else if (strncmp(argv[i], "--report-mode=", 14) == 0) {
            strncpy(report_mode, argv[i] + 14, 31);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
    }

    // 根據參數執行
    if (strcmp(report_mode, "per-thread") == 0) {
        print_per_thread_reports();
    } else if (strcmp(report_mode, "merged") == 0) {
        print_merged_report();
    } else if (strcmp(report_mode, "both") == 0) {
        print_per_thread_reports();
        print_merged_report();
    }
}
```

#### 支援的參數

| 參數 | 說明 |
|------|------|
| `--multi-threaded` | 執行多執行緒測試 |
| `--shared-test` | 執行共享函數測試 |
| `--report-mode=per-thread` | 顯示各執行緒獨立報表 |
| `--report-mode=merged` | 顯示合併報表 |
| `--report-mode=both` | 同時顯示兩種報表 |
| `--help` | 顯示說明 |

---

### 7. 共享函數測試

#### 測試目的

驗證多個執行緒呼叫相同函數時,合併報表是否正確累加。

#### 測試實作

```c
void* thread_worker_shared(void* arg) {
    long thread_num = (long)arg;  // 1, 2, 3, 4

    // 各執行緒呼叫不同次數
    for (int i = 0; i < thread_num + 1; i++) {
        function_a();
        function_cpu_heavy();
    }

    register_thread_data();
    return NULL;
}

void run_shared_function_test() {
    pthread_t threads[4];

    // Thread 1: 呼叫 2 次
    // Thread 2: 呼叫 3 次
    // Thread 3: 呼叫 4 次
    // Thread 4: 呼叫 5 次
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, thread_worker_shared, (void*)(long)(i + 1));
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
}
```

#### 驗證結果

**Per-Thread 報表**:
```
=== Thread 80101 Report ===
function_a            2 calls
function_cpu_heavy    2 calls

=== Thread 80108 Report ===
function_a            3 calls
function_cpu_heavy    3 calls

=== Thread 80113 Report ===
function_a            4 calls
function_cpu_heavy    4 calls

=== Thread 80119 Report ===
function_a            5 calls
function_cpu_heavy    5 calls
```

**Merged 報表**:
```
Function          Calls  Threads
function_a           14        4    (2+3+4+5 = 14 ✅)
function_cpu_heavy   14        4    (2+3+4+5 = 14 ✅)
```

**驗證通過**: 彙總正確!

---

### 8. 記憶體管理

#### 動態分配與釋放

```c
// 分配快照
thread_data_snapshot_t* snapshot = malloc(sizeof(thread_data_snapshot_t));

// 使用快照...

// 程式結束前釋放
void cleanup_thread_snapshots() {
    for (int i = 0; i < thread_snapshot_count; i++) {
        free(thread_snapshots[i]);
        thread_snapshots[i] = NULL;
    }
    thread_snapshot_count = 0;
}
```

#### 為何使用動態分配?

- TLS 變數在執行緒結束後被釋放
- `malloc()` 分配的記憶體在堆積上,生命週期獨立於執行緒
- 主執行緒可在所有執行緒結束後存取快照資料

---

### 9. 常見問題與除錯

#### Q1: 為何只有第一個執行緒有資料?

**原因**: `static` 變數沒有改為 `static __thread`。

**檢查**:
```c
// ❌ 錯誤
#define PROFILE_FUNCTION() \
    static int __func_id = -1;

// ✅ 正確
#define PROFILE_FUNCTION() \
    static __thread int __func_id = -1;
```

#### Q2: 合併報表的 Calls 不等於各執行緒總和?

**可能原因**:
- 某些執行緒沒有呼叫 `register_thread_data()`
- 函數名稱對應錯誤 (global registry 問題)

**除錯**:
```c
// 在 register_thread_data() 加入除錯輸出
printf("Thread %d: Registered %d functions\n", tid, function_count);
```

#### Q3: 記憶體洩漏?

確保呼叫 `cleanup_thread_snapshots()`:

```c
int main() {
    // ...
    print_merged_report();

    cleanup_thread_snapshots();  // 必須呼叫!
    return 0;
}
```

---

### 10. 學習心得與收穫

#### 技術收穫

1. **TLS 資料收集模式**:
   - 執行緒結束前快照到全域堆積
   - 使用 `malloc()` 確保資料持久性
   - Mutex 保護全域快照陣列

2. **資料彙總演算法**:
   - 透過全域函數註冊表統一 ID
   - 遍歷所有執行緒累加統計資料
   - 記錄 thread_count 識別熱門函數

3. **Static __thread 陷阱**:
   - Static 變數是全域共享的
   - 儲存 thread-local 索引會造成錯誤
   - 必須使用 `static __thread` 確保執行緒隔離

#### 實作經驗

1. **命令列參數設計**:
   - 使用 `--key=value` 格式
   - 提供 `--help` 說明
   - 預設值合理化

2. **測試驗證**:
   - Shared function test 驗證彙總正確性
   - 數學驗證: 2+3+4+5 = 14 ✅
   - 除錯輸出協助問題定位

3. **報表設計**:
   - Per-thread: 適合除錯單一執行緒
   - Merged: 適合整體效能分析
   - Both: 提供完整資訊

#### 實際應用價值

**Per-Thread 報表**:
- 識別哪個執行緒最慢
- 檢查執行緒負載是否平衡
- 除錯特定執行緒的效能問題

**Merged 報表**:
- 識別整體熱點函數
- Threads 欄位顯示「並行度」
- 優化最常被呼叫的函數

**範例**:
```
Function         Calls  Threads  Total(ms)
send_request        1000       1    5000.00    ← 單執行緒瓶頸
process_data        1000      10     100.00    ← 平行處理良好
```

---

## 下一步學習

完成 **Phase 5: 動態記憶體與 Hash Table** 後,將學習:
- Hash function 設計 (針對函數指標)
- Collision resolution (Linear Probing vs Chaining)
- Dynamic resizing 與 load factor
- 記憶體管理與避免 memory leak
- 移除靜態陣列限制,支援無限函數數量

準備好開始下一階段了嗎? 請參考 `CLAUDE.md` 中的 Phase 5 實作項目!

---

## Phase 8: 視覺化輸出 (已完成)

**完成日期**: 2026-01-27

### 核心成果

實作了 Graphviz DOT 格式輸出，支援：
- Per-thread 模式：使用 subgraph cluster 分組顯示各執行緒
- Merged 模式：彙總所有執行緒資料
- 顏色編碼：紅色(>20%) → 橙色(>10%) → 黃色(>5%) → 綠色(>1%) → 藍色(≤1%)
- 呼叫次數標註在邊上
- 自動產生 PNG 圖片

### 實際成果

```bash
$ ./main --multi-threaded --export-dot
Merged call graph exported to callgraph_merged.dot
Generate image with: dot -Tpng callgraph_merged.dot -o callgraph_merged.png

$ dot -Tpng callgraph_merged.dot -o callgraph_merged.png
```

視覺化效果：
- `function_io_heavy`: 97.9% (紅色) ← 一眼看出瓶頸
- `function_cpu_heavy`: 1.9% (淺綠) ← 次要熱點

---


---

## Phase 5: 動態記憶體與 Hash Table (已完成)

**完成日期**: 2026-01-31

### 核心目標

移除靜態陣列的 1000 函數限制，實作基於 Hash Table 的動態記憶體管理，支援任意數量的函數追蹤。

### 為什麼需要 Hash Table？

**原始設計的限制**：
```c
// Phase 0-4 使用靜態陣列
__thread function_info_t functions[MAX_FUNCTIONS];  // 最多 1000 個函數
__thread time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];  // O(n²) 記憶體
```

問題：
1. **固定上限**: 無法追蹤超過 1000 個函數
2. **記憶體浪費**: caller_counts 佔用 1000×1000×8 = 8MB（即使只用 10 個函數）
3. **不可擴展**: 大型專案需要重新編譯調整 MAX_FUNCTIONS

**Hash Table 優勢**：
- ✅ **動態擴展**: 沒有函數數量上限
- ✅ **記憶體高效**: 只分配實際使用的函數（O(n) 而非 O(n²)）
- ✅ **彈性**: 無需預先知道函數數量

---

### Hash Table 架構設計

#### 三層 Hash Table 結構

```
1. hash_table_t (函數資訊)
   ├─ buckets[0] → hash_entry → hash_entry → NULL
   ├─ buckets[1] → NULL
   ├─ buckets[2] → hash_entry → NULL
   └─ ...

2. caller_counts_hash_t (caller→callees 映射)
   ├─ buckets[0] → caller_map_entry (caller="main")
   │                └─ callees (caller_hash_table_t)
   │                    ├─ "function_a" → count=3
   │                    └─ "function_b" → count=2
   └─ buckets[1] → caller_map_entry (caller="function_a")
                   └─ callees
                       └─ "function_c" → count=1
```

#### 資料結構定義

```c
// 1. 函數資訊 hash entry
typedef struct hash_entry {
    char key[256];              // 函數名
    function_info_t value;      // profiling 資料
    struct hash_entry* next;    // 鏈表（解決碰撞）
} hash_entry_t;

// 2. 函數 hash table
typedef struct {
    hash_entry_t** buckets;     // bucket 陣列
    int capacity;               // bucket 數量
    int size;                   // 已儲存的 entry 數量
} hash_table_t;

// 3. Callee 計數 entry
typedef struct caller_entry {
    char key[256];              // Callee 名稱
    time_stamp count;           // 呼叫次數
    struct caller_entry* next;
} caller_entry_t;

// 4. Caller→Callees 映射 entry
typedef struct caller_map_entry {
    char key[256];              // Caller 名稱
    caller_hash_table_t* callees;  // 該 caller 呼叫的所有 callees
    struct caller_map_entry* next;
} caller_map_entry_t;
```

---

### Hash 函數：djb2 演算法

```c
static unsigned long hash_string(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    }
    return hash;
}
```

**為什麼選擇 djb2？**
1. **速度快**: 只有位移、加法運算
2. **分佈均勻**: 對字串有良好的 hash 分佈
3. **簡單可靠**: 被廣泛使用且經過驗證
4. **無需外部函數**: 自包含實作

**使用方式**：
```c
unsigned long index = hash_string("function_name") % ht->capacity;
```

**碰撞解決**：使用 **Chaining (鏈表法)**
- 同一個 bucket 的 entries 形成鏈表
- 查找時遍歷鏈表比較 key
- 插入時加到鏈表頭部（O(1)）

---

### 核心函數重構

#### 1. 從 ID 到名稱的轉變

**Phase 0-4 (基於 ID)**:
```c
int register_function(const char *name) {
    int local_id = function_count;
    strncpy(functions[local_id].name, name, 255);
    function_count++;
    return local_id;  // 返回整數 ID
}

void enter_function(int func_id) {
    functions[func_id].call_count++;
    call_stack.stack[++call_stack.top] = func_id;  // 儲存 ID
}
```

**Phase 5 (基於名稱)**:
```c
const char* register_function(const char *name) {
    if (!functions) {
        functions = create_hash_table(512);  // 動態建立
    }

    function_info_t* func = hash_insert(functions, name);
    if (func && func->call_count == 0) {
        func->thread_id = current_thread_id;
    }

    return name;  // 返回字串指標
}

void enter_function(const char* func_name) {
    function_info_t* func = hash_find(functions, func_name);
    if (!func) {
        func = hash_insert(functions, func_name);
    }
    func->call_count++;

    // Call stack 儲存函數名而非 ID
    strncpy(call_stack.stack[call_stack.top + 1], func_name, 255);
    call_stack.top++;
}
```

#### 2. PROFILE_FUNCTION 巨集更新

**Phase 4 版本**:
```c
#define PROFILE_FUNCTION() \
    static __thread int __func_id = -1; \
    if (__func_id == -1) { \
        __func_id = register_function(__func__); \
    } \
    enter_function(__func_id); \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard = __func_id;

static inline void __profile_cleanup_int(void *p) {
    int id = *(int *)p;
    leave_function(id);
}
```

**Phase 5 版本**:
```c
#define PROFILE_FUNCTION() \
    static __thread const char* __func_name = NULL; \
    if (__func_name == NULL) { \
        __func_name = register_function(__func__); \
    } \
    enter_function(__func_name); \
    __attribute__((cleanup(__profile_cleanup_str))) const char* __profile_scope_guard = __func_name;

static inline void __profile_cleanup_str(void *p) {
    const char* name = *(const char **)p;
    leave_function(name);
}
```

**關鍵變化**:
- `int __func_id` → `const char* __func_name`
- `__profile_cleanup_int` → `__profile_cleanup_str`
- Cleanup 函數接收字串指標而非整數

---

### 深拷貝 vs 淺拷貝

#### 問題：為什麼不能用 memcpy？

**Phase 4 的 snapshot (陣列版本)**:
```c
// ✅ 陣列可以直接 memcpy
memcpy(snapshot->functions, functions, sizeof(functions));
memcpy(snapshot->caller_counts, caller_counts, sizeof(caller_counts));
```

**Phase 5 的 snapshot (hash table 版本)**:
```c
// ❌ 這樣做會崩潰！
snapshot->functions = functions;  // 淺拷貝：只複製指標

// 問題：當執行緒結束時
cleanup_thread_local();  // 釋放原始 hash table
free_hash_table(functions);

// 此時 snapshot->functions 指向已釋放的記憶體 → Segmentation Fault
```

#### 解決方案：深拷貝

```c
// Phase 5: 深拷貝 hash table
static hash_table_t* deep_copy_hash_table(hash_table_t* src) {
    if (!src) return NULL;

    // 1. 建立新的 hash table
    hash_table_t* dst = create_hash_table(src->capacity);
    if (!dst) return NULL;

    // 2. 遍歷所有 buckets
    for (int i = 0; i < src->capacity; i++) {
        hash_entry_t* entry = src->buckets[i];

        // 3. 遍歷鏈表中的所有 entries
        while (entry) {
            // 4. 為每個 entry 建立新的副本
            function_info_t* dst_func = hash_insert(dst, entry->key);
            if (dst_func) {
                *dst_func = entry->value;  // 複製 function_info_t 內容
            }
            entry = entry->next;
        }
    }
    return dst;
}
```

**深拷貝的關鍵步驟**:
1. 建立新的 hash table 結構
2. 遍歷原始 hash table 的所有 entries
3. 為每個 entry 分配新的記憶體
4. 複製資料內容（而非指標）

**記憶體示意圖**:
```
原始執行緒 TLS:
  functions → hash_table_t {
                buckets → [entry1, entry2, ...]
              }

淺拷貝（錯誤）:
  snapshot->functions → 指向同一個 hash_table_t

深拷貝（正確）:
  snapshot->functions → 新的 hash_table_t {
                          buckets → [新 entry1, 新 entry2, ...]
                        }
```

---

### Caller Counts 的深拷貝

更複雜的三層結構深拷貝：

```c
static caller_counts_hash_t* deep_copy_caller_counts(caller_counts_hash_t* src) {
    if (!src) return NULL;

    caller_counts_hash_t* dst = create_caller_counts_hash();
    if (!dst) return NULL;

    // 第一層：遍歷所有 callers
    for (int i = 0; i < src->capacity; i++) {
        caller_map_entry_t* entry = src->buckets[i];

        while (entry) {
            // 第二層：為每個 caller 建立 callees hash table
            caller_hash_table_t* dst_callees =
                caller_counts_find_or_create(dst, entry->key);

            if (dst_callees && entry->callees) {
                // 第三層：複製所有 callees
                for (int j = 0; j < entry->callees->capacity; j++) {
                    caller_entry_t* callee = entry->callees->buckets[j];

                    while (callee) {
                        time_stamp* dst_count =
                            caller_hash_insert(dst_callees, callee->key);
                        if (dst_count) {
                            *dst_count = callee->count;  // 複製計數
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
```

---

### 記憶體管理

#### 釋放 Hash Table

```c
static void free_hash_table(hash_table_t* ht) {
    if (!ht) return;

    // 1. 釋放所有 buckets 中的鏈表
    for (int i = 0; i < ht->capacity; i++) {
        hash_entry_t* entry = ht->buckets[i];
        while (entry) {
            hash_entry_t* next = entry->next;
            free(entry);  // 釋放當前 entry
            entry = next;
        }
    }

    // 2. 釋放 buckets 陣列
    free(ht->buckets);

    // 3. 釋放 hash table 結構本身
    free(ht);
}
```

#### 記憶體清理時機

1. **執行緒結束時**:
```c
void* thread_worker_cpu(void* arg) {
    // ... 執行工作 ...

    register_thread_data();  // 先儲存 snapshot
    cleanup_thread_local();  // 然後釋放 TLS hash tables

    return NULL;
}
```

2. **程式結束時**:
```c
int main() {
    // ... 執行測試 ...

    cleanup_thread_snapshots();  // 釋放所有 snapshots
    cleanup_thread_local();       // 釋放主執行緒 TLS

    return 0;
}
```

#### Valgrind 驗證

```bash
$ valgrind --leak-check=full ./main --multi-threaded

==93349== HEAP SUMMARY:
==93349==     in use at exit: 0 bytes in 0 blocks
==93349==   total heap usage: 117 allocs, 117 frees, 77,248 bytes allocated
==93349==
==93349== All heap blocks were freed -- no leaks are possible
```

✅ **完美的記憶體管理**：117 次分配 = 117 次釋放

---

### 報表函數重構

#### 從陣列遍歷到 Hash Table 遍歷

**Phase 4 (陣列)**:
```c
void print_profiling_results() {
    for (int i = 0; i < function_count; i++) {
        if (functions[i].call_count > 0) {
            printf("%-30s %10llu\n",
                functions[i].name,
                functions[i].call_count);
        }
    }
}
```

**Phase 5 (Hash Table)**:
```c
void print_profiling_results() {
    if (!functions) return;

    // 遍歷所有 buckets
    for (int i = 0; i < functions->capacity; i++) {
        hash_entry_t* entry = functions->buckets[i];

        // 遍歷 bucket 中的鏈表
        while (entry) {
            function_info_t* func = &entry->value;
            if (func->call_count > 0) {
                printf("%-30s %10llu\n",
                    func->name,
                    func->call_count);
            }
            entry = entry->next;
        }
    }
}
```

**關鍵差異**:
- 陣列：直接索引 `functions[i]`
- Hash Table：雙層遍歷（buckets + 鏈表）

---

### 遇到的問題與解決

#### Bug #1: sys_time 計算錯誤

**錯誤程式碼** (main.c:528):
```c
long long sys_delta = (end_rusage.ru_stime.tv_sec - func->start_rusage.ru_stime.tv_sec) * 1000000LL +
                      (end_rusage.ru_stime.tv_usec - func->start_rusage.ru_utime.tv_usec);
                      //                                                   ^^^^^^^ 錯誤！
```

**症狀**: sys_time 顯示超大負數（18446744073709...）

**原因**: 誤用 `ru_utime.tv_usec` 而非 `ru_stime.tv_usec`，導致計算錯誤產生負值，轉為 unsigned 後變成巨大數字

**修正**:
```c
long long sys_delta = (end_rusage.ru_stime.tv_sec - func->start_rusage.ru_stime.tv_sec) * 1000000LL +
                      (end_rusage.ru_stime.tv_usec - func->start_rusage.ru_stime.tv_usec);
                      //                                                   ^^^^^^^ 正確
```

#### Bug #2: function_count 未定義

**錯誤程式碼** (main.c:1527):
```c
if (function_count > 0) {
    register_thread_data();
}
```

**問題**: Phase 5 移除了 `function_count` 變數（改用 hash table 的 size）

**修正**:
```c
if (functions && functions->size > 0) {
    register_thread_data();
}
```

#### Bug #3: 重複定義 cleanup_thread_snapshots

**問題**: 兩個版本的 cleanup_thread_snapshots 函數
- 舊版本（簡單 free）
- 新版本（正確釋放 hash tables）

**修正**: 移除舊版本，保留正確釋放 hash tables 的新版本

---

### 效能影響分析

#### 記憶體使用比較

**Phase 4 (靜態陣列)**:
```
functions:       1000 * sizeof(function_info_t) = 1000 * ~280 bytes = 280 KB
caller_counts:   1000 * 1000 * 8 bytes = 8 MB
總計: ~8.3 MB (無論實際使用多少函數)
```

**Phase 5 (Hash Table)**:
```
假設實際使用 50 個函數:
functions:       50 * sizeof(hash_entry_t) ≈ 50 * 300 bytes = 15 KB
caller_counts:   50 個 caller * 平均 5 個 callee * 32 bytes ≈ 8 KB
總計: ~23 KB (節省 99.7% 記憶體)
```

#### 速度影響

- **查找**: O(1) 平均，O(n) 最壞（鏈表長度）
- **插入**: O(1) 平均
- **遍歷**: O(n + capacity)

實測：對於 < 100 個函數的程式，速度差異可忽略（< 1%）

---

### 學習要點總結

#### 1. Hash Table 基礎

**核心概念**:
- Key → Hash Function → Index → Bucket → Entry
- Load Factor = size / capacity（建議 < 0.75）
- Collision 不可避免，需要解決策略

**Chaining vs Open Addressing**:
- Chaining: 用鏈表儲存碰撞的 entries（本專案採用）
- Open Addressing: 尋找下一個空的 bucket（線性探測、二次探測）

**選擇 Chaining 的原因**:
- 實作簡單
- 刪除容易（直接移除鏈表節點）
- Load factor 可以 > 1

#### 2. 深拷貝的重要性

**何時需要深拷貝？**
- 資料結構包含指標
- 需要獨立的生命週期
- 多執行緒間共享資料

**深拷貝的實作步驟**:
1. 分配新記憶體
2. 遞迴複製所有層級
3. 更新指標指向新記憶體

#### 3. 記憶體管理最佳實踐

**規則**:
- 誰分配誰釋放（RAII 原則）
- 使用 Valgrind 驗證
- NULL 檢查防止 double free

**檢查清單**:
- [ ] 每個 malloc 都有對應的 free
- [ ] 釋放後設為 NULL
- [ ] 多層結構從內到外釋放

#### 4. 從 ID 到名稱的權衡

**基於 ID 的優勢**:
- ✅ 查找速度快（陣列索引 O(1)）
- ✅ 記憶體連續（cache-friendly）
- ✅ 實作簡單

**基於名稱的優勢**:
- ✅ 動態擴展
- ✅ 記憶體高效
- ✅ 無固定上限
- ❌ 查找稍慢（hash + 鏈表）

**何時選擇哪種？**
- 函數數量確定且少 → ID
- 函數數量不確定或很多 → 名稱
- 需要動態擴展 → 名稱

#### 5. GCC Cleanup 屬性與指標類型

**問題**: Cleanup 函數的參數是 `void*`，指向被清理的變數

**Phase 4 (整數)**:
```c
__attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard = __func_id;

void __profile_cleanup_int(void *p) {
    int id = *(int *)p;  // p 指向 int
    leave_function(id);
}
```

**Phase 5 (指標)**:
```c
__attribute__((cleanup(__profile_cleanup_str))) const char* __profile_scope_guard = __func_name;

void __profile_cleanup_str(void *p) {
    const char* name = *(const char **)p;  // p 指向 const char*，需要雙重解引用
    leave_function(name);
}
```

**關鍵**: `p` 是指向變數的指標，所以：
- `int` 變數 → `*(int *)p`
- `const char*` 變數 → `*(const char **)p` （指標的指標）

---

### 實戰經驗

#### 開發流程

1. **設計階段** (1 小時)
   - 繪製資料結構圖
   - 確定 hash function
   - 規劃記憶體生命週期

2. **核心實作** (2 小時)
   - Hash table 基礎函數
   - 核心函數重寫（register, enter, leave）
   - PROFILE_FUNCTION 巨集更新

3. **深拷貝機制** (1.5 小時)
   - 實作 deep_copy_hash_table
   - 實作 deep_copy_caller_counts
   - 測試 snapshot 機制

4. **報表重構** (2 小時)
   - print_profiling_results
   - print_thread_report
   - print_merged_report
   - export_dot 函數

5. **記憶體清理** (0.5 小時)
   - cleanup_thread_local
   - cleanup_thread_snapshots
   - Valgrind 驗證

6. **除錯與測試** (1 小時)
   - 修正 sys_time bug
   - 修正 function_count bug
   - 完整功能測試

**總計**: 約 8 小時

#### 除錯技巧

**技巧 #1**: 使用 printf 追蹤
```c
void hash_insert(hash_table_t* ht, const char* key) {
    printf("[DEBUG] Inserting '%s', size=%d\n", key, ht->size);
    // ...
}
```

**技巧 #2**: Valgrind 定位記憶體問題
```bash
valgrind --track-origins=yes ./main  # 追蹤未初始化的值
valgrind --leak-check=full ./main    # 檢查記憶體洩漏
```

**技巧 #3**: GDB 檢查資料結構
```bash
gdb ./main
(gdb) break hash_insert
(gdb) print *ht
(gdb) print ht->buckets[0]->key
```

---

### 總結：Phase 5 的價值

**技術成就**:
- ✅ 移除 1000 函數限制
- ✅ 記憶體使用降低 99%+（對於小型程式）
- ✅ 零記憶體洩漏（Valgrind 驗證）
- ✅ 完整的深拷貝機制
- ✅ Thread-safe 設計

**學習價值**:
- 深入理解 Hash Table 原理與實作
- 掌握動態記憶體管理
- 理解深拷貝 vs 淺拷貝
- 實踐大型重構的方法論
- 學習記憶體除錯技巧

**實用性**:
- 適合分析大型專案（1000+ 函數）
- 記憶體高效，適合嵌入式系統
- 為未來擴展打下基礎（Phase 6, 7）

**是否值得？**
- 對於 < 100 函數的小專案：Phase 4 足夠
- 對於 100-1000 函數的中型專案：Phase 5 提供更好的彈性
- 對於 1000+ 函數的大型專案：Phase 5 是必須的

Phase 5 不僅是功能升級，更是深入學習系統程式設計的絕佳機會！

## 總結

**已完成階段**:
- Phase 0: 基礎原型
- Phase 1: User/System Time 分離
- Phase 2: I/O Wait Time 估算
- Phase 3: 多執行緒支援 (TLS)
- Phase 4: 多執行緒報表輸出
- Phase 5: 動態記憶體與 Hash Table ✨ (NEW)
- Phase 8: 視覺化輸出 (Graphviz)

**未完成階段** (可作為未來擴展):
- Phase 6: gmon.out 格式輸出
- Phase 7: ELF 符號解析

專案已具備完整的 profiling、多執行緒、視覺化和動態記憶體管理功能！
