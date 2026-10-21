/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMPACT_H
#define HAX_COMPACT_H

#include "provider.h"

struct agent_session;
struct session_log;
struct transcript_log;

/* Compaction appends a summary seed without deleting the history it summarizes. The newest seed
 * becomes the start of the model-visible context while older items remain in history and logs. */

enum compact_outcome {
    COMPACT_COMPLETE,
    COMPACT_NO_PROVIDER,
    COMPACT_NO_MODEL,
    COMPACT_EMPTY,
    COMPACT_CANCELLED,
    COMPACT_PROVIDER_ERROR,
    COMPACT_NO_SUMMARY,
};

/* `on_event` is for frontend accounting; its return value is ignored. `is_cancelled` is sampled
 * after streaming and may reject an otherwise complete summary. All callbacks are optional. */
struct compact_hooks {
    void *user;
    stream_cb on_event;
    http_tick_cb tick;
    int (*is_cancelled)(void *user);
};

/* All pointers are borrowed for the duration of compact_run. `session` must be non-NULL;
 * `provider`, both logs, and `instructions` may be NULL. */
struct compact_params {
    struct agent_session *session;
    struct provider *provider;
    struct session_log *session_log;
    struct transcript_log *transcript_log;
    const char *instructions;
    struct compact_hooks hooks;
};

struct compact_result {
    enum compact_outcome outcome;
    int attempts;
    char *error_message;
};

/* Summarize the current model-visible context and append a seed on success. Every attempt's
 * reported usage is appended to the session and both logs are flushed. The result owns
 * `error_message`. */
void compact_run(const struct compact_params *params, struct compact_result *result);
void compact_result_destroy(struct compact_result *result);

/* Summarize an abandoned branch and return the owned text, recording every attempt's usage in the
 * live conversation. Nothing is appended and the history is untouched: the caller decides whether
 * to switch. Returns NULL when the provider or model is missing, the request failed, it was
 * cancelled, or it produced nothing. `attempts` may be NULL. */
char *compact_summarize_branch(const struct compact_params *params, const struct item *items,
                               size_t n_items, int *attempts);

/* Wrap a branch summary in the note that frames it as a report about work that is no longer
 * current. Returns an owned string. */
char *compact_branch_seed(const char *summary);

/* A borrowed one-line hint when a provider error reads like a request too large for the context
 * window, or NULL when it says nothing about that. Providers report this as free-form text, so the
 * match is a short list of literal shapes: a miss costs a hint, never an action. */
const char *compact_overflow_hint(const char *message);

/* Read the compact.auto setting. Manual compaction does not consult it. */
int compact_auto_enabled(void);

/* Return whether `context_tokens` is at least `threshold_percent` of `context_limit`. Invalid token
 * counts, a non-positive limit, or a percentage outside 1..100 do not trigger compaction. */
int compact_over_threshold(long context_tokens, long context_limit, int threshold_percent);

/* Apply compact.auto and compact.threshold to the latest context usage. */
int compact_should_auto(long context_tokens, long context_limit);

#endif /* HAX_COMPACT_H */
