#include "tui.h"
#include "procstate.h"

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Color pairs */
#define CP_HEADER    1
#define CP_TIMELINE  2
#define CP_SELECTED  3
#define CP_DATA_OUT  4
#define CP_DATA_IN   5
#define CP_RUNNING   6
#define CP_EXITED    7
#define CP_BLOCKED   8
#define CP_EOF       9
#define CP_HELP      10

/* Layout constants */
#define HEADER_ROWS  2
#define TIMELINE_ROWS 2
#define STAGE_MIN_ROWS 4
#define FOOTER_ROWS  2
#define DATA_MIN_ROWS 6

/* Stage states */
#define SS_WAITING  0
#define SS_RUNNING  1
#define SS_EXITED   2
#define SS_BLOCKED  3

static void tui_init_colors(void)
{
    start_color();
    use_default_colors();

    init_pair(CP_HEADER,   COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_TIMELINE, COLOR_BLACK,  COLOR_CYAN);
    init_pair(CP_SELECTED, COLOR_BLACK,  COLOR_WHITE);
    init_pair(CP_DATA_OUT, COLOR_GREEN,  -1);
    init_pair(CP_DATA_IN,  COLOR_YELLOW, -1);
    init_pair(CP_RUNNING,  COLOR_GREEN,  -1);
    init_pair(CP_EXITED,   COLOR_RED,    -1);
    init_pair(CP_BLOCKED,  COLOR_YELLOW, -1);
    init_pair(CP_EOF,      COLOR_MAGENTA,-1);
    init_pair(CP_HELP,     COLOR_WHITE,  COLOR_BLUE);
}

/*
 * Load all events and payloads into memory for fast access.
 * For large traces this would need windowing, but for typical
 * pipelines (< 100k events) it fits easily.
 */
static int tui_load_events(TuiState *ts)
{
    uint32_t n = ts->reader.num_events;
    if (n == 0)
        return 0;

    ts->events   = calloc(n, sizeof(TraceEvent));
    ts->payloads = calloc(n, sizeof(uint8_t *));
    if (!ts->events || !ts->payloads)
        return -1;

    /* Seek to start of event data */
    long events_start = (long)(sizeof(TraceFileHeader) +
                        ts->reader.header.num_stages * sizeof(TraceStageHeader));
    fseek(ts->reader.fp, events_start, SEEK_SET);

    for (uint32_t i = 0; i < n; i++) {
        if (fread(&ts->events[i], sizeof(TraceEvent), 1,
                  ts->reader.fp) != 1)
            break;

        if (ts->events[i].payload_len > 0) {
            ts->payloads[i] = malloc(ts->events[i].payload_len);
            if (ts->payloads[i]) {
                if (fread(ts->payloads[i], 1, ts->events[i].payload_len,
                          ts->reader.fp) != ts->events[i].payload_len) {
                    free(ts->payloads[i]);
                    ts->payloads[i] = NULL;
                }
            } else {
                fseek(ts->reader.fp, ts->events[i].payload_len, SEEK_CUR);
            }
        }
        ts->num_loaded = i + 1;
    }

    /* Calculate total duration */
    if (ts->num_loaded > 0)
        ts->total_duration_ns = ts->events[ts->num_loaded - 1].timestamp_ns;

    return 0;
}

/*
 * Compute stage states at a given event index by replaying from start.
 */
