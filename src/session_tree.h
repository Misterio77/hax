/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_TREE_H
#define HAX_SESSION_TREE_H

#include <stddef.h>

#include "provider.h"

struct session_loaded;
struct session_tree_node;

/* Replay-time ancestry for append-only session records. Zero-initialize before use. Record ids
 * are physical, one-based JSONL line numbers; zero denotes the empty conversation. */
struct session_tree {
    struct session_tree_node *nodes;
    size_t count;
    size_t capacity;
    size_t position;
};

/* Takes ownership of item and appends it at the selected position. */
void session_tree_append(struct session_tree *tree, size_t record, struct item item);
void session_tree_undo(struct session_tree *tree, size_t keep_user_turns);

/* Number of items at the end of the selected path that a navigation to `target` would leave behind:
 * everything below the last item it shares with the target path. Zero when the target is already on
 * the selected path. */
size_t session_tree_departing_count(const struct session_tree *tree, size_t target);

/* Select an existing item record, or zero for the root. Invalid targets leave tree unchanged. */
int session_tree_navigate(struct session_tree *tree, size_t position);

/* Transfer items into the live/retired arrays and populate turn picker entries.
 * The destination's arrays must be empty. Metadata and timing are left untouched. */
void session_tree_export(struct session_tree *tree, struct session_loaded *loaded);
void session_tree_free(struct session_tree *tree);

#endif /* HAX_SESSION_TREE_H */
