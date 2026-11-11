# Putup

A build system using the [Tupfile](https://gittup.org/tup/) format.

**Install:**

```bash
curl -fsSL https://raw.githubusercontent.com/typeless/putup/main/install.sh | sh
```

> **Note:** The binary is named `putup`, but `pup` works as an alias.

```
putup - build system using Tupfile format

Usage: putup [OPTIONS] [TARGETS]
       putup [OPTIONS] <command>

Running 'putup' executes the build. Use a command for other operations.

Commands:
  configure         Generate tup.config files (two-stage build)
  clean             Remove generated files
  distclean         Full reset: remove .pup and variant directory
  parse [--check=LEVEL]
                    Parse and validate Tupfiles
  show <format>     Show build info:
                      script  - Shell script
                      compdb  - compile_commands.json
                      graph   - DOT format (--summary for text)
                      var [NAME] [--json] - Variable tracking
                      index   - Index dump (--summary for counts only)

Options:
  -j, --jobs N       Run N jobs in parallel
  -k, --keep-going   Continue after failures
  --no-stat-cache    Hash every file's contents (skip the size+mtime fast path)
  --rerun            Re-execute up-to-date commands (scoped by targets if given)
  -n, --dry-run      Print commands without executing
  -v, --verbose      Name each command by its full command line
  -q, --quiet        Name only commands that fail or write output
  -D, --define VAR=value
                     Override config variable (-D VAR is shorthand for -D VAR=y)
  -S DIR             Source directory (where source files live)
  -C DIR             Config directory (where Tupfiles live)
  -B DIR             Build/output directory (can use multiple times)
  -c, --config FILE  Install FILE as root tup.config before config rules
  --check=LEVEL      Convention checking for 'parse':
                       none  - skip checks
                       warn  - report violations, exit 0 (default)
                       error - report violations, exit non-zero
                     (--strict is an alias for --check=error)
  --summary          Human-readable output (for show graph)
  --stat             Print build statistics
  -A, --all          Full project build (ignore cwd scoping)
  -a, --all-deps     Include upstream deps in scoped builds
  -x, --exclude PATTERN
                     Skip building directories matching PATTERN (gitignore
                     syntax); their index state is preserved, not discarded
  --                 End of options; remaining args are targets
  --version          Print version
  -h, --help         Print this help

Targets:
  A variant is a directory containing tup.config. Anything else is a scope.

  TARGET              VARIANT       SCOPE
  build               build         (all)       # if build/tup.config exists
  build/src/lib       build         src/lib
  src/lib             (none)        src/lib     # no tup.config in src/
  build/foo.o         build         foo.o       # single output rebuild
  build-*             (glob)        (all)       # multiple variants

Examples:
  putup                Build in-tree, or the enclosing build dir when run inside one
  putup build-debug    Build single variant
  putup build-*        Build all matching variants
  putup src/lib        Scoped build (variant selection rules above apply)

Environment:
  PUP_SOURCE_DIR     Source directory (overridden by -S)
  PUP_CONFIG_DIR     Config directory (overridden by -C)
  PUP_BUILD_DIR      Build directory (overridden by -B)
  PUP_IMPLICIT_DEPS  Set to 0 to disable auto-generated dep rules (default: enabled)
```

## Documentation

Using putup:

- **[Installation](INSTALL.md)** - Install script, building from source, bootstrapping
- **[Reference Manual](docs/reference.md)** - Commands, Tupfile syntax, configuration, build modes, troubleshooting
- **[Compatibility](COMPATIBILITY.md)** - What putup shares with tup, where it differs, and moving from tup
- **[Migrating from Make/Kbuild](docs/tupfile-migration-guide.md)** - Patterns for converting an existing build to Tupfiles
- **[Examples](examples/)** - `helloworld`, `busybox`, and `bsp` (a cross-compiling GCC toolchain)

Working on putup:

- **[CLAUDE.md](CLAUDE.md)** - Developer guide: building, testing, workflow
- **[Testing](TESTING.md)** - Test workflow, tags, E2E fixtures, and coverage reports
- **[STYLE.md](STYLE.md)** - C++ code style guide
- **[DESIGN.md](DESIGN.md)** - Internal architecture and design rationale
- **[spec/requirements](spec/requirements/README.md)** - Machine-checked behaviour requirements
- **[docs/design](docs/design/)** - Open design proposals; finished ones are in [docs/archive](docs/archive/)

## Claude Code Plugin

Putup ships a [Claude Code](https://claude.ai/code) plugin with skills for Tupfile authoring, project setup, cross-compilation, composable libraries, and contributing.

**For contributors** (working in this repo):
```bash
claude --plugin-dir plugins/putup
```

**For external users** (install from marketplace):
```
/plugin marketplace add typeless/putup
/plugin install putup@typeless-putup
```

Skills: `/putup:tupfile-authoring`, `/putup:project-setup`, `/putup:cross-compile`, `/putup:composable-libraries`, `/putup:contributing`

## License

MIT License - see [LICENSE](LICENSE) for details.