static void tui_compute_stage_states(TuiState *ts, uint32_t up_to_idx)
{
    uint32_t ns = ts->reader.header.num_stages;
    memset(ts->stage_states, SS_WAITING, ns);
    memset(ts->exit_codes, -1, ns * sizeof(int));

    for (uint32_t i = 0; i <= up_to_idx && i < ts->num_loaded; i++) {
        TraceEvent *e = &ts->events[i];
        if (e->stage_id >= ns) continue;

        switch (e->event_type) {
        case EVT_PROC_START:
            ts->stage_states[e->stage_id] = SS_RUNNING;
            break;
        case EVT_PROC_EXIT:
            ts->stage_states[e->stage_id] = SS_EXITED;
            if (e->payload_len >= 4 && ts->payloads[i])
                memcpy(&ts->exit_codes[e->stage_id], ts->payloads[i], 4);
            break;
        case EVT_PROC_STATE:
            if (e->payload_len >= 1 && ts->payloads[i]) {
                ProcState ps = (ProcState)ts->payloads[i][0];
                if (ps == PROC_SLEEPING || ps == PROC_DISK_SLEEP)
                    ts->stage_states[e->stage_id] = SS_BLOCKED;
                else if (ps == PROC_RUNNING)
                    ts->stage_states[e->stage_id] = SS_RUNNING;
            }
            break;
        default:
            break;
        }
    }
}

/* ---- Drawing functions ---- */

static void draw_header(TuiState *ts, int width)
{
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', width);
    mvprintw(0, 1, "PipeRewind: %s", ts->trace_path);

    char info[64];
    snprintf(info, sizeof(info), "%u stages  %u events  [q]uit [?]help",
             ts->reader.header.num_stages, ts->num_loaded);
    mvprintw(0, width - (int)strlen(info) - 1, "%s", info);
    attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
}

static void draw_timeline(TuiState *ts, int row, int width)
{
    double cur_ms  = (double)ts->current_time_ns / 1e6;
    double tot_ms  = (double)ts->total_duration_ns / 1e6;
    double frac    = (ts->total_duration_ns > 0)
                     ? (double)ts->current_time_ns / (double)ts->total_duration_ns
                     : 0.0;

    /* Label */
    attron(A_BOLD);
    mvprintw(row, 1, "Timeline:");
    attroff(A_BOLD);

    /* Bar area */
    int bar_start = 12;
    int bar_end   = width - 22;
    int bar_len   = bar_end - bar_start;
    if (bar_len < 10) bar_len = 10;

    int filled = (int)(frac * bar_len);
    if (filled > bar_len) filled = bar_len;

    move(row, bar_start);
    attron(COLOR_PAIR(CP_TIMELINE));
    for (int i = 0; i < filled; i++)
        addch(ACS_BLOCK);
    attroff(COLOR_PAIR(CP_TIMELINE));
    for (int i = filled; i < bar_len; i++)
        addch(ACS_BULLET);

    /* Time readout */
    mvprintw(row, bar_end + 2, "%7.2f / %.2f ms", cur_ms, tot_ms);

    /* Event counter */
    mvprintw(row + 1, 1, "Event: %u / %u",
             ts->current_event_idx + 1, ts->num_loaded);
}

static void draw_stages(TuiState *ts, int start_row, int width)
{
    uint32_t ns = ts->reader.header.num_stages;

    attron(A_BOLD);
    mvprintw(start_row, 1, "Pipeline Stages:");
    attroff(A_BOLD);

    for (uint32_t i = 0; i < ns; i++) {
        int row = start_row + 1 + (int)i;
        int is_sel = ((int)i == ts->selected_stage);

        if (is_sel)
            attron(COLOR_PAIR(CP_SELECTED));

        mvhline(row, 0, ' ', width);
        mvprintw(row, 1, "%c Stage %u: %-30s",
                 is_sel ? '>' : ' ',
                 i, ts->reader.stages[i].command);

        /* State badge */
        const char *state_str;
        int state_cp;
        switch (ts->stage_states[i]) {
        case SS_RUNNING:
            state_str = "RUNNING";
            state_cp  = CP_RUNNING;
            break;
        case SS_EXITED:
            state_str = "EXITED";
            state_cp  = CP_EXITED;
            break;
        case SS_BLOCKED:
            state_str = "BLOCKED";
            state_cp  = CP_BLOCKED;
            break;
        default:
            state_str = "WAITING";
            state_cp  = CP_BLOCKED;
            break;
        }

        if (is_sel) attroff(COLOR_PAIR(CP_SELECTED));

        attron(COLOR_PAIR(state_cp) | A_BOLD);
        mvprintw(row, width - 18, "[%s", state_str);
        if (ts->stage_states[i] == SS_EXITED && ts->exit_codes[i] >= 0)
            printw(" %d", ts->exit_codes[i]);
        printw("]");
        attroff(COLOR_PAIR(state_cp) | A_BOLD);
    }
}

