#include "procstate.h"

#include <stdio.h>
#include <string.h>

ProcState proc_read_state(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return PROC_UNKNOWN;

    char line[256];
    ProcState result = PROC_UNKNOWN;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "State:", 6) == 0) {
            /* State line format: "State:\tX (description)\n" */
            const char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;

            switch (*p) {
            case 'R': result = PROC_RUNNING;    break;
            case 'S': result = PROC_SLEEPING;   break;
            case 'D': result = PROC_DISK_SLEEP; break;
            case 'T': result = PROC_STOPPED;    break;
            case 'Z': result = PROC_ZOMBIE;     break;
            default:  result = PROC_UNKNOWN;    break;
            }
            break;
        }
    }

    fclose(fp);
    return result;
}

const char *proc_state_str(ProcState s)
{
    switch (s) {
    case PROC_RUNNING:    return "RUNNING";
    case PROC_SLEEPING:   return "SLEEPING";
    case PROC_DISK_SLEEP: return "DISK_WAIT";
    case PROC_STOPPED:    return "STOPPED";
    case PROC_ZOMBIE:     return "ZOMBIE";
    default:              return "UNKNOWN";
    }
}
