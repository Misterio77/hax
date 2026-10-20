/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_SGR_H
#define HAX_TERMINAL_SGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A model of the SGR attributes a terminal has active, built from the CSI m sequences written to
 * it, so styling can be reopened after something else reset it. Zero-initialized is the default
 * style. */
/* Active SGR attributes; fg/bg hold the raw parameter text that set them ("31", "38;5;173"). */
struct sgr_state {
    bool bold;
    bool dim;
    bool italic;
    bool underline;
    bool blink;
    bool inverse;
    bool hidden;
    bool strike;
    char fg[20];
    char bg[20];
};

bool sgr_is_default(const struct sgr_state *sgr);

/* Write the one sequence that turns the default style into sgr; nothing when sgr is the default. */
void sgr_emit(const struct sgr_state *sgr, FILE *out);

/* Apply one complete CSI sequence ending in 'm'. Codes the model does not know are ignored, and a
 * private-use prefix marks a sequence that is not SGR at all. */
void sgr_apply(struct sgr_state *sgr, const char *sequence, size_t sequence_len);

#endif /* HAX_TERMINAL_SGR_H */
