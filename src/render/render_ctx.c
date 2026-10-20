/* SPDX-License-Identifier: MIT */
#include "render/render_ctx.h"

#include <stdio.h>

#include "render/disp.h"
#include "render/footer.h"
#include "render/markdown.h"
#include "system/clock.h"
#include "terminal/ansi.h"

static void close_mode(struct render_ctx *render)
{
    switch (render->mode) {
    case RENDER_IDLE:
        break;
    case RENDER_WAITING:
        break;
    case RENDER_REASONING:
        if (render->md)
            md_flush(render->md);
        disp_write_ansi(&render->disp, ANSI_RESET);
        disp_putc(&render->disp, '\n');
        break;
    case RENDER_TEXT:
        if (render->md)
            md_flush(render->md);
        break;
    case RENDER_TOOL_CLUSTER:
        /* Hide first because a parked spinner restores the cluster cursor position. */
        footer_hide(render->footer);
        if (render->cluster.line_open)
            disp_putc(&render->disp, '\n');
        disp_flush(&render->disp);
        render->cluster.last_tool = NULL;
        render->cluster.line_open = 0;
        break;
    }
}

static void open_mode(struct render_ctx *render, enum render_mode mode)
{
    switch (mode) {
    case RENDER_IDLE:
        footer_hide(render->footer);
        break;
    case RENDER_WAITING:
        disp_block_separator(&render->disp);
        footer_set_label(render->footer, "working", "working...");
        footer_show(render->footer);
        break;
    case RENDER_REASONING:
        disp_block_separator(&render->disp);
        /* Output on screen settles the state, so the label follows it without hysteresis. */
        footer_set_label(render->footer, "thinking", "thinking...");
        footer_show(render->footer);
        disp_write_ansi(&render->disp, ANSI_DIM ANSI_ITALIC);
        /* Markdown styling would override the surrounding reasoning style. */
        if (render->md)
            md_set_styled(render->md, 0);
        break;
    case RENDER_TEXT:
        disp_block_separator(&render->disp);
        footer_set_label(render->footer, "working", "working...");
        footer_show(render->footer);
        render->stream.answer_started = 1;
        if (render->md)
            md_set_styled(render->md, 1);
        break;
    case RENDER_TOOL_CLUSTER:
        footer_hide(render->footer);
        disp_block_separator(&render->disp);
        /* The parked row must not inherit a label from the previous mode. */
        footer_set_label(render->footer, "working", "working...");
        break;
    }
}

void render_set_mode(struct render_ctx *render, enum render_mode target)
{
    if (render->mode == target)
        return;

    close_mode(render);
    open_mode(render, target);
    render->mode = target;
    disp_flush(&render->disp);
}

static void render_wait_for_stream(struct render_ctx *render)
{
    render->stream.in_table = 0;
    render_set_mode(render, RENDER_WAITING);
    /* Deferred labels prevent rapid item boundaries from flickering. */
    footer_request_label(render->footer, "working", "working...");
    footer_show(render->footer);
}

void render_stream_boundary(struct render_ctx *render)
{
    if (render->mode == RENDER_TOOL_CLUSTER) {
        footer_request_label(render->footer, "working", "working...");
        return;
    }
    render_wait_for_stream(render);
}

void render_stream_begin(struct render_ctx *render)
{
    render->stream.in_table = 0;
    render->retry.deadline_ms = 0;
    render->retry.next_attempt = 0;
    render->retry.max_attempts = 0;
    render->stream.content_seen = 0;
    render->stream.answer_started = 0;
    render->stream.output_rendered = 0;

    if (render->mode != RENDER_TOOL_CLUSTER)
        render_wait_for_stream(render);
    if (render->md)
        md_reset(render->md, md_cols());
}

void render_stream_retry(struct render_ctx *render)
{
    if (render->stream.output_rendered) {
        render_open_block(render);
        disp_write_ansi(&render->disp, ANSI_DIM);
        disp_printf(&render->disp, "[unexpected end]");
        disp_write_ansi(&render->disp, ANSI_RESET);
        disp_putc(&render->disp, '\n');
        render_set_mode(render, RENDER_WAITING);
    }
    render->stream.in_table = 0;
    render->stream.content_seen = 0;
    render->stream.answer_started = 0;
    render->stream.output_rendered = 0;
    if (render->md)
        md_reset(render->md, md_cols());
}

void render_open_block(struct render_ctx *render)
{
    render_set_mode(render, RENDER_IDLE);
    /* Tool-status rows are footer-owned but are not represented by render_mode. */
    footer_hide(render->footer);
    disp_block_separator(&render->disp);
}

void render_write_text(struct render_ctx *render, const char *bytes, size_t len)
{
    render->stream.output_rendered = 1;
    if (render->md)
        md_feed(render->md, bytes, len);
    else
        disp_write(&render->disp, bytes, len);
    disp_flush(&render->disp);

    /* A table shows only once complete; the label explains the wait, and its settle time keeps a
     * quick table from flashing it. */
    if (render->md && md_in_table(render->md)) {
        footer_request_label(render->footer, "composing", "composing...");
        render->stream.in_table = 1;
    } else if (render->stream.in_table) {
        footer_set_label(render->footer, "working", "working...");
        render->stream.in_table = 0;
    }
}

void render_text_delta(struct render_ctx *render, const char *bytes, size_t len)
{
    if (!render->stream.answer_started) {
        while (len > 0 && (*bytes == '\n' || *bytes == '\r')) {
            bytes++;
            len--;
        }
    }
    if (len == 0)
        return;

    render_set_mode(render, RENDER_TEXT);
    render_write_text(render, bytes, len);
}

void render_retry_label(const struct render_ctx *render, long now_ms, char *out, size_t size)
{
    long remaining_ms = render->retry.deadline_ms - now_ms;
    /* The retry state persists until the retried attempt produces its first event, so an
     * expired deadline means the request is in flight: a stalled attempt would otherwise read
     * as a countdown stuck at its final second. */
    if (remaining_ms <= 0) {
        snprintf(out, size, "retrying (attempt %d/%d)...", render->retry.next_attempt,
                 render->retry.max_attempts);
        return;
    }

    /* Round up so the final second is displayed as 1s rather than 0s. */
    long remaining_seconds = (remaining_ms + 999) / 1000;
    snprintf(out, size, "retrying in %lds (attempt %d/%d)...", remaining_seconds,
             render->retry.next_attempt, render->retry.max_attempts);
}

void render_update_retry_label(struct render_ctx *render)
{
    char label[64];
    render_retry_label(render, monotonic_ms(), label, sizeof(label));
    footer_set_label(render->footer, "retry", label);
}
