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
- reference: upstream's extra-output loop (`do_rule`'s `extra_onl` loop) is identical to its main-output loop (its `onl` loop) — same `tup_db_create_unique_link`, same removal from `gen_delete_root` and `save_root` — except for the `move_name_list_entry` into the list `%o` expands from
- discharge: test "Scenario: An extra output is owned by its command and removed by clean"

putup shall own an output declared after a second `|` as it owns one declared before it.

### REQ-OUTPUT-EXTRA-NOT-OPERAND

- conformance: tup-conformant
- reference: tup.1's `extra-outputs` section — extra outputs "behave exactly as regular outputs, except they do not appear in the %o flag"
- discharge: test "Scenario: An extra output is left out of %o"

putup shall leave an output declared after a second `|` out of the operands `%o` expands to.

### REQ-OUTPUT-EXTRA-MACRO-UNION

- conformance: tup-conformant
- reference: upstream copies a bang macro's extra outputs into a separate list applied alongside the rule's own (`parse_bang_rule_internal`, both passed to `do_rule_outputs`), where its main outputs are a fallback the rule replaces
- discharge: test "Scenario: A bang macro's extra outputs join the rule's own rather than replacing them"

While expanding a bang macro, putup shall add the macro's extra outputs to those the rule declares
rather than replacing them.

### REQ-OUTPUT-EXTRA-OPERAND-FLAG

- conformance: tup-conformant
- reference: upstream expands `%o` and `%O` inside the extra-outputs section, passing the primary outputs as `use_onl` (`do_rule_outputs`, error text in `tup_printf`), and `%O` appends the declared path truncated at its last dot rather than its basename — pinned by upstream's own `test/t4162-*.sh`, whose expected failure names `sub/out/file.txt.2` for a primary `out/file.txt` under a `sub/%O.txt.2` extra output; tup.1's `%O` entry describes the same and names the bang-macro linker-map case it exists for, while `tup.1`'s extra-outputs prose calls it a basename, which those tests contradict (#423)
- discharge: test "Scenario: A percent-O extra output names the primary output without its extension"
- discharge: test "Scenario: A percent-o extra output names the primary outputs"
- discharge: test "Scenario: A percent-O extra output keeps the directory the output was declared with"

While expanding an extra output, putup shall expand `%o` to the rule's primary outputs and `%O` to
its single primary output with the extension removed.

### REQ-OUTPUT-OPERAND-FLAG-PLACEMENT

- conformance: tup-conformant
- reference: both flags read `tup_printf`'s output name list, which upstream binds for a command string, a display string, and the extra-outputs section, and leaves null for the primary outputs section — `do_rule` passes `&onl` when expanding the command and `do_rule_outputs` passes `use_onl`, null for the primary section only. Upstream leaves it null for input path lists too (`eval_path_list`), so it refuses both flags there; putup instead treats the text as a literal filename, which this requirement does not cover. `%O`'s message names the extra-outputs section alone and so understates where it is accepted; putup keeps upstream's wording rather than correcting it, since that is the text a user searches for
- discharge: test "Scenario: A percent-O in the outputs section is refused"
- discharge: test "Scenario: A percent-o in the outputs section is refused"
- discharge: test "Scenario: A percent-O in a command string names the output without its extension"

If a rule spells `%o` or `%O` in its primary outputs section, then putup shall reject the
Tupfile.

### REQ-OUTPUT-EXTRA-FLAG-ARITY

- conformance: tup-conformant
- reference: upstream refuses `%O` unless the rule declares exactly one output (`tup_printf`, `num_entries != 1`), wherever `%O` is accepted, pinned by its own `test/t4163-*.sh`; putup would otherwise have to pick one silently, which is how `%O` came to disagree with its own documentation
- discharge: test "Scenario: A percent-O with more than one output is refused"
- discharge: test "Scenario: A percent-O in a command string with more than one output is refused"

If a rule spells `%O` anywhere it is accepted while declaring other than exactly one output, then
putup shall reject the Tupfile.

### REQ-OUTPUT-OPERAND-FLAG-NO-OUTPUTS

- conformance: tup-conformant
- reference: upstream refuses `%o` when the bound output name list is empty (`tup_printf`, `num_entries == 0`), in the same switch as the `%O` arity guard; expanding it to nothing instead would run a command whose operand silently vanished
- discharge: test "Scenario: A percent-o in a rule with no outputs is refused"

If a rule spells `%o` while declaring no outputs, then putup shall reject the Tupfile.

### REQ-OUTPUT-EXTRA-FLAG-NO-EXTENSION

- conformance: deliberate-deviation
- reference: upstream computes the truncation point by scanning back for a dot at four sites and guards `extlesslen == 0` at three of them (`nl_add_external_path`, `nl_add_bin`, `build_name_list_cb`) but not at the fourth, `do_rule_outputs`, which is the one that computes it for an output name; an output with no dot therefore leaves the length at zero and `%O` expands to nothing, declaring an extra output named for the suffix alone. putup keeps the whole name instead, and takes the extension from the filename rather than the whole path, so a dot in a directory cannot truncate a name that has no extension of its own (#423)
- discharge: test "Scenario: A percent-O on an output with no extension keeps the whole name"

While expanding `%O` for an output whose filename has no extension, putup shall expand it to the
whole output rather than to nothing.

### REQ-OUTPUT-EXTRA-GROUP-DIR

- conformance: unclassified
- reference: upstream parses the group and its directory prefix from the output section as a whole rather than per list; not read closely enough to claim equivalence
- discharge: test "Scenario: A group directory prefix after an extra outputs section names the group"

While reading a rule whose order-only group carries a directory prefix, putup shall take that
prefix from the last output list the rule declared rather than always from the first.
