#ifndef WORKLOADS_H
#define WORKLOADS_H

/* Runs the single-threaded demo and prints the current thread report. */
void run_single_threaded_tests(void);

/* Runs the shared-function threading demo. */
void run_shared_function_test(void);

/* Runs the multi-threaded demo with different worker workloads. */
void run_multi_threaded_tests(void);

#endif
