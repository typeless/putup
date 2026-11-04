# Display text — requirements

- area: display
- required-legs: none

A rule may carry a `^ text ^` annotation ahead of its command. The subject here is what that
annotation names in output and how it resolves: where it stands in for the command, where it
deliberately does not, how it composes with a `!macro`, and what it must not touch. Both branches
belong here — a rule *without* an annotation still has to be named, and stating only the branch
that has one is what let four call sites report nothing at all (#229). See `README.md` for the
format and the rules that apply to every area.

This area requires no legs. A display is a function of the rule that declares it and carries no
state across builds, so its requirements are invariants of that function and carry no `leg` field.

The grammar of the caret head belongs here too. Upstream keys flags against display on whether a
space follows the opening `^`, and rejects an unterminated caret at parse time; putup implements
neither flag, so it refuses the flag form rather than rendering it as a label.

---

## Group: substitution

Where the annotation stands in for the command, and where it must not.

### REQ-DISP-SUBST

- conformance: tup-conformant
- reference: upstream substitutes the display for the entry's name when not verbose (tup `print_tup_entry`)
- discharge: test "Scenario: A rule's display text stands in for the command in build output"

Where a rule carries a display annotation, putup shall report that rule by its annotation rather
than by its command text.

### REQ-DISP-ABSENT

- conformance: tup-conformant
- reference: upstream substitutes the display for the entry's name only where one exists (tup `print_tup_entry`); with no annotation the entry's name — the command itself — is what gets reported, so a rule without one is never nameless
- discharge: test "Scenario: A rebuild reason names a command that carries no display annotation"
- discharge: test "Scenario: Removing a group member re-runs the commands that consume the group"

Where a rule carries no display annotation, putup shall report that rule by its command text.

### REQ-DISP-FAIL

- conformance: deliberate-deviation
- reference: upstream reports a failing command by its display unless --verbose (tup `print_tup_entry`); a success line names the step by its display, but reproducing a failure needs the exact command, which upstream shows only under --verbose; upstream also prefixes the failure with the percent complete when its bar is off (tup `show_result`), and putup's failure line carries no percent, since it is not a result line
- discharge: test "Scenario: A failing command is reported by its command line, not its display"
- discharge: test "Scenario: A failing config rule is reported by its command line, not its display"

When a command fails, putup shall report it by its command line rather than by its display
annotation.

### REQ-DISP-VERBOSE

- conformance: tup-conformant
- reference: upstream's --verbose switches from the display back to the real command (tup `print_tup_entry`, tup `tup_entry_set_verbose`)
- discharge: test "Scenario: Verbose output names the command a display annotation stands in for"

Where verbose output is requested, putup shall report each command it runs by its command line
rather than by its display annotation.

---

## Group: resolution

What the annotation's text resolves to before it is reported.

### REQ-DISP-FLAGS

- conformance: tup-conformant
- reference: upstream expands the display through the same tup_printf percent expansion as the command (tup `do_rule`)
- discharge: test "Scenario: Percent flags inside display text expand against the rule"

putup shall expand percent flags in a display annotation against the rule that declares it.

### REQ-DISP-EXPAND-FAIL

- conformance: tup-conformant
- reference: upstream returns -1 when display expansion fails rather than dropping the display (tup `do_rule`)
- discharge: test "Scenario: Percent flags inside display text expand against the rule"

If expanding a display annotation fails, then putup shall report the failure rather than build the
command without its annotation.

### REQ-DISP-GROUP-FLAG-TEXT

- conformance: tup-conformant
- reference: upstream's `tup_printf` copies `%<` through as text in every string it expands (tup `parser.c`, the `'<'` branch: "%<group> is expanded by the updater before executing a command"), and the updater splices groups only into the command (`updater.c`, `expand_command`); `print_tup_entry` prints the stored display as written, so a display reads `%<objs>` whether or not the command reads the group, names a group the rule never reads without complaint, keeps a `%<` with no closing `>` as text (the end-group marker is looked for only in `updater.c`), and reads `%%<objs>` as `%<objs>` (the `'%'` branch, with the display expanded under `EXPAND_PERCPERC` by `do_rule`'s `tup_printf(tf, cs.display, ...)`). putup substituted the group's members into a display only when the command also spliced that group, printed it as written otherwise, refused a display naming a group the rule could not resolve, refused one whose `%<` was never closed, and spliced the members into a display spelling `%%<objs>`
- discharge: test "Scenario: A group flag inside display text is reported as written"

putup shall report a `%<` group flag in a display annotation as written, whether or not a `>`
closes it, neither splicing the group's members into it nor resolving the group it names.

### REQ-DISP-MACRO

- conformance: putup-only
- discharge: test "Scenario: A bang macro's display wins over one written on the rule"

Where a rule applies a `!macro` that carries its own display annotation, putup shall report the
macro's annotation and discard the rule's.

### REQ-DISP-UNTERMINATED

- conformance: tup-conformant
- reference: upstream makes a missing closing caret a parse-time error, "Missing ending `'^'` flag" (tup `split_command_string`)
- discharge: test "Scenario: An unterminated caret is a parse error, not a shell error"

If a display annotation has no closing `^`, then putup shall reject the Tupfile while parsing it.

### REQ-DISP-HEAD

- conformance: deliberate-deviation
- reference: upstream reads the non-space run after `^` as flags — `t` marks outputs transient, `o` requests output comparison (tup `split_command_string`); putup implements neither, and treating the run as display text would render a flag as a label, honouring nothing and refusing nothing
- discharge: test "Scenario: An upstream caret flag is rejected, not rendered as a label"

Where the characters after `^` are not preceded by a space, putup shall reject the Tupfile rather
than treat them as a display annotation.

---

## Group: identity

What the annotation must not reach.

### REQ-DISP-IDENTITY

- conformance: putup-only
- discharge: test "Display text stays out of command identity"

putup shall exclude a display annotation from a command's identity.
