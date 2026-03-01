# Repository Guidelines

## 專案結構與模組配置

目前這個儲存庫是一個精簡的外部 profiling 包裝器。CLI 入口在 [`src/main.c`](/home/paulboul/re_gprof/src/main.c)，負責解析 `--run-target` 參數並啟動外部分析流程。實際執行目標程式、收集 `gmon.out`、以及呼叫系統 `gprof` 的邏輯位於 [`src/external_runner.c`](/home/paulboul/re_gprof/src/external_runner.c)，對應的公開介面在 [`include/external_runner.h`](/home/paulboul/re_gprof/include/external_runner.h)。[`Makefile`](/home/paulboul/re_gprof/Makefile) 會建置可執行檔 `main`。長期規劃文件仍放在 [`docs/plans/`](/home/paulboul/re_gprof/docs/plans)，但目前實作以外部執行檔分析為主。

## 建置、測試與開發指令

執行 `make` 會使用 `gcc` 建置目前的 CLI 工具。執行 `make clean` 可移除 `main`、`src/*.o` 與 `gmon.out`。查看使用方式請執行 `./main --help`。驗證核心流程時，先用 `gcc -pg -g -o my_app my_app.c` 建立外部測試程式，再執行 `./main --run-target=./my_app` 或 `./main --run-target=./my_app -- arg1 arg2`。

## 程式風格與命名慣例

請遵循現有 C 風格：4 個空白縮排、大括號與宣告同行，以及以簡潔的 `static` helper 封裝內部流程。函式、變數與 CLI 旗標採用 `snake_case`，例如 `run_external_profile` 與 `--run-target`。保留目前短而直接的函式註解風格。專案沒有 formatter，因此請手動對齊周邊格式，並維持 `-Wall -Wextra` 下可乾淨編譯。

## 測試指引

目前沒有獨立的自動化測試目錄，回歸驗證以實際執行外部 `-pg` 範例程式為主。修改後至少執行一次 `make`，再以簡單的外部程式確認 `gmon.out` 有產生且 `gprof` 報表能成功輸出。如果你修改了參數轉發、暫存目錄或子行程處理，應額外測試 `./main --run-target=./my_app -- arg1 arg2`。

## Commit 與 Pull Request 指引

近期提交記錄使用類似 Conventional Commits 的前綴，例如 `feat:`、`refactor:` 與 `docs:`。commit subject 請使用祈使語氣，直接描述行為變更。Pull request 應說明影響的 CLI 行為、列出實際驗證指令，若修改了外部分析輸出，請附上代表性的終端輸出片段。
