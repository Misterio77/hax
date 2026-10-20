/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_DISP_H
#define HAX_RENDER_DISP_H

#include <stddef.h>
#include <stdio.h>

#include "terminal/escape.h"
#include "terminal/sgr.h"
#include "text/utf8.h"

#define DISP_WIDE_STARTS_MAX 128

/* Block-aware output for separating visual blocks with exactly one blank line.
 *
 * Content writes defer trailing line endings so disp_block_separator() can collapse them to the
 * two newlines between blocks. A later visible byte instead commits pending newlines verbatim.
 *
 * The sink is borrowed. Direct visible writes must resynchronize newline state before more content
 * passes through the display; disp_sync_external_line() records the common one-line case. */
struct disp {
    FILE *sink; /* borrowed; NULL selects stdout */
    size_t committed_newlines;
    size_t pending_newlines;
    /* Cells since the cursor's row began, excluding escapes and unfolded at the terminal width. A
     * direct visible write leaves it stale until the next line ending. */
    size_t column;
    size_t wide_starts[DISP_WIDE_STARTS_MAX]; /* columns of the row's wide characters */
    size_t wide_count;                        /* may exceed the array, which keeps the first */
    /* Partial codepoint and escape sequence carried across writes. */
    struct utf8_cell_stream cell_stream;
    enum escape_scan_state escape_state;
    char escape_bytes[64]; /* the escape being scanned; a longer one is not applied */
    size_t escape_len;
    struct sgr_state style;            /* the output's active style, for whatever paints over it */
    const struct disp_live_area *live; /* borrowed; NULL when nothing is painted below */
    int live_suspended;
};

/* A live area painted below the output that steps aside for it: suspend runs before the first
 * write after a flush and must leave the cursor where output ends; resume runs at the next
 * disp_flush(). */
struct disp_live_area {
    void (*suspend)(void *user);
    void (*resume)(void *user);
    void *user;
};

FILE *disp_sink(const struct disp *disp);
void disp_flush(struct disp *disp);

/* The cursor's column on a screen terminal_cols wide, where a wide character that would straddle
 * the edge wraps early; a row ending at the edge returns terminal_cols, its wrap pending. Wide
 * characters past DISP_WIDE_STARTS_MAX count as narrow, and an unknown width returns column. */
size_t disp_screen_column(const struct disp *disp, int terminal_cols);

/* Synchronize after output written outside disp ends with one newline. */
void disp_sync_external_line(struct disp *disp);

/* Commit all pending newlines without collapsing them. */
void disp_commit_newlines(struct disp *disp);

/* Newlines remain pending until visible content, disp_commit_newlines(), or a block separator. */
void disp_putc(struct disp *disp, char byte);

/* Trailing LF and CRLF endings become pending; bare carriage returns preserve newline state. */
void disp_write(struct disp *disp, const char *bytes, size_t len);

/* Write zero-width terminal control bytes without changing newline state. */
void disp_write_ansi(struct disp *disp, const char *bytes);

/* disp_write_ansi() for len bytes. Both track the SGR style they set. */
void disp_write_raw(struct disp *disp, const char *bytes, size_t len);

__attribute__((format(printf, 2, 3), nonnull(2))) void disp_printf(struct disp *disp,
                                                                   const char *format, ...);

/* End the previous block with exactly one blank line, collapsing pending line endings. */
void disp_block_separator(struct disp *disp);

#endif /* HAX_RENDER_DISP_H */
