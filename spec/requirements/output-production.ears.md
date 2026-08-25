# Output production — requirements

- area: output-production
- required-legs: none

A declared output is a claim, and this area is about when the claim is checked. `output-paths`
answers where a rule may write and which spelling is recorded, both while the Tupfile is read;
this area answers whether the write happened, which nothing can know until the command exits.

The claim is load-bearing in three places at once: a downstream rule consumes the file, the
record names the command as owning it, and `clean` deletes it. A command that exits zero
without producing one leaves all three asserting a file that is not there, and the first
symptom appears at whichever consumer reads it next — a rule that never ran wrong, reporting
an error about a file it does not own (issue #415).

Upstream sees the writes themselves, through FUSE, which lets it answer a wider question:
every write is classified, so a file written but never declared is an error there too. putup
has no FUSE and asks the narrower question this area states — the declared paths are known
before the command runs, so a stat of each after it exits settles it. The narrowing is real
and is recorded on the requirement: a stat cannot distinguish an output never written from one
written and then removed, and it says nothing about undeclared writes.

The check runs only where a command actually ran. A dry run reports what would execute without
executing it, so it has nothing to stat, and a group carries no file of its own.

See `README.md` for the format and the rules that apply to every area.

---

## Group: declared-outputs

Whether the files a rule declares exist once its command has exited.

### REQ-OUTPUT-PRODUCED

- conformance: tup-conformant
- reference: upstream reports `tup error: Expected to write to file '...'` from `src/tup/db.c` (cited in issue #415, not read here); the outcome matches, the mechanism does not — tup observes the write through FUSE where putup stats the declared path after the command exits, which narrows it three ways: an output written and then removed is indistinguishable from one never written; a directory created at the declared path satisfies it; and a dangling symlink reads as absent on POSIX, where `stat` follows the link, but as present on Windows, where `GetFileAttributesW` answers for the reparse point, so that one shape is rejected on one platform and accepted on the other
- discharge: test "Scenario: A command that creates none of its declared outputs names every one of them"
- discharge: test "Scenario: A command that writes only some of its declared outputs fails the build"

If a command exits successfully without creating every file it declares as an output, then putup
shall fail that command and name each declared output that does not exist.
