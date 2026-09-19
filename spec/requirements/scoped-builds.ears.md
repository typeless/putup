# Scoped builds — requirements

- area: scoped-builds
- required-legs: none

The subject here is the scope filter: what a build restricted to one or more directory arguments
may look at, and what it may say when it fails. A scope narrows the work a build performs; it is
not a category of state carried across builds, so this area declares no legs. The state the
requirements below lean on — the recorded content of a file, the recorded text of a command — is
owned by `command-record` and `record-read`, and the sentences here only forbid the scope filter
from being applied to it.

That is the whole of this area's content: a scope is a restriction on *work*, never on
*observation*. A build that narrows what it looks at cannot know whether the part it skipped
invalidates the part it built, and the failure is silent — the build reports success, or reports
nothing to do, with a stale artifact on disk. Upstream tup draws the same line: its target
arguments reach the graph through `prune_graph`, after the DAG has already been built from a
whole-tree modify list, and `mark_nodes` walks incoming edges, so everything upstream of a target
survives the pruning whatever edge reaches it.

The reporting group covers the other direction — what a scoped build may claim when it fails. A
remedy is only worth printing where it can change the outcome, and `-a` changes the outcome only
for a build that left rules out of scope.

See `README.md` for the format and the rules that apply to every area.

---

## Group: detection

What a scoped build must still see outside its scope.

### REQ-SCOPE-DECLARED-INPUT

- conformance: tup-conformant
- reference: `prune_graph` and `mark_nodes` — target arguments prune the DAG after it is built from a whole-tree modify list, and marking walks incoming edges, so a file upstream of a target survives pruning whatever kind of edge reaches it
- discharge: test "Scenario: A scoped build sees a declared input outside the scope change"

While a build is restricted to a scope, putup shall detect a change to a file that a rule in that
scope declares as an input even where the file lies outside the scope, whatever kind of edge
carries the dependency, rather than exempting out-of-scope files from the comparison except for
the edge kinds some bypass list happens to name.

### REQ-SCOPE-OUT-OF-SCOPE-HEADER

- conformance: tup-conformant
- reference: `prune_graph` and `mark_nodes` — a header reached only by a discovered include is still upstream of the command that read it, so it survives the same pruning a declared input does
- discharge: test "Scenario: Scoped build detects header changes outside scope"
- discharge: test "Scenario: Scoped build with multiple scopes detects out-of-scope header changes"
- discharge: test "Scenario: Out-of-tree variant build picks up out-of-scope header changes"
- discharge: test "Scenario: Scoped rebuild after a SCOPED initial multi-scope build"

While a build is restricted to one or more scopes, putup shall re-run every in-scope command that
read a header changed outside those scopes and then re-run the in-scope commands consuming those
commands' outputs, whether the record sits in the source tree or under a build directory and
whether the preceding build was full or itself scoped, so that neither a report of nothing to do
nor a link against the objects the edit did not reach can stand.

### REQ-SCOPE-COMMAND-TEXT

- conformance: unclassified
- reference: upstream's comparison of a command's rendered text runs in its parser phase, which has not been read; `prune_graph` establishes only that the target arguments are applied after the DAG is built
- discharge: test "Scenario: Tupfile changes detected regardless of scope"

While a build is restricted to a scope, putup shall treat a command whose rendered text differs
from the text recorded for it as changed even where the Tupfile that renders it lies outside that
scope, rather than comparing recorded identities only for the commands the scope filter admits.

## Group: reporting

What a scoped build may claim when an input resolves to nothing.

### REQ-SCOPE-GHOST-HINT

- conformance: putup-only
- discharge: test "Scenario: The ghost hint offers -a only where -a could help"

When a build fails on an input that no rule it parsed produces, putup shall offer `-a` only where
that build was restricted to targets and was not already given `-a`, and otherwise name the
removal of the producing rule as the likely cause, rather than printing one hint that sends a
build the remedy cannot change round the same failure.
