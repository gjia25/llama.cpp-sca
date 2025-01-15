#define _GNU_SOURCE

#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>

#define PROC_CLEAR_ACCESSED "/proc/clear_accessed_bits"
struct clear_request {
    pid_t pid;
    unsigned long start_vaddr;
    unsigned long end_vaddr;
};

#define PROC_READ_ACCESSED "/proc/read_accessed"
struct read_request {
    pid_t pid;
    unsigned long start_vaddr;
    unsigned long end_vaddr;
};
struct result_entry {
    unsigned long vaddr;
};

#define PATHSIZE		128
#define LINESIZE		256
#define PAGEMAP_CHUNK_SIZE	8
#define CHAR_BIT 8
#define MAX_REQUESTS 1024
#define NUM_CORES 16

#define NUM_FEATURES 1
#define NUM_EMBDS 250880
#define EMB_SIZE 1024

// globals
char *g_outdir = "/dev/shm/llm";
char *g_timefile = "/dev/shm/llm/times";
unsigned long g_input_addrs[NUM_CORES-1];
static struct timeval g_ts0;
static struct timeval g_ts_start;
static struct timeval g_ts_end;
static unsigned long long dur_clear;
static unsigned long long dur_read;
int g_got_inputs[NUM_CORES-1];
int g_in_lookup[NUM_CORES-1];
int g_num_lookups = 0;
pid_t child_pids[NUM_CORES-1];
struct read_request g_requests[MAX_REQUESTS];
int g_num_requests = 0;

void read_input_addrs(unsigned long *list, pid_t cpid, int cidx) {
    FILE *file;

    char fpath[100];
    sprintf(fpath, "/home/grace/llama.cpp-sca/start-%d.out", cpid);
    file = fopen(fpath, "r");
    if (!file) {
        perror("failed to open input_addrs");
        exit(EXIT_FAILURE);
    }
    
    if (fscanf(file, "%lx\n", &list[cidx]) != 1) {
        perror("failed to read input_addr");
        exit(EXIT_FAILURE);
    }

    fclose(file);
    printf("[Parent] child %d has start addr %lx\n", cidx, list[cidx]);
}

void clear_accessed_bits(unsigned long start_vaddr, unsigned long end_vaddr, pid_t cpid) {
    int fd;
    struct clear_request req;

    req.pid = cpid;
    req.start_vaddr = start_vaddr;
    req.end_vaddr = end_vaddr;

    fd = open(PROC_CLEAR_ACCESSED, O_WRONLY);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    if (write(fd, &req, sizeof(req)) != sizeof(req)) {
        perror("write");
        close(fd);
        exit(EXIT_FAILURE);
    }
    
    close(fd);
    
    if (g_num_requests >= MAX_REQUESTS){
        printf("WARNING! Max requests exceeded\n");
        return;
    }
    g_requests[g_num_requests].pid = req.pid;
    g_requests[g_num_requests].start_vaddr = req.start_vaddr;
    g_requests[g_num_requests].end_vaddr = req.end_vaddr;
    g_num_requests++;
}

void clear_bits_for_lookups(pid_t cpid, int cidx) {
    unsigned long start, end;

    start = g_input_addrs[cidx] & 0xFFFFFFFFFFFFF000;
    end = (g_input_addrs[cidx] + EMB_SIZE * NUM_EMBDS) & 0xFFFFFFFFFFFFF000 + 1;
    clear_accessed_bits(start, end, cpid);

}

