# Source protection — requirements

- area: source-protection
- required-legs: none

The subject here is what a build may write over. A rule's output path is a path the build intends
to destroy the contents of, and the only question that matters before it does so is whether the
file already sitting there is one the build produced. That question is not answerable from the
Tupfiles alone, which is why this is its own area: the answer is a function of the Tupfiles, the
previous build's record, and what is on disk at the moment the build starts — a wider input domain
than `output-paths`, whose subject is a function of the Tupfiles alone.

The record is the only statement of ownership, and it is read positively. A path it attributes to
a rule this configuration runs may be overwritten; a path it recorded while producing nothing at
that path is owned by nobody and is protected, whether it is a checked-in source, a foreign input,
or a file a user created after putup deleted what it once produced there. Absence from a list is
never license: a record legitimately omits what a build had no authority to attribute, so
"unlisted" means "unknown", and acting on unknown in the destructive direction is what issues #386
and #389 both were. The blast radius decides the direction of doubt — a wrong refusal is loud and
carries its remedy in the message, a wrong overwrite is silent loss.

See `README.md` for the format and the rules that apply to every area.

---

## Group: ownership

Whether the build may destroy what is already there.

### REQ-PROTECT-OWNERSHIP

- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: A build refuses to overwrite a file the record does not attribute to a rule"

If a rule declares an output at a path holding a file the previous build's record does not
attribute to a rule this configuration runs, then putup shall refuse the build and name that path.

### REQ-PROTECT-RECORD-INDEPENDENT

- conformance: unclassified
- reference: upstream mechanism not read — REQ-OUT-OWNERSHIP records that tup's node type lives in its database rather than being re-derived per build, which suggests the question cannot arise there, but that inference was not measured for this requirement
- discharge: test "Scenario: A build refuses to overwrite a file the record does not attribute to a rule"

putup shall give the same answer about overwriting a file whether or not a previous build recorded
that path.
