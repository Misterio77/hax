# Session files and JSON output

hax records conversations as JSONL — one JSON record per line — and exposes the same records in
two places: session files on disk, and the `hax --json` stream on stdout. This document is the
format reference for both.

The format is versioned (`"version": 2` in new headers) and is a supported read surface for
scripts and orchestrators. Fields may be added over time, so readers must ignore keys they do not
recognize; existing fields keep their meaning within a version. Version 1 files remain readable.
A `navigate` record requires version 2 replay semantics even when appended to a version 1 file;
older binaries must not open such files. Records omit absent values rather than writing `null`.

## Session files

Sessions live under `$XDG_STATE_HOME/hax/sessions/<encoded-cwd>/` (by default
`~/.local/state/hax/sessions/...`), where `<encoded-cwd>` is a readable slug of the working
directory plus a disambiguating hash. Filenames are `<timestamp>_<uuid>.jsonl`; the UUID is the
session id printed by the one-shot banner and accepted by `--resume`. Files are owner-only and
are pruned after `session_retention_days` of inactivity. The same directory holds the working
directory's prompt history, the `history` file behind Up and Ctrl-R; it is not a session and
pruning leaves it alone.

Files are append-only and flushed at each newline, so an in-progress run can be followed with
`tail -f`; nothing already written is ever rewritten, and `/undo` appends a record rather than
truncating. A crash can leave one partial final line; readers should skip lines that do not parse.

## Record types

A line is either a control record, which carries a `type` key, or a conversation item, which
carries `kind` instead. Readers should ignore record types they do not recognize.

### Header (`"type": "session"`)

The first line of a file identifies the session:

| Field | Meaning |
| --- | --- |
| `version` | Record-format version, currently `2`. |
| `hax_version` | The hax build that created the session. |
| `id` | Session UUID, as used by `--resume=ID`. |
| `timestamp` | Creation time, UTC. |
| `cwd` | Directory the session belongs to. |
| `provider`, `model`, `effort`, `preset` | The selection the session started with. |
| `model_label` | Display name; present only when it differs from `model`. |
| `git_branch`, `git_commit`, `git_subject` | HEAD at session start, when in a repository. |
| `forked_from` | Source session id on a `/fork`; the copied items carry `inherited`. |

### Selection (`"type": "selection"`)

A complete provider/model/effort/preset snapshot, written when the selection changes mid-session
(for example a `/model` switch). Absent fields mean "unset", not "unchanged".

### Undo (`"type": "undo"`)

Written by `/undo`: `keep_user_turns` is the number of typed user prompts that remain live. The
items after that many prompts, and the `turn_boundary` before the next one, are no longer part of
the conversation but stay in the file. A reader reconstructing the conversation applies undo
records in order against the items read so far; the count refers to prompts, not lines, so
skipped lines do not shift it.

### Navigation (`"type": "navigate"`)

Written by `/tree`: `version` is `2`, and `position` names an earlier item record by its physical,
one-based JSONL line number. Zero selects an empty conversation. Blank, invalid, and unknown
lines still occupy line numbers; never renumber or remove them. A torn final line is terminated
before reopening the file for appends.

Replay keeps an ancestry edge from each item to the selected item before it was appended. An undo
moves that position to the retained prefix; a navigation selects its recorded position directly.
Subsequent items extend that position rather than the previous end of the file. The final position
selects the live root-to-leaf path; other items remain available as branches and for accounting.
Invalid navigation targets or unsupported versions fail loading rather than mixing branch context.

Each picker row represents a typed prompt and its response. Editing selects the position before
its preceding turn boundary; continuing selects the end of its response, before any later typed
prompt. The selected-path response is preferred when a prompt has several resumed responses;
otherwise the newest recorded response is used. Both operations preserve tool-call/result batches;
the usual incomplete-tool-call repair still applies when loading interrupted records.
Selection records and worked-time totals remain session-wide, just as for undo. Navigating does
not re-execute tools, copy items, generate summaries, or reset the bill.

