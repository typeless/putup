# Reading a record — requirements

- area: record-read
- required-legs: none

The subject here is what the reader does when a record's declared positions do not hold up. Every
offset in the index is a number the record itself supplies, so a reader validates before it trusts;
the question this area settles is what happens at the moment a check fails. It is a property of one
read, not of state carried between builds, so this area declares no legs. See `README.md` for the
format and the rules that apply to every area.

A record is trusted whole or not at all. A field this reader cannot faithfully reproduce is not
recoverable by substituting a default: an empty operand set and an empty file name are claims
downstream code acts on — the first feeds change detection, the second the paths `clean` deletes —
so a validation failure makes the record unreadable rather than weaker. `DESIGN.md`'s "What a
record claims" carries the rule and its display-side counterpart.

Two boundaries keep that from over-reaching. Rejection follows a failed check, never a value: the
string table's offset 0 is a legitimate empty string and reads as one. And each reader validates
what it reads, so the recovery read of the file table (`read_prior_paths`, issue #291) is unaffected
by damage in sections it never looks at — which is what makes whole-record rejection safe rather
than a wider outage than the damage warrants.

Upstream tup keeps its state in a database and delegates this class of decision to SQLite, so it
has no counterpart and every requirement here is `putup-only`.

---

## Group: rejection

What a failed validation does to the record.

### REQ-READ-REJECT-ON-FAILURE

- conformance: putup-only
- discharge: test "An operand count larger than the record makes it unreadable"
- discharge: test "An operand offset that wraps makes the record unreadable"
- discharge: test "A name offset that wraps makes the record unreadable"

If a semantics-bearing field's declared position fails its bounds check, then putup shall report the
record as unreadable rather than returning an empty value in that field's place.

### REQ-READ-EMPTY-IS-A-VALUE

- conformance: putup-only
- discharge: test "An empty recorded string is a value rather than a failure"

While reading a recorded string whose declared length is zero, putup shall return the empty string
as the field's value rather than treating it as a failed read.

### REQ-READ-RECOVERY-SCOPE

- conformance: putup-only
- discharge: test "A corrupt operand record still leaves the recorded paths readable"

Where a record's operand data fails validation, putup shall still recover the paths its file table
records, because that read examines the file table alone.

## Group: announcement

What the build says about a record it could not load.

### REQ-READ-ANNOUNCE-DAMAGE

- conformance: putup-only
- discharge: test "Scenario: A damaged record says so instead of rebuilding in silence"
- discharge: test "Scenario: An index from an unsupported version rebuilds without calling it damage"

If the record a build would load is damaged, then putup shall announce the damage; if its version is
merely outside the readable window, putup shall not call it damage.
