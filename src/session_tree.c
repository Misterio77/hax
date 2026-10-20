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

/* A seed that keeps a tail was appended after the items it keeps, since a file can only grow at
 * the end. Relink it in front of them, so the path — and therefore the model-visible window —
 * reads as summary, kept tail, then everything recorded afterwards. */
static void splice_kept_tail(struct session_tree *tree, size_t seed)
{
    size_t kept = tree->nodes[seed].item.keep_count;
    if (kept == 0 || tree->nodes[seed].parent == SIZE_MAX)
        return;

    size_t oldest = tree->nodes[seed].parent; /* the first kept item */
    size_t picked = 1;
    while (picked < kept && tree->nodes[oldest].parent != SIZE_MAX) {
        oldest = tree->nodes[oldest].parent;
        picked++;
    }
    /* Whatever comes before the kept tail now precedes the seed; a truncated walk keeps the whole
     * conversation, which makes the seed a root. */
    size_t head = tree->nodes[oldest].parent;
    tree->nodes[oldest].parent = seed;
    tree->nodes[seed].parent = head;
}

void session_tree_append(struct session_tree *tree, size_t record, struct item item)
{
    if (tree->count == tree->capacity) {
        tree->capacity = tree->capacity ? tree->capacity * 2 : 16;
        tree->nodes = xrealloc(tree->nodes, tree->capacity * sizeof(*tree->nodes));
    }
    size_t parent = find_position(tree, tree->position);
    size_t previous_position = tree->position;
    tree->nodes[tree->count] = (struct session_tree_node){
        .item = item,
        .record = record,
        .parent = parent,
    };
    tree->count++;
    if (item.kind == ITEM_USER_MESSAGE && item.origin == ITEM_ORIGIN_COMPACT_SEED &&
        item.keep_count > 0) {
        splice_kept_tail(tree, tree->count - 1);
        /* Later records extend the kept tail, so stay on its last item. */
        tree->position = previous_position;
    } else {
        tree->position = record;
    }
}

int session_tree_navigate(struct session_tree *tree, size_t position)
{
    if (position != 0 && find_position(tree, position) == SIZE_MAX)
        return -1;
    tree->position = position;
    return 0;
}

size_t session_tree_departing_count(const struct session_tree *tree, size_t target)
{
    unsigned char *shared = xcalloc(tree->count ? tree->count : 1, sizeof(*shared));
    size_t target_leaf = target == 0 ? SIZE_MAX : find_position(tree, target);
    for (size_t i = target_leaf; i != SIZE_MAX; i = tree->nodes[i].parent)
        shared[i] = 1;

    size_t departing = 0;
    for (size_t i = find_position(tree, tree->position); i != SIZE_MAX; i = tree->nodes[i].parent) {
        if (shared[i])
            break;
        departing++;
    }
    free(shared);
    return departing;
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

/* Prompt ancestry comes from the parent chain rather than from record order: splicing a kept tail
 * behind its seed links a lower record to a higher one. Each node is walked up once, so the pass
 * stays linear in the number of nodes. `prompts[i]` is the index of the nearest typed prompt at or
 * above node i, or SIZE_MAX when the node has none. */
#define PROMPT_UNVISITED SIZE_MAX

static size_t resolve_prompt(const struct session_tree *tree, size_t index, size_t *prompts,
                             size_t *walk)
{
    size_t depth = 0;
    size_t node = index;
    size_t ancestor = SIZE_MAX;

    while (prompts[node] == PROMPT_UNVISITED) {
        walk[depth++] = node;
        if (tree->nodes[node].parent == SIZE_MAX)
            break;
        node = tree->nodes[node].parent;
    }
    if (prompts[node] != PROMPT_UNVISITED)
        ancestor = prompts[node];
    while (depth > 0) {
        size_t current = walk[--depth];
        if (item_is_typed_prompt(&tree->nodes[current].item))
            ancestor = current;
        prompts[current] = ancestor;
    }
    return prompts[index];
}

void session_tree_export(struct session_tree *tree, struct session_loaded *loaded)
{
    size_t count = tree->count;
    unsigned char *active = xcalloc(count, sizeof(*active));
    unsigned char *has_active_response = xcalloc(count, sizeof(*has_active_response));
    size_t *prompts = xmalloc(count * sizeof(*prompts));
    size_t *rows = xmalloc(count * sizeof(*rows));
    size_t *path = xmalloc(count * sizeof(*path));
    size_t *walk = xmalloc(count * sizeof(*walk));
    size_t path_count = 0;
    size_t leaf = find_position(tree, tree->position);
    for (size_t i = leaf; i != SIZE_MAX; i = tree->nodes[i].parent) {
        active[i] = 1;
        path[path_count++] = i;
    }

    loaded->tree = xcalloc(count, sizeof(*loaded->tree));
    for (size_t i = 0; i < count; i++) {
        rows[i] = SIZE_MAX;
        prompts[i] = PROMPT_UNVISITED;
        if (item_is_typed_prompt(&tree->nodes[i].item))
            rows[i] = loaded->n_tree++;
    }
    for (size_t i = 0; i < count; i++)
        (void)resolve_prompt(tree, i, prompts, walk);
    for (size_t i = 0; i < count; i++) {
        if (rows[i] == SIZE_MAX)
            continue;
        size_t parent = tree->nodes[i].parent;
        size_t ancestor = parent == SIZE_MAX ? SIZE_MAX : prompts[parent];
        loaded->tree[rows[i]] = (struct session_tree_entry){
            .parent = ancestor == SIZE_MAX ? SIZE_MAX : rows[ancestor],
            .position = prompt_position(tree, i),
            .after_position = tree->nodes[i].record,
            .prompt = xstrdup(tree->nodes[i].item.text ? tree->nodes[i].item.text : ""),
        };
    }
    for (size_t i = 0; i < count; i++) {
        if (prompts[i] == SIZE_MAX || tree->nodes[i].item.kind == ITEM_TURN_BOUNDARY)
            continue;

        /* A prompt can have several resumed responses. Prefer its selected-path response;
         * otherwise continue its newest recorded response, not a descendant's conversation. */
        size_t row = rows[prompts[i]];
        if (active[i] || !has_active_response[row])
            loaded->tree[row].after_position = tree->nodes[i].record;
        if (active[i])
            has_active_response[row] = 1;
    }
    if (leaf != SIZE_MAX && prompts[leaf] != SIZE_MAX)
        loaded->tree[rows[prompts[leaf]]].current = 1;

    /* The live conversation follows the path, which a compaction may order differently from the
     * file; everything off it is retired in record order. */
    loaded->items = xmalloc(count * sizeof(*loaded->items));
    loaded->retired = xmalloc(count * sizeof(*loaded->retired));
    for (size_t i = path_count; i-- > 0;) {
        loaded->items[loaded->n_items++] = tree->nodes[path[i]].item;
        memset(&tree->nodes[path[i]].item, 0, sizeof(tree->nodes[path[i]].item));
    }
    for (size_t i = 0; i < count; i++) {
        if (active[i])
            continue;
        loaded->retired[loaded->n_retired++] = tree->nodes[i].item;
        memset(&tree->nodes[i].item, 0, sizeof(tree->nodes[i].item));
    }
    free(walk);
    free(path);
    free(rows);
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
