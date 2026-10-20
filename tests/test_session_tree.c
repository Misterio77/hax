/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "harness.h"
#include "provider.h"
#include "session.h"
#include "session_tree.h"
#include "xalloc.h"

static void append(struct session_tree *tree, size_t id, enum item_kind kind, const char *text)
{
    session_tree_append(tree, id, (struct item){.kind = kind, .text = xstrdup(text)});
}

static void add_exchange(struct session_tree *tree, size_t id, const char *prompt)
{
    append(tree, id, ITEM_TURN_BOUNDARY, NULL);
    append(tree, id + 1, ITEM_USER_MESSAGE, prompt);
    append(tree, id + 2, ITEM_ASSISTANT_MESSAGE, "reply");
}

static void test_linear_turn_boundaries(void)
{
    struct session_tree tree = {0};
    add_exchange(&tree, 2, "first");
    add_exchange(&tree, 7, "second");
    struct session_loaded loaded = {0};
    session_tree_export(&tree, &loaded);
    EXPECT(loaded.n_items == 6);
    EXPECT(loaded.n_retired == 0);
    EXPECT(loaded.n_tree == 2);
    EXPECT(loaded.tree[0].position == 0);
    EXPECT(loaded.tree[0].parent == SIZE_MAX);
    EXPECT(loaded.tree[1].position == 4);
    EXPECT(loaded.tree[1].parent == 0);
    EXPECT(loaded.tree[0].after_position == 4);
    EXPECT(loaded.tree[1].after_position == 9);
    EXPECT(!loaded.tree[0].current);
    EXPECT(loaded.tree[1].current);
    session_loaded_free(&loaded);
    session_tree_free(&tree);
}

static void test_undo_preserves_sibling_and_can_restore_it(void)
{
    struct session_tree tree = {0};
    add_exchange(&tree, 2, "first");
    add_exchange(&tree, 5, "original");
    session_tree_undo(&tree, 1);
    EXPECT(tree.position == 4);
    add_exchange(&tree, 11, "alternative");
    EXPECT(session_tree_navigate(&tree, 7) == 0);
    struct session_loaded loaded = {0};
    session_tree_export(&tree, &loaded);
    EXPECT(loaded.n_items == 6);
    EXPECT(loaded.n_retired == 3);
    EXPECT_STR_EQ(loaded.items[4].text, "original");
    EXPECT_STR_EQ(loaded.retired[1].text, "alternative");
    EXPECT(loaded.n_tree == 3);
    EXPECT(loaded.tree[1].parent == 0);
    EXPECT(loaded.tree[2].parent == 0);
    EXPECT(loaded.tree[1].current);
    EXPECT(!loaded.tree[2].current);
    EXPECT(!loaded.tree[0].current);
    EXPECT(loaded.tree[1].after_position == 7);
    EXPECT(loaded.tree[2].after_position == 13);
    session_loaded_free(&loaded);
    session_tree_free(&tree);
}

static void test_root_branches_and_invalid_positions(void)
{
    struct session_tree tree = {0};
    add_exchange(&tree, 3, "original root");
    EXPECT(session_tree_navigate(&tree, 0) == 0);
    add_exchange(&tree, 20, "alternative root");
    EXPECT(session_tree_navigate(&tree, 999) == -1);
    EXPECT(tree.position == 22);
    EXPECT(session_tree_navigate(&tree, 4) == 0);
    struct session_loaded loaded = {0};
    session_tree_export(&tree, &loaded);
    EXPECT(loaded.n_items == 2);
    EXPECT(loaded.n_retired == 4);
    EXPECT(loaded.tree[0].parent == SIZE_MAX);
    EXPECT(loaded.tree[1].parent == SIZE_MAX);
    EXPECT(loaded.tree[0].current);
    EXPECT(!loaded.tree[1].current);
    session_loaded_free(&loaded);
    session_tree_free(&tree);
}

static void test_compaction_and_continuation_follow_ancestry(void)
{
    struct session_tree tree = {0};
    add_exchange(&tree, 2, "first");
    session_tree_append(&tree, 8,
                        (struct item){.kind = ITEM_USER_MESSAGE,
                                      .text = xstrdup("summary of first branch"),
                                      .origin = ITEM_ORIGIN_COMPACT_SEED});
    add_exchange(&tree, 9, "second");
    session_tree_append(&tree, 12,
                        (struct item){.kind = ITEM_USER_MESSAGE,
                                      .text = xstrdup("continue"),
                                      .origin = ITEM_ORIGIN_CONTINUATION});
    session_tree_undo(&tree, 1);
    EXPECT(tree.position == 8);
    EXPECT(session_tree_navigate(&tree, 4) == 0);
    add_exchange(&tree, 20, "without summary");
    struct session_loaded loaded = {0};
    session_tree_export(&tree, &loaded);
    EXPECT(loaded.n_items == 6);
    EXPECT(items_context_floor(loaded.items, loaded.n_items) == 0);
    EXPECT(loaded.n_tree == 3);
    EXPECT_STR_EQ(loaded.tree[1].prompt, "second");
    EXPECT(loaded.tree[1].position == 8);
    EXPECT(loaded.tree[0].after_position == 4);
    EXPECT(loaded.tree[1].after_position == 12);
    EXPECT_STR_EQ(loaded.tree[2].prompt, "without summary");
    session_loaded_free(&loaded);
    session_tree_free(&tree);
}

static void test_repeated_navigation_does_not_duplicate_items(void)
{
    struct session_tree tree = {0};
    add_exchange(&tree, 2, "first");
    add_exchange(&tree, 5, "second");
    for (size_t i = 0; i < 100; i++) {
        EXPECT(session_tree_navigate(&tree, 4) == 0);
        EXPECT(session_tree_navigate(&tree, 7) == 0);
    }
    session_tree_undo(&tree, 0);
    EXPECT(tree.position == 0);
    session_tree_undo(&tree, 99);
    EXPECT(tree.position == 0);
    struct session_loaded loaded = {0};
    session_tree_export(&tree, &loaded);
    EXPECT(loaded.n_items == 0);
    EXPECT(loaded.n_retired == 6);
    session_loaded_free(&loaded);
    session_tree_free(&tree);
}

static void test_empty_tree(void)
{
    struct session_tree tree = {0};
    struct session_loaded loaded = {0};
    EXPECT(session_tree_navigate(&tree, 0) == 0);
    EXPECT(session_tree_navigate(&tree, 1) == -1);
    session_tree_export(&tree, &loaded);
    EXPECT(loaded.n_items == 0);
    EXPECT(loaded.n_tree == 0);
    session_loaded_free(&loaded);
    session_tree_free(&tree);
}

int main(void)
{
    test_linear_turn_boundaries();
    test_undo_preserves_sibling_and_can_restore_it();
    test_root_branches_and_invalid_positions();
    test_compaction_and_continuation_follow_ancestry();
    test_repeated_navigation_does_not_duplicate_items();
    test_empty_tree();
    T_REPORT();
}
