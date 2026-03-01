#include "external_runner.h"

#include <stdio.h>
#include <string.h>

/* Stores the parsed CLI flags for the external analysis tool. */
typedef struct {
    int run_external_target;
    char external_target_path[512];
} app_options_t;

/* Prints the CLI help text and examples for external target analysis. */
static void print_usage(const char* argv0) {
    printf("\nUsage: %s --run-target=PATH [-- arg1 arg2 ...]\n", argv0);
    printf("Options:\n");
    printf("  --run-target=PATH        Execute an external -pg binary and analyze its gmon.out\n");
    printf("  --                       Pass remaining arguments to --run-target\n");
    printf("  --help                   Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s --run-target=./my_app\n", argv0);
    printf("  %s --run-target=./my_app -- arg1 arg2\n", argv0);
    printf("\n");
    printf("Compile the target with -pg before running it through this tool.\n\n");
}

/* Fills the options struct with default values. */
static void init_app_options(app_options_t* options) {
    memset(options, 0, sizeof(*options));
}

/* Parses supported CLI flags and returns whether execution should continue. */
static int parse_args(int argc, char* argv[], app_options_t* options, int* target_arg_index) {
    *target_arg_index = -1;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--run-target=", 13) == 0) {
            options->run_external_target = 1;
            strncpy(options->external_target_path, argv[i] + 13, sizeof(options->external_target_path) - 1);
        } else if (strcmp(argv[i], "--") == 0) {
            *target_arg_index = i + 1;
            break;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!options->run_external_target || options->external_target_path[0] == '\0') {
        fprintf(stderr, "Missing required option: --run-target=PATH\n");
        print_usage(argv[0]);
        return 0;
    }

    return 1;
}

/* Builds the argv array used to exec one external target binary. */
static void build_external_target_argv(
    app_options_t* options,
    int argc,
    char* argv[],
    int target_arg_index,
    char* target_argv[]
) {
    int out = 0;

    target_argv[out++] = options->external_target_path;
    if (target_arg_index >= 0) {
        for (int i = target_arg_index; i < argc; i++) {
            target_argv[out++] = argv[i];
        }
    }
    target_argv[out] = NULL;
}

/* Main entry point that only handles external profiled targets. */
int main(int argc, char* argv[]) {
    app_options_t options;
    int target_arg_index;
    char* target_argv[argc + 1];

    init_app_options(&options);

    if (!parse_args(argc, argv, &options, &target_arg_index)) {
        return 0;
    }

    build_external_target_argv(&options, argc, argv, target_arg_index, target_argv);
    return run_external_profile(options.external_target_path, target_argv);
}
