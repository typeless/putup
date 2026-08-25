# Command record — requirements

- area: command-record
- required-legs: record, compare, route

The single definition of "a command's recorded state" that issue #189 asks for. See
`README.md` for the format and the rules that apply to every area.

Each **group** below is one category of state the index carries for a command, and a category
is complete only when all three legs exist: the previous value is recorded, this build's value
is compared against it, and a difference is routed to the command and its consumers. A group
may also carry `invariant` requirements, for an obligation the category owes that is not one
of the three.

A category missing a leg is a silent wrong build. Every instance this campaign found (#125,
#126, #128, #138, #152, #166, #169, #172, #187) was a missing leg. All are closed and no group
below carries a `gap:`, so every category here claims all three legs — `spec-check` fails the
build on one that does not.

---

## Group: identity-key

Which rule this is: the key a command is joined by across builds.

### REQ-KEY-RECORD

- leg: record
- conformance: tup-conformant
- reference: tup `find_existing_command` joins by walking the outputs' incoming links
- discharge: test "compute_command_key separates rules by directory"
- discharge: test "compute_command_key separates dep-scan commands by parent"

putup shall record, for each command, a key derived from its rendered text, its source
directory, and for a dep-scan command the key of its parent.

### REQ-KEY-COMPARE

- leg: compare
- conformance: tup-conformant
- reference: tup `find_existing_command`
- discharge: test "Scenario: Editing a rule's recipe does not make it a different rule"

When a build starts, putup shall join each graph command to at most one recorded command
by output ownership, falling back to the key for an output-less command.

### REQ-KEY-ROUTE

- leg: route
- conformance: tup-conformant
- reference: tup `find_existing_command`
- discharge: test "Scenario: New source file triggers rebuild"

When a graph command joins no recorded command, putup shall schedule it.

### REQ-KEY-CLEANUP

- leg: route
- conformance: tup-conformant
- reference: tup `find_existing_command`; measured on tup v0.8-8-g4247a523, a project emptied of every rule reports "rm: in.o" and "Deleting 1 command", so the join is over the recorded set whether or not the graph still has commands to run
- discharge: test "Scenario: Removed source file triggers stale output cleanup"
- discharge: test "Scenario: A scoped build keeps the record of what an out-of-scope command produced"
- discharge: test "Scenario: A directory that failed to parse keeps the record of what it produced"
- discharge: test "Scenario: A project whose last rule is removed deletes the output it built"

When a recorded command in a directory this build has authority over joins no graph command,
putup shall delete the outputs it produced.

### REQ-KEY-RETIRE

- leg: invariant
- conformance: tup-conformant
- reference: tup `delete_files` (updater.c) retires each removed command with `tup_del_id_force` in the same update that unlinks its outputs; measured on tup v0.8-8-g4247a523, turning off a rule's `ifdef` reports "rm: in.o" and "Deleting 1 command" once, and every later update reports "No files to delete"
- discharge: test "Scenario: A rule turned off by a guard is reported removed only once"

While a build is not a dry run, when a recorded command in a directory that build has
authority over joins no graph command, putup shall retire that record.

### REQ-KEY-DRYRUN

- leg: invariant
- conformance: putup-only
- reference: upstream's closest analogue is `tup todo`, which lists only the commands that would be executed; measured on tup v0.8-8-g4247a523, it reports no retirement at any phase and ends with "Everything is up-to-date", so there is no wording to conform to; the internal reference is the stale-output line in the same loop, which already hedges as "Would remove stale"
- discharge: test "Scenario: A dry run reports the command removal it would make, not one it made"

While a build is a dry run, putup shall not report a command retirement as one it has
performed.

### REQ-KEY-UNDELETABLE

- leg: invariant
- conformance: tup-conformant
- reference: tup `delete_files` (updater.c) aborts the update when `delete_file` fails, and retires the node with `tup_del_id_force` only after the unlink has succeeded
- discharge: test "Scenario: A stale output that cannot be deleted fails the build and keeps its record"

If putup cannot delete a stale output, then putup shall fail the build and keep the record
that names that output.

### REQ-KEY-INJECTIVE

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "GraphBuilder rejects two output-less commands that share one key"
- discharge: test "Scenario: Duplicate command detection"

putup shall reject a project in which two distinct guard-satisfied commands compute the
same key.

### REQ-KEY-LENGTH

- leg: invariant
- conformance: putup-only
- reference: the obligation exists only because putup's own index format caps a string table entry at 64 KB; upstream has no such entry (#360)
- discharge: test "A command longer than the string table's entry limit is still recorded"
- discharge: test "Scenario: A build whose command outgrows the record's string entry still converges"

When a build completes, putup shall record the key and signature of every command it ran,
whatever the length of that command's rendered text.

---

## Group: identity-signature

What the command will do: a difference means re-run, not a different rule.

### REQ-SIG-RECORD

- leg: record
- conformance: tup-conformant
- reference: tup `do_rule`'s command-reuse comment; `tup_db_set_name` + `command_modified`. For the output paths the outcome matches and the mechanism does not: upstream diffs the declared set against the recorded one at parse time (`tup_db_write_outputs` in db.c, diffing through `compare_tent_trees`), where a gained output (`add_output`) or a lost one (`rm_output`) sets `outputs_differ` and re-runs the command, while putup folds the paths into a hash. One case is left out rather than matched: upstream also treats a changed output GROUP as differing outputs (`tup_db_write_outputs`'s group branch) and re-runs, where putup does not — verified on both sides, and left as it stands because a group carries no content of its own and its edges are rebuilt every parse
- discharge: test "CommandEntry conversion"
- discharge: test "Scenario: Declaring another output re-runs the command that writes it"

