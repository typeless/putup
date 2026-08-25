# Requirements

One file per area of putup's behaviour, written in EARS and checked against the test suite by
`.github/scripts/spec-check`. This file holds the format and the rules that apply to every
area; each area file carries only its own subject.

Run them locally with `make spec-check`. CI runs the same check plus `--verify-gaps`.

## File format

```markdown
# Glob expansion — requirements

- area: glob-expansion
- required-legs: none

Prose about this area's subject.

## Group: ordering

### REQ-GLOB-ORDER

- leg: invariant          (only in areas that require legs)
- conformance: unclassified
- reference: upstream mechanism not read
- discharge: test "Scenario: glob expansion orders matches by path, not by interning order"

putup shall order a glob's matches by path rather than by the order the paths were interned.
```

Both header fields are mandatory and must appear before the first group. `required-legs: none`
has to be written out — omitting the line is an error, not a default, because an area that
silently required nothing would look exactly like an area that passed its checks.

A requirement is one sentence in one of the five EARS patterns, ending in a period.

## Fields

The vocabulary is closed; an unknown key is an error.

| Field | Meaning |
|---|---|
| `leg` | which obligation of the area this discharges — see below |
| `conformance` | this project's relationship to upstream tup |
| `reference` | evidence for that relationship |
| `discharge` | a test that witnesses the requirement; may repeat, one per clause |
| `gap` | an open issue number, when no test witnesses it yet |

A requirement carries either `discharge` lines or a `gap`, never both and never neither.

## Legs

`record`, `compare`, `route` are the obligations of a category of state carried across builds:
the previous value is written down, this build's value is compared against it, and a
difference becomes work for the command and its consumers. A category missing one is a silent
wrong build.

**Legs are declared per area, not globally.** They are obligations of *state*, not of a pure
function. An area whose subject is a function of its inputs declares `required-legs: none`,
and its requirements carry no `leg` field at all — a label with one legal value is noise, and
the checker rejects it. Legs exist exactly where legs are obligations.

`invariant` is available in areas that require legs, for an obligation the category owes that
is not one of the three.

## Conformance

Upstream tup is the specification of record.

| Value | Needs a `reference` |
|---|---|
| `tup-conformant` | yes — where upstream does the same |
| `deliberate-deviation` | yes — why we differ |
| `putup-only` | no — no upstream counterpart |
| `unclassified` | yes — why it has not been classified yet |

`unclassified` is a declared debt, not a claim of equivalence. Most requirements are
`unclassified` today; narrowing that set means reading upstream, and a stated reason beats an
invented citation.

**Cite upstream by name, never by line number.** A `reference` names the function, symbol, or
tup.1 section that carries the behaviour — `` `do_rule_outputs` ``, `` `tup_db_write_outputs` ``,
tup.1's `%O` entry — not `parser.c:3542`. Upstream moves under us: syncing the reference checkout
shifts line numbers by a few and every pinned citation silently starts pointing at a brace. A
symbol name survives that, and it says what the reader is looking for instead of merely where it
sat. Nothing checks what one says — `spec-check` requires a `reference` to be present but never
reads what it points at — so their accuracy rests entirely on the convention.

## Two rules no checker can enforce

**A requirement must name one implementation it forbids.** The checker gates form and gates
whether a cited test exists; neither measures information. A sentence like "putup shall decide
whether a candidate path matches a glob pattern" passes both and is satisfied by `return
true`. A requirement's content is the set of behaviours it rules out.

**A required slot creates pressure to fill it.** The first draft of `command-record` put a
preservation statement and a precision statement into `route` slots, because the gate demanded
a `route` and those were the nearest true sentences to hand. A leg whose sentence does not
match its obligation is worse than a missing leg: the missing one fails the build, the
mislabelled one teaches the reader a rule that is false in that group. When no genuine
sentence for a leg exists, record a `gap:` — do not promote a neighbour.

## Requirement ids

`REQ-<AREA>-<NAME>`, area-scoped, unique across every file. The suffix is the leg name for a
group's primary required-leg requirement and a content name otherwise.

`command-record` predates this convention and derives its prefixes from the group
(`REQ-KEY-*`, `REQ-SIG-*`, `REQ-EXIT-*`); those are grandfathered. New areas use an
area-scoped prefix, because ids share one namespace across files and a group-derived prefix
claims a name a future area may need.

## What is generated

`spec-check --gherkin-dir` writes one `.feature` per area, plus the sorted list of Catch2 tags
no requirement witnesses yet — the backlog of unspecified subsystems.

The Gherkin is a rendering, not a check. It is derived from these files, so it cannot disagree
with them; treating generated scenarios as verification would produce a document that reports
success forever. The verification is `spec-check`.
