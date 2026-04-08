#ifndef PIPEREWIND_PROCSTATE_H
#define PIPEREWIND_PROCSTATE_H

#include <sys/types.h>
#include <stdint.h>

/*
 * Process state tracking via /proc filesystem.
 *
 * Reads /proc/[pid]/status to determine the current scheduling
 * state of a process.  Used during capture to record whether
 * pipeline stages are running, sleeping (I/O wait), or in
 * other kernel states.
 */

typedef enum {
    PROC_UNKNOWN    = 0,
    PROC_RUNNING    = 1,   /* R — running or runnable */
    PROC_SLEEPING   = 2,   /* S — interruptible sleep (e.g., waiting on pipe) */
    PROC_DISK_SLEEP = 3,   /* D — uninterruptible sleep (disk I/O) */
    PROC_STOPPED    = 4,   /* T — stopped by signal */
    PROC_ZOMBIE     = 5,   /* Z — zombie (exited but not reaped) */
} ProcState;

/*
 * Read the scheduling state of a process from /proc/[pid]/status.
 *
 * Returns the process state, or PROC_UNKNOWN on error (e.g., process
 * no longer exists or /proc not available).
 */
ProcState proc_read_state(pid_t pid);

/*
 * Return a human-readable string for a ProcState value.
 */
const char *proc_state_str(ProcState s);

#endif /* PIPEREWIND_PROCSTATE_H */
