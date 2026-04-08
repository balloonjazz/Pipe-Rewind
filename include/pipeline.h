#ifndef PIPEREWIND_PIPELINE_H
#define PIPEREWIND_PIPELINE_H

#include <sys/types.h>
#include <stdint.h>

#define MAX_STAGES    32
#define MAX_ARGS      64
#define MAX_CMD_LEN   1024

/*
 * A single stage in the pipeline.
 *
 * For "cat file.txt | grep ERROR | sort":
 *   stage 0: argv = {"cat", "file.txt", NULL}
 *   stage 1: argv = {"grep", "ERROR", NULL}
 *   stage 2: argv = {"sort", NULL}
 */
typedef struct {
    char  *argv[MAX_ARGS];
    char   raw_cmd[MAX_CMD_LEN];  /* original command string for display */
    int    argc;
} PipelineStage;

/*
 * Parsed pipeline: an ordered list of stages.
 */
typedef struct {
    PipelineStage stages[MAX_STAGES];
    int           num_stages;
} Pipeline;

/*
 * Runtime info for a spawned stage.
 * After pipeline_exec, each stage has:
 *   - a PID
 *   - file descriptors for the interposed capture pipes
 */
typedef struct {
    pid_t  pid;
    int    capture_in_fd;    /* fd we read to see what this stage receives */
    int    capture_out_fd;   /* fd we read to see what this stage produces */
} StageProcess;

typedef struct {
    StageProcess procs[MAX_STAGES];
    int          num_stages;
    int          original_stdin;   /* saved if pipeline reads from terminal */
    int          original_stdout;  /* saved if pipeline writes to terminal */
} PipelineExec;

/*
 * Parse a pipeline string (e.g. "cat foo | grep bar | wc -l")
 * into a Pipeline structure.
 *
 * Returns 0 on success, -1 on parse error.
 */
int pipeline_parse(const char *cmdline, Pipeline *pl);

/*
 * Execute a parsed pipeline with interposed capture pipes.
 *
 * For N stages, this creates N processes and 2*(N-1) capture taps:
 *
 *   [stage0] --pipe--> [tee/capture] --pipe--> [stage1] --pipe--> ...
 *
 * The capture file descriptors are returned in PipelineExec so the
 * event loop can monitor them with epoll.
 *
 * Returns 0 on success, -1 on error.
 */
int pipeline_exec(const Pipeline *pl, PipelineExec *pe);

/*
 * Wait for all pipeline processes to finish.
 * Populates exit_codes[] (one per stage).
 * Returns 0 on success.
 */
int pipeline_wait(PipelineExec *pe, int exit_codes[], int num_stages);

/*
 * Free resources (close remaining fds, etc.)
 */
void pipeline_cleanup(PipelineExec *pe);

/* Debug: print parsed pipeline to stderr */
void pipeline_dump(const Pipeline *pl);

#endif /* PIPEREWIND_PIPELINE_H */
