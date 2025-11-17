
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <execinfo.h>


#define MAX_FUNCTIONS 1000
#define MAX_CALL_STACK 100
#define PROFILING_INTERVAL 10000 //sample interval 10ms


typedef unsigned long long time_stamp;

typedef struct {
    char name[256];
    time_stamp total_time;
    time_stamp self_time;
    time_stamp call_count;
    int is_active;
    struct timeval start_time;
} function_info_t;


static function_info_t functions[MAX_FUNCTIONS];
static time_stamp caller_counts[MAX_FUNCTIONS][MAX_FUNCTIONS];
static struct itimerval  timer;
static int function_count=0;
static int profiling_enabled=0;

typedef struct {
    int stack[MAX_CALL_STACK];
    int top;
} call_stack_t;

static call_stack_t call_stack={.top=-1};


void profiling_handler(int sig){
    if (!profiling_enabled) return;

    static int init=0;
    static struct timeval last_sample;
    struct timeval current_time;

    gettimeofday(&current_time,NULL);

    if (!init){
        last_sample=current_time;
        init=1;
        return;
    }

    //count time interval
    long interval_us=(current_time.tv_sec-last_sample.tv_sec)*1000000 +
    (current_time.tv_usec-last_sample.tv_usec);

    if (call_stack.top>=0){
        // 純取樣自有時間：只累計堆疊頂端函式的 self_time
        int current_func = call_stack.stack[call_stack.top];
        functions[current_func].self_time += interval_us;
    }

    last_sample=current_time;
}

void init_profilier(){
    //set timer
    timer.it_interval.tv_sec=0;
    timer.it_interval.tv_usec=PROFILING_INTERVAL;
    timer.it_value=timer.it_interval;

    //set signal handler
    struct sigaction sa;
    sa.sa_handler=profiling_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=0;
    sigaction(SIGPROF,&sa,NULL);
    
}


void start_profiling() {
    profiling_enabled=1;
    setitimer(ITIMER_PROF,&timer,NULL);
}

void stop_profiling() {
    profiling_enabled=0;
    timer.it_value.tv_sec=0;
    timer.it_value.tv_usec=0;
    setitimer(ITIMER_PROF,&timer,NULL);

}

int register_function(const char *name) {
    if (function_count >= MAX_FUNCTIONS) {
        return -1;
    }

    strncpy(functions[function_count].name,name,255);
    functions[function_count].total_time=0;
    functions[function_count].self_time = 0;
    functions[function_count].call_count = 0;
    functions[function_count].is_active = 0;

    return function_count++;

}

void enter_function(int func_id) {
    if (func_id < 0 || func_id>=function_count) return;

    functions[func_id].call_count++;
    functions[func_id].is_active=1;
    gettimeofday(&functions[func_id].start_time,NULL);

    if (call_stack.top >= 0) {
        int caller_id = call_stack.stack[call_stack.top];
        if (caller_id >= 0 && caller_id < function_count) {
            caller_counts[caller_id][func_id]++;
        }
    }

    if (call_stack.top < MAX_CALL_STACK-1){
        call_stack.stack[++call_stack.top]=func_id;
    }
}

void leave_function(int func_id) {
    if (func_id < 0 || func_id>=function_count) return;

    struct timeval end_time;
    gettimeofday(&end_time,NULL);

    long execute_time=(end_time.tv_sec-functions[func_id].start_time.tv_sec)*1000000 +
    (end_time.tv_usec - functions[func_id].start_time.tv_usec);

    functions[func_id].total_time+=execute_time;
    // functions[func_id].self_time+=execute_time;
    functions[func_id].is_active=0;

    if (call_stack.top >=0 && call_stack.stack[call_stack.top]==func_id){
        call_stack.top--;
    }
}


void print_profiling_results() {
    printf("\n=== profiling results ===\n");
    printf("%-30s %12s %12s %12s %12s %15s %15s\n", 
        "Function", "Calls", "Total(ms)", "Self(ms)", "Self%", "Self(ms)/call", "Total(ms)/call");
    printf("------------------------------------------------------------------------------------------------------------------\n");

    time_stamp total_self_time=0;

    for (int i=0;i<function_count;i++){
        total_self_time+=functions[i].self_time;
    }

    for (int i=0;i<function_count;i++){
        if (functions[i].call_count > 0){
            double total_ms=functions[i].total_time/1000.0;
            double self_ms=functions[i].self_time/1000.0;
            double self_percent=(total_self_time > 0) ? 
            (functions[i].self_time * 100.0 / total_self_time) : 0;
            double avg_self = (functions[i].call_count > 0) ? (self_ms / (double)functions[i].call_count) : 0.0;
            double avg_total = (functions[i].call_count > 0) ? (total_ms / (double)functions[i].call_count) : 0.0;

            printf("%-30s %12llu %12.2f %12.2f %12.2f %15.3f %15.3f\n",
                functions[i].name,
                functions[i].call_count,
                total_ms,
                self_ms,
                self_percent,
                avg_self,
                avg_total);
        }
    }
    printf("------------------------------------------------------------------------------------------------------------------\n");

    // Show caller counts for each function
    printf("\n--- Callers (counts) ---\n");
    for (int callee = 0; callee < function_count; callee++) {
        if (functions[callee].call_count == 0) continue;
        int has_caller = 0;
        printf("%-30s <- ", functions[callee].name);
        for (int caller = 0; caller < function_count; caller++) {
            if (caller_counts[caller][callee] > 0) {
                has_caller = 1;
                printf("%s(%llu) ", functions[caller].name, caller_counts[caller][callee]);
            }
        }
        if (!has_caller) {
            printf("[none]");
        }
        printf("\n");
    }
}

static inline void __profile_cleanup_int(void *p) {
    int id = *(int *)p;
    leave_function(id);
}

#ifdef AUTO_PROFILE
#define PROFILE_FUNCTION()                                                     \
    static int __func_id = -1;                                                 \
    if (__func_id == -1) {                                                     \
        __func_id = register_function(__func__);                               \
    }                                                                          \
    enter_function(__func_id);                                                 \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard = __func_id;

#define PROFILE_SCOPE(name)                                                    \
    static int __scope_id_##__LINE__ = -1;                                     \
    if (__scope_id_##__LINE__ == -1) {                                         \
        __scope_id_##__LINE__ = register_function(name);                       \
    }                                                                          \
    enter_function(__scope_id_##__LINE__);                                     \
    __attribute__((cleanup(__profile_cleanup_int))) int __profile_scope_guard_##__LINE__ = __scope_id_##__LINE__;

#else
#define PROFILE_FUNCTION()
#define PROFILE_SCOPE(name)
#endif


void function_a() {
    PROFILE_FUNCTION();
    for (volatile int  i=0;i<1000000;i++);
}

void function_b() {
    PROFILE_FUNCTION();
    for (volatile int  i=0;i<500000;i++);
    function_a();
}

void function_c() {
    PROFILE_FUNCTION();
   

    for(volatile int i=0;i<2000000;i++);
    function_b();
}

int main(){


    init_profilier();
    
#ifndef AUTO_PROFILE
    register_function("main");
    register_function("function_a");
    register_function("function_b");
    register_function("function_c");
#endif

    printf("Start  profiling...\n");
    start_profiling();

    for (int i=0;i<5;i++){
        PROFILE_SCOPE("main_loop");
        function_a();
        function_b();
        function_c();

        for (volatile int j=0;j<1000000;j++);
    }

    stop_profiling();
    

    print_profiling_results();

    printf("profiling  stopped.\n");

    return 0;
}