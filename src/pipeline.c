#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

/* ---- Parsing ---- */

/*
 * Split a command line on '|' into stages, then split each stage
 * into argv tokens.  Simple tokenizer: no quoting or escaping yet.
 */
static int split_args(char *cmd, char **argv, int max_args)
{
    int argc = 0;
    char *tok = strtok(cmd, " \t");
    while (tok && argc < max_args - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    return argc;
}

int pipeline_parse(const char *cmdline, Pipeline *pl)
{
    memset(pl, 0, sizeof(*pl));

    /* Work on a mutable copy */
    char *buf = strdup(cmdline);
    if (!buf)
        return -1;

    /* Split on pipe characters */
    char *saveptr = NULL;
    char *segment = strtok_r(buf, "|", &saveptr);

    while (segment && pl->num_stages < MAX_STAGES) {
        PipelineStage *s = &pl->stages[pl->num_stages];

        /* Trim leading/trailing whitespace */
        while (*segment == ' ' || *segment == '\t') segment++;
        char *end = segment + strlen(segment) - 1;
        while (end > segment && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (*segment == '\0') {
            free(buf);
            return -1;  /* empty stage */
        }

        strncpy(s->raw_cmd, segment, MAX_CMD_LEN - 1);

        /* Split into argv */
        char *argbuf = strdup(segment);
        if (!argbuf) {
            free(buf);
            return -1;
        }
        s->argc = split_args(argbuf, s->argv, MAX_ARGS);

        if (s->argc == 0) {
            free(argbuf);
            free(buf);
            return -1;
        }

        /* Note: argv pointers point into argbuf which we intentionally
         * leak here (lives for program duration).  A production tool
         * would manage this memory properly. */

        pl->num_stages++;
        segment = strtok_r(NULL, "|", &saveptr);
    }

    free(buf);

    if (pl->num_stages == 0)
        return -1;

    return 0;
}

/* ---- Execution with interposed capture pipes ---- */

/*
 * Architecture for 3 stages (A | B | C):
 *
 *   [A] --stdout--> pipeAB_out[wr]
 *                    pipeAB_out[rd]  <-- capture engine reads, records
 *                    pipeAB_in[wr]   --> capture engine writes
 *                    pipeAB_in[rd]   --stdin--> [B]
 *
 *   [B] --stdout--> pipeBC_out[wr]
 *                    pipeBC_out[rd]  <-- capture engine reads, records
 *                    pipeBC_in[wr]   --> capture engine writes
 *                    pipeBC_in[rd]   --stdin--> [C]
 *
 *   [C] --stdout--> terminal (or redirect)
 *
 * Each inter-stage connection uses TWO kernel pipes so the capture
 * engine can sit in the middle, record bytes, and forward them.
 *
 * Additionally, we capture the very first stage's stdin and the
 * very last stage's stdout using similar interposition when they
 * connect to the terminal.
 */

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int pipeline_exec(const Pipeline *pl, PipelineExec *pe)
{
    memset(pe, 0, sizeof(*pe));
    pe->num_stages = pl->num_stages;
    int n = pl->num_stages;

    /*
     * For each inter-stage boundary (n-1 boundaries), create two pipes:
     *   out_pipe: stage[i] stdout -> capture engine
     *   in_pipe:  capture engine  -> stage[i+1] stdin
     *
     * out_pipe[0] = read end  (capture engine reads from here)
     * out_pipe[1] = write end (stage[i] writes here, dup2'd to stdout)
     * in_pipe[0]  = read end  (stage[i+1] reads here, dup2'd to stdin)
     * in_pipe[1]  = write end (capture engine writes here)
     */
    int out_pipes[MAX_STAGES][2];  /* stage i stdout capture */
    int in_pipes[MAX_STAGES][2];   /* stage i+1 stdin feed */

    /* Create all pipes first */
    for (int i = 0; i < n - 1; i++) {
        if (pipe(out_pipes[i]) < 0 || pipe(in_pipes[i]) < 0) {
            perror("pipe");
            return -1;
        }
    }

    /* Also capture the last stage's stdout */
    int last_out[2] = {-1, -1};
    if (pipe(last_out) < 0) {
        perror("pipe");
        return -1;
    }

    /* Spawn each stage */
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return -1;
        }

        if (pid == 0) {
            /* --- Child process --- */

            /* Set up stdin */
            if (i > 0) {
                /* Read from the in_pipe of boundary (i-1) */
                dup2(in_pipes[i - 1][0], STDIN_FILENO);
            }
            /* else: stage 0 inherits parent's stdin */

            /* Set up stdout */
            if (i < n - 1) {
                /* Write to the out_pipe of boundary i */
                dup2(out_pipes[i][1], STDOUT_FILENO);
            } else {
                /* Last stage: write to last_out capture pipe */
                dup2(last_out[1], STDOUT_FILENO);
            }

            /* Close all pipe fds the child doesn't need */
            for (int j = 0; j < n - 1; j++) {
                close(out_pipes[j][0]);
                close(out_pipes[j][1]);
                close(in_pipes[j][0]);
                close(in_pipes[j][1]);
            }
            close(last_out[0]);
            close(last_out[1]);

            /* Exec */
            execvp(pl->stages[i].argv[0], pl->stages[i].argv);
            fprintf(stderr, "piperewind: exec failed for '%s': %s\n",
                    pl->stages[i].argv[0], strerror(errno));
            _exit(127);
        }

        /* --- Parent --- */
        pe->procs[i].pid = pid;
    }

    /* Close pipe ends that belong to children */
    for (int i = 0; i < n - 1; i++) {
        close(out_pipes[i][1]);   /* child i writes here */
        close(in_pipes[i][0]);    /* child i+1 reads here */
    }
    close(last_out[1]);           /* last child writes here */

    /*
     * Set up capture file descriptors:
     *
     * For stage i:
     *   capture_out_fd = read end of the pipe coming FROM stage i's stdout
     *   capture_in_fd  = write end of the pipe going TO stage i's stdin
     *
     * The capture engine reads from capture_out_fd, records the data,
     * and writes it to the NEXT stage's capture_in_fd (forwarding).
     */
    for (int i = 0; i < n; i++) {
        if (i < n - 1) {
            pe->procs[i].capture_out_fd = out_pipes[i][0];
            set_nonblock(out_pipes[i][0]);
        } else {
            pe->procs[i].capture_out_fd = last_out[0];
            set_nonblock(last_out[0]);
        }

        if (i > 0) {
            pe->procs[i].capture_in_fd = in_pipes[i - 1][1];
            set_nonblock(in_pipes[i - 1][1]);
        } else {
            pe->procs[i].capture_in_fd = -1;  /* stage 0 has no feed-in */
        }
    }

    return 0;
}

