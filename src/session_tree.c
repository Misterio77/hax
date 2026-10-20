/* SPDX-License-Identifier: MIT */
#include "session_tree.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "session.h"
#include "xalloc.h"

struct session_tree_node {
    struct item item;
    size_t record;
    size_t parent; /* node index, or SIZE_MAX for the root */
};

static size_t find_position(const struct session_tree *tree, size_t position)
{
    size_t low = 0;
    size_t high = tree->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (tree->nodes[middle].record < position)
            low = middle + 1;
        else
            high = middle;
    }
    return low < tree->count && tree->nodes[low].record == position ? low : SIZE_MAX;
}

void session_tree_append(struct session_tree *tree, size_t record, struct item item)
{
    if (tree->count == tree->capacity) {
        tree->capacity = tree->capacity ? tree->capacity * 2 : 16;
        tree->nodes = xrealloc(tree->nodes, tree->capacity * sizeof(*tree->nodes));
    }
    size_t parent = find_position(tree, tree->position);
    tree->nodes[tree->count++] = (struct session_tree_node){
        .item = item,
        .record = record,
        .parent = parent,
    };
    tree->position = record;
}

int session_tree_navigate(struct session_tree *tree, size_t position)
{
    if (position != 0 && find_position(tree, position) == SIZE_MAX)
        return -1;
    tree->position = position;
    return 0;
}

void session_tree_undo(struct session_tree *tree, size_t keep_user_turns)
{
    size_t leaf = find_position(tree, tree->position);
    size_t turns = 0;
    for (size_t i = leaf; i != SIZE_MAX; i = tree->nodes[i].parent)
        if (item_is_typed_prompt(&tree->nodes[i].item))
            turns++;
    if (keep_user_turns >= turns)
        return;

    size_t cut = leaf;
    for (size_t i = leaf; i != SIZE_MAX; i = tree->nodes[i].parent) {
        if (item_is_typed_prompt(&tree->nodes[i].item) && --turns == keep_user_turns) {
            cut = tree->nodes[i].parent;
            break;
        }
    }
    if (cut != SIZE_MAX && tree->nodes[cut].item.kind == ITEM_TURN_BOUNDARY)
        cut = tree->nodes[cut].parent;
    tree->position = cut == SIZE_MAX ? 0 : tree->nodes[cut].record;
}

static size_t prompt_position(const struct session_tree *tree, size_t index)
{
    size_t parent = tree->nodes[index].parent;
    if (parent != SIZE_MAX && tree->nodes[parent].item.kind == ITEM_TURN_BOUNDARY)
        parent = tree->nodes[parent].parent;
    return parent == SIZE_MAX ? 0 : tree->nodes[parent].record;
}

void session_tree_export(struct session_tree *tree, struct session_loaded *loaded)
{
    size_t count = tree->count;
    unsigned char *active = xcalloc(count, sizeof(*active));
    unsigned char *has_active_response = xcalloc(count, sizeof(*has_active_response));
    size_t *prompts = xmalloc(count * sizeof(*prompts));
    size_t leaf = find_position(tree, tree->position);
    for (size_t i = leaf; i != SIZE_MAX; i = tree->nodes[i].parent)
        active[i] = 1;
    loaded->tree = xcalloc(count, sizeof(*loaded->tree));
    for (size_t i = 0; i < count; i++) {
        struct session_tree_node *node = &tree->nodes[i];
        size_t parent_prompt = node->parent == SIZE_MAX ? SIZE_MAX : prompts[node->parent];
        prompts[i] = parent_prompt;
        if (item_is_typed_prompt(&node->item)) {
            prompts[i] = loaded->n_tree;
            loaded->tree[loaded->n_tree++] = (struct session_tree_entry){
                .parent = parent_prompt,
                .position = prompt_position(tree, i),
                .after_position = node->record,
                .prompt = xstrdup(node->item.text ? node->item.text : ""),
            };
        }
        if (prompts[i] == SIZE_MAX || node->item.kind == ITEM_TURN_BOUNDARY)
            continue;

        /* A prompt can have several resumed responses. Prefer its selected-path response;
         * otherwise continue its newest recorded response, not a descendant's conversation. */
        size_t prompt = prompts[i];
        if (active[i] || !has_active_response[prompt])
            loaded->tree[prompt].after_position = node->record;
        if (active[i])
            has_active_response[prompt] = 1;
    }
    if (leaf != SIZE_MAX && prompts[leaf] != SIZE_MAX)
        loaded->tree[prompts[leaf]].current = 1;

    loaded->items = xmalloc(count * sizeof(*loaded->items));
    loaded->retired = xmalloc(count * sizeof(*loaded->retired));
    for (size_t i = 0; i < count; i++) {
        struct item item = tree->nodes[i].item;
        if (active[i])
            loaded->items[loaded->n_items++] = item;
        else
            loaded->retired[loaded->n_retired++] = item;
        memset(&tree->nodes[i].item, 0, sizeof(tree->nodes[i].item));
    }
    free(prompts);
    free(has_active_response);
    free(active);
}

void session_tree_free(struct session_tree *tree)
{
    for (size_t i = 0; i < tree->count; i++)
        item_free(&tree->nodes[i].item);
    free(tree->nodes);
    memset(tree, 0, sizeof(*tree));
}
