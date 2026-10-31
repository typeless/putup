# Operand flags — requirements

- area: operand-flags
- required-legs: none

The subject here is the numbered `%N`-flags a rule may spell, and what each names. The
unnumbered flags that decide where a rule writes are the subject of `output-paths`, which also
holds `%o` and `%O`; `%b`, `%B` and the numbered input flags name a spelling of an input rather
than a location, so those are collected here.

## Group: spellings

### REQ-OPERAND-NUMBERED-INPUT-SPELLINGS

- conformance: tup-conformant
- reference: upstream selects the name list by letter and then the spelling within it (`tup_printf`, the `%N`-flag branch): `f`, `b` and `B` all read the input list, and the append distinguishes the whole path from the basename and from the basename with its extension removed. It appends every entry carrying the order id the number names, joined by single spaces (#429)
- discharge: test "Scenario: Numbered input flags name the basename and the basename without extension"
- discharge: test "Scenario: A numbered input flag names every operand of its token"

If a rule spells `%Nf`, `%Nb` or `%NB`, then putup shall expand it to every operand of the N-th
input token, spelled as its whole path, its basename, or its basename without its extension
respectively and joined by single spaces.

### REQ-OPERAND-UNNUMBERED-INPUT-SPELLINGS

- conformance: tup-conformant
- reference: upstream's `%b` and `%B` walk the one input name list `%f` walks (`tup_printf`, the `b` and `B` branches: `TAILQ_FOREACH` over the entries, appending `nle->base` with its whole length or its extension-less length, joined by single spaces), so a rule with several inputs spells every one of them; a foreach rule hands `tup_printf` a one-entry list per iteration, which is why the two readings agree there. Measured against tup: `: *.c sub/dd.tar.gz <g> a.c` with `a.c` and `bb.c` present writes `b=[a.c bb.c dd.tar.gz <g>] B=[a bb dd.tar <g>]`. putup spelled only the first input (#414)
- discharge: test "Scenario: A basename flag names every input, not only the first"
- discharge: test "Evaluator pattern expansion - multiple inputs"

If a rule spells `%b` or `%B`, then putup shall expand it to every input operand of the command,
spelled as its basename or its basename without its extension respectively and joined by single
spaces.

### REQ-OPERAND-DIRECTORY-SPELLING

- conformance: tup-conformant
- reference: upstream spells `%d` as the last path element of the parsed directory, and at the top of the hierarchy as the last path element of the directory holding `.tup` (tup `parser.c`, `tup_printf`, the `%d` branch: `tf->tent->tnode.tupid == DOT_DT` takes the text after the last separator of `get_tup_top()`). putup's project root is its source root, which `Layout` holds canonical, so the root spells the source tree's own directory name. putup spelled nothing at the root (#445); before #430 it spelled `.` at parse time and nothing when the command ran. putup now writes `%d` into the command as text while reading the Tupfile, so the scheduler and the index never spell it; a command recorded before this change and carried forward for a directory not parsed again still shows `%d` empty at the root in `show` output and in a removed-command label until that directory is parsed
- discharge: test "Scenario: Percent-d expands to Tupfile directory name"
- discharge: test "Scenario: A percent-d at the project root names the project directory"
- discharge: test "Scenario: A dep scan sees the same directory flag its compile sees"

When a rule spells `%d`, putup shall expand it to the last path element of the rule's Tupfile
directory, and at the project root to the last path element of the project's source directory.

### REQ-OPERAND-EXTENSION-SPELLING

- conformance: tup-conformant
- reference: upstream's extension is the filename's text after its last dot, which is empty for a name that ends in a dot (`do_rule`, from `extlessbaselen`; `tup_printf`, the `%e` branch). Measured against tup: a foreach rule over `a.c`, `a.tar.gz` and `trail.` writes `e=[c]`, `e=[gz]` and `e=[]` (#453)
- discharge: test "Scenario: A percent-e in a foreach rule names each file's last extension"

While expanding `%e` in a foreach rule, putup shall expand it to the text after the last dot in
the current file's name.

### REQ-OPERAND-GLOB-MATCH-SPELLING

- conformance: tup-conformant
- reference: upstream records, for every input a glob pattern produced, the span of the file's name each of the pattern's wildcards matched, and spells `%g` as the first wildcard's span (`build_name_list_cb`, `glob_parse`; `tup_printf`, the `%g` branch). The span can be empty. Measured against tup: `: *.c |>` over the one file `a.c` writes `g=[a]`, and `: foreach *a.c |>` over `a.c` writes `g=[]`. A `?` or a bracket spans one character, so `foreach ?.c` and `foreach [ab].c` write `g=[a]` and `g=[b]`; a `*` spans up to the last occurrence of the literal text after it, up to the next wildcard, so `foreach *_*.c` over `x_y.c` writes `g=[x]`; a `*` with nothing after it before another wildcard spans the rest of the name. A `*` followed anywhere later, past the character right after it, by a `?` or a `[` spans nothing, which upstream's source marks as not supported yet. putup's extraction read only the first `*` and took what followed it as a literal suffix, so those three forms wrote `g=[]` (#465, #467)
- discharge: test "Scenario: A percent-g names the text a single glob input's star matched"
- discharge: test "Scenario: A percent-g names the first wildcard's match whatever its kind"
- discharge: test "glob_match_extract"
- discharge: test "Evaluator pattern expansion - glob match"

While expanding `%g` in a rule whose one input a glob produced, putup shall expand it to the
text that glob's first wildcard matched in the input's name, even when that text is empty.

### REQ-OPERAND-GLOB-MATCH-OWN-GLOB

- conformance: tup-conformant
- reference: upstream matches each input against the path element that produced it, not against any other glob in the rule: `nl_add_path` hands each element's own text to `build_name_list_cb`, which records that element's match on the entry (`args.globstr`, `nle->globcnt`). A variable's words are separate elements. Measured against tup: `: foreach *.c *.h |>` over `a.c` and `b.h` writes `g=[a]` and `g=[b]`, and so does the same list carried in one `$(SRCS)`; `: *.c *.h |>` over `a.c` alone writes `g=[a]`. putup kept one pattern per rule, the last written, and matched the input against it, so every input an earlier glob produced wrote nothing (#458, #465)
- discharge: test "Scenario: A percent-g names the match of the glob that produced its input"

While expanding `%g` for an input a glob produced, putup shall match the input against the
glob that produced it, whatever other globs the rule names before or after it, and whether
or not they arrived in one variable.

### REQ-OPERAND-NUMBER-NAMES-A-WRITTEN-TOKEN

- conformance: tup-conformant
- reference: upstream numbers the whitespace-separated tokens of a rule's input list rather than the operands they expand to (`get_path_list` assigns one order id per token, before variable expansion), copies that id onto every entry the token produces (`eval_path_list` for a variable's words, `build_name_list_cb` for a glob's matches, `nl_add_bin` for a bin's members), and never renumbers. Measured against tup: `: $(EMPTY) $(SRCS) a.c` with `SRCS = a.c b.c` writes `N1=[] N2=[a.c b.c]` (#429)
- discharge: test "Scenario: A numbered input flag names every operand of its token"
- discharge: test "Scenario: A token that expands to nothing still consumes its number"

putup shall give a rule's number to the token it was written as, so that a token expanding to
several operands answers to one number and a token expanding to none still holds its own.

### REQ-OPERAND-NUMBER-SURVIVES-FOREACH

- conformance: tup-conformant
- reference: upstream's foreach loop shallow-copies one name list entry into the per-iteration list with its order id intact (`execute_rule`), so a number names nothing on the iterations whose operand came from a different token. Measured against tup: `: foreach d1.in d2.in` writes `N1=[d1.in] N2=[]` for the first iteration and `N1=[] N2=[d2.in]` for the second (#429)
- discharge: test "Scenario: An operand keeps its token number through a foreach split"

When a foreach rule expands to one command per input, putup shall keep each operand under the
number of the token it was written as rather than renumbering it for its own iteration.

### REQ-OPERAND-NUMBERED-OUTPUT-SPELLING

- conformance: tup-conformant
- reference: upstream builds the output list with the same tokenizer it builds the input list with (`parse_output_pattern` calls `get_path_list`), on its own counter starting at one, and `%No` selects from it exactly as `%Nf` selects from the inputs (`tup_printf`, the `%N`-flag branch). Measured against tup: `|> out.txt $(OUTS)` with `OUTS = p.txt q.txt` writes `O1=[out.txt] O2=[p.txt q.txt]` (#429). Where upstream binds no output list — the primary outputs section and an order-only path list — the branch reads a null list and appends nothing, with no error, unlike `%o`, `%O` and `%Ni`, which refuse an unbound list; so `|> %1o.txt` declares an output named `.txt`. putup refused `%No` there with the message for `%o` (#426)
- discharge: test "Scenario: A numbered output flag names every operand of its output token"
- discharge: test "Scenario: A numbered output flag where no output list exists expands to nothing"

If a rule spells `%No`, then putup shall expand it to every operand of the N-th output token,
joined by single spaces, and to nothing in its primary outputs section or an order-only input list.

### REQ-OPERAND-ORDER-ONLY-INPUT-SPELLING

- conformance: tup-conformant
- reference: upstream passes the rule's own order-only name list to `tup_printf` as its own argument for the command and display strings (`do_rule`, `&r->order_only_inputs`; a bang macro's `bang_oo_inputs` are kept apart and never passed), and the `%i` branch walks that list appending `nle->path`, joined by single spaces; the `%N`-flag branch selects from the same list by order id. The list is built by the tokenizer the input list is built by, so a group is one entry spelled as its reference, a bin is its members, a glob its sorted matches, and a path named twice is two entries. Measured against tup: `: a.c | y.h z.h |>` writes `i=[y.h z.h]`; `| *.h sub/*.h` writes `i=[y.h z.h sub/s.h]` and `%2i` writes `sub/s.h`; `| <grp>` writes `i=[<grp>]` and `| sub/<grp>` writes `i=[sub/<grp>]`; `| {bn}` writes the bin's member; `| z.h z.h` writes `i=[z.h z.h]`; a rule in `sub/` writing `| ../z.h` writes `i=[../z.h]`; `!m = | y.h |> echo %i` called by `: a.c | z.h |> !m` writes `i=[z.h]`, and `!m = |> echo %2i` called by `: a.c | y.h z.h |> !m` writes `z.h`; `%2i` over one order-only input, and `%1i` over none, write nothing; a display string spells it too. putup spelled `%i` as the inputs, the same text as `%f`, at every expansion site, and refused `%Ni` as unsupported (#461)
- discharge: test "Scenario: A percent-i spells the rule's order-only inputs"
- discharge: test "Evaluator pattern expansion - %i spells the order-only inputs"

If a rule spells `%i` in its command or display string, then putup shall expand it to every
order-only operand the rule itself wrote after `|`, spelled as `%f` spells an input and joined by
single spaces; and `%Ni` to every operand of the N-th such token, or to nothing when the token
does not exist.

### REQ-OPERAND-ORDER-ONLY-PATH-EXPANDS-INPUT-FLAGS

- conformance: deliberate-deviation
- reference: upstream runs an order-only path list through `tup_printf` before evaluating its variables (`eval_path_list` with a live `input_nl`), passing the rule's inputs, no outputs, no order-only list and no extension, so input flags expand, `%No` reads no list and expands to nothing, and `%o` and `%O` are refused with "%o can only be used in a command string or extra outputs section" and "%O can only be used in the extra outputs section", `%i` with "%i is only valid in a command string", and `%e` with "%e is only valid with a foreach rule for files that have extensions"; `%<` is copied through as text. A foreach rule passes the one input of each iteration (`parser.c`, the `path_list_to_nl` call on `tmp_oo_inputs`), any other rule its whole input list. A regular input list gets no `input_nl`, so its text is a filename there and in putup alike. Upstream expands a bang macro's own order-only paths only when the calling rule is foreach (`parse_bang_rule` gets a NULL name list otherwise); putup expands them against the calling rule's inputs either way. putup expands after evaluating variables, as it does for outputs, so a `%`-flag in a variable's value expands where upstream leaves it as text. A rule whose input glob matched nothing produces no command in putup before its order-only list is read, where upstream refuses an input flag in that list. putup took the order-only text as a filename, so `| dep_%B.txt` named a file called `dep_%B.txt` and failed as a missing input (#425)
- discharge: test "Scenario: An order-only input list expands the rule's input %-flags"

When a rule's order-only input list or its bang macro's spells a `%`-flag, putup shall expand an
input flag against the inputs of the command being built and reject the Tupfile for `%o`, `%O`,
`%i` or `%e`; and putup shall take a regular input list's `%` as text.

### REQ-OPERAND-DUPLICATE-INPUT-PRUNED

- conformance: tup-conformant
- reference: upstream prunes a rule's input list to one entry per resolved node before it expands anything (`make_name_list_unique`, called once on the rule's inputs and before both the foreach split and `!`-macro resolution). It removes the later entry rather than blanking it, and never renumbers, so the token that named it keeps its number and names nothing. putup keys on the operand's normalized path instead of the resolved node, since nodes do not exist at that point; the two agree on every spelling of one path but not on two paths resolving to one node. They also differ on an input outside the project: upstream gives an external path no `tup_entry` (`nl_add_external_path`) and `make_name_list_unique` skips every entry without one, so `: /etc/hostname /etc/hostname a.c` keeps both, while putup prunes the second. Measured against tup: `: a.c a.c b.c c.c` writes `ALL=[a.c b.c c.c] N1=[a.c] N2=[] N3=[b.c] N4=[c.c]` (#449)
- discharge: test "Scenario: An input named twice is one operand and empties its second token"
- discharge: test "Scenario: An input named two ways is one operand"
- discharge: test "Scenario: A group named twice in the inputs section is one operand"
- discharge: test "Scenario: A group named two ways is one operand"
- discharge: test "Scenario: A group named two ways from a subdirectory is one operand"

When a rule names one input more than once, putup shall keep the first and leave the tokens
that named the rest holding nothing.

## Group: refusals

### REQ-OPERAND-NUMBERED-RANGE

- conformance: deliberate-deviation
- reference: putup refuses a numbered `t` outside the bound where upstream's parser exempts it, deferring the number to node resolution instead, which is the deviation; otherwise this matches upstream, which bounds-checks the number before selecting a list, refusing anything below 1 or at 99 and above -- its message says "1-99" but its guard is `num >= 99`, so 99 itself is refused and putup refuses it too (`tup_printf`, the `%N`-flag branch). putup expanded an out-of-range number to nothing, so a rule naming an operand that cannot exist produced a command with the operand silently gone. Upstream exempts `%Nt` from this bound because it accepts node references; putup has none, so a numbered `t` is refused here when out of range and by the unknown-letter rule otherwise
- discharge: test "A numbered flag outside one to ninety-nine is refused"

If a rule spells a `%N`-flag whose number is below one, or is ninety-nine or above, then putup
shall reject the Tupfile.

### REQ-OPERAND-NUMBERED-UNFINISHED

- conformance: tup-conformant
- reference: upstream refuses a `%N`-flag with no letter after the number (`tup_printf`, the `%N`-flag branch)
- discharge: test "A numbered flag with no letter after its number is refused"

If a rule spells a `%N`-flag with no letter after its number, then putup shall reject the
Tupfile.

### REQ-OPERAND-NUMBERED-UNKNOWN-LETTER

- conformance: deliberate-deviation
- reference: upstream refuses any letter after the number outside `f`, `b`, `B`, `o` and `i`, and its message names exactly those five; it also accepts `%Nt` for node references, which it defers to a later pass rather than expanding here. putup has no node references, so `%Nt` falls into this refusal rather than being deferred, and upstream's message stays accurate for putup because it never named `t` either (#426)
- discharge: test "A numbered flag with an unknown letter is refused"

If a rule spells a `%N`-flag whose letter is not one of `f`, `b`, `B`, `o` or `i`, then putup
shall reject the Tupfile.

### REQ-OPERAND-UNKNOWN-FLAG

- conformance: tup-conformant
- reference: upstream's `tup_printf` (`src/tup/parser.c`) refuses any character after `%` that none of its branches recognises with "Unknown %-flag: '<c>'", a `%` that ends the text included, since it reads the terminating NUL as the flag; putup's message for that case names the cause instead of printing an empty character. putup emitted the `%` and the character as text, so a typo'd flag produced a command other than the one the Tupfile spells, and the bsp example's `printf '%s'` worked only because of it (#446)
- discharge: test "A flag letter upstream does not recognise is refused"
- discharge: test "Scenario: A rule spelling an unknown percent flag is refused"

If a rule's command, inputs or outputs spell a `%` followed by a character that is not a flag,
or a `%` that ends the text, then putup shall reject the Tupfile and name the character.

### REQ-OPERAND-QUOTED-FLAG-UNSUPPORTED

- conformance: deliberate-deviation
- reference: upstream's `tup_printf` accepts `%'f`, `%"f`, `%'o` and `%"o`, spelling each operand wrapped in the quote with the quote escaped inside it; putup does not implement them and refuses them by name rather than passing the text through, which spelled `%"f` literally
- discharge: test "A flag letter upstream does not recognise is refused"

If a rule spells a quoted `%`-flag, `%'` or `%"`, then putup shall reject the Tupfile and say the
form is not supported.

### REQ-OPERAND-ORDER-ONLY-FLAG-COMMAND-ONLY

- conformance: tup-conformant
- reference: upstream passes a null order-only list to `tup_printf` for an output pattern and an input path (`parse_output_pattern`, `eval_path_list`), and both the `%i` branch and the `%N`-flag branch refuse on it with `%i is only valid in a command string`, the numbered one naming its number. Measured against tup: `|> %i.txt`, `|> out.txt | %i.txt` and `|> %1i.txt` are refused; `^ DISP[%i]^` is not (#461)
- discharge: test "Scenario: A percent-i in a rule with no order-only inputs or outside a command is refused"
- discharge: test "Evaluator pattern expansion - %i with no order-only inputs or outside a command is refused"

If a rule spells `%i` or `%Ni` in its outputs or extra outputs, then putup shall reject the
Tupfile.

### REQ-OPERAND-ORDER-ONLY-FLAG-NO-ORDER-ONLY-INPUTS

- conformance: tup-conformant
- reference: upstream refuses `%i` when the order-only name list is empty with `%i used in rule pattern and no order-only input files were specified` (`tup_printf`, the `i` branch, guarded on `ooinput_nl->num_entries == 0`), in the same function as the `%f` guard REQ-OPERAND-INPUT-FLAGS-NO-INPUTS conforms to; the numbered form never refuses, `%1i` over no order-only inputs expanding to nothing like `%1f`. The list is the rule's own, so a bang macro's order-only inputs do not fill it, and neither does a group written in the inputs section, which upstream treats as order-only for dependencies but not for `%i`. Measured against tup: a rule with inputs and none order-only, a rule with no inputs at all, `!m = | z.h |> echo %i` called by `: a.c |> !m`, `: a.c <grp> |> echo %i`, and a display string in a rule with no order-only inputs, all refused with the same message; `%1i` over none builds. putup expanded `%i` to the inputs and built (#461)
- discharge: test "Scenario: A percent-i in a rule with no order-only inputs or outside a command is refused"
- discharge: test "Evaluator pattern expansion - %i with no order-only inputs or outside a command is refused"

If a rule spells `%i` in its command or display string and it wrote no order-only inputs after
`|`, then putup shall reject the Tupfile.

### REQ-OPERAND-EXTENSION-FOREACH-ONLY

- conformance: tup-conformant
- reference: upstream binds an extension only for a foreach rule whose file's name has an extension -- a dot after its first character, so `.hidden` has none and `trail.` has an empty one -- and refuses `%e` wherever it finds none bound, naming the input when the rule has exactly one and saying it is not a foreach rule otherwise (`tup_printf`, the `%e` branch; the extension is set in `do_rule` only when `extlessbaselen != baselen`). A group operand has no dot of its own and is refused the same way. putup expanded `%e` to the first input's extension in any rule, or to nothing, so a rule that could never have a single extension ran with one picked for it; measured against tup for a rule over one and several inputs, a bang macro's output, a display string, a name whose only dot leads it, a file with no dot under a directory with one, and a group. Upstream also expands order-only inputs through the same function and refuses `%e` there; putup does not expand flags in that section at all (#425), so this requirement covers the command, display and output sections only (#453)
- discharge: test "Scenario: A percent-e in a rule that is not foreach is refused"
- discharge: test "Scenario: A percent-e in a foreach rule over a file with no extension is refused"
- discharge: test "Evaluator pattern expansion - %e with no extension bound is refused"

If a rule spells `%e` in its command, display or outputs and it is not a foreach rule, or its
current file's name has no extension, then putup shall reject the Tupfile.

### REQ-OPERAND-FOREACH-FROM-MACRO

- conformance: tup-conformant
- reference: upstream makes a rule foreach when either the rule or the bang macro its command names says so, and neither can cancel the other (`execute_rule`, parser.c:2145-2151, `foreach = br->foreach` then `if(r->foreach)`); a macro says so with `foreach` first after its `=`, before any `|`, and a `foreach` after the `|` is an order-only input path like any other word there (`parse_bang_definition`, the `strncmp(input, "foreach", 7)` test). A foreach macro over inputs that came out empty takes the foreach branch, which generates nothing, so it never reaches an `.EMPTY` variant (REQ-GLOB-EMPTY-BANG-VARIANT). It sits in this area because `%e`'s binding is where foreach-ness is observable and no area covers rule semantics; the macro alias form `!a = !b`, which upstream lets carry the foreach, does not parse in putup (#464)
- discharge: test "Scenario: A bang macro declared foreach makes the rule that invokes it foreach"

Where a rule's command names a bang macro defined with `foreach` before its order-only inputs,
putup shall treat the rule as a foreach rule.

### REQ-OPERAND-INPUT-FLAGS-NO-INPUTS

- conformance: tup-conformant
- reference: upstream refuses `%f`, `%b` and `%B` when the input name list is empty, each with its own message naming the letter (`tup_printf`, the `f`, `b` and `B` branches, guarded on `nl->num_entries == 0`), in the same function as the `%o` guard that REQ-OUTPUT-OPERAND-FLAG-NO-OUTPUTS conforms to. A rule reaches the guard with an empty list when its input section was written empty or holds only order-only inputs, or through a bang macro's `.EMPTY` variant, which runs over the empty list (REQ-GLOB-EMPTY-BANG-VARIANT); otherwise a non-empty section that matched nothing generates no command (REQ-GLOB-NO-MATCH-NO-COMMAND). The numbered forms never refuse: `%1f` over no inputs expands to nothing. putup expanded all three to nothing and ran the command with the operand gone; measured against tup for a rule with no inputs spelling `%f`, `%b` or `%B` in its command, `%f` in its output name and in its display string, a rule whose only inputs are order-only, and a bang macro's command, all refused with the same message, and for `%1f`, a group as the only input, a foreach over a glob that matched nothing, a glob that matched nothing in a rule that is not foreach, and an input section that is one empty variable, which build the same (#460)
- discharge: test "Scenario: A percent-f, percent-b or percent-B in a rule with no inputs is refused"
- discharge: test "Evaluator pattern expansion - %f, %b and %B with no inputs are refused"

If a rule spells `%f`, `%b` or `%B` in its command, display or outputs and it has no inputs,
then putup shall reject the Tupfile.

### REQ-OPERAND-GLOB-MATCH-SINGLE-GLOB-INPUT

- conformance: tup-conformant
- reference: upstream refuses `%g` in three orders: no inputs, then more than one input, then one input that no glob produced, each with its own message (`tup_printf`, the `%g` branch, guarded on `nl->num_entries` and `nl->globcnt`). A rule reaches the third refusal whether or not it is foreach, since a named file has no wildcard to match, and a named file beside a glob in the same rule, or in the same variable, is still one no glob produced. Order-only inputs are not counted. putup expanded `%g` to the primary input's match in any rule, or to nothing, so a rule over several files ran with the match of whichever sorted first; measured against tup for a rule with no inputs, with only order-only inputs, over two named files, over a glob matching two files, over a glob plus a named file, over one named file with and without foreach, over a named file beside a glob that matched nothing, at the named file of a foreach over a glob and a name, written out and carried in one variable, a bang macro's command, a display string, and an output name. A file the rule names twice is REQ-OPERAND-GLOB-MATCH-NAMED-TWICE (#465)
- discharge: test "Scenario: A percent-g in a rule with no inputs or several is refused"
- discharge: test "Scenario: A percent-g in a rule with one input and no glob is refused"
- discharge: test "Evaluator pattern expansion - %g with no single glob input is refused"

If a rule spells `%g` in its command, display or outputs and it has no inputs, or more than one,
or its one input was not produced by a glob, then putup shall reject the Tupfile.

### REQ-OPERAND-GLOB-MATCH-NAMED-TWICE

- conformance: deliberate-deviation
- reference: upstream keeps one entry for a file a rule names twice but reads the glob count of the entry it pruned: `add_name_list_entry` overwrites the list's count with each entry added and `delete_name_list_entry` never restores it, so the outcome follows token order. Measured against tup over `a.c` alone: `: *.c a.c |>` and `: foreach a.c *.c |>` are refused with `%g flag found no globs`, `: a.c *.c |>` writes `g=[]`, and `: foreach *.c a.c |>` writes `g=[a]`. No order gives the match the glob made. putup keeps the first entry and carries the glob onto it if a later duplicate had one, so every order writes `g=[a]`; the alternative was to reproduce a count that belongs to an entry no longer in the list (#465)
- discharge: test "Scenario: A percent-g over a file a rule names both by a glob and by name is the glob's match"

While expanding `%g` for an input the rule names both by a glob and by name, putup shall
expand it to that glob's match, whichever was written first, with or without foreach.

## Group: splices

### REQ-OPERAND-SPLICED-TEXT-NOT-RESCANNED

- conformance: tup-conformant
- reference: upstream expands a command exactly once, in `tup_printf`, and stores the expanded string; its group splice runs later in the updater as a bare `strstr` replace over that stored text with no second scan (tup `tup_printf`, tup `updater.c` group substitution), so a member path or glob match carrying a `%` is never read as a flag. putup defers expansion so the index can store instruction text once per distinct template, which gave it a second scan upstream does not have; recording the instruction as validated atoms restores upstream's single-expansion guarantee without giving up that sharing
- discharge: test "Scenario: A group member whose name carries a percent flag reaches its consumer whole"
- discharge: test "Scenario: A percent escape in a group rule is not expanded a second time"
- discharge: test "Scenario: A glob match containing a percent flag expands once, not twice"
- discharge: test "Scenario: A dependency scan resolves its parent's group reference"

When text that names a file is spliced into a recorded instruction, putup shall not expand a
`%`-flag spelled by that text.

### REQ-OPERAND-ESCAPED-GROUP-MARKER-INERT

- conformance: deliberate-deviation
- reference: upstream's group splice is a `strstr` over the string `tup_printf` already expanded (tup `updater.c` group substitution), so a `%%<name>` that `tup_printf` reduced to `%<name>` is indistinguishable from one the author wrote, and upstream splices it. That indistinguishability is the same second scan REQ-OPERAND-SPLICED-TEXT-NOT-RESCANNED removes: an escape exists to say the byte is not a flag introducer, and honouring it for `%%f` while revoking it for `%%<name>` would leave the escape meaning one thing per flag letter. putup resolves a group reference only where the funnel recorded one, so `%%<name>` reaches the shell as the six bytes the author escaped
- discharge: test "Scenario: A percent escape in a group rule is not expanded a second time"

When a rule escapes a percent that would otherwise open a group reference, putup shall pass the
reference through as text rather than splice the group's members.

### REQ-OPERAND-GROUP-SPLICE-UNRESOLVED

- conformance: deliberate-deviation
- reference: upstream keeps `%<name>` in the command and splices it when the command runs (tup `updater.c`, `expand_group`), refusing a group that is not among the command's inputs with "Unable to find group '<name>' as an input for use as a resource file. Make sure it is listed as an input to the command" and a `%<` with no `>` with "Unable to find end-group marker '>' for %<group> flag". putup reaches both verdicts while reading the Tupfile, before any command runs, and drops "for use as a resource file" from the first message, which names upstream's `.res` form putup does not implement. It also resolves a group declared in the rule's own directory that the rule does not list, which upstream refuses; the bsp example's xgcc link relies on that. putup deleted an unresolved reference and kept an unclosed `%<` as text, so a typo'd group name ran the command without its inputs (#444)
- discharge: test "Evaluator %<group> pattern expansion"
- discharge: test "Scenario: A rule splicing a group it does not take as an input is refused"

If a rule's command splices a `%<name>` that names no group the rule can resolve, or a `%<` with
no closing `>`, then putup shall reject the Tupfile and name the group or the missing marker.

### REQ-OPERAND-GROUP-IS-AN-OPERAND

- conformance: tup-conformant
- reference: upstream carries a group named in a rule's inputs section on the same input name list as the files (tup `parser.c`, the input branch that creates a `TUP_NODE_GROUP` tent for a bracketed name), and `tup_printf` reads that one list, so the group holds the position it was written and is spelled as written. Running tup on `: <hdrs> a.c sub/<shdrs> b.c` writes `N1=[<hdrs>] N2=[a.c] N3=[sub/<shdrs>] N4=[b.c] ALL=[<hdrs> a.c sub/<shdrs> b.c]`, and on `: <hdrs> a.c b.c |> gcc -c %2f -o %o |> o.o` it compiles `a.c`. putup built two operand lists that disagreed about this, one including the group and one not, so its generated dependency scan read a different file than its compile compiled (#448). The group's dependency stays order-only either way: that edge comes from a separate walk, not from the operand list
- discharge: test "Scenario: A group reference in the inputs section is an operand the scan agrees with"
- discharge: test "Scenario: A group reference in the inputs section expands the same way at both sites"

When a rule's inputs section names a group, putup shall make that reference an operand at the
position it was written.

### REQ-OPERAND-RECORDED-COMMAND-FOLDS-THROUGH-THE-FUNNEL

- conformance: putup-only
- reference: upstream has no requirement of this shape because it has nothing to reconcile — `tup_printf` expands a command once and stores the finished string, so there is one expander and no second reader to disagree with it. putup defers expansion so a template can be shared (all but `%g` and `%d`, which name one file's match and one directory and are spelled while the Tupfile is read), which buys that sharing at the price of one expander per place a command is reconstructed: the parser's, the scheduler's, and a third inside the index that re-read the recorded template with a scanner of its own. That third one had already drifted — it spelled `%O` as the output's basename with its extension kept, the reading `output-paths` rejects, because the change that gave `%O` upstream's meaning could not see a copy the compiler never pointed at. Folding every site through one total switch over the atom kinds makes the next such change a compile error rather than a silent third answer
- discharge: test "Scenario: A recorded command and a graph command expand a template the same way"

When putup reconstructs a command from its recorded template, putup shall produce the command
string the graph produces for that same command.
