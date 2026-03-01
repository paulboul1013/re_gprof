#include "workloads.h"

#include "profiler_core.h"
#include "reports.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Runs a tiny CPU loop used by the original demo chain. */
static void function_a(void) {
    PROFILE_FUNCTION();
    for (volatile int i = 0; i < 1000000; i++) {
    }
}

/* Runs a medium CPU loop and then calls function_a. */
static void function_b(void) {
    PROFILE_FUNCTION();
    for (volatile int i = 0; i < 500000; i++) {
    }
    function_a();
}

/* Runs a larger CPU loop and then calls function_b. */
static void function_c(void) {
    PROFILE_FUNCTION();
    for (volatile int i = 0; i < 2000000; i++) {
    }
    function_b();
}

/* Creates synchronous file I/O so wait and sys time become visible. */
static void function_io_heavy(void) {
    char buffer[4096];
    int fd;

    PROFILE_FUNCTION();

    fd = open("test_io.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
    if (fd < 0) {
        return;
    }

    memset(buffer, 'A', sizeof(buffer));
    for (int i = 0; i < 1000; i++) {
        write(fd, buffer, sizeof(buffer));
    }

    fsync(fd);
    close(fd);
    unlink("test_io.tmp");
}

/* Generates many small syscalls to surface kernel-mode accounting. */
static void function_syscall_heavy(void) {
    PROFILE_FUNCTION();
    for (int i = 0; i < 100000; i++) {
        getpid();
    }
}

/* Burns CPU cycles with floating-point math to emphasize user time. */
static void function_cpu_heavy(void) {
    volatile double result = 0.0;

    PROFILE_FUNCTION();

    for (int i = 0; i < 2000000; i++) {
        result += i * 3.14159;
        result /= (i + 1.0);
    }
}

/* Sleeps for 100ms to produce wait time with minimal CPU usage. */
static void function_sleep_test(void) {
    struct timespec sleep_time = {0, 100000000};

    PROFILE_FUNCTION();
    nanosleep(&sleep_time, NULL);
}

/* Mixes CPU, file I/O, and sleep in one profiled function. */
static void function_mixed(void) {
    struct timespec ts = {0, 50000000};
    volatile int sum = 0;
    int fd;

    PROFILE_FUNCTION();

    for (int i = 0; i < 100000; i++) {
        sum += i;
    }

    fd = open("test_mixed.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[256] = "test";
        write(fd, buf, sizeof(buf));
        close(fd);
        unlink("test_mixed.tmp");
    }

    nanosleep(&ts, NULL);
}

/* Worker thread that performs only CPU-heavy work. */
static void* thread_worker_cpu(void* arg) {
    (void)arg;
    PROFILE_SCOPE("thread_worker_cpu");

    printf("Thread %ld: Starting CPU-intensive work\n", syscall(SYS_gettid));
    for (int i = 0; i < 3; i++) {
        function_cpu_heavy();
    }
    printf("Thread %ld: CPU work done\n", syscall(SYS_gettid));

    register_thread_data();
    cleanup_current_thread_data();
    return NULL;
}

/* Worker thread that performs only I/O-heavy work. */
static void* thread_worker_io(void* arg) {
    (void)arg;
    PROFILE_SCOPE("thread_worker_io");

    printf("Thread %ld: Starting I/O-intensive work\n", syscall(SYS_gettid));
    function_io_heavy();
    printf("Thread %ld: I/O work done\n", syscall(SYS_gettid));

    register_thread_data();
    cleanup_current_thread_data();
    return NULL;
}

/* Worker thread that performs repeated sleep-heavy work. */
static void* thread_worker_sleep(void* arg) {
    (void)arg;
    PROFILE_SCOPE("thread_worker_sleep");

    printf("Thread %ld: Starting sleep work\n", syscall(SYS_gettid));
    for (int i = 0; i < 5; i++) {
        function_sleep_test();
    }
    printf("Thread %ld: Sleep work done\n", syscall(SYS_gettid));

    register_thread_data();
    cleanup_current_thread_data();
    return NULL;
}

/* Worker thread that mixes several profiling scenarios together. */
static void* thread_worker_mixed(void* arg) {
    (void)arg;
    PROFILE_SCOPE("thread_worker_mixed");

    printf("Thread %ld: Starting mixed work\n", syscall(SYS_gettid));
    function_a();
    function_b();
    function_c();
    function_mixed();
    printf("Thread %ld: Mixed work done\n", syscall(SYS_gettid));

    register_thread_data();
    cleanup_current_thread_data();
    return NULL;
}

/* Worker thread that intentionally shares the same profiled functions across threads. */
static void* thread_worker_shared(void* arg) {
    long thread_num = (long)arg;

    for (int i = 0; i < thread_num + 1; i++) {
        function_a();
        function_cpu_heavy();
    }

    register_thread_data();
    cleanup_current_thread_data();
    return NULL;
}

/* Runs the single-threaded demo sequence and prints the current report. */
void run_single_threaded_tests(void) {
    printf("\n========================================\n");
    printf("Single-threaded Tests (Phase 0, 1, 2)\n");
    printf("========================================\n");

    for (int i = 0; i < 3; i++) {
        PROFILE_SCOPE("main_loop");
        function_a();
        function_b();
        function_c();
        for (volatile int j = 0; j < 1000000; j++) {
        }
    }

    printf("Running CPU-heavy test...\n");
    function_cpu_heavy();

    printf("Running I/O-heavy test (real file with O_SYNC)...\n");
    function_io_heavy();

    printf("Running syscall-heavy test (100k getpid calls)...\n");
    function_syscall_heavy();

    printf("Running sleep test (100ms sleep - should show wait time)...\n");
    function_sleep_test();

    printf("Running mixed workload test (CPU + I/O + Sleep)...\n");
    function_mixed();

    print_profiling_results();
}

/* Runs the shared-function threading demo. */
void run_shared_function_test(void) {
    pthread_t threads[4];

    printf("\n========================================\n");
    printf("Shared Function Test (Phase 4)\n");
    printf("========================================\n");
    printf("Testing multiple threads calling same functions...\n\n");

    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, thread_worker_shared, (void*)(long)(i + 1));
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads completed!\n");
}

/* Runs the multi-threaded demo with four different worker roles. */
void run_multi_threaded_tests(void) {
    pthread_t threads[4];

    printf("\n========================================\n");
    printf("Multi-threaded Tests (Phase 3/4)\n");
    printf("========================================\n");
    printf("Creating 4 threads with different workloads...\n\n");

    pthread_create(&threads[0], NULL, thread_worker_cpu, NULL);
    pthread_create(&threads[1], NULL, thread_worker_io, NULL);
    pthread_create(&threads[2], NULL, thread_worker_sleep, NULL);
    pthread_create(&threads[3], NULL, thread_worker_mixed, NULL);

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n========================================\n");
    printf("All threads completed!\n");
    printf("========================================\n");
}
