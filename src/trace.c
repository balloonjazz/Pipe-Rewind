#include "trace.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_INDEX_CAP 4096

uint64_t trace_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------- Writer ---------- */

int trace_writer_open(TraceWriter *tw, const char *path, uint32_t num_stages)
{
    memset(tw, 0, sizeof(*tw));

    tw->fp = fopen(path, "wb");
    if (!tw->fp)
        return -1;

    tw->start_time_ns = trace_now_ns();
    tw->num_stages    = num_stages;
    tw->num_events    = 0;

    /* Allocate index buffer */
    tw->index_cap = INITIAL_INDEX_CAP;
    tw->index = calloc(tw->index_cap, sizeof(TraceIndexEntry));
    if (!tw->index) {
        fclose(tw->fp);
        return -1;
    }

    /* Write file header (will be updated at finalize) */
    TraceFileHeader hdr = {0};
    hdr.magic          = PRT_MAGIC;
    hdr.version        = PRT_VERSION;
    hdr.num_stages     = num_stages;
    hdr.start_time_ns  = tw->start_time_ns;
    hdr.wall_start_sec = (uint64_t)time(NULL);

    if (fwrite(&hdr, sizeof(hdr), 1, tw->fp) != 1) {
        fclose(tw->fp);
        free(tw->index);
        return -1;
    }

    return 0;
}

int trace_writer_add_stage(TraceWriter *tw, uint32_t stage_id,
                           uint32_t pid, const char *command)
{
    TraceStageHeader sh = {0};
    sh.stage_id = stage_id;
    sh.pid      = pid;
    strncpy(sh.command, command, sizeof(sh.command) - 1);

    if (fwrite(&sh, sizeof(sh), 1, tw->fp) != 1)
        return -1;

    return 0;
}

int trace_writer_record(TraceWriter *tw, uint32_t stage_id,
                        TraceEventType type, const void *payload,
                        uint32_t payload_len)
{
    /* Grow index if needed */
    if (tw->num_events >= tw->index_cap) {
        uint32_t new_cap = tw->index_cap * 2;
        TraceIndexEntry *new_idx = realloc(tw->index,
                                           new_cap * sizeof(TraceIndexEntry));
        if (!new_idx)
            return -1;
        tw->index     = new_idx;
        tw->index_cap = new_cap;
    }

    uint64_t now = trace_now_ns();
    uint64_t ts  = now - tw->start_time_ns;
    uint64_t offset = (uint64_t)ftell(tw->fp);

    /* Write event header */
    TraceEvent evt = {0};
    evt.timestamp_ns = ts;
    evt.stage_id     = stage_id;
    evt.event_type   = (uint8_t)type;
    evt.payload_len  = payload_len;

    if (fwrite(&evt, sizeof(evt), 1, tw->fp) != 1)
        return -1;

    /* Write payload */
    if (payload_len > 0 && payload) {
        if (fwrite(payload, 1, payload_len, tw->fp) != payload_len)
            return -1;
    }

    /* Record in index */
    TraceIndexEntry *ie = &tw->index[tw->num_events];
    ie->timestamp_ns = ts;
    ie->file_offset  = offset;
    ie->stage_id     = stage_id;
    ie->event_type   = (uint8_t)type;

    tw->num_events++;
    fflush(tw->fp);
    return 0;
}

int trace_writer_finalize(TraceWriter *tw)
{
    if (!tw->fp)
        return -1;

    /* Record where the index starts */
    uint64_t index_offset = (uint64_t)ftell(tw->fp);

    /* Write index entries */
    if (tw->num_events > 0) {
        if (fwrite(tw->index, sizeof(TraceIndexEntry),
                   tw->num_events, tw->fp) != tw->num_events)
            return -1;
    }

    /* Go back and update the file header */
    fseek(tw->fp, 0, SEEK_SET);

    TraceFileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, tw->fp) != 1)

        return -1;

    hdr.num_events   = tw->num_events;

    hdr.index_offset = index_offset;

    fseek(tw->fp, 0, SEEK_SET);

    if (fwrite(&hdr, sizeof(hdr), 1, tw->fp) != 1)
        return -1;

    fflush(tw->fp);
    return 0;
}

void trace_writer_close(TraceWriter *tw)
{
    if (tw->fp) {
        fclose(tw->fp);
        tw->fp = NULL;
    }
    free(tw->index);
    tw->index = NULL;
}

/* ---------- Reader ---------- */

