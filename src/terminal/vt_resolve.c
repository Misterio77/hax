/* SPDX-License-Identifier: MIT */
#include "terminal/vt_resolve.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "terminal/escape.h"
#include "terminal/sgr.h"
#include "text/utf8.h"

enum segment_kind {
    SEGMENT_GLYPHS,
    SEGMENT_CONTROL,
};

struct row_segment {
    enum segment_kind kind;
    char *bytes;
    size_t byte_len;
    size_t cell_width;
};

struct terminal_row {
    struct row_segment *segments;
    size_t count;
    size_t capacity;
    size_t cursor_col;
};

enum escape_kind {
    ESCAPE_OTHER,
    ESCAPE_CSI,
};

struct escape_sequence {
    enum escape_kind kind;
    size_t byte_len;
    char final;
    int first_param;
    bool has_first_param;
};

/* Cursor-forward input may come from untrusted model text and materialize as padding. */
#define VT_MAX_CURSOR_COL 4096
#define VT_MAX_CSI_PARAM  1000000

static void row_reset(struct terminal_row *row)
{
    for (size_t i = 0; i < row->count; i++)
        free(row->segments[i].bytes);
    row->count = 0;
    row->cursor_col = 0;
}

static void row_free(struct terminal_row *row)
{
    row_reset(row);
    free(row->segments);
    row->segments = NULL;
    row->capacity = 0;
}

static void row_insert(struct terminal_row *row, size_t index, enum segment_kind kind,
                       const char *bytes, size_t byte_len, size_t cell_width)
{
    if (row->count == row->capacity) {
        row->capacity = row->capacity ? row->capacity * 2 : 16;
        row->segments = xrealloc(row->segments, row->capacity * sizeof(*row->segments));
    }
    if (index < row->count) {
        memmove(&row->segments[index + 1], &row->segments[index],
                (row->count - index) * sizeof(*row->segments));
    }
    row->segments[index].kind = kind;
    row->segments[index].bytes = xmalloc(byte_len);
    memcpy(row->segments[index].bytes, bytes, byte_len);
    row->segments[index].byte_len = byte_len;
    row->segments[index].cell_width = cell_width;
    row->count++;
}

static void segment_append(struct row_segment *segment, const char *bytes, size_t byte_len,
                           size_t cell_width)
{
    segment->bytes = xrealloc(segment->bytes, segment->byte_len + byte_len);
    memcpy(segment->bytes + segment->byte_len, bytes, byte_len);
    segment->byte_len += byte_len;
    segment->cell_width += cell_width;
}

static size_t row_width(const struct terminal_row *row)
{
    size_t width = 0;
    for (size_t i = 0; i < row->count; i++)
        width += row->segments[i].cell_width;
    return width;
}

/* Return the byte boundary at or before col, keeping combining marks with their base glyph. */
static size_t segment_offset_at_col(const struct row_segment *segment, size_t col,
                                    size_t *cols_before)
{
    size_t offset = 0;
    size_t current_col = 0;
    while (offset < segment->byte_len) {
        size_t consumed = 1;
        int glyph_width =
            utf8_codepoint_cells(segment->bytes, segment->byte_len, offset, &consumed);
        if (glyph_width < 0)
            glyph_width = 1;
        if (glyph_width > 0 && (current_col >= col || current_col + (size_t)glyph_width > col))
            break;
        current_col += (size_t)glyph_width;
        offset += consumed ? consumed : 1;
    }
    *cols_before = current_col;
    return offset;
}

/* Split at a cell boundary and return the segment to its right. Control sequences at the
 * boundary remain on the left so they still precede the glyph they style. */
static size_t row_split_at_col(struct terminal_row *row, size_t col)
{
    size_t current_col = 0;
    for (size_t i = 0; i < row->count; i++) {
        struct row_segment *segment = &row->segments[i];
        if (segment->cell_width == 0) {
            if (segment->kind == SEGMENT_GLYPHS && current_col == col)
                return i;
            continue;
        }
        if (current_col == col)
            return i;
        if (current_col + segment->cell_width > col) {
            size_t head_width = 0;
            size_t offset = segment_offset_at_col(segment, col - current_col, &head_width);
            if (offset == 0)
                return i;

            size_t tail_len = segment->byte_len - offset;
            size_t tail_width = segment->cell_width - head_width;
            char *tail = xmalloc(tail_len);
            memcpy(tail, segment->bytes + offset, tail_len);
            segment->byte_len = offset;
            segment->cell_width = head_width;
            row_insert(row, i + 1, SEGMENT_GLYPHS, tail, tail_len, tail_width);
            free(tail);
            return i + 1;
        }
        current_col += segment->cell_width;
    }
    return row->count;
}

