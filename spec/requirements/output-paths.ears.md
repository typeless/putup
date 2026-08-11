# Output paths — requirements

- area: output-paths
- required-legs: none

The subject here is where a rule may write and how that location is recorded. A rule names its
outputs relative to its Tupfile, and `..` is allowed among those names, so the path a rule writes is
not the path a rule says: it is what the `..` elements resolve to. Two questions follow, and both are
answered while the Tupfile is read, before anything is recorded — this area is a function of the
Tupfiles alone and declares no legs.

The first is how far out a rule may reach. An output that resolves above the build root is a write
into a tree the build does not own, and the file already sitting there is a file no build produced:
`putup` overwrote committed sources this way (issue #385). An absolute output is the same question in
another spelling, and its answer here is stricter than upstream's, for the reason recorded on the
requirement. Upstream tup answers both in its parser, which is the enforcement point this area
follows — establishing the rule once, where the path is read, rather than at each site that later
acts on a recorded path.

The second is which path is recorded. Every reader downstream — the deletion pass, the source-shadow
guard, the record's own source/generated split — compares recorded paths against each other, and two
spellings of one file defeat all of them. So an output is recorded by where it lands, not by how it
was written.

See `README.md` for the format and the rules that apply to every area.

---

## Group: hierarchy

How far out of the tree a rule may write.

### REQ-OUTPUT-INSIDE-HIERARCHY

- conformance: deliberate-deviation
- reference: upstream rejects the same class in the `PG_OUTSIDE_TUP` branch of `create_output_dirs` (`src/tup/parser.c`) — putup is stricter twice over: upstream re-roots an absolute path that lands inside its hierarchy where putup rejects it (interning it under the build root mirrored the whole path inside the tree, so it never named the file it spelled), and upstream never evaluates an unsatisfied branch at all where putup registers its rules for the phi model and records their outputs
- discharge: test "GraphBuilder rejects an output above the build root"
- discharge: test "GraphBuilder rejects an output above the build root under an unsatisfied guard"
- discharge: test "GraphBuilder rejects an absolute output path"
- discharge: test "Scenario: A rule writing above the build root fails the build instead of overwriting the file there"

If a rule declares an output whose path does not resolve to a location inside the build root, then
putup shall reject the Tupfile and name that path, whether or not the rule's guards are satisfied.

## Group: canonicality

Which of a path's spellings the record carries.

### REQ-OUTPUT-CANONICAL

- conformance: unclassified
- reference: upstream resolves each path element to a directory node rather than comparing strings, which has the same effect; not read closely enough to claim equivalence
- discharge: test "GraphBuilder accepts a parent reference that stays inside the tree"
- discharge: test "Scenario: A parent reference inside the tree is recorded canonically"

While recording a rule's output, putup shall record the path the output resolves to rather than the
path as the rule spelled it.