int pipeline_wait(PipelineExec *pe, int exit_codes[], int num_stages)
{
    for (int i = 0; i < num_stages; i++) {
        int status = 0;
        if (pe->procs[i].pid > 0) {
            waitpid(pe->procs[i].pid, &status, 0);
            if (WIFEXITED(status))
                exit_codes[i] = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                exit_codes[i] = 128 + WTERMSIG(status);
            else
                exit_codes[i] = -1;
        }
    }
    return 0;
}

void pipeline_cleanup(PipelineExec *pe)
{
    for (int i = 0; i < pe->num_stages; i++) {
        if (pe->procs[i].capture_out_fd >= 0)
            close(pe->procs[i].capture_out_fd);
        if (pe->procs[i].capture_in_fd >= 0)
            close(pe->procs[i].capture_in_fd);
        pe->procs[i].capture_out_fd = -1;
        pe->procs[i].capture_in_fd  = -1;
    }
}

void pipeline_dump(const Pipeline *pl)
{
    fprintf(stderr, "Pipeline (%d stages):\n", pl->num_stages);
    for (int i = 0; i < pl->num_stages; i++) {
        fprintf(stderr, "  [%d] %s", i, pl->stages[i].raw_cmd);
        fprintf(stderr, "  (argc=%d:", pl->stages[i].argc);
        for (int j = 0; j < pl->stages[i].argc; j++)
            fprintf(stderr, " '%s'", pl->stages[i].argv[j]);
        fprintf(stderr, ")\n");
    }
}
