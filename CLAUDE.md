# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案概述

**simple_gprof** 是一個輕量級的 C 語言效能分析器(profiler)實作,用於理解程式效能分析原理。此專案目前處於早期開發階段,包含一個單一的自包含實作檔案 (`main.c`)。

README.md 中描述了一個更宏大的願景(re_gprof),包括 Runtime Library 和 Analyzer CLI,但目前的實作是一個簡化的原型版本。

## 核心架構

### 設計原理

此程式碼實作了混合式效能分析:

1. **Instrumentation (插樁)**:
   - 使用 GCC `cleanup` 屬性自動追蹤函數進入/離開
   - 透過 `PROFILE_FUNCTION()` 和 `PROFILE_SCOPE(name)` 巨集實現
   - 記錄精確的呼叫次數和 total time (透過 `clock_gettime(CLOCK_MONOTONIC)`)

2. **Sampling (採樣)**:
   - 使用 `SIGPROF` 信號和 `ITIMER_PROF` 計時器 (10ms 間隔)
   - 只在 signal handler 中累計 call stack 頂端函數的 `self_time`
   - 這是統計性的測量,區分函數自身耗時與呼叫子函數的耗時

3. **Call Graph**:
   - 使用二維陣列 `caller_counts[caller][callee]` 追蹤呼叫關係
   - 透過手動維護的 call stack (`call_stack_t`) 追蹤目前執行路徑

### 關鍵資料結構

- `function_info_t`: 儲存每個函數的統計資訊
  - `total_time`: 函數從進入到離開的總時間(包含子函數)
  - `self_time`: 採樣得到的自身執行時間(不含子函數,透過 SIGPROF)
  - `call_count`: 被呼叫次數

- `caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS]`: 呼叫關係矩陣

- `call_stack_t`: 維護目前函數呼叫堆疊,用於採樣時判斷正在執行的函數

### 重要機制

**AUTO_PROFILE 模式**:
- 編譯時定義 `-DAUTO_PROFILE` 啟用自動插樁
- `PROFILE_FUNCTION()` 使用 GCC 的 `cleanup` 屬性,在函數返回時自動呼叫 `leave_function()`
- 函數 ID 使用 static local variable 懶初始化,確保每個函數只註冊一次

**Time Attribution**:
- `total_time`: 透過 `clock_gettime(CLOCK_MONOTONIC)` 在 enter/leave 時計算差值 (wall time)
- `user_time` / `sys_time`: 透過 `getrusage(RUSAGE_SELF)` 測量 CPU time (Phase 1)
- `wait_time`: 計算 wall_time - (user_time + sys_time),反映 I/O 與 sleep 等待 (Phase 2)
- `self_time`: 只在 `profiling_handler()` (SIGPROF) 中累加,只歸屬給 call stack 頂端

## 建置與使用

### 編譯

```bash
make        # 編譯主程式 (啟用 AUTO_PROFILE)
make clean  # 清理編譯產物
```

Makefile 使用 `-DAUTO_PROFILE` 和 `-g` 旗標編譯。

### 執行

```bash
./main
```

程式會自動執行測試函數並輸出兩個報表:
1. Flat Profile: 每個函數的呼叫次數、總時間、自身時間、時間百分比
2. Callers: 顯示每個函數被哪些函數呼叫及次數

### 測試用函數

**基礎測試** (Phase 0):
- `function_a()`: 簡單迴圈,無子函數呼叫
- `function_b()`: 迴圈 + 呼叫 `function_a()`
- `function_c()`: 迴圈 + 呼叫 `function_b()`

**Phase 1 & 2 測試** (User/Sys/Wait Time):
- `function_io_heavy()`: I/O 密集型 - 使用 O_SYNC 寫入實際檔案 (驗證 wait time)
- `function_cpu_heavy()`: CPU 密集型 - 200 萬次浮點運算 (驗證 user time)
- `function_syscall_heavy()`: System call 密集 - 10 萬次 `getpid()` (驗證 sys time)
- `function_sleep_test()`: Sleep 測試 - `nanosleep(100ms)` (驗證 wait time 精度)
- `function_mixed()`: 混合負載 - CPU + I/O + Sleep

