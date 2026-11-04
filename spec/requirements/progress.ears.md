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

- conformance: tup-conformant
- reference: upstream gates its overwriting progress bar on the `display.progress` option, whose default is generated from isatty on stdout (tup `show_progress`, tup `stdout_isatty`); with the bar off, the result lines (REQ-PROG-RESULT) carry the progress instead
- discharge: test "Scenario: A build whose output is not a terminal prints no progress counter"
- discharge: test "Scenario: A failing build whose output is not a terminal prints no progress counter"
- discharge: test "Scenario: A configure whose output is not a terminal prints no progress counter"

While its standard output is not a terminal, putup shall report no progress counter.

### REQ-PROG-PERCENT

- conformance: tup-conformant
- reference: upstream prefixes a result line with `~%2i%% `, or `%3i%% ` once complete, exactly when a total is known and the progress bar is off (tup `show_result`); without a time estimate its percent counts finished commands (tup `progress.c`)
- discharge: test "Scenario: A build whose output is not a terminal names each command it runs with its percent complete"
- discharge: test "Scenario: An incremental build's percent counts only the commands it runs"
- discharge: test "render_result prefixes a finished command with the percent complete"
- discharge: test "render_result without a known total is the text alone"

While its standard output is not a terminal, putup shall prefix each result line with the share
of the commands this run schedules that have finished.

## Group: results

What a run reports as each command finishes.

### REQ-PROG-RESULT

- conformance: tup-conformant
- reference: upstream prints one newline-terminated line per finished command, naming it by its display, or by its command under --verbose (tup `show_result`); upstream starts a success line with one space, the width of the `* ` that marks its error line (tup `progress.c` `show_result`), and putup omits it because its failure line is `FAILED: ` (REQ-DISP-FAIL), leaving nothing to align with
- discharge: test "Scenario: A build whose output is not a terminal names each command it runs with its percent complete"
- discharge: test "Scenario: A rule's display text stands in for the command in build output"
- discharge: test "Scenario: A configure whose output is not a terminal prints no progress counter"

When a command succeeds, putup shall write one line to standard output that names it.

### REQ-PROG-QUIET

- conformance: tup-conformant
- reference: upstream's `-q`/`--quiet` returns from `show_result` early unless the command wrote output (tup `updater.c` option parsing, tup `show_result`'s `always_display`)
- discharge: test "Scenario: A build whose output is not a terminal names each command it runs with its percent complete"
- discharge: test "Scenario: A successful command's output appears under the line that names it"

Where `--quiet` is given, putup shall write no result line for a successful command that wrote no
output.

### REQ-PROG-OUTPUT

- conformance: tup-conformant
- reference: upstream writes a successful command's captured output to stdout after its result line (tup `display_output` with `iserr` 0, called from `updater.c`)
- discharge: test "Scenario: A successful command's output appears under the line that names it"

When a successful command wrote output, putup shall write that output to standard output directly
under the command's result line.

### REQ-PROG-OUTPUT-VERBATIM

- conformance: deliberate-deviation
- reference: upstream copies the command's output file to the terminal byte for byte (tup `display_output`); putup prefixes each line with its variant (REQ-PROG-VARIANT-LINE-WHOLE), so a command's output is attributable when variants build at once. putup captured the two streams apart and added a separator between them and a newline after them, which printed blank lines inside a failure report
- discharge: test "Scenario: A successful command's output appears under the line that names it"
- discharge: test "Scenario: A failed command's output follows its FAILED line without blank lines"

When putup writes a command's output, putup shall write each line the command wrote once, adding
only the `[variant] ` prefix and a newline its last line lacks.

### REQ-PROG-SCAN-READ

- conformance: putup-only
- discharge: test "Scenario: A dep scan's rule is read, not printed"

When a dependency scan succeeds, putup shall read the rule it printed as dependencies rather than
write it as the scan's output.

## Group: parallel-variants

### REQ-PROG-VARIANT-LINE-WHOLE

- conformance: putup-only
- discharge: test "Scenario: Variants built in parallel write each prefixed line whole"
- discharge: test "Scenario: Variants built in parallel keep each command's output under its own line"

When putup builds more than one variant at once, putup shall write each line a variant emits, its
`[variant] ` prefix included, in one write, so that no line carries another variant's prefix or
loses its own; a command's result or failure line and the output under it go out in one write.
