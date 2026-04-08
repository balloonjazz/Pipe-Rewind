#ifndef PIPEREWIND_DIFF_H
#define PIPEREWIND_DIFF_H

#include <stdint.h>

/*
 * Side-by-side diff between adjacent pipeline stages.
 *
 * Compares what stage N produced (DATA_OUT) vs what stage N+1
 * received (DATA_IN). For text pipelines, this shows how each
 * command transforms the data.
 *
 * The diff is line-based: each line is marked as SAME, ADDED,
 * or REMOVED relative to the left (output) side.
 */

typedef enum {
    DIFF_SAME    = 0,   /* line present in both */
    DIFF_REMOVED = 1,   /* line only in left (stage N out) */
    DIFF_ADDED   = 2,   /* line only in right (stage N+1 in) */
} DiffLineType;

typedef struct {
    DiffLineType  type;
    const char   *text;
    int           len;
} DiffLine;

typedef struct {
    DiffLine *lines;
    int       num_lines;
    int       capacity;
} DiffResult;

/*
 * Compute a line-based diff between two buffers.
 *
 * Uses a simple LCS (longest common subsequence) algorithm on
 * lines.  Suitable for typical pipeline data (text output from
 * grep, sort, awk, etc.).
 *
 * The caller must call diff_free() on the result when done.
 */
int diff_compute(const uint8_t *left, uint32_t left_len,
                 const uint8_t *right, uint32_t right_len,
                 DiffResult *result);

/*
 * Free resources allocated by diff_compute().
 */
void diff_free(DiffResult *result);

#endif /* PIPEREWIND_DIFF_H */