**Phase 3 測試** (多執行緒):
- `thread_worker_cpu()`: 執行緒 1 - 3 次 CPU 密集運算
- `thread_worker_io()`: 執行緒 2 - I/O 密集工作
- `thread_worker_sleep()`: 執行緒 3 - 5 次 sleep 測試
- `thread_worker_mixed()`: 執行緒 4 - 混合工作負載

**Main 函數**:
- `main()`: 支援 `--multi-threaded` 參數切換單/多執行緒模式
- `run_single_threaded_tests()`: 執行 Phase 0-2 測試
- `run_multi_threaded_tests()`: 建立 4 個執行緒執行 Phase 3 測試

## 程式碼修改注意事項

### 新增 Profiled 函數

在新函數中加入 `PROFILE_FUNCTION()`:

```c
void my_function() {
    PROFILE_FUNCTION();  // 必須在函數開頭
    // your code
}
```

### 常數限制

- `MAX_FUNCTIONS`: 1000 (最大追蹤函數數量)
- `MAX_CALL_STACK`: 100 (最大遞迴深度)
- `PROFILING_INTERVAL`: 10000 微秒 (10ms 採樣間隔)

修改這些值時需注意記憶體使用量 (`caller_counts` 為 O(n²))。

### Signal Safety

`profiling_handler()` 在信號處理器中執行,需要:
- 避免呼叫非 async-signal-safe 的函數
- 目前僅進行簡單的整數運算和時間累加,符合安全要求
- 不要在 handler 中加入 malloc、printf 等操作

### Call Stack 管理

- Call stack 必須與實際執行流程一致
- `enter_function()` 和 `leave_function()` 必須成對
- GCC cleanup 屬性確保即使提前 return 也會正確清理

## 已知限制

1. **不支援多執行緒**: 全域變數 `functions`、`call_stack` 和 `caller_counts` 在多執行緒下會產生 race condition
2. **無法分析動態載入的函數**: 需要在編譯時插入 `PROFILE_FUNCTION()`
3. **記憶體限制**: 靜態陣列大小固定,不適合大型專案
4. **無持久化**: 結果僅輸出到 stdout,未產生 `gmon.out` 格式
5. **無 Kernel Time 測量**: 尚未實作 `getrusage()` 來分離 user/system time

## 開發路線圖 (Development Roadmap)

本專案採用階段性開發,每個 Phase 實作一個核心功能。完成每個階段後,會更新 `know.md` 記錄該階段的學習知識點。

### Phase 0: 基礎原型 ✅ (已完成)

**目標**: 建立基本的插樁和採樣機制

**功能**:
- [x] 基於 GCC cleanup 屬性的自動插樁
- [x] SIGPROF 信號採樣機制
- [x] Call graph 追蹤 (caller-callee 關係)
- [x] Total time 和 Self time 測量
- [x] 基本的 Flat Profile 和 Callers 報表

**驗證標準**:
- 能正確追蹤函數呼叫次數
- Self time 總和接近程式實際執行時間
- Call graph 正確顯示呼叫關係

---

### Phase 1: User/System Time 分離 ✅ (已完成)

**目標**: 使用 `getrusage()` 區分使用者態和核心態時間

**實作項目**:
- [x] 在 `function_info_t` 中新增 `user_time` 和 `sys_time` 欄位
- [x] 在 `enter_function()` 和 `leave_function()` 中呼叫 `getrusage(RUSAGE_SELF, ...)`
- [x] 計算函數執行期間的 user time 和 system time 增量
- [x] 更新報表格式,顯示 User(s) 和 Sys(s) 欄位

**測試案例**:
- [x] 建立 I/O 密集型測試函數 (`function_io_heavy`)
- [x] 建立 CPU 密集型測試函數 (`function_cpu_heavy`)
- ⚠️ I/O 函數的 sys_time 為 0 (因 `/dev/null` 優化,Phase 2 可改進)

