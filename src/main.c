#include "profiler_core.h"
#include "reports.h"
#include "symbols.h"
#include "workloads.h"

#include <stdio.h>
#include <string.h>

/* Stores the parsed CLI flags used by the demo executable. */
typedef struct {
    int multi_threaded;
    int shared_test;
    int export_dot;
    int export_gmon;
    int use_sysmap;
    char resolve_symbols_path[512];
    char report_mode[32];
    char dot_mode[32];
} app_options_t;

/* Prints the CLI help text and usage examples. */
static void print_usage(const char* argv0) {
    printf("\nUsage: %s [options]\n", argv0);
    printf("Options:\n");
    printf("  --multi-threaded         Run multi-threaded tests\n");
    printf("  --shared-test            Run shared function test (multiple threads call same functions)\n");
    printf("  --report-mode=MODE       Report mode: per-thread, merged, or both (default: per-thread)\n");
    printf("  --export-dot             Export call graph to Graphviz DOT format (Phase 8)\n");
    printf("  --dot-mode=MODE          DOT export mode: per-thread or merged (default: merged)\n");
    printf("  --export-gmon            Export gmon.out binary file for gprof analysis (Phase 6)\n");
    printf("  --resolve-symbols        Resolve addresses via ELF .symtab (Phase 7)\n");
    printf("  --resolve-symbols=PATH   Use specified ELF file or System.map\n");
    printf("  --sysmap                 Treat --resolve-symbols path as System.map format\n");
    printf("  --help                   Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s                                    # Single-threaded test\n", argv0);
    printf("  %s --multi-threaded                   # Multi-threaded test, per-thread reports\n", argv0);
    printf("  %s --multi-threaded --report-mode=merged  # Multi-threaded test, merged report\n", argv0);
    printf("  %s --shared-test --report-mode=both   # Shared function test, both reports\n", argv0);
    printf("  %s --multi-threaded --export-dot      # Export merged call graph to DOT\n", argv0);
    printf("  %s --multi-threaded --export-dot --dot-mode=per-thread  # Export per-thread call graphs\n", argv0);
    printf("\n");
}

/* Fills the options struct with the repository's default runtime choices. */
static void init_app_options(app_options_t* options) {
    memset(options, 0, sizeof(*options));
    strcpy(options->report_mode, "per-thread");
    strcpy(options->dot_mode, "merged");
}

/* Parses all supported CLI flags and stops early when --help is requested. */
static int parse_args(int argc, char* argv[], app_options_t* options) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--multi-threaded") == 0) {
            options->multi_threaded = 1;
        } else if (strcmp(argv[i], "--shared-test") == 0) {
            options->shared_test = 1;
            options->multi_threaded = 1;
        } else if (strncmp(argv[i], "--report-mode=", 14) == 0) {
            strncpy(options->report_mode, argv[i] + 14, sizeof(options->report_mode) - 1);
        } else if (strcmp(argv[i], "--export-dot") == 0) {
            options->export_dot = 1;
        } else if (strncmp(argv[i], "--dot-mode=", 11) == 0) {
            strncpy(options->dot_mode, argv[i] + 11, sizeof(options->dot_mode) - 1);
        } else if (strcmp(argv[i], "--export-gmon") == 0) {
            options->export_gmon = 1;
        } else if (strncmp(argv[i], "--resolve-symbols=", 18) == 0) {
            strncpy(options->resolve_symbols_path, argv[i] + 18, sizeof(options->resolve_symbols_path) - 1);
        } else if (strcmp(argv[i], "--resolve-symbols") == 0) {
            strncpy(options->resolve_symbols_path, "/proc/self/exe", sizeof(options->resolve_symbols_path) - 1);
        } else if (strcmp(argv[i], "--sysmap") == 0) {
            options->use_sysmap = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    return 1;
}

/* Runs the selected workload set before reporting begins. */
static void run_selected_workload(const app_options_t* options) {
    if (options->shared_test) {
        run_shared_function_test();
    } else if (options->multi_threaded) {
        run_multi_threaded_tests();
    } else {
        run_single_threaded_tests();
    }
}

/* Prints reports according to the requested report mode. */
static void emit_reports(const app_options_t* options) {
    if (strcmp(options->report_mode, "per-thread") == 0) {
        print_per_thread_reports();
    } else if (strcmp(options->report_mode, "merged") == 0) {
        print_merged_report();
    } else if (strcmp(options->report_mode, "both") == 0) {
        print_per_thread_reports();
        print_merged_report();
    } else {
        printf("Unknown report mode: %s\n", options->report_mode);
        printf("Using default: per-thread\n");
        print_per_thread_reports();
    }
}

/* Runs optional DOT export after profiling data has been collected. */
static void maybe_export_dot(const app_options_t* options) {
    if (!options->export_dot) {
        return;
    }

    printf("\n");
    printf("================================================================================\n");
    printf("Exporting Call Graph (Phase 8)\n");
    printf("================================================================================\n");

    if (strcmp(options->dot_mode, "per-thread") == 0) {
        export_dot_per_thread("callgraph_per_thread.dot");
    } else if (strcmp(options->dot_mode, "merged") == 0) {
        export_dot_merged("callgraph_merged.dot");
    } else {
        printf("Unknown DOT mode: %s, using merged\n", options->dot_mode);
        export_dot_merged("callgraph_merged.dot");
    }
}

/* Runs optional gmon export after profiling data has been collected. */
static void maybe_export_gmon(const app_options_t* options, int use_merged) {
    if (!options->export_gmon) {
        return;
    }

    printf("\n");
    printf("================================================================================\n");
    printf("Exporting gmon.out (Phase 6) - %s mode\n", use_merged ? "merged" : "single-threaded");
    printf("================================================================================\n");
    export_gmon_out("gmon.out", use_merged);
}

/* Prints the symbol-resolution report for single-thread or merged data. */
static void maybe_print_symbol_report(const app_options_t* options) {
    elf_sym_table_t* sym_table;

    if (options->resolve_symbols_path[0] == '\0') {
        return;
    }

    sym_table = load_symbol_table(options->resolve_symbols_path, options->use_sysmap);
    if (!sym_table) {
        return;
    }

    if (options->multi_threaded || options->shared_test) {
        hash_table_t* merged_ht = build_merged_function_lookup();
        if (merged_ht) {
            print_symbol_report(sym_table, merged_ht);
            free_hash_table(merged_ht);
        }
    } else {
        print_symbol_report(sym_table, functions);
    }

    free_symbol_table(sym_table);
}

/* Main entry point that orchestrates profiling, workloads, and reporting. */
int main(int argc, char* argv[]) {
    app_options_t options;

    init_profiler();
    init_app_options(&options);

#ifndef AUTO_PROFILE
    register_function("main");
#endif

    if (!parse_args(argc, argv, &options)) {
        return 0;
    }

    printf("==============================================\n");
    printf("simple_gprof - Multi-threaded Profiler Demo\n");
    printf("==============================================\n");

    start_profiling();
    run_selected_workload(&options);
    stop_profiling();

    if (options.multi_threaded || options.shared_test) {
        if (functions && functions->size > 0) {
            register_thread_data();
        }
        emit_reports(&options);
        maybe_export_dot(&options);
        maybe_export_gmon(&options, 1);
        maybe_print_symbol_report(&options);
        cleanup_thread_snapshots();
    } else {
        maybe_print_symbol_report(&options);
        maybe_export_gmon(&options, 0);
    }

    printf("\nProfiling stopped.\n");
    cleanup_current_thread_data();

    return 0;
}
