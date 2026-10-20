/* SPDX-License-Identifier: MIT */
#include "compact.h"

#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_usage.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "transcript.h"
#include "turn.h"
#include "xalloc.h"

/* Fixed sections and exact identifiers make the seed useful across model changes. The leading
 * output-only instruction prevents weaker models from continuing the task instead. */
static const char COMPACT_PROMPT[] =
    "Summarize the conversation so far into a structured context checkpoint that lets the "
    "work continue without access to the full history above.\n"
    "\n"
    "CRITICAL: Respond with the summary text ONLY. Do not call any tools and do not continue "
    "the task — your entire reply is the summary.\n"
    "\n"
    "Use this exact Markdown structure, keeping every section even when empty:\n"
    "\n"
    "## Goal\n"
    "- [what the user is ultimately trying to accomplish]\n"
    "\n"
    "## Constraints & Preferences\n"
    "- [explicit user constraints, preferences, or requirements, or \"(none)\"]\n"
    "\n"
    "## Progress\n"
    "### Done\n"
    "- [completed work, or \"(none)\"]\n"
    "### In Progress\n"
    "- [work underway right now, or \"(none)\"]\n"
    "### Blocked\n"
    "- [blockers, or \"(none)\"]\n"
    "\n"
    "## Key Decisions\n"
    "- [decision: brief rationale, or \"(none)\"]\n"
    "\n"
    "## Files\n"
    "- [path: why it matters / what changed, or \"(none)\"]\n"
    "\n"
    "## Next Steps\n"
    "- [ordered next actions, or \"(none)\"]\n"
    "\n"
    "## Critical Context\n"
    "- [important technical facts, error strings, identifiers, commands, open questions, or "
    "\"(none)\"]\n"
    "\n"
    "Rules:\n"
    "- Be terse: bullets, not prose paragraphs.\n"
    "- Preserve exact file paths, function names, commands, and error strings.\n"
    "- Do not mention that a summary was produced or that context was compacted.";

static const char COMPACT_SEED_PREAMBLE[] =
    "The earlier part of this conversation was condensed to free up context. The summary "
    "below captures everything that happened before this point — treat it as established "
    "context and continue the work from here.";

/* A branch summary is read at a point in the conversation that never saw the branch run, so its
 * sections separate what was learned from what is still standing outside the conversation. */
static const char BRANCH_PROMPT[] =
    "Summarize the conversation branch below. The user explored it and then went back to an "
    "earlier point, so this is what remains of that work when the conversation continues "
    "elsewhere.\n"
    "\n"
    "CRITICAL: Respond with the summary text ONLY. Do not call any tools and do not continue "
    "the task — your entire reply is the summary.\n"
    "\n"
    "Use this exact Markdown structure, keeping every section even when empty:\n"
    "\n"
    "## Explored\n"
    "- [what this branch set out to do, or \"(none)\"]\n"
    "\n"
    "## Learned\n"
    "- [facts established by reading, running, or testing, with the file, command, or error "
    "string that established them, or \"(none)\"]\n"
    "\n"
    "## Tried\n"
    "- [changes and approaches that ran and their outcome, saying so when the outcome is "
    "unverified, or \"(none)\"]\n"
    "\n"
    "## Left Behind\n"
    "- [anything this branch changed outside the conversation — edited files, created branches, "
    "running processes — or \"(none)\"]\n"
    "\n"
    "## Worth Keeping\n"
    "- [conclusions or fixes worth carrying forward, marking anything unverified, or "
    "\"(none)\"]\n"
    "\n"
    "Rules:\n"
    "- Be terse: bullets, not prose paragraphs.\n"
    "- Use the past tense: the branch is not the current state of the work.\n"
    "- Preserve exact file paths, function names, commands, and error strings.\n"
    "- Do not mention that a summary was produced or that a branch was abandoned.";

static const char BRANCH_SEED_PREAMBLE[] =
    "Before continuing from here, the user explored a different branch of this conversation and "
    "then returned. The summary below is what that branch did and learned. It describes work that "
    "is no longer the current state — files it changed may have changed since, and nothing it "
    "describes has been re-checked — so verify before relying on it.";

int compact_auto_enabled(void)
{
    return config_bool("compact.auto");
}

int compact_over_threshold(long context_tokens, long context_limit, int threshold_percent)
{
    if (context_tokens < 0 || context_limit <= 0 || threshold_percent < 1 ||
        threshold_percent > 100)
        return 0;

    long threshold_tokens = context_limit / 100 * threshold_percent;
    long scaled_remainder = context_limit % 100 * threshold_percent;
    threshold_tokens += scaled_remainder / 100 + (scaled_remainder % 100 != 0);
    return context_tokens >= threshold_tokens;
}

