/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "compact.h"
#include "config.h"
#include "harness.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "transport/http.h"

static void test_over_threshold(void)
{
    EXPECT(!compact_over_threshold(100, 0, 85));
    EXPECT(!compact_over_threshold(100, -1, 85));
    EXPECT(!compact_over_threshold(-1, 1000, 85));
    EXPECT(!compact_over_threshold(100, 1000, 0));
    EXPECT(!compact_over_threshold(100, 1000, 101));

    EXPECT(!compact_over_threshold(8499, 10000, 85));
    EXPECT(compact_over_threshold(8500, 10000, 85));
    EXPECT(compact_over_threshold(9999, 10000, 85));
    EXPECT(!compact_over_threshold(9999, 10000, 100));
    EXPECT(compact_over_threshold(10000, 10000, 100));

    EXPECT(!compact_over_threshold(LONG_MAX - 1, LONG_MAX, 100));
    EXPECT(compact_over_threshold(LONG_MAX, LONG_MAX, 100));
}

static void test_should_auto(void)
{
    /* Drive the config tiers via runtime overrides so the test is
     * hermetic regardless of env / file config. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");

    EXPECT(!compact_should_auto(8999, 10000)); /* 89.99% < 90% */
    EXPECT(compact_should_auto(9000, 10000));  /* exactly 90% */
    EXPECT(!compact_should_auto(9999, 0));     /* unknown window */

    /* Disabled via config: never auto-compacts, even when far over. */
    config_set_override("compact.auto", "0");
    EXPECT(!compact_should_auto(100000, 10000));

    /* Out-of-range threshold falls back to the 85% default. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "0");
    EXPECT(!compact_should_auto(8499, 10000));
    EXPECT(compact_should_auto(8500, 10000));
}

enum mock_script {
    MOCK_SUCCESS,
    MOCK_RETRY_ONCE,
    MOCK_ALWAYS_TOOL_CALL,
    MOCK_ERROR,
    MOCK_NO_SUMMARY,
    MOCK_INCOMPLETE,
    MOCK_STREAM_RETRY,
    MOCK_STREAM_RETRY_INCOMPLETE,
};

struct mock_state {
    enum mock_script script;
    int stream_calls;
    int cancel_requested;
    int observed_events;
    size_t last_request_items;
    char last_prompt[2048];    /* newest user message the request carried */
    size_t last_output_bytes;  /* largest tool result the request carried */
    int last_output_truncated; /* a result carried the truncation marker */
};

static struct mock_state mock;

static void reset_mock(enum mock_script script)
{
    memset(&mock, 0, sizeof(mock));
    mock.script = script;
}

static struct stream_usage usage_tokens(long input, long output)
{
    return (struct stream_usage){
        .input_tokens = input,
        .output_tokens = output,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
}

static void emit_text(stream_cb cb, void *user, const char *text)
{
    struct stream_event ev = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = text}};
    cb(&ev, user);
}

static void emit_done(stream_cb cb, void *user, long input_tokens)
{
    struct stream_event ev = {.kind = EV_DONE, .u.done = {.usage = usage_tokens(input_tokens, 10)}};
    cb(&ev, user);
}

static void emit_error(stream_cb cb, void *user, const char *message,
                       const struct stream_usage *usage)
{
    struct stream_event ev = {.kind = EV_ERROR, .u.error = {.message = message, .usage = usage}};
    cb(&ev, user);
}

static void emit_retry(stream_cb cb, void *user, const struct stream_usage *usage)
{
    struct stream_event ev = {.kind = EV_RETRY,
                              .u.retry = {.attempt = 1, .max_attempts = 5, .usage = usage}};
    cb(&ev, user);
}

static void emit_tool_call(stream_cb cb, void *user)
{
    struct stream_event start = {.kind = EV_TOOL_CALL_START,
                                 .u.tool_call_start = {.id = "c1", .name = "read"}};
    struct stream_event delta = {.kind = EV_TOOL_CALL_DELTA,
                                 .u.tool_call_delta = {.id = "c1", .args_delta = "{}"}};
    struct stream_event end = {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "c1"}};
    cb(&start, user);
    cb(&delta, user);
    cb(&end, user);
}