putup shall record, for each command, a signature derived from its rendered text, its
source directory, the paths it declares as outputs, and the value of every sticky variable
it read.

### REQ-SIG-COMPARE

- leg: compare
- conformance: tup-conformant
- reference: tup `command_modified`
- discharge: test "Scenario: Editing an output-less command re-runs it"

When a joined command's signature differs from the recorded signature, putup shall treat
the command as changed.

### REQ-SIG-ROUTE

- leg: route
- conformance: tup-conformant
- reference: tup `command_modified`
- discharge: test "Scenario: Config-driven identity change re-runs an output-less command"

When a command is treated as changed, putup shall schedule it even if it declares no
outputs.

### REQ-SIG-STABLE

- leg: invariant
- conformance: tup-conformant
- reference: tup decides re-runs by `command_modified` over the command's own text and sticky variable values, so a Tupfile edit rendering identical commands reports "No commands to execute" — verified by running tup v0.8-8-g4247a523 on a comment-only edit
- discharge: test "Scenario: A Tupfile edit that changes no command re-runs nothing"

If a Tupfile changes without changing the signature of any command it declares, then putup
shall not treat those commands as changed.

---

## Group: must-rerun

Whether the last run is evidence that the outputs are current. Two things retract that
evidence — the run failed, or a build changed a dependency without re-running the command —
and both oblige the same treatment, so both are recorded in one state. Closed by #187 and #241.

### REQ-EXIT-RECORD

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read; #187 fixed putup's behaviour without citing tup's
- discharge: test "Scenario: A target build does not forget another command's failure"

When a command exits nonzero, putup shall record that command as needing to run and shall
keep that record until the command exits zero.

### REQ-EXIT-CARRY

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: An unverified record survives a build that scheduled it without running it"
- discharge: test "Scenario: An out-of-scope command is marked when its order-only input changes"

When a build carries a command's record forward across a change to one of that command's
dependencies, putup shall record that command as needing to run.

### REQ-EXIT-DISCHARGEABLE

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A config rule a build will not run does not stay marked unverified"

putup shall not retain a needing-to-run record that no putup invocation can discharge.

### REQ-EXIT-COMPARE

- leg: compare
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A command that failed is re-run on the next build"
- discharge: test "Scenario: An unverified record survives a build that scheduled it without running it"

If a command is recorded as needing to run, then putup shall schedule it even when every
declared output exists on disk, and shall keep that record until it exits zero.

### REQ-EXIT-ROUTE

- leg: route
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A failed command's consumer runs once the command succeeds"

When a command recorded as needing to run is scheduled, putup shall also schedule the
commands that consume its outputs.

### REQ-EXIT-SCOPE

- leg: invariant
- conformance: unclassified
- reference: same class as #125
- discharge: test "Scenario: A scoped build does not forget an out-of-scope failure"

