/* SPDX-License-Identifier: MIT */
#include "render/footer.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/spinner.h"
#include "system/clock.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"
#include "terminal/sgr.h"
#include "terminal/theme.h"
#include "terminal/width.h"

/* An indicator that would be replaced this quickly reads as flicker, not feedback. */
#define STATUS_SHOW_GRACE_MS 300

enum footer_mode {
    FOOTER_HIDDEN = 0,
    FOOTER_STATUS,
    FOOTER_ROWS,
};

struct footer {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    enum footer_mode mode;
    int stop_requested;

    struct spinner_label label;
    long timer_started_at_ms;
    /* Rows from where output ends to the footer, and its column when unattached. */
    int parked_rows;
    int origin_col;
    /* A swap bracket opened by footer_swap_begin() and not yet closed. */
    int swap_open;
    /* A deferred status show waiting out its grace period on the animation thread. */
    int status_show_pending;
    long status_show_at_ms;
    int pending_parked_rows;
    int pending_origin_col;
    /* Prepared rows, newest last; the animated glyph overprints the last row. */
    char *row_bytes[FOOTER_ROWS_MAX];
    int row_cells[FOOTER_ROWS_MAX];
    int row_count;
    int rows_dirty;
    struct live_region region;

    struct disp *disp; /* borrowed; NULL when unattached */
    struct disp_live_area live;
    /* Stepped aside for the attached output; changes meanwhile are painted on resume. */
    int suspended;
    int suspended_synced;
};

void footer_build_frame(struct buf *frame, struct live_region *region,
                        const struct footer_row *rows, int row_count, const char *glyph,
                        int terminal_cols)
{
    buf_append_str(frame, ANSI_SYNC_BEGIN);
    /* Rows paint from the default style, whatever the output above left active. */
    buf_append_str(frame, ANSI_RESET);
    live_region_rewind(region, frame, terminal_cols);
    for (int row = 0; row < row_count; row++)
        live_region_add_row(region, frame, rows[row].bytes, strlen(rows[row].bytes),
                            rows[row].cells);
    /* The cursor is hidden while the footer shows, so it rests at the row's start, where no
     * terminal's rewrap can carry it off the row. */
    live_region_end_frame(region, frame, row_count - 1, 0);
    if (glyph) {
        /* The glyph overprints the last row's first cell, so it must follow the erase. */
        buf_append_str(frame, theme_open(THEME_CHROME_DIM));
        buf_append_str(frame, glyph);
        buf_append_str(frame, ANSI_RESET);
    }
    buf_append_str(frame, ANSI_SYNC_END);
}

static void write_frame_locked(struct buf *frame)
{
    fwrite(frame->data ? frame->data : "", 1, frame->len, stdout);
    fflush(stdout);
}

static void draw_status_locked(struct footer *footer, const char *glyph)
{
    long elapsed_ms =
        footer->timer_started_at_ms > 0 ? monotonic_ms() - footer->timer_started_at_ms : 0;
    struct buf status;
    buf_init(&status);
    /* Reserve the last terminal column because filling it can trigger deferred autowrap. */
    int cells =
        spinner_format_status(&status, glyph, footer->label.text, elapsed_ms, term_width() - 1);
    struct footer_row row = {.bytes = status.data ? status.data : "", .cells = cells};

    struct buf frame;
    buf_init(&frame);
    footer_build_frame(&frame, &footer->region, &row, 1, NULL, term_width());
    write_frame_locked(&frame);
    buf_free(&frame);
    buf_free(&status);
}

