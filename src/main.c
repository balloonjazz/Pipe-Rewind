#include "capture.h"
#include "trace.h"
#include "tui.h"
#include "procstate.h"

#include <stdio.h>
#include <string.h>
#include <getopt.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "PipeRewind - Time-Travel Debugger for Shell Pipelines\n"
        "\n"
        "Usage:\n"
        "  %s record  [-v] [-o trace.prt] \"cmd1 | cmd2 | cmd3\"\n"
        "  %s replay  trace.prt\n"
        "  %s live    [-o trace.prt] \"cmd1 | cmd2 | cmd3\"\n"
        "  %s dump    trace.prt\n"
        "\n"
        "Commands:\n"
        "  record   Run a pipeline and record all inter-stage data into a file\n"
        "  replay   Open the TUI to scrub through a recording\n"
        "  live     Run pipeline and open TUI immediately to watch data live\n"
        "  dump     Print all events in a trace file (text format)\n"
        "\n"
        "Options:\n"
        "  -o FILE  Output trace file (default: trace.prt)\n"
        "  -v       Verbose output (show capture progress on stderr)\n"
        "  -s N     (replay) Focus on stage N\n"
        "  -t MS    (replay) Seek to timestamp in milliseconds\n"
        "  -h       Show this help\n"
        "\n"
        "Examples:\n"
        "  %s record \"cat /var/log/syslog | grep error | sort | uniq -c\"\n"
        "  %s dump trace.prt\n",
        prog, prog, prog, prog, prog, prog);
}

/* ---- dump command ---- */

static const char *event_type_str(uint8_t t)
{
    switch (t) {
    case EVT_DATA_IN:    return "DATA_IN";
    case EVT_DATA_OUT:   return "DATA_OUT";
    case EVT_PROC_START: return "PROC_START";
    case EVT_PROC_EXIT:  return "PROC_EXIT";
    case EVT_PIPE_EOF:   return "PIPE_EOF";
    case EVT_PROC_STATE: return "PROC_STATE";
    default:             return "UNKNOWN";
    }
}

static int cmd_dump(const char *path)
{
    TraceReader tr;
    if (trace_reader_open(&tr, path) < 0) {
        fprintf(stderr, "piperewind: cannot open trace '%s'\n", path);
        return 1;
    }

    printf("=== PipeRewind Trace: %s ===\n", path);
    printf("Stages: %u   Events: %u\n\n",
           tr.header.num_stages, tr.header.num_events);

    for (uint32_t i = 0; i < tr.header.num_stages; i++) {
        printf("  Stage %u [pid %u]: %s\n",
               tr.stages[i].stage_id,
               tr.stages[i].pid,
               tr.stages[i].command);
    }
    printf("\n--- Events ---\n");

    /* Seek to start of events */
    long events_start = (long)(sizeof(TraceFileHeader) +
                        tr.header.num_stages * sizeof(TraceStageHeader));
    fseek(tr.fp, events_start, SEEK_SET);

    uint8_t payload_buf[4096];
    TraceEvent evt;
    int count = 0;

    while (count < (int)tr.header.num_events &&
           trace_reader_next_event(&tr, &evt, payload_buf,
                                    sizeof(payload_buf)) == 0) {
        double ms = (double)evt.timestamp_ns / 1e6;

        printf("[%8.3f ms] stage %u  %-12s",
               ms, evt.stage_id, event_type_str(evt.event_type));

        if (evt.event_type == EVT_DATA_IN || evt.event_type == EVT_DATA_OUT) {
            printf("  %u bytes", evt.payload_len);

            /* Show a preview of the data */
            if (evt.payload_len > 0) {
                printf("  \"");
                uint32_t show = evt.payload_len < 60 ? evt.payload_len : 60;
                for (uint32_t i = 0; i < show; i++) {
                    uint8_t c = payload_buf[i];
                    if (c == '\n')
                        printf("\\n");
                    else if (c == '\t')
                        printf("\\t");
                    else if (c >= 32 && c < 127)
                        putchar(c);
                    else
                        printf("\\x%02x", c);
                }
                if (evt.payload_len > 60)
                    printf("...");
                printf("\"");
            }
        } else if (evt.event_type == EVT_PROC_EXIT && evt.payload_len >= 4) {
            uint32_t code;
            memcpy(&code, payload_buf, 4);
            printf("  exit_code=%u", code);
        } else if (evt.event_type == EVT_PROC_STATE && evt.payload_len >= 1) {
            ProcState ps = (ProcState)payload_buf[0];
            printf("  state=%s", proc_state_str(ps));
        }

        printf("\n");
        count++;

        /* Safety limit for dump */
        if (count > 100000) {
            printf("... (truncated at 100000 events)\n");
            break;
        }
    }

    printf("\n--- %d events total ---\n", count);
    trace_reader_close(&tr);
    return 0;
}

/* ---- record command ---- */

static int cmd_record(const char *cmdline, const char *trace_path,
                      int verbose)
{
    CaptureEngine ce;

    if (capture_init(&ce, cmdline, trace_path, verbose) < 0)
        return 1;

    fprintf(stderr, "piperewind: recording \"%s\" -> %s\n",
            cmdline, trace_path);

    int ret = capture_run(&ce);

    capture_destroy(&ce);

    if (ret == 0) {
        fprintf(stderr, "piperewind: trace saved to %s\n", trace_path);
        fprintf(stderr, "piperewind: run '%s dump %s' to inspect\n",
                "piperewind", trace_path);
    }

    return ret == 0 ? 0 : 1;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (strcmp(cmd, "dump") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s dump <trace.prt>\n", argv[0]);
            return 1;
        }
        return cmd_dump(argv[2]);
    }

    if (strcmp(cmd, "record") == 0) {
        const char *trace_path = "trace.prt";
        int verbose = 0;

        /* Parse options after "record" */
        int opt;
        optind = 2;  /* start after "record" */
        while ((opt = getopt(argc, argv, "o:vh")) != -1) {
            switch (opt) {
            case 'o': trace_path = optarg; break;
            case 'v': verbose = 1; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
            }
        }

        if (optind >= argc) {
            fprintf(stderr, "piperewind: missing pipeline command\n");
            fprintf(stderr, "Usage: %s record \"cmd1 | cmd2\"\n", argv[0]);
            return 1;
        }

        return cmd_record(argv[optind], trace_path, verbose);
    }

    if (strcmp(cmd, "replay") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s replay <trace.prt>\n", argv[0]);
            return 1;
        }
        return tui_run(argv[2]);
    }

    if (strcmp(cmd, "live") == 0) {
        const char *trace_path = "trace.prt";

        int opt;
        optind = 2;  /* start after "live" */
        while ((opt = getopt(argc, argv, "o:h")) != -1) {
            switch (opt) {
            case 'o': trace_path = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
            }
        }

        if (optind >= argc) {
            fprintf(stderr, "Error: missing pipeline command.\n");
            usage(argv[0]);
            return 1;
        }

        return tui_run_live(trace_path, argv[optind]);
    }

    fprintf(stderr, "Error: unknown command '%s'\n", cmd);
    usage(argv[0]);
    return 1;
}