While a build is restricted to a target or a scope, putup shall preserve the recorded
state of every command outside that scope.

---

## Group: input-set

The explicit inputs a command was built from. Closed by #166.

### REQ-INPUT-RECORD

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Index serialization roundtrip"

putup shall record, for each command, the set of input files it was built from.

### REQ-INPUT-COMPARE

- leg: compare
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: New source file triggers rebuild"
- discharge: test "Scenario: A file added while building from a subdirectory is still built"

When a command's input set differs from the recorded set, putup shall treat the command as
changed.

### REQ-INPUT-ROUTE

- leg: route
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Removed file under an order-only glob triggers rebuild"
- discharge: test "Scenario: New file under an order-only glob triggers rebuild"

When a command's input set changes, putup shall schedule that command and the commands that
consume its outputs.

### REQ-INPUT-COMPLETE

- leg: invariant
- conformance: putup-only
- reference: the operand stream is putup's own record layout; upstream has no counterpart (#365). The input half has no observable behaviour to discharge against: every operand input also carries an edge, which `any_dep_changed` walks
- discharge: test "A command with more than 255 operands records all of them"

putup shall record every input a command was built from, whatever their number.

### REQ-INPUT-PRESERVE

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Implicit deps survive command-id shift from a removed source"

When a command's input set changes, putup shall preserve the recorded state of the inputs
that remain.

### REQ-INPUT-UNEXAMINED

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read; same class as #125, stated for files rather than commands
- discharge: test "Scenario: A build run from a subdirectory does not stamp an out-of-scope change as current"
- discharge: test "Scenario: A build with --all-deps does not stamp an out-of-scope change as current"

If a build does not examine a recorded file, then putup shall preserve that file's recorded
state rather than replacing it with state observed during that build.

### REQ-INPUT-UNRESOLVED

- leg: invariant
- conformance: tup-conformant
- reference: tup `nl_add_path` rejects an explicitly named input it cannot find, and rejects one that resolves to a ghost; both apply to order-only inputs, verified by running tup v0.8-8-g4247a523 on #213's reproducer
- discharge: test "Scenario: A rule still naming a deleted stale output is rejected rather than re-run"
- discharge: test "Scenario: Fresh scoped build WITHOUT -a fails for cross-directory deps"

If a command declares an input that no rule produces and that does not exist on disk, then
putup shall fail the build, whether the input is declared as a regular or an order-only one.

---

## Group: implicit-deps

The files a command actually read during its last run.

### REQ-IMPL-RECORD

- leg: record
- conformance: unclassified
- reference: tup's central mechanism, but no line citation read yet
- discharge: test "Scenario: Implicit dependencies track header changes"
- discharge: test "Scenario: A build whose discovered dependency was deleted quiesces"

putup shall record, for each command, the set of files it read during its last successful
run, including when that run read none.

### REQ-IMPL-COMPARE

- leg: compare
- conformance: unclassified
- reference: no line citation read yet
- discharge: test "Scenario: Incremental rebuilds detect header changes"
- discharge: test "Scenario: A dependency that was absent when recorded still re-runs its reader when it appears"

When the pre-build comparison finds a recorded implicit dependency of a command changed, putup
shall treat the command as changed.

### REQ-IMPL-ABSENT

- leg: compare
- conformance: tup-conformant
- reference: tup v0.8-8-g4247a523 records an accessed path that does not exist as a ghost node with a sentinel mtime (`find_dir_tupid_dt_pg` in create_name_file.c) and never deletes it (`tup_del_id_type`, which returns early for a ghost, citing upstream test t6061), so a still-absent dependency compares equal; measured on a command opening a missing file — one run, then "No commands to execute", and a re-run when the file appears
- discharge: test "Scenario: A dependency absent when it was recorded settles rather than re-running forever"
- discharge: test "Scenario: A deleted dependency a command still reports re-runs it once"

When a recorded implicit dependency was absent at the time it was recorded and the pre-build
comparison finds it still absent, putup shall not treat the command as changed.

### REQ-IMPL-ROUTE

- leg: route
- conformance: unclassified
- reference: no line citation read yet
- discharge: test "Scenario: Implicit deps survive identical rules in sibling directories"

