/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "terminal/sgr.h"

static void apply(struct sgr_state *sgr, const char *sequence)
{
    sgr_apply(sgr, sequence, strlen(sequence));
}

static char *emitted(const struct sgr_state *sgr)
{
    char *bytes = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&bytes, &len);
    sgr_emit(sgr, out);
    fclose(out);
    return bytes;
}

static void test_emit_reopens_the_applied_style(void)
{
    struct sgr_state sgr = {0};
    apply(&sgr, "\x1b[1;3m");
    apply(&sgr, "\x1b[38;5;244m");
    char *bytes = emitted(&sgr);
    EXPECT_STR_EQ(bytes, "\x1b[1;3;38;5;244m");
    free(bytes);
}

static void test_resets_return_to_the_default(void)
{
    struct sgr_state sgr = {0};
    apply(&sgr, "\x1b[2;3m");
    apply(&sgr, "\x1b[22m");
    EXPECT(!sgr.dim);
    EXPECT(sgr.italic);
    apply(&sgr, "\x1b[0m");
    EXPECT(sgr_is_default(&sgr));

    char *bytes = emitted(&sgr);
    EXPECT_STR_EQ(bytes, "");
    free(bytes);
}

int main(void)
{
    test_emit_reopens_the_applied_style();
    test_resets_return_to_the_default();
    T_REPORT();
}
