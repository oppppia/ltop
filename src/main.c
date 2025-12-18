#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 
    /proc
    every process - dir /proc/<PID>

    /proc/stat - CPU info
    /proc/meminfo - MEMORY info
    /proc/[pid]/stat - CPU and process status
    /proc/[pid]/status - MEMORY and NAME process
  */

typedef struct {
    int           pid;
    char          name[128];
    char          state;
    unsigned long utime;
    unsigned long stime;
    unsigned long rss;
} ProcessInfo;

int is_number(const char* str) {
    if (*str == '\0')
        return 0;

    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] < '0' || str[i] > '9') {
            return 0;
        }
    }
    return 1;
}

void fill_process_name(ProcessInfo* p) {
    char path[64];
    char line[256];

    snprintf(path, sizeof(path), "/proc/%d/status", p->pid);

    FILE* file = fopen(path, "r");
    if (!file) {
        strcpy(p->name, "N/A");
        return;
    }

    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "Name:", 5) == 0) {
            sscanf(line, "Name:\t%127s", p->name);
            fclose(file);
            return;
        }
    }

    fclose(file);
    strcpy(p->name, "N/A");
}

void fill_process_stat(ProcessInfo* p) {
    char path[64];
    char buf[1024];

    snprintf(path, sizeof(path), "/proc/%d/stat", p->pid);

    FILE* file = fopen(path, "r");
    if (!file) {
        p->state = '?';
        p->utime = p->stime = p->rss = 0;
        return;
    }

    if (fgets(buf, sizeof(buf), file)) {
        char          comm[256];
        int           pid;
        char          state;
        unsigned long utime, stime;
        long          rss;

        sscanf(buf,
               "%d (%[^)]) %c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu "
               "%*s %*s %*s %*s %*s %*s %*s %*s %ld",
               &pid, comm, &state, &utime, &stime, &rss);

        p->state = state;
        p->utime = utime;
        p->stime = stime;
        p->rss   = rss;
    } else {
        p->state = '?';
        p->utime = p->stime = p->rss = 0;
    }

    fclose(file);
}

ProcessInfo* get_processes(size_t* count) {
    struct dirent* proc_dir_pointer;
    DIR*           proc_dir = opendir("/proc");

    size_t         capacity = 128;
    size_t         size     = 0;

    ProcessInfo*   processes = malloc(capacity * sizeof(ProcessInfo));

    if (!proc_dir) {
        perror("Failed to open /proc dir");
        return NULL;
    }

    while ((proc_dir_pointer = readdir(proc_dir)) != NULL) {
        if (!is_number(proc_dir_pointer->d_name))
            continue;

        if (size == capacity) {
            capacity *= 2;
            ProcessInfo* tmp =
                realloc(processes, capacity * sizeof(ProcessInfo));

            if (!tmp) {
                free(processes);
                closedir(proc_dir);

                return NULL;
            }

            processes = tmp;
        }

        processes[size].pid = atoi(proc_dir_pointer->d_name);
        fill_process_name(&processes[size]);
        fill_process_stat(&processes[size]);
        size++;
    }

    closedir(proc_dir);
    *count = size;

    return processes;
}

int main(int argc, char** argv) {
    size_t       count;
    ProcessInfo* processes = get_processes(&count);

    if (!processes) {
        fprintf(stderr, "Failed to get processes");
        return 1;
    }

    for (size_t i = 0; i < count; i++) {
        printf("%5d  %-20s %-5c %-8lu %-8lu %-8ld\n\n", processes[i].pid,
               processes[i].name, processes[i].state, processes[i].utime,
               processes[i].stime, processes[i].rss);
    }

    free(processes);

    return 0;
}