static void draw_hex_line(int row, int col, const uint8_t *data,
                          uint32_t offset, uint32_t len, int width)
{
    uint32_t bytes_per_line = ((uint32_t)width - 14) / 4;
    if (bytes_per_line > 16) bytes_per_line = 16;

    mvprintw(row, col, "%04x: ", offset);

    /* Hex part */
    for (uint32_t i = 0; i < bytes_per_line; i++) {
        if (offset + i < len)
            printw("%02x ", data[offset + i]);
        else
            printw("   ");
    }

    printw(" ");

    /* ASCII part */
    for (uint32_t i = 0; i < bytes_per_line; i++) {
        if (offset + i < len) {
            uint8_t c = data[offset + i];
            addch(isprint(c) ? c : '.');
        }
    }
}

static void draw_data_panel(TuiState *ts, int start_row, int height,
                            int width)
{
    int row = start_row;
    double cur_ms = (double)ts->current_time_ns / 1e6;

    attron(A_BOLD);
    mvhline(row, 0, ACS_HLINE, width);
    mvprintw(row, 1, " Data at t=%.3f ms ", cur_ms);
    attroff(A_BOLD);
    row++;

    int data_rows = height - 1;
    int lines_used = 0;

    /* Show events near the current time for the selected stage */
    uint32_t idx = ts->current_event_idx;
    TraceEvent *e = &ts->events[idx];

    /* Show a window of recent events around current position */
    /* First, find a window of events near current time */
    int window_start = (int)idx - 3;
    if (window_start < 0) window_start = 0;
    int window_end = (int)idx + 3;
    if (window_end >= (int)ts->num_loaded) window_end = (int)ts->num_loaded - 1;

    for (int i = window_start; i <= window_end && lines_used < data_rows; i++) {
        e = &ts->events[i];
        int is_current = (i == (int)idx);
        int is_selected_stage = ((int)e->stage_id == ts->selected_stage);

        if (e->event_type != EVT_DATA_OUT && e->event_type != EVT_DATA_IN)
            continue;

        /* Event header line */
        if (is_current)
            attron(A_BOLD);

        int cp = (e->event_type == EVT_DATA_OUT) ? CP_DATA_OUT : CP_DATA_IN;
        const char *dir = (e->event_type == EVT_DATA_OUT) ? "OUT" : "IN ";
        double ms = (double)e->timestamp_ns / 1e6;

        if (is_selected_stage)
            attron(A_BOLD);
        attron(COLOR_PAIR(cp));
        mvprintw(row, 1, "%s Stage %u %s (%u bytes) @ %.3f ms",
                 is_current ? ">>" : "  ",
                 e->stage_id, dir, e->payload_len, ms);
        attroff(COLOR_PAIR(cp));
        if (is_selected_stage || is_current)
            attroff(A_BOLD);

        row++;
        lines_used++;

        /* Show payload for current event or selected stage events */
        if ((is_current || is_selected_stage) &&
            ts->payloads[i] && e->payload_len > 0 &&
            lines_used < data_rows) {

            if (ts->hex_mode) {
                uint32_t off = 0;
                uint32_t bytes_per_line = 16;
                while (off < e->payload_len && lines_used < data_rows - 1) {
                    draw_hex_line(row, 3, ts->payloads[i], off,
                                  e->payload_len, width);
                    off += bytes_per_line;
                    row++;
                    lines_used++;
                }
            } else {
                /* ASCII mode: show lines of text */
                const char *p = (const char *)ts->payloads[i];
                uint32_t remaining = e->payload_len;
                int max_show = width - 6;

                while (remaining > 0 && lines_used < data_rows - 1) {
                    /* Find next newline */
                    const char *nl = memchr(p, '\n', remaining);
                    int line_len;
                    if (nl)
                        line_len = (int)(nl - p);
                    else
                        line_len = (int)remaining;

                    if (line_len > max_show)
                        line_len = max_show;

                    mvprintw(row, 3, "| ");
                    for (int c = 0; c < line_len; c++) {
                        if (isprint((unsigned char)p[c]))
                            addch(p[c]);
                        else
                            addch('.');
                    }

                    int consumed = nl ? (int)(nl - p) + 1 : (int)remaining;
                    p += consumed;
                    remaining -= (uint32_t)consumed;
                    row++;
                    lines_used++;
                }
            }
        }
    }

    /* Fill remaining rows */
    while (lines_used < data_rows) {
        mvhline(row, 0, ' ', width);
        row++;
        lines_used++;
    }
}

/* ---- Diff panel ---- */

/*
 * Color pairs for diff: reuse CP_DATA_OUT for SAME, CP_EXITED for REMOVED,
 * CP_DATA_IN for ADDED.
 */
#define CP_DIFF_SAME    CP_DATA_OUT
#define CP_DIFF_REMOVED CP_EXITED
#define CP_DIFF_ADDED   CP_DATA_IN

static void draw_diff_panel(TuiState *ts, int start_row, int height, int width)
{
    int row = start_row;
    int sel = ts->selected_stage;
    uint32_t ns = ts->reader.header.num_stages;

    attron(A_BOLD);
    mvhline(row, 0, ACS_HLINE, width);
    mvprintw(row, 1, " Diff: Stage %d OUT vs Stage %d IN ",
             sel, sel + 1);
    attroff(A_BOLD);
    row++;

    int panel_rows = height - 1;

    if (sel < 0 || (uint32_t)sel >= ns - 1) {
        mvprintw(row, 3, "(Select a stage with a downstream neighbor for diff)");
        return;
    }

    /* Find the latest DATA_OUT for selected stage and DATA_IN for next stage
     * at or before the current event index */
    const uint8_t *left_data  = NULL;
    uint32_t       left_len   = 0;
    const uint8_t *right_data = NULL;
    uint32_t       right_len  = 0;

    for (uint32_t i = 0; i <= ts->current_event_idx && i < ts->num_loaded; i++) {
        TraceEvent *e = &ts->events[i];
        if (e->stage_id == (uint32_t)sel && e->event_type == EVT_DATA_OUT &&
            ts->payloads[i]) {
            left_data = ts->payloads[i];
            left_len  = e->payload_len;
        }
        if (e->stage_id == (uint32_t)(sel + 1) && e->event_type == EVT_DATA_IN &&
            ts->payloads[i]) {
            right_data = ts->payloads[i];
            right_len  = e->payload_len;
        }
    }

    if (!left_data && !right_data) {
        mvprintw(row, 3, "(No data events for these stages yet)");
        return;
    }

    /* Compute diff */
    DiffResult dr;
    if (diff_compute(left_data ? left_data : (const uint8_t *)"", left_len,
                     right_data ? right_data : (const uint8_t *)"", right_len,
                     &dr) < 0) {
        mvprintw(row, 3, "(Diff computation failed)");
        return;
    }

    /* Draw diff lines */
    int max_show = width - 6;
    int lines_shown = 0;

    for (int i = 0; i < dr.num_lines && lines_shown < panel_rows; i++) {
        DiffLine *dl = &dr.lines[i];
        char prefix;
        int cp;

        switch (dl->type) {
        case DIFF_SAME:    prefix = ' '; cp = CP_DIFF_SAME;    break;
        case DIFF_REMOVED: prefix = '-'; cp = CP_DIFF_REMOVED; break;
        case DIFF_ADDED:   prefix = '+'; cp = CP_DIFF_ADDED;   break;
        default:           prefix = '?'; cp = CP_DIFF_SAME;    break;
        }

        attron(COLOR_PAIR(cp));
        if (dl->type != DIFF_SAME)
            attron(A_BOLD);

        mvprintw(row, 1, "%c ", prefix);

        int show_len = dl->len;
        if (show_len > max_show)
            show_len = max_show;

        for (int c = 0; c < show_len; c++) {
            if (isprint((unsigned char)dl->text[c]))
                addch(dl->text[c]);
            else
                addch('.');
        }

        if (dl->type != DIFF_SAME)
            attroff(A_BOLD);
        attroff(COLOR_PAIR(cp));

        row++;
        lines_shown++;
    }

    diff_free(&dr);
}