**學習重點**:
- ✅ `getrusage()` API 的使用方式
- ✅ RUSAGE_SELF vs RUSAGE_THREAD 的差異
- ✅ User mode 與 Kernel mode 的區別
- ✅ System call 如何消耗 system time
- ✅ `struct rusage` 結構與欄位說明
- ✅ 時間計算與單位轉換

**知識點更新**: ✅ 已在 `know.md` 新增詳細的「Phase 1: User/System Time 分離」章節

**完成日期**: 2026-01-25

---

### Phase 2: I/O Wait Time 估算 ✅ (已完成)

**目標**: 透過 wall clock time 與 CPU time 的差值,估算等待時間

**實作項目**:
- [x] 新增 `wait_time` 欄位到 `function_info_t`
- [x] 使用 `clock_gettime(CLOCK_MONOTONIC)` 測量 wall time (替代 gettimeofday)
- [x] 計算 `wait_time = wall_time - (user_time + sys_time)`
- [x] 在報表中顯示 Wait(s) 欄位
- [x] 處理負值情況 (測量誤差保護)

**測試案例**:
- [x] 建立 `function_sleep_test()` 使用 `nanosleep()` (100ms)
- [x] 建立 `function_mixed()` 混合 CPU + I/O + Sleep
- ✅ wait_time 準確反映 sleep 時間 (誤差 < 1%)
- ✅ `function_io_heavy` 顯示 91% 時間在等待 I/O

**學習重點**:
- ✅ Wall clock time vs CPU time 的應用
- ✅ I/O blocking 造成的 wait time (2+ 秒)
- ✅ `clock_gettime(CLOCK_MONOTONIC)` 與 struct timespec (奈秒精度)
- ✅ 時間計算公式: Wait = Wall - (User + Sys)
- ✅ `nanosleep()` 與精確時間測量

**重要發現**:
- `function_io_heavy`: 2.02s wait time (91% 的總時間) - 典型的 I/O bound
- `function_sleep_test`: 100.1ms wait time - 與 sleep(100ms) 幾乎完全一致
- `function_cpu_heavy`: 0s wait time - 純 CPU bound,無等待

**知識點更新**: ✅ 已在 `know.md` 新增詳細的「Phase 2: I/O Wait Time 估算」章節

**完成日期**: 2026-01-25

---

### Phase 3: 多執行緒支援 - Thread Local Storage ✅ (已完成)

**目標**: 使用 TLS 讓每個執行緒擁有獨立的 profiling 資料

**實作項目**:
- [x] 將 `functions`、`caller_counts`、`call_stack` 改為 `__thread` 變數
- [x] 實作 thread-safe 的函數註冊機制 (使用 mutex 保護全域函數名稱表)
- [x] 每個執行緒獨立輸出 profiling 報表
- [x] 為每個執行緒的輸出加上 Thread ID 標記 (syscall(SYS_gettid))
- [x] 升級為 RUSAGE_THREAD (Linux) 精確測量執行緒 CPU 時間

**測試案例**:
- [x] 建立 4 個執行緒測試程式 (pthread_create)
- [x] 每個執行緒執行不同的工作負載 (CPU/IO/Sleep/Mixed)
- [x] 驗證各執行緒的統計資料互不干擾
- [x] 使用 `./main --multi-threaded` 執行測試

**學習重點**:
- ✅ Thread Local Storage (`__thread` 關鍵字)
- ✅ pthread API 基礎 (pthread_create, pthread_join)
- ✅ Race condition 與 data race 的區別與避免
- ✅ Mutex 與 critical section (pthread_mutex_t)
- ✅ RUSAGE_THREAD vs RUSAGE_SELF
- ✅ Thread ID 取得方式 (syscall(SYS_gettid))
- ✅ 編譯選項: -pthread, -D_GNU_SOURCE

**重要發現**:
- TLS 機制成功隔離各執行緒資料,無 race condition
- Global function registry 使用 mutex 保護,確保 thread-safe 註冊
- 4 個執行緒同時執行,各自輸出獨立報表,Thread ID 正確顯示
- RUSAGE_THREAD 精確測量各執行緒的 user/sys time