static int mock_stream(struct provider *provider, const struct context *ctx, const char *model,
                       stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)provider;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    mock.last_request_items = ctx->n_items;
    mock.last_output_bytes = 0;
    mock.last_output_truncated = 0;
    for (size_t i = 0; i < ctx->n_items; i++) {
        if (ctx->items[i].kind != ITEM_TOOL_RESULT || !ctx->items[i].output)
            continue;
        size_t bytes = strlen(ctx->items[i].output);
        if (bytes > mock.last_output_bytes)
            mock.last_output_bytes = bytes;
        if (strstr(ctx->items[i].output, "characters truncated"))
            mock.last_output_truncated = 1;
    }
    mock.last_prompt[0] = '\0';
    for (size_t i = ctx->n_items; i-- > 0;) {
        if (ctx->items[i].kind != ITEM_USER_MESSAGE || !ctx->items[i].text)
            continue;
        snprintf(mock.last_prompt, sizeof(mock.last_prompt), "%s", ctx->items[i].text);
        break;
    }
    mock.stream_calls++;

    if ((mock.script == MOCK_RETRY_ONCE && mock.stream_calls == 1) ||
        mock.script == MOCK_ALWAYS_TOOL_CALL) {
        emit_text(cb, user, "I'll inspect the files");
        emit_tool_call(cb, user);
        emit_done(cb, user, 100);
        return 0;
    }
    if (mock.script == MOCK_ERROR) {
        struct stream_usage usage = usage_tokens(150, 5);
        emit_text(cb, user, "partial");
        emit_error(cb, user, "summary failed", &usage);
        return 0;
    }
    if (mock.script == MOCK_STREAM_RETRY || mock.script == MOCK_STREAM_RETRY_INCOMPLETE) {
        struct stream_usage retry_usage = usage_tokens(50, -1);
        emit_retry(cb, user, &retry_usage);
        /* A pause cancelling the redo leaves the banked usage with no terminal event. */
        if (mock.script == MOCK_STREAM_RETRY_INCOMPLETE)
            return 0;
    }
    if (mock.script != MOCK_NO_SUMMARY)
        emit_text(cb, user, "## Goal\n- continue");
    if (mock.script == MOCK_INCOMPLETE)
        return 0;
    emit_done(cb, user, mock.stream_calls == 1 ? 100 : 200);
    return 0;
}

static int observe_event(const struct stream_event *event, void *user)
{
    (void)event;
    (void)user;
    mock.observed_events++;
    return 0;
}

static int is_cancelled(void *user)
{
    (void)user;
    return mock.cancel_requested;
}

static void init_session(struct agent_session *session)
{
    memset(session, 0, sizeof(*session));
    session->model = xstrdup("model");
    session->provider_id = "test";
    agent_session_append(session,
                         (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("old history")});
}

static struct compact_result run_compaction(struct agent_session *session,
                                            struct provider *provider,
                                            struct session_log *session_log)
{
    struct compact_params params = {
        .session = session,
        .provider = provider,
        .session_log = session_log,
        .hooks =
            {
                .on_event = observe_event,
                .is_cancelled = is_cancelled,
            },
    };
    struct compact_result result;
    compact_run(&params, &result);
    return result;
}

static void test_applies_summary(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 1);
    EXPECT(mock.observed_events == 2);
    /* The summarized history stays; the seed and its usage footer are appended after it. */
    EXPECT(session.n_items == 4);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_BOUNDARY);
    EXPECT(session.items[2].kind == ITEM_USER_MESSAGE &&
           session.items[2].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[2].text && strstr(session.items[2].text, "earlier part") != NULL);
    EXPECT(session.items[2].text && strstr(session.items[2].text, "continue") != NULL);
    EXPECT(session.items[3].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[3].usage->usage.input_tokens == 100);

    /* What the model sees starts at the seed. */
    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 2);
    EXPECT(window.items[0].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(window.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_retried_attempt_usage_is_billed(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_STREAM_RETRY);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    /* The dead attempt was a served request of its own: its footer belongs to the summarized
     * prefix, and only the accepted attempt's footer follows the seed. */
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.n_items == 5);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 50);
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[4].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[4].usage->usage.input_tokens == 100);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_cancelled_retry_usage_is_billed(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_STREAM_RETRY_INCOMPLETE);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    /* No terminal event ever arrived, but the banked usage still gets a footer. */
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(session.n_items == 2);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].origin == ITEM_ORIGIN_COMPACTION);
    EXPECT(session.items[1].usage->usage.input_tokens == 50);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* A second pass summarizes only the window the first one left. */
