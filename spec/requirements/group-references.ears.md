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
