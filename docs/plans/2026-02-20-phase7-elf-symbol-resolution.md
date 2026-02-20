# Phase 7: ELF Symbol Resolution Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在 `main.c` 中加入 ELF 符號解析器，從可執行檔的 `.symtab` 直接讀取函數名稱，並支援以 `System.map` 作為 stripped binary 的備援，讓 profiler 報表能在無插樁情況下顯示有意義的函數名稱。

**Architecture:** 新增獨立的 ELF 符號解析模組（仍在 `main.c` 內，以 `// Phase 7` 區塊標記）。核心資料結構 `elf_sym_table_t` 儲存從 ELF `.symtab` 讀出的 `(addr, name)` 對，並以位址排序供二分搜尋。在 flat profile 報表尾端加入 `Symbol Table` 區塊，顯示 ELF 解析結果，並在 `function_info_t.addr` 存在時驗證其與 ELF 符號的對應。

**Tech Stack:** C11, `<elf.h>` (Linux), `<sys/mman.h>` (mmap), GCC, Makefile

---

## 背景知識

### ELF 檔案結構（64-bit Linux）

```
ELF Header (Elf64_Ehdr, 64 bytes)
  → e_shoff: section header table 在檔案中的偏移
  → e_shnum: section 數量
  → e_shstrndx: .shstrtab 的 section index（用來查 section 名稱）

Section Header Table (Elf64_Shdr[])
  → 每個 section 有 sh_type, sh_offset, sh_size, sh_name
  → SHT_SYMTAB: .symtab（靜態符號表，strip 後會消失）
  → SHT_DYNSYM: .dynsym（動態符號表，strip 後仍保留）
  → SHT_STRTAB: 字串表（配合 .symtab 使用）

.symtab (Elf64_Sym[])
  → st_name: 在 .strtab 中的字串偏移
  → st_value: 符號位址
  → st_size: 符號大小
  → st_info: 高 4 bit = binding, 低 4 bit = type
  → ELF64_ST_TYPE(st_info) == STT_FUNC: 函數符號
```

### System.map 格式（更簡單）

```
ffffffff81000000 T startup_64
ffffffff81000030 T secondary_startup_64
...
```
每行：`<hex_addr> <type> <name>`，type 為 T/t（text，程式碼區段）。

---

## Task 1：定義 ELF 符號表資料結構

**Files:**
- Modify: `main.c`（在 Phase 6 常數定義之後，新增 Phase 7 區塊）

### Step 1: 在 main.c 頂端加入 Phase 7 include

找到這段：
```c
#include <stdint.h>       // Phase 6: uintptr_t
```

在其後插入：
```c
#include <elf.h>          // Phase 7: ELF format structures
#include <sys/mman.h>     // Phase 7: mmap for reading ELF file
#include <sys/stat.h>     // Phase 7: stat for file size
```

### Step 2: 在 `// Phase 6: gmon.out format constants` 區塊後插入

```c
// ============================================================
// Phase 7: ELF Symbol Resolution
// ============================================================

// One entry in our symbol table: address + name
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
```

### Step 3: 驗證編譯

```bash
make
```

預期：編譯成功，無新警告。

### Step 4: Commit

```bash
git add main.c
git commit -m "feat(phase7): add ELF symbol table data structures"
```

---

## Task 2：實作 `elf_load_symbols()`

從 ELF 可執行檔解析 `.symtab` + `.strtab`，建立函數符號表。

**Files:**
- Modify: `main.c`（在 Task 1 結構定義之後插入函數實作）

### Step 1: 插入輔助函數 `elf_sym_table_create` / `elf_sym_table_add`

```c
static elf_sym_table_t* elf_sym_table_create(void) {
    elf_sym_table_t* t = (elf_sym_table_t*)malloc(sizeof(elf_sym_table_t));
    if (!t) return NULL;
    t->capacity = 256;
    t->count = 0;
    t->entries = (elf_sym_t*)malloc(t->capacity * sizeof(elf_sym_t));
    if (!t->entries) { free(t); return NULL; }
    return t;
}

static void elf_sym_table_add(elf_sym_table_t* t, uintptr_t addr, uintptr_t size, const char* name) {
    if (t->count >= t->capacity) {
        t->capacity *= 2;
        t->entries = (elf_sym_t*)realloc(t->entries, t->capacity * sizeof(elf_sym_t));
        if (!t->entries) return;
    }
    t->entries[t->count].addr = addr;
    t->entries[t->count].size = size;
    strncpy(t->entries[t->count].name, name, 255);
    t->entries[t->count].name[255] = '\0';
    t->count++;
}

static int elf_sym_cmp(const void* a, const void* b) {
    const elf_sym_t* sa = (const elf_sym_t*)a;
    const elf_sym_t* sb = (const elf_sym_t*)b;
    if (sa->addr < sb->addr) return -1;
    if (sa->addr > sb->addr) return 1;
    return 0;
}

void elf_free_sym_table(elf_sym_table_t* t) {
    if (!t) return;
    free(t->entries);
    free(t);
}
```

