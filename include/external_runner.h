#ifndef EXTERNAL_RUNNER_H
#define EXTERNAL_RUNNER_H

/* Executes one external profiled binary, waits for gmon.out, and prints gprof output. */
int run_external_profile(const char* target_path, char* const target_args[]);

#endif