static void test_compacts_twice(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);

    struct compact_result first = run_compaction(&session, &provider, NULL);
    EXPECT(first.outcome == COMPACT_COMPLETE);
    /* One history item plus the summarization prompt. */
    EXPECT(mock.last_request_items == 2);
    compact_result_destroy(&first);

    struct compact_result second = run_compaction(&session, &provider, NULL);
    EXPECT(second.outcome == COMPACT_COMPLETE);
    /* The first seed and its footer plus the prompt — the summarized prefix is not re-read. */
    EXPECT(mock.last_request_items == 3);
    EXPECT(session.n_items == 7);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[2].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[5].origin == ITEM_ORIGIN_COMPACT_SEED);

    /* The newest seed wins; the first one is now part of the summarized prefix. */
    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 2);
    EXPECT(window.items == &session.items[5]);

    /* Cutting past a seed, as /undo does, falls back to the older one and then to no seed. */
    EXPECT(items_context_floor(session.items, 5) == 2);
    EXPECT(items_context_floor(session.items, 2) == 0);

    compact_result_destroy(&second);
    agent_session_free(&session);
}

static void free_items(struct item *items, size_t n_items)
{
    for (size_t i = 0; i < n_items; i++)
        item_free(&items[i]);
    free(items);
}

/* Compaction continues the session file it was given, so one compacted conversation stays one
 * entry in the picker instead of a chain of them. */
