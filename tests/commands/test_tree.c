/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "session.h"
#include "xalloc.h"
#include "commands/tree.h"
#include "terminal/picker.h"

static void test_tree_connectors_and_current_position(void)
{
    struct session_tree_entry entries[] = {
        {.parent = SIZE_MAX, .prompt = "repo"},    {.parent = 0, .prompt = "favorite"},
        {.parent = 0, .prompt = "least favorite"}, {.parent = 1, .prompt = "second"},
        {.parent = 3, .prompt = "third"},          {.parent = 2, .prompt = "worst", .current = 1},
    };
    struct session_loaded loaded = {.tree = entries, .n_tree = 6};
    struct tree_view view;
    tree_view_build(&loaded, 1, &view);
    EXPECT(view.count == 6);
    EXPECT_STR_EQ(view.items[0].label, "repo");
    EXPECT_STR_EQ(view.items[1].label, "├─ favorite");
    EXPECT_STR_EQ(view.items[2].label, "│  └─ second");
    EXPECT_STR_EQ(view.items[3].label, "│     └─ third");
    EXPECT_STR_EQ(view.items[4].label, "└─ least favorite");
    EXPECT_STR_EQ(view.items[5].label, "   └─ worst");
    EXPECT(view.initial == 5);
    EXPECT(view.entries[2] == 3);
    for (size_t i = 0; i < view.count; i++) {
        EXPECT(view.items[i].detail == NULL);
        EXPECT(view.items[i].current == (i == 5));
    }
    tree_view_free(&view);
}

static void test_multiple_roots_and_ascii_fallback(void)
{
    struct session_tree_entry entries[] = {
        {.parent = SIZE_MAX, .prompt = "root A"},
        {.parent = 0, .prompt = "child"},
        {.parent = SIZE_MAX, .prompt = "root B", .current = 1},
    };
    struct session_loaded loaded = {.tree = entries, .n_tree = 3};
    struct tree_view view;
    tree_view_build(&loaded, 0, &view);
    EXPECT_STR_EQ(view.items[0].label, "|- root A");
    EXPECT_STR_EQ(view.items[1].label, "|  `- child");
    EXPECT_STR_EQ(view.items[2].label, "`- root B");
    EXPECT(view.initial == 2);
    tree_view_free(&view);
}

static void test_empty_and_unsafe_prompt_labels(void)
{
    struct session_tree_entry entries[] = {
        {.parent = SIZE_MAX, .prompt = ""},
        {.parent = 0, .prompt = "one\ntwo\x1b[2J"},
    };
    struct session_loaded loaded = {.tree = entries, .n_tree = 2};
    struct tree_view view;
    tree_view_build(&loaded, 1, &view);
    EXPECT_STR_EQ(view.items[0].label, "(empty prompt)");
    EXPECT(strchr(view.items[1].label, '\n') == NULL);
    EXPECT(strchr(view.items[1].label, '\x1b') == NULL);
    tree_view_free(&view);
    loaded = (struct session_loaded){0};
    tree_view_build(&loaded, 1, &view);
    EXPECT(view.count == 0);
    tree_view_free(&view);
}

static void test_deep_histories_bound_indentation(void)
{
    size_t count = 1000;
    struct session_tree_entry *entries = xcalloc(count, sizeof(*entries));
    for (size_t i = 0; i < count; i++) {
        entries[i].parent = i == 0 ? SIZE_MAX : i - 1;
        entries[i].prompt = "turn";
    }
    entries[count - 1].current = 1;
    struct session_loaded loaded = {.tree = entries, .n_tree = count};
    struct tree_view view;
    tree_view_build(&loaded, 0, &view);
    EXPECT(view.count == count);
    EXPECT(view.initial == count - 1);
    EXPECT(strlen(view.items[count - 1].label) < 85);
    EXPECT(strncmp(view.items[count - 1].label, "... ", 4) == 0);
    tree_view_free(&view);
    free(entries);
}

int main(void)
{
    test_tree_connectors_and_current_position();
    test_multiple_roots_and_ascii_fallback();
    test_empty_and_unsafe_prompt_labels();
    test_deep_histories_bound_indentation();
    T_REPORT();
}
