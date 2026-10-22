/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/tool_input.h"
#include "system/locale.h"

static void feed_str(struct tool_input_preview *preview, const char *text)
{
    tool_input_preview_feed(preview, text, strlen(text));
}

static void feed_fill(struct tool_input_preview *preview, char byte, size_t count)
{
    char chunk[256];
    memset(chunk, byte, sizeof(chunk));
    while (count > 0) {
        size_t step = count < sizeof(chunk) ? count : sizeof(chunk);
        tool_input_preview_feed(preview, chunk, step);
        count -= step;
    }
}

static void free_rows(char **rows, int count)
{
    for (int i = 0; i < count; i++)
        free(rows[i]);
}

static void test_ready_needs_the_size_threshold(void)
{
    struct tool_input_preview preview;
    tool_input_preview_init(&preview);
    EXPECT(!tool_input_preview_ready(&preview));

    feed_fill(&preview, 'a', TOOL_INPUT_MIN_BYTES - 1);
    EXPECT(!tool_input_preview_ready(&preview));

    feed_fill(&preview, 'a', 1);
    EXPECT(tool_input_preview_ready(&preview));
}

static void test_size_is_compact_and_monotonic(void)
{
    struct tool_input_preview preview;
    char out[32];

    tool_input_preview_init(&preview);
    tool_input_preview_size(&preview, out, sizeof(out));
    EXPECT_STR_EQ(out, "0B");

    feed_fill(&preview, 'a', 999);
    tool_input_preview_size(&preview, out, sizeof(out));
    EXPECT_STR_EQ(out, "999B");

    feed_fill(&preview, 'a', 25);
    tool_input_preview_size(&preview, out, sizeof(out));
    EXPECT_STR_EQ(out, "1.0kB");

    feed_fill(&preview, 'a', 1024);
    tool_input_preview_size(&preview, out, sizeof(out));
    EXPECT_STR_EQ(out, "2.0kB");

    feed_fill(&preview, 'a', 1024 * 1024);
    tool_input_preview_size(&preview, out, sizeof(out));
    EXPECT_STR_EQ(out, "1.0MB");
}

static void test_empty_preview_has_no_rows(void)
{
    struct tool_input_preview preview;
    char *rows[TOOL_INPUT_TAIL_ROWS];

    tool_input_preview_init(&preview);
    EXPECT(tool_input_preview_rows(&preview, 40, rows, TOOL_INPUT_TAIL_ROWS) == 0);
}

static void test_rows_wrap_at_the_budget(void)
{
    struct tool_input_preview preview;
    char *rows[3];

    tool_input_preview_init(&preview);
    feed_str(&preview, "0123456789AB");
    int count = tool_input_preview_rows(&preview, 4, rows, 3);
    EXPECT(count == 3);
    EXPECT_STR_EQ(rows[0], "0123");
    EXPECT_STR_EQ(rows[1], "4567");
    EXPECT_STR_EQ(rows[2], "89AB");
    free_rows(rows, count);
}

static void test_rows_drop_oldest_to_fit(void)
{
    struct tool_input_preview preview;
    char *rows[2];

    tool_input_preview_init(&preview);
    feed_str(&preview, "0123456789AB");
    int count = tool_input_preview_rows(&preview, 4, rows, 2);
    EXPECT(count == 2);
    /* The newest text is the point of the preview, so the oldest row goes. */
    EXPECT_STR_EQ(rows[0], "4567");
    EXPECT_STR_EQ(rows[1], "89AB");
    free_rows(rows, count);
}

static void test_rows_bound_by_max_rows(void)
{
    struct tool_input_preview preview;
    char *rows[TOOL_INPUT_TAIL_ROWS];
    char text[200];

    tool_input_preview_init(&preview);
    memset(text, 'z', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    feed_str(&preview, text);

    int count = tool_input_preview_rows(&preview, 10, rows, TOOL_INPUT_TAIL_ROWS);
    EXPECT(count == TOOL_INPUT_TAIL_ROWS);
    /* The last row ends at the newest byte. */
    size_t last_len = strlen(rows[count - 1]);
    EXPECT(last_len > 0 && last_len <= 10);
    free_rows(rows, count);
}

static void test_ring_keeps_the_newest_bytes(void)
{
    struct tool_input_preview preview;
    char *rows[1];

    tool_input_preview_init(&preview);
    feed_fill(&preview, 'a', TOOL_INPUT_RING_CAPACITY - 500);
    feed_fill(&preview, 'b', 500 + 96);
    EXPECT(preview.bytes == TOOL_INPUT_RING_CAPACITY + 96);

    int count = tool_input_preview_rows(&preview, TOOL_INPUT_RING_CAPACITY, rows, 1);
    EXPECT(count == 1);
    EXPECT(strlen(rows[0]) == TOOL_INPUT_RING_CAPACITY);
    EXPECT(rows[0][0] == 'a');
    EXPECT(rows[0][strlen(rows[0]) - 1] == 'b');
    free_rows(rows, count);
}

static void test_control_bytes_never_reach_a_row(void)
{
    struct tool_input_preview preview;
    char *rows[TOOL_INPUT_TAIL_ROWS];

    tool_input_preview_init(&preview);
    feed_str(&preview, "before\x1b[31mred\nafter");
    int count = tool_input_preview_rows(&preview, 60, rows, TOOL_INPUT_TAIL_ROWS);
    EXPECT(count == 1);
    EXPECT(strchr(rows[0], '\x1b') == NULL);
    EXPECT(strchr(rows[0], '\n') == NULL);
    free_rows(rows, count);
}

static void test_rows_never_split_a_codepoint(void)
{
    struct tool_input_preview preview;
    char *rows[TOOL_INPUT_TAIL_ROWS];

    locale_init_utf8();
    tool_input_preview_init(&preview);
    /* Two-byte codepoints against a one-cell budget would land mid-codepoint if wrapping were
     * byte-based. */
    feed_str(&preview, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
    int count = tool_input_preview_rows(&preview, 2, rows, TOOL_INPUT_TAIL_ROWS);
    EXPECT(count > 0);
    for (int i = 0; i < count; i++)
        EXPECT(((unsigned char)rows[i][0] & 0xC0) != 0x80);
    free_rows(rows, count);
}

static void test_zero_budget_yields_no_rows(void)
{
    struct tool_input_preview preview;
    char *rows[TOOL_INPUT_TAIL_ROWS];

    tool_input_preview_init(&preview);
    feed_str(&preview, "0123456789");
    EXPECT(tool_input_preview_rows(&preview, 0, rows, TOOL_INPUT_TAIL_ROWS) == 0);
    EXPECT(tool_input_preview_rows(&preview, 10, rows, 0) == 0);
}

int main(void)
{
    test_ready_needs_the_size_threshold();
    test_size_is_compact_and_monotonic();
    test_empty_preview_has_no_rows();
    test_rows_wrap_at_the_budget();
    test_rows_drop_oldest_to_fit();
    test_rows_bound_by_max_rows();
    test_ring_keeps_the_newest_bytes();
    test_control_bytes_never_reach_a_row();
    test_rows_never_split_a_codepoint();
    test_zero_budget_yields_no_rows();
    T_REPORT();
}
