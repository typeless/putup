# Writing a record — requirements

- area: record-write
- required-legs: none

The subject here is what the record a build writes says, as opposed to what a later build makes of
it. Two properties of the written bytes are settled in this area: a path appears in exactly one
entry, and the content is a function of the project rather than of the run that produced it — of
which trees the build ran in, in which order its jobs finished, and whether it had anything to do.
These are properties of one write, not obligations of a category of state carried between builds,
so this area declares no legs. See `README.md` for the format and the rules that apply to every
area, and `record-read.ears.md` for what a reader does with a record that breaks them.

Both properties exist because the record is read back by something other than the build that wrote
it. A second entry for one path is a fork in the reader's lookup — one entry carries the recorded
state a later build compares against and the other does not — which is why the reader treats a
record naming one path twice as unreadable (`REQ-READ-REJECT-SELF-CONTRADICTION`); the writer's job
is not to produce one. Content that varies run to run has no such reader-side failure: it simply
makes every comparison of two records, and every claim that a build changed nothing, unfalsifiable.

Upstream tup keeps its state in a SQLite database rather than a serialized record, so the
byte-content properties here have no upstream counterpart and are `putup-only`; the uniqueness
property does, because a database schema is where upstream states it.

---

## Group: uniqueness

How many entries one path gets.

### REQ-WRITE-ONE-ENTRY-PER-PATH

- conformance: tup-conformant
- reference: upstream tup's `node` table declares `unique(dir, name)` in db.c's schema, so a directory-and-name pair names at most one node and a second insert for it is rejected by the database rather than by the walk
- discharge: test "Scenario: A build records one entry per path"

When a build writes its record, putup shall write exactly one entry per path rather than one entry
per directory chain that reached that path.

## Group: determinism

What the record's content is allowed to depend on.

### REQ-WRITE-SAME-TREE-SAME-SHAPE

- conformance: putup-only
- discharge: test "Scenario: Two builds of one tree record the same thing"

When the same project is built twice in trees that share nothing, putup shall write records of the
same shape and size rather than recording edges in the order the jobs that produced them finished.

### REQ-WRITE-NOOP-SAYS-THE-SAME

- conformance: putup-only
- discharge: test "Scenario: Two builds of one tree record the same thing"

While a build has nothing to run, putup shall write a record whose entries, edges, operands and
strings say what the record it read said, rather than one that restates them as the run it did not
need to do would have.
