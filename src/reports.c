#include "reports.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define GMON_MAGIC "gmon"
#define GMON_VERSION 1
#define GMON_TAG_TIME_HIST 0
#define GMON_TAG_CG_ARC 1

/* Maps one self-time percentage to a DOT node color. */
static const char* get_color_for_percentage(double percent) {
    if (percent > 20.0) {
        return "#FF0000";
    }
    if (percent > 10.0) {
        return "#FF8800";
    }
    if (percent > 5.0) {
        return "#FFFF00";
    }
    if (percent > 1.0) {
        return "#88FF88";
    }
    return "#AAAAFF";
}

/* Prints the current thread's flat profile and caller counts. */
void print_profiling_results(void) {
    time_stamp total_self_time = 0;
    pid_t tid;

    if (!functions) {
        return;
    }

    tid = current_thread_id ? current_thread_id : syscall(SYS_gettid);

    printf("\n=== Profiling Results (Phase 5: Thread %d) ===\n", tid);
    printf("%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Total(ms)", "Self(ms)", "User(s)", "Sys(s)", "Wait(s)", "Self%", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < functions->capacity; i++) {
        hash_entry_t* entry = functions->buckets[i];
        while (entry) {
            total_self_time += entry->value.self_time;
            entry = entry->next;
        }
    }

    for (int i = 0; i < functions->capacity; i++) {
        hash_entry_t* entry = functions->buckets[i];
        while (entry) {
            function_info_t* func = &entry->value;
            if (func->call_count > 0) {
                double total_ms = func->total_time / 1000.0;
                double self_ms = func->self_time / 1000.0;
                double user_s = func->user_time / 1000000.0;
                double sys_s = func->sys_time / 1000000.0;
                double wait_s = func->wait_time / 1000000.0;
                double self_percent = total_self_time > 0 ? (func->self_time * 100.0 / total_self_time) : 0.0;
                double avg_total = func->call_count > 0 ? (total_ms / (double)func->call_count) : 0.0;

                printf("%-30s %10llu %10.2f %10.2f %10.4f %10.4f %10.4f %9.2f%% %10.3f\n",
                    func->name, func->call_count, total_ms, self_ms,
                    user_s, sys_s, wait_s, self_percent, avg_total);
            }
            entry = entry->next;
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    if (caller_counts) {
        printf("\n--- Callers (counts) ---\n");
        for (int i = 0; i < functions->capacity; i++) {
            hash_entry_t* callee_entry = functions->buckets[i];
            while (callee_entry) {
                if (callee_entry->value.call_count == 0) {
                    callee_entry = callee_entry->next;
                    continue;
                }
                printf("%-30s <- ", callee_entry->key);

                int has_caller = 0;
                for (int j = 0; j < caller_counts->capacity; j++) {
                    caller_map_entry_t* caller_entry = caller_counts->buckets[j];
                    while (caller_entry) {
                        if (caller_entry->callees) {
                            time_stamp* count = caller_hash_find(caller_entry->callees, callee_entry->key);
                            if (count && *count > 0) {
                                has_caller = 1;
                                printf("%s(%llu) ", caller_entry->key, *count);
                            }
                        }
                        caller_entry = caller_entry->next;
                    }
                }

                if (!has_caller) {
                    printf("[none]");
                }
                printf("\n");
                callee_entry = callee_entry->next;
            }
        }
    }
}

/* Prints one flat profile for one stored thread snapshot. */
void print_thread_report(thread_data_snapshot_t* snapshot) {
    time_stamp total_self_time = 0;

    if (!snapshot || !snapshot->functions) {
        return;
    }

    printf("\n=== Thread %d Report ===\n", snapshot->thread_id);
    printf("%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Total(ms)", "Self(ms)", "User(s)", "Sys(s)", "Wait(s)", "Self%", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < snapshot->functions->capacity; i++) {
        hash_entry_t* entry = snapshot->functions->buckets[i];
        while (entry) {
            total_self_time += entry->value.self_time;
            entry = entry->next;
        }
    }

    for (int i = 0; i < snapshot->functions->capacity; i++) {
        hash_entry_t* entry = snapshot->functions->buckets[i];
        while (entry) {
            function_info_t* func = &entry->value;
            if (func->call_count > 0) {
                double total_ms = func->total_time / 1000.0;
                double self_ms = func->self_time / 1000.0;
                double user_s = func->user_time / 1000000.0;
                double sys_s = func->sys_time / 1000000.0;
                double wait_s = func->wait_time / 1000000.0;
                double self_percent = total_self_time > 0 ? (func->self_time * 100.0 / total_self_time) : 0.0;
                double avg_total = func->call_count > 0 ? (total_ms / (double)func->call_count) : 0.0;

                printf("%-30s %10llu %10.2f %10.2f %10.4f %10.4f %10.4f %9.2f%% %10.3f\n",
                    func->name, func->call_count, total_ms, self_ms,
                    user_s, sys_s, wait_s, self_percent, avg_total);
            }
            entry = entry->next;
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");
}

/* Prints every stored thread report one by one. */
void print_per_thread_reports(void) {
    printf("\n");
    printf("================================================================================\n");
    printf("Per-Thread Profiling Reports (Phase 4)\n");
    printf("================================================================================\n");
    printf("Total threads: %d\n", thread_snapshot_count);

    for (int i = 0; i < thread_snapshot_count; i++) {
        print_thread_report(thread_snapshots[i]);
    }
}

/* Prints one report that aggregates all thread snapshots into shared rows. */
void print_merged_report(void) {
    typedef struct {
        char name[256];
        time_stamp total_time;
        time_stamp self_time;
        time_stamp user_time;
        time_stamp sys_time;
        time_stamp wait_time;
        time_stamp call_count;
        int thread_count;
    } merged_function_t;

    merged_function_t merged[MAX_GLOBAL_FUNCTIONS];
    time_stamp total_self_time = 0;

    printf("\n");
    printf("================================================================================\n");
    printf("Merged Profiling Report (All Threads - Phase 5)\n");
    printf("================================================================================\n");
    printf("Total threads: %d\n", thread_snapshot_count);

    if (thread_snapshot_count == 0) {
        printf("No thread data collected.\n");
        return;
    }

    memset(merged, 0, sizeof(merged));
    for (int i = 0; i < global_function_count; i++) {
        strncpy(merged[i].name, global_function_registry[i].name, 255);
        merged[i].name[255] = '\0';
    }

    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->functions) {
            continue;
        }
        for (int b = 0; b < snapshot->functions->capacity; b++) {
            hash_entry_t* entry = snapshot->functions->buckets[b];
            while (entry) {
                if (entry->value.call_count == 0) {
                    entry = entry->next;
                    continue;
                }

                int global_id = -1;
                for (int g = 0; g < global_function_count; g++) {
                    if (strcmp(entry->value.name, global_function_registry[g].name) == 0) {
                        global_id = g;
                        break;
                    }
                }

                if (global_id >= 0) {
                    merged[global_id].total_time += entry->value.total_time;
                    merged[global_id].self_time += entry->value.self_time;
                    merged[global_id].user_time += entry->value.user_time;
                    merged[global_id].sys_time += entry->value.sys_time;
                    merged[global_id].wait_time += entry->value.wait_time;
                    merged[global_id].call_count += entry->value.call_count;
                    merged[global_id].thread_count++;
                }
                entry = entry->next;
            }
        }
    }

    printf("\n%-30s %10s %10s %10s %10s %10s %10s %10s %10s\n",
        "Function", "Calls", "Threads", "Total(ms)", "User(s)", "Sys(s)", "Wait(s)", "Avg/call", "Total/call");
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < global_function_count; i++) {
        total_self_time += merged[i].self_time;
    }
    (void)total_self_time;

    for (int i = 0; i < global_function_count; i++) {
        if (merged[i].call_count > 0) {
            double total_ms = merged[i].total_time / 1000.0;
            double user_s = merged[i].user_time / 1000000.0;
            double sys_s = merged[i].sys_time / 1000000.0;
            double wait_s = merged[i].wait_time / 1000000.0;
            double avg_per_call = merged[i].call_count > 0 ? (total_ms / (double)merged[i].call_count) : 0.0;
            double total_per_call = total_ms / (double)merged[i].call_count;

            printf("%-30s %10llu %10d %10.2f %10.4f %10.4f %10.4f %10.3f %10.3f\n",
                merged[i].name,
                merged[i].call_count,
                merged[i].thread_count,
                total_ms, user_s, sys_s, wait_s,
                avg_per_call, total_per_call);
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------------------------------\n");
}

/* Writes one per-thread call graph DOT file. */
void export_dot_per_thread(const char* filename) {
    FILE* fp = fopen(filename, "w");
    time_stamp total_self_time = 0;

    if (!fp) {
        fprintf(stderr, "Error: Cannot create %s\n", filename);
        return;
    }

    fprintf(fp, "digraph CallGraph {\n");
    fprintf(fp, "    rankdir=LR;\n");
    fprintf(fp, "    node [shape=box, style=filled];\n\n");

    for (int t = 0; t < thread_snapshot_count; t++) {
        if (!thread_snapshots[t] || !thread_snapshots[t]->functions) {
            continue;
        }
        hash_table_t* ht = thread_snapshots[t]->functions;
        for (int i = 0; i < ht->capacity; i++) {
            hash_entry_t* entry = ht->buckets[i];
            while (entry) {
                total_self_time += entry->value.self_time;
                entry = entry->next;
            }
        }
    }

    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->functions) {
            continue;
        }

        fprintf(fp, "    // Thread %d\n", snapshot->thread_id);
        fprintf(fp, "    subgraph cluster_%d {\n", snapshot->thread_id);
        fprintf(fp, "        label=\"Thread %d\";\n", snapshot->thread_id);
        fprintf(fp, "        style=dashed;\n");

        for (int i = 0; i < snapshot->functions->capacity; i++) {
            hash_entry_t* entry = snapshot->functions->buckets[i];
            while (entry) {
                if (entry->value.call_count > 0) {
                    double percent = total_self_time > 0 ? (entry->value.self_time * 100.0 / total_self_time) : 0.0;
                    const char* color = get_color_for_percentage(percent);

                    fprintf(fp, "        \"T%d_%s\" [label=\"%s\\n%.1f%%\\n%llu calls\", fillcolor=\"%s\"];\n",
                        snapshot->thread_id, entry->key, entry->key, percent, entry->value.call_count, color);
                }
                entry = entry->next;
            }
        }

        fprintf(fp, "    }\n\n");
    }

    fprintf(fp, "    // Call relationships\n");
    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->caller_counts) {
            continue;
        }
        for (int i = 0; i < snapshot->caller_counts->capacity; i++) {
            caller_map_entry_t* caller_entry = snapshot->caller_counts->buckets[i];
            while (caller_entry) {
                if (caller_entry->callees) {
                    for (int j = 0; j < caller_entry->callees->capacity; j++) {
                        caller_entry_t* callee = caller_entry->callees->buckets[j];
                        while (callee) {
                            if (callee->count > 0) {
                                fprintf(fp, "    \"T%d_%s\" -> \"T%d_%s\" [label=\"%llu\"];\n",
                                    snapshot->thread_id, caller_entry->key,
                                    snapshot->thread_id, callee->key, callee->count);
                            }
                            callee = callee->next;
                        }
                    }
                }
                caller_entry = caller_entry->next;
            }
        }
    }

    fprintf(fp, "}\n");
    fclose(fp);

    printf("Call graph exported to %s\n", filename);
    printf("Generate image with: dot -Tpng %s -o callgraph.png\n", filename);
}

