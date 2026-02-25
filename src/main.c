#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>
#include <time.h>

#define PROC_PATH_MAX         64
#define PROC_NAME_MAX         128
#define LINE_BUFFER_SIZE      256
#define KB_TO_MB              1024
#define REFRESH_INTERVAL_MS   3000
#define INITIAL_CAPACITY_SIZE 128

typedef struct {
    int           pid;
    char          name[PROC_NAME_MAX];
    char          state;
    unsigned long vm_rss_kb;
} ProcessInfo;

typedef struct {
    long mem_total_kb;
    long mem_free_kb;
    long mem_available_kb;
    long mem_cached_kb;
    long swap_total_kb;
    long swap_free_kb;
    long buffers_kb;
} SystemMemoryInfo;

typedef struct {
    int             selected_index;
    bool            should_quit;
    bool            needs_refresh;
    struct timespec last_update;
} AppState;

static bool         is_numeric_string(const char* str);
static bool         read_process_info(ProcessInfo* process);
static bool         read_system_memory_info(SystemMemoryInfo* mem_info);
static ProcessInfo* collect_processes(size_t* count);
static void handle_user_input(AppState* state, const ProcessInfo* processes,
                              size_t count);
static void terminate_process_with_dialog(const ProcessInfo* proc);
static void render_memory_info(const SystemMemoryInfo* mem_info);
static void render_process_list(const AppState*    state,
                                const ProcessInfo* processes, size_t count);
static void init_ncurses(void);
static void cleanup_ncurses(void);
static bool should_refresh(AppState* state);

static bool is_numeric_string(const char* str) {
    if (str == NULL || *str == '\0')
        return false;

    for (int i = 0; str[i]; i++) {
        if (str[i] < '0' || str[i] > '9')
            return false;
    }

    return true;
}

static bool read_process_info(ProcessInfo* process) {
    if (process == NULL || process->pid <= 0) {
        return false;
    }

    char proc_path[PROC_PATH_MAX];
    char line[LINE_BUFFER_SIZE];

    snprintf(proc_path, sizeof(proc_path), "/proc/%d/status", process->pid);

    FILE* file = fopen(proc_path, "r");
    if (file == NULL) {
        return false;
    }

    bool name_found  = false;
    bool state_found = false;
    bool rss_found   = false;

    while (fgets(line, sizeof(line), file) != NULL) {
        if (!name_found && strncmp(line, "Name:", 5) == 0) {
            char* name_start = line + 5;
            while (*name_start != '\0' && isspace((unsigned char)*name_start)) {
                name_start++;
            }

            strncpy(process->name, name_start, sizeof(process->name) - 1);
            process->name[sizeof(process->name) - 1] = '\0';

            char* newline = strchr(process->name, '\n');
            if (newline != NULL) {
                *newline = '\0';
            }
            name_found = true;
        } else if (!state_found && strncmp(line, "State:", 6) == 0) {
            char* state_ptr = line + 6;
            while (*state_ptr != '\0' && isspace((unsigned char)*state_ptr)) {
                state_ptr++;
            }
            if (*state_ptr != '\0') {
                process->state = *state_ptr;
                state_found    = true;
            }
        } else if (!rss_found && strncmp(line, "VmRSS:", 6) == 0) {
            if (sscanf(line + 6, "%lu", &process->vm_rss_kb) == 1) {
                rss_found = true;
            }
        }

        if (name_found && state_found && rss_found) {
            break;
        }
    }

    fclose(file);

    if (!name_found) {
        snprintf(process->name, sizeof(process->name), "?");
    }
    if (!state_found) {
        process->state = '?';
    }
    if (!rss_found) {
        process->vm_rss_kb = 0;
    }

    return true;
}

static bool read_system_memory_info(SystemMemoryInfo* mem_info) {
    if (mem_info == NULL)
        return false;

    // Initialize with zeros
    memset(mem_info, 0, sizeof(SystemMemoryInfo));

    char  line[LINE_BUFFER_SIZE];

    FILE* file = fopen("/proc/meminfo", "r");

    if (file == NULL)
        return false;

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %ld", &mem_info->mem_total_kb);
        } else if (strncmp(line, "MemFree:", 8) == 0) {
            sscanf(line, "MemFree: %ld", &mem_info->mem_free_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line, "MemAvailable: %ld", &mem_info->mem_available_kb);
        } else if (strncmp(line, "Cached:", 7) == 0) {
            sscanf(line, "Cached: %ld", &mem_info->mem_cached_kb);
        } else if (strncmp(line, "SwapTotal:", 10) == 0) {
            sscanf(line, "SwapTotal: %ld", &mem_info->swap_total_kb);
        } else if (strncmp(line, "SwapFree:", 9) == 0) {
            sscanf(line, "SwapFree: %ld", &mem_info->swap_free_kb);
        } else if (strncmp(line, "Buffers:", 8) == 0) {
            sscanf(line, "Buffers: %ld", &mem_info->buffers_kb);
        }
    }

    fclose(file);

    return true;
}

