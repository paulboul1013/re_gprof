# re_gprof

## 摘要

這是一份基於您提供的資料與需求，為 C 語言重構版 Gprof 所設計的 `README.md` 規格書。這份文件不僅是專案的說明檔，更是一份開發藍圖。


##  專案簡介 (Introduction)

**re_gprof** 是一個為了深入理解程式效能分析（Profiling）原理而設計的教育性暨實用性專案。本專案旨在不依賴任何第三方龐大函式庫（如 Boost 或 glib）的情況下，僅使用標準 C 語言（Standard C libraries）與 POSIX 系統呼叫，重構並改進經典的 GNU gprof 工具。

透過實作此專案，開發者將學習到：
*   **Instrumentation (插樁)** 原理：編譯器如何透過 `-pg` 插入程式碼。
*   **Sampling (採樣)** 機制：如何利用 `SIGPROF` 信號進行統計分析。
*   **Call Graph (呼叫圖)** 建構：如何追蹤函數呼叫關係（Caller-Callee）。
*   **Binary Parsing**：如何解析 ELF 符號表與 `gmon.out` 二進位格式。

##  關於原版 Gprof (Original Gprof)

Gprof (GNU Profiler) 是 Unix 系統下最經典的效能分析工具。它結合了兩種分析技術：
1.  **PC Sampling (程式計數器採樣)**：透過作業系統定時中斷，紀錄目前執行到的程式位址，產生「Flat Profile」（各函數消耗時間排名）。
2.  **Call Graph Arcs (呼叫圖弧)**：透過編譯器插入的 `mcount` 函數，記錄函數被呼叫的次數與路徑，產生「Call Graph」（函數間的上下游關係）。

原版 Gprof 的核心價值在於它能指出「程式大部分的時間花在哪裡」，是優化程式碼的第一步。

##  現況分析：限制與缺點 (Current Limitations)

儘管 Gprof 經典，但在現代多核心與複雜系統下，它存在以下限制，這也是本專案試圖解決的問題：

*   **多執行緒 (Multi-threading) 支援不佳**：原版 Gprof 設計時多為單執行緒環境，其全域計數器在多執行緒下不準確，且 `gmon.out` 格式對線程區分不明顯。
*   **精度限制**：傳統採樣頻率通常為 100Hz (10ms)，對於短暫執行的函數誤差極大。
*   **無法區分 Kernel/User Mode**：傳統輸出難以精確區分該函數是因為邏輯複雜（User CPU high）還是因為系統呼叫/IO等待（Kernel/Sys time）。
*   **動態連結庫 (Shared Library) 支援麻煩**：需要額外的環境變數設定才能正確分析 `.so` 檔。
*   **遞迴 (Recursion) 處理**：在循環呼叫的計算上，原版演算法有時會造成時間歸屬的混淆。

## 核心功能規格 (Core Features)

本專案分為兩個部分：**Runtime Library (收集端)** 與 **Analyzer CLI (分析端)**。

### 1. Runtime Library (替代 glibc 的 mcount)
*   **完全相容 `-pg`**：支援 GCC/Clang 的 `mcount` / `_mcount` / `__fentry__` 介面。
*   **Thread-Safe**：使用 Thread Local Storage (TLS) 儲存各執行緒的 Profile 數據，避免 Race Condition。
*   **高精度計時**：使用 `clock_gettime(CLOCK_MONOTONIC)` 或 CPU TSC 暫存器替代傳統粗糙計時。

### 2. Analyzer CLI (分析器)
*   **gmon.out 解析**：能夠讀取標準 GCC 產生的 `gmon.out`，以及本專案增強版 Runtime 產生的數據。
*   **Flat Profile**：顯示函數執行時間、百分比、呼叫次數。
*   **Call Graph**：建立函數呼叫樹，計算 Self time 與 Children time。

## 改進與優化 (Improvements)

本專案針對原版缺點進行以下改進：

1.  **核心模式時間 (Kernel Mode Timing)**：
    *   利用 `getrusage(RUSAGE_THREAD, ...)` 在函數進出時快照，計算該函數及其子函數消耗的 System Time (Kernel Time) 與 User Time。
    *   區分 `CPU Active Time` 與 `I/O Wait Time` (透過 Wall clock time 與 CPU time 的差值估算)。
2.  **多執行緒感知 (Multi-thread Awareness)**：
    *   輸出報告將依據 Thread ID 分組，或提供合併視圖。
    *   解決鎖競爭 (Lock Contention) 導致的計時誤差。
3.  **零依賴 (Zero Dependency)**：
    *   不使用 `libelf`，自行實作簡易 ELF 解析器以讀取 Symbol Table。
    *   自行實作 Hash Table 進行符號查找，確保分析效率。
4.  **精準度提升**：
    *   修正「採樣盲點」，結合 Instrumentation 提供的精確進入/退出時間（可選模式），而非僅依賴隨機採樣。

## 6. 建置與使用方法 (Build & Usage)

