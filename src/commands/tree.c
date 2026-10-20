/* SPDX-License-Identifier: MIT */
#include "commands/tree.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "buf.h"
#include "session.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/render_ctx.h"
#include "system/locale.h"
#include "terminal/picker.h"
#include "terminal/ui.h"
#include "text/display_safe.h"
#include "text/width.h"
#include "tools/task_registry.h"

struct tree_row {
    size_t entry;
    size_t depth;
};

static char *row_label(const char *prompt, size_t depth, const unsigned char *more, int unicode)
{
    struct buf label;
    buf_init(&label);
    size_t first_level = depth > 24 ? depth - 23 : 1;
    if (first_level > 1)
        buf_append_str(&label, "... ");
    for (size_t level = first_level; level < depth; level++)
        buf_append_str(&label, more[level] ? (unicode ? "│  " : "|  ") : "   ");
    if (depth > 0)
        buf_append_str(&label, more[depth] ? (unicode ? "├─ " : "|- ") : (unicode ? "└─ " : "`- "));
    char *flat = flatten_for_display(prompt);
    char *preview = truncate_for_display(flat, 512);
    buf_append_str(&label, preview && *preview ? preview : "(empty prompt)");
    free(preview);
    free(flat);
    return buf_steal(&label);
}

void tree_view_build(const struct session_loaded *loaded, int unicode, struct tree_view *view)
{
    *view = (struct tree_view){.count = loaded->n_tree};
    size_t count = view->count;
    size_t *first = xmalloc((count + 1) * sizeof(*first));
    size_t *next = xmalloc(count * sizeof(*next));
    unsigned char *more = xcalloc(count + 1, sizeof(*more));
    for (size_t i = 0; i <= count; i++)
        first[i] = SIZE_MAX;
    size_t roots = 0;
    for (size_t i = count; i-- > 0;) {
        size_t parent = loaded->tree[i].parent;
        if (parent == SIZE_MAX) {
            parent = count;
            roots++;
        }
        next[i] = first[parent];
        first[parent] = i;
    }

    struct tree_row *stack = xmalloc(count * sizeof(*stack));
    view->items = xcalloc(count, sizeof(*view->items));
    view->entries = xmalloc(count * sizeof(*view->entries));
    size_t pending = 0;
    size_t written = 0;
    if (first[count] != SIZE_MAX)
        stack[pending++] = (struct tree_row){.entry = first[count], .depth = roots > 1};
    while (pending > 0) {
        struct tree_row row = stack[--pending];
        const struct session_tree_entry *entry = &loaded->tree[row.entry];
        more[row.depth] = next[row.entry] != SIZE_MAX;
        view->entries[written] = row.entry;
        view->items[written].label = row_label(entry->prompt, row.depth, more, unicode);
        view->items[written].current = entry->current;
        if (entry->current)
            view->initial = written;
        written++;
        if (next[row.entry] != SIZE_MAX)
            stack[pending++] = (struct tree_row){.entry = next[row.entry], .depth = row.depth};
        if (first[row.entry] != SIZE_MAX)
            stack[pending++] = (struct tree_row){.entry = first[row.entry], .depth = row.depth + 1};
    }
    free(stack);
    free(more);
    free(next);
    free(first);
}

void tree_view_free(struct tree_view *view)
{
    for (size_t i = 0; i < view->count; i++)
        free((char *)view->items[i].label);
    free(view->items);
    free(view->entries);
    *view = (struct tree_view){0};
}

void tree_command(struct agent_state *state)
{
    struct task_info *tasks;
    size_t task_count = task_list(&tasks);
    free(tasks);
    if (task_count > 0) {
        ui_error("collect or stop background tasks before switching branches");
        disp_sync_external_line(&state->render->disp);
        return;
    }
    if (!session_log_materialized(state->session_log)) {
        ui_note("/tree needs a recorded conversation");
        disp_sync_external_line(&state->render->disp);
        return;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        ui_error("/tree needs an interactive terminal");
        disp_sync_external_line(&state->render->disp);
        return;
    }

    struct agent_session *session = state->session;
    agent_flush_logs(state->transcript, state->session_log, session->items, session->n_items);
    struct session_loaded loaded;
    if (session_load_all(session_log_path(state->session_log), &loaded) != 0) {
        ui_error("could not read the session tree");
        disp_sync_external_line(&state->render->disp);
        return;
    }
    if (loaded.n_tree == 0) {
        ui_note("no prompts in this conversation yet");
        disp_sync_external_line(&state->render->disp);
        session_loaded_free(&loaded);
        return;
    }

    struct tree_view view;
    tree_view_build(&loaded, locale_have_utf8(), &view);
    int edit = 0;
    struct picker_opts options = {
        .title = "session tree · enter continue · e edit · / search · alt-e edit filtered",
        .items = view.items,
        .item_count = view.count,
        .initial_index = view.initial,
        .repeat_clipped_label = 1,
        .alternate_key = 'e',
        .alternate_selected = &edit,
    };
    long selected = picker_run(&options);
    if (selected >= 0) {
        const struct session_tree_entry *entry = &loaded.tree[view.entries[selected]];
        agent_navigate_tree(state, edit ? entry->position : entry->after_position, entry->prompt,
                            edit);
    }
    tree_view_free(&view);
    session_loaded_free(&loaded);
}