static size_t row_zero_width_insert_index(struct terminal_row *row)
{
    if (row->cursor_col >= row_width(row))
        return row->count;

    size_t index = row_split_at_col(row, row->cursor_col);
    while (index < row->count && row->segments[index].cell_width == 0)
        index++;
    return index;
}

/* Remove visible segments in [start, end), preserving terminal state in that range. */
static size_t row_remove_glyphs(struct terminal_row *row, size_t start, size_t end)
{
    size_t kept_end = start;
    for (size_t i = start; i < end; i++) {
        if (row->segments[i].kind == SEGMENT_CONTROL)
            row->segments[kept_end++] = row->segments[i];
        else
            free(row->segments[i].bytes);
    }
    if (kept_end != end) {
        memmove(&row->segments[kept_end], &row->segments[end],
                (row->count - end) * sizeof(*row->segments));
        row->count -= end - kept_end;
    }
    return kept_end;
}

static void row_pad_to_cursor(struct terminal_row *row, size_t width)
{
    if (row->cursor_col <= width)
        return;

    size_t padding_len = row->cursor_col - width;
    char *padding = xmalloc(padding_len);
    memset(padding, ' ', padding_len);
    row_insert(row, row->count, SEGMENT_GLYPHS, padding, padding_len, padding_len);
    free(padding);
}

static void row_write_glyph(struct terminal_row *row, const char *bytes, size_t byte_len,
                            size_t cell_width)
{
    size_t width = row_width(row);
    if (row->cursor_col >= width) {
        row_pad_to_cursor(row, width);
        if (row->count > 0 && row->segments[row->count - 1].cell_width > 0)
            segment_append(&row->segments[row->count - 1], bytes, byte_len, cell_width);
        else
            row_insert(row, row->count, SEGMENT_GLYPHS, bytes, byte_len, cell_width);
        row->cursor_col += cell_width;
        return;
    }

    size_t start = row_split_at_col(row, row->cursor_col);
    size_t end = row_split_at_col(row, row->cursor_col + cell_width);
    row_remove_glyphs(row, start, end);
    row_insert(row, start, SEGMENT_GLYPHS, bytes, byte_len, cell_width);
    row->cursor_col += cell_width;
}

static void row_write_control(struct terminal_row *row, const char *bytes, size_t byte_len)
{
    row_insert(row, row_zero_width_insert_index(row), SEGMENT_CONTROL, bytes, byte_len, 0);
}

/* Combining marks belong to their base glyph; unlike terminal state, they must not survive its
 * erasure. A mark without an adjacent base remains in place as zero-width content. */
static void row_write_combining(struct terminal_row *row, const char *bytes, size_t byte_len)
{
    size_t width = row_width(row);
    if (row->cursor_col == width && row->count > 0 &&
        row->segments[row->count - 1].kind == SEGMENT_GLYPHS &&
        row->segments[row->count - 1].cell_width > 0) {
        segment_append(&row->segments[row->count - 1], bytes, byte_len, 0);
        return;
    }

    row_insert(row, row_zero_width_insert_index(row), SEGMENT_GLYPHS, bytes, byte_len, 0);
}

static void row_erase_line(struct terminal_row *row, int mode)
{
    size_t width = row_width(row);
    size_t from_col = (mode == 1 || mode == 2) ? 0 : row->cursor_col;
    size_t to_col = mode == 1 ? row->cursor_col + 1 : width;
    if (to_col > width)
        to_col = width;
    if (from_col >= to_col)
        return;

    size_t start = row_split_at_col(row, from_col);
    size_t end = row_split_at_col(row, to_col);
    size_t kept_end = row_remove_glyphs(row, start, end);
    if (mode != 1)
        return;

    /* Erase-to-cursor needs spaces only when visible content remains to its right. */
    for (size_t i = kept_end; i < row->count; i++) {
        if (row->segments[i].cell_width > 0) {
            size_t padding_len = to_col - from_col;
            char *padding = xmalloc(padding_len);
            memset(padding, ' ', padding_len);
            row_insert(row, kept_end, SEGMENT_GLYPHS, padding, padding_len, padding_len);
            free(padding);
            return;
        }
    }
}

static bool row_has_glyphs(const struct terminal_row *row)
{
    for (size_t i = 0; i < row->count; i++)
        if (row->segments[i].cell_width > 0)
            return true;
    return false;
}

