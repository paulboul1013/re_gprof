#include "symbols.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uintptr_t addr;
    uintptr_t size;
    char name[256];
} elf_sym_t;

struct elf_sym_table {
    elf_sym_t* entries;
    int count;
    int capacity;
};

/* Allocates one growable symbol table. */
static elf_sym_table_t* elf_sym_table_create(void) {
    elf_sym_table_t* t = (elf_sym_table_t*)malloc(sizeof(elf_sym_table_t));

    if (!t) {
        return NULL;
    }

    t->capacity = 256;
    t->count = 0;
    t->entries = (elf_sym_t*)malloc(t->capacity * sizeof(elf_sym_t));
    if (!t->entries) {
        free(t);
        return NULL;
    }

    return t;
}

/* Appends one resolved symbol to the growable symbol table. */
static int elf_sym_table_add(elf_sym_table_t* t, uintptr_t addr, uintptr_t size, const char* name) {
    if (!t || !t->entries) {
        return -1;
    }

    if (t->count >= t->capacity) {
        int new_cap = t->capacity * 2;
        elf_sym_t* new_entries = (elf_sym_t*)realloc(t->entries, new_cap * sizeof(elf_sym_t));
        if (!new_entries) {
            return -1;
        }
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

/* Compares two symbols by start address for sorting and binary search. */
static int elf_sym_cmp(const void* a, const void* b) {
    const elf_sym_t* sa = (const elf_sym_t*)a;
    const elf_sym_t* sb = (const elf_sym_t*)b;

    if (sa->addr < sb->addr) {
        return -1;
    }
    if (sa->addr > sb->addr) {
        return 1;
    }
    return 0;
}

/* Loads function symbols from one ELF file's .symtab section. */
static elf_sym_table_t* elf_load_symbols(const char* path) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    size_t file_size;
    void* base;
    Elf64_Ehdr* ehdr;
    Elf64_Shdr* shdrs;
    Elf64_Shdr* shstrtab_hdr;
    const char* shstrtab;
    Elf64_Shdr* symtab_hdr = NULL;
    Elf64_Shdr* strtab_hdr = NULL;
    elf_sym_table_t* table;

    if (fd < 0) {
        perror("elf_load_symbols: open");
        return NULL;
    }

    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    file_size = (size_t)st.st_size;
    base = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        perror("elf_load_symbols: mmap");
        return NULL;
    }

    ehdr = (Elf64_Ehdr*)base;
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1
        || ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        fprintf(stderr, "elf_load_symbols: not an ELF file: %s\n", path);
        munmap(base, file_size);
        return NULL;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "elf_load_symbols: only 64-bit ELF supported\n");
        munmap(base, file_size);
        return NULL;
    }

    shdrs = (Elf64_Shdr*)((char*)base + ehdr->e_shoff);
    if (ehdr->e_shstrndx == SHN_UNDEF || ehdr->e_shstrndx >= ehdr->e_shnum) {
        fprintf(stderr, "elf_load_symbols: invalid section name table index\n");
        munmap(base, file_size);
        return NULL;
    }

    shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
    shstrtab = (const char*)base + shstrtab_hdr->sh_offset;

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

    table = elf_sym_table_create();
    if (!table) {
        munmap(base, file_size);
        return NULL;
    }

    {
        Elf64_Sym* syms = (Elf64_Sym*)((char*)base + symtab_hdr->sh_offset);
        int num_syms = (int)(symtab_hdr->sh_size / sizeof(Elf64_Sym));
        const char* strtab = (const char*)base + strtab_hdr->sh_offset;

        for (int i = 0; i < num_syms; i++) {
            if (ELF64_ST_TYPE(syms[i].st_info) == STT_FUNC && syms[i].st_value != 0) {
                const char* sym_name = strtab + syms[i].st_name;
                elf_sym_table_add(table, (uintptr_t)syms[i].st_value, (uintptr_t)syms[i].st_size, sym_name);
            }
        }
    }

    munmap(base, file_size);
    qsort(table->entries, table->count, sizeof(elf_sym_t), elf_sym_cmp);

    printf("[ELF] Loaded %d function symbols from %s\n", table->count, path);
    return table;
}

/* Resolves one runtime address to the nearest matching symbol entry. */
static const elf_sym_t* elf_resolve_addr(const elf_sym_table_t* table, uintptr_t query_addr) {
    int lo = 0;
    int hi;
    const elf_sym_t* best = NULL;

    if (!table || table->count == 0) {
        return NULL;
    }

    hi = table->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (table->entries[mid].addr <= query_addr) {
            best = &table->entries[mid];
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (!best) {
        return NULL;
    }
    if (best->size > 0 && query_addr >= best->addr + best->size) {
        return NULL;
    }
    return best;
}

/* Loads text symbols from a Linux System.map file. */
static elf_sym_table_t* sysmap_load_symbols(const char* path) {
    FILE* fp = fopen(path, "r");
    elf_sym_table_t* table;
    char line[512];
    int loaded = 0;

    if (!fp) {
        perror("sysmap_load_symbols: fopen");
        return NULL;
    }

    table = elf_sym_table_create();
    if (!table) {
        fclose(fp);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp)) {
        char addr_str[32];
        char type_str[4];
        char name[256];
        char* endptr = NULL;
        uintptr_t addr;

        if (sscanf(line, "%31s %3s %255s", addr_str, type_str, name) != 3) {
            continue;
        }
        if (type_str[0] != 'T' && type_str[0] != 't') {
            continue;
        }

        addr = (uintptr_t)strtoull(addr_str, &endptr, 16);
        if (endptr == addr_str || addr == 0) {
            continue;
        }

        if (elf_sym_table_add(table, addr, 0, name) == 0) {
            loaded++;
        }
    }

    fclose(fp);
    if (table->count > 0) {
        qsort(table->entries, table->count, sizeof(elf_sym_t), elf_sym_cmp);
    }

    printf("[ELF] Loaded %d symbols from System.map: %s\n", loaded, path);
    return table;
}

/* Picks the correct symbol loader based on the CLI flag. */
elf_sym_table_t* load_symbol_table(const char* path, int use_sysmap) {
    if (!path || path[0] == '\0') {
        return NULL;
    }

    if (use_sysmap) {
        return sysmap_load_symbols(path);
    }

    return elf_load_symbols(path);
}

/* Prints profiler addresses next to resolved ELF or System.map symbols. */
void print_symbol_report(elf_sym_table_t* sym_table, hash_table_t* ht) {
    int matched = 0;
    int total = 0;

    printf("\n================================================================================\n");
    printf("ELF Symbol Resolution Report (Phase 7)\n");
    printf("================================================================================\n");
    printf("%-40s %-18s %-18s %s\n",
        "Function (profiler)", "Profiler Addr", "ELF Addr", "Match?");
    printf("%-40s %-18s %-18s %s\n",
        "----------------------------------------", "------------------", "------------------", "-------");

    if (!ht) {
        printf("(no profiling data)\n");
        return;
    }

    for (int i = 0; i < ht->capacity; i++) {
        hash_entry_t* entry = ht->buckets[i];
        while (entry) {
            function_info_t* func = &entry->value;
            const char* match_str = "-";
            const char* elf_name = "(not found)";
            char elf_addr_str[32] = "(none)";

            total++;

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

/* Releases one loaded symbol table. */
void free_symbol_table(elf_sym_table_t* sym_table) {
    if (!sym_table) {
        return;
    }
    free(sym_table->entries);
    free(sym_table);
}