**知識點更新**: ✅ 已在 `know.md` 新增詳細的「Phase 3: 多執行緒支援」章節

**完成日期**: 2026-01-26

---

### Phase 4: 多執行緒報表輸出 ✅ (已完成)

**目標**: 提供依 Thread 分組或合併的報表視圖

**實作項目**:
- [x] 建立執行緒資料收集機制 (TLS → 全域快照)
- [x] 實作 `register_thread_data()` 複製執行緒 TLS 資料
- [x] 實作分執行緒報表輸出模式 (`--report-mode=per-thread`)
- [x] 實作合併報表模式 (`--report-mode=merged`)
- [x] 實作雙報表模式 (`--report-mode=both`)
- [x] 新增命令列參數控制報表模式
- [x] 修正 `PROFILE_FUNCTION` 巨集,將 `static` 變數改為 `static __thread`
- [x] 新增 `--shared-test` 測試多執行緒共享函數

**測試案例**:
- [x] 多執行緒程式同時呼叫相同函數 (Thread 1: 2次, Thread 2: 3次, Thread 3: 4次, Thread 4: 5次)
- [x] 驗證分執行緒模式下各執行緒的 call_count 正確 (2, 3, 4, 5)
- [x] 驗證合併模式下 call_count 為所有執行緒總和 (14 = 2+3+4+5) ✅
- [x] 驗證 Threads 欄位正確顯示呼叫該函數的執行緒數量

**學習重點**:
- ✅ TLS 資料收集與快照機制
- ✅ 動態記憶體分配 (`malloc`) 儲存執行緒快照
- ✅ 資料彙總演算法 (跨執行緒累加)
- ✅ 命令列參數解析 (`--report-mode=`, `--shared-test`)
- ✅ Static __thread 變數修正 (解決多執行緒 ID 衝突)

**重要發現**:
- `static` 變數在多執行緒下會造成 ID 衝突,必須改為 `static __thread`
- 執行緒結束前必須呼叫 `register_thread_data()` 複製 TLS 資料
- 合併報表需要透過全域函數註冊表對應函數 ID
- Merged report 的 Threads 欄位可識別函數的「熱門程度」

**知識點更新**: ✅ 已在 `know.md` 新增詳細的「Phase 4: 多執行緒報表輸出」章節

**完成日期**: 2026-01-26

---

### Phase 5: 動態記憶體與 Hash Table

**目標**: 移除靜態陣列限制,實作可擴展的資料結構

**實作項目**:
- [ ] 實作簡易 Hash Table (Linear Probing 或 Chaining)
- [ ] 將 `functions` 陣列改為動態分配的 Hash Table
- [ ] 將 `caller_counts` 二維陣列改為 Hash Table of Hash Table
- [ ] 實作動態擴容機制

**測試案例**:
- 建立超過 1000 個函數的測試程式 (透過迴圈動態產生 PROFILE_SCOPE)
- 驗證記憶體使用量隨函數數量線性成長

**學習重點**:
- Hash function 設計 (針對函數指標)
- Collision resolution (Linear Probing vs Chaining)
- Dynamic resizing 與 load factor
- 記憶體管理與避免 memory leak

**知識點更新**: 完成後在 `know.md` 新增「Phase 5: Hash Table 實作」章節

---

### Phase 6: gmon.out 格式輸出

**目標**: 支援標準 gmon.out 格式,相容原版 gprof

**實作項目**:
- [ ] 研究 gmon.out 二進位格式規格
- [ ] 實作 gmon header 寫入
- [ ] 實作 histogram (PC sampling) 資料寫入
- [ ] 實作 call graph arcs 寫入
- [ ] 使用標準 gprof 驗證輸出檔案

**測試案例**:
- 產生 gmon.out 後使用 `gprof ./main gmon.out` 驗證
- 比對本工具輸出與 gprof 輸出的一致性

**學習重點**:
- Binary file I/O in C
- gmon.out 檔案格式結構
- Histogram bins 的計算
- 大小端 (Endianness) 問題

