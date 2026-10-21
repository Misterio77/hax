# Usage

Run hax from the directory you want it to work in. Sessions, project instructions, file completion,
and the environment shown to the model are all based on the current directory.

## A good everyday workflow

1. Start `hax` in the repository and choose a provider/model with `/provider` if needed.
2. State the goal, constraints, and how the result should be verified. Mention relevant files with
   `@path` rather than pasting large files into the prompt.
3. Let hax inspect the project before prescribing an implementation. Put durable build, test,
   formatting, and safety rules in `AGENTS.md`.
4. Press Esc once when you want to pause after the current step and steer. Use Esc twice only when
   the current model request or command must stop immediately.
5. Review the diff and validation results before accepting the work. hax has no per-command approval
   prompts and is not a security boundary; use OS-level isolation for untrusted work.

## CLI modes

With no arguments, hax starts the interactive REPL:

```sh
hax                              # interactive
hax -p "fix the failing test"    # one-shot
printf "explain x" | hax -p      # one-shot prompt from stdin
hax -c                           # continue the latest session here
hax --resume                     # pick a session for this directory
hax --resume=ID -p "continue"    # resume non-interactively
```

| Option | Purpose |
| --- | --- |
| `-p`, `--print` | Run to completion and print the final assistant message to stdout. |
| `--json` | One-shot (implies `-p`) with stdout as JSON-line conversation records, closed by a `result` record. |
| `-c`, `--continue` | Resume the newest session for the current directory. |
| `--resume[=ID]` | Pick a session, or resume an id/unique prefix. The id form works with `-p`; without a prompt it continues the run. |
| `--no-session` | Do not record this run or add its prompts to persistent recall. |
| `--raw` | Send only the prompt: no system/context sections and no tools. |
| `--bare` | Keep the base prompt, environment, and tools, but omit project/delegation context. |
| `--provider=NAME` | Select a provider for this run. |
| `--model=ID` | Select a model for this run. |
| `--effort=LEVEL` | Select reasoning effort for this run. |
| `--preset=NAME` | Apply a named preset; explicit provider/model/effort flags still win. `hax NAME` is a shortcut for it. |
| `-h`, `--help` | Show current CLI help. |
| `-v`, `--version` | Show the version. |

In one-shot mode, positional arguments are joined with spaces. With no arguments, a non-terminal
stdin becomes the prompt. The final answer goes to stdout; the provider banner, usage stats, errors,
and resume hint go to stderr. This makes `hax -p` safe to pipe without losing diagnostics:

```sh
answer=$(hax -p "summarize the public API")
hax -p "produce JSON only" >result.json 2>run.log
```

For orchestrators and scripts that want to observe a run as it happens — turns, tool calls,
cost — `--json` (which implies `-p`) turns stdout into a JSONL event stream: the run's session
records live as they are appended, closed by a `result` record with the outcome, final text,
usage, and the session id for a follow-up `--resume=ID -p`. The stderr banner and stats are
omitted; errors and warnings still go there. See [sessions.md](./sessions.md) for the record
reference.

```sh
hax --json "fix the failing test" | jq 'select(.kind == "tool_call" or .type == "result")'
```

A picker needs a terminal, so use `--resume=ID` rather than bare `--resume` with `-p`. `--raw` and
`--bare` still record the conversation; combine either with `--no-session` for a disposable run.
A one-shot run is unlimited by default; set `max_turns` to abort after that many provider
round-trips.

A one-shot run responds to signals the way the REPL responds to Esc: SIGUSR1 pauses cleanly at
the next turn boundary (outcome `paused`), and SIGINT (Ctrl-C) or SIGTERM interrupts at once,
saving completed work (outcome `interrupted`, exit status 130); a second signal kills the
process. The session stays resumable: `hax --resume=ID -p` without a prompt continues where the
run stopped, and a new prompt steers it instead.

## REPL commands

Type `/help` for the authoritative live list.