### Step 2: 插入主函數 `elf_load_symbols()`

```c
// Phase 7: Load function symbols from ELF binary (.symtab section)
// Returns NULL if file cannot be opened or has no .symtab
elf_sym_table_t* elf_load_symbols(const char* path) {
    // Open and mmap the file
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
    if (ehdr->e_shstrndx == SHN_UNDEF) {
        munmap(base, file_size);
        return NULL;
    }
    Elf64_Shdr* shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
    const char* shstrtab = (const char*)base + shstrtab_hdr->sh_offset;

    // Find .symtab and its associated .strtab
    Elf64_Shdr* symtab_hdr = NULL;
    Elf64_Shdr* strtab_hdr = NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char* sec_name = shstrtab + shdrs[i].sh_name;
        if (shdrs[i].sh_type == SHT_SYMTAB && strcmp(sec_name, ".symtab") == 0) {
            symtab_hdr = &shdrs[i];
            // .strtab linked via sh_link
            if (shdrs[i].sh_link < ehdr->e_shnum) {
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
        // Only keep function symbols with non-zero address
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
```

### Step 3: 編譯驗證

```bash
make
```

預期：成功，無錯誤。

### Step 4: Commit

```bash
git add main.c
git commit -m "feat(phase7): implement elf_load_symbols() ELF .symtab parser"
```

---

## Task 3：實作 `elf_resolve_addr()` 二分搜尋

**Files:**
- Modify: `main.c`（緊接 `elf_load_symbols()` 之後）

### Step 1: 插入 `elf_resolve_addr()`

```c
// Phase 7: Resolve an address to function name using binary search.
// Returns the symbol whose address is <= addr and addr < addr+size.
// If size is 0 (unknown), returns closest symbol with addr <= query_addr.
const elf_sym_t* elf_resolve_addr(const elf_sym_table_t* table, uintptr_t query_addr) {
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
```

### Step 2: 快速驗證（手動在 main() 加入臨時測試，執行後移除）

在 `main()` 的 `return 0;` 前暫時加入：

```c
// Temp test: resolve our own function addresses
elf_sym_table_t* sym_table = elf_load_symbols("/proc/self/exe");
if (sym_table) {
    void* fn = (void*)function_a;
    const elf_sym_t* sym = elf_resolve_addr(sym_table, (uintptr_t)fn);
    printf("[ELF TEST] function_a addr=%p -> %s\n", fn, sym ? sym->name : "(not found)");
    elf_free_sym_table(sym_table);
}
```

```bash
make && ./main 2>&1 | grep "ELF"
```

預期輸出類似：
```
[ELF] Loaded 42 function symbols from /proc/self/exe
[ELF TEST] function_a addr=0x401234 -> function_a
```

### Step 3: 移除臨時測試程式碼

### Step 4: Commit

```bash
git add main.c
git commit -m "feat(phase7): implement elf_resolve_addr() binary search"
```

---

## Task 4：實作 `sysmap_load_symbols()`

支援 `System.map` 格式作為備援（當 binary 被 strip 時）。

**Files:**
- Modify: `main.c`

### Step 1: 插入 `sysmap_load_symbols()`

```c
// Phase 7: Load symbols from System.map format file.
// Format: "<hex_addr> <type> <name>" one per line.
// Only loads type 'T' or 't' (text/code section symbols).
elf_sym_table_t* sysmap_load_symbols(const char* path) {
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

        // Only text (code) symbols
        if (type_str[0] != 'T' && type_str[0] != 't') continue;

        uintptr_t addr = (uintptr_t)strtoull(addr_str, NULL, 16);
        if (addr == 0) continue;

        elf_sym_table_add(table, addr, 0, name);
        loaded++;
    }
    fclose(fp);

    qsort(table->entries, table->count, sizeof(elf_sym_t), elf_sym_cmp);
    printf("[ELF] Loaded %d symbols from System.map: %s\n", loaded, path);
    return table;
}
```

### Step 2: Commit

