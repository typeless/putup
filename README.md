# Putup

A build system using the [Tupfile](https://gittup.org/tup/) format.

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
  parse             Parse and validate Tupfiles
  show <format>     Show build info:
                      script  - Shell script
                      compdb  - compile_commands.json
                      graph   - DOT format (--summary for text)
                      var [NAME] [--json] - Variable tracking

Options:
  -j, --jobs N       Run N jobs in parallel
  -k, --keep-going   Continue after failures
  -n, --dry-run      Print commands without executing
  -v, --verbose      Verbose output
  -D, --define VAR=value
                     Override config variable (-D VAR is shorthand for -D VAR=y)
  -S DIR             Source directory (where source files live)
  -C DIR             Config directory (where Tupfiles live)
  -B DIR             Build/output directory (can use multiple times)
  -c, --config FILE  Use FILE as tup.config (skip config rules)
  --summary          Human-readable output (for show graph)
  --stat             Print build statistics
  -A, --all          Full project build (ignore cwd scoping)
  -a, --all-deps     Include upstream deps in scoped builds
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
  putup                Build all variants
  putup build-debug    Build single variant
  putup build-*        Build all matching variants
  putup src/lib        Scoped build across all variants

Environment:
  PUP_SOURCE_DIR     Source directory (overridden by -S)
  PUP_CONFIG_DIR     Config directory (overridden by -C)
  PUP_BUILD_DIR      Build directory (overridden by -B)
  PUP_IMPLICIT_DEPS  Set to 0 to disable auto-generated dep rules (default: enabled)
```

## Documentation

- **[Installation](INSTALL.md)** - Requirements, building from source, bootstrapping
- **[Reference Manual](docs/reference.md)** - Complete user guide: commands, Tupfile syntax, configuration
- **[Compatibility](COMPATIBILITY.md)** - Tup compatibility matrix and migration guide
- **[CLAUDE.md](CLAUDE.md)** - Developer guide: building, testing, project structure
- **[STYLE.md](STYLE.md)** - C++ code style guide
- **[DESIGN.md](DESIGN.md)** - Internal architecture and design rationale

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
