#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

/* Enum representing the current state of the parser 
 * NORMAL       - Reading within the context of no string
 * SINGLE_QUOTE - Reading within the context of a single-quoted string
 * DOUBLE_QUOTE - Reading within the context of a double-quoted string
 * */
enum parse_state { NORMAL, SINGLE_QUOTE, DOUBLE_QUOTE };

const char *pipeline_strerror(PipelineResult err) {
    switch (err) {
        /* parsing errors */
        case PIPELINE_ERR_MEMORY:
            return "out of memory";
        case PIPELINE_ERR_PARSE_TOO_MANY_ARGS:
            return "max argc exceeded";
        case PIPELINE_ERR_PARSE_TOO_MANY_STAGES:
            return "max stages exceeded";
        case PIPELINE_ERR_PARSE_EMPTY_STAGE:
            return "attempt to parse empty stage";
        case PIPELINE_ERR_PARSE_UNTERMINATED_QUOTE:
            return "unterminated quote";

        /* process errors */
        case PIPELINE_ERR_EXEC:
            return "exec failed";
        case PIPELINE_ERR_FORK:
            return "fork failed";
        case PIPELINE_ERR_PIPE:
            return "pipe creation failed";
        default:
            return "";
    }
}

/* ---- Parsing ---- */

/*
 * State-machine tokenizer with shell quoting support.
 *
 * Handles:
 *   'single quotes'  — literal content, no escaping inside
 *   "double quotes"  — literal content, \" and \\ are interpreted
 *   \x outside quotes — treats the next character literally
 *
 * Tokens are split on unquoted whitespace.
 */
static PipelineResult split_args(const char *cmd, char **argv, int max_args,
                      char *outbuf, int outbuf_size, int *argc_out)
{
    enum parse_state state = NORMAL;
    int argc = 0;
    int out  = 0;        /* position in outbuf */
    int in_token = 0;

    for (const char *p = cmd; *p != '\0'; p++) {
        if (out >= outbuf_size - 1)
            break;

        switch (state) {
        case NORMAL:
            if (*p == '\'') {
                state = SINGLE_QUOTE;
                if (!in_token) {
                    if (argc >= max_args - 1)
                        return PIPELINE_ERR_PARSE_TOO_MANY_ARGS;

                    argv[argc] = &outbuf[out];
                    in_token = 1;
                }

            } else if (*p == '"') {
                state = DOUBLE_QUOTE;
                if (!in_token) {
                    if (argc >= max_args - 1)
                        return PIPELINE_ERR_PARSE_TOO_MANY_ARGS;

                    argv[argc] = &outbuf[out];
                    in_token = 1;
                }

            } else if (*p == '\\' && *(p + 1) != '\0') {
                if (!in_token) {
                    if (argc >= max_args - 1)
                        return PIPELINE_ERR_PARSE_TOO_MANY_ARGS;

                    argv[argc] = &outbuf[out];
                    in_token = 1;
                }
                p++;
                outbuf[out++] = *p;

            /* handling whitespace */
            } else if (*p == ' ' || *p == '\t') {
                if (in_token) {
                    outbuf[out++] = '\0';
                    argc++;
                    in_token = 0;
                }

            } else {
                if (!in_token) {
                    if (argc >= max_args - 1)
                        return PIPELINE_ERR_PARSE_TOO_MANY_ARGS;

                    argv[argc] = &outbuf[out];
                    in_token = 1;
                }
                outbuf[out++] = *p;

            }
            break;

        case SINGLE_QUOTE:
            if (*p == '\'') {
                state = NORMAL;
            } else {
                outbuf[out++] = *p;
            }
            break;

        case DOUBLE_QUOTE:
            /* closing quote */
            if (*p == '"') {
                state = NORMAL;

            /* handling escape character */
            } else if (*p == '\\' && *(p + 1) != '\0' &&
                       (*(p + 1) == '"' || *(p + 1) == '\\')) {
                p++;
                outbuf[out++] = *p;

            } else {
                outbuf[out++] = *p;
            }
            break;
        }
    }

    /* handling unterminated quote */
    if (state != NORMAL)
        return PIPELINE_ERR_PARSE_UNTERMINATED_QUOTE;

    /* pushing remaining token */
    if (in_token) {
        outbuf[out++] = '\0';
        argc++;
    }
    *argc_out = argc;
    argv[argc] = NULL;

    return PIPELINE_OK;
}