### 前置需求
*   GCC 或 Clang 編譯器
*   Make
*   Linux 環境 (依賴 POSIX 信號與 procfs)

### 編譯專案
```bash
# 編譯分析工具 (gprof-ng) 與 靜態庫 (libgprof_ng.a)
make all
```

### 使用方法

#### 模式 A：分析標準 gmon.out (相容模式)
如果你只有標準編譯的程式：
```bash
# 1. 編譯你的程式 (使用標準 -pg)
gcc -pg -o my_app my_app.c

# 2. 執行程式 (產生 gmon.out)
./my_app

# 3. 使用本工具分析
./bin/gprof-ng my_app gmon.out
```

#### 模式 B：使用增強版 Runtime (推薦，支援多執行緒與 Kernel Time)
為了獲得 Kernel time 和線程安全，需連結本專案的庫：
```bash
# 1. 編譯並連結 libgprof_ng
gcc -g -pg -o my_app my_app.c -L./lib -lre_gprof

# 2. 執行程式 (產生 gmon-enhanced.out 或多個 gmon-tid.out)
./my_app

# 3. 分析並顯示 System Time 資訊
./bin/gprof-ng --kernel-mode my_app gmon-enhanced.out
```

## 7. 測試範例 (Test Cases)

本專案包含 `tests/` 目錄，提供以下情境驗證：

1.  **基礎測試 (`basic_loop.c`)**：
    *   單純的 `for` 迴圈與數學運算，驗證 Sampling 準確度。
2.  **遞迴測試 (`recursion_fib.c`)**：
    *   費氏數列計算，驗證 Call Graph 中 Cycle 的處理與計數正確性。
3.  **多執行緒測試 (`multi_thread_work.c`)**：
    *   建立 4 個 Thread 進行不同負載的工作，驗證資料是否發生 Race Condition，以及是否正確產出各 Thread 的報告。
4.  **I/O 密集測試 (`io_heavy.c`)**：
    *   大量的 `write` 與 `sleep`，驗證 Kernel Mode Time 與 Wait Time 的計算功能。

## 8. 輸出格式規格 (Output Specification)

除傳統文字報表外，本工具支援增強型輸出：

```text
--------------------------------------------------------------------------------
Flat Profile (Enhanced)
--------------------------------------------------------------------------------
 %Time   User(s)   Sys(s)    Wait(s)    Calls     Name
 40.00   0.40      0.10      0.00       1000      calculation_heavy()
 30.00   0.05      0.25      1.50       50        file_write_heavy()  <-- High Sys/Wait
 ...
```
*   **User(s)**: 使用者空間 CPU 時間。
*   **Sys(s)**: 核心空間 CPU 時間 (System Call)。
*   **Wait(s)**: 等待時間 (Wall Time - User - Sys)，通常為 I/O 或 Lock 等待。

## 9. 技術實作細節 (Implementation Details)

### 9.1 資料結構設計
為了達到高效且線程安全，核心資料結構設計如下：

```c
// 類似 gmon.out 的核心結構，但在記憶體中針對每個 Thread 獨立維護
typedef struct {
    unsigned long from_pc;  // Caller 地址
    unsigned long self_pc;  // Callee 地址
    unsigned long count;    // 呼叫次數
    struct timespec sys_time; // 累積的核心時間
    struct timespec user_time; // 累積的用戶時間
} ArcInfo;

// 使用 Hash Map 管理 Arc (不使用第三方庫，自行實作 Linear Probing Hash)
```

### 9.2 攔截機制
我們不修改編譯器，而是提供 `mcount` 的實作：
```c
void mcount(void) {
    // 1. 保存暫存器狀態 (彙編實現)
    // 2. 獲取 Caller 和 Callee 地址 (__builtin_return_address)
    // 3. 獲取當前 Thread ID
    // 4. 記錄時間 (User/Sys) 差值
    // 5. 更新 TLS 中的 Call Graph
    // 6. 恢復暫存器
}
```


---

### 補充細節 (Bonus Features)

*   **視覺化輸出 (Visualization)**：
    *   支援輸出 `.dot` 檔案格式，可配合 Graphviz 產生圖形化的 Call Graph，直觀顯示熱點路徑（Hot path 以紅色標示）。
*   **大小端相容 (Endianness Support)**：
    *   分析器包含自動偵測機制，可於 Little-endian 機器上分析 Big-endian 架構產生的 `gmon.out`（例如嵌入式 MIPS 開發）。
*   **符號解析優化**：
    *   針對 stripped binary (無符號表)，支援讀取外部 System.map 檔案進行位址對應。

*   **與原版 gmon.out 兼容**：
    *   支援分析標準 gmon.out，並提供與原版 gprof 類似的的輸出格式。

---

## 參考
https://en.wikipedia.org/wiki/Gprof
https://zhuanlan.zhihu.com/p/385842627
https://www.bigcatblog.com/profiling/
https://ftp.gnu.org/old-gnu/Manuals/gprof-2.9.1/html_mono/gprof.html