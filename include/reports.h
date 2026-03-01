#ifndef REPORTS_H
#define REPORTS_H

#include "profiler_core.h"

/* Prints the current thread's flat profile and caller summary. */
void print_profiling_results(void);

/* Prints a flat profile for one captured thread snapshot. */
void print_thread_report(thread_data_snapshot_t* snapshot);

/* Prints all stored per-thread reports. */
void print_per_thread_reports(void);

/* Prints one report that merges all stored thread snapshots. */
void print_merged_report(void);

/* Exports one DOT file containing per-thread call graph clusters. */
void export_dot_per_thread(const char* filename);

/* Exports one DOT file containing a merged call graph across threads. */
void export_dot_merged(const char* filename);

/* Exports profiling data in gmon.out format for gprof compatibility. */
void export_gmon_out(const char* filename, int use_merged);

#endif
