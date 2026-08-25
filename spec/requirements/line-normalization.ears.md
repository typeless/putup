# Line normalization — requirements

- area: line-normalization
- required-legs: none

The subject here is what the `Lexer` constructor rewrites before anything tokenizes: a line
continued with a trailing backslash, and the carriage return of a CRLF line ending. Both are
arms of one pass, and the subject is the bytes they leave in the text putup stores, runs and
re-splits into words. See `README.md` for the format and the rules that apply to every area.

This area requires no legs. Normalization is a function of the Tupfile text and carries no
state across builds.

The rewrite happens once, in the `Lexer` constructor, so the invariants are structural: no
token text can contain a continuation or a line-ending carriage return, and no consumer of a
token, a variable value, or a command built from either has to allow for one. Every arm is
length-preserving — each rewritten byte becomes a space rather than disappearing — so token
offsets and the physical-line bookkeeping keep their meaning. (The requirement ids are
`REQ-CONT-*` for historical reasons; they predate the area's rename and are stable citations.)

The trailing run putup walks is narrower than the one tup trims: tup uses `isspace`, so a
vertical tab or form feed before the line ending stops putup's walk and leaves the carriage
returns behind it in place — issue #353.

---

## Group: rendering

What a continuation becomes.

### REQ-CONT-SPACES

- conformance: tup-conformant
- reference: tup `parse_tupfile` overwrites both bytes with spaces before parsing
- discharge: test "Lexer renders a continuation as spaces"

putup shall replace each byte of a backslash-newline continuation with a space, so a
continuation renders as two spaces and a backslash-CRLF continuation as three.

### REQ-CONT-CRLF

- conformance: tup-conformant
- reference: tup `parse_tupfile` accepts a `\r` before the newline
- discharge: test "Parser renders a continuation as spaces"

putup shall accept a continuation whose newline is preceded by a carriage return rather than
reporting a parse error.

### REQ-CONT-EOF

- conformance: tup-conformant
- reference: tup `get_newline` ends the last line at the terminating nul
- discharge: test "Lexer renders a continuation as spaces"

putup shall treat a backslash that ends the file as a continuation, whether or not a carriage
return follows it and with no newline after either.

### REQ-CONT-LINES

- conformance: tup-conformant
- reference: tup `parse_tupfile` increments its line counter once per joined line
- discharge: test "Lexer renders a continuation as spaces"

putup shall report a diagnostic below a continuation against the physical line it is written
on, counting each joined line.

### REQ-CONT-EQUIVALENT

- conformance: tup-conformant
- reference: tup rewrites the buffer before parsing, so a continuation is the spaces it becomes
- discharge: test "Parser continuation equals its spelling in spaces at every position"

putup shall parse a Tupfile containing a continuation to the same rules, variable values and
paths as the same text with that continuation written out as spaces.

### REQ-EOL-CR

- conformance: tup-conformant
- reference: tup `parse_tupfile` trims each line's trailing whitespace run, `\r` included
- discharge: test "Lexer normalizes a CRLF line ending"
- discharge: test "Parser ignores whitespace at the end of a line"
- discharge: test "Scenario: A CRLF Tupfile builds what its LF twin builds"
- discharge: test "Scenario: A CRLF Tupfile names its outputs without the carriage return"
- discharge: test "Scenario: A CRLF include line resolves"

putup shall replace with a space every carriage return in a line's trailing run of blanks and
carriage returns, so no token text, variable value or output path carries it.

## Group: single-line text

What no consumer has to allow for.

### REQ-CONT-NO-NEWLINE

- conformance: tup-conformant
- reference: tup's rewrite precedes tokenization, so no parsed text spans lines
- discharge: test "Parser renders a continuation as spaces"
- discharge: test "Scenario: compdb reports no argument carrying a newline"

putup shall produce no command text, variable value or path containing a newline, whatever
continuations the Tupfile spells them with.

### REQ-CONT-SCANNED

- conformance: tup-conformant
- reference: under tup's rewrite a continued compile has the words of the one-line spelling
- discharge: test "Scenario: A continuation without a space before it builds and is scanned"

putup shall recognize a compile written across a continuation as a compile, and scan it for
implicit dependencies, however the continuation is spaced.
