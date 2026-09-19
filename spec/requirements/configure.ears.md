# Configure — requirements

- area: configure
- required-legs: none

Two subjects meet here. The first is which `tup.config` a directory's Tupfile is evaluated
against: putup collects every `tup.config` from the output root down to that directory and
merges them, the parent overriding the child, so a composed subtree can ship defaults an
integrator overrides from above (`docs/reference.md` §6.1). The second is the `configure`
command, which runs only the rules that produce `tup.config` files, so that a build can be
given configuration a rule generated rather than configuration a user wrote.

The two meet at a chicken-and-egg problem: the directories `configure` generates configuration
for have none while it runs. `configure` therefore parses against the build root's
`tup.config` alone, and its own products are read by the per-directory merge on the build that
follows.

This area requires no legs. Neither subject carries state across builds: the merge is a
function of the configuration files present, and `configure` produces files rather than a
record. The configuration a build reads persists because nothing rewrites it, which is the
subject of one requirement here rather than a record leg.

Upstream tup has no counterpart to the `configure` command — its subcommand dispatch in
`main.c` has no such command, `init_command` is what creates `.tup`, and `updater()` reads a
configuration the user wrote — so the requirements about the command are `putup-only`. The
configuration files themselves do have an upstream counterpart, and the per-directory merge is
a deviation from it.

See `README.md` for the format and the rules that apply to every area.

---

## Group: inheritance

Which configuration a directory's Tupfile is evaluated against when the directory has none of
its own.

### REQ-CONFIGURE-INHERIT

- conformance: deliberate-deviation
- reference: upstream gives a variant one configuration file — `updater` reads the variant's `tup.config` entry once through `tup_db_read_vars`, and `variant_add` registers a variant by that single file — so there is no ancestor to inherit from and no merge; putup reads a `tup.config` per directory instead, so a subtree composed into a larger project can carry its own configuration (`docs/reference.md` §6.1)
- discharge: test "Scenario: Subdir inherits from parent when no local config"
- discharge: test "Scenario: Root config used when no intermediate configs"

Where a directory has no `tup.config` of its own, putup shall evaluate its Tupfile against the
configuration of the nearest ancestor directory that has one, rather than against an empty
configuration or against the directory's own file alone.

### REQ-CONFIGURE-INHERIT-EMPTY

- conformance: deliberate-deviation
- reference: upstream has one configuration file per variant (`tup_db_read_vars`, `variant_add`), so an empty one is the whole configuration there and there is nothing for it to shadow; under putup's per-directory merge (`docs/reference.md` §6.1) it must shadow nothing
- discharge: test "Scenario: Empty subdir config does not block parent merge"

Where a directory's `tup.config` sets no variable, putup shall still evaluate its Tupfile
against the ancestors' configuration, rather than reading the file's presence as the whole
configuration for that directory.

---

## Group: scope

What `configure` reads, runs, and creates.

### REQ-CONFIGURE-ROOT-ONLY

- conformance: putup-only
- discharge: test "Scenario: Configure uses root tup.config only"

While running `configure`, putup shall resolve a config-generating rule's `@()` references from
the build root's `tup.config`, so that a directory whose `tup.config` this run is about to
generate parses before that file exists.

### REQ-CONFIGURE-NO-RECORD

- conformance: putup-only
- discharge: test "Scenario: Configure does not create .pup directory"

While running `configure`, putup shall create no build record directory, rather than
initialising a project that has not been built yet.

### REQ-CONFIGURE-DEPS

- conformance: putup-only
- discharge: test "Scenario: Configure handles config rule depending on non-config rule"

When a rule that produces configuration consumes a file another rule produces, putup shall run
that other rule under `configure` as well, rather than scheduling the configuration-producing
commands alone and leaving the rule's input unbuilt.

---

## Group: reporting

What `configure` may claim it produced.

### REQ-CONFIGURE-CREATED

- conformance: putup-only
- discharge: test "Scenario: A configure that cannot write tup.config does not report creating it"

If `configure` cannot write `tup.config`, then putup shall fail and name that file in its
error, rather than announcing the file as created before the write has succeeded.

---

## Group: persistence

What a build may do to the configuration `configure` gave it.

### REQ-CONFIGURE-PERSIST

- conformance: putup-only
- discharge: test "Scenario: Config selection persists across multiple builds"

When a build runs after `configure`, putup shall leave the values in `tup.config` as
`configure` left them, rather than re-deriving the configuration each build and giving a build
with nothing to do something to write.