```bash
git add main.c
git commit -m "feat(phase7): implement sysmap_load_symbols() for System.map format"
```

---

## Task 5：新增 `--resolve-symbols` CLI 選項與 Symbol Table 報表

整合 ELF 符號解析到 profiler 報表中。

**Files:**
- Modify: `main.c`（`main()` 函數 + 新增報表函數）

### Step 1: 新增全域符號表指標（在全域變數區塊）

```c
// Phase 7: Global ELF symbol table (loaded once, shared across reports)
static elf_sym_table_t* g_sym_table = NULL;
```

### Step 2: 新增 `print_elf_symbol_report()` 函數

在 `main()` 之前插入：

```c
// Phase 7: Print ELF symbol resolution report
// Compares profiler-captured addresses against ELF symbols
void print_elf_symbol_report(elf_sym_table_t* sym_table, hash_table_t* ht) {
    printf("\n================================================================================\n");
    printf("ELF Symbol Resolution Report (Phase 7)\n");
    printf("================================================================================\n");
    printf("%-40s %-18s %-18s %s\n", "Function (profiler)", "Profiler Addr", "ELF Addr", "Match?");
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
                    snprintf(elf_addr_str, sizeof(elf_addr_str), "0x%016lx", sym->addr);
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
                   func->addr ? (uintptr_t)func->addr : 0UL,
                   elf_addr_str,
                   match_str,
                   elf_name);

            entry = entry->next;
        }
    }

    printf("\nSummary: %d/%d functions matched ELF symbols\n", matched, total);

    // Also print all ELF symbols (sorted by address)
    printf("\n--- All ELF Function Symbols (%d total) ---\n", sym_table ? sym_table->count : 0);
    if (sym_table) {
        printf("%-18s %-10s %s\n", "Address", "Size", "Name");
        for (int i = 0; i < sym_table->count; i++) {
            printf("0x%016lx %-10lu %s\n",
                   sym_table->entries[i].addr,
                   sym_table->entries[i].size,
                   sym_table->entries[i].name);
        }
    }
}
```

### Step 3: 在 `main()` 解析參數區段加入新旗標

在 `int export_gmon = 0;` 之後加入：
```c
char resolve_symbols_path[512] = "";  // Phase 7: path to ELF or System.map
int use_sysmap = 0;                   // Phase 7: use System.map format
```

在 `for` 迴圈中加入：
```c
} else if (strncmp(argv[i], "--resolve-symbols=", 18) == 0) {
    strncpy(resolve_symbols_path, argv[i] + 18, 511);
} else if (strcmp(argv[i], "--resolve-symbols", 17) == 0) {
    // Auto: use /proc/self/exe
    strncpy(resolve_symbols_path, "/proc/self/exe", 511);
} else if (strcmp(argv[i], "--sysmap", 8) == 0) {
    use_sysmap = 1;
```

**注意**：正確的字串比較如下（修正上面的寫法）：

```c
} else if (strncmp(argv[i], "--resolve-symbols=", 18) == 0) {
    strncpy(resolve_symbols_path, argv[i] + 18, 511);
} else if (strcmp(argv[i], "--resolve-symbols") == 0) {
    strncpy(resolve_symbols_path, "/proc/self/exe", 511);
} else if (strcmp(argv[i], "--sysmap") == 0) {
    use_sysmap = 1;
}
```

在 help 輸出中加入：
```c
printf("  --resolve-symbols        Resolve addresses via ELF .symtab (Phase 7)\n");
printf("  --resolve-symbols=PATH   Use specified ELF file or System.map\n");
printf("  --sysmap                 Treat --resolve-symbols path as System.map format\n");
```

### Step 4: 在 `stop_profiling()` 後加入符號解析邏輯

在 `stop_profiling();` 後插入：

```c
    // Phase 7: Load ELF symbols if requested
    if (resolve_symbols_path[0] != '\0') {
        if (use_sysmap) {
            g_sym_table = sysmap_load_symbols(resolve_symbols_path);
        } else {
            g_sym_table = elf_load_symbols(resolve_symbols_path);
        }
    }
```

### Step 5: 在報表輸出後加入 ELF report

在 `cleanup_thread_snapshots();` 前（多執行緒路徑），以及單執行緒路徑的 `if (export_gmon)` 前，分別加入：

```c
    // Phase 7: ELF symbol resolution report
    if (g_sym_table) {
        print_elf_symbol_report(g_sym_table, functions);
        elf_free_sym_table(g_sym_table);
        g_sym_table = NULL;
    }
```

