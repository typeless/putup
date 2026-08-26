# Operand flags — requirements

- area: operand-flags
- required-legs: none

The subject here is the numbered `%N`-flags a rule may spell, and what each names. The
unnumbered flags that decide where a rule writes are the subject of `output-paths`, which also
holds `%o` and `%O`; a numbered input flag names a spelling of an input rather than a location,
so those are collected here.

## Group: spellings

### REQ-OPERAND-NUMBERED-INPUT-SPELLINGS

- conformance: deliberate-deviation
- reference: upstream selects the name list by letter and then the spelling within it (`tup_printf`, the `%N`-flag branch): `f`, `b` and `B` all read the input list, and the append distinguishes the whole path from the basename and from the basename with its extension removed. putup matches upstream on that spelling but not on which entries a number selects: upstream numbers the input *tokens* (`get_path_list` assigns one order id per whitespace-separated path and a glob's matches inherit their token's), then appends every entry carrying that id, so `%1f` on a globbed token names all of its matches; putup numbers the flattened file list and names one file. Pre-existing for `%Nf` and `%No` and extended here to `%Nb` and `%NB` rather than introduced (#429)
- discharge: test "Scenario: Numbered input flags name the basename and the basename without extension"

If a rule spells `%Nf`, `%Nb` or `%NB`, then putup shall expand it to the N-th input's whole
path, its basename, or its basename without its extension respectively.

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

### REQ-OPERAND-NUMBERED-ORDER-ONLY-UNSUPPORTED

- conformance: deliberate-deviation
- reference: upstream expands `%Ni` to the N-th order-only input in a command string and refuses it elsewhere (`tup_printf`, the `%N`-flag branch, which is passed a null order-only list outside a command). putup does not bind that list yet and refuses `%Ni` everywhere, naming the issue; the alternative was to keep emitting it as literal text into a command line or a filename, which is the silent failure this area exists to end (#426)
- discharge: test "A numbered order-only input flag is refused as unsupported"

If a rule spells `%Ni`, then putup shall reject the Tupfile naming the issue that tracks it.