int compact_should_auto(long context_tokens, long context_limit)
{
    if (!compact_auto_enabled())
        return 0;
    return compact_over_threshold(context_tokens, context_limit, config_int("compact.threshold"));
}

static char *build_summary_prompt(const char *instructions)
{
    if (instructions && *instructions)
        return xasprintf("%s\n\nAdditional focus for this summary:\n%s", COMPACT_PROMPT,
                         instructions);
    return xstrdup(COMPACT_PROMPT);
}

static char *build_summary_seed(const char *summary)
{
    return xasprintf("%s\n\n%s", COMPACT_SEED_PREAMBLE, summary);
}

#define COMPACT_MAX_ATTEMPTS 4

struct compact_sink {
    struct turn turn;
    struct attempt_log attempt_log;
    const struct compact_hooks *hooks;
    char *error_message;
};

struct summary_request {
    struct context base_context;
    struct item *items;
    size_t count;
    size_t capacity;
    size_t borrowed_count;
};

static int compact_sink_on_event(const struct stream_event *event, void *user)
{
    struct compact_sink *sink = user;
    const struct stream_usage *usage = NULL;
    const struct stream_response *response = NULL;

    if (event->kind == EV_DONE) {
        usage = &event->u.done.usage;
        response = &event->u.done.response;
    } else if (event->kind == EV_ERROR) {
        if (!sink->error_message)
            sink->error_message =
                xstrdup(event->u.error.message ? event->u.error.message : "stream failed");
        usage = event->u.error.usage;
        response = event->u.error.response;
    } else if (event->kind == EV_RETRY) {
        attempt_log_retry(&sink->attempt_log, event->u.retry.usage, event->u.retry.delay_ms);
    }
    if (usage)
        attempt_log_record(&sink->attempt_log, usage, response);
    if (sink->hooks->on_event)
        sink->hooks->on_event(event, sink->hooks->user);
    turn_consume(&sink->turn, event);
    return 0;
}

/* Ownership of `item` transfers to `request`. */
static void summary_request_append(struct summary_request *request, struct item item)
{
    if (request->count == request->capacity) {
        request->capacity = request->capacity ? request->capacity * 2 : 16;
        request->items = xrealloc(request->items, request->capacity * sizeof(*request->items));
    }
    request->items[request->count++] = item;
}

/* The request borrows the window it summarizes and appends its instructions as the final user
 * message. Everything else — system prompt, tools, effort — comes from the live session, so a
 * summarization looks like an ordinary turn to the provider. */
static void summary_request_init(struct summary_request *request,
                                 const struct compact_params *params, const struct item *items,
                                 size_t n_items, char *prompt)
{
    memset(request, 0, sizeof(*request));
    request->base_context = agent_session_context(params->session);
    request->base_context.items = items;
    request->base_context.n_items = n_items;
    request->base_context.image_input =
        model_meta_image_input(params->provider, params->session->model);
    request->base_context.session_id = session_log_id(params->session_log);
    request->borrowed_count = n_items;
    request->capacity = n_items + 1;
    request->items = xmalloc(request->capacity * sizeof(*request->items));
    if (n_items > 0)
        memcpy(request->items, items, n_items * sizeof(*request->items));
    request->count = n_items;
    summary_request_append(request, (struct item){.kind = ITEM_USER_MESSAGE, .text = prompt});
}

static void summary_request_destroy(struct summary_request *request)
{
    for (size_t i = request->borrowed_count; i < request->count; i++)
        item_free(&request->items[i]);
    free(request->items);
}

static int response_has_tool_call(const struct item *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (items[i].kind == ITEM_TOOL_CALL)
            return 1;
    return 0;
}

static char *extract_summary(const struct item *items, size_t count)
{
    const char *summary = NULL;

    for (size_t i = 0; i < count; i++)
        if (items[i].kind == ITEM_ASSISTANT_MESSAGE && items[i].text && *items[i].text)
            summary = items[i].text;
    return summary ? xstrdup(summary) : NULL;
}

static void free_items(struct item *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        item_free(&items[i]);
    free(items);
}

static const char TOOL_CALL_REJECTION[] =
    "[rejected] Tool calls are disabled while summarizing. Respond with the summary text "
    "only — do not call any tools.";

/* The request takes ownership of every response item. */
static void append_rejected_response(struct summary_request *request, struct item *items,
                                     size_t count)
{
    size_t response_start = request->count;
    for (size_t i = 0; i < count; i++)
        summary_request_append(request, items[i]);
    free(items);

    size_t response_end = request->count;
    for (size_t i = response_start; i < response_end; i++) {
        if (request->items[i].kind != ITEM_TOOL_CALL)
            continue;
        char *call_id = request->items[i].call_id ? xstrdup(request->items[i].call_id) : NULL;
        summary_request_append(request, (struct item){
                                            .kind = ITEM_TOOL_RESULT,
                                            .call_id = call_id,
                                            .output = xstrdup(TOOL_CALL_REJECTION),
                                        });
    }
}