static ProcessInfo* collect_processes(size_t* count) {
    if (count == NULL)
        return NULL;

    DIR* proc_dir = opendir("/proc");

    if (proc_dir == NULL)
        return NULL;

    size_t       capacity = INITIAL_CAPACITY_SIZE;
    size_t       size     = 0;

    ProcessInfo* processes = malloc(capacity * sizeof(ProcessInfo));

    if (processes == NULL) {
        closedir(proc_dir);
        return NULL;
    }

    struct dirent* entry;

    while ((entry = readdir(proc_dir)) != NULL) {
        if (!is_numeric_string(entry->d_name))
            continue;

        int pid = atoi(entry->d_name);

        if (pid <= 0)
            continue;

        if (size >= capacity) {
            size_t       new_capacity = capacity * 2;
            ProcessInfo* new_processes =
                realloc(processes, new_capacity * sizeof(ProcessInfo));

            if (new_processes == NULL) {
                free(processes);
                closedir(proc_dir);
                return NULL;
            }

            processes = new_processes;
            capacity  = new_capacity;
        }

        processes[size].pid = pid;
        if (read_process_info(&processes[size])) {
            size++;
        }
    }

    closedir(proc_dir);
    *count = size;

    return processes;
}

static void handle_user_input(AppState* state, const ProcessInfo* processes,
                              size_t count) {
    if (state == NULL || processes == NULL)
        return;

    int ch = getch();

    switch (ch) {
        case 'q':
        case 'Q':
            state->should_quit = true;
            break;

        case KEY_UP:
            if (state->selected_index > 0) {
                state->selected_index--;
                state->needs_refresh = true;
            }
            break;

        case KEY_DOWN:
            if (state->selected_index < (int)count - 1) {
                state->selected_index++;
                state->needs_refresh = true;
            }
            break;

        case 'k':
        case 'K':
            if (count > 0 && state->selected_index >= 0 &&
                state->selected_index < (int)count) {
                terminate_process_with_dialog(
                    &processes[state->selected_index]);
                state->needs_refresh = true;
            }
            break;

        case 'r':
        case 'R':
            state->needs_refresh = true;
            break;

        default:
            break;
    }
}

static void terminate_process_with_dialog(const ProcessInfo* proc) {
    if (proc == NULL)
        return;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    WINDOW* terminate_dialog = newwin(8, 60, max_y / 2 - 4, max_x / 2 - 30);

    if (terminate_dialog == NULL)
        return;

    box(terminate_dialog, 0, 0);
    keypad(terminate_dialog, TRUE);

    mvwprintw(terminate_dialog, 1, 2, "Terminate process: %s PID %d",
              proc->name, proc->pid);
    mvwprintw(terminate_dialog, 2, 2, "--------------------------------");
    mvwprintw(terminate_dialog, 3, 4, "1. SIGTERM (graceful termination)");
    mvwprintw(terminate_dialog, 4, 4, "2. Cancel");
    mvwprintw(terminate_dialog, 5, 4, "Select option [1-2]: ");

    wrefresh(terminate_dialog);

    int choice = wgetch(terminate_dialog);

    if (choice == '1') {
        if (kill(proc->pid, SIGTERM) == 0) {
            mvwprintw(terminate_dialog, 1, 2,
                      "Successfully sent SIGTERM to %s PID %d", proc->name,
                      proc->pid);
        } else {
            mvwprintw(terminate_dialog, 1, 2,
                      "Failed to send SIGTERM to %s PID %d", proc->name,
                      proc->pid);
            mvwprintw(terminate_dialog, 2, 4, "Error: %s", strerror(errno));
        }
        mvwprintw(terminate_dialog, 4, 4, "Press any key to continue...");
        wrefresh(terminate_dialog);
        wgetch(terminate_dialog);
    }

    delwin(terminate_dialog);
    touchwin(stdscr);
    wrefresh(stdscr);
}

