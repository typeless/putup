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

"Resolves" is answered by the platform's own path law, which is the only sense the phrase ever had.
A backslash separates on Windows and is an ordinary character in a POSIX filename, so `..\victim.txt`
names a location above the build root on one and a single file on the other, and both answers are
this area's (issue #388). The same divergence already held for `C:\victim.txt`; what #388 added was
the spellings that carry no `/` at all — `..\`, the UNC `\\host\share\`, and the drive-relative
`C:victim.txt` — each of which reached the record unrecognised until `pup::path` was taught the
separator and root forms the platform's own file APIs already use. Both sides of the divergence are
pinned, because a rule pinned on one platform reads as a defect report against the other.

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
- reference: upstream rejects the same class in the `PG_OUTSIDE_TUP` branch of `create_output_dirs` (`src/tup/parser.c`) — putup is stricter twice over: upstream re-roots an absolute path that lands inside its hierarchy where putup rejects it (interning it under the build root mirrored the whole path inside the tree, so it never named the file it spelled), and upstream never evaluates an unsatisfied branch at all where putup registers its rules for the phi model and resolves their output paths, which is what brings them within reach of this rejection even though it never classifies them as generated (REQ-OUT-INACTIVE)
- discharge: test "GraphBuilder rejects an output above the build root"
- discharge: test "GraphBuilder rejects an output above the build root under an unsatisfied guard"
- discharge: test "GraphBuilder rejects an absolute output path"
- discharge: test "Scenario: A rule writing above the build root fails the build instead of overwriting the file there"
- discharge: test "GraphBuilder rejects a backslash escape on Windows and names the file on POSIX"
- discharge: test "GraphBuilder rejects a UNC output on Windows and names the file on POSIX"
- discharge: test "GraphBuilder rejects a drive-relative output on Windows and names the file on POSIX"

If a rule declares an output whose path does not resolve to a location inside the build root, then
putup shall reject the Tupfile and name that path, whether or not the rule's guards are satisfied.

## Group: canonicality

Which of a path's spellings the record carries.

### REQ-OUTPUT-CANONICAL

- conformance: unclassified
- reference: upstream resolves each path element to a directory node rather than comparing strings, which has the same effect; not read closely enough to claim equivalence
- discharge: test "GraphBuilder accepts a parent reference that stays inside the tree"
- discharge: test "Scenario: A parent reference inside the tree is recorded canonically"
- discharge: test "GraphBuilder records a backslash output as two components on Windows and one on POSIX"

While recording a rule's output, putup shall record the path the output resolves to rather than the
path as the rule spelled it.

## Group: extra-outputs

Which of a rule's declared outputs the command text is expected to name.

A rule may declare outputs after a second `|`. Upstream calls these extra outputs and tracks them
exactly as it tracks the first list — same ownership link, same generated-file deletion, same
overwrite guard — differing in one thing only: they are left out of `%o`. They exist for files a
command writes whose names never appear in its command line, so absence from `%o` is the whole
point rather than an omission.

putup keeps a rule's outputs in two places, and the difference falls cleanly between them: every
declared output gets a command-to-file edge, which is what ownership, deletion, the overwrite
guard and scheduling all walk, while only the first list becomes the operand vector `%o` expands.
An extra output is therefore not excluded from `%o` by a rule the expanders apply; it is absent
from the list they read.

### REQ-OUTPUT-EXTRA-OWNED

- conformance: tup-conformant
- reference: upstream's extra-output loop (`src/tup/parser.c:3667-3675`) is identical to its main-output loop (`:3652-3665`) — same `tup_db_create_unique_link`, same removal from `gen_delete_root` and `save_root` — except for the `move_name_list_entry` into the list `%o` expands from
- discharge: test "Scenario: An extra output is owned by its command and removed by clean"

putup shall own an output declared after a second `|` as it owns one declared before it.

### REQ-OUTPUT-EXTRA-NOT-OPERAND

- conformance: tup-conformant
- reference: `tup.1:439-440` — extra outputs "behave exactly as regular outputs, except they do not appear in the %o flag"
- discharge: test "Scenario: An extra output is left out of %o"

putup shall leave an output declared after a second `|` out of the operands `%o` expands to.

### REQ-OUTPUT-EXTRA-MACRO-UNION

- conformance: tup-conformant
- reference: upstream copies a bang macro's extra outputs into a separate list applied alongside the rule's own (`src/tup/parser.c:1861-1867`, both passed to `do_rule_outputs` at `:3542-3545`), where its main outputs are a fallback the rule replaces (`:1857-1860`)
- discharge: test "Scenario: A bang macro's extra outputs join the rule's own rather than replacing them"

While expanding a bang macro, putup shall add the macro's extra outputs to those the rule declares
rather than replacing them.

### REQ-OUTPUT-EXTRA-NO-OPERAND-FLAG

- conformance: deliberate-deviation
- reference: upstream expands `%o` and `%O` inside the extra-outputs section, passing the primary outputs as `use_onl` (`src/tup/parser.c:3542`, error text at `:3993`), and `tup.1:534` names the bang-macro linker-map case it exists for; putup refuses the form instead, because its own `%O` returns the filename with its extension where `docs/reference.md:1011` promises it without, so expanding it here would spell a name neither upstream nor putup's documentation predicts (#370)
- discharge: test "Scenario: A percent-O in an extra outputs section is refused by name"

If a rule spells an extra output with `%o` or `%O`, then putup shall reject the Tupfile and name
the unsupported form.

### REQ-OUTPUT-EXTRA-GROUP-DIR

- conformance: unclassified
- reference: upstream parses the group and its directory prefix from the output section as a whole rather than per list; not read closely enough to claim equivalence
- discharge: test "Scenario: A group directory prefix after an extra outputs section names the group"

While reading a rule whose order-only group carries a directory prefix, putup shall take that
prefix from the last output list the rule declared rather than always from the first.
