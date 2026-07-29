# Glob expansion — requirements

- area: glob-expansion
- required-legs: none

A glob is expanded during the parse and its result becomes a rule's inputs. The subject here
is that expansion: which paths a pattern names, in what order, and what an exclusion removes.
See `README.md` for the format and the rules that apply to every area.

This area requires no legs. Glob expansion carries no state across builds — it is a function
of the project and the filesystem — so its requirements are invariants of that function and
carry no `leg` field.

Two open issues appear below as gaps rather than prose: #188 (the match set depends on parse
traversal order) and #195 (an exclusion that is itself a glob is expanded against the
filesystem only). Both are load-bearing: the first makes a rule's inputs a function of
something other than the project, and the second makes `!pattern` silently ineffective
out-of-tree.

---

## Group: pattern-recognition

When an input is a pattern at all, and how it decomposes.

### REQ-GLOB-CHARS

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "has_glob_chars"

putup shall treat a rule input containing an asterisk, a question mark, or an opening bracket
as a glob pattern.

### REQ-GLOB-SPLIT

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Glob split path"

putup shall split a glob pattern into the literal directory prefix that needs no matching and
the remainder that does.

### REQ-GLOB-STAR

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Glob pattern matching"

putup shall match an asterisk against any run of characters that contains no path separator.

### REQ-GLOB-QUESTION

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Glob pattern matching"

putup shall match a question mark against exactly one character, never against none.

### REQ-GLOB-RECURSIVE

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Glob pattern matching"

putup shall match a double asterisk against any number of path segments, including none.

### REQ-GLOB-CLASS

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Glob pattern matching"

putup shall match a bracketed character class against exactly one character drawn from that
class.

---

## Group: match-set

Which paths a pattern names.

### REQ-GLOB-SOURCES

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A glob matches source and generated files together"
- discharge: test "GraphBuilder glob expansion - filesystem"
- discharge: test "GraphBuilder glob expansion - generated files"

putup shall expand a glob to both the source files on disk and the files produced by the
project's rules.

### REQ-GLOB-DEDUP

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: Out-of-tree, a generated file shadowing a source is one glob match and the source survives"

Where a generated file shadows a source file of the same name, putup shall include that path
once and leave the source file in place.

### REQ-GLOB-SIBLING

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A glob reaching into a sibling directory counts each match once"

When a glob reaches into a sibling directory, putup shall include each matching path exactly
once.

### REQ-GLOB-PROJECT

- conformance: unclassified
- reference: not yet read against upstream; filed as #188
- gap: #188

putup shall expand a glob to the same match set whatever order the project's Tupfiles were
parsed in.

---

## Group: ordering

A rule's inputs reach its command line through `%f`, so match order is observable output.

### REQ-GLOB-ORDER

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: glob expansion orders matches by path, not by interning order"

putup shall order a glob's matches by path rather than by the order the paths were interned.

### REQ-GLOB-BUILDDIR

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A glob's %f order does not depend on the build directory's name"

putup shall order a glob's matches independently of the name of the build directory.

### REQ-GLOB-STABLE

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A glob over generated files is path-ordered and stable across builds"

putup shall order a glob's matches identically on every build of an unchanged project.

---

## Group: exclusions

What `!pattern` removes from a match set.

### REQ-GLOB-EXCL-LITERAL

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: An exclusion applies to generated glob matches out-of-tree"

When a rule names an exclusion that is a literal path, putup shall remove that path from the
match set whether the build is in-tree or out-of-tree.

### REQ-GLOB-EXCL-PATTERN

- conformance: unclassified
- reference: not yet read against upstream; filed as #195
- gap: #195

When a rule names an exclusion that is itself a glob, putup shall remove every path it
matches from the match set, generated files included.