/* The cursor idles just after the glyph between frames. */
static void draw_glyph_only_locked(const char *glyph)
{
    fputs("\r", stdout);
    fputs(theme_open(THEME_CHROME_DIM), stdout);
    fputs(glyph, stdout);
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

static void draw_rows_locked(struct footer *footer, const char *glyph)
{
    if (!footer->rows_dirty && footer->region.row_count > 0) {
        draw_glyph_only_locked(glyph);
        return;
    }

    struct footer_row rows[FOOTER_ROWS_MAX];
    for (int row = 0; row < footer->row_count; row++) {
        rows[row].bytes = footer->row_bytes[row];
        rows[row].cells = footer->row_cells[row];
    }

    struct buf frame;
    buf_init(&frame);
    footer_build_frame(&frame, &footer->region, rows, footer->row_count, glyph, term_width());
    write_frame_locked(&frame);
    footer->rows_dirty = 0;
    buf_free(&frame);
}

static void draw_frame_locked(struct footer *footer)
{
    const char *glyph = spinner_glyph_now();

    switch (footer->mode) {
    case FOOTER_HIDDEN:
        break;
    case FOOTER_STATUS:
        draw_status_locked(footer, glyph);
        break;
    case FOOTER_ROWS:
        draw_rows_locked(footer, glyph);
        break;
    }
}

/* Erase under the current width so a resize between paint and erase still clears every row, then
 * return to the cursor footer_park() saved. */
void footer_build_return(struct buf *out, const struct disp *disp, int gap_rows, int terminal_cols)
{
    char move[32];
    if (gap_rows > 0) {
        snprintf(move, sizeof(move), ANSI_CSI "%dA", gap_rows);
        buf_append_str(out, move);
    }
    size_t col = disp_screen_column(disp, terminal_cols);
    if (col > 0) {
        /* The footer stays away while a wrap is pending, so clamping never loses one. */
        snprintf(move, sizeof(move), ANSI_CSI "%zuG",
                 terminal_cols > 0 && col >= (size_t)terminal_cols ? (size_t)terminal_cols
                                                                   : col + 1);
        buf_append_str(out, move);
    }
}

static void erase_locked(struct footer *footer)
{
    struct buf erase;
    buf_init(&erase);
    live_region_erase(&footer->region, &erase, term_width());
    if (footer->disp) {
        /* Worked out now: a resize since painting rewraps the output. */
        footer_build_return(&erase, footer->disp, footer->parked_rows, term_width());
    } else if (footer->parked_rows > 0) {
        char restore[32];
        snprintf(restore, sizeof(restore), ANSI_CSI "%dA", footer->parked_rows);
        buf_append_str(&erase, restore);
        if (footer->origin_col > 0) {
            snprintf(restore, sizeof(restore), ANSI_CSI "%dG", footer->origin_col + 1);
            buf_append_str(&erase, restore);
        }
    }
    write_frame_locked(&erase);
    buf_free(&erase);
    /* Frames end in a reset, so output written next would lose the styling it had open. */
    if (footer->disp) {
        sgr_emit(&footer->disp->style, stdout);
        fflush(stdout);
    }
}

static void show_locked(struct footer *footer, enum footer_mode mode, int parked_rows,
                        int origin_col)
{
    footer->status_show_pending = 0;
    spinner_label_settle(&footer->label, monotonic_ms());
    if (footer->suspended) {
        footer->mode = mode;
        return;
    }
    if (footer->mode == mode && footer->parked_rows == parked_rows &&
        footer->origin_col == origin_col)
        return;

    /* A transition erases one indicator before painting the next; synchronized output keeps the
     * swap from rendering as a blank-and-repaint. */
    enum footer_mode previous_mode = footer->mode;
    int synced = previous_mode != FOOTER_HIDDEN;
    if (synced) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        erase_locked(footer);
    }

    footer->mode = mode;
    footer->parked_rows = parked_rows;
    footer->origin_col = origin_col;
    for (int i = 0; i < parked_rows; i++)
        fputc('\n', stdout);
    draw_frame_locked(footer);
    if (synced) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
    }

    if (previous_mode == FOOTER_HIDDEN)
        pthread_cond_signal(&footer->wake);
}

/* A first appearance from hidden waits out a grace period on the animation thread, so a status
 * whose wait ends quickly never blinks. Transitions from a visible mode stay immediate, and so does
 * a show inside an open swap bracket: there the erase and this repaint land in one synchronized
 * frame, while deferring would end the bracket with the indicator erased. */
static void request_status_show_locked(struct footer *footer, int parked_rows, int origin_col)
{
    if (footer->mode != FOOTER_HIDDEN || footer->swap_open) {
        show_locked(footer, FOOTER_STATUS, parked_rows, origin_col);
        return;
    }
    footer->status_show_pending = 1;
    footer->status_show_at_ms = monotonic_ms() + STATUS_SHOW_GRACE_MS;
    footer->pending_parked_rows = parked_rows;
    footer->pending_origin_col = origin_col;
    pthread_cond_signal(&footer->wake);
}

/* An open batch of output would keep the footer erased. */
static void finish_output(struct footer *footer)
{
    if (footer->disp)
        disp_flush(footer->disp);
}