/* Pagers reset SGR state at every line, so each committed row must be style-self-contained:
 * reopen the styling carried in from earlier rows and close styling left open at the row end.
 * carried advances to the state the next row starts in. */
static void row_commit(struct terminal_row *row, struct sgr_state *carried,
                       const struct sgr_state *current, FILE *out)
{
    bool restyle = row_has_glyphs(row);
    if (restyle && !sgr_is_default(carried))
        sgr_emit(carried, out);
    for (size_t i = 0; i < row->count; i++)
        fwrite(row->segments[i].bytes, 1, row->segments[i].byte_len, out);
    if (restyle && !sgr_is_default(current))
        fputs("\x1b[0m", out);
    fputc('\n', out);
    row_reset(row);
    *carried = *current;
}

/* Read the final byte and first numeric parameter of a complete CSI sequence. */
static void parse_csi(const char *bytes, size_t len, struct escape_sequence *escape)
{
    escape->kind = ESCAPE_CSI;
    escape->final = bytes[len - 1];
    bool reading_first = true;
    for (size_t index = 2; index + 1 < len; index++) {
        unsigned char byte = (unsigned char)bytes[index];
        if (byte >= '0' && byte <= '9') {
            if (reading_first) {
                int digit = byte - '0';
                if (escape->first_param > (VT_MAX_CSI_PARAM - digit) / 10)
                    escape->first_param = VT_MAX_CSI_PARAM;
                else
                    escape->first_param = escape->first_param * 10 + digit;
                escape->has_first_param = true;
            }
        } else if (byte == ';' || byte == ':') {
            reading_first = false;
        }
    }
}

/* Unterminated sequences consume the remaining input so their payload is not rendered twice; an
 * unterminated CSI is not modeled. */
static struct escape_sequence parse_escape(const char *bytes, size_t len)
{
    struct escape_sequence escape = {.byte_len = escape_sequence_len(bytes, len)};
    unsigned char final = (unsigned char)bytes[escape.byte_len - 1];
    if (escape.byte_len >= 3 && bytes[1] == '[' && final >= 0x40 && final <= 0x7e)
        parse_csi(bytes, escape.byte_len, &escape);
    return escape;
}

void vt_resolve(const char *bytes, size_t len, FILE *out)
{
    struct terminal_row row = {0};
    struct sgr_state style = {0};
    struct sgr_state carried = {0};
    size_t offset = 0;
    while (offset < len) {
        char byte = bytes[offset];
        if (byte == '\n') {
            row_commit(&row, &carried, &style, out);
            offset++;
            continue;
        }
        if (byte == '\r') {
            row.cursor_col = 0;
            offset++;
            continue;
        }
        if (byte == 0x1b) {
            struct escape_sequence escape = parse_escape(bytes + offset, len - offset);
            if (escape.kind == ESCAPE_CSI) {
                switch (escape.final) {
                case 'D': {
                    size_t distance = escape.has_first_param && escape.first_param > 0
                                          ? (size_t)escape.first_param
                                          : 1;
                    row.cursor_col = row.cursor_col > distance ? row.cursor_col - distance : 0;
                    break;
                }
                case 'C':
                    row.cursor_col += escape.has_first_param && escape.first_param > 0
                                          ? (size_t)escape.first_param
                                          : 1;
                    if (row.cursor_col > VT_MAX_CURSOR_COL)
                        row.cursor_col = VT_MAX_CURSOR_COL;
                    break;
                case 'K':
                    row_erase_line(&row, escape.has_first_param ? escape.first_param : 0);
                    break;
                case 'm':
                    sgr_apply(&style, bytes + offset, escape.byte_len);
                    row_write_control(&row, bytes + offset, escape.byte_len);
                    break;
                default:
                    row_write_control(&row, bytes + offset, escape.byte_len);
                    break;
                }
            } else {
                row_write_control(&row, bytes + offset, escape.byte_len);
            }
            offset += escape.byte_len;
            continue;
        }

        size_t consumed = 1;
        int glyph_width = utf8_codepoint_cells(bytes, len, offset, &consumed);
        if (consumed == 0)
            consumed = 1;
        if (glyph_width < 0)
            glyph_width = 1;
        if (glyph_width == 0)
            row_write_combining(&row, bytes + offset, consumed);
        else
            row_write_glyph(&row, bytes + offset, consumed, (size_t)glyph_width);
        offset += consumed;
    }

    if (row.count > 0)
        row_commit(&row, &carried, &style, out);
    row_free(&row);
}
