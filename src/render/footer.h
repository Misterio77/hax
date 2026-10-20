/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_FOOTER_H
#define HAX_RENDER_FOOTER_H

/* Thread-safe live area below the conversation, written directly to stdout for interactive
 * terminals only: either the spinner's status row or rows of live tool output, animated until
 * hidden. Operations taking a footer pointer are NULL-safe no-ops.
 *
 * Between show and hide, callers must not write to stdout except through an attached disp, which
 * makes way for the footer; otherwise the footer owns the cursor. */
struct footer;
struct disp;

#define FOOTER_ROWS_MAX 8

/* Create a footer whose status row starts with a copied label. NULL or an empty label selects
 * "working...". Returns NULL when stdout is not a terminal or the animation thread cannot start. */
struct footer *footer_new(const char *label);
void footer_free(struct footer *footer);

/* Let output written through disp pass the footer, which steps aside before each batch of writes
 * and repaints below the output at disp_flush(). disp is borrowed until footer_free(). */
void footer_attach(struct footer *footer, struct disp *disp);

/* Rows from where disp's output ends down to the status row: the status row starts the next
 * block, one blank row below the output's last row. */
int footer_status_gap(const struct disp *disp);

/* Show the status row where the next block of the attached output starts, or at column zero of the
 * current empty row when unattached. Hide erases it. A first appearance from hidden is deferred
 * briefly; any show or hide within the grace period cancels it. */
void footer_show(struct footer *footer);

/* Show the status row one blank row below the current cursor, then restore the cursor on hide.
 * cursor_col is the current zero-based column. The current line must not wrap. First appearances
 * are deferred as for footer_show(). */
void footer_park(struct footer *footer, int cursor_col);

/* One prepared row: fully styled bytes for a single physical row without a line ending, and its
 * painted cell width for reflow-aware repaints. Rows must be clipped to the terminal by the
 * caller; a wrapped row breaks the footer's row accounting. */
struct footer_row {
    const char *bytes;
    int cells;
};

/* Replace the status row with up to FOOTER_ROWS_MAX copied prepared rows, newest last; excess
 * oldest rows are dropped. Rows are painted verbatim, with the spinner's glyph overprinting the
 * last row's first cell. Shows them if the footer is hidden; content changes repaint on the next
 * animation frame, not synchronously. */
void footer_set_rows(struct footer *footer, const struct footer_row *rows, int count);

/* Erase the visible rows and restore the cursor position saved by footer_park(). */
void footer_hide(struct footer *footer);

/* Hide as footer_hide(), but begin DEC 2026 synchronized output atomically with the erase and
 * leave the bracket open for the caller's replacement writes. Pair with footer_swap_end();
 * brackets do not nest. Until then the caller may only write replacement output and optionally
 * re-show the footer as the final act: a show inside the bracket paints immediately, without the
 * first-appearance grace, so the indicator survives the swap without blinking. */
void footer_swap_begin(struct footer *footer);

/* End the swap's synchronized output. Emits nothing when no swap is open. */
void footer_swap_end(struct footer *footer);

/* Change the status label as spinner_label_set() and spinner_label_request(), repainting a
 * visible status row when its text changes. */
void footer_set_label(struct footer *footer, const char *key, const char *label);
void footer_request_label(struct footer *footer, const char *key, const char *label);

/* Set the status row's elapsed-time origin to a positive monotonic_ms() value, or disable the
 * counter with zero. The counter appears only for long waits and never on prepared rows. */
void footer_set_timer(struct footer *footer, long started_at_ms);

struct buf;
struct live_region;

/* Append the moves from the footer's top to where disp's output ends, gap_rows above it, on a
 * screen terminal_cols wide. Pure with respect to the terminal. */
void footer_build_return(struct buf *out, const struct disp *disp, int gap_rows, int terminal_cols);

/* Append one synchronized repaint of region with rows, overprinting each row before erasing its
 * stale tail so terminals without synchronized output never show a blank frame. The cursor rests
 * at the start of the last row, after a glyph overprinting its first cell when one is given. Pure
 * with respect to the terminal. */
void footer_build_frame(struct buf *frame, struct live_region *region,
                        const struct footer_row *rows, int row_count, const char *glyph,
                        int terminal_cols);

#endif /* HAX_RENDER_FOOTER_H */
