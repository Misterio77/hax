/* SPDX-License-Identifier: MIT */
#include "render/tool_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "text/display_safe.h"
#include "text/width.h"

void tool_input_preview_init(struct tool_input_preview *preview)
{
    memset(preview, 0, sizeof(*preview));
}

void tool_input_preview_feed(struct tool_input_preview *preview, const char *delta, size_t len)
{
    if (!delta || len == 0)
        return;

    preview->bytes += len;
    /* An oversized fragment keeps only its own tail; earlier bytes are already out of reach. */
    size_t from = len > TOOL_INPUT_RING_CAPACITY ? len - TOOL_INPUT_RING_CAPACITY : 0;
    for (size_t i = from; i < len; i++) {
        preview->ring[preview->written] = delta[i];
        if (++preview->written == TOOL_INPUT_RING_CAPACITY) {
            preview->written = 0;
            preview->wrapped = 1;
        }
    }
}

int tool_input_preview_ready(const struct tool_input_preview *preview)
{
    return preview->bytes >= TOOL_INPUT_MIN_BYTES;
}

void tool_input_preview_size(const struct tool_input_preview *preview, char *out, size_t size)
{
    /* One decimal keeps the header narrow while the count grows, and a repaint never widens or
     * shrinks it by more than a digit. */
    if (preview->bytes < 1024)
        snprintf(out, size, "%zuB", preview->bytes);
    else if (preview->bytes < 1024 * 1024)
        snprintf(out, size, "%.1fkB", (double)preview->bytes / 1024.0);
    else
        snprintf(out, size, "%.1fMB", (double)preview->bytes / (1024.0 * 1024.0));
}

static size_t ring_copy(const struct tool_input_preview *preview, char *out)
{
    size_t len = preview->wrapped ? TOOL_INPUT_RING_CAPACITY : preview->written;
    size_t oldest = preview->wrapped ? preview->written : 0;
    for (size_t i = 0; i < len; i++)
        out[i] = preview->ring[(oldest + i) % TOOL_INPUT_RING_CAPACITY];
    out[len] = '\0';
    return len;
}

/* Whether text wraps into at most max_rows rows of budget cells. */
static int wraps_within(const char *text, size_t budget, int max_rows)
{
    size_t offset = 0;
    for (int row = 0; row < max_rows; row++) {
        if (!text[offset])
            return 1;
        size_t separator = 0;
        size_t len = wrap_row_bytes(text + offset, budget, &separator);
        if (len == 0)
            return 0;
        offset += len + separator;
    }
    return text[offset] == '\0';
}

int tool_input_preview_rows(const struct tool_input_preview *preview, size_t budget, char **rows,
                            int max_rows)
{
    if (budget == 0 || max_rows <= 0)
        return 0;

    char linearized[TOOL_INPUT_RING_CAPACITY + 1];
    if (ring_copy(preview, linearized) == 0)
        return 0;

    /* Flattening before wrapping keeps control bytes and newlines out of the row accounting. */
    char *text = flatten_for_display(linearized);
    char *start = text;
    /* Drop whole rows until the remainder fits, so re-wrapping can choose fresh boundaries
     * instead of inheriting one the dropped row ended on. */
    while (!wraps_within(start, budget, max_rows)) {
        size_t separator = 0;
        size_t row = wrap_row_bytes(start, budget, &separator);
        if (row == 0)
            break;
        start += row + separator;
    }

    int count = 0;
    size_t offset = 0;
    while (start[offset] && count < max_rows) {
        size_t separator = 0;
        size_t row = wrap_row_bytes(start + offset, budget, &separator);
        if (row == 0)
            break;
        char terminator = start[offset + row];
        start[offset + row] = '\0';
        rows[count++] = xstrdup(start + offset);
        start[offset + row] = terminator;
        offset += row + separator;
    }
    free(text);
    return count;
}
