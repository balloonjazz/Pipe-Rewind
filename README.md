# PipeRewind

**A Time-Travel Debugger for Unix Shell Pipelines**

PipeRewind captures, records, and replays every byte flowing through a Unix shell pipeline. It interposes transparent capture taps between each stage of a pipeline, recording all data with precise timestamps into a binary trace file that supports random-access playback.

## Building

```bash
make          # build the piperewind binary
make test     # run smoke tests
make clean    # remove build artifacts
```

Requires: GCC, Linux (uses epoll, /proc).

## Usage

### Record a pipeline

```bash
./piperewind record "cat access.log | grep ERROR | sort | uniq -c"
```

This runs the pipeline normally (output appears on stdout as usual) while silently capturing all inter-stage data into `trace.prt`.

Options:
- `-o FILE` — set output trace file (default: `trace.prt`)
- `-v` — verbose mode (print capture progress on stderr)

### Inspect a trace

```bash
./piperewind dump trace.prt
```

Prints every recorded event with timestamps, showing what each stage received and produced at each moment.

### Replay (TUI) — coming soon

```bash
./piperewind replay trace.prt
```

Will open an ncurses-based TUI with a timeline scrubber.

## Architecture

```
                     PipeRewind Capture Engine (epoll loop)
                     ┌──────────────────────────────────────┐
                     │                                      │
  [stage 0] ──pipe──>│ read ──record──> forward ──pipe──>   │ [stage 1] ──pipe──> ...
                     │                                      │
                     └──────────────────────────────────────┘
                                    │
                                    ▼
                            trace.prt (binary)
                     ┌──────────────────────────┐
                     │ FileHeader                │
                     │ StageHeader × N           │
                     │ Event Event Event ...     │
                     │ IndexEntry × total_events │
                     └──────────────────────────┘
```

For N pipeline stages, PipeRewind creates 2×(N-1) kernel pipes plus 1 for the last stage's stdout. The capture engine sits in the parent process, using epoll to multiplex reads from all stage outputs. When data arrives from stage i, the engine:

1. Records a `DATA_OUT` event for stage i
2. Records a `DATA_IN` event for stage i+1
3. Forwards the bytes to stage i+1's stdin pipe

This is fully transparent — the pipeline produces the same output it would without PipeRewind.

## Trace File Format (.prt)

Binary format with a fixed header, per-stage metadata, a stream of variable-length events, and an index table at the end for random access. See `include/trace.h` for the full specification.

In the live demo you will see the back and forth scrubbing of the timeline.

## Course Themes

| Theme | How it's used |
|-------|---------------|
| Process Control | fork/exec to spawn stages, waitpid for lifecycle, ptrace for observation |
| Event-Based Concurrency | epoll event loop multiplexing N pipe file descriptors |
| System-Level I/O | Raw read/write on pipes, dup2 for fd manipulation, non-blocking I/O |
| Process Scheduling | Observing kernel scheduling via /proc, analyzing blocked vs. runnable states |

## Challenges Encountered & Lessons Learned
1.Pipe topology complexity was the first real wall we hit. Getting the supervisor to sit between every stage without breaking the pipeline's natural behavior required careful management of which ends of which pipes the parent held open versus which the children inherited. A single fd left open in the wrong process caused stages to hang indefinitely waiting for EOF that never came. The lesson: draw the full pipe diagram on paper before writing a single line of dup2.
2.Binary trace format correctness was harder than expected. Our first trace_writer_finalize implementation rewrote the file header from scratch, which silently zeroed out the wall-clock start time that had been written at open time. We didn't catch it until the dump command started showing wrong timestamps. The fix was simple — read the original header back before patching it — but it taught us to be deliberate about which fields are written when.
3.The LCS diff algorithm doesn't scale naively. Our diff_compute function allocates an O(n·m) table, which works fine for typical pipeline output but hits memory limits for large inputs. We added a 2000-line cap with a sequential fallback, which taught us that correctness under normal conditions isn't enough — you have to think about what happens when inputs are pathological.

## Team

- Luiz Takahashi (GitHub: LTTakahashi)
- Ehzoc Chavez (GitHub: balloonjazz)
- Fellow (Github: samzeas)

## License

Academic project — Washington State University, Systems Programming.
