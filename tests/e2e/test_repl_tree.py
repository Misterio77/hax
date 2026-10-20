#!/usr/bin/env python3
"""Tree navigation edits prompts, preserves sibling branches, and changes pager context without
replaying old output into native scrollback. A cursor-only switch survives restart."""

import os
import shlex

import harness


def test_tree_edit_return_and_resume():
    home, workdir = harness.make_home()
    pager = home / "pager.txt"
    env = {"HAX_NO_SESSION": "0", "PAGER": f"cat > {shlex.quote(str(pager))}"}
    term = harness.Terminal(
        """
        text FIRST_REPLY
        end-turn
        text ORIGINAL_REPLY
        end-turn
        text ALTERNATE_REPLY
        end-turn
        """,
        home=home,
        workdir=workdir,
        extra_env=env,
        size=(110, 44),
    )
    term.wait_for("❯")
    term.type("first prompt")
    term.send("Enter")
    term.wait_for("FIRST_REPLY")
    term.type("original prompt")
    term.send("Enter")
    term.wait_for("ORIGINAL_REPLY")

    term.type("/tree")
    term.send("Enter")
    term.wait_for("session tree · enter")
    term.type("original prompt")
    term.send("M-e")
    term.wait_for("❯ original prompt")
    harness.expect(
        term.screen().count("ORIGINAL_REPLY") == 1,
        "branch switching does not replay the abandoned reply into scrollback",
    )
    term.send("C-u")
    term.type("alternate prompt")
    term.send("Enter")
    term.wait_for("ALTERNATE_REPLY")

    term.type("/tree")
    term.send("Enter")
    term.wait_for("session tree · enter")
    harness.expect("original prompt" in term.screen(), "the original branch remains selectable")
    harness.expect("alternate prompt" in term.screen(), "the new branch appears in the tree")
    term.send("Up", "Enter")
    term.wait_for("switched to after: original prompt")
    term.send("C-t")
    term.wait_for("❯")
    context = pager.read_text()
    harness.expect("ORIGINAL_REPLY" in context, "the restored branch is model-visible")
    harness.expect("ALTERNATE_REPLY" not in context, "the sibling branch is not model-visible")

    term.send("C-d")
    harness.expect(term.wait_exit() == 0, "exit after switching without sending another prompt")
    resumed = harness.Terminal(
        "end-turn", args=["-c"], home=home, workdir=workdir, extra_env=env, size=(110, 44)
    )
    resumed.wait_for("ORIGINAL_REPLY")
    resumed.send("C-o")
    resumed.wait_for("❯")
    history = pager.read_text()
    harness.expect("ORIGINAL_REPLY" in history, "resume keeps the selected branch")
    harness.expect("ALTERNATE_REPLY" not in history, "history pager shows only the active branch")

    resumed.type("/tree")
    resumed.send("Enter")
    resumed.wait_for("session tree · enter")
    screen = resumed.screen()
    harness.expect("continue at branch tip" not in screen, "no synthetic tip rows")
    harness.expect(screen.count("✓ current") == 1, "only the current turn is marked")
    harness.expect("├─ original prompt" in screen, "the earlier sibling uses a fork connector")
    harness.expect("└─ alternate prompt" in screen, "the last sibling closes the fork")
    resumed.send("e")
    resumed.wait_for("❯ original prompt")

    resumed.send("C-u")
    resumed.type("/tree")
    resumed.send("Enter")
    resumed.wait_for("session tree · enter")
    resumed.type("/e")
    resumed.wait_for("alternate prompt")
    harness.expect("session tree · enter" in resumed.screen(), "search can start with e")
    resumed.send("M-e")
    resumed.wait_for("❯ alternate prompt")


def test_escape_empty_prompt_and_escape_sequences():
    term = harness.Terminal(
        "text READY_REPLY\nend-turn", extra_env={"HAX_NO_SESSION": "0"}, size=(110, 32)
    )
    term.wait_for("❯")
    term.type("seed prompt")
    term.send("Enter")
    term.wait_for("READY_REPLY")
    term.wait_for("❯")
    term.type("alpha beta")
    # A harmless control key terminates any still-buffered escape sequence. The visible edit
    # confirms processing without relying on timing sleeps before the next sequence.
    term.send("Escape", "C-e")
    term.type("!")
    term.wait_for("❯ alpha beta!")
    harness.expect("session tree · enter" not in term.screen(), "drafts block the shortcut")
    term.send("BSpace", "Escape", "Left")
    term.type("X")
    term.wait_for("❯ alpha Xbeta")
    term.send("C-e", "C-u")
    term.send("Escape", "Up")
    term.wait_for("❯ seed prompt")
    harness.expect("session tree · enter" not in term.screen(), "prefixed arrow keys are preserved")
    term.send("C-e", "C-u")
    term.send("Escape")
    term.wait_for("session tree · enter")
    harness.expect("/tree" not in term.screen(), "Escape does not submit a synthetic command")
    term.send("C-g")
    term.type("cancelled draft")
    term.wait_for("❯ cancelled draft")
    harness.expect("session tree · enter" not in term.screen(), "cancelling returns to the editor")
    term.send("C-u", "Escape")
    term.wait_for("session tree · enter")
    term.send("e")
    term.wait_for("❯ seed prompt")


def test_tree_requires_recording():
    term = harness.Terminal("text hello\nend-turn")
    term.wait_for("❯")
    term.type("/tree")
    term.send("Enter")
    term.wait_for("/tree needs a recorded conversation")


def test_tree_blocks_background_tasks():
    home, workdir = harness.make_home()
    os.mkfifo(workdir / "wait.fifo")
    term = harness.Terminal(
        '\n'.join([
            'tool bash {"command":"cat wait.fifo","background":true,"name":"hold"}',
            'end-turn',
            'text TASK_RUNNING',
            'end-turn',
        ]),
        home=home,
        workdir=workdir,
        extra_env={"HAX_NO_SESSION": "0"},
    )
    term.wait_for("❯")
    term.type("start background work")
    term.send("Enter")
    term.wait_for("TASK_RUNNING")
    term.type("/tree")
    term.send("Enter")
    term.wait_for("collect or stop background tasks before switching branches")


harness.main(globals())