When a command is re-joined across builds, putup shall carry its recorded implicit
dependencies to the joined command and to no other.

### REQ-IMPL-SCHEDULE

- leg: route
- conformance: unclassified
- reference: upstream mechanism not read; same shape as REQ-SIG-ROUTE, which states this for identity
- discharge: test "Scenario: A changed header re-runs the output-less command that read it"

When the pre-build comparison finds a recorded implicit dependency of a command changed, putup
shall schedule that command even if it declares no outputs.

### REQ-IMPL-SURVIVE

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A changed header re-runs the output-less command that read it"
- discharge: test "A forced command brings the scanner that reports what it read"

When a command runs, putup shall record the files that run read, whatever caused it to be
scheduled.

### REQ-IMPL-ORDER

- leg: invariant
- conformance: unclassified
- reference: upstream keeps discovered dependencies in its dependency graph, so ordering by them is not a putup invention; no line citation read yet
- discharge: test "Scenario: A discovered dependency orders its consumer on a later build"
- discharge: test "Scenario: A recorded discovery that the rules now contradict does not stall the build"

When a build schedules both a command and one that produced a file that command was recorded as
having read, putup shall run the producer first unless the rules order the two the other way.

### REQ-IMPL-RACE

- leg: invariant
- conformance: unclassified
- reference: upstream orders by its discovered dependencies, so it has no unordered pair left to mark; no line citation read yet
- discharge: test "Scenario: A command that raced its discovered dependency runs again"
- discharge: test "Scenario: Ordering the scheduler did not enforce does not excuse a race"

When a command reads a file another command produced in the same build and nothing that build
enforced put the reader after the producer, putup shall run the reader again on a later build,
whatever ordering an earlier build recorded.

### REQ-IMPL-PREROUTE

- leg: invariant
- conformance: unclassified
- reference: upstream's discovered dependencies are graph members, so scheduling reaches them the way it reaches a declared input; no line citation read yet
- discharge: test "Scenario: A producer's own input change reaches the consumer that only discovered it"
- discharge: test "Scenario: A producer's input change reaches a chain of discovered consumers"
- discharge: test "Scenario: A producer's input change reaches an output-less discovered consumer"

When a change reaches a command that produces a file another command was recorded as having
read, putup shall schedule the reader as well.

---

## Group: output-set

The files a command declares it produces.

### REQ-OUT-RECORD

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "CommandEntry conversion"

putup shall record, for each command, the set of files it declares as outputs.

### REQ-OUT-COMPARE

- leg: compare
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Dropping one output of a rule removes that output"

When a command's declared output set differs from the recorded set, putup shall delete the
outputs that are no longer declared.

### REQ-OUT-EXTRA-CARRY

- leg: compare
- conformance: unclassified
- reference: upstream threads `command_modified` through its extra-outputs call so the rule re-runs at parse time (`do_rule_outputs`, called for the main outputs and again for the extra and bang-extra outputs); putup instead compares the carried record's own edges against what this build read, so a change no in-scope rule re-read is left for a later build to find rather than marked here; the two were not read closely enough to claim equivalence
- discharge: test "Scenario: A carried-forward record notices its extra output changed"

When a command outside a build's scope is carried forward, putup shall mark it for rerun if the
build read content for an output it owns that differs from the record, including an output
declared after a second `|`.

### REQ-OUT-ROUTE

- leg: route
- conformance: deliberate-deviation
- reference: tup v0.8-8-g4247a523 rejects rather than reroutes when the consumer names the file explicitly — a deleted output named as an input is a parse error (`nl_add_path`), so tup deletes nothing and the file survives. Only globs and groups reach tup's rerouting path, where deleted files drop out of the match set (`nl_add_path`'s glob path); there putup and tup agree on the match set, though not on scheduling — tup runs the consumer zero times (see REQ-OUT-SETTLE). putup reroutes in both cases and then rejects the explicitly named one on the following build (REQ-INPUT-UNRESOLVED), because after glob expansion it can no longer tell an explicitly named input from a globbed one
- discharge: test "Scenario: Deleting a stale output re-runs its order-only consumer in the same build"

When putup deletes an output that is no longer declared, putup shall schedule the commands
that consume that output.

### REQ-OUT-SETTLE