void footer_show(struct footer *footer)
{
    if (!footer)
        return;

    finish_output(footer);
    pthread_mutex_lock(&footer->mutex);
    if (footer->disp)
        request_status_show_locked(footer, footer_status_gap(footer->disp), 0);
    else
        request_status_show_locked(footer, 0, 0);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_park(struct footer *footer, int cursor_col)
{
    if (!footer)
        return;

    finish_output(footer);
    pthread_mutex_lock(&footer->mutex);
    /* At column zero the current empty row is the gap; an open line needs another row. */
    int parked_rows = cursor_col > 0 ? 2 : 1;
    request_status_show_locked(footer, parked_rows, cursor_col > 0 ? cursor_col : 0);
    pthread_mutex_unlock(&footer->mutex);
}

static void free_rows_locked(struct footer *footer)
{
    for (int row = 0; row < footer->row_count; row++) {
        free(footer->row_bytes[row]);
        footer->row_bytes[row] = NULL;
    }
    footer->row_count = 0;
}

static int rows_equal_locked(const struct footer *footer, const struct footer_row *rows, int count)
{
    if (footer->row_count != count)
        return 0;
    for (int row = 0; row < count; row++) {
        if (footer->row_cells[row] != rows[row].cells ||
            strcmp(footer->row_bytes[row], rows[row].bytes ? rows[row].bytes : "") != 0)
            return 0;
    }
    return 1;
}

void footer_set_rows(struct footer *footer, const struct footer_row *rows, int count)
{
    if (!footer || count < 1)
        return;

    if (count > FOOTER_ROWS_MAX) {
        rows += count - FOOTER_ROWS_MAX;
        count = FOOTER_ROWS_MAX;
    }

    finish_output(footer);
    pthread_mutex_lock(&footer->mutex);
    if (!rows_equal_locked(footer, rows, count)) {
        free_rows_locked(footer);
        for (int row = 0; row < count; row++) {
            footer->row_bytes[row] = xstrdup(rows[row].bytes ? rows[row].bytes : "");
            footer->row_cells[row] = rows[row].cells;
        }
        footer->row_count = count;
        footer->rows_dirty = 1;
    }
    show_locked(footer, FOOTER_ROWS, 0, 0);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_set_label(struct footer *footer, const char *key, const char *label)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    if (spinner_label_set(&footer->label, key, label) && footer->mode == FOOTER_STATUS &&
        !footer->suspended)
        draw_frame_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_request_label(struct footer *footer, const char *key, const char *label)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    if (spinner_label_request(&footer->label, key, label, monotonic_ms()) &&
        footer->mode == FOOTER_STATUS && !footer->suspended)
        draw_frame_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_set_timer(struct footer *footer, long started_at_ms)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    footer->timer_started_at_ms = started_at_ms > 0 ? started_at_ms : 0;
    pthread_mutex_unlock(&footer->mutex);
}

static void hide_locked(struct footer *footer)
{
    footer->status_show_pending = 0;
    if (footer->mode == FOOTER_HIDDEN)
        return;

    if (!footer->suspended)
        erase_locked(footer);
    footer->mode = FOOTER_HIDDEN;
    footer->parked_rows = 0;
    footer->origin_col = 0;
    free_rows_locked(footer);
}

void footer_hide(struct footer *footer)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    hide_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_swap_begin(struct footer *footer)
{
    if (!footer)
        return;

    /* The bracket must open under the lock: a frame drawn between opening and hiding would end
     * it before the caller's replacement writes. */
    pthread_mutex_lock(&footer->mutex);
    if (footer->mode != FOOTER_HIDDEN && !footer->suspended) {
        fputs(ANSI_SYNC_BEGIN, stdout);
        footer->swap_open = 1;
    }
    hide_locked(footer);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_swap_end(struct footer *footer)
{
    if (!footer)
        return;

    pthread_mutex_lock(&footer->mutex);
    if (footer->swap_open) {
        fputs(ANSI_SYNC_END, stdout);
        fflush(stdout);
        footer->swap_open = 0;
    }
    pthread_mutex_unlock(&footer->mutex);
}

/* pthread_cond_timedwait portably uses CLOCK_REALTIME; macOS cannot select CLOCK_MONOTONIC. */
static struct timespec deadline_after_ms(long wait_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wait_ms / 1000;
    deadline.tv_nsec += (wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    return deadline;
}

static void *footer_thread(void *arg)
{
    struct footer *footer = arg;

    /* Keep process signals on threads that own the foreground operation and terminal state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&footer->mutex);
    while (!footer->stop_requested) {
        if (footer->suspended) {
            pthread_cond_wait(&footer->wake, &footer->mutex);
            continue;
        }
        if (footer->mode == FOOTER_HIDDEN) {
            if (!footer->status_show_pending) {
                pthread_cond_wait(&footer->wake, &footer->mutex);
                continue;
            }
            long wait_ms = footer->status_show_at_ms - monotonic_ms();
            if (wait_ms <= 0) {
                show_locked(footer, FOOTER_STATUS, footer->pending_parked_rows,
                            footer->pending_origin_col);
                continue;
            }
            struct timespec show_deadline = deadline_after_ms(wait_ms);
            pthread_cond_timedwait(&footer->wake, &footer->mutex, &show_deadline);
            continue;
        }

        spinner_label_settle(&footer->label, monotonic_ms());
        draw_frame_locked(footer);
        struct timespec deadline = deadline_after_ms(SPINNER_FRAME_INTERVAL_MS);
        pthread_cond_timedwait(&footer->wake, &footer->mutex, &deadline);
    }
    pthread_mutex_unlock(&footer->mutex);
    return NULL;
}

int footer_status_gap(const struct disp *disp)
{
    size_t newlines = disp->column > 0 ? 0 : disp->committed_newlines;
    return newlines >= 2 ? 0 : 2 - (int)newlines;
}

/* Prepared rows continue the output, starting on the row after it. */
static int rows_gap(const struct disp *disp)
{
    return disp->column > 0 ? 1 : 0;
}

static void suspend_for_output(void *user)
{
    struct footer *footer = user;
    pthread_mutex_lock(&footer->mutex);
    if (footer->suspended) {
        pthread_mutex_unlock(&footer->mutex);
        return;
    }
    footer->suspended = 1;
    if (footer->mode != FOOTER_HIDDEN) {
        /* The erase and the repaint after the output land in one synchronized frame. */
        fputs(ANSI_SYNC_BEGIN, stdout);
        footer->suspended_synced = 1;
        erase_locked(footer);
    }
    pthread_mutex_unlock(&footer->mutex);
}

static void resume_after_output(void *user)
{
    struct footer *footer = user;
    pthread_mutex_lock(&footer->mutex);
    /* No cursor movement restores a pending wrap, so the footer stays away while one is pending. */
    int cols = term_width();
    if (cols > 0 && disp_screen_column(footer->disp, cols) == (size_t)cols) {
        if (footer->suspended_synced) {
            fputs(ANSI_SYNC_END, stdout);
            footer->suspended_synced = 0;
        }
        fflush(stdout);
        pthread_mutex_unlock(&footer->mutex);
        return;
    }
    footer->suspended = 0;
    int gap =
        footer->mode == FOOTER_ROWS ? rows_gap(footer->disp) : footer_status_gap(footer->disp);
    if (footer->status_show_pending) {
        footer->pending_parked_rows = gap;
        footer->pending_origin_col = 0;
    }
    if (footer->mode != FOOTER_HIDDEN) {
        footer->parked_rows = gap;
        footer->origin_col = 0;
        for (int i = 0; i < gap; i++)
            fputc('\n', stdout);
        draw_frame_locked(footer);
    }
    if (footer->suspended_synced) {
        fputs(ANSI_SYNC_END, stdout);
        footer->suspended_synced = 0;
    }
    fflush(stdout);
    pthread_cond_signal(&footer->wake);
    pthread_mutex_unlock(&footer->mutex);
}

void footer_attach(struct footer *footer, struct disp *disp)
{
    if (!footer)
        return;
    footer->disp = disp;
    footer->live = (struct disp_live_area){
        .suspend = suspend_for_output,
        .resume = resume_after_output,
        .user = footer,
    };
    disp->live = &footer->live;
}

struct footer *footer_new(const char *label)
{
    if (!isatty(fileno(stdout)))
        return NULL;

    struct footer *footer = xcalloc(1, sizeof(*footer));
    spinner_label_init(&footer->label, label);
    pthread_mutex_init(&footer->mutex, NULL);
    pthread_cond_init(&footer->wake, NULL);

    if (pthread_create(&footer->thread, NULL, footer_thread, footer) != 0) {
        pthread_mutex_destroy(&footer->mutex);
        pthread_cond_destroy(&footer->wake);
        spinner_label_free(&footer->label);
        free(footer);
        return NULL;
    }
    return footer;
}

void footer_free(struct footer *footer)
{
    if (!footer)
        return;

    if (footer->disp) {
        footer->disp->live = NULL;
        footer->disp->live_suspended = 0;
    }
    pthread_mutex_lock(&footer->mutex);
    hide_locked(footer);
    /* Never leave the terminal in synchronized mode, even on an abandoned swap or suspension. */
    if (footer->swap_open || footer->suspended_synced) {
        fputs(ANSI_SYNC_END, stdout);
        footer->swap_open = 0;
        footer->suspended_synced = 0;
    }
    footer->stop_requested = 1;
    pthread_cond_signal(&footer->wake);
    pthread_mutex_unlock(&footer->mutex);

    pthread_join(footer->thread, NULL);
    pthread_mutex_destroy(&footer->mutex);
    pthread_cond_destroy(&footer->wake);
    spinner_label_free(&footer->label);
    live_region_free(&footer->region);
    free(footer);
}