### User turn (`"type": "user_turn"`)

Written when a user turn finishes: `elapsed_ms` is its wall-clock duration, tool execution
included.

### Items (`"kind": ...`)

Conversation records, in order. `kind` is one of:

| `kind` | Meaning |
| --- | --- |
| `user` | A user message. Synthetic ones carry an `origin`. |
| `assistant` | Assistant message text. |
| `tool_call` | A requested tool invocation: `call_id`, `tool_name`, and `arguments` (raw JSON as a string). |
| `tool_result` | The paired result: `call_id`, `output`, and optionally `images`. |
| `reasoning` | Provider reasoning state (`reasoning_json`/`reasoning_text`) with its `provider`/`model`; `reasoning_field` names the Chat Completions field the text streamed in. |
| `turn_boundary` | Separates provider round-trips ("turns"). The first one precedes the user prompt. |
| `turn_usage` | Usage footer for one round-trip; see below. |

`origin` marks synthetic records: `compact_seed` (a compaction summary), `continuation`,
`interrupted`, `skipped`, `refused`, `summarized`, `task_note` (a background-task report), and
`compaction` (a `turn_usage` footer for a summarization request rather than an agent turn).
Items without `origin` are ordinary typed or streamed content. `inherited: true` marks items
`/fork` copied from the source session: context this conversation reads but did not pay for.

A `turn_usage` item carries `provider`, `model`, and a `usage` object: token counts (`input`,
`output`, `cached`, `cache_write`, and `in_tokens` for uncached input), `elapsed_ms`, a cost
breakdown ending in `cost_total` (`cost_estimated: true` when derived from catalog prices), and
provenance such as `served_model`, `route`, and `response_id`. Unknown values are omitted. One
footer is written per request the provider served: an attempt that died mid-stream and was
retried gets its own footer ahead of the terminal response's.

## The one-shot `--json` stream

`hax --json` runs one-shot (it implies `-p`) and turns stdout into a live JSONL stream. The
stderr banner and stats are omitted — the stream carries them — while errors and warnings still
go to stderr. The stream is the run's session records as they are appended, framed by two extra
records:

1. A `"type": "session"` record identifying the run — the header fields above, minus
   `timestamp`. `id` is omitted when recording is disabled; on a resumed run the fields describe
   this run rather than the original header.
2. The conversation items this run appends. A resumed run's prior items are not replayed.
3. A closing `"type": "result"` record:

| Field | Meaning |
| --- | --- |
| `outcome` | `complete`, `error`, `interrupted`, `paused`, or `max_turns`. |
| `text` | The final assistant text on `complete`, verbatim — unlike plain output, no trailing newline is appended. |
| `error` | Provider error message, when the run failed. |
| `turns` | Provider round-trips used. |
| `elapsed_ms` | Wall-clock duration of the run. |
| `context_tokens` | Context size of the last round-trip, when reported. |
| `cost` | Total spend in USD, with `cost_estimated: true` when catalog-priced. |
| `session_id` | Resume handle for `hax --resume=ID -p`; with no prompt, the resumed run continues where this one stopped. |

`interrupted` and `paused` report a stop signal (SIGINT/SIGTERM and SIGUSR1 respectively; see
[usage.md](./usage.md#cli-modes)). A startup failure can end the stream before any result record,
so also treat process exit as terminal. The exit status matches plain `-p`: 0 on `complete`, 130
on a signal-caused `interrupted`, 1 otherwise. When the stream itself becomes unwritable mid-run
(the consumer exited, the disk filled), hax stops the run and exits nonzero rather than keep
working unobserved.

Example — watch tool calls and cost from a script, no state-directory knowledge needed:

```sh
hax --json "run the tests and fix the first failure" |
    jq 'select(.kind == "tool_call" or .type == "result")'
```

The stream can be tried without a provider, from the repository root:
`HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock/demo.txt hax --json go`.
