#ifndef SYMBOLS_H
#define SYMBOLS_H

#include "profiler_core.h"

typedef struct elf_sym_table elf_sym_table_t;

/* Loads symbols from an ELF binary or a System.map file, depending on the flag. */
elf_sym_table_t* load_symbol_table(const char* path, int use_sysmap);

/* Prints profiler-to-symbol matching results for one function table. */
void print_symbol_report(elf_sym_table_t* sym_table, hash_table_t* ht);

/* Releases a loaded symbol table. */
void free_symbol_table(elf_sym_table_t* sym_table);

#endif
