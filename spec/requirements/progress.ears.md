# Build progress — requirements

- area: progress
- required-legs: none

Progress reporting is a function of a run's job outcomes and of whether stdout is a terminal. It
carries no state across builds, so this area requires no legs and its requirements carry no
`leg` field. The subject here is what a run emits *while* it works; the failure line and the
`^ text ^` annotation belong to `display`. Both schedulers are in scope — `configure` runs one
of its own, and a counter that overwrites nothing is as useless ahead of a config rule as ahead
of a compile. See `README.md` for the format and the rules that apply to every area.

---

## Group: non-interactive

### REQ-PROG-NONTTY

- conformance: deliberate-deviation
- reference: upstream gates only its overwriting progress bar on the `display.progress` option, whose default is generated from isatty on stdout (tup `show_progress`, tup `stdout_isatty`), and still prints one newline-terminated result line per command — with a percent prefix added precisely when the bar is off — in that mode (tup `show_result`), silenced only by `display.quiet` (tup `progress_quiet`, tup.1 `--quiet`); putup's default output echoes no per-command line at all (REQ-DISP-FAIL, REQ-DISP-VERBOSE), so with the bar gone nothing per-command remains to report, and the counter it emitted instead named no command and overwrote nothing in a pipe
- discharge: test "Scenario: A build whose output is not a terminal prints no progress counter"
- discharge: test "Scenario: A failing build whose output is not a terminal prints no progress counter"
- discharge: test "Scenario: A configure whose output is not a terminal prints no progress counter"

While its standard output is not a terminal, putup shall report no progress counter.
