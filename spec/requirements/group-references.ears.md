# Group references — requirements

- area: group-references
- required-legs: none

A group is declared by the rules that name it as an output and referenced by the rules that
consume it. The subject here is the reference: which directory's group a written reference
resolves to, what has to happen before it can be resolved, and which edges the resolution puts
into the graph. How a resolved reference is spelled inside a command is `operand-flags`; what
the record keeps of the resulting edges is `command-record`. See `README.md` for the format and
the rules that apply to every area.

This area requires no legs. A reference resolves during the parse and is a function of the
project's Tupfiles and the trees the build was pointed at, not of state carried from the
previous build.

---

## Group: resolution

Which group a written reference names, and what must be parsed before that can be answered.

### REQ-GROUPREF-DEMAND-PARSE

- conformance: deliberate-deviation
- reference: upstream needs no rule of this shape because a group reference resolves against a database rather than against this build's parse — tup's input path list creates the group itself when the lookup misses (`tup_db_create_node_part` with `TUP_NODE_GROUP`, the bracketed-name branch of `parser.c`'s input path handling), and `add_input` then carries that node as an input whether or not the producing directory has been parsed in this run; the link is resolved by the updater afterwards. putup keeps no node database between the parse and the graph, so a reference to an unparsed directory's group has nothing to resolve against: it parses that directory on demand instead of inventing a placeholder a later parse would have to reconcile
- discharge: test "Scenario: Bang macro order-only groups trigger demand-driven parsing"

When a rule expands a `!`-macro whose order-only inputs name a group in a directory this build has
not parsed yet, putup shall parse that directory before resolving the reference rather than
resolving it to an empty group.

### REQ-GROUPREF-RELATIVE-TO-TUPFILE

- conformance: tup-conformant
- reference: upstream resolves every path in an input list against the directory of the Tupfile being parsed, a group's included, so a bare `<g>` and `./<g>` name the same group. Measured against tup in `sub/Tupfile`: `: <g> ./<g> a.c |> echo 'ALL=[%f] N1=[%1f] N2=[%2f] N3=[%3f]'` writes `ALL=[<g> a.c] N1=[<g>] N2=[] N3=[a.c]`. putup bound a reference whose directory part was empty or began with `./` to the project root when it reached the resolver as text rather than as a parsed group pattern, a convention kept for `$(S)/…/<g>` with `S` expanding to `.`; `TUP_CWD` expands relative to the including Tupfile in putup as upstream, so `$(S)` names the right directory without it (#456)
- discharge: test "Scenario: A group named two ways from a subdirectory is one operand"
- discharge: test "GraphBuilder a bare group reference names the group of the referencing directory"

When a rule names a group by a relative path, an empty one or one beginning with `./` included,
putup shall resolve it against the directory of the Tupfile the rule is in.

### REQ-GROUPREF-PRODUCING-DIRECTORY

- conformance: putup-only
- discharge: test "Scenario: Cross-directory groups in 3-tree builds"

Where a group reference is spelled through variables that expand to a path across the separate
source, config and build trees, putup shall resolve it to the group declared by the directory
that expanded path names rather than to the directory the referencing Tupfile sits in.

### REQ-GROUPREF-INPUTS-SECTION

- conformance: tup-conformant
- reference: upstream recognises a bracketed name in a rule's inputs section, not only after the order-only separator: the same input path-list branch that creates a missing group (`tup_db_create_node_part` with `TUP_NODE_GROUP` in `parser.c`) runs on the normal input list, and `add_input`'s `TUP_NODE_GROUP` arm adds that node to the rule's input set, so the reference names the group rather than a file. `operand-flags`' REQ-OPERAND-GROUP-IS-AN-OPERAND records the same list carrying it as an operand, measured against tup
- discharge: test "Scenario: Group references in regular inputs expand correctly"

When a rule names a group in its inputs section rather than after the order-only separator,
putup shall resolve the bracketed name to that group and expand `%<name>` to the group's
members rather than read the name as a file to be found on disk.

### REQ-GROUPREF-NAME-NOT-A-FILE

- conformance: deliberate-deviation
- reference: upstream keeps a group and a file in one `(directory, name)` key, and on a type mismatch `tup_db_create_node_part_display` force-deletes the existing node (`tup_del_id_force`) and recreates it with the new type, read from the source and not run; putup kept them in one key too, so whichever rule came first decided the node: with the group first, a rule reading the file bound to the group and never reran when the file changed, and with the reader first, the group adopted the file node and took its members as inputs (#491). putup refuses instead of replacing one with the other, so neither rule order yields a graph in which a rule reads something other than what it names
- discharge: test "Scenario: A group cannot share its name with a file in its directory"

If a rule declares a group in a directory whose source or config tree holds an entry with the group's
bracketed name, or where the graph already holds a node of another type under that name, then putup
shall reject the Tupfile and name the group and what the name already holds.

## Group: bins

Which `{bin}` a rule may name in its inputs or order-only inputs.

A bin is a list of the outputs of earlier rules in the same Tupfile. Upstream keeps one list per
Tupfile, adds a bin to it when it reads a rule that names the bin after its outputs — before any
input is matched, so a rule whose glob matched nothing still declares it — and refuses a reference
to a name that list does not hold. putup skipped such a reference, so a typo'd bin name dropped
every operand it was meant to supply and the rule ran without them (#472).

### REQ-GROUPREF-BIN-DECLARED

- conformance: deliberate-deviation
- reference: upstream's `new_pl` (`src/tup/parser.c`, the `{` branch) calls `bin_find` on the Tupfile's bin list and fails with "Unable to find bin '<name>'"; `parse_rule` adds the rule's own bin with `bin_add` before it reads the inputs, so a bin is declared by the rule text, not by what the rule matched. putup declares a bin from a rule under an unsatisfied guard too, which upstream never reads, so a reference to a bin declared only there is accepted whatever the configuration, as REQ-OUTPUT-INSIDE-HIERARCHY keeps acceptance independent of it; upstream refuses it in a configuration that leaves the guard unsatisfied
- discharge: test "Scenario: A rule naming a bin no earlier rule declares is refused"

If a rule's inputs or order-only inputs name a bin that no earlier rule in the same Tupfile declares
after its outputs, then putup shall reject the Tupfile and name the bin.
