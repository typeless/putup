# Change detection — requirements

- area: change-detection
- required-legs: none

The subject here is the predicate a build applies to a file it already knows about: given the
file on disk and what the previous build recorded about it, has it changed? Only the predicate
is in scope. Writing the previous value down and turning a difference into work for the command
and its consumers are the record and route obligations of `command-record`, which owns them; this
area declares `required-legs: none` because what is left once those are removed is a function of
one file and one recorded value, not state carried across builds.

The predicate is content, not timestamp. That is the deliberate deviation from upstream tup, and
it decides the two ways the predicate can fail a user: calling a file changed that is not, which
costs a rebuild, and failing to read the file at all, which leaves the answer unknown and must be
said out loud rather than resolved silently in either direction.

See `README.md` for the format and the rules that apply to every area.

---

## Group: predicate

What makes a known file changed.

### REQ-CHANGE-CONTENT

- conformance: deliberate-deviation
- reference: upstream `tup_file_mod_mtime` marks a file modified whenever `MTIME_EQ` fails against the recorded timestamp and never compares content; putup hashes instead, so a touch costs nothing
- discharge: test "Scenario: Touch does not trigger unnecessary rebuild"

When a file's modification time changes and its content does not, putup shall treat that file as
unchanged.

### REQ-CHANGE-HASH-FAILURE

- conformance: putup-only
- discharge: test "Scenario: A file that cannot be hashed is named in a warning"

If putup cannot read a file whose content it must hash, then putup shall warn and name that file.
