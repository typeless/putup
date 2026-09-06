# Keep-going — requirements

- area: keep-going
- required-legs: none

Keep-going widens what a run attempts after something has gone wrong. The subject here is what
it must still refuse to attempt. `exit-status` says what a refusal does to the process status;
this area says what it does to the work. A directory is refused when the run found its Tupfile
and got no rules from it — unreadable, unparseable, or failing to evaluate. Evaluation stops at
the failing statement, so the rules a Tupfile declared before that point are a prefix of a file
nobody finished reading, not a subset of a project putup understands.

## Group: refusal

### REQ-KEEPGOING-REFUSED-RUNS-NOTHING

- conformance: deliberate-deviation
- reference: upstream wraps its whole create phase in one transaction — `process_create_nodes` in `updater.c` calls `tup_db_begin` before parsing, `tup_db_rollback` on any failure, and `tup_db_commit` only at the end — and passes a hardcoded keep-going of 0 to `execute_graph`, so a Tupfile that fails partway leaves behind no rule of its own and no rule of any other directory either: the update aborts. putup narrows that rollback to the failing directory, which is the deviation `REQ-STATUS-EVAL-FAILED` records in the status; this sentence records the same deviation in scope. The sentence names declaration order because a Tupfile that fails to evaluate has already declared rules by the time it fails, and those are exactly the rules a naive skip runs
- discharge: test "Scenario: A directory refused mid-evaluation runs none of the rules it declared"
- discharge: test "Scenario: A refused directory's half-declared rule does not reach the shell"
- discharge: test "Scenario: A refused directory's config rule does not run under configure"

While keep-going is in effect, when a directory's Tupfile is refused, putup shall run none of
the rules that Tupfile declared, whether it declared them before or after the failure.

### REQ-KEEPGOING-ACCEPTED-UNAFFECTED

- conformance: putup-only
- discharge: test "Scenario: A refused directory does not stop a sibling's new rule running"

While keep-going is in effect, when a directory's Tupfile is refused, putup shall run the rules
declared by every directory whose Tupfile it accepted and whose inputs the refusal left
resolvable.

### REQ-KEEPGOING-REFUSED-INPUT

- conformance: putup-only
- reference: the qualifier on REQ-KEEPGOING-ACCEPTED-UNAFFECTED needs a sentence of its own, because refusing a directory retracts the declarations that made its outputs resolvable, not the files themselves. A consumer whose input the refused directory had already built still finds that file on disk and is governed by the ordinary record; only a consumer whose input no accepted directory declares and which does not exist is stopped, and it is stopped as an unresolved input rather than as a refusal of its own
- discharge: test "Scenario: A rule consuming a refused directory's output fails the build"

While keep-going is in effect, when a rule's input is an output that no accepted directory
declares and that does not already exist, putup shall not run that rule.