/* The request owns `prompt` once it is built. */
static char *generate_summary(const struct compact_params *params, struct compact_sink *sink,
                              int *attempt_count, const struct item *items, size_t n_items,
                              char *prompt)
{
    struct summary_request request;
    summary_request_init(&request, params, items, n_items, prompt);
    char *summary = NULL;

    /* Advertising the normal tools preserves the provider's cached request prefix. Calls are
     * rejected rather than executed because compaction must not mutate task state. */
    for (int attempt = 0; attempt < COMPACT_MAX_ATTEMPTS; attempt++) {
        struct context context = request.base_context;
        context.items = request.items;
        context.n_items = request.count;
        (*attempt_count)++;
        params->provider->stream(params->provider, &context, params->session->model,
                                 compact_sink_on_event, sink, params->hooks.tick,
                                 params->hooks.user);

        if (sink->turn.state != TURN_DONE)
            break;

        size_t response_count;
        struct item *response = turn_take_items(&sink->turn, &response_count);
        if (!response_has_tool_call(response, response_count)) {
            summary = extract_summary(response, response_count);
            free_items(response, response_count);
            break;
        }
        if (attempt == COMPACT_MAX_ATTEMPTS - 1) {
            free_items(response, response_count);
            break;
        }

        append_rejected_response(&request, response, response_count);
        turn_reset(&sink->turn);
    }

    summary_request_destroy(&request);
    return summary;
}

/* The boundary prevents the seed from rendering as part of the preceding assistant turn. A kept
 * tail follows the seed, and opening it with its own turn boundary already separates the two. */
static void append_summary_seed(struct agent_session *session, const char *summary, size_t keep)
{
    if (keep == 0)
        agent_session_add_boundary(session);
    agent_session_append(session, (struct item){
                                      .kind = ITEM_USER_MESSAGE,
                                      .text = build_summary_seed(summary),
                                      .origin = ITEM_ORIGIN_COMPACT_SEED,
                                      .keep_count = keep,
                                  });
}

/* Records can only be appended, so the seed is written after the turns it keeps and moved in front
 * of them in memory. The replayed path does the same, which keeps a live session and a resumed one
 * ordered alike. */
static void splice_seed_before_tail(struct agent_session *session, size_t keep_from,
                                    size_t seed_index)
{
    if (seed_index <= keep_from)
        return;
    struct item seed = session->items[seed_index];
    memmove(&session->items[keep_from + 1], &session->items[keep_from],
            (seed_index - keep_from) * sizeof(*session->items));
    session->items[keep_from] = seed;
}

/* A tail that fills the window would leave no room for the summary or the next turn, so a known
 * window caps it at a quarter of itself. */
#define COMPACT_KEEP_TURNS_BUDGET_DIVISOR 4

/* The kept tail is measured in whole user turns rather than tokens: a count never splits a tool
 * batch, and the newest turn is the state a continued conversation actually resumes from. A turn
 * too large for the budget is dropped from the tail rather than split, so an oversized turn
 * degrades to summarizing everything instead of leaving a window the provider will reject. */
static size_t compact_keep_floor(const struct compact_params *params)
{
    struct agent_session *session = params->session;
    int keep_turns = config_int("compact.keep_turns");
    if (keep_turns <= 0)
        return session->n_items;
    long limit = model_meta_context(params->provider, session->model);
    long budget = limit > 0 ? limit / COMPACT_KEEP_TURNS_BUDGET_DIVISOR : -1;

    for (size_t turns = (size_t)keep_turns; turns > 0; turns--) {
        size_t cut = items_keep_tail_cut(session->items, session->n_items, turns);
        /* A zero cut means there are no more turns than requested, so nothing older would be left
         * to summarize; asking for fewer is the only way to keep something. */
        if (cut == 0)
            continue;
        if (budget < 0 ||
            items_token_estimate(session->items + cut, session->n_items - cut) <= budget)
            return cut;
    }
    /* Retention never turns compaction off: when no tail fits, everything is summarized. */
    return session->n_items;
}

static void flush_compaction_logs(const struct compact_params *params)
{
    transcript_log_append(params->transcript_log, params->session->items, params->session->n_items);
    session_log_append(params->session_log, params->session->items, params->session->n_items);
}

static void append_attempt_usage(const struct compact_params *params, const struct attempt_log *log,
                                 size_t first, size_t end)
{
    for (size_t i = first; i < end; i++) {
        struct stream_response response = {
            .id = log->attempts[i].response_id,
            .model = log->attempts[i].served_model,
            .route = log->attempts[i].route,
        };
        agent_session_add_turn_usage(params->session, params->provider, &log->attempts[i].usage,
                                     log->attempts[i].elapsed_ms, &response,
                                     ITEM_ORIGIN_COMPACTION);
    }
}