static void test_keeps_one_session(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct session_log *session_log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(session_log != NULL);
    session_log_append(session_log, session.items, session.n_items);
    char *path = xstrdup(session_log_path(session_log));
    reset_mock(MOCK_RETRY_ONCE);

    struct compact_result result = run_compaction(&session, &provider, session_log);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 2);
    EXPECT_STR_EQ(path, session_log_path(session_log));

    /* The rejected attempt's footer belongs to the summarized prefix; only the accepted
     * attempt's footer follows the seed. */
    EXPECT(session.n_items == 5);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 100);
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[4].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[4].usage->usage.input_tokens == 200);

    struct item *recorded = NULL;
    size_t n_recorded = 0;
    EXPECT(session_load(path, &recorded, &n_recorded, NULL) == 0);
    EXPECT(n_recorded == 5);
    EXPECT_STR_EQ(recorded[0].text, "old history");
    EXPECT(recorded[3].kind == ITEM_USER_MESSAGE && recorded[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(recorded[4].usage->usage.input_tokens == 200);

    free_items(recorded, n_recorded);
    free(path);
    compact_result_destroy(&result);
    session_log_close(session_log);
    agent_session_free(&session);
}

static void test_tool_call_retries_are_bounded(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_ALWAYS_TOOL_CALL);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(result.attempts == 4);
    EXPECT(mock.stream_calls == 4);
    EXPECT(mock.last_request_items == 11);
    EXPECT(session.n_items == 5);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    for (size_t i = 1; i < session.n_items; i++)
        EXPECT(session.items[i].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* The floor is derived from the recorded seed, so a resumed file needs no carried-over state. */
static void test_resumed_session_floors_at_seed(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct session_log *session_log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(session_log != NULL);
    session_log_append(session_log, session.items, session.n_items);
    char *path = xstrdup(session_log_path(session_log));
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, session_log);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    compact_result_destroy(&result);
    session_log_close(session_log);
    agent_session_free(&session);

    struct agent_session resumed;
    memset(&resumed, 0, sizeof(resumed));
    EXPECT(session_load(path, &resumed.items, &resumed.n_items, NULL) == 0);
    resumed.cap_items = resumed.n_items;
    EXPECT(resumed.n_items == 4);
    EXPECT_STR_EQ(resumed.items[0].text, "old history");

    struct context window = agent_session_context(&resumed);
    EXPECT(window.n_items == 2);
    EXPECT(window.items[0].origin == ITEM_ORIGIN_COMPACT_SEED);

    free(path);
    agent_session_free(&resumed);
}

static void add_turn(struct agent_session *session, const char *prompt)
{
    agent_session_add_user(session, prompt);
    agent_session_append(session,
                         (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("reply")});
}

/* One turn is summarized, the newest one survives word for word in front of which the seed is
 * placed: a compacted conversation continues with the state it was last in. */
static void test_keeps_recent_turn(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    add_turn(&session, "old history");
    add_turn(&session, "recent history");
    struct provider provider = {.name = "test", .stream = mock_stream};
    config_set_override("compact.keep_turns", "1");
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    /* The request carries the summarized turn and the prompt, not the kept one. */
    EXPECT(mock.last_request_items == 4);
    EXPECT(session.n_items == 8);
    EXPECT_STR_EQ(session.items[1].text, "old history");
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[3].keep_count == 3);
    EXPECT(session.items[4].kind == ITEM_TURN_BOUNDARY);
    EXPECT_STR_EQ(session.items[5].text, "recent history");
    EXPECT(session.items[7].kind == ITEM_TURN_USAGE);

    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 5);
    EXPECT(window.items[0].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT_STR_EQ(window.items[2].text, "recent history");
    /* The kept turn's own footers describe the window that was just replaced. */
    EXPECT(agent_session_last_context_tokens(&session) == -1);

    config_set_override("compact.keep_turns", NULL);
    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* compact.keep_turns = 0 restores summarizing the whole window. */
static void test_keep_turns_zero_summarizes_all(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    add_turn(&session, "old history");
    add_turn(&session, "recent history");
    struct provider provider = {.name = "test", .stream = mock_stream};
    config_set_override("compact.keep_turns", "0");
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(mock.last_request_items == 7);
    EXPECT(session.n_items == 9);
    EXPECT(session.items[7].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[6].keep_count == 0);
    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 2);

    config_set_override("compact.keep_turns", NULL);
    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* Asking for more turns than the conversation has keeps as many as still leave something to
 * summarize, rather than declining to compact at all. */
static void test_keeping_more_turns_than_exist_keeps_one_fewer(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    add_turn(&session, "old history");
    add_turn(&session, "recent history");
    struct provider provider = {.name = "test", .stream = mock_stream};
    config_set_override("compact.keep_turns", "2");
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.n_items == 8);
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[3].keep_count == 3);
    EXPECT_STR_EQ(session.items[5].text, "recent history");
    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 5);

    config_set_override("compact.keep_turns", NULL);
    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* A window the tail would fill leaves no room to continue, so the budget drops turns instead of
 * keeping a window the provider would reject. */
