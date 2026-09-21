# Variant builds — requirements

- area: variant-builds
- required-legs: none

The subject here is a build whose outputs land somewhere other than beside the sources: a variant
build under `-B`, and the three-tree build where the source root, the configuration root and the
build root are three separate places. One question runs through all of it — given a path written in
a Tupfile, which node does it name. The same file is reachable by several spellings at once: a
rule declares `include/header.h` and a consumer in another directory references
`$(B)/include/header.h`, or `../data.txt`, or a `../` chain whose length depends on which
subdirectory did the spelling. Every one of those has to arrive at the node the producing rule
created, because a second node at a second spelling is a dependency nothing ever satisfies and a
rebuild that never converges.

Resolution is a function of the Tupfiles and the three roots, so this area declares no legs. Several
of its requirements are witnessed by a no-op rebuild, but what those scenarios probe is still
identity: the record and the graph must spell one file one way, or the comparison across builds is
comparing two files. The obligations of recorded state itself are `command-record`'s, not this
area's.

Upstream tup has variants and solves the same identity problem, by mirroring the source tree into
the variant directory as database entries that carry a `srcid` back to their source entry. It does
not have this area's other two shapes: a build root outside the tup hierarchy (`variant_add` names
a variant directory by its entry under the tup root), and a configuration tree separate from the
source tree. Requirements about those are `putup-only`.

See `README.md` for the format and the rules that apply to every area.

---

## Group: node-identity

Which node a path spelled in a Tupfile names, when more than one spelling reaches the same file.

### REQ-VARIANT-NAME-IS-A-FILE

- conformance: tup-conformant
- reference: upstream parents a configuration variable's node under the `tup.config` node itself rather than under its directory - `tup_db_get_tup_config_tent` finds or creates that node, `tup_db_read_vars` passes it down, and `add_var` creates the variable under it - so under tup's `unique(dir, name)` key a configuration variable and a source file can never collide; putup reaches the same disjointness by refusing a path to any node type that names none (`is_path_addressable`), because putup's path namespace is lexical and a parent alone would leave the collision spellable
- discharge: test "Scenario: A config variable does not hide a source file of the same name"
- discharge: test "Scenario: A config variable does not capture a rule's input in an out-of-tree build"
- discharge: test "Scenario: A config variable does not hide a discovered header of the same name"
- discharge: test "Scenario: A config variable does not hide a source file a glob matches"

When a rule names an input, a glob matches a name, or a dep scan discovers one, putup shall
resolve that name to the file on disk rather than to a configuration variable that shares it.

### REQ-VARIANT-OUTPUT-NODE

- conformance: tup-conformant
- reference: upstream parses each Tupfile against the variant's own directory entry (`parse` sets `tf.variant` from `tup_entry_variant` before `parse_tupfile` runs, `src/tup/parser.c`), so a rule's outputs are created under the variant directory rather than at the source spelling, and a variant entry reaches its source counterpart through `srcid` (`variant_get_srctent`, `variant_tent_to_srctent`, `src/tup/variant.c`) instead of a second entry; putup carries the same rule with paths grounded at the build root rather than with mirrored database entries
- discharge: test "Scenario: Variant outputs are automatically mapped to build directory"
- discharge: test "Scenario: Order-only deps on generated outputs resolve correctly in variants"

Where a build writes outside the source tree, putup shall represent a rule's output and every
reference to it, spelled source-relative or build-relative, as one node under the build root.

### REQ-VARIANT-UNRESOLVED-UPGRADE

- conformance: tup-conformant
- reference: upstream turns an existing entry into a real file by changing its type in place, keeping its tupid and so every link already recorded against it — `ghost_to_file` (`src/tup/create_name_file.c`), and `tup_db_set_type` on the found entry in `gitignore` (`src/tup/parser.c`), which the comment there names as the variant-to-in-tree case; neither path deletes the entry and inserts a replacement
- discharge: test "Scenario: Cross-directory regular inputs work in variant builds"

When a directory parsed before its producer references a file a later-parsed directory generates,
putup shall keep the edges recorded against the unresolved reference by changing that node in place
once it becomes the generated file, rather than by creating a second node for the producer.

### REQ-VARIANT-BUILD-ROOT-DEPTH

- conformance: putup-only
- discharge: test "Scenario: Cross-project order-only dependency resolution"

Where a reference to the build root is written from a subdirectory so that it carries fewer parent
elements than the build root's own name, putup shall resolve it to the generated file the producing
rule declared rather than by comparing the two chains of parent elements as text.

### REQ-VARIANT-RECORD-SPELLING

- conformance: putup-only
- discharge: test "Scenario: Sibling directory inputs work with incremental variant builds"

putup shall derive a cross-directory input's spelling in the build record with the derivation the
build graph uses, rather than with a second derivation that renders a parent reference differently.

## Group: roots

Which of the three roots a path is resolved against.

### REQ-VARIANT-GLOB-ROOT

- conformance: putup-only
- discharge: test "Scenario: Config tree inside source tree"

Where the configuration root lies inside the source root, putup shall expand a rule's globs against
the source root rather than against the configuration root.
