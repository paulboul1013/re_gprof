# re_gprof

`re_gprof` 是一個以 C 語言實作的 profiling 實驗專案，目標是用最少依賴重現並延伸傳統 `gprof` 的核心概念。它目前有兩條主要路徑：

- 分析專案內建的單執行緒與多執行緒示範工作負載
- 執行外部以 `-pg` 編譯的 C 執行檔，並用系統 `gprof` 輸出報表

這個專案目前是原型與研究工具，不是完整替代 `gprof` 的通用分析器。

## 目前功能

- `SIGPROF` 取樣，累積函式 `self_time`
- 以 thread-local storage 維護每個執行緒的函式資料與呼叫關係
- 使用 `clock_gettime` 與 `getrusage` 計算 wall time、user time、sys time、wait time
- 輸出單執行緒、每執行緒與合併報表
- 匯出 Graphviz DOT call graph
- 匯出 `gmon.out`
- 解析 ELF `.symtab` 或 `System.map` 做符號比對
- 執行外部 `-pg` 程式並呼叫 `gprof` 顯示結果

## 專案結構

```text
include/
  profiler_core.h
  reports.h
  symbols.h
  workloads.h
  external_runner.h
src/
  main.c
  profiler_core.c
  reports.c
  symbols.c
  workloads.c
  external_runner.c
docs/plans/
```

## 建置

需求：

- Linux
- `gcc` 或 `clang`
- `make`
- `gprof`

編譯：

```bash
make
```

清理：

```bash
make clean
```

## 使用方式

查看所有參數：

```bash
./main --help
```

執行內建單執行緒示範：

```bash
./main
```

執行多執行緒示範並輸出合併報表：

```bash
./main --multi-threaded --report-mode=merged
```

輸出 call graph：

```bash
./main --multi-threaded --export-dot
```

檢查執行中的符號對應：

```bash
./main --resolve-symbols=/proc/self/exe
```

## 分析外部 C 執行檔

先把目標程式用 `-pg` 編譯：

```bash
gcc -pg -g -o my_app my_app.c
```

再交給 `re_gprof` 執行：

```bash
./main --run-target=./my_app
./main --run-target=./my_app -- arg1 arg2
```

此模式會：

1. 在暫存目錄執行目標程式
2. 收集它產生的 `gmon.out`
3. 呼叫系統 `gprof` 輸出 flat profile 與 call graph

如果報表只有呼叫次數、沒有時間欄位，通常表示程式太快，採樣沒有累積到足夠樣本。這種情況下請增加工作量或重複執行熱點函式。

## 目前限制

- 內建報表只分析專案自身的示範工作負載，尚未直接解析外部程式的 `gmon.out`
- 外部執行檔分析目前依賴系統 `gprof`
- 非 `-pg` 編譯的目標不會產生可用的 `gmon.out`
- 專案仍在演進中，部分輸出格式與模組邊界可能調整

## 驗證範例

```bash
make
./main
./main --multi-threaded --report-mode=both
./main --run-target=./my_app
```
