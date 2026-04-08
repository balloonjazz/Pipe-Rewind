#include "capture.h"
#include "procstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/wait.h>

#define CAPTURE_BUF_SIZE  65536
#define MAX_EPOLL_EVENTS  64

/*
 * Map from file descriptor back to stage info so the epoll loop
 * knows which stage produced the data and where to forward it.
 */
typedef struct {
    int  fd;
    int  stage_id;       /* which pipeline stage this fd belongs to */
    int  forward_fd;     /* fd to forward data to (-1 = write to real stdout) */
    int  is_active;
} FdMapping;

int capture_init(CaptureEngine *ce, const char *cmdline,
                 const char *trace_path, int verbose)
{
    memset(ce, 0, sizeof(*ce));
    ce->trace_path = trace_path;
    ce->verbose    = verbose;

    if (pipeline_parse(cmdline, &ce->pipeline) < 0) {
        fprintf(stderr, "piperewind: failed to parse pipeline\n");
        return -1;
    }

    if (verbose)
        pipeline_dump(&ce->pipeline);

    return 0;
}

int capture_run(CaptureEngine *ce)
{
    int n = ce->pipeline.num_stages;

    /* Execute pipeline with interposed pipes */
    if (pipeline_exec(&ce->pipeline, &ce->exec) < 0) {
        fprintf(stderr, "piperewind: failed to execute pipeline\n");
        return -1;
    }

    /* Open trace file */
    if (trace_writer_open(&ce->writer, ce->trace_path, (uint32_t)n) < 0) {
        fprintf(stderr, "piperewind: failed to open trace file '%s'\n",
                ce->trace_path);
        return -1;
    }

    /* Record stage info (all headers first, then events) */
    for (int i = 0; i < n; i++) {
        trace_writer_add_stage(&ce->writer, (uint32_t)i,
                               (uint32_t)ce->exec.procs[i].pid,
                               ce->pipeline.stages[i].raw_cmd);
    }
    for (int i = 0; i < n; i++) {
        trace_writer_record(&ce->writer, (uint32_t)i, EVT_PROC_START,
                            NULL, 0);
    }

    /* Set up epoll */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return -1;
    }

    /*
     * Build fd-to-stage mappings.
     * We monitor each stage's capture_out_fd (the read end of the pipe
     * that captures that stage's stdout).
     *
     * When data arrives on stage[i].capture_out_fd, we:
     *   1. Record it as EVT_DATA_OUT for stage i
     *   2. Record it as EVT_DATA_IN for stage i+1
     *   3. Forward the bytes to stage[i+1].capture_in_fd
     *      (or to real stdout if i == n-1)
     */
    FdMapping mappings[MAX_STAGES];
    int num_active = 0;

    for (int i = 0; i < n; i++) {
        int fd = ce->exec.procs[i].capture_out_fd;
        if (fd < 0)
            continue;

        mappings[num_active].fd        = fd;
        mappings[num_active].stage_id  = i;
        mappings[num_active].is_active = 1;

        /* Where to forward: next stage's input, or real stdout */
        if (i < n - 1) {
            mappings[num_active].forward_fd = ce->exec.procs[i + 1].capture_in_fd;
        } else {
            mappings[num_active].forward_fd = STDOUT_FILENO;
        }

        struct epoll_event ev = {0};
        ev.events  = EPOLLIN;
        ev.data.u32 = (uint32_t)num_active;

        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl");
            close(epfd);
            return -1;
        }

        num_active++;
    }

    if (ce->verbose) {
        fprintf(stderr, "piperewind: capturing %d stages, "
                "monitoring %d fds\n", n, num_active);
    }

    /* Track last-seen process state to emit only on change */
    ProcState last_state[MAX_STAGES];
    memset(last_state, 0, sizeof(last_state));

    /* Event loop */
    uint8_t buf[CAPTURE_BUF_SIZE];
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int fds_open = num_active;

    while (fds_open > 0) {
        int nready = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 100);

        if (nready < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        /* Check for exited children (non-blocking) */
        for (int i = 0; i < n; i++) {
            if (ce->exec.procs[i].pid > 0) {
                int status;
                pid_t ret = waitpid(ce->exec.procs[i].pid, &status,
                                    WNOHANG);
                if (ret > 0) {
                    int code = WIFEXITED(status) ?
                               WEXITSTATUS(status) : -1;
                    uint32_t payload = (uint32_t)code;
                    trace_writer_record(&ce->writer, (uint32_t)i,
                                        EVT_PROC_EXIT,
                                        &payload, sizeof(payload));
                    ce->exec.procs[i].pid = -1;  /* mark as reaped */

                    if (ce->verbose) {
                        fprintf(stderr, "piperewind: stage %d (%s) "
                                "exited with code %d\n",
                                i, ce->pipeline.stages[i].raw_cmd, code);
                    }
                }
            }
        }

        /* Poll /proc for process state changes (on epoll timeout) */
        if (nready == 0) {
            for (int i = 0; i < n; i++) {
                if (ce->exec.procs[i].pid <= 0)
                    continue;

                ProcState cur = proc_read_state(ce->exec.procs[i].pid);
                if (cur != PROC_UNKNOWN && cur != last_state[i]) {
                    last_state[i] = cur;
                    uint8_t state_byte = (uint8_t)cur;
                    trace_writer_record(&ce->writer, (uint32_t)i,
                                        EVT_PROC_STATE,
                                        &state_byte, 1);
                    if (ce->verbose) {
                        fprintf(stderr, "piperewind: stage %d state -> %s\n",
                                i, proc_state_str(cur));
                    }
                }
            }
        }

        for (int e = 0; e < nready; e++) {
            uint32_t idx = events[e].data.u32;
            FdMapping *m = &mappings[idx];

            if (!m->is_active)
                continue;

            if (events[e].events & (EPOLLIN | EPOLLHUP)) {
                ssize_t nread = read(m->fd, buf, sizeof(buf));

                if (nread > 0) {
                    /* Record: this stage produced data */
                    trace_writer_record(&ce->writer,
                                        (uint32_t)m->stage_id,
                                        EVT_DATA_OUT,
                                        buf, (uint32_t)nread);

                    /* Record: next stage received data */
                    if (m->stage_id < n - 1) {
                        trace_writer_record(&ce->writer,
                                            (uint32_t)(m->stage_id + 1),
                                            EVT_DATA_IN,
                                            buf, (uint32_t)nread);
                    }

                    /* Forward to next stage (blocking write) */
                    if (m->forward_fd >= 0) {
                        ssize_t total = 0;
                        while (total < nread) {
                            ssize_t w = write(m->forward_fd,
                                              buf + total,
                                              (size_t)(nread - total));
                            if (w < 0) {
                                if (errno == EAGAIN || errno == EINTR)
                                    continue;
                                /* Broken pipe - downstream died */
                                break;
                            }
                            total += w;
                        }
                    }
                } else if (nread == 0) {
                    /* EOF on this pipe */
                    trace_writer_record(&ce->writer,
                                        (uint32_t)m->stage_id,
                                        EVT_PIPE_EOF, NULL, 0);

                    /* Close the forward fd to propagate EOF downstream */
                    if (m->forward_fd >= 0 &&
                        m->forward_fd != STDOUT_FILENO) {
                        close(m->forward_fd);
                        m->forward_fd = -1;
                    }

                    epoll_ctl(epfd, EPOLL_CTL_DEL, m->fd, NULL);
                    close(m->fd);
                    m->fd = -1;
                    m->is_active = 0;
                    fds_open--;

                    if (ce->verbose) {
                        fprintf(stderr, "piperewind: stage %d pipe "
                                "closed (%d remaining)\n",
                                m->stage_id, fds_open);
                    }
                } else {
                    /* read error */
                    if (errno != EAGAIN && errno != EINTR) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, m->fd, NULL);
                        close(m->fd);
                        m->fd = -1;
                        m->is_active = 0;
                        fds_open--;
                    }
                }
            }

            if (events[e].events & EPOLLERR) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, m->fd, NULL);
                close(m->fd);
                m->fd = -1;
                m->is_active = 0;
                fds_open--;
            }
        }
    }

    /* Reap any remaining children */
    for (int i = 0; i < n; i++) {
        if (ce->exec.procs[i].pid > 0) {
            int status;
            waitpid(ce->exec.procs[i].pid, &status, 0);
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            uint32_t payload = (uint32_t)code;
            trace_writer_record(&ce->writer, (uint32_t)i,
                                EVT_PROC_EXIT, &payload, sizeof(payload));
        }
    }

    /* Finalize trace */
    trace_writer_finalize(&ce->writer);

    close(epfd);
    return 0;
}

void capture_destroy(CaptureEngine *ce)
{
    trace_writer_close(&ce->writer);
    pipeline_cleanup(&ce->exec);
}