void compact_run(const struct compact_params *params, struct compact_result *result)
{
    memset(result, 0, sizeof(*result));
    if (!params->provider) {
        result->outcome = COMPACT_NO_PROVIDER;
        return;
    }
    if (!params->session->model || !*params->session->model) {
        result->outcome = COMPACT_NO_MODEL;
        return;
    }
    if (params->session->n_items == 0) {
        result->outcome = COMPACT_EMPTY;
        return;
    }

    /* A resumed session may compact before its first ordinary turn, so resolve effort here too. */
    if (agent_session_resync_effort(params->session, params->provider, NULL))
        session_log_set_meta(params->session_log, agent_provider_log_name(params->provider),
                             params->session->model, params->session->model_label,
                             params->session->effort, config_str("preset"));

    /* An empty window — a seed with nothing after it — has nothing to summarize. */
    size_t floor = items_context_floor(params->session->items, params->session->n_items);
    size_t keep_from = compact_keep_floor(params);
    if (keep_from <= floor) {
        result->outcome = COMPACT_EMPTY;
        return;
    }

    struct compact_sink sink = {.hooks = &params->hooks};
    turn_init(&sink.turn);
    attempt_log_init(&sink.attempt_log);
    /* Only the prefix is summarized: whatever the seed keeps is still in the window verbatim, so
     * summarizing it would pay twice and could drop detail the tail states plainly. */
    char *summary =
        generate_summary(params, &sink, &result->attempts, params->session->items + floor,
                         keep_from - floor, build_summary_prompt(params->instructions));
    int cancelled = params->hooks.is_cancelled && params->hooks.is_cancelled(params->hooks.user);
    if (cancelled) {
        free(summary);
        summary = NULL;
    }
    turn_reset(&sink.turn);
    result->error_message = sink.error_message;

    if (!summary || !*summary) {
        append_attempt_usage(params, &sink.attempt_log, 0, sink.attempt_log.count);
        attempt_log_free(&sink.attempt_log);
        flush_compaction_logs(params);
        free(summary);
        if (cancelled)
            result->outcome = COMPACT_CANCELLED;
        else if (result->error_message)
            result->outcome = COMPACT_PROVIDER_ERROR;
        else
            result->outcome = COMPACT_NO_SUMMARY;
        return;
    }

    /* Rejected attempts are part of the summarized prefix; accepted usage follows the seed. */
    size_t accepted_usage_index = sink.attempt_log.count ? sink.attempt_log.count - 1 : 0;
    size_t keep = params->session->n_items - keep_from;
    size_t seed_index = params->session->n_items;
    append_attempt_usage(params, &sink.attempt_log, 0, accepted_usage_index);
    append_summary_seed(params->session, summary, keep);
    free(summary);
    append_attempt_usage(params, &sink.attempt_log, accepted_usage_index, sink.attempt_log.count);
    attempt_log_free(&sink.attempt_log);
    /* Records keep append order; only the live array reads summary-first. */
    flush_compaction_logs(params);
    splice_seed_before_tail(params->session, keep_from, seed_index);
    result->outcome = COMPACT_COMPLETE;
}

/* Everything the request learns is billed to the branch being left, in this conversation: its
 * footers land before the navigation that abandons them. */
char *compact_summarize_branch(const struct compact_params *params, const struct item *items,
                               size_t n_items, int *attempts)
{
    char *summary = NULL;
    if (attempts)
        *attempts = 0;
    if (!params->provider || !params->session->model || !*params->session->model || n_items == 0)
        return NULL;

    struct compact_sink sink = {.hooks = &params->hooks};
    turn_init(&sink.turn);
    attempt_log_init(&sink.attempt_log);
    int attempt_count = 0;
    summary =
        generate_summary(params, &sink, &attempt_count, items, n_items, xstrdup(BRANCH_PROMPT));
    if (attempts)
        *attempts = attempt_count;
    if (params->hooks.is_cancelled && params->hooks.is_cancelled(params->hooks.user)) {
        free(summary);
        summary = NULL;
    }
    turn_reset(&sink.turn);
    free(sink.error_message);
    append_attempt_usage(params, &sink.attempt_log, 0, sink.attempt_log.count);
    attempt_log_free(&sink.attempt_log);
    flush_compaction_logs(params);
    if (!summary || !*summary) {
        free(summary);
        return NULL;
    }
    return summary;
}

char *compact_branch_seed(const char *summary)
{
    return xasprintf("%s\n\n%s", BRANCH_SEED_PREAMBLE, summary);
}

void compact_result_destroy(struct compact_result *result)
{
    free(result->error_message);
    result->error_message = NULL;
}
