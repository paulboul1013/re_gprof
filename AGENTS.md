# Repository Guidelines

## 專案結構與模組配置

目前這個儲存庫是一個精簡的 C 原型，核心集中在 [`main.c`](/home/paulboul/re_gprof/main.c)，其中包含 profiler runtime、報表邏輯、CLI 參數解析與內建示範工作負載。[`Makefile`](/home/paulboul/re_gprof/Makefile) 會建置可執行檔 `main`。設計筆記與實作規劃放在 [`docs/plans/`](/home/paulboul/re_gprof/docs/plans)，較完整的背景說明則在 [`README.md`](/home/paulboul/re_gprof/README.md)、[`CLAUDE.md`](/home/paulboul/re_gprof/CLAUDE.md) 與 [`know.md`](/home/paulboul/re_gprof/know.md)。`callgraph_merged.dot`、`callgraph_merged.png`、`gmon.out` 與編譯出的 `main` 都屬於產出物，不應視為原始碼。

## 建置、測試與開發指令

執行 `make` 會使用 `gcc` 建置 profiler，並開啟 debug symbols、pthread 支援與 `AUTO_PROFILE`。執行 `make clean` 可移除 `main`、`main.o` 與 `gmon.out`。使用 `./main` 執行預設的單執行緒示範，使用 `./main --multi-threaded` 驗證多執行緒 profiling，使用 `./main --help` 查看所有模式。常用驗證指令包含 `./main --shared-test --report-mode=both`、`./main --multi-threaded --export-dot`，以及 `./main --resolve-symbols=/proc/self/exe`。

## 程式風格與命名慣例

請遵循 `main.c` 既有的 C 風格：4 個空白縮排、大括號與宣告同行，以及以簡潔的 `static` helper 封裝內部實作。函式、變數、結構與 CLI 旗標採用 `snake_case`，例如 `--report-mode`。Phase 註解應保持簡短且具體。專案目前沒有設定 formatter，因此請手動對齊周邊格式，並維持 `-Wall` 下可乾淨編譯。

## 測試指引

目前尚未有獨立的 `tests/` 目錄；回歸驗證主要依賴 `main` 內建的工作負載。修改後先執行 `make` 重新建置，再跑最受影響的模式。例如，執行緒彙總相關變更應使用 `./main --multi-threaded --report-mode=both` 驗證；符號解析相關變更則應額外執行 `./main --resolve-symbols=/proc/self/exe` 或搭配 `--sysmap`。

## Commit 與 Pull Request 指引

近期提交記錄使用類似 Conventional Commits 的前綴，例如 `feat(phase7):`、`fix(phase7):`、`test(phase7):` 與 `docs:`。commit subject 請使用祈使語氣，並在適合時標示對應 phase 或子系統。Pull request 應說明行為變更、列出驗證用過的指令，若修改報表輸出或 DOT/PNG call graph 產生流程，請附上更新後的輸出結果或截圖。
