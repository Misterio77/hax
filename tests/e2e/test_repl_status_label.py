#!/usr/bin/env python3
"""The status row's label while a turn waits on something silent: it says what the wait is for,
once the wait outlasts the label's settle time."""

import harness


def test_says_composing_while_a_table_streams():
    """A table appears only once complete, so a long one is labeled rather than looking stalled,
    even while its rows keep arriving."""
    # Rows arrive well inside the label's settle time and keep coming past it; the scenario ends
    # once the label shows, long before the table would.
    rows = "".join(f"text | row {i} | {i} |\\n\ndelay 100\n" for i in range(100))
    term = harness.Terminal(
        "text Here is a table:\\n\\n| Name | Value |\\n|---|---|\\n\n" + rows + "end-turn\n"
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("composing...")


if __name__ == "__main__":
    harness.main(globals())
