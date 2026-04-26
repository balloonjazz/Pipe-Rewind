#ifndef CAPTURE_H
#define CAPTURE_H

#include "pipeline.h"
#include "trace.h"

/*
 * The capture engine ties together the pipeline execution and the
 * trace writer.  It runs an epoll-based event loop that:
 *   1. Monitors all interposed pipe file descriptors
 *   2. Reads data as it flows between stages
 *   3. Forwards data to the next stage (transparent interposition)
 *   4. Records every byte with timestamps into the trace file
 *   5. Tracks process state transitions
 * The engine runs until all pipeline stages have exited and all
 * pipes have been drained.
 */

typedef struct {
    Pipeline      pipeline;
    PipelineExec  exec;
    TraceWriter   writer;
    const char   *trace_path;
    int           verbose;
    volatile int stop_requested;
} CaptureEngine;

/*
 * Initialize the capture engine with a command line string.
 * Parses the pipeline and prepares for execution.
 */
int capture_init(CaptureEngine *ce, const char *cmdline,
                 const char *trace_path, int verbose);

/*
 * Run the capture engine.
 * This spawns the pipeline, enters the epoll event loop,
 * records all data, and returns when the pipeline completes.
 * Returns 0 on success, -1 on error.
 */
int capture_run(CaptureEngine *ce);

/*
 * Clean up all resources.
 */
void capture_destroy(CaptureEngine *ce);

#endif