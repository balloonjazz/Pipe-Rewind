#include "tui.h"
#include "procstate.h"

#include <ncurses.h>
#include <stdlib.h>
#include <unistd.h>
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
 * Compute stage states at a given event index by replaying from start.
 * Since we don't hold the entire trace in memory anymore, we scan the
 * index and only load payloads for EXIT/STATE events.
 */
static void tui_compute_stage_states(TuiState *ts, uint32_t up_to_idx)
{
    uint32_t ns = ts->reader.header.num_stages;
    memset(ts->stage_states, SS_WAITING, ns);
    memset(ts->exit_codes, -1, ns * sizeof(int));

    for (uint32_t i = 0; i <= up_to_idx && i < ts->reader.num_events; i++) {
        TraceIndexEntry *ie = &ts->reader.index[i];
        if (ie->stage_id >= ns) continue;

        switch (ie->event_type) {
        case EVT_PROC_START:
            ts->stage_states[ie->stage_id] = SS_RUNNING;
            break;
        case EVT_PROC_EXIT:
            ts->stage_states[ie->stage_id] = SS_EXITED;
            /* exit code payload is 4 bytes */
            {
                uint32_t code;
                fseek(ts->reader.fp, (long)(ie->file_offset + sizeof(TraceEvent)), SEEK_SET);
                if (fread(&code, 4, 1, ts->reader.fp) == 1)
                    ts->exit_codes[ie->stage_id] = (int)code;
            }
            break;
        case EVT_PROC_STATE: {
            uint8_t ps_byte;
            fseek(ts->reader.fp, (long)(ie->file_offset + sizeof(TraceEvent)), SEEK_SET);
            if (fread(&ps_byte, 1, 1, ts->reader.fp) == 1) {
                ProcState ps = (ProcState)ps_byte;
                if (ps == PROC_SLEEPING || ps == PROC_DISK_SLEEP)
                    ts->stage_states[ie->stage_id] = SS_BLOCKED;
                else if (ps == PROC_RUNNING)
                    ts->stage_states[ie->stage_id] = SS_RUNNING;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void tui_free_window(TuiState *ts)
{
    if (ts->events) {
        uint32_t count = ts->window_end_idx - ts->window_start_idx;
        for (uint32_t i = 0; i < count; i++) {
            if (ts->payloads[i]) free(ts->payloads[i]);
        }
        free(ts->events);        ts->events = NULL;
        free(ts->payloads);      ts->payloads = NULL;
    }
    ts->window_start_idx = 0;
    ts->window_end_idx = 0;
}

static int tui_load_window(TuiState *ts, uint32_t center_idx)
{
    uint32_t n = ts->reader.num_events;
    if (n == 0) return 0;

    if (ts->window_capacity == 0)
        ts->window_capacity = 20000;

    uint32_t quarter = ts->window_capacity / 4;
    int in_bounds = (ts->events != NULL &&
                     center_idx >= ts->window_start_idx + quarter &&
                     center_idx < ts->window_end_idx - quarter);

    if (in_bounds && (!ts->live_mode || ts->window_end_idx == n))
        return 0;

    /* Rebuild window */
    tui_free_window(ts);

    uint32_t half = ts->window_capacity / 2;
    uint32_t start = (center_idx > half) ? center_idx - half : 0;
    uint32_t end = start + ts->window_capacity;
    if (end > n) {
        end = n;
        start = (end > ts->window_capacity) ? end - ts->window_capacity : 0;
    }

    uint32_t count = end - start;
    if (count > 0) {
        ts->events = calloc(count, sizeof(TraceEvent));
        ts->payloads = calloc(count, sizeof(uint8_t *));
        
        for (uint32_t i = 0; i < count; i++) {
            uint32_t gidx = start + i;
            if (trace_reader_read_event_at(&ts->reader, gidx, &ts->events[i], NULL, 0) == 0) {
                if (ts->events[i].payload_len > 0) {
                    ts->payloads[i] = malloc(ts->events[i].payload_len);
                    if (ts->payloads[i]) {
                        if (fread(ts->payloads[i], 1, ts->events[i].payload_len, ts->reader.fp) != ts->events[i].payload_len) {
                            free(ts->payloads[i]);
                            ts->payloads[i] = NULL;
                        }
                    }
                }
            }
        }
    }
    ts->window_start_idx = start;
    ts->window_end_idx = end;
    
    if (n > 0)
        ts->total_duration_ns = ts->reader.index[n - 1].timestamp_ns;

    return 0;
}



/* ---- Drawing functions ---- */

static void draw_header(TuiState *ts, int width)
{
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', width);
    char info[64];
    snprintf(info, sizeof(info), " %d stages | %u events ",
             ts->reader.header.num_stages, ts->reader.num_events);
    mvprintw(0, 1, "PipeRewind %s", ts->trace_path);
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
             ts->current_event_idx + 1, ts->reader.num_events);
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

    /* Show a window of recent events around current position */
    /* First, find a window of events near current time */
    int window_start = (int)idx - 3;
    if (window_start < 0) window_start = 0;
    int window_end = (int)idx + 3;
    if (window_end >= (int)ts->reader.num_events) window_end = (int)ts->reader.num_events - 1;

    for (int i = window_start; i <= window_end && lines_used < data_rows; i++) {
        /* Check if event is in loaded window */
        if (i < (int)ts->window_start_idx || i >= (int)ts->window_end_idx) continue;
        TraceEvent *e = &ts->events[i - ts->window_start_idx];
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
            ts->payloads[i - ts->window_start_idx] && e->payload_len > 0 &&
            lines_used < data_rows) {

            if (ts->hex_mode) {
                uint32_t off = 0;
                uint32_t bytes_per_line = 16;
                while (off < e->payload_len && lines_used < data_rows - 1) {
                    draw_hex_line(row, 3, ts->payloads[i - ts->window_start_idx], off,
                                  e->payload_len, width);
                    off += bytes_per_line;
                    row++;
                    lines_used++;
                }
            } else {
                /* ASCII mode: show lines of text */
                const char *p = (const char *)ts->payloads[i - ts->window_start_idx];
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

    for (uint32_t i = 0; i <= ts->current_event_idx && i < ts->reader.num_events; i++) {
        if (i < ts->window_start_idx || i >= ts->window_end_idx)
            continue;
        uint32_t local_idx = i - ts->window_start_idx;
        TraceEvent *e = &ts->events[local_idx];
        if (e->stage_id == (uint32_t)sel && e->event_type == EVT_DATA_OUT &&
            ts->payloads[local_idx]) {
            left_data = ts->payloads[local_idx];
            left_len  = e->payload_len;
        }
        if (e->stage_id == (uint32_t)(sel + 1) && e->event_type == EVT_DATA_IN &&
            ts->payloads[local_idx]) {
            right_data = ts->payloads[local_idx];
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
    if (ts->reader.num_events == 0)
        return;

    if (idx >= ts->reader.num_events)
        idx = ts->reader.num_events - 1;

    tui_load_window(ts, idx);
    ts->current_event_idx = idx;

    TraceIndexEntry *ie = &ts->reader.index[idx];
    ts->current_time_ns = ie->timestamp_ns;

    tui_compute_stage_states(ts, idx);
}

static void tui_step_forward(TuiState *ts, int count)
{
    uint32_t new_idx = ts->current_event_idx + (uint32_t)count;
    if (new_idx >= ts->reader.num_events)
        new_idx = ts->reader.num_events > 0 ? ts->reader.num_events - 1 : 0;
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
    if (ts->reader.num_events == 0) return;
    uint32_t lo = 0, hi = ts->reader.num_events;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ts->reader.index[mid].timestamp_ns < target_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= ts->reader.num_events) lo = ts->reader.num_events > 0 ? ts->reader.num_events - 1 : 0;

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

    ts.stage_states = calloc(ts.reader.header.num_stages, 1);
    ts.exit_codes   = calloc(ts.reader.header.num_stages, sizeof(int));
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

    if (ts.live_mode)
        halfdelay(1); /* 100ms timeout for live updates */

    while (running) {
        if (ts.live_mode) {
            int new_events = trace_reader_refresh(&ts.reader);
            if (new_events > 0 && ts.follow_mode) {
                tui_goto_event(&ts, ts.reader.num_events - 1);
            }
        }

        int height, width;
        getmaxyx(stdscr, height, width);
        erase();

        /* Layout: header(1) + timeline(2) + stages(ns+1) + data(rest) + footer(1) */
        uint32_t ns = ts.reader.header.num_stages;
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

        int ch = getch();
        if (ch == ERR) {
            /* timeout, just loop back to refresh */
            continue;
        }

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

        case 'f':
            ts.follow_mode = !ts.follow_mode;
            break;

        case 'k':  /* step backward */
            ts.follow_mode = 0;
            tui_step_backward(&ts, 1);
            break;
        case 'j':  /* step forward */
            tui_step_forward(&ts, 1);
            break;
        case 'K':  /* jump backward 10 */
            ts.follow_mode = 0;
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
            ts.follow_mode = 0;
            tui_goto_event(&ts, 0);
            break;
        case KEY_END:
            ts.follow_mode = 1;
            tui_goto_event(&ts, ts.reader.num_events - 1);
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

    free(ts.stage_states);
    free(ts.exit_codes);
    trace_reader_close(&ts.reader);
    tui_free_window(&ts);
    return 0;
}

/* ---- Live mode wrapper ---- */

#include "capture.h"

struct LiveCaptureArgs {
    CaptureEngine ce;
    int done;
};

static void *capture_thread_func(void *arg)
{
    struct LiveCaptureArgs *args = (struct LiveCaptureArgs *)arg;
    capture_run(&args->ce);
    capture_destroy(&args->ce);
    args->done = 1;
    return NULL;
}

int tui_run_live(const char *trace_path, const char *pipeline_cmd)
{
    struct LiveCaptureArgs args;
    memset(&args, 0, sizeof(args));

    if (capture_init(&args.ce, pipeline_cmd, trace_path, 0) < 0) {
        fprintf(stderr, "Error: failed to initialize capture engine\n");
        return -1;
    }

    TuiState ts;
    memset(&ts, 0, sizeof(ts));
    ts.trace_path = trace_path;
    ts.live_mode = 1;
    ts.follow_mode = 1;

    pthread_t thread;
    if (pthread_create(&thread, NULL, capture_thread_func, &args) != 0) {
        fprintf(stderr, "Error: failed to create capture thread\n");
        capture_destroy(&args.ce);
        return -1;
    }

    /* Wait briefly for headers to be written before reader opens */
    for (int i = 0; i < 50; i++) {
        if (trace_reader_open(&ts.reader, trace_path) == 0)
            break;
        usleep(10000); /* 10ms */
    }

    if (!ts.reader.fp) {
        fprintf(stderr, "Error: failed to open trace reader for live file\n");
        pthread_join(thread, NULL);
        return -1;
    }

    ts.stage_states = calloc(ts.reader.header.num_stages, 1);
    ts.exit_codes   = calloc(ts.reader.header.num_stages, sizeof(int));
    if (!ts.stage_states || !ts.exit_codes) {
        trace_reader_close(&ts.reader);
        pthread_join(thread, NULL);
        return -1;
    }

    tui_load_window(&ts, 0);
    tui_compute_stage_states(&ts, 0);

    /* Run the TUI */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    tui_init_colors();

    halfdelay(1); /* 100ms timeout */
    int show_help = 0;
    int running = 1;

    while (running) {
        int new_events = trace_reader_refresh(&ts.reader);
        if (new_events > 0 && ts.follow_mode) {
            tui_goto_event(&ts, ts.reader.num_events - 1);
        }

        int height, width;
        getmaxyx(stdscr, height, width);
        erase();

        uint32_t ns = ts.reader.header.num_stages;
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

        int ch = getch();
        if (ch == ERR) {
            if (args.done && ts.follow_mode) {
                /* Capture is done, follow mode is on, no user input: we can wait blockingly now?
                 * Actually, keep halfdelay so user doesn't get stuck */
            }
            continue;
        }

        if (show_help) {
            show_help = 0;
            continue;
        }

        switch (ch) {
        case 'q':
        case 'Q': running = 0; break;
        case '?': show_help = 1; break;
        case 'f': ts.follow_mode = !ts.follow_mode; break;
        case 'k': ts.follow_mode = 0; tui_step_backward(&ts, 1); break;
        case 'j': tui_step_forward(&ts, 1); break;
        case 'K': ts.follow_mode = 0; tui_step_backward(&ts, 10); break;
        case 'J': tui_step_forward(&ts, 10); break;
        case KEY_UP: if (ts.selected_stage > 0) ts.selected_stage--; break;
        case KEY_DOWN: if ((uint32_t)ts.selected_stage < ns - 1) ts.selected_stage++; break;
        case KEY_HOME: ts.follow_mode = 0; tui_goto_event(&ts, 0); break;
        case KEY_END: ts.follow_mode = 1; tui_goto_event(&ts, ts.reader.num_events - 1); break;
        case 'h': ts.hex_mode = !ts.hex_mode; break;
        case 'd': ts.diff_mode = !ts.diff_mode; break;
        case 's': ts.show_all_stages = !ts.show_all_stages; break;
        default: break;
        }
    }

    endwin();

    trace_reader_close(&ts.reader);
    tui_free_window(&ts);

    /* Clean up background thread if it's still running */
    if (!args.done) {
        /* Pipelined processes will exit when the TUI exits if we kill them?
         * For now, just detach or wait */
        pthread_cancel(thread);
    }
    pthread_join(thread, NULL);

    return 0;
}
