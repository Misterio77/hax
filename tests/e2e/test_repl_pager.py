#!/usr/bin/env python3
"""Active-turn pagers own terminal input without stopping tools or model execution."""

import json
import os
import shlex
import shutil
import time

import harness


def start(script: str, pager: str, fifos: tuple[str, ...] = (), capture: bool = True):
    home, workdir = harness.make_home()
    for name in fifos:
        os.mkfifo(workdir / name)
    snapshot = home / "snapshot.txt"
    command = f"cat > {shlex.quote(str(snapshot))}; {pager}" if capture else pager
    term = harness.Terminal(
        script, home=home, workdir=workdir, extra_env={"PAGER": command}
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    return term, snapshot


def test_transcript_while_command_runs():
    command = (
        "cat tool-fifo; printf 'QUEUED_%s\\n' TOOL_OUTPUT; "
        "printf 'COMMAND_COMPLETED\\n' > pager-fifo; : "
        + "long_argument_" * 30
        + "FULL_COMMAND_END"
    )
    term, snapshot = start(
        f"""
        tool bash {json.dumps({'command': command})}
        end-turn
        tool bash {{"command":"printf 'MODEL_CONTINUED\\n' > model-fifo"}}
        end-turn
        text Final response.
        end-turn
        """,
        "printf 'PAGER_READY\\n'; cat pager-fifo; cat model-fifo; read reply </dev/tty",
        ("tool-fifo", "pager-fifo", "model-fifo"),
    )
    term.wait_for("[bash] cat tool-fifo")
    term.send("C-t")
    term.wait_for("PAGER_READY")
    harness.expect("FULL_COMMAND_END" in snapshot.read_text(), "full running command in snapshot")
    term.write_fifo(term.workdir / "tool-fifo", "")
    term.wait_for("COMMAND_COMPLETED")
    term.wait_for("MODEL_CONTINUED")
    harness.expect("QUEUED_TOOL_OUTPUT" not in term.screen(), "tool output stays spooled")
    harness.expect("Final response." not in term.screen(), "model output stays spooled")
    term.send("Enter")
    term.wait_for("Final response.")
    term.wait_for("❯")


def test_history_while_model_streams():
    term, snapshot = start(
        """
        text Earlier response.
        delay 30000
        text Still working.
        end-turn
        """,
        "printf 'HISTORY_READY\\n'; read reply </dev/tty",
    )
    term.wait_for("Earlier response.")
    term.send("C-o")
    term.wait_for("HISTORY_READY")
    harness.expect("go" in snapshot.read_text(), "history snapshot includes the submitted prompt")
    term.send("Enter")
    # The mock stream is silent, so wait for the foreground tick to reap the pager.
    time.sleep(0.2)
    term.send("Escape", "Escape")
    term.wait_for("[interrupted]")
    term.wait_for("❯")


def test_pager_exit_restores_command_interrupts():
    term, _ = start(
        """
        tool bash {"command":"cat first-fifo; printf 'BACK_FROM_PAGER\\n'; cat second-fifo"}
        end-turn
        """,
        "printf 'PAGER_READY\\n'; read reply </dev/tty",
        ("first-fifo", "second-fifo"),
    )
    term.wait_for("[bash] cat first-fifo")
    term.send("C-t")
    term.wait_for("PAGER_READY")
    term.send("Enter")
    term.write_fifo(term.workdir / "first-fifo", "")
    term.wait_for("BACK_FROM_PAGER")
    term.send("Escape", "Escape")
    term.wait_for("[interrupted]")
    term.wait_for("❯")


def test_early_pager_exit():
    term, _ = start(
        """
        tool bash {"command":"cat tool-fifo; printf 'AFTER_EARLY_EXIT\\n'"}
        end-turn
        text Done.
        end-turn
        """,
        "printf 'EARLY_EXIT\\n'; exit 1",
        ("tool-fifo",),
    )
    term.wait_for("[bash] cat tool-fifo")
    term.send("C-t")
    term.wait_for("EARLY_EXIT")
    term.write_fifo(term.workdir / "tool-fifo", "")
    term.wait_for("AFTER_EARLY_EXIT")
    term.wait_for("Done.")
    term.wait_for("❯")


def test_less_restores_terminal():
    if shutil.which("less") is None:
        harness.skip("less not installed")
        return
    term, _ = start(
        """
        tool bash {"command":"cat tool-fifo; : LESS_COMMAND_END"}
        end-turn
        text After less.
        end-turn
        """,
        "less -R",
        ("tool-fifo",),
        capture=False,
    )
    term.wait_for("[bash] cat tool-fifo")
    term.send("C-t")
    term.wait_for("TRANSCRIPT")
    term.type("/LESS_COMMAND_END")
    term.send("Enter")
    term.wait_for("LESS_COMMAND_END")
    term.type("q")
    term.write_fifo(term.workdir / "tool-fifo", "")
    term.wait_for("After less.")
    term.wait_for("❯")
    term.type("/help")
    term.send("Enter")
    term.wait_for("view model-facing transcript in $PAGER")


harness.main(globals())