/* Writes one merged call graph DOT file. */
void export_dot_merged(const char* filename) {
    typedef struct {
        char name[256];
        time_stamp self_time;
        time_stamp call_count;
        int thread_count;
    } merged_func_t;

    typedef struct {
        char caller[256];
        char callee[256];
        time_stamp count;
    } call_edge_t;

    FILE* fp = fopen(filename, "w");
    merged_func_t merged[MAX_GLOBAL_FUNCTIONS];
    call_edge_t edges[10000];
    time_stamp total_self_time = 0;
    int edge_count = 0;

    if (!fp) {
        fprintf(stderr, "Error: Cannot create %s\n", filename);
        return;
    }

    fprintf(fp, "digraph MergedCallGraph {\n");
    fprintf(fp, "    rankdir=LR;\n");
    fprintf(fp, "    node [shape=box, style=filled];\n\n");

    memset(merged, 0, sizeof(merged));
    memset(edges, 0, sizeof(edges));

    for (int i = 0; i < global_function_count; i++) {
        strncpy(merged[i].name, global_function_registry[i].name, 255);
        merged[i].name[255] = '\0';
    }

    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->functions) {
            continue;
        }

        for (int b = 0; b < snapshot->functions->capacity; b++) {
            hash_entry_t* entry = snapshot->functions->buckets[b];
            while (entry) {
                if (entry->value.call_count == 0) {
                    entry = entry->next;
                    continue;
                }

                int global_id = -1;
                for (int g = 0; g < global_function_count; g++) {
                    if (strcmp(entry->value.name, global_function_registry[g].name) == 0) {
                        global_id = g;
                        break;
                    }
                }

                if (global_id >= 0) {
                    merged[global_id].self_time += entry->value.self_time;
                    merged[global_id].call_count += entry->value.call_count;
                    merged[global_id].thread_count++;
                    total_self_time += entry->value.self_time;
                }
                entry = entry->next;
            }
        }
    }

    fprintf(fp, "    // Functions (merged from all threads)\n");
    for (int i = 0; i < global_function_count; i++) {
        if (merged[i].call_count == 0) {
            continue;
        }

        double percent = total_self_time > 0 ? (merged[i].self_time * 100.0 / total_self_time) : 0.0;
        const char* color = get_color_for_percentage(percent);

        fprintf(fp, "    \"%s\" [label=\"%s\\n%.1f%%\\n%llu calls\\n%d threads\", fillcolor=\"%s\"];\n",
            merged[i].name, merged[i].name, percent, merged[i].call_count, merged[i].thread_count, color);
    }

    for (int t = 0; t < thread_snapshot_count; t++) {
        thread_data_snapshot_t* snapshot = thread_snapshots[t];
        if (!snapshot || !snapshot->caller_counts) {
            continue;
        }

        for (int i = 0; i < snapshot->caller_counts->capacity; i++) {
            caller_map_entry_t* caller_entry = snapshot->caller_counts->buckets[i];
            while (caller_entry) {
                if (caller_entry->callees) {
                    for (int j = 0; j < caller_entry->callees->capacity; j++) {
                        caller_entry_t* callee = caller_entry->callees->buckets[j];
                        while (callee) {
                            if (callee->count > 0) {
                                int found = -1;
                                for (int e = 0; e < edge_count; e++) {
                                    if (strcmp(edges[e].caller, caller_entry->key) == 0
                                        && strcmp(edges[e].callee, callee->key) == 0) {
                                        found = e;
                                        break;
                                    }
                                }

                                if (found >= 0) {
                                    edges[found].count += callee->count;
                                } else if (edge_count < 10000) {
                                    strncpy(edges[edge_count].caller, caller_entry->key, 255);
                                    edges[edge_count].caller[255] = '\0';
                                    strncpy(edges[edge_count].callee, callee->key, 255);
                                    edges[edge_count].callee[255] = '\0';
                                    edges[edge_count].count = callee->count;
                                    edge_count++;
                                }
                            }
                            callee = callee->next;
                        }
                    }
                }
                caller_entry = caller_entry->next;
            }
        }
    }

    fprintf(fp, "\n    // Call relationships\n");
    for (int i = 0; i < edge_count; i++) {
        fprintf(fp, "    \"%s\" -> \"%s\" [label=\"%llu\"];\n",
            edges[i].caller, edges[i].callee, edges[i].count);
    }

    fprintf(fp, "}\n");
    fclose(fp);

    printf("Merged call graph exported to %s\n", filename);
    printf("Generate image with: dot -Tpng %s -o callgraph_merged.png\n", filename);
}

