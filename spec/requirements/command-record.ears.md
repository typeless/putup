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

A category missing a leg is a silent wrong build. Every historical instance in this campaign
(#125, #126, #128, #138, #152, #166, #172, #187) was a missing leg, and the one still open
(#169) is recorded below as `gap:` so the absence is visible rather than inferred.

---

## Group: identity-key

Which rule this is: the key a command is joined by across builds.

### REQ-KEY-RECORD

- leg: record
- conformance: tup-conformant
- reference: tup parser.c:3105 `find_existing_command` joins by walking the outputs' incoming links
- discharge: test "compute_command_key separates rules by directory"
- discharge: test "compute_command_key separates dep-scan commands by parent"

putup shall record, for each command, a key derived from its rendered text, its source
directory, and for a dep-scan command the key of its parent.

### REQ-KEY-COMPARE

- leg: compare
- conformance: tup-conformant
- reference: tup parser.c:3105 `find_existing_command`
- discharge: test "Scenario: Editing a rule's recipe does not make it a different rule"

When a build starts, putup shall join each graph command to at most one recorded command
by output ownership, falling back to the key for an output-less command.

### REQ-KEY-ROUTE

- leg: route
- conformance: tup-conformant
- reference: tup parser.c:3105 `find_existing_command`
- discharge: test "Scenario: New source file triggers rebuild"

When a graph command joins no recorded command, putup shall schedule it.

### REQ-KEY-CLEANUP

- leg: route
- conformance: tup-conformant
- reference: tup parser.c:3105 `find_existing_command`
- discharge: test "Scenario: Removed source file triggers stale output cleanup"

When a recorded command joins no graph command, putup shall delete the outputs it produced.

### REQ-KEY-INJECTIVE

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "GraphBuilder rejects two output-less commands that share one key"
- discharge: test "Scenario: Duplicate command detection"

putup shall reject a project in which two distinct guard-satisfied commands compute the
same key.

---

## Group: identity-signature

What the command will do: a difference means re-run, not a different rule.

### REQ-SIG-RECORD

- leg: record
- conformance: tup-conformant
- reference: tup parser.c:3573 comment; `tup_db_set_name` + `command_modified`
- discharge: test "CommandEntry conversion"

putup shall record, for each command, a signature derived from its rendered text, its
source directory, and the value of every sticky variable it read.

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

## Group: exit-status

Whether the last run succeeded. Closed by #187.

### REQ-EXIT-RECORD

- leg: record
- conformance: unclassified
- reference: upstream mechanism not read; #187 fixed putup's behaviour without citing tup's
- discharge: test "Scenario: A target build does not forget another command's failure"

When a command exits nonzero, putup shall record that command as failed and shall keep
that record until the command exits zero.

### REQ-EXIT-COMPARE

- leg: compare
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A command that failed is re-run on the next build"

If a command is recorded as failed, then putup shall treat it as needing to run even when
every declared output exists on disk.

### REQ-EXIT-ROUTE

- leg: route
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A failed command's consumer runs once the command succeeds"

When a recorded-failed command is scheduled, putup shall also schedule the commands that
consume its outputs.

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

### REQ-INPUT-PRESERVE

- leg: invariant
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Implicit deps survive command-id shift from a removed source"

When a command's input set changes, putup shall preserve the recorded state of the inputs
that remain.

### REQ-INPUT-UNRESOLVED

- leg: invariant
- conformance: tup-conformant
- reference: tup parser.c:2746 rejects an explicitly named input it cannot find, and parser.c:2761 rejects one that resolves to a ghost; both apply to order-only inputs, verified by running tup v0.8-8-g4247a523 on #213's reproducer
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

When a recorded implicit dependency of a command changes, putup shall treat the command as
changed.

### REQ-IMPL-ROUTE

- leg: route
- conformance: unclassified
- reference: no line citation read yet
- discharge: test "Scenario: Implicit deps survive identical rules in sibling directories"

When a command is re-joined across builds, putup shall carry its recorded implicit
dependencies to the joined command and to no other.

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

### REQ-OUT-ROUTE

- leg: route
- conformance: deliberate-deviation
- reference: tup v0.8-8-g4247a523 rejects rather than reroutes when the consumer names the file explicitly — a deleted output named as an input is a parse error (parser.c:2783), so tup deletes nothing and the file survives. Only globs and groups reach tup's rerouting path, where deleted files drop out of the match set (parser.c:2799); there putup and tup agree. putup reroutes in both cases and then rejects the explicitly named one on the following build (REQ-INPUT-UNRESOLVED), because after glob expansion it can no longer tell an explicitly named input from a globbed one
- discharge: test "Scenario: Deleting a stale output re-runs its order-only consumer in the same build"

When putup deletes an output that is no longer declared, putup shall schedule the commands
that consume that output.

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