/*
 * Split a command line on unquoted '|' characters into pipeline stages.
 * Respects single quotes, double quotes, and backslash escapes.
 */
PipelineResult pipeline_parse(const char *cmdline, Pipeline *pl)
{
    memset(pl, 0, sizeof(*pl));

    int len = (int)strlen(cmdline);

    /* Find unquoted pipe characters to split into segments */
    const char *seg_start = cmdline;
    enum parse_state state = NORMAL;

    for (int i = 0; i <= len; i++) {
        char c = (i < len) ? cmdline[i] : '\0';
        int is_pipe = 0;

        switch (state) {
        case NORMAL:
            if (c == '\'')      state = SINGLE_QUOTE;
            else if (c == '"')  state = DOUBLE_QUOTE;
            else if (c == '\\' && i + 1 < len) { i++; continue; }
            else if (c == '|' || c == '\0') is_pipe = 1;
            break;
        case SINGLE_QUOTE:
            if (c == '\'') state = NORMAL;
            break;
        case DOUBLE_QUOTE:
            if (c == '"') state = NORMAL;
            else if (c == '\\' && i + 1 < len) { i++; continue; }
            break;
        }

        if (is_pipe) {
            if (pl->num_stages >= MAX_STAGES)
                return PIPELINE_ERR_PARSE_TOO_MANY_STAGES;

            /* Extract this segment */
            int seg_len = (int)(&cmdline[i] - seg_start);
            if (seg_len <= 0 && c == '|') {
                return -1;  /* empty stage */
            }

            PipelineStage *s = &pl->stages[pl->num_stages];

            /* Copy segment for raw_cmd (trimmed) */
            const char *ts = seg_start;
            while (ts < &cmdline[i] && (*ts == ' ' || *ts == '\t')) ts++;
            const char *te = &cmdline[i] - 1;
            while (te > ts && (*te == ' ' || *te == '\t')) te--;

            int trimmed_len = (int)(te - ts + 1);
            if (trimmed_len <= 0) {
                if (c == '|') return -1;  /* empty stage */
                break;  /* trailing whitespace at end */
            }
            if (trimmed_len >= MAX_CMD_LEN)
                trimmed_len = MAX_CMD_LEN - 1;
            memcpy(s->raw_cmd, ts, (size_t)trimmed_len);
            s->raw_cmd[trimmed_len] = '\0';

            /* Parse into argv using the quoting-aware tokenizer */
            char *argbuf = malloc(trimmed_len + 1);
            if (!argbuf) 
                return PIPELINE_ERR_MEMORY;

            /* propagating error if tokenizer fails */
            PipelineResult err;
            err = split_args(s->raw_cmd, s->argv, MAX_ARGS, 
                    argbuf, trimmed_len + 1, &s->argc);

            if (err != PIPELINE_OK) {
                free(argbuf);
                return err;
            }

            /* Note: argv pointers point into argbuf which we intentionally
             * leak here (lives for program duration).  A production tool
             * would manage this memory properly. */

            pl->num_stages++;
            seg_start = &cmdline[i + 1];
        }
    }

    if (pl->num_stages == 0)
        return PIPELINE_ERR_PARSE_EMPTY_STAGE;

    return PIPELINE_OK;
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

PipelineResult pipeline_exec(const Pipeline *pl, PipelineExec *pe)
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
            return PIPELINE_ERR_PIPE;
        }
    }

    /* Also capture the last stage's stdout */
    int last_out[2] = {-1, -1};
    if (pipe(last_out) < 0) {
        return PIPELINE_ERR_PIPE;
    }

    /* Spawn each stage */
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            return PIPELINE_ERR_FORK;
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

    return PIPELINE_OK;
}

PipelineResult pipeline_wait(PipelineExec *pe, int exit_codes[])
{
    for (int i = 0; i < pe->num_stages; i++) {
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
    return PIPELINE_OK;
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
