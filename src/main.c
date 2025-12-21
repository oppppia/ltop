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
    long swap_total_kb;
    long swap_free_kb;
} SystemMemoryInfo;

typedef struct {
    int  selected_index;
    bool should_quit;
    bool needs_refresh;
} AppState;

static bool         is_numeric_string(const char* str);
static bool         read_process_info(ProcessInfo* process);
static bool         read_system_memory_info(SystemMemoryInfo* mem_info);
static ProcessInfo* collect_processes(size_t* count);
static void handle_user_input(AppState* state, const ProcessInfo* processes,
                              size_t count);
static void terminate_process_with_dialog(int pid);
static void render_ui(const AppState* state, const ProcessInfo* processes,
                      size_t count, const SystemMemoryInfo* mem_info);
static void init_ncurses(void);
static void cleanup_ncurses(void);

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
        } else if (strncmp(line, "SwapTotal:", 10) == 0) {
            sscanf(line, "SwapTotal: %ld", &mem_info->swap_total_kb);
        } else if (strncmp(line, "SwapFree:", 9) == 0) {
            sscanf(line, "SwapFree: %ld", &mem_info->swap_free_kb);
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
        if (!read_process_info(&processes[size]))
            continue;

        size++;
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
            if (count > 0 && state->selected_index < (int)count) {
                terminate_process_with_dialog(
                    processes[state->selected_index].pid);
                state->needs_refresh = true;
            }
            break;

        case 'r':
            state->needs_refresh = true;
            break;

        default:
            break;
    }
}

static void terminate_process_with_dialog(int pid) {
    if (pid <= 0)
        return;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    WINDOW* terminate_dialog = newwin(8, 50, max_y / 2 - 4, max_x / 2 - 25);

    if (terminate_dialog == NULL)
        return;

    box(terminate_dialog, 0, 0);
    keypad(terminate_dialog, TRUE);

    mvwprintw(terminate_dialog, 1, 2, "Terminate process: PID %d", pid);
    mvwprintw(terminate_dialog, 2, 2, "------------------------------");
    mvwprintw(terminate_dialog, 3, 4, "1. SIGTERM");
    mvwprintw(terminate_dialog, 4, 4, "2. Cancel");
    mvwprintw(terminate_dialog, 5, 4, "Select option [1-2]: ");

    wrefresh(terminate_dialog);

    int choice = wgetch(terminate_dialog);

    if (choice == '1') {
        if (kill(pid, SIGTERM) == 0) {
            mvwprintw(terminate_dialog, 1, 2,
                      "Successfully sent SIGTERM to PID %d", pid);
        } else {
            mvwprintw(terminate_dialog, 1, 2,
                      "Failed to send SIGTERM to PID %d", pid);
            mvwprintw(terminate_dialog, 2, 4, "Error: %s", strerror(errno));
        }
        mvwprintw(terminate_dialog, 4, 4, "Press any key to continue...");
        wrefresh(terminate_dialog);
        wgetch(terminate_dialog);
    }

    delwin(terminate_dialog);
    refresh();
}

static void render_ui(const AppState* state, const ProcessInfo* processes,
                      size_t count, const SystemMemoryInfo* mem_info) {
    if (processes == NULL || mem_info == NULL)
        return;

    clear();

    int max_x = getmaxx(stdscr);

    mvprintw(4, 0, "%-8s %-22s %-6s %-12s", "PID", "NAME", "STATE", "MEM (KB)");

    for (int i = 0; i < max_x; i++) {
        mvaddch(5, i, '-');
    }

    int max_y, max_x_full;
    getmaxyx(stdscr, max_y, max_x_full);

    int visible_rows = max_y - 8;

    if (visible_rows < 0) {
        visible_rows = 0;
    }

    int start_idx = 0;

    if (state->selected_index >= visible_rows) {
        start_idx = state->selected_index - visible_rows + 1;
    }

    for (int i = 0; i < visible_rows && (i + start_idx) < (int)count; i++) {
        int                process_idx = i + start_idx;
        const ProcessInfo* proc        = &processes[process_idx];

        if (process_idx == state->selected_index) {
            attron(A_REVERSE);
        }

        mvprintw(i + 6, 0, "%-8d %-22.22s %-6c %-12lu", proc->pid, proc->name,
                 proc->state, proc->vm_rss_kb);

        if (process_idx == state->selected_index) {
            attroff(A_REVERSE);
        }
    }

    mvprintw(max_y - 2, 0, "Processes: %zu | Selected %d", count,
             state->selected_index + 1);

    mvprintw(max_y - 1, 0, "Q:Quit  ↑↓:Navigate  K:Kill");

    refresh();
}

static void init_ncurses(void) {
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_RED, COLOR_BLACK);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    }

    timeout(REFRESH_INTERVAL_MS);
}

static void cleanup_ncurses(void) {
    endwin();
}

int main(void) {
    init_ncurses();

    AppState app_state = {
        .selected_index = 0, .should_quit = false, .needs_refresh = true};

    while (!app_state.should_quit) {
        size_t       process_count = 0;
        ProcessInfo* processes     = collect_processes(&process_count);

        if (processes == NULL) {
            refresh();
            sleep(1);
            continue;
        }

        SystemMemoryInfo mem_info;
        if (!read_system_memory_info(&mem_info)) {
            memset(&mem_info, 0, sizeof(mem_info));
        }

        if (process_count > 0) {
            if (app_state.selected_index >= (int)process_count) {
                app_state.selected_index = process_count - 1;
            }
        } else {
            app_state.selected_index = 0;
        }

        if (app_state.needs_refresh) {
            render_ui(&app_state, processes, process_count, &mem_info);
            app_state.needs_refresh = false;
        }

        handle_user_input(&app_state, processes, process_count);

        free(processes);
    }

    cleanup_ncurses();
    return EXIT_SUCCESS;
}