static void test_oversized_turn_is_summarized_instead_of_kept(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    add_turn(&session, "old history");
    add_turn(&session, "recent history");
    struct provider provider = {.name = "test", .stream = mock_stream};
    config_set_override("compact.keep_turns", "1");
    /* A 24-token window budgets 6; even one turn is larger. */
    config_set_override("context_limit", "24");
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    /* Everything is summarized, as if nothing were kept. */
    EXPECT(mock.last_request_items == 7);
    EXPECT(session.n_items == 9);
    EXPECT(session.items[7].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[7].keep_count == 0);
    EXPECT(agent_session_context(&session).n_items == 2);

    config_set_override("context_limit", NULL);
    config_set_override("compact.keep_turns", NULL);
    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* The budget only trims: a window with room keeps the turn it was asked to. */
static void test_budget_keeps_a_turn_that_fits(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    add_turn(&session, "old history");
    add_turn(&session, "recent history");
    struct provider provider = {.name = "test", .stream = mock_stream};
    config_set_override("compact.keep_turns", "1");
    config_set_override("context_limit", "4096");
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[3].keep_count == 3);

    config_set_override("context_limit", NULL);
    config_set_override("compact.keep_turns", NULL);
    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* The kept tail is recorded after the seed and restored ahead of it, so a resumed conversation
 * sees the same window without carrying replay state in the file. */
static void test_kept_tail_survives_reload(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    add_turn(&session, "old history");
    add_turn(&session, "recent history");
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct session_log *session_log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(session_log != NULL);
    session_log_append(session_log, session.items, session.n_items);
    char *path = xstrdup(session_log_path(session_log));
    config_set_override("compact.keep_turns", "1");
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, session_log);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    session_log_close(session_log);
    agent_session_free(&session);
    config_set_override("compact.keep_turns", NULL);

    struct agent_session resumed;
    memset(&resumed, 0, sizeof(resumed));
    EXPECT(session_load(path, &resumed.items, &resumed.n_items, NULL) == 0);
    resumed.cap_items = resumed.n_items;
    EXPECT(resumed.n_items == 8);
    EXPECT_STR_EQ(resumed.items[1].text, "old history");
    EXPECT(resumed.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(resumed.items[3].keep_count == 3);
    EXPECT_STR_EQ(resumed.items[5].text, "recent history");
    struct context window = agent_session_context(&resumed);
    EXPECT(window.n_items == 5);
    EXPECT(window.items[0].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT_STR_EQ(window.items[2].text, "recent history");

    free(path);
    compact_result_destroy(&result);
    agent_session_free(&resumed);
}

/* The file sections are data, not a model's recollection: paths come from the branch's own tool
 * calls, and a summary carried in from an earlier switch keeps its lists. */
static void test_branch_summary_records_touched_files(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct item departing[] = {
        {.kind = ITEM_USER_MESSAGE, .text = xstrdup("explore the parser")},
        {.kind = ITEM_TOOL_CALL,
         .call_id = xstrdup("c1"),
         .tool_name = xstrdup("read"),
         .tool_arguments_json = xstrdup("{\"path\":\"src/parse.c\"}")},
        {.kind = ITEM_TOOL_RESULT, .call_id = xstrdup("c1"), .output = xstrdup("clean")},
        {.kind = ITEM_TOOL_CALL,
         .call_id = xstrdup("c2"),
         .tool_name = xstrdup("edit"),
         .tool_arguments_json = xstrdup("{\"path\":\"src/parse.c\"}")},
        {.kind = ITEM_TOOL_RESULT, .call_id = xstrdup("c2"), .output = xstrdup("ok")},
        {.kind = ITEM_TOOL_CALL,
         .call_id = xstrdup("c3"),
         .tool_name = xstrdup("read"),
         .tool_arguments_json = xstrdup("{\"path\":\"src/parse.c\"}")},
        {.kind = ITEM_TOOL_RESULT, .call_id = xstrdup("c3"), .output = xstrdup("clean")},
        {.kind = ITEM_TOOL_CALL,
         .call_id = xstrdup("c4"),
         .tool_name = xstrdup("bash"),
         .tool_arguments_json = xstrdup("{\"command\":\"make\"}")},
        {.kind = ITEM_TOOL_RESULT, .call_id = xstrdup("c4"), .output = xstrdup("built")},
    };
    size_t count = sizeof(departing) / sizeof(departing[0]);
    reset_mock(MOCK_SUCCESS);

    char *summary = compact_summarize_branch(
        &(struct compact_params){.session = &session, .provider = &provider}, departing, count,
        NULL);
    EXPECT(summary != NULL);
    if (summary) {
        /* Each path appears once, under the section its tool belongs to. */
        EXPECT(strstr(summary, "## Goal") != NULL); /* the summary itself comes first */
        EXPECT(strstr(summary, "<read-files>\nsrc/parse.c\n</read-files>") != NULL);
        EXPECT(strstr(summary, "<modified-files>\nsrc/parse.c\n</modified-files>") != NULL);
        EXPECT(strstr(summary, "make") == NULL); /* bash has no attributable path */
    }
    free(summary);
    for (size_t i = 0; i < count; i++)
        item_free(&departing[i]);
    agent_session_free(&session);
}

/* A branch left earlier already recorded its files; switching away from it again must not lose
 * them. */
static void test_branch_summary_carries_earlier_file_lists(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct item departing[] = {
        {.kind = ITEM_USER_MESSAGE,
         .origin = ITEM_ORIGIN_BRANCH_SUMMARY,
         .text = xstrdup("earlier branch\n\n<read-files>\nREADME.md\n</read-files>\n\n"
                         "<modified-files>\ndocs/usage.md\n</modified-files>")},
        {.kind = ITEM_TOOL_CALL,
         .call_id = xstrdup("c1"),
         .tool_name = xstrdup("write"),
         .tool_arguments_json = xstrdup("{\"path\":\"src/new.c\"}")},
        {.kind = ITEM_TOOL_RESULT, .call_id = xstrdup("c1"), .output = xstrdup("ok")},
    };
    size_t count = sizeof(departing) / sizeof(departing[0]);
    reset_mock(MOCK_SUCCESS);

    char *summary = compact_summarize_branch(
        &(struct compact_params){.session = &session, .provider = &provider}, departing, count,
        NULL);
    EXPECT(summary != NULL);
    if (summary) {
        EXPECT(strstr(summary, "<read-files>\nREADME.md\n</read-files>") != NULL);
        EXPECT(strstr(summary, "<modified-files>\ndocs/usage.md\nsrc/new.c\n</modified-files>") !=
               NULL);
    }
    free(summary);
    for (size_t i = 0; i < count; i++)
        item_free(&departing[i]);
    agent_session_free(&session);
}

/* A branch with no tool calls has nothing to list. */
static void test_branch_summary_without_files_adds_no_sections(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct item departing[] = {{.kind = ITEM_USER_MESSAGE, .text = xstrdup("just talking")}};
    reset_mock(MOCK_SUCCESS);

    char *summary = compact_summarize_branch(
        &(struct compact_params){.session = &session, .provider = &provider}, departing, 1, NULL);
    EXPECT(summary != NULL);
    if (summary)
        EXPECT(strstr(summary, "<read-files>") == NULL);
    free(summary);
    item_free(&departing[0]);
    agent_session_free(&session);
}

/* Summarizing pays for every byte it sends, and a tool result is usually most of a window. The
 * request sees its head only; the conversation keeps all of it. */
static void test_summary_request_truncates_tool_output(void)
{
    struct agent_session session;
    memset(&session, 0, sizeof(session));
    session.model = xstrdup("model");
    session.provider_id = "test";
    char *long_output = xmalloc(5001);
    memset(long_output, 'x', 5000);
    long_output[5000] = '\0';
    agent_session_append(&session, (struct item){.kind = ITEM_TOOL_CALL,
                                                 .call_id = xstrdup("c1"),
                                                 .tool_name = xstrdup("read"),
                                                 .tool_arguments_json = xstrdup("{}")});
    agent_session_append(
        &session,
        (struct item){.kind = ITEM_TOOL_RESULT, .call_id = xstrdup("c1"), .output = long_output});
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(mock.last_output_truncated);
    EXPECT(mock.last_output_bytes > 2000 && mock.last_output_bytes < 2100);
    /* The live conversation still holds every byte the tool produced. */
    EXPECT(session.items[1].kind == ITEM_TOOL_RESULT);
    EXPECT(strlen(session.items[1].output) == 5000);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* A branch summary is a report about work that is not the current conversation, so it must not
 * append a seed or otherwise reshape the window it was asked from. */
static void test_branch_summary_leaves_history_alone(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct item departing[] = {
        {.kind = ITEM_USER_MESSAGE, .text = xstrdup("explore the parser")},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("it lives in parse.c")},
    };
    reset_mock(MOCK_SUCCESS);

    int attempts = 0;
    char *summary = compact_summarize_branch(
        &(struct compact_params){.session = &session, .provider = &provider}, departing, 2,
        &attempts);
    EXPECT(summary != NULL);
    EXPECT(summary && strstr(summary, "continue") != NULL);
    EXPECT(attempts == 1);
    /* The departing items and the instructions; the live window is not resent. */
    EXPECT(mock.last_request_items == 3);
    EXPECT(strstr(mock.last_prompt, "Summarize the conversation branch") != NULL);
    EXPECT(strstr(mock.last_prompt, "## Left Behind") != NULL);

    /* History gains the request's usage footer and nothing else. */
    EXPECT(session.n_items == 2);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].origin == ITEM_ORIGIN_COMPACTION);

    char *seed = compact_branch_seed(summary);
    EXPECT(seed && strstr(seed, "no longer the current state") != NULL);
    EXPECT(seed && strstr(seed, "continue") != NULL);
    free(seed);
    free(summary);
    for (size_t i = 0; i < 2; i++)
        item_free(&departing[i]);
    agent_session_free(&session);
}

