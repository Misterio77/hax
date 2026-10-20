/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMMANDS_TREE_H
#define HAX_COMMANDS_TREE_H

#include <stddef.h>

struct agent_state;
struct session_loaded;
struct picker_item;

/* Owned picker rows in depth-first order; entries maps rows back to session_loaded.tree. */
struct tree_view {
    struct picker_item *items;
    size_t *entries;
    size_t count;
    size_t initial;
};

void tree_view_build(const struct session_loaded *loaded, int unicode, struct tree_view *view);
void tree_view_free(struct tree_view *view);
void tree_command(struct agent_state *state);

#endif /* HAX_COMMANDS_TREE_H */