| Command | Purpose |
| --- | --- |
| `/new [preset]` | Start a fresh conversation, optionally with a preset. `/clear` is an alias. |
| `/resume` | Pick a past session for this directory. |
| `/fork [n]` | Create a new session before an earlier prompt; `/fork 0` clones the current tip. |
| `/tree` | Browse conversation turns; Enter continues, e edits a prompt, s summarizes and switches. |
| `/undo [n]` | Roll the conversation back to before an earlier prompt. |
| `/provider [id]` | Switch provider. |
| `/model [id]` | Switch the current provider's model. |
| `/effort [level]` | Set reasoning effort, when the model supports it. |
| `/preset [name]` | Apply a config-defined preset. |
| `/preset-save <name> [tint]` | Save the current provider/model/effort as a preset. |
| `/config [key [value]]` | Inspect settings or change one for this process. |
| `/compact [focus]` | Summarize older context, optionally emphasizing a focus. |
| `/copy` | Copy the latest assistant response. |
| `/tasks [kill <id>... \| kill all]` | List or stop background tasks. |
| `/session` | Show the session's selection and usage totals. |
| `/usage` | Query provider account/subscription usage when supported. |
| `/login [provider]` | Log in to a provider account with a hax-managed token (ChatGPT/codex). |
| `/logout [provider]` | Revoke and remove a hax-managed login. |

Without an argument, `/provider`, `/model`, and `/effort` open pickers, and the provider and model
pickers continue to the settings that depend on them. With an argument they switch directly:
`/provider <id>` starts on the provider's default model, or none until `/model` picks one, and
`/model <id>` keeps the current effort where the new model allows it. Model ids complete from the
provider's model list, fetched in the background; where that isn't available, open the `/model`
picker once.

Use `/tree` to try alternatives in the same session, or `/fork` to create a separate session.
`/undo` removes turns from the selected path, but `/tree` can recover their branches. All requests,
including those on unselected branches, still count toward the session's usage totals.

## Keyboard shortcuts

The editor supports common readline-style movement and history keys. Notable hax bindings:

| Key | Action |
| --- | --- |
| Enter | Submit; at a paused empty prompt, continue without adding a message. |
| Shift-Enter | Insert a newline if the terminal sends LF for it. |
| Up / Down | Recall previous/next prompts. |
| Ctrl-R | Search prompt history. |
| Esc | While working, pause after the current step; at an empty prompt, open `/tree`. |
| Esc Esc | Interrupt the model or running tool immediately. |
| Ctrl-C | Clear the current prompt; twice on an empty prompt quits. |
| Ctrl-D | Quit on an empty prompt. |
| Ctrl-L | Clear the screen and redraw the prompt. |
| Ctrl-G | Edit the prompt in `$EDITOR`. |
| Ctrl-O | Open the rendered conversation in `$PAGER`. |
| Ctrl-T | Open the model-facing transcript in `$PAGER`. |
| Ctrl-V | Paste an image, or clipboard text when no image is available. |
| Tab | Complete a `/` command name or argument. |
| `@` + Tab | Choose a project file with `fzf`. |

Ctrl-O is the best view for reviewing what happened. Ctrl-T includes the system prompt, tool schemas,
model-visible tool calls/results, and post-compaction context; use it when diagnosing why the model
behaved a certain way. Both default to `less -R` when `$PAGER` is unset.

## Project instructions and context

Unless `--raw` is used, hax gives the model:

1. the built-in base prompt, plus configured prompt additions;
2. an Environment section with the working directory, platform, shell, model, Git root, and useful
   command-line tools;
3. discovered `AGENTS.md` instructions and skill descriptions; and
4. tools for reading, editing, writing, and running commands.

`AGENTS.md` is intended for stable instructions, not task-specific prompts. Useful contents include:

- build, test, lint, and formatting commands;
- where important modules live;
- project naming and structure conventions;
- commands or files that must not be changed;
- the expected validation before work is considered complete.

hax loads a global `~/.config/hax/AGENTS.md` first. In a Git worktree, it
then loads files from the repository root down to the current directory, allowing narrower rules to
follow broader ones. Outside Git, it considers only `./AGENTS.md`.

Skills are discovered at `<dir>/.agents/skills/<name>/SKILL.md`, searched in this order:

