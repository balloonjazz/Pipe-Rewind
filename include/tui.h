#ifndef PIPEREWIND_TUI_H
#define PIPEREWIND_TUI_H

#include "trace.h"
#include "diff.h"

/*
 * PipeRewind TUI - ncurses-based time-travel replay viewer
 *
 * Layout:
 *   ┌─────────────────────────────────────────────────────────┐
 *   │ PipeRewind: trace.prt                    [q]uit [?]help │
 *   ├─────────────────────────────────────────────────────────┤
 *   │ Timeline: ████████░░░░░░░░░░░░░░░░░░░  3.21 / 10.56 ms│
 *   ├─────────────────────────────────────────────────────────┤
 *   │ > Stage 0: cat file.txt      [EXITED 0]                │
 *   │   Stage 1: grep ERROR        [RUNNING]                 │
 *   │   Stage 2: sort              [BLOCKED]                 │
 *   │   Stage 3: uniq -c           [WAITING]                 │
 *   ├─────────────────────────────────────────────────────────┤
 *   │ Data flow at t=3.21ms                                   │
 *   │ ─── Stage 0 OUT (12 bytes) ───                         │
 *   │ hello world                                             │
 *   │ ─── Stage 1 IN  (12 bytes) ───                         │
 *   │ hello world                                             │
 *   ├─────────────────────────────────────────────────────────┤
 *   │ [←/→] scrub  [j/k] step  [0-9] stage  [h] hex  [q] quit│
 *   └─────────────────────────────────────────────────────────┘
 *
 * Controls:
 *   Left/Right  - scrub through time (coarse)
 *   j/k         - step to prev/next event
 *   Up/Down     - select stage for detail view
 *   h           - toggle hex/ascii display
 *   Home/End    - jump to start/end
 *   q           - quit
 */

typedef struct {
    TraceReader   reader;
    const char   *trace_path;

    /* Playback state */
    uint32_t      current_event_idx;   /* index into reader.index */
    uint64_t      current_time_ns;
    uint64_t      total_duration_ns;
    int           selected_stage;

    /* Display options */
    int           hex_mode;            /* 0 = ascii, 1 = hex dump */
    int           show_all_stages;     /* 0 = selected only, 1 = all */
    int           diff_mode;           /* 0 = normal, 1 = diff view */

    /* Preloaded events for current view window */
    TraceEvent   *events;
    uint8_t     **payloads;
    uint32_t      num_loaded;

    /* Stage state tracking */
    uint8_t      *stage_states;  /* per-stage: 0=waiting, 1=running, 2=exited */
    int          *exit_codes;
} TuiState;

/*
 * Open the TUI on a trace file.
 * Takes over the terminal until the user quits.
 * Returns 0 on normal exit.
 */
int tui_run(const char *trace_path);

#endif /* PIPEREWIND_TUI_H */