static void draw_footer(int row, int width)
{
    attron(COLOR_PAIR(CP_HELP));
    mvhline(row, 0, ' ', width);
    mvprintw(row, 1,
        "[</> ] scrub  [j/k] step  [up/dn] stage  "
        "[h] hex  [d] diff  [Home/End] jump  [q] quit");
    attroff(COLOR_PAIR(CP_HELP));
}

static void draw_help_overlay(int height, int width)
{
    int bh = 18, bw = 52;
    int by = (height - bh) / 2;
    int bx = (width - bw) / 2;

    attron(COLOR_PAIR(CP_HEADER));
    for (int r = 0; r < bh; r++)
        mvhline(by + r, bx, ' ', bw);

    mvprintw(by + 0,  bx + 2, "PipeRewind Controls");
    mvhline( by + 1,  bx, ACS_HLINE, bw);
    mvprintw(by + 2,  bx + 2, "Left / Right    Scrub through time");
    mvprintw(by + 3,  bx + 2, "j / k           Step to next/prev event");
    mvprintw(by + 4,  bx + 2, "J / K           Jump 10 events");
    mvprintw(by + 5,  bx + 2, "Up / Down       Select pipeline stage");
    mvprintw(by + 6,  bx + 2, "h               Toggle hex / ASCII view");
    mvprintw(by + 7,  bx + 2, "d               Toggle diff view");
    mvprintw(by + 8,  bx + 2, "Home / End      Jump to start / end");
    mvprintw(by + 9,  bx + 2, "s               Toggle show all / selected");
    mvprintw(by + 10, bx + 2, "q               Quit");
    mvhline( by + 11, bx, ACS_HLINE, bw);
    mvprintw(by + 12, bx + 2, "Data panel shows bytes flowing");
    mvprintw(by + 13, bx + 2, "between stages at current time.");
    mvprintw(by + 14, bx + 2, "Use hex mode (h) for binary data.");
    mvhline( by + 15, bx, ACS_HLINE, bw);
    mvprintw(by + 16, bx + 2, "Press any key to close");

    attroff(COLOR_PAIR(CP_HEADER));
}

/* ---- Navigation ---- */

static void tui_goto_event(TuiState *ts, uint32_t idx)
{
    if (idx >= ts->num_loaded)
        idx = ts->num_loaded - 1;

    ts->current_event_idx = idx;
    ts->current_time_ns   = ts->events[idx].timestamp_ns;
    tui_compute_stage_states(ts, idx);
}

static void tui_step_forward(TuiState *ts, int count)
{
    uint32_t new_idx = ts->current_event_idx + (uint32_t)count;
    if (new_idx >= ts->num_loaded)
        new_idx = ts->num_loaded - 1;
    tui_goto_event(ts, new_idx);
}

static void tui_step_backward(TuiState *ts, int count)
{
    int new_idx = (int)ts->current_event_idx - count;
    if (new_idx < 0) new_idx = 0;
    tui_goto_event(ts, (uint32_t)new_idx);
}

static void tui_scrub(TuiState *ts, double fraction)
{
    /* Jump to a fractional position in the timeline */
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    uint64_t target_ns = (uint64_t)((double)ts->total_duration_ns * fraction);

    /* Binary search for nearest event */
    uint32_t lo = 0, hi = ts->num_loaded;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ts->events[mid].timestamp_ns < target_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= ts->num_loaded) lo = ts->num_loaded - 1;

    tui_goto_event(ts, lo);
}

/* ---- Main TUI loop ---- */