1. the current directory, then each parent up to the repository root (outside Git, only the current
   directory);
2. `~/.config/hax/skills/`, for skills meant only for hax;
3. `~/.agents/skills/`, the cross-agent location other tools install into and read from.

The first match for a given skill name wins, so a nearer directory shadows a wider one. hax only
reads `~/.agents/skills` and never writes to it. A skill should explain when and how to use a CLI
or repeatable workflow; project policy usually belongs in `AGENTS.md` instead.

Use `--bare` for a task that needs tools but should not receive project instructions, skills, or
subagent guidance. Use `--raw` only when you want a plain model chat with no tools. Individual
context sections can also be disabled through [configuration](./configuration.md).

## Files and images

Mention a file as ordinary prompt text, for example `@src/main.c`. With `fzf` installed, type an
`@`-prefixed fragment and press Tab to search project files. In a Git repository the picker includes
tracked and untracked-but-not-ignored files. `@../`, `@~/`, and absolute prefixes search from the
named directory. Selecting a file inserts its path; the model reads it only if needed.

Ctrl-V copies a clipboard image to a temporary file and inserts its path. On Linux and the BSDs
this requires `wl-paste` (Wayland) or `xclip` (X11); macOS works without an extra utility. If no
image is present, Ctrl-V pastes text. Image understanding also depends on the selected
model/provider; hax detects support when metadata is available, and `image_input` can override
detection.

## Sessions and prompt history

Non-empty conversations are recorded as JSONL session files under:

```text
~/.local/state/hax/sessions/<encoded-cwd>/
```

Sessions are scoped to the current directory. `-c`, `--resume`, and `/resume` therefore list the
sessions for where hax is running, not every repository. Sessions inactive for 30 days are removed
by default; set `session_retention_days` to another value or `0` to keep them indefinitely. The
file format is documented in [sessions.md](./sessions.md) and is safe to read from scripts.

Prompt history (Up, Ctrl-R) is scoped the same way: each directory keeps its own `history` file
beside its session files, so recall stays within the project.

Resuming restores the provider, model, effort, and preset last used by that conversation. A CLI
selection flag deliberately overrides the restored choice:

```sh
hax --resume=ID --model=other-model
hax --resume=ID --preset=review
```

The new selection is recorded in the resumed session. If its old provider or preset is unavailable,
hax reports the problem rather than silently choosing another backend.

`--no-session` (or `no_session`) prevents new session and prompt-history writes. It does not hide
existing sessions or disable prompt recall.

### Conversation trees

Run `/tree`, or press Escape at an empty prompt, to browse conversation turns
with their actual parent/child connectors. Drafts are never replaced by the shortcut.
Only the turn containing your current conversation position is marked:

- **Enter** restores context through the selected turn's response, excluding later prompts.
  Continue with a new message without rerunning that turn.
- **e** restores context before the selected prompt and puts its original text in the editor.
  Edit and send it to create another branch without deleting the original continuation.
- **s** summarizes the branch you are leaving and continues at the selected turn, so what it
  learned is still available. One summarization request runs before the switch; if it fails or you
  interrupt it, the conversation stays where it was. A branch longer than the context window is
  summarized from its newest turns that fit, since a request that does not fit teaches nothing.
