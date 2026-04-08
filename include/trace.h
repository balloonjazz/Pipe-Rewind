#ifndef PIPEREWIND_TRACE_H
#define PIPEREWIND_TRACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

/*
 * PipeRewind Trace File Format (.prt)
 *
 * Layout:
 *   [FileHeader]
 *   [StageHeader] x num_stages
 *   [Event] [Event] [Event] ...   (variable-length, each followed by payload)
 *   [IndexEntry] x num_events     (written at end for random access)
 *
 * All multi-byte integers are little-endian.
 */

#define PRT_MAGIC 0x54525050  /* "PPRT" */
#define PRT_VERSION 1

/* Stored at the very beginning of the trace file */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_stages;        /* number of pipeline stages */
    uint64_t start_time_ns;     /* CLOCK_MONOTONIC at pipeline start */
    uint64_t wall_start_sec;    /* time_t at pipeline start (for display) */
    uint32_t num_events;        /* total events (written at finalize) */
    uint64_t index_offset;      /* byte offset to index table (written at finalize) */
    uint8_t  reserved[32];
} __attribute__((packed)) TraceFileHeader;

/* One per pipeline stage, follows the file header */
typedef struct {
    uint32_t stage_id;
    uint32_t pid;
    char     command[256];      /* the argv[0] or full command string */
} __attribute__((packed)) TraceStageHeader;

/* Event types */
typedef enum {
    EVT_DATA_IN   = 1,  /* data read by a stage from its stdin pipe */
    EVT_DATA_OUT  = 2,  /* data written by a stage to its stdout pipe */
    EVT_PROC_START = 3, /* stage process started */
    EVT_PROC_EXIT  = 4, /* stage process exited */
    EVT_PIPE_EOF   = 5, /* EOF on a pipe */
    EVT_PROC_STATE = 6, /* process scheduling state snapshot */
} TraceEventType;

/* Each event in the stream */
typedef struct {
    uint64_t timestamp_ns;      /* offset from start_time_ns */
    uint32_t stage_id;          /* which pipeline stage */
    uint8_t  event_type;        /* TraceEventType */
    uint32_t payload_len;       /* bytes of payload following this header */
    /* payload bytes follow immediately */
} __attribute__((packed)) TraceEvent;

/* Index entry for random access (written at end of file) */
typedef struct {
    uint64_t timestamp_ns;
    uint64_t file_offset;       /* byte offset to the TraceEvent */
    uint32_t stage_id;
    uint8_t  event_type;
} __attribute__((packed)) TraceIndexEntry;

/*
 * Trace writer context
 */
typedef struct {
    FILE     *fp;
    uint64_t  start_time_ns;
    uint32_t  num_stages;
    uint32_t  num_events;

    /* In-memory index (grown dynamically) */
    TraceIndexEntry *index;
    uint32_t         index_cap;
} TraceWriter;

/*
 * Trace reader context
 */
typedef struct {
    FILE            *fp;
    TraceFileHeader  header;
    TraceStageHeader *stages;

    /* Loaded index for random access */
    TraceIndexEntry *index;
    uint32_t         num_events;
} TraceReader;

/* Writer API */
int  trace_writer_open(TraceWriter *tw, const char *path, uint32_t num_stages);
int  trace_writer_add_stage(TraceWriter *tw, uint32_t stage_id, uint32_t pid,
                            const char *command);
int  trace_writer_record(TraceWriter *tw, uint32_t stage_id,
                         TraceEventType type, const void *payload,
                         uint32_t payload_len);
int  trace_writer_finalize(TraceWriter *tw);
void trace_writer_close(TraceWriter *tw);

/* Reader API */
int  trace_reader_open(TraceReader *tr, const char *path);
int  trace_reader_seek_time(TraceReader *tr, uint64_t timestamp_ns);
int  trace_reader_next_event(TraceReader *tr, TraceEvent *evt,
                             void *payload_buf, uint32_t buf_size);
int  trace_reader_get_events_for_stage(TraceReader *tr, uint32_t stage_id,
                                       uint64_t from_ns, uint64_t to_ns,
                                       TraceEvent **events, void ***payloads,
                                       uint32_t *count);
void trace_reader_close(TraceReader *tr);

/* Utility */
uint64_t trace_now_ns(void);

#endif /* PIPEREWIND_TRACE_H */
