/* SPDX-License-Identifier: MIT */
#include "render/disp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "terminal/escape.h"
#include "terminal/sgr.h"
#include "text/utf8.h"

FILE *disp_sink(const struct disp *disp)
{
    return disp->sink ? disp->sink : stdout;
}

size_t disp_screen_column(const struct disp *disp, int terminal_cols)
{
    if (terminal_cols <= 0)
        return disp->column;
    size_t cols = (size_t)terminal_cols;
    /* Each wide character that would start in a row's last cell is pushed one cell on. */
    size_t padding = 0;
    size_t recorded =
        disp->wide_count < DISP_WIDE_STARTS_MAX ? disp->wide_count : DISP_WIDE_STARTS_MAX;
    for (size_t i = 0; i < recorded && cols > 1; i++) {
        if ((disp->wide_starts[i] + padding) % cols == cols - 1)
            padding++;
    }
    size_t end = disp->column + padding;
    if (end > 0 && end % cols == 0)
        return cols;
    return end % cols;
}

/* Output written while the live area is up would land on it, so it steps aside first. */
static void make_way(struct disp *disp)
{
    if (!disp->live || disp->live_suspended)
        return;
    disp->live_suspended = 1;
    disp->live->suspend(disp->live->user);
}

void disp_flush(struct disp *disp)
{
    if (disp->live_suspended) {
        disp->live_suspended = 0;
        disp->live->resume(disp->live->user);
    }
    fflush(disp_sink(disp));
}

/* A fresh row also drops a stray partial codepoint or unterminated escape, so one malformed write
 * cannot swallow the widths of the rows after it. */
static void start_row(struct disp *disp)
{
    disp->column = 0;
    disp->wide_count = 0;
    utf8_cell_stream_reset(&disp->cell_stream);
    disp->escape_state = ESCAPE_SCAN_IDLE;
}

void disp_sync_external_line(struct disp *disp)
{
    disp->committed_newlines = 1;
    disp->pending_newlines = 0;
    start_row(disp);
}

static void write_newlines(struct disp *disp, size_t count)
{
    if (count > 0)
        make_way(disp);
    for (size_t i = 0; i < count; i++)
        fputc('\n', disp_sink(disp));
    if (count > 0)
        start_row(disp);
}

/* An incomplete codepoint ends where another kind of byte begins; terminals draw what arrived as
 * replacement cells. */
static void flush_partial_codepoint(struct disp *disp)
{
    const char *unit;
    size_t unit_len;
    int cells;
    if (utf8_cell_stream_flush(&disp->cell_stream, &unit, &unit_len, &cells))
        disp->column += (size_t)cells;
}

static void scan_escape_byte(struct disp *disp, unsigned char byte)
{
    if (disp->escape_state == ESCAPE_SCAN_IDLE)
        disp->escape_len = 0;
    if (disp->escape_len < sizeof(disp->escape_bytes))
        disp->escape_bytes[disp->escape_len] = (char)byte;
    disp->escape_len++;

    disp->escape_state = escape_scan(disp->escape_state, byte);
    if (disp->escape_state != ESCAPE_SCAN_IDLE)
        return;
    size_t len = disp->escape_len;
    if (len >= 3 && len <= sizeof(disp->escape_bytes) && disp->escape_bytes[1] == '[' &&
        disp->escape_bytes[len - 1] == 'm')
        sgr_apply(&disp->style, disp->escape_bytes, len);
}

static void advance_column(struct disp *disp, const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (disp->escape_state != ESCAPE_SCAN_IDLE || byte == 0x1b) {
            flush_partial_codepoint(disp);
            scan_escape_byte(disp, byte);
        } else if (byte == '\r' || byte == '\n') {
            utf8_cell_stream_reset(&disp->cell_stream);
            disp->column = 0;
            disp->wide_count = 0;
        } else if (byte == '\t') {
            /* Renderers expand tabs themselves; one reaching disp is raw text (Markdown off),
             * which the terminal moves to its next default 8-column stop. */
            flush_partial_codepoint(disp);
            disp->column = (disp->column / 8 + 1) * 8;
        } else if (byte < 0x20 || byte == 0x7f) {
            flush_partial_codepoint(disp);
        } else {
            const char *unit;
            size_t unit_len;
            int cells;
            if (utf8_cell_stream_feed(&disp->cell_stream, byte, &unit, &unit_len, &cells)) {
                if (cells == 2) {
                    if (disp->wide_count < DISP_WIDE_STARTS_MAX)
                        disp->wide_starts[disp->wide_count] = disp->column;
                    disp->wide_count++;
                }
                disp->column += (size_t)cells;
            }
        }
    }
}

void disp_commit_newlines(struct disp *disp)
{
    write_newlines(disp, disp->pending_newlines);
    disp->committed_newlines += disp->pending_newlines;
    disp->pending_newlines = 0;
}

void disp_putc(struct disp *disp, char byte)
{
    if (byte == '\n') {
        disp->pending_newlines++;
        return;
    }

    make_way(disp);
    disp_commit_newlines(disp);
    fputc(byte, disp_sink(disp));
    advance_column(disp, &byte, 1);
    if (byte != '\r')
        disp->committed_newlines = 0;
}

void disp_write(struct disp *disp, const char *bytes, size_t len)
{
    size_t visible_len = len;
    size_t trailing_newlines = 0;
    while (visible_len > 0 && bytes[visible_len - 1] == '\n') {
        visible_len--;
        if (visible_len > 0 && bytes[visible_len - 1] == '\r')
            visible_len--;
        trailing_newlines++;
    }

    if (visible_len > 0) {
        make_way(disp);
        disp_commit_newlines(disp);
        fwrite(bytes, 1, visible_len, disp_sink(disp));
        advance_column(disp, bytes, visible_len);

        /* Carriage returns move within a row, so only other bytes end the trailing newline run. */
        size_t control_start = visible_len;
        size_t written_newlines = 0;
        while (control_start > 0) {
            char byte = bytes[control_start - 1];
            if (byte == '\n')
                written_newlines++;
            else if (byte != '\r')
                break;
            control_start--;
        }
        if (control_start > 0)
            disp->committed_newlines = written_newlines;
        else
            disp->committed_newlines += written_newlines;
    }
    disp->pending_newlines += trailing_newlines;
}

void disp_write_ansi(struct disp *disp, const char *bytes)
{
    disp_write_raw(disp, bytes, strlen(bytes));
}

void disp_write_raw(struct disp *disp, const char *bytes, size_t len)
{
    if (len == 0)
        return;
    make_way(disp);
    fwrite(bytes, 1, len, disp_sink(disp));
    advance_column(disp, bytes, len);
}

void disp_printf(struct disp *disp, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *bytes = xvasprintf(format, args);
    va_end(args);
    if (!bytes)
        return;
    disp_write(disp, bytes, strlen(bytes));
    free(bytes);
}

void disp_block_separator(struct disp *disp)
{
    size_t needed = disp->committed_newlines < 2 ? 2 - disp->committed_newlines : 0;
    write_newlines(disp, needed);
    disp->committed_newlines += needed;
    disp->pending_newlines = 0;
}