int trace_reader_open(TraceReader *tr, const char *path)
{
    memset(tr, 0, sizeof(*tr));

    tr->fp = fopen(path, "rb");
    if (!tr->fp)
        return -1;

    /* Read file header */
    if (fread(&tr->header, sizeof(tr->header), 1, tr->fp) != 1)
        goto fail;

    if (tr->header.magic != PRT_MAGIC || tr->header.version != PRT_VERSION)
        goto fail;

    /* Read stage headers */
    tr->stages = calloc(tr->header.num_stages, sizeof(TraceStageHeader));
    if (!tr->stages)
        goto fail;

    for (uint32_t i = 0; i < tr->header.num_stages; i++) {
        if (fread(&tr->stages[i], sizeof(TraceStageHeader), 1, tr->fp) != 1)
            goto fail;
    }

    /* Load index from end of file */
    tr->num_events = tr->header.num_events;
    tr->index_cap  = tr->num_events > 0 ? tr->num_events : 256;
    if (tr->index_cap > 0) {
        tr->index = calloc(tr->index_cap, sizeof(TraceIndexEntry));
        if (!tr->index)
            goto fail;
    }

    if (tr->num_events > 0) {
        if (!tr->index)
            goto fail;

        fseek(tr->fp, (long)tr->header.index_offset, SEEK_SET);
        if (fread(tr->index, sizeof(TraceIndexEntry),
                  tr->num_events, tr->fp) != tr->num_events)
            goto fail;
    }

    /* Seek back to start of events */
    long events_start = (long)(sizeof(TraceFileHeader) +
                        tr->header.num_stages * sizeof(TraceStageHeader));
    fseek(tr->fp, events_start, SEEK_SET);

    return 0;

fail:
    trace_reader_close(tr);
    return -1;
}

int trace_reader_seek_time(TraceReader *tr, uint64_t timestamp_ns)
{
    if (!tr->index || tr->num_events == 0)
        return -1;

    /* Binary search for the first event at or after timestamp_ns */
    uint32_t lo = 0, hi = tr->num_events;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (tr->index[mid].timestamp_ns < timestamp_ns)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo >= tr->num_events)
        return -1;

    fseek(tr->fp, (long)tr->index[lo].file_offset, SEEK_SET);
    return 0;
}

int trace_reader_next_event(TraceReader *tr, TraceEvent *evt,
                            void *payload_buf, uint32_t buf_size)
{
    if (fread(evt, sizeof(*evt), 1, tr->fp) != 1)
        return -1;

    if (evt->payload_len > 0) {
        if (payload_buf && buf_size >= evt->payload_len) {
            if (fread(payload_buf, 1, evt->payload_len, tr->fp)
                    != evt->payload_len)
                return -1;
        } else {
            /* Skip payload if buffer too small */
            fseek(tr->fp, evt->payload_len, SEEK_CUR);
        }
    }

    return 0;
}

int trace_reader_refresh(TraceReader *tr)
{
    /* Find the last known event boundary */
    long pos;
    if (tr->num_events == 0) {
        pos = (long)(sizeof(TraceFileHeader) +
                     tr->header.num_stages * sizeof(TraceStageHeader));
    } else {
        /*
         * Cannot just use tr->index[num_events-1].offset because that's the
         * timestamp or relative offset? Wait, trace.h says:
         * uint64_t timestamp_ns; uint64_t file_offset;
         * Wait, trace.c writes the index during finalize or dynamically?
         */
        pos = (long)tr->index[tr->num_events - 1].file_offset;
        
        /* We need to advance past the last event's payload */
        /* But wait, we don't know the last event's size unless we read it! */
        /* Let's just seek to pos, read the event header, skip payload */
        fseek(tr->fp, pos, SEEK_SET);
        TraceEvent evt;
        if (fread(&evt, sizeof(evt), 1, tr->fp) != 1)
            return 0; /* nothing here? */
        pos += sizeof(TraceEvent) + evt.payload_len;
    }

    /* Keep reading appended events dynamically */
    int new_events = 0;
    while (1) {
        fseek(tr->fp, pos, SEEK_SET);
        TraceEvent evt;
        if (fread(&evt, sizeof(evt), 1, tr->fp) != 1)
            break;

        /* Expand index if full */
        if (tr->num_events >= tr->index_cap) {
            uint32_t new_cap = tr->index_cap ? tr->index_cap * 2 : 256;
            TraceIndexEntry *new_idx = realloc(tr->index,
                                               new_cap * sizeof(TraceIndexEntry));
            if (!new_idx) break; /* OOM */
            tr->index = new_idx;
            tr->index_cap = new_cap;
        }

        tr->index[tr->num_events].timestamp_ns = evt.timestamp_ns;
        tr->index[tr->num_events].file_offset  = (uint64_t)pos;
        tr->num_events++;
        new_events++;

        pos += sizeof(TraceEvent) + evt.payload_len;
    }
    return new_events;
}

int trace_reader_read_event_at(TraceReader *tr, uint32_t event_idx,
                               TraceEvent *evt, void *payload_buf,
                               uint32_t buf_size)
{
    if (event_idx >= tr->num_events)
        return -1;

    fseek(tr->fp, (long)tr->index[event_idx].file_offset, SEEK_SET);
    if (fread(evt, sizeof(TraceEvent), 1, tr->fp) != 1)
        return -1;

    if (evt->payload_len > 0 && payload_buf) {
        if (buf_size < evt->payload_len)
            return -1;
        if (fread(payload_buf, 1, evt->payload_len, tr->fp) != evt->payload_len)
            return -1;
    } else if (evt->payload_len > 0) {
        /* Skip payload if requested */
        fseek(tr->fp, evt->payload_len, SEEK_CUR);
    }
    return 0;
}

void trace_reader_close(TraceReader *tr)
{
    if (tr->fp)     fclose(tr->fp);
    if (tr->stages) free(tr->stages);
    if (tr->index)  free(tr->index);
    memset(tr, 0, sizeof(*tr));
}
