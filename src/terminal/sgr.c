/* SPDX-License-Identifier: MIT */
#include "terminal/sgr.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

bool sgr_is_default(const struct sgr_state *sgr)
{
    return !sgr->bold && !sgr->dim && !sgr->italic && !sgr->underline && !sgr->blink &&
           !sgr->inverse && !sgr->hidden && !sgr->strike && !sgr->fg[0] && !sgr->bg[0];
}

void sgr_emit(const struct sgr_state *sgr, FILE *out)
{
    const char *codes[10];
    size_t count = 0;
    if (sgr->bold)
        codes[count++] = "1";
    if (sgr->dim)
        codes[count++] = "2";
    if (sgr->italic)
        codes[count++] = "3";
    if (sgr->underline)
        codes[count++] = "4";
    if (sgr->blink)
        codes[count++] = "5";
    if (sgr->inverse)
        codes[count++] = "7";
    if (sgr->hidden)
        codes[count++] = "8";
    if (sgr->strike)
        codes[count++] = "9";
    if (sgr->fg[0])
        codes[count++] = sgr->fg;
    if (sgr->bg[0])
        codes[count++] = sgr->bg;
    if (count == 0)
        return;

    fputs("\x1b[", out);
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            fputc(';', out);
        fputs(codes[i], out);
    }
    fputc('m', out);
}

static size_t sgr_token_end(const char *params, size_t params_len, size_t start)
{
    while (start < params_len && params[start] != ';')
        start++;
    return start;
}

/* An empty token means 0 (reset) per ECMA-48; a non-numeric lead returns -1 (not SGR). */
static int sgr_token_value(const char *token, size_t token_len)
{
    if (token_len > 0 && (token[0] < '0' || token[0] > '9'))
        return -1;
    int value = 0;
    for (size_t i = 0; i < token_len && token[i] >= '0' && token[i] <= '9'; i++) {
        value = value * 10 + (token[i] - '0');
        if (value > 9999)
            return 9999;
    }
    return value;
}

static void sgr_apply_code(struct sgr_state *sgr, int code)
{
    switch (code) {
    case 0:
        memset(sgr, 0, sizeof(*sgr));
        break;
    case 1:
        sgr->bold = true;
        break;
    case 2:
        sgr->dim = true;
        break;
    case 3:
        sgr->italic = true;
        break;
    case 4:
        sgr->underline = true;
        break;
    case 5:
    case 6:
        sgr->blink = true;
        break;
    case 7:
        sgr->inverse = true;
        break;
    case 8:
        sgr->hidden = true;
        break;
    case 9:
        sgr->strike = true;
        break;
    case 22:
        sgr->bold = false;
        sgr->dim = false;
        break;
    case 23:
        sgr->italic = false;
        break;
    case 24:
        sgr->underline = false;
        break;
    case 25:
        sgr->blink = false;
        break;
    case 27:
        sgr->inverse = false;
        break;
    case 28:
        sgr->hidden = false;
        break;
    case 29:
        sgr->strike = false;
        break;
    case 39:
        sgr->fg[0] = '\0';
        break;
    case 49:
        sgr->bg[0] = '\0';
        break;
    default:
        if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97))
            snprintf(sgr->fg, sizeof(sgr->fg), "%d", code);
        else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107))
            snprintf(sgr->bg, sizeof(sgr->bg), "%d", code);
        break;
    }
}

/* Extended colors keep their raw parameter span so re-emitting reproduces them verbatim; a color
 * too long for its slot falls back to default rather than reopening truncated. */
void sgr_apply(struct sgr_state *sgr, const char *sequence, size_t sequence_len)
{
    const char *params = sequence + 2;
    size_t params_len = sequence_len - 3;
    /* A private-use prefix marks a non-SGR use of final 'm', such as xterm modifyOtherKeys. */
    if (params_len > 0 &&
        (params[0] == '<' || params[0] == '=' || params[0] == '>' || params[0] == '?'))
        return;
    size_t start = 0;
    while (start <= params_len) {
        size_t end = sgr_token_end(params, params_len, start);
        int code = sgr_token_value(params + start, end - start);
        const char *colon = memchr(params + start, ':', end - start);
        if (code == 38 || code == 48) {
            /* Semicolon payloads span tokens: 38;5;n or 38;2;r;g;b. Colon forms are one token. */
            size_t span_end = end;
            if (!colon) {
                size_t kind_start = end + 1;
                size_t kind_end = sgr_token_end(params, params_len, kind_start);
                int kind = kind_start <= params_len
                               ? sgr_token_value(params + kind_start, kind_end - kind_start)
                               : 0;
                int payload_tokens = kind == 5 ? 2 : kind == 2 ? 4 : 0;
                for (int i = 0; i < payload_tokens && span_end < params_len; i++)
                    span_end = sgr_token_end(params, params_len, span_end + 1);
            }
            char *slot = code == 38 ? sgr->fg : sgr->bg;
            size_t span_len = span_end - start;
            if (span_len < sizeof(sgr->fg)) {
                memcpy(slot, params + start, span_len);
                slot[span_len] = '\0';
            } else {
                slot[0] = '\0';
            }
            start = span_end + 1;
            continue;
        }
        if (code == 4 && colon)
            sgr->underline = colon[1] != '0';
        else if (code >= 0)
            sgr_apply_code(sgr, code);
        start = end + 1;
    }
}