void append_accessed_pages(int request_idx) {
    int fd, ret;
    struct result_entry results[1024];
    ssize_t count;
    FILE *file;
    pid_t pid;

    struct read_request req = g_requests[request_idx];
    pid = req.pid;

    fd = open(PROC_READ_ACCESSED, O_RDWR);
    if (fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    ret = write(fd, &req, sizeof(req));
    if (ret != sizeof(req)) {
        perror("write");
        close(fd);
        exit(EXIT_FAILURE);
    }
    count = read(fd, results, sizeof(results));
    if (count == -1) {
        perror("read");
        close(fd);
        exit(EXIT_FAILURE);
    }

    int num_entries = count / sizeof(struct result_entry);

    char filename[PATHSIZE];
    sprintf(filename, "%s/llm-%llu", g_outdir, g_ts0.tv_sec * (uint64_t)1000000 + g_ts0.tv_usec);
    file = fopen(filename, "a");
    if (file == NULL) {
        perror("Unable to open output file");
        close(fd);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < num_entries; i++) {
        fprintf(file, "%d, %d, 0x%lx\n", g_num_lookups, pid, results[i].vaddr);
    }

    fclose(file);
    close(fd);
}


// Expected signal usage:
// child process (python script) sends SIGUSR1 to parent process (this program) when it is about to start a lookup
// parent process clears page table entry flags, then sends SIGUSR1 back to child
// child performs lookup, then sends SIGUSR1 to parent
// parent reads page table entry flags, then sends SIGUSR1 back to child
void signal_handler(int signal_num, siginfo_t *info, void *context)
{
    pid_t cpid;
    int cidx;
    if (info == NULL) {
        perror("no siginfo");
        return;
    } else {
        cpid = info->si_pid;
        for (cidx = 0; cidx < NUM_CORES-1; cidx++) {
            if (cpid == child_pids[cidx]) {
                break;
            }
        }
        printf("[Parent] received signal from child %d, PID %d\n", cidx, cpid);
    }
    FILE *file;
    if (signal_num == SIGUSR1) {
        if (g_got_inputs[cidx] == 0) {
            read_input_addrs(g_input_addrs, cpid, cidx);
            g_got_inputs[cidx] = 1;
        }
        if (g_in_lookup[cidx] == 0) {
            printf("[Parent] child %d starting lookup\n", cidx);
            gettimeofday(&g_ts_start, NULL);
            g_in_lookup[cidx] = 1;
            g_num_lookups++;
            clear_bits_for_lookups(cpid, cidx);
            gettimeofday(&g_ts_end, NULL);
            dur_read = 1000000 * (g_ts_end.tv_sec - g_ts_start.tv_sec) + (g_ts_end.tv_usec - g_ts_start.tv_usec);
        } else {
            printf("[Parent] child %d ending lookup\n", cidx);
            gettimeofday(&g_ts_start, NULL);
            for (int i = 0; i < g_num_requests; i++) {
                append_accessed_pages(i);
            }
            g_num_requests = 0;
            g_in_lookup[cidx] = 0;
            gettimeofday(&g_ts_end, NULL);
            dur_clear = 1000000 * (g_ts_end.tv_sec - g_ts_start.tv_sec) + (g_ts_end.tv_usec - g_ts_start.tv_usec);
            file = fopen(g_timefile, "a");
            if (file == NULL) {
                perror("Unable to open timefile");
                exit(EXIT_FAILURE);
            }
            fprintf(file, "%llu,%llu\n", dur_clear, dur_read);
            fclose(file);
        }
        kill(cpid, SIGUSR1);
    }
}

int main(int argc, char *argv[])
{
	int status, err = 0;
	double mbytes;
	pid_t ppid, pid, cpid;
	
	// options
	if (argc < 7) {
		printf("USAGE: walk <bin> -m <model_path> -n <n_predict> prompt_dir\n");
		exit(0);
	}
    printf("RUNNING: %s %s %s %s %s %s\n", argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
	ppid = getpid(); // parent PID

	gettimeofday(&g_ts0, NULL);

    for (int i = 0; i < NUM_CORES-1; i++) {
        pid = fork(); // child PID

        if (pid == -1) {
            // Fork failed
            perror("fork");
            exit(EXIT_FAILURE);
        } else if (pid == 0) { // in child
            printf("[child %d] has PID %d\n", i, getpid());
            
            // Set child process to run on core i
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(i, &cpuset); 

            if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
                perror("sched_setaffinity");
                exit(EXIT_FAILURE);
            }

            // Pass parent PID to child
            char pid_arg[20];
            sprintf(pid_arg, "%d", ppid);
            char filepath[20];
            sprintf(filepath, "%s/med%d.txt", argv[6], i);
            printf("[child %d] Running %s %s %s %s %s %s %s %s\n", i, argv[1], argv[2], argv[3], argv[4], argv[5], "-p", pid_arg, filepath);
            execlp(argv[1], argv[1], argv[2], argv[3], argv[4], argv[5], "-p", pid_arg, filepath, NULL);
            
            // If execlp returns, it means it failed
            perror("execlp");
            exit(EXIT_FAILURE);
        } else { // in parent
            child_pids[i] = pid;
        }
    }

    printf("In parent process\n");
    
    // Set parent process to run on core 0
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(NUM_CORES-1, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }

    struct sigaction sa;
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    printf("[Parent] Set up signal handler for %d\n", SIGUSR1);

    sleep(1);

    for (int i = 1; i < NUM_CORES; i++) {
        pid_t cpid = wait(NULL); // Wait for each child process
        if (cpid == -1) {
            perror("wait");
            exit(EXIT_FAILURE);
        }
        printf("Parent: Child process %d exited\n", cpid);
    }

    printf("[Parent] all children exited\n");
    printf("[Parent] g_num_lookups = %d\n", g_num_lookups);
	return 1;
}
