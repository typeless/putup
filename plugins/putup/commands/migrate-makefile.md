---
description: Migrate a Make/Kbuild project to putup Tupfiles
argument-hint: <directory>
---

# Migrate Makefile to Tupfiles

Guide the user through converting a Make/Kbuild project to putup Tupfiles.

## Process

1. **Read the Makefile** in the target directory (or `$ARGUMENTS` if provided). Identify:
   - Source files and their compilation flags
   - Conditional compilation (`ifdef`, `ifeq`)
   - Generated files (config headers, tables, code generators)
   - Library targets and their dependencies
   - Cross-compilation variables (CROSS_COMPILE, CC, CXX)

2. **Create project structure:**
   - `Tupfile.ini` at project root (empty file, marks the root)
   - `Tuprules.tup` with S/B anchors, toolchain defaults, and bang macros
   - Per-directory `Tupfile` for each source directory

3. **Apply conversion patterns:**

   | Makefile | Tupfile |
   |----------|---------|
   | `obj-y += foo.o` | `srcs-y += foo.c` |
   | `obj-$(CONFIG_FOO) += bar.o` | `srcs-@(FOO) += bar.c` |
   | `CFLAGS += -DFOO` | `CFLAGS += -DFOO` |
   | `%.o: %.c` | `: foreach *.c \|> !cc \|> %B.o` |
   | `$(CC) -o $@ $^` | `: {objs} \|> $(CC) %f -o %o \|> program` |
   | `include sub/Makefile` | (each dir gets its own Tupfile) |

4. **Handle special cases:**
   - Assembly files: separate `!as` bang macro with appropriate flags
   - Generated headers: `!gen-config` pattern or explicit rules with `<gen-headers>` group
   - Negative conditionals: putup supports `ifneq` directly — write `ifneq (@(CONFIG),y)` (parens required); no `ifdef/else` rewrite needed. See the `putup:tupfile-authoring` migration patterns.
   - Recursive make: each subdirectory becomes a separate Tupfile

5. **Create Tuprules.tup template:**

   ```tup
   S ?= $(TUP_CWD)
   B ?= $(TUP_VARIANT_OUTPUTDIR)/$(S)
   CC ?= gcc
   CXX ?= g++
   AR ?= ar
   CFLAGS = -Wall -O2
   CFLAGS += -I$(S)/include
   !cc = |> ^ CC %b^ $(CC) $(CFLAGS) -c %f -o %o |> %B.o
   !cxx = |> ^ CXX %b^ $(CXX) $(CXXFLAGS) -c %f -o %o |> %B.o
   ```

6. **Validate** by running `putup parse -v` and fixing any errors.

7. **Report** what was converted, what needs manual attention (assembly, complex generators, recursive make), and suggest next steps (variant builds, `putup configure`).

## Important notes

- Each directory gets its own Tupfile — no recursive includes
- Use `srcs-@(CONFIG)` pattern for conditional compilation
- Config variables need `CONFIG_` prefix in tup.config but are accessed via `@(VAR)` (without prefix)
- Order-only groups `<gen-headers>` ensure generated files are built before consumers
- For detailed patterns, see the `putup:tupfile-authoring` skill