**知識點更新**: 完成後在 `know.md` 新增「Phase 6: gmon.out 格式」章節

---

### Phase 7: ELF 符號解析 (進階選用)

**目標**: 從 stripped binary 或外部符號表讀取函數名稱

**實作項目**:
- [ ] 實作 ELF header 解析
- [ ] 實作 section header table 解析
- [ ] 讀取 `.symtab` 和 `.strtab` sections
- [ ] 將函數位址對應到符號名稱
- [ ] 支援讀取外部 System.map 檔案

**測試案例**:
- 建立 stripped binary (`strip ./main`)
- 提供外部符號表並驗證能正確顯示函數名稱

**學習重點**:
- ELF (Executable and Linkable Format) 結構
- Symbol table 格式
- 位址到符號的對應
- `nm` 和 `objdump` 工具的原理

**知識點更新**: 完成後在 `know.md` 新增「Phase 7: ELF 解析」章節

---

### Phase 8: 視覺化輸出 (進階選用)

**目標**: 產生 Graphviz DOT 格式的 Call Graph

**實作項目**:
- [ ] 實作 DOT 格式輸出
- [ ] 使用顏色標示 hot path (高 self_time 的函數)
- [ ] 在邊上標示呼叫次數
- [ ] 整合 `dot` 命令自動產生圖片

**測試案例**:
- 產生 `callgraph.dot`
- 執行 `dot -Tpng callgraph.dot -o callgraph.png`
- 視覺化驗證 call graph 正確性

**學習重點**:
- DOT 語言基礎
- 圖形化資料視覺化
- 整合外部工具

**知識點更新**: 完成後在 `know.md` 新增「Phase 8: 視覺化」章節

---

## 當前階段

**目前位於**: Phase 4 (已完成) ✅
**下一步**: Phase 5 - 動態記憶體與 Hash Table

**Phase 4 完成摘要**:
- ✅ 建立執行緒資料收集機制 (快照 TLS 到堆積)
- ✅ 實作三種報表模式: per-thread, merged, both
- ✅ 修正 `static __thread` 變數問題
- ✅ 新增 `--shared-test` 驗證合併正確性
- ✅ Merged report 正確彙總多執行緒資料
- ✅ 命令列參數系統完善 (`--help`, `--report-mode=`)
- ✅ 詳細記錄學習成果於 `know.md`

**已完成階段**:
- Phase 0: 基礎原型 (插樁 + 採樣)
- Phase 1: User/System Time 分離
- Phase 2: I/O Wait Time 估算
- Phase 3: 多執行緒支援 (Thread Local Storage)
- Phase 4: 多執行緒報表輸出 (Per-thread & Merged Reports)

**執行方式**:
```bash
make                                              # 編譯
./main                                            # 單執行緒測試
./main --help                                     # 顯示說明
./main --multi-threaded                           # 多執行緒, per-thread 報表
./main --multi-threaded --report-mode=merged      # 多執行緒, 合併報表
./main --multi-threaded --report-mode=both        # 多執行緒, 兩種報表
./main --shared-test --report-mode=both           # 共享函數測試
```

開始實作下一個 phase 前,請先閱讀對應的「實作項目」和「學習重點」,並在完成後更新 `know.md` 記錄學習成果。

## 與 README 願景的對應

README.md 中的改進項目對應到以下 phases:
- **核心模式時間**: Phase 1, Phase 2
- **多執行緒感知**: Phase 3, Phase 4
- **零依賴**: Phase 5 (Hash Table), Phase 7 (ELF 解析)
- **精準度提升**: Phase 1, Phase 2 (更精確的時間歸屬)
- **gmon.out 支援**: Phase 6

## 術語對照

- **Flat Profile**: 扁平分析,列出所有函數的獨立統計
- **Call Graph**: 呼叫圖,顯示函數間的呼叫關係
- **Self Time**: 自身時間,不包含子函數執行時間
- **Total Time**: 總時間,包含子函數執行時間
- **Instrumentation**: 插樁,在程式碼中插入測量程式碼
- **Sampling**: 採樣,定時中斷檢查程式執行狀態
