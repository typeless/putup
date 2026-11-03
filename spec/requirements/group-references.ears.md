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

### REQ-GROUPREF-EXPANDED-WORDS

- conformance: tup-conformant
- reference: upstream's `eval_path_list` (`src/tup/parser.c`) evaluates each input and order-only word and re-splits the result through `new_pl`, so every word a variable expands to is classified on its own: `<a>` and `<b>` are two groups, and `!`/`^` words are exclusions rather than references. putup's order-only group pre-scan in `expand_rule` re-read each written pattern as one string instead, so `G = <a> <b>` became one group `<a> /<b>` with `%<b>` unresolved, and `E = !<b>` became a group in a directory named `!`; it now takes its groups from the operands `expand_inputs` produced, the list the rule's edges are built from
- discharge: test "Scenario: Order-only groups a variable names are the words it expands to"

When a variable in a rule's inputs or order-only inputs expands to several words, putup shall
resolve each word that is a group reference as its own group, and shall not read an exclusion
word as a group.

### REQ-GROUPREF-NAME-NOT-A-FILE

- conformance: deliberate-deviation
- reference: upstream keeps a group and a file in one `(directory, name)` key, and on a type mismatch `tup_db_create_node_part_display` force-deletes the existing node (`tup_del_id_force`) and recreates it with the new type, read from the source and not run; putup kept them in one key too, so whichever rule came first decided the node: with the group first, a rule reading the file bound to the group and never reran when the file changed, and with the reader first, the group adopted the file node and took its members as inputs (#491). putup refuses instead of replacing one with the other, so neither rule order yields a graph in which a rule reads something other than what it names
- discharge: test "Scenario: A group cannot share its name with a file in its directory"

If a rule declares a group in a directory whose source or config tree holds an entry with the group's
bracketed name, or where the graph already holds a node of another type under that name, then putup
shall reject the Tupfile and name the group and what the name already holds.

### REQ-GROUPREF-OUTPUT-EXPANDED-WORD

- conformance: tup-conformant
- reference: upstream's `do_rule_outputs` (`src/tup/parser.c`) evaluates the output list with `eval_path_list` before `new_pl` classifies each word, so a variable expanding to `<g>` or `dir/<g>` declares the rule's output group as the written word would, and a second group, written or expanded, fails with "Multiple output groups detected". upstream's `get_path_list` gives every written output word an `orderid`, the group's word included, and `%No` selects by it, so in `a.txt $(G) b.txt` the output `b.txt` is `%3o`; putup keeps the expanded group's token slot the same way, while a group written in the Tupfile still takes no slot in putup, a divergence this requirement does not cover. putup recognised an output group only as written in the Tupfile, so `G = <g>` with `|> a.txt $(G)` declared a file named `<g>` and the command failed in the shell
- discharge: test "Scenario: An output group named through a variable collects the rule's outputs"

When a word of a rule's outputs expands to `<name>` or `dir/<name>`, putup shall add the rule's
outputs to that group, and if the rule then names more than one output group, putup shall reject
the Tupfile and name two of them.

### REQ-GROUPREF-OUTPUT-DIR-GLUED

- conformance: tup-conformant
- reference: upstream's `get_path_list` and `next_path` (`src/tup/parser.c`) split the output list at whitespace and `new_pl` classifies each word on its own, so in `notsub/ <g>` the word `notsub/` is an output and `<g>` is the rule directory's group; how upstream names the output `notsub/` (`get_path_elements`) was not read. putup's output parser joined a preceding word ending in `/` to a following `<name>` across the space, declaring the group `notsub/<g>` and dropping `notsub/` from the outputs
- discharge: test "Scenario: A directory word set apart from an output group is an output of its own"

When a rule's outputs write a word ending in `/` and, after whitespace, a `<name>`, putup shall
read the word as an output and `<name>` as the group of the rule's directory.

### REQ-GROUPREF-NAMED

- conformance: deliberate-deviation
- reference: upstream's `new_pl` (`src/tup/parser.c`) accepts `<>` as a group named `<>`, and the updater's `expand_command` (`src/tup/updater.c`) takes the group name from `%<` through `>` inclusive, so `%<>` reaches that group and no other: upstream handles the empty name consistently. putup refuses it in every section, written or reached through a variable, because a reference with nothing between its brackets is a misspelling far more often than a name, and refusing it costs a Tupfile that meant it one rename. Before the refusal a written `<>` in the inputs failed with "Malformed group reference in inputs: './<>'", in the outputs beside another output it was accepted as a group with an empty name, and one reached through a variable was refused as not a whole path element
- discharge: test "Scenario: A group with an empty name is refused"

If a rule's inputs, order-only inputs or outputs name a group with nothing between `<` and `>`,
then putup shall reject the Tupfile.

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

### REQ-GROUPREF-BIN-WORD-START

- conformance: tup-conformant
- reference: upstream's `new_pl` (`src/tup/parser.c`) takes the bin branch only when a path-list word begins with `{`, so `a{x}.c` and `../lib/{objs}` are paths; its output-list scan in `split_input_pattern` skips a `{` whose preceding character is not a space ("it's part of a filename") and reads a bin only from a `{` at the start of the list or after whitespace. putup's `parse_path_pattern` ended a path word at any `{`, so `a{x}.c` became the path `a` followed by a reference to a bin `x`, and `out{z}.txt` became the output `out` followed by a bin
- discharge: test "Parser reads a brace inside a path word as part of the path"
- discharge: test "Scenario: A brace inside a path word names a file rather than a bin"

When a `{` appears inside a word of a rule's inputs, order-only inputs or outputs rather than at
its start, putup shall read it as part of that word's path rather than as the start of a bin.

### REQ-GROUPREF-WHOLE-ELEMENT

- conformance: deliberate-deviation
- reference: upstream's `new_pl` (`src/tup/parser.c`) marks any path-list word holding a `<` as a group. In the outputs `do_rule_outputs` names the group node after the word's whole last element (`tup_db_create_node_part(dest_tent, pl->pel->path, ...)`), so `x<g>.txt`, `pre<g>`, `<g>post`, `<g>{b}` and `<g>#` declare groups by those names, which no `<g>` or `%<g>` reaches. In the inputs `nl_add_path` makes a group only of an element that begins with `<`, so `<g>post` reads a group named `<g>post` and `pre<g>` or `x<g>.txt` is looked up as a file and refused as "Explicitly named file ... not found" unless one by that name exists. putup refuses every such word instead, since each either names a group no reference can reach or a file whose name only looks like a group reference. Upstream reads the words after variable expansion; putup checks input words and output file words after expansion too, but decides where an output group written in the Tupfile begins before expansion, so `$(DIR)<g>` with `DIR = dir/` is refused as an output while upstream reads `dir/<g>`, and an input `$(DIR)<g>` reads `dir/<g>` as upstream does. Before the refusal putup split an output word at its `<`, so `x<g>.txt` wrote files `x` and `.txt` and declared `<g>`, `x%<g>.txt` failed with "Unknown %-flag: ''", and `<g>post` declared `<g>` and wrote `post`; in the inputs `pre<g>` read the group `g` of a directory named `pre`, `x<g>.txt` read a file of that name, and `<g>post` read the group and a file `post`
- discharge: test "Scenario: A group spelled inside a longer path word is refused"

If a word of a rule's inputs, order-only inputs or outputs holds a `<` that does not open its whole
last path element, as in `<name>` or `dir/<name>`, then putup shall reject the Tupfile.

## Group: exclusion

What `!` or `^` in front of a bin or group reference means.

### REQ-GROUPREF-NOT-EXCLUDED

- conformance: deliberate-deviation
- reference: upstream has no `!` input exclusion (its `!` is a bang macro), and reads `^` in a path list as the start of a PCRE2 regex: `new_pl` (`src/tup/parser.c`) compiles the text after `^` with `pcre2_compile` and takes the bin branch only when the entry begins with `{`, so `^{b}` and `^<g>` are regexes that match the literal text `{b}` and `<g>`. putup refuses both spellings instead: a regex that can only match a path containing a brace or an angle bracket is a misspelt exclusion, and before the refusal putup read the entry as an exclusion and a bin at once and dropped the reference without notice
- discharge: test "Parser rejects an exclusion of a bin or group"

If a rule's inputs, order-only inputs or outputs write `!` or `^` immediately before a `{bin}` or
`<group>` reference, then putup shall reject the Tupfile rather than exclude or reference it.