int tui_run(const char *trace_path)
{
    TuiState ts;
    memset(&ts, 0, sizeof(ts));
    ts.trace_path = trace_path;

    /* Open and load trace */
    if (trace_reader_open(&ts.reader, trace_path) < 0) {
        fprintf(stderr, "piperewind: cannot open trace '%s'\n", trace_path);
        return 1;
    }

    if (ts.reader.num_events == 0) {
        fprintf(stderr, "piperewind: trace has no events\n");
        trace_reader_close(&ts.reader);
        return 1;
    }

    if (tui_load_events(&ts) < 0) {
        fprintf(stderr, "piperewind: failed to load events\n");
        trace_reader_close(&ts.reader);
        return 1;
    }

    uint32_t ns = ts.reader.header.num_stages;
    ts.stage_states = calloc(ns, sizeof(uint8_t));
    ts.exit_codes   = calloc(ns, sizeof(int));
    if (!ts.stage_states || !ts.exit_codes) {
        trace_reader_close(&ts.reader);
        return 1;
    }

    /* Start at event 0 */
    tui_goto_event(&ts, 0);

    /* Initialize ncurses */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    tui_init_colors();

    int show_help = 0;
    int running = 1;

    while (running) {
        int height, width;
        getmaxyx(stdscr, height, width);
        erase();

        /* Layout: header(1) + timeline(2) + stages(ns+1) + data(rest) + footer(1) */
        int stages_section = (int)ns + 1;
        int data_start = HEADER_ROWS + TIMELINE_ROWS + stages_section + 1;
        int data_height = height - data_start - FOOTER_ROWS;
        if (data_height < DATA_MIN_ROWS) data_height = DATA_MIN_ROWS;

        draw_header(&ts, width);
        draw_timeline(&ts, HEADER_ROWS, width);
        draw_stages(&ts, HEADER_ROWS + TIMELINE_ROWS, width);

        if (ts.diff_mode)
            draw_diff_panel(&ts, data_start, data_height, width);
        else
            draw_data_panel(&ts, data_start, data_height, width);

        draw_footer(height - 1, width);

        if (show_help)
            draw_help_overlay(height, width);

        refresh();

        /* Handle input */
        int ch = getch();
        if (show_help) {
            show_help = 0;
            continue;
        }

        switch (ch) {
        case 'q':
        case 'Q':
            running = 0;
            break;

        case '?':
            show_help = 1;
            break;

        case 'k':  /* step backward */
            tui_step_backward(&ts, 1);
            break;
        case 'j':  /* step forward */
            tui_step_forward(&ts, 1);
            break;
        case 'K':  /* jump backward 10 */
            tui_step_backward(&ts, 10);
            break;
        case 'J':  /* jump forward 10 */
            tui_step_forward(&ts, 10);
            break;

        case KEY_LEFT:
            /* Scrub backward by ~2% */
            {
                double frac = (double)ts.current_time_ns /
                              (double)ts.total_duration_ns;
                tui_scrub(&ts, frac - 0.02);
            }
            break;
        case KEY_RIGHT:
            /* Scrub forward by ~2% */
            {
                double frac = (double)ts.current_time_ns /
                              (double)ts.total_duration_ns;
                tui_scrub(&ts, frac + 0.02);
            }
            break;

        case KEY_UP:
            if (ts.selected_stage > 0)
                ts.selected_stage--;
            break;
        case KEY_DOWN:
            if (ts.selected_stage < (int)ns - 1)
                ts.selected_stage++;
            break;

        case KEY_HOME:
            tui_goto_event(&ts, 0);
            break;
        case KEY_END:
            tui_goto_event(&ts, ts.num_loaded - 1);
            break;

        case 'h':
            ts.hex_mode = !ts.hex_mode;
            break;

        case 'd':
            ts.diff_mode = !ts.diff_mode;
            break;

        default:
            break;
        }
    }

    /* Cleanup */
    endwin();

    for (uint32_t i = 0; i < ts.num_loaded; i++)
        free(ts.payloads[i]);
    free(ts.events);
    free(ts.payloads);
    free(ts.stage_states);
    free(ts.exit_codes);
    trace_reader_close(&ts.reader);

    return 0;
}
