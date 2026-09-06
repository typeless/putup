# Exit status — requirements

- area: exit-status
- required-legs: none

The subject here is what putup's own process returns, which is the only part of a run a caller
can read without parsing output. A build reports its outcome twice — in the log it prints and in
the status it exits with — and only the status composes: `putup && next` and a CI step read it
and nothing else. So each sentence below is an implication from an outcome to a status, never
from a status back to an outcome: a nonzero exit says something went wrong, not which thing,
because more than one cause returns the same code.

A command *failed* here whenever putup's verdict on it is failure, which is wider than its own
status: it exited nonzero, or a signal killed it, or it exited zero without writing every output
it declares and `output-production`'s REQ-OUTPUT-PRODUCED failed it. All three reach the status
through one counter, so no sentence below distinguishes them.

Ids here are `REQ-STATUS-*`. `REQ-EXIT-*` belongs to `command-record`, whose subject is what a
command's own nonzero exit gets written down as, not what putup returns.

## Group: commands

### REQ-STATUS-COMMAND-FAILED

- conformance: tup-conformant
- reference: upstream counts a job whose command exits nonzero in `execute_graph`'s `failed` counter and returns -1 from it, which `updater` propagates and `main` returns as the process status. Both exit 1: upstream's `main` converts every negative rc to 1 before returning it
- discharge: test "Scenario: A failing command is reported by its command line, not its display"
- discharge: test "Scenario: Build fails when command fails"

If a command putup ran exits nonzero, then putup shall exit nonzero.

### REQ-STATUS-SIGNALLED-COMMAND

- conformance: tup-conformant
- reference: upstream decodes `WIFSIGNALED` in `exec_internal` and records the terminating signal in the server's `exit_sig`, where a set `exit_sig` fails the job rather than completing it
- discharge: test "Scenario: A command killed by a signal fails the build"

If a command putup ran is killed by a signal, then putup shall exit nonzero.

### REQ-STATUS-KEEP-GOING

- conformance: tup-conformant
- reference: upstream's keep-going governs only the loop condition in `execute_graph` — `(!failed || keep_going)` — leaving untouched the `failed` counter that decides the return. The sentence is about a failed command rather than a failed build because keep-going does change the status of a Tupfile that fails to evaluate, which is REQ-STATUS-EVAL-FAILED's subject below
- discharge: test "Scenario: Partial failure with -k saves successful outputs"
- discharge: test "Scenario: A command that failed is re-run on the next build"

While keep-going is in effect, when a command putup ran exits nonzero, putup shall exit
nonzero.

### REQ-STATUS-SUCCESS

- conformance: tup-conformant
- reference: upstream returns 0 from `execute_graph` when nothing failed, and `updater` returns that to `main`
- discharge: test "Scenario: Building a simple C project"
- discharge: test "Scenario: A Tupfile edit that changes no command re-runs nothing"

If no command putup ran failed and the build record was saved, then putup shall exit zero.

## Group: evaluation

### REQ-STATUS-EVAL-FAILED

- conformance: deliberate-deviation
- reference: upstream aborts the update when a Tupfile fails to parse rather than skipping the directory and continuing, so it has no state in which a run both refused a directory and succeeded; putup skips the directory under keep-going, which is what makes this sentence necessary and what makes it fail today (#431)
- gap: #431

While keep-going is in effect, when a directory's Tupfile fails to evaluate, putup shall exit
nonzero.

## Group: bookkeeping

### REQ-STATUS-RECORD-NOT-SAVED

- conformance: putup-only
- discharge: test "Scenario: A build that cannot save its record does not report success"

If putup ran commands but could not save the build record, then putup shall exit nonzero even
though every command exited zero.

### REQ-STATUS-ANY-VARIANT

- conformance: putup-only
- reference: what has no upstream counterpart is the aggregation, not the outcome — upstream builds every variant within one `execute_graph` run, where a variant's failed command is already the `failed` counter of REQ-STATUS-COMMAND-FAILED, while putup runs a process per variant and has to combine their statuses
- discharge: test "Scenario: A multi-variant run fails when one variant's command fails"

If a run builds more than one variant and any variant's build failed, then putup shall exit
nonzero.