static void test_branch_summary_cancel_still_bills(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct item departing[] = {{.kind = ITEM_USER_MESSAGE, .text = xstrdup("explore")}};
    reset_mock(MOCK_SUCCESS);
    mock.cancel_requested = 1;

    char *summary = compact_summarize_branch(
        &(struct compact_params){
            .session = &session,
            .provider = &provider,
            .hooks = {.is_cancelled = is_cancelled},
        },
        departing, 1, NULL);
    EXPECT(summary == NULL);
    EXPECT(session.n_items == 2);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].origin == ITEM_ORIGIN_COMPACTION);

    item_free(&departing[0]);
    agent_session_free(&session);
}

/* Without a provider or model there is nothing to ask, and nothing to bill. */
static void test_branch_summary_requires_a_model(void)
{
    struct agent_session session;
    init_session(&session);
    struct item departing[] = {{.kind = ITEM_USER_MESSAGE, .text = xstrdup("explore")}};
    reset_mock(MOCK_SUCCESS);

    EXPECT(compact_summarize_branch(&(struct compact_params){.session = &session, .provider = NULL},
                                    departing, 1, NULL) == NULL);
    free(session.model);
    session.model = NULL;
    struct provider provider = {.name = "test", .stream = mock_stream};
    EXPECT(compact_summarize_branch(
               &(struct compact_params){.session = &session, .provider = &provider}, departing, 1,
               NULL) == NULL);
    EXPECT(mock.stream_calls == 0);
    EXPECT(session.n_items == 1);

    item_free(&departing[0]);
    agent_session_free(&session);
}