/* Writes profiling data in gmon.out format from TLS or merged snapshots. */
void export_gmon_out(const char* filename, int use_merged) {
    FILE* fp = fopen(filename, "wb");
    uintptr_t low_pc = UINTPTR_MAX;
    uintptr_t high_pc = 0;
    const int bin_bytes = 2;
    const uint32_t prof_rate = 100;

    if (!fp) {
        perror("export_gmon_out: fopen");
        return;
    }

    fwrite(GMON_MAGIC, 4, 1, fp);
    {
        uint32_t version = GMON_VERSION;
        char spare[12] = {0};
        fwrite(&version, sizeof(uint32_t), 1, fp);
        fwrite(spare, 12, 1, fp);
    }

    if (use_merged && thread_snapshot_count > 0) {
        pthread_mutex_lock(&snapshot_mutex);
        for (int t = 0; t < thread_snapshot_count; t++) {
            hash_table_t* ht = thread_snapshots[t]->functions;
            if (!ht) {
                continue;
            }
            for (int i = 0; i < ht->capacity; i++) {
                hash_entry_t* entry = ht->buckets[i];
                while (entry) {
                    if (entry->value.addr) {
                        uintptr_t a = (uintptr_t)entry->value.addr;
                        if (a < low_pc) {
                            low_pc = a;
                        }
                        if (a > high_pc) {
                            high_pc = a;
                        }
                    }
                    entry = entry->next;
                }
            }
        }
        pthread_mutex_unlock(&snapshot_mutex);
    } else if (!use_merged && functions) {
        for (int i = 0; i < functions->capacity; i++) {
            hash_entry_t* entry = functions->buckets[i];
            while (entry) {
                if (entry->value.addr) {
                    uintptr_t a = (uintptr_t)entry->value.addr;
                    if (a < low_pc) {
                        low_pc = a;
                    }
                    if (a > high_pc) {
                        high_pc = a;
                    }
                }
                entry = entry->next;
            }
        }
    }

    if (low_pc == UINTPTR_MAX || high_pc == 0 || high_pc <= low_pc) {
        fprintf(stderr, "export_gmon_out: no valid function addresses found\n");
        fclose(fp);
        return;
    }

    high_pc += 0x1000;

    {
        uintptr_t addr_range = high_pc - low_pc;
        int num_bins = (int)(addr_range / bin_bytes);
        double actual_bin_bytes;
        uint32_t hist_size;
        uint16_t* hist;
        uint8_t tag;

        if (num_bins > 65536) {
            num_bins = 65536;
        }
        if (num_bins <= 0) {
            num_bins = 1;
        }

        hist_size = (uint32_t)num_bins;
        hist = (uint16_t*)calloc(num_bins, sizeof(uint16_t));
        if (!hist) {
            fprintf(stderr, "export_gmon_out: out of memory for histogram\n");
            fclose(fp);
            return;
        }

        actual_bin_bytes = (double)addr_range / num_bins;

        if (use_merged && thread_snapshot_count > 0) {
            pthread_mutex_lock(&snapshot_mutex);
            for (int t = 0; t < thread_snapshot_count; t++) {
                hash_table_t* ht = thread_snapshots[t]->functions;
                if (!ht) {
                    continue;
                }
                for (int i = 0; i < ht->capacity; i++) {
                    hash_entry_t* entry = ht->buckets[i];
                    while (entry) {
                        if (entry->value.addr && entry->value.self_time > 0) {
                            uintptr_t a = (uintptr_t)entry->value.addr;
                            int bin = (int)((a - low_pc) / actual_bin_bytes);
                            long long samples = (long long)(entry->value.self_time / PROFILING_INTERVAL);
                            if (bin < 0) {
                                bin = 0;
                            }
                            if (bin >= num_bins) {
                                bin = num_bins - 1;
                            }
                            if (samples > 65535) {
                                samples = 65535;
                            }
                            hist[bin] = (uint16_t)(hist[bin] + samples > 65535 ? 65535 : hist[bin] + samples);
                        }
                        entry = entry->next;
                    }
                }
            }
            pthread_mutex_unlock(&snapshot_mutex);
        } else if (!use_merged && functions) {
            for (int i = 0; i < functions->capacity; i++) {
                hash_entry_t* entry = functions->buckets[i];
                while (entry) {
                    if (entry->value.addr && entry->value.self_time > 0) {
                        uintptr_t a = (uintptr_t)entry->value.addr;
                        int bin = (int)((a - low_pc) / actual_bin_bytes);
                        long long samples = (long long)(entry->value.self_time / PROFILING_INTERVAL);
                        if (bin < 0) {
                            bin = 0;
                        }
                        if (bin >= num_bins) {
                            bin = num_bins - 1;
                        }
                        if (samples > 65535) {
                            samples = 65535;
                        }
                        hist[bin] = (uint16_t)(hist[bin] + samples > 65535 ? 65535 : hist[bin] + samples);
                    }
                    entry = entry->next;
                }
            }
        }

        tag = GMON_TAG_TIME_HIST;
        fwrite(&tag, 1, 1, fp);
        fwrite(&low_pc, sizeof(uintptr_t), 1, fp);
        fwrite(&high_pc, sizeof(uintptr_t), 1, fp);
        fwrite(&hist_size, sizeof(uint32_t), 1, fp);
        fwrite(&prof_rate, sizeof(uint32_t), 1, fp);
        {
            char dimen[15] = "seconds        ";
            char abbrev = 's';
            fwrite(dimen, 15, 1, fp);
            fwrite(&abbrev, 1, 1, fp);
        }
        fwrite(hist, sizeof(uint16_t), num_bins, fp);
        free(hist);

        tag = GMON_TAG_CG_ARC;
        if (use_merged && thread_snapshot_count > 0) {
            pthread_mutex_lock(&snapshot_mutex);
            for (int t = 0; t < thread_snapshot_count; t++) {
                caller_counts_hash_t* cc = thread_snapshots[t]->caller_counts;
                hash_table_t* ht = thread_snapshots[t]->functions;
                if (!cc || !ht) {
                    continue;
                }
                for (int i = 0; i < cc->capacity; i++) {
                    caller_map_entry_t* cme = cc->buckets[i];
                    while (cme) {
                        function_info_t* caller_fi = hash_find(ht, cme->key);
                        void* from_pc = caller_fi ? caller_fi->addr : NULL;
                        if (from_pc && cme->callees) {
                            for (int j = 0; j < cme->callees->capacity; j++) {
                                caller_entry_t* ce = cme->callees->buckets[j];
                                while (ce) {
                                    function_info_t* callee_fi = hash_find(ht, ce->key);
                                    void* self_pc = callee_fi ? callee_fi->addr : NULL;
                                    if (self_pc && ce->count > 0) {
                                        uint32_t cnt = ce->count > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)ce->count;
                                        fwrite(&tag, 1, 1, fp);
                                        fwrite(&from_pc, sizeof(void*), 1, fp);
                                        fwrite(&self_pc, sizeof(void*), 1, fp);
                                        fwrite(&cnt, sizeof(uint32_t), 1, fp);
                                    }
                                    ce = ce->next;
                                }
                            }
                        }
                        cme = cme->next;
                    }
                }
            }
            pthread_mutex_unlock(&snapshot_mutex);
        } else if (!use_merged && caller_counts && functions) {
            for (int i = 0; i < caller_counts->capacity; i++) {
                caller_map_entry_t* cme = caller_counts->buckets[i];
                while (cme) {
                    function_info_t* caller_fi = hash_find(functions, cme->key);
                    void* from_pc = caller_fi ? caller_fi->addr : NULL;
                    if (from_pc && cme->callees) {
                        for (int j = 0; j < cme->callees->capacity; j++) {
                            caller_entry_t* ce = cme->callees->buckets[j];
                            while (ce) {
                                function_info_t* callee_fi = hash_find(functions, ce->key);
                                void* self_pc = callee_fi ? callee_fi->addr : NULL;
                                if (self_pc && ce->count > 0) {
                                    uint32_t cnt = ce->count > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)ce->count;
                                    fwrite(&tag, 1, 1, fp);
                                    fwrite(&from_pc, sizeof(void*), 1, fp);
                                    fwrite(&self_pc, sizeof(void*), 1, fp);
                                    fwrite(&cnt, sizeof(uint32_t), 1, fp);
                                }
                                ce = ce->next;
                            }
                        }
                    }
                    cme = cme->next;
                }
            }
        }
    }

    fclose(fp);
    printf("gmon.out exported to %s\n", filename);
    printf("Analyze with: gprof ./main %s\n", filename);
}
