/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_TOOL_INPUT_H
#define HAX_RENDER_TOOL_INPUT_H

#include <stddef.h>

/* Rows the argument tail may take, which leaves the header row and a spare inside the spinner's
 * SPINNER_TOOL_VIEW_ROWS_MAX. */
#define TOOL_INPUT_TAIL_ROWS 6

/* A call whose arguments never grow this large keeps the plain composing label: the wait is not
 * worth a repainted view, and the preview would only show a fragment of something about to
 * render in full. */
#define TOOL_INPUT_MIN_BYTES 512

/* Arguments arrive as one long JSON line, so the preview keeps the newest bytes: that is the
 * text the tail rows can show. */
#define TOOL_INPUT_RING_CAPACITY 4096

/* Bounded tail of a streamed tool call's arguments. Terminal-independent: callers style and clip
 * the rows it produces. */
struct tool_input_preview {
    char ring[TOOL_INPUT_RING_CAPACITY];
    size_t written;
    int wrapped;
    size_t bytes; /* everything streamed, including bytes the ring dropped */
};

void tool_input_preview_init(struct tool_input_preview *preview);

/* Append one argument fragment; NULL or empty fragments are ignored. */
void tool_input_preview_feed(struct tool_input_preview *preview, const char *delta, size_t len);

/* Whether the preview is worth painting yet. */
int tool_input_preview_ready(const struct tool_input_preview *preview);

/* Format the total streamed size compactly, e.g. "512B" or "12.4kB". */
void tool_input_preview_size(const struct tool_input_preview *preview, char *out, size_t size);

/* Split the newest arguments into at most max_rows rows of at most budget cells each, oldest
 * first, so the last row holds the newest text. Each row is allocated and must be freed by the
 * caller; the return value is how many were filled. Rows hold display-safe text, not styling. */
int tool_input_preview_rows(const struct tool_input_preview *preview, size_t budget, char **rows,
                            int max_rows);

#endif /* HAX_RENDER_TOOL_INPUT_H */