static void render_memory_info(const SystemMemoryInfo* mem_info) {
    if (mem_info == NULL)
        return;

    int max_x = getmaxx(stdscr);

    for (int i = 0; i < 3; i++) {
        move(i, 0);
        clrtoeol();
    }

    double mem_total_mb     = (double)mem_info->mem_total_kb / KB_TO_MB;
    double mem_free_mb      = (double)mem_info->mem_free_kb / KB_TO_MB;
    double mem_available_mb = (double)mem_info->mem_available_kb / KB_TO_MB;
    double mem_cached_mb    = (double)mem_info->mem_cached_kb / KB_TO_MB;
    double swap_total_mb    = (double)mem_info->swap_total_kb / KB_TO_MB;
    double swap_free_mb     = (double)mem_info->swap_free_kb / KB_TO_MB;

    double mem_used_mb = mem_total_mb - mem_free_mb - mem_cached_mb;
    if (mem_used_mb < 0)
        mem_used_mb = 0;

    double swap_used_mb = swap_total_mb - swap_free_mb;
    if (swap_used_mb < 0)
        swap_used_mb = 0;

    attron(A_BOLD);
    mvprintw(0, 0, "System Memory Information (auto-refresh every 3s):");
    attroff(A_BOLD);

    mvprintw(1, 0,
             "MiB Mem : %8.1f total, %8.1f free, %8.1f used, %8.1f buff/cache",
             mem_total_mb, mem_free_mb, mem_used_mb, mem_cached_mb);

    mvprintw(2, 0,
             "MiB Swap: %8.1f total, %8.1f free, %8.1f used, %8.1f avail Mem",
             swap_total_mb, swap_free_mb, swap_used_mb, mem_available_mb);

    // Draw separator line
    for (int i = 0; i < max_x; i++) {
        mvaddch(4, i, '-');
    }
}

static void render_process_list(const AppState*    state,
                                const ProcessInfo* processes, size_t count) {
    if (processes == NULL || state == NULL)
        return;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    for (int i = 5; i < max_y - 2; i++) {
        move(i, 0);
        clrtoeol();
    }

    attron(A_UNDERLINE);
    mvprintw(3, 0, "%-8s %-22s %-6s %-12s", "PID", "NAME", "STATE", "MEM (KB)");
    attroff(A_UNDERLINE);

    for (int i = 0; i < max_x; i++) {
        mvaddch(4, i, '-');
    }

    int visible_rows = max_y - 7;

    if (visible_rows < 0) {
        visible_rows = 0;
    }

    int start_idx = 0;

    if (count > 0 && state->selected_index >= visible_rows) {
        start_idx = state->selected_index - visible_rows + 1;
    }

    for (int i = 0; i < visible_rows && (i + start_idx) < (int)count; i++) {
        int                process_idx = i + start_idx;
        const ProcessInfo* proc        = &processes[process_idx];

        if (process_idx == state->selected_index) {
            attron(A_REVERSE);
        }

        mvprintw(i + 5, 0, "%-8d %-22.22s %-6c %-12lu", proc->pid, proc->name,
                 proc->state, proc->vm_rss_kb);

        if (process_idx == state->selected_index) {
            attroff(A_REVERSE);
        }
    }

    mvprintw(max_y - 2, 0, "Processes: %zu | Selected %d of %zu", count,
             state->selected_index + 1, count);

    int y, x;
    getyx(stdscr, y, x);
    mvaddch(y, x - 1, ' ');

    mvprintw(max_y - 1, 0, "Q:Quit  ↑↓:Navigate  K:Kill  R:Refresh Now");
}

static bool should_refresh(AppState* state) {
    if (state == NULL)
        return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_ms = (now.tv_sec - state->last_update.tv_sec) * 1000 +
        (now.tv_nsec - state->last_update.tv_nsec) / 1000000;

    if (elapsed_ms >= REFRESH_INTERVAL_MS || state->needs_refresh) {
        state->last_update = now;
        return true;
    }

    return false;
}

static void init_ncurses(void) {
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);
}

static void cleanup_ncurses(void) {
    endwin();
}

int main(void) {
    init_ncurses();

    AppState app_state = {
        .selected_index = 0, .should_quit = false, .needs_refresh = true};
    clock_gettime(CLOCK_MONOTONIC, &app_state.last_update);

    SystemMemoryInfo last_mem_info      = {0};
    ProcessInfo*     last_processes     = NULL;
    size_t           last_process_count = 0;

    while (!app_state.should_quit) {
        if (should_refresh(&app_state)) {
            if (last_processes != NULL) {
                free(last_processes);
                last_processes = NULL;
            }

            last_processes = collect_processes(&last_process_count);

            if (!read_system_memory_info(&last_mem_info)) {
                memset(&last_mem_info, 0, sizeof(last_mem_info));
            }

            if (last_process_count > 0) {
                if (app_state.selected_index >= (int)last_process_count) {
                    app_state.selected_index = last_process_count - 1;
                }
            } else {
                app_state.selected_index = 0;
            }

            app_state.needs_refresh = false;
        }

        if (last_processes != NULL) {
            render_memory_info(&last_mem_info);
            render_process_list(&app_state, last_processes, last_process_count);
        } else {
            clear();
            mvprintw(0, 0,
                     "Unable to read process information. Check permissions.");
        }

        refresh();

        handle_user_input(&app_state, last_processes, last_process_count);
    }

    if (last_processes != NULL) {
        free(last_processes);
    }

    cleanup_ncurses();
    return EXIT_SUCCESS;
}