- Type to filter. **/** starts explicit search, including queries beginning with `e` or `s`.
  **Alt-E** and **Alt-S** act while filtering; **Ctrl-U** clears the search. Escape cancels the
  picker.

Rows represent turns, not individual tool calls. Intermediate turns are valid continuation points,
not only branch endpoints. No synthetic branch-tip rows or assistant previews are shown.

The selected position is saved immediately, even if you quit without sending another prompt.
History left by `/undo` is also available, including in older session files. `/fork` still creates
an independent file; `/tree` shows only the history in the current file.

Switching prints a short marker and leaves terminal scrollback alone. Old output can therefore
belong to a branch the model no longer sees: Ctrl-O shows the selected conversation, and Ctrl-T
shows its model-facing context. Compaction summaries follow their own branch, not its siblings.
Nothing is rolled back: file edits and command side effects are not undone, and neither provider,
model, effort, nor preset changes. A branch summary written by **s** is appended where you land and
says so — it reports what that branch explored, learned, and left running, and warns that none of it
is the current state. It also ends with the files the branch read and changed, taken from its tool
calls rather than from the model's recollection, and keeps lists carried by any earlier branch
summary it summarizes.

Navigation requires session recording and no running or uncollected background tasks. Collect
outstanding tasks before switching, or stop them with `/tasks kill` and let the agent collect their
final reports. Keep branched files away from older hax binaries: those do not understand the saved
branch cursor and can reconstruct the wrong context.

## Pausing and steering

The first Esc requests a clean pause. Work already in flight normally finishes, including tool calls,
and hax returns to a prompt at the next model-turn boundary. At that prompt:

- press Enter on an empty line to continue;
- type a message to add new direction before the next model request; or
- use commands such as `/model`, `/compact`, or `/session` before continuing.

Esc again requests an immediate interrupt. Answer text already shown and completed tool activity
remain visible, but unfinished reasoning is discarded so it cannot confuse the next request. If the
model had not yet produced answer text or a tool call, the conversation is left unchanged; press
Enter on an empty line to retry. `max_turns` provides the same kind of periodic check-in after a
configured number of model round-trips. In one-shot mode, SIGUSR1 and SIGINT request the same
pause and interrupt from outside; see [CLI modes](#cli-modes).

Resuming an interrupted conversation (`--resume`, `-c`, `/resume`) offers the same empty-Enter
continue at the first prompt.

## Background tasks and delegation

Long-running commands may detach into managed background tasks instead of being killed at the bash
timeout. `/tasks` shows them. The model can wait for a task, collect its output, or stop it; completed
tasks are reported without polling. Tasks belong to the conversation and are stopped when you leave
it. In one-shot mode, tasks not awaited before the final answer are stopped.

Set `no_tasks` to disable this behavior and make command timeouts kill the command. Related timeout
and concurrency settings are in [configuration](./configuration.md#tools-and-transport).

When explicitly asked to use subagents, hax can run additional `hax -p` processes as background
tasks. They inherit the current provider/model/effort by default. Presets with a `description` are
advertised to the model as named roles; favorite-only presets remain private picker shortcuts. Keep
delegation for independent work that benefits from parallel context — ordinary small tasks are
faster and cheaper in one conversation.

## Context, compaction, and usage

After a turn, hax shows elapsed time, current context use, and spend when the provider or model
metadata can supply it. A `~` marks estimated cost. `/session` shows the conversation's totals,
including undone user turns and retried requests, with a token row per model when models were
switched; a resumed session shows the same totals. `/usage` asks the provider for account-level
usage when supported. One-shot runs put equivalent stats on stderr.

Automatic compaction summarizes the older part of the conversation near 85% of a known context
window. Use `/compact` earlier when the conversation has accumulated obsolete exploration,
optionally naming what the summary must preserve. Manual compaction works even when the model's
context limit is unknown.

A compaction keeps the newest user turns word for word after the summary, so the model continues
from exactly the state the conversation was last in instead of a paraphrase of it: one turn by
default, `compact.keep_turns` to keep more, and `0` to summarize everything. Retention never blocks
compaction. A conversation with fewer turns than that keeps as many as leave something to summarize,
and when the context window is known the kept tail may take at most a quarter of it: turns are
dropped from the tail, whole, until it fits. A turn too large to keep — one big file dump — is
therefore summarized rather than left to overflow the window. A summarization request reads only the
first 2000 characters of each tool result, marked where it was cut: what continues needs to know
what was run, not to re-read every byte it printed.

When a provider rejects a request because it no longer fits, hax says so and names the remedy:
`/compact` summarizes the conversation, `/new` starts a fresh one. A prompt that is itself larger
than the window cannot be compacted away, so those need the second.

For local servers, configure a context window large enough for the system prompt, project context,
conversation, and response. A small server-side context often appears as truncated answers rather
than a hax error; see the provider-specific notes in [providers.md](./providers.md).