static void test_preserves_failure(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_ERROR);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_PROVIDER_ERROR);
    EXPECT_STR_EQ(result.error_message, "summary failed");
    /* Partial summary text never enters history; billed failure usage does. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 150);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_rejects_empty_summary(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_NO_SUMMARY);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    /* A clean but empty response is still a completed, billed attempt; it gets
     * a footer without replacing usable history. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_rejects_incomplete_response(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_INCOMPLETE);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(session.n_items == 1);
    EXPECT_STR_EQ(session.items[0].text, "old history");

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_discards_cancelled_summary(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.cancel_requested = 1;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    /* A late cancel wins over a complete summary: old history survives and
     * the completed attempt still receives its footer. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

int main(void)
{
    /* Outside any repository, so recorded sessions neither run git nor depend on the checkout. */
    EXPECT(chdir(t_tempdir()) == 0);
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    test_over_threshold();
    test_should_auto();
    test_applies_summary();
    test_retried_attempt_usage_is_billed();
    test_cancelled_retry_usage_is_billed();
    test_compacts_twice();
    test_keeps_one_session();
    test_tool_call_retries_are_bounded();
    test_resumed_session_floors_at_seed();
    test_branch_summary_records_touched_files();
    test_branch_summary_carries_earlier_file_lists();
    test_branch_summary_without_files_adds_no_sections();
    test_summary_request_truncates_tool_output();
    test_branch_summary_leaves_history_alone();
    test_branch_summary_cancel_still_bills();
    test_branch_summary_requires_a_model();
    test_keeps_recent_turn();
    test_keep_turns_zero_summarizes_all();
    test_keeping_more_turns_than_exist_keeps_one_fewer();
    test_oversized_turn_is_summarized_instead_of_kept();
    test_budget_keeps_a_turn_that_fits();
    test_kept_tail_survives_reload();
    test_preserves_failure();
    test_rejects_empty_summary();
    test_rejects_incomplete_response();
    test_discards_cancelled_summary();
    T_REPORT();
}
