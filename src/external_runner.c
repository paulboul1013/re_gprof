#include "external_runner.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Deletes the temporary profiling directory and its gmon.out file. */
static void cleanup_temp_dir(const char* temp_dir, const char* gmon_path) {
    if (gmon_path && gmon_path[0] != '\0') {
        unlink(gmon_path);
    }
    if (temp_dir && temp_dir[0] != '\0') {
        rmdir(temp_dir);
    }
}

/* Streams one child process pipe back to the user's terminal. */
static int stream_child_output(int read_fd) {
    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = read(read_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;
        while (total_written < bytes_read) {
            ssize_t bytes_written = write(STDOUT_FILENO, buffer + total_written, bytes_read - total_written);
            if (bytes_written < 0) {
                close(read_fd);
                return -1;
            }
            total_written += bytes_written;
        }
    }

    close(read_fd);
    return bytes_read < 0 ? -1 : 0;
}

/* Runs gprof against the generated gmon.out and forwards its report to stdout. */
static int print_gprof_report(const char* target_path, const char* gmon_path) {
    int pipefd[2];
    pid_t pid;
    int status = 0;

    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    if (pid == 0) {
        char* const gprof_argv[] = {"gprof", (char*)target_path, (char*)gmon_path, NULL};

        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execvp("gprof", gprof_argv);
        perror("execvp gprof");
        _exit(127);
    }

    close(pipefd[1]);
    if (stream_child_output(pipefd[0]) < 0) {
        perror("read gprof output");
        waitpid(pid, &status, 0);
        return 1;
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "gprof failed with status %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : status);
        return 1;
    }

    return 0;
}

/* Executes the target binary inside an isolated temp directory so its gmon.out is easy to collect. */
int run_external_profile(const char* target_path, char* const target_args[]) {
    char resolved_target[PATH_MAX];
    char temp_template[] = "/tmp/re_gprofXXXXXX";
    char gmon_path[PATH_MAX];
    char* temp_dir;
    pid_t pid;
    int status = 0;

    if (!target_path || target_path[0] == '\0') {
        fprintf(stderr, "No external target path provided.\n");
        return 1;
    }

    if (!realpath(target_path, resolved_target)) {
        perror("realpath");
        return 1;
    }

    temp_dir = mkdtemp(temp_template);
    if (!temp_dir) {
        perror("mkdtemp");
        return 1;
    }

    snprintf(gmon_path, sizeof(gmon_path), "%s/gmon.out", temp_dir);

    printf("================================================================================\n");
    printf("External Target Profiling\n");
    printf("================================================================================\n");
    printf("Target: %s\n", resolved_target);
    printf("Working directory: %s\n", temp_dir);
    fflush(stdout);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        cleanup_temp_dir(temp_dir, gmon_path);
        return 1;
    }

    if (pid == 0) {
        if (chdir(temp_dir) < 0) {
            perror("chdir");
            _exit(127);
        }

        execv(resolved_target, target_args);
        perror("execv");
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        cleanup_temp_dir(temp_dir, gmon_path);
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Target process failed with status %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : status);
        cleanup_temp_dir(temp_dir, gmon_path);
        return 1;
    }

    if (access(gmon_path, F_OK) != 0) {
        fprintf(stderr, "No gmon.out generated. Compile the target with -pg and rerun.\n");
        cleanup_temp_dir(temp_dir, gmon_path);
        return 1;
    }

    printf("\nGenerated profile: %s\n\n", gmon_path);
    fflush(stdout);
    status = print_gprof_report(resolved_target, gmon_path);
    cleanup_temp_dir(temp_dir, gmon_path);

    return status;
}
