#include "diff.h"

#include <stdlib.h>
#include <string.h>

/* ---- Line splitting ---- */

typedef struct {
    const char **lines;
    int         *lens;
    int          count;
} LineArray;

static int split_lines(const uint8_t *data, uint32_t len, LineArray *la)
{
    la->count = 0;
    la->lines = NULL;
    la->lens  = NULL;

    if (len == 0)
        return 0;

    /* Count lines first */
    int n = 1;
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            n++;
    }

    la->lines = calloc((size_t)n, sizeof(char *));
    la->lens  = calloc((size_t)n, sizeof(int));
    if (!la->lines || !la->lens) {
        free(la->lines);
        free(la->lens);
        return -1;
    }

    const char *start = (const char *)data;
    int idx = 0;

    for (uint32_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            la->lines[idx] = start;
            la->lens[idx]  = (int)((const char *)&data[i] - start);
            idx++;
            start = (const char *)&data[i + 1];
        }
    }

    /* Last segment (if no trailing newline) */
    if (start < (const char *)&data[len]) {
        la->lines[idx] = start;
        la->lens[idx]  = (int)((const char *)&data[len] - start);
        idx++;
    }

    la->count = idx;
    return 0;
}

static void free_lines(LineArray *la)
{
    free(la->lines);
    free(la->lens);
    la->lines = NULL;
    la->lens  = NULL;
    la->count = 0;
}

/* ---- LCS-based diff ---- */

static int lines_equal(const LineArray *a, int ai, const LineArray *b, int bi)
{
    if (a->lens[ai] != b->lens[bi])
        return 0;
    return memcmp(a->lines[ai], b->lines[bi], (size_t)a->lens[ai]) == 0;
}

static int diff_add_line(DiffResult *r, DiffLineType type,
                         const char *text, int len)
{
    if (r->num_lines >= r->capacity) {
        int new_cap = r->capacity == 0 ? 256 : r->capacity * 2;
        DiffLine *nl = realloc(r->lines, (size_t)new_cap * sizeof(DiffLine));
        if (!nl) return -1;
        r->lines    = nl;
        r->capacity = new_cap;
    }
    r->lines[r->num_lines].type = type;
    r->lines[r->num_lines].text = text;
    r->lines[r->num_lines].len  = len;
    r->num_lines++;
    return 0;
}

int diff_compute(const uint8_t *left, uint32_t left_len,
                 const uint8_t *right, uint32_t right_len,
                 DiffResult *result)
{
    memset(result, 0, sizeof(*result));

    LineArray la, ra;
    if (split_lines(left, left_len, &la) < 0)
        return -1;
    if (split_lines(right, right_len, &ra) < 0) {
        free_lines(&la);
        return -1;
    }

    int m = la.count;
    int n = ra.count;

    /*
     * LCS dynamic programming table.
     * dp[i][j] = length of LCS of la[0..i-1] and ra[0..j-1]
     *
     * For very large inputs, cap the table size to avoid OOM.
     * Fall back to simple sequential comparison if too large.
     */
    int use_lcs = (m <= 2000 && n <= 2000);

    if (use_lcs && m > 0 && n > 0) {
        /* Allocate 2D table as flat array */
        int *dp = calloc((size_t)(m + 1) * (size_t)(n + 1), sizeof(int));
        if (!dp) {
            use_lcs = 0;
        } else {
            /* Fill LCS table */
            for (int i = 1; i <= m; i++) {
                for (int j = 1; j <= n; j++) {
                    if (lines_equal(&la, i - 1, &ra, j - 1))
                        dp[i * (n + 1) + j] = dp[(i - 1) * (n + 1) + (j - 1)] + 1;
                    else if (dp[(i - 1) * (n + 1) + j] >= dp[i * (n + 1) + (j - 1)])
                        dp[i * (n + 1) + j] = dp[(i - 1) * (n + 1) + j];
                    else
                        dp[i * (n + 1) + j] = dp[i * (n + 1) + (j - 1)];
                }
            }

            /* Backtrack to produce diff */
            /* Collect in reverse, then reverse at the end */
            int i = m, j = n;
            while (i > 0 || j > 0) {
                if (i > 0 && j > 0 && lines_equal(&la, i - 1, &ra, j - 1)) {
                    diff_add_line(result, DIFF_SAME, la.lines[i - 1], la.lens[i - 1]);
                    i--;
                    j--;
                } else if (j > 0 && (i == 0 || dp[i * (n + 1) + (j - 1)] >= dp[(i - 1) * (n + 1) + j])) {
                    diff_add_line(result, DIFF_ADDED, ra.lines[j - 1], ra.lens[j - 1]);
                    j--;
                } else {
                    diff_add_line(result, DIFF_REMOVED, la.lines[i - 1], la.lens[i - 1]);
                    i--;
                }
            }

            /* Reverse the output (we built it backwards) */
            for (int a2 = 0, b2 = result->num_lines - 1; a2 < b2; a2++, b2--) {
                DiffLine tmp = result->lines[a2];
                result->lines[a2] = result->lines[b2];
                result->lines[b2] = tmp;
            }

            free(dp);
        }
    }

    if (!use_lcs) {
        /* Fallback: sequential comparison, line by line */
        int common = m < n ? m : n;
        for (int i = 0; i < common; i++) {
            if (lines_equal(&la, i, &ra, i))
                diff_add_line(result, DIFF_SAME, la.lines[i], la.lens[i]);
            else {
                diff_add_line(result, DIFF_REMOVED, la.lines[i], la.lens[i]);
                diff_add_line(result, DIFF_ADDED, ra.lines[i], ra.lens[i]);
            }
        }
        for (int i = common; i < m; i++)
            diff_add_line(result, DIFF_REMOVED, la.lines[i], la.lens[i]);
        for (int i = common; i < n; i++)
            diff_add_line(result, DIFF_ADDED, ra.lines[i], ra.lens[i]);
    }

    free_lines(&la);
    free_lines(&ra);
    return 0;
}

void diff_free(DiffResult *result)
{
    free(result->lines);
    memset(result, 0, sizeof(*result));
}