### Step 6: 編譯並測試

```bash
make && ./main --resolve-symbols 2>&1 | grep -A5 "ELF"
```

預期輸出：
```
[ELF] Loaded N function symbols from /proc/self/exe
================================================================================
ELF Symbol Resolution Report (Phase 7)
...
function_a    0x00401234  0x00401234  OK (function_a)
...
Summary: N/N functions matched ELF symbols
```

### Step 7: Commit

```bash
git add main.c
git commit -m "feat(phase7): add --resolve-symbols CLI option and ELF symbol report"
```

---

## Task 6：測試 Stripped Binary

驗證 stripped binary 的行為差異。

**Files:**
- 無程式碼修改，只測試指令

### Step 1: 建立 stripped 版本並驗證符號消失

```bash
# 複製一份再 strip
cp main main_stripped
strip main_stripped
nm main_stripped 2>&1 | head -5
```

預期：
```
nm: main_stripped: no symbols
```

### Step 2: 用 stripped binary 測試 --resolve-symbols（應失敗）

```bash
./main_stripped --resolve-symbols 2>&1 | grep "ELF"
```

預期：
```
elf_load_symbols: no .symtab found in /proc/self/exe (stripped?)
```

### Step 3: 產生 System.map 作為備援

從原始 binary 用 `nm` 產生 System.map 格式：

```bash
nm main | awk '{print $1, $2, $3}' > my_system.map
head -5 my_system.map
```

### Step 4: 用 stripped binary + System.map 測試

```bash
./main_stripped --resolve-symbols=my_system.map --sysmap 2>&1 | grep -A3 "ELF"
```

預期：System.map 成功載入，函數位址仍可解析（因位址相同）。

### Step 5: 清理

```bash
rm main_stripped my_system.map
```

### Step 6: Commit（只有文件更新）

```bash
git commit -m "test(phase7): verify stripped binary fallback to System.map" --allow-empty
```

---

## Task 7：更新 CLAUDE.md 與 know.md

**Files:**
- Modify: `CLAUDE.md`（Phase 7 標記為已完成）
- Modify: `know.md`（新增 Phase 7 章節）

### Step 1: 更新 CLAUDE.md

- Phase 7 標題改為 `✅ (已完成)`
- 所有 `[ ]` 改為 `[x]`
- 新增完成日期
- 更新「當前階段」區塊
- 更新已完成/未開始清單

### Step 2: 在 know.md 新增 Phase 7 章節

章節應涵蓋：
1. ELF 格式結構（Ehdr, Shdr, Sym）
2. `.symtab` vs `.dynsym` 差異（strip 後的行為）
3. `mmap()` 讀取大型二進位檔案
4. 二分搜尋（`qsort` + 手動實作）
5. System.map 格式
6. `strip` 指令的影響

### Step 3: Commit

```bash
git add CLAUDE.md know.md
git commit -m "docs: update CLAUDE.md and know.md for Phase 7 completion"
```

---

## 執行驗證清單

完成所有 Task 後，執行以下指令全面驗證：

```bash
# 1. 基本編譯
make clean && make

# 2. 單執行緒 + ELF 解析
./main --resolve-symbols 2>&1 | tail -30

# 3. 多執行緒 + ELF 解析
./main --multi-threaded --report-mode=merged --resolve-symbols 2>&1 | grep -A 20 "ELF Symbol"

# 4. 自訂路徑
./main --resolve-symbols=/proc/self/exe 2>&1 | grep "Loaded"

# 5. System.map 格式
nm main | awk '{print $1, $2, $3}' > /tmp/test.map
./main --resolve-symbols=/tmp/test.map --sysmap | grep "Loaded"
rm /tmp/test.map
```

---

## 注意事項

1. **`-no-pie -fno-pie`**：Makefile 已有此旗標，確保位址為固定值，ELF `.symtab` 的 `st_value` 直接對應執行期位址
2. **`-rdynamic`**：已有，確保 `dladdr()` 也能解析符號（Phase 6 用）
3. **32-bit ELF**：本計畫只支援 64-bit，拒絕 `ELFCLASS32`
4. **大端序**：x86_64 為 little-endian，直接讀取 native struct 即可，無需轉換
5. **`SHN_UNDEF`**：若 `e_shstrndx == SHN_UNDEF`，表示沒有 section name table，直接 return NULL
6. **`sh_link`**：`.symtab` 的 `sh_link` 欄位指向對應的 `.strtab` section index，比直接找名稱更可靠