- leg: invariant
- conformance: unclassified
- reference: upstream never reaches this state — tup runs the consumer zero times for the same deletion (see REQ-OUT-ROUTE), so it has no second run to suppress
- discharge: test "Scenario: A glob consumer of a deleted stale output settles after the healing build"

When putup has deleted an output and scheduled its consumers, putup shall not schedule them
again on a later build for that same deletion.

### REQ-OUT-REAPPEAR

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A glob consumer of a deleted stale output settles after the healing build"

If an output putup deleted exists again on disk, then putup shall treat it as changed.

### REQ-OUT-COMPLETE

- leg: invariant
- conformance: putup-only
- reference: the operand stream is putup's own record layout; upstream has no counterpart (#365)
- discharge: test "A command with more than 255 operands records all of them"
- discharge: test "Scenario: A carried-forward record stops claiming currency whatever the operand's position"

putup shall record every output a command declares, whatever their number.

### REQ-OUT-OWNERSHIP

- leg: invariant
- conformance: putup-only
- reference: putup re-derives a file's classification from each build's graph, which upstream has no counterpart for — tup's node type lives in its database and changes only by an explicit transition (`tup_db_set_type` in db.c, `ghost_to_file` in create_name_file.c), and its generated→file demotions (`delete_files`, on files the parse already found stale; `remove_tup_gitignore`, reverting a .gitignore whose directive a parse removed) are each driven by a parse rather than by a build declining to look, so upstream's answer to whether a build that did not look at a producer may retract its output's classification is a structural no (#369)
- discharge: test "Scenario: A scoped build keeps the record of who owns a generated file it did not parse"
- discharge: test "Scenario: A scoped build keeps the record of who owns a generated header it discovered"

While a build has no authority over the directory that produces a file, putup shall keep the
classification the previous record gave that file.

### REQ-OUT-INACTIVE

- leg: invariant
- conformance: deliberate-deviation
- reference: upstream never evaluates an unsatisfied branch, so its rules declare nothing and the question does not arise (`src/tup/parser.c` skips the block); putup registers them for the phi model and must therefore say what their outputs are, and answers that a classification names what this configuration produces (#386)
- discharge: test "Scenario: clean leaves a source file an inactive branch merely declares"
- discharge: test "Scenario: A glob skips a path only an inactive branch declares"
- discharge: test "GraphBuilder inactive branch does not generate its declared output"
- discharge: test "GraphBuilder generates an output the taken branch of a phi declares"

Where a command sits in an inactive conditional branch, putup shall not record its declared
outputs as generated files.

---

## Group: group-membership

Which groups a command contributes to, and the order-only edges that follow.

### REQ-GRP-RECORD

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Order-only groups ensure build ordering"

putup shall record, for each command, the groups its outputs belong to and the order-only
edges derived from them.

### REQ-GRP-COMPARE

- leg: compare
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Conditional branches only contribute active outputs to groups"

Where a command sits in an inactive conditional branch, putup shall exclude its outputs
from group membership.

### REQ-GRP-ROUTE

- leg: route
- conformance: deliberate-deviation
- reference: tup 0.8-8-g4247a523 leaves the consumer unscheduled — it deletes the member and reports "No commands to execute", so the consumer's output stays stale permanently; putup reschedules instead, which costs no Tupfile portability because only scheduling differs
- discharge: test "Scenario: Removing a group member re-runs the commands that consume the group"

When a command stops contributing an output to a group, putup shall schedule the commands
that consume that group.

---

## Group: env-values

The configuration and environment values a command's identity depends on.

### REQ-ENV-RECORD

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Fine-grained config variable tracking with indirect usage"

putup shall record, for each command, the value of every configuration variable its
rendered text depends on.

### REQ-ENV-COMPARE

- leg: compare
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Fine-grained config variable tracking with $(CONFIG_VAR) syntax"

When a recorded configuration value changes, putup shall treat every command that read it
as changed.

### REQ-ENV-ROUTE

- leg: route
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Config file changes trigger rebuild"

When a recorded configuration value changes, putup shall schedule every command that read
it.

### REQ-ENV-PRECISION

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Fine-grained config variable tracking"

If a configuration value changes and no command read it, then putup shall leave every
command unscheduled.
