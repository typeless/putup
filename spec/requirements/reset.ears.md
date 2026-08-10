# Reset — requirements

- area: reset
- required-legs: none

The subject here is what `clean` and `distclean` may remove and what they must keep. Both remove
state a previous build owned, and the build record is the only statement of that ownership: it is
what tells a file this project generated apart from one the user wrote. Nothing regenerates it,
so discarding it while a file it owns is still on disk leaves that file attributable to no one —
which is what strands an in-tree project, because a later build then reads its own leftover output
as a source file it must not overwrite.

`clean` removes owned files and never touches the record; `distclean` removes both, and one rule
governs the order it may do so in:

> The record may be discarded only when the set of surviving owned files is known to be empty.

A record putup cannot read makes that set unknowable; a file that could not be removed makes it
known non-empty. Both keep the record. Only a pass that left nothing behind discards it. Stating
the rule over what survives rather than over the reasons a removal failed is deliberate: a third
reason gets a third requirement here rather than a fourth silent case in the code.

`tup.config` is not covered by that rule and is removed either way. The asymmetry is the point —
the record is irreplaceable, and `configure` regenerates `tup.config` from `configs/`.

A dry run removes nothing, so it predicts the removals succeed and therefore predicts the record's
removal. Probing writability up front would replace a prediction with a guess that has its own
false answers.

Upstream tup keeps its state in a database it deletes wholesale, and has no counterpart to a
reset that must decide what to keep, so every requirement here is `putup-only`.

See `README.md` for the format and the rules that apply to every area.

---

## Group: record-retention

When a reset may discard the record it is resetting.

### REQ-RESET-KEEP-UNREMOVED

- conformance: putup-only
- discharge: test "Scenario: Distcleaning keeps a record whose files it could not remove"
- discharge: test "Scenario: Distcleaning out of tree keeps a record whose files it could not remove"

If a file the record names as generated is still on disk after a reset's removal pass, then putup
shall keep the build record and fail without reporting the reset complete, whether or not the
build was in tree.

### REQ-RESET-KEEP-UNREADABLE

- conformance: putup-only
- discharge: test "Scenario: Distcleaning keeps a record it cannot read and resets the rest"

If a reset cannot read the build record, then putup shall keep that record and name the command
that resets the project without it, rather than removing a record whose files it could not name.
