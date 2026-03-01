# re_gprof

`re_gprof` 是一個專注於外部 C 執行檔分析的簡化工具。它不再提供內建 workload profiling，也不再維護自家 runtime instrumentation；目前唯一目標是執行以 `-pg` 編譯的外部程式，收集 `gmon.out`，再呼叫系統 `gprof` 顯示 flat profile 與 call graph。

## 功能

- 執行外部 `-pg` 編譯的 C 執行檔
- 在暫存目錄收集 `gmon.out`
- 自動呼叫系統 `gprof`
- 支援把參數轉發給目標程式

## 專案結構

```text
include/
  external_runner.h
src/
  external_runner.c
  main.c
```

## 需求

- Linux
- `gcc` 或 `clang`
- `make`
- `gprof`

## 建置

```bash
make
```

清理：

```bash
make clean
```

## 使用方式

先把目標程式用 `-pg` 編譯：

```bash
gcc -pg -g -o my_app my_app.c
```

再用 `re_gprof` 執行：

```bash
./main --run-target=./my_app
./main --run-target=./my_app -- arg1 arg2
```

這個流程會：

1. 在暫存目錄執行目標程式
2. 收集該次執行產生的 `gmon.out`
3. 呼叫 `gprof target gmon.out`
4. 把分析結果直接輸出到終端

## 範例

```bash
gcc -pg -g -o main2 main2.c
./main --run-target=./main2
```

## 限制

- 目標程式必須先用 `-pg` 編譯
- 目前分析輸出完全依賴系統 `gprof`
- 如果目標程式太快，`gprof` 可能只顯示呼叫次數而沒有時間樣本
- 這個工具目前不解析外部 `gmon.out` 檔案內容，也不提供自定義報表格式
