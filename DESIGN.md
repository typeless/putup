# Pup Design Document

A reimplementation of the [Tup build system](https://gittup.org/tup/).

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Core Types](#core-types)
4. [Parser Module](#parser-module)
5. [Graph Module](#graph-module)
6. [Index Module](#index-module)
7. [Execution Module](#execution-module)
8. [Build Pipeline](#build-pipeline)
9. [Multi-Directory Builds](#multi-directory-builds)
10. [Variant Builds](#variant-builds)
11. [Design Decisions](#design-decisions)

---

## Overview

Pup reimplements tup with these goals:

- **Compatibility** - Parse existing Tupfiles without modification
- **Content hashing** - SHA-256 for precise change detection
- **Custom index format** - Binary format instead of SQLite
- **No FUSE** - Compute changes from index comparison
- **No Lua** - Traditional Tupfile syntax only

### Module Organization

```
pup/
├── include/pup/
│   ├── core/       # Fundamental types: Result, Hash, NodeId
│   ├── parser/     # Lexer, AST, Parser, Evaluator, Glob
│   ├── graph/      # DAG, Builder, Topological sort
│   ├── index/      # Binary format, Reader, Writer
│   └── exec/       # Scheduler, CommandRunner
├── src/            # Implementation files
└── third_party/    # sha256, expected-lite, Catch2
```

Dependencies flow one direction: `main → exec → graph → parser → core`

---

## Architecture

### High-Level Data Flow

```
Tupfile (text)
    │
    ▼ Lexer + Parser
Tupfile (AST)
    │
    ▼ GraphBuilder + Evaluator
BuildGraph (DAG)
    │
    ▼ Topological Sort
BuildJob[] (execution plan)
    │
    ▼ Scheduler + CommandRunner
Output files
    │
    ▼ IndexWriter
.pup/index (binary)
```

### Component Responsibilities

| Component | Responsibility |
|-----------|----------------|
| **Lexer** | Context-aware tokenization of Tupfile syntax |
| **Parser** | Recursive descent parsing, AST construction |
| **Evaluator** | Variable expansion, pattern substitution |
| **GraphBuilder** | AST to dependency graph transformation |
| **Scheduler** | Parallel job execution with dependency ordering |
| **IndexReader/Writer** | Binary index persistence |

---

## Core Types

### Identifiers

```cpp
using NodeId = std::uint64_t;
constexpr NodeId INVALID_NODE_ID = 0;
constexpr NodeId ROOT_NODE_ID = 1;
```

### Content Hash

```cpp
using Hash256 = std::array<std::byte, 32>;  // SHA-256
```

### File Timestamps

```cpp
struct FileTime {
    std::int64_t seconds;
    std::int32_t nanoseconds;
};
```

### Node Classification

```cpp
enum class NodeType : std::uint8_t {
    File,           // Source file
    Command,        // Build command
    Directory,      // Directory node
    Variable,       // Configuration variable
    Generated,      // Output file
    Ghost,          // Missing file placeholder
    Group,          // Named group {objs}
    GeneratedDir,   // Auto-created directory
    Root            // Project root
};
```

### Edge Classification

```cpp
enum class LinkType : std::uint8_t {
    Normal = 1,     // Standard dependency
    Sticky = 2,     // Explicit Tupfile dependency
    Group  = 3,     // Group membership
    Implicit = 4    // Header deps from .d files
};
```

### Error Handling

Pup uses `Result<T>` (alias for `expected<T, Error>`) for explicit error propagation:

```cpp
struct Error {
    ErrorCode code;
    std::string message;
};

template<typename T>
using Result = pup::expected<T, Error>;

// Usage
auto parse() -> Result<Tupfile>;

if (auto result = parse()) {
    process(*result);
} else {
    report(result.error());
}
```

Error codes are categorized:

| Category | Examples |
|----------|----------|
| General | InvalidArgument, NotFound, IoError |
| Index | IndexCorrupted, ChecksumMismatch |
| Parser | ParseError, UnterminatedString, CircularInclude |
| Graph | CyclicDependency, UnknownMacro |
| Exec | CommandFailed, MissingInput |

---

## Parser Module

### Lexer

The lexer is **context-aware** because Tupfile syntax changes meaning based on position:

```cpp
enum class Context {
    LineStart,      // Expect directive/rule/assignment
    Inputs,         // After ':', before '|>'
    Command,        // Between '|>' markers
    Outputs,        // After second '|>'
    Conditional     // Inside ifeq/ifneq
};
```

Context transitions:

```
LineStart ──':'──▶ Inputs ──'|>'──▶ Command ──'|>'──▶ Outputs ──'\n'──▶ LineStart
```

In **Command** context, the lexer performs minimal tokenization to preserve command text.

### Token Types

Key token categories:

| Category | Examples |
|----------|----------|
| Delimiters | `:`, `\|`, `\|>`, `{`, `}`, `(`, `)` |
| Operators | `=`, `:=`, `+=` |
| Special | `$`, `%`, `^`, `!`, `@`, `&` |
| Keywords | `foreach`, `include`, `ifdef`, `ifeq` |

### Abstract Syntax Tree

**Expression system** for variable references:

```cpp
struct VarRef {
    enum Kind { Regular, Config, Node };  // $(X), @(X), &(X)
    Kind kind;
    std::string name;
};

struct Expression {
    std::vector<std::variant<std::string, VarRef>> parts;
};
```

**Path patterns** for inputs/outputs:

```cpp
struct PathPattern {
    Expression path;
    bool is_foreach;
    bool is_exclusion;      // !pattern
    bool is_group;          // {name}
    std::string group_name;
};
```

**Rule** - the core build statement:

```cpp
struct Rule {
    bool foreach_;
    std::vector<PathPattern> inputs;
    std::vector<PathPattern> order_only_inputs;
    Expression command;
    Expression display;     // From ^ ^ markers
    std::vector<PathPattern> outputs;
    std::optional<std::string> output_group;
};
```

**Bang macro** - reusable command template:

```cpp
struct BangMacro {
    std::string name;       // !cc
    bool foreach_;
    Expression command;
    Expression display;
    std::vector<PathPattern> outputs;
};
```

### Parser

Recursive descent parser with these entry points:

```cpp
class Parser {
    auto parse() -> Result<Tupfile>;
    auto parse_statement() -> Result<Statement>;
    auto parse_rule() -> Result<Rule>;
    auto parse_expression() -> Result<Expression>;
    // ...
};
```

The parser uses a **FileResolver** interface for I/O abstraction:

```cpp
struct FileResolver {
    virtual auto resolve(path, relative_to) -> Result<std::filesystem::path> = 0;
    virtual auto read_file(path) -> Result<std::string> = 0;
    virtual auto find_tuprules(from_dir) -> Result<std::filesystem::path> = 0;
};
```

### Evaluator

**Variable database** with three namespaces:

```cpp
class VarDb {
    auto set(name, value) -> void;
    auto append(name, value) -> void;  // Space-separated
    auto get(name) -> std::string;
};

struct EvalContext {
    VarDb* vars;          // $(VAR)
    VarDb* config_vars;   // @(VAR)
    VarDb* node_vars;     // &(VAR)
    std::string tup_cwd, tup_platform, tup_arch;
    std::string tup_variantdir, tup_variant_outputdir;
};
```

**Pattern flags** for command/output substitution:

| Flag | Meaning | Example |
|------|---------|---------|
| `%f` | All inputs | `main.c util.c` |
| `%o` | All outputs | `main.o` |
| `%b` | Basename with ext | `main.c` |
| `%B` | Basename no ext | `main` |
| `%e` | Extension | `c` |
| `%d` | Directory | `src` |
| `%Nf` | Nth input | `%1f` → first input |

**Expansion pipeline:**

```
"$(CC) -c %f -o %o"
    │
    ▼ Variable expansion
"gcc -c %f -o %o"
    │
    ▼ Pattern substitution
"gcc -c main.c -o main.o"
```

### Glob Support

```cpp
class Glob {
    auto matches(filename) -> bool;
    auto is_literal() -> bool;
    auto is_recursive() -> bool;  // Contains **
};

auto glob_expand(pattern, base_dir, options) -> Result<std::vector<std::string>>;
```

Supported patterns: `*`, `?`, `[abc]`, `**`, `!pattern` (exclusion)

---

## Graph Module

### BuildGraph

Unified representation where all entities are nodes:

```cpp
struct Node {
    NodeId id;
    NodeType type;
    NodeFlags flags;
    std::string name;       // Basename only (tup-style identification)
    std::string command;    // For commands
    std::string display;    // Display text
    NodeId parent_dir;      // Parent directory node (used with name for lookup)
    Hash256 content_hash;
    FileTime mtime;
    std::vector<NodeId> inputs;
    std::vector<NodeId> outputs;
    std::vector<NodeId> order_only;
};

struct Edge {
    NodeId from, to;
    LinkType type;
    NodeId group_cmd_id;    // For group edges
};
```

**Path storage model**: Nodes store only their basename in `name`. Full paths are reconstructed by walking the `parent_dir` chain via `get_full_path()`, which caches results for efficiency.

Graph operations:

```cpp
class BuildGraph {
    auto add_node(Node) -> NodeId;
    auto add_edge(from, to, type) -> void;
    auto get_full_path(id) -> std::string;                    // Reconstruct from parent chain
    auto find_by_dir_name(parent_id, name) -> std::optional<NodeId>;  // O(1) lookup
    auto find_by_path(path) -> std::optional<NodeId>;         // Derived from get_full_path
    auto get_inputs(id) -> std::span<NodeId const>;
    auto get_outputs(id) -> std::span<NodeId const>;
    auto nodes_of_type(type) -> std::vector<NodeId>;
};
```

### Topological Sort

```cpp
struct TopoSortResult {
    std::vector<NodeId> order;
    bool has_cycle;
    std::vector<NodeId> cycle;  // Path if cycle found
};

auto topological_sort(graph) -> TopoSortResult;
auto reverse_topological_sort(graph) -> TopoSortResult;
auto detect_cycles(graph) -> std::optional<std::vector<NodeId>>;
```

Additional graph analysis:

```cpp
auto reachable_from(graph, start) -> std::unordered_set<NodeId>;
auto node_depth(graph, id) -> std::size_t;
auto critical_path(graph) -> std::vector<NodeId>;
```

### GraphBuilder

Transforms AST to BuildGraph:

```cpp
struct BuilderOptions {
    std::filesystem::path root_dir;
    std::filesystem::path variant_dir;
    bool expand_globs;
    bool validate_inputs;
};

class GraphBuilder {
    auto build(Tupfile, EvalContext) -> Result<BuildGraph>;
};
```

**Rule expansion process:**

```
Rule { inputs: ["*.c"], outputs: ["%B.o"], foreach: true }
    │
    ▼ expand_inputs() via glob
["main.c", "util.c"]
    │
    ▼ For each input (foreach=true):
    │
    ├── expand_outputs() with PatternFlags
    │   %B.o → main.o
    │
    ├── expand_command() with substitution
    │   gcc -c %f -o %o → gcc -c main.c -o main.o
    │
    ├── Create nodes:
    │   - File node for main.c
    │   - Command node for "gcc -c main.c -o main.o"
    │   - Generated node for main.o
    │
    └── Create edges:
        main.c → command → main.o
```

**Bang macro substitution:**

```
!cc = |> ^ CC %o^ gcc -c %f -o %o |> %B.o

: foreach *.c |> !cc |> {objs}
```

When `!cc` is referenced, the builder substitutes the stored macro definition.

**Group management:**

```cpp
struct BuilderContext {
    std::unordered_map<std::string, BangMacroDef> macros;
    std::unordered_map<std::string, std::vector<NodeId>> groups;
};
```

---

## Index Module

### Binary Format

```
┌─────────────────────────────────────┐
│ Header (64 bytes)                   │
│   magic: "PUPI"                     │
│   version: 1                        │
│   file_count, command_count, ...    │
│   offsets to each section           │
├─────────────────────────────────────┤
│ FileEntry[] (96 bytes each)         │
│   id, parent_id, type, flags        │
│   path_offset, path_length          │
│   mtime, size, content_hash         │
├─────────────────────────────────────┤
│ CommandEntry[] (64 bytes each)      │
│   id, dir_id                        │
│   cmd_offset, cmd_length            │
│   display_offset, display_length    │
├─────────────────────────────────────┤
│ Edge[] (24 bytes each)              │
│   from_id, to_id, type              │
├─────────────────────────────────────┤
│ String Table                        │
│   Packed null-terminated strings    │
├─────────────────────────────────────┤
│ Footer (32 bytes)                   │
│   SHA-256 checksum of above         │
└─────────────────────────────────────┘
```

Design principles:
- Fixed-size entries for O(1) random access
- String table for deduplication
- Checksum for corruption detection
- Aligned structures for efficient access

### IndexReader

Memory-mapped for efficiency:

```cpp
class IndexReader {
    static auto open(path) -> Result<IndexReader>;
    auto read() -> Result<Index>;
    auto verify_checksum() -> bool;
    auto header() -> RawHeader const*;
    auto get_string(offset, length) -> std::string_view;
};
```

### IndexWriter

Atomic writes prevent corruption:

```cpp
class IndexWriter {
    static auto write(path, index) -> Result<Unit>;
};
```

Write process:
1. Serialize to temporary file
2. Compute SHA-256 checksum
3. Write footer with checksum
4. Atomic rename to final path

---

## Execution Module

### CommandRunner

```cpp
struct CommandResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds duration;
    bool timed_out;
    bool signaled;
    int signal;
};

struct RunOptions {
    std::filesystem::path working_dir;
    std::vector<std::string> env;
    bool inherit_env;
    std::optional<std::chrono::seconds> timeout;
    bool capture_stdout, capture_stderr;
};

class CommandRunner {
    auto run(command) -> Result<CommandResult>;
    auto run(command, options) -> Result<CommandResult>;
};
```

### Scheduler

```cpp
struct BuildJob {
    NodeId id;
    std::string command;
    std::string display;
    std::filesystem::path working_dir;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<std::string> order_only_inputs;
};

struct SchedulerOptions {
    std::size_t jobs;       // 0 = auto-detect
    bool keep_going;
    bool dry_run;
    bool verbose;
    std::optional<std::chrono::seconds> timeout;
};

class Scheduler {
    auto build(graph) -> Result<BuildStats>;
    auto build_incremental(graph, old_index, changed) -> Result<BuildStats>;
    auto on_job_start(callback) -> void;
    auto on_job_complete(callback) -> void;
    auto cancel() -> void;
};
```

**Parallel execution algorithm:**

```
1. Compute in_degree[job] = count of dependencies
2. Queue jobs where in_degree == 0
3. Thread pool processes queue:
   a. Dequeue job
   b. Execute command
   c. On completion:
      - For each dependent job:
        - Decrement in_degree
        - If in_degree == 0: enqueue
4. Repeat until queue empty or cancelled
```

This ensures:
- Maximum parallelism within dependency constraints
- Order-only dependencies respected for ordering only
- Automatic load balancing across threads

---

## Build Pipeline

### Complete Build Flow

```cpp
// 1. Find project root
auto root = find_project_root();

// 2. Parse Tupfile
auto parser = Parser { read_file(root / "Tupfile"), resolver };
auto tupfile = parser.parse();

// 3. Load configuration
auto config = parse_config(variant_dir / "tup.config");

// 4. Create evaluation context
auto vars = VarDb {};
auto ctx = EvalContext {
    .vars = &vars,
    .config_vars = &config,
    .tup_cwd = root.string(),
    .tup_platform = detect_platform(),
    .tup_variantdir = compute_variantdir(root, variant_dir),
};

// 5. Build graph
auto builder = GraphBuilder { options };
auto graph = builder.build(*tupfile, ctx);

// 6. Execute build
auto scheduler = Scheduler { sched_options };
scheduler.on_job_start([](job) { print_start(job); });
scheduler.on_job_complete([](result) { print_result(result); });
auto stats = scheduler.build(*graph);

// 7. Save index for incremental builds
IndexWriter::write(root / ".pup" / "index", make_index(*graph));
```

### Incremental Build

```cpp
// 1. Load previous index
auto reader = IndexReader::open(root / ".pup" / "index");
auto old_index = reader.read();

// 2. Find changed files (mtime → size → hash)
auto changed = find_changed_files(root, *old_index);

// 3. Expand with implicit dependencies
changed = expand_with_implicit_deps(changed, *old_index);

// 4. Incremental build
auto stats = scheduler.build_incremental(*graph, *old_index, changed);
```

**Change detection algorithm:**
1. Compare modification times - if different, rebuild
2. If mtime matches, compare file sizes - if different, rebuild
3. If size matches, compute SHA-256 hash - if different, rebuild

This hierarchy minimizes expensive hash computations while ensuring correctness.

### Implicit Header Dependencies

Pup automatically tracks header dependencies discovered at compile time:

```
┌─────────────┐     compile     ┌─────────────┐
│   main.c    │ ──────────────▶ │   main.o    │
└─────────────┘                 └─────────────┘
                                      │
                                      │ generates
                                      ▼
                                ┌─────────────┐
                                │   main.d    │  (depfile)
                                └─────────────┘
```

**Depfile format** (generated by `gcc -MD`):
```makefile
main.o: main.c \
  include/header.h \
  /usr/include/stdio.h
```

**Tracking flow:**
1. Command succeeds → scheduler parses `.d` file
2. Discovered headers stored as `LinkType::Implicit` edges
3. Index persisted to `.pup/index`
4. On rebuild, changed headers → affected commands via reverse lookup

**All headers tracked** including system headers (`/usr/include/*`), ensuring complete rebuild correctness.

Only commands transitively depending on changed files are re-executed.

---

## Multi-Directory Builds

Pup supports projects with Tupfiles in multiple subdirectories, enabling modular project organization.

### Tupfile Discovery

On startup, pup recursively scans for all directories containing `Tupfile`:

```cpp
auto discover_tupfile_dirs(root) -> std::set<std::filesystem::path>
{
    // Skip variant directories (contain tup.config)
    // Return set of relative paths to Tupfile directories
}
```

### Demand-Driven Parsing

Instead of parsing all Tupfiles upfront, pup uses **demand-driven parsing**:

```
1. Start with root Tupfile
2. When a rule references a path in another directory:
   - Check if that directory has a Tupfile
   - Parse it if not already parsed
3. Repeat until all dependencies resolved
4. Parse remaining "orphan" Tupfiles
```

This approach:
- Handles cross-directory dependencies correctly
- Detects circular Tupfile dependencies
- Minimizes unnecessary parsing

### Parse State Tracking

```cpp
struct TupfileParseState {
    std::set<std::filesystem::path> available;  // Dirs with Tupfiles
    std::set<std::filesystem::path> parsed;     // Already processed
    std::set<std::filesystem::path> parsing;    // Currently processing (cycle detection)
};
```

The `parsing` set detects circular dependencies - if a directory appears while still being parsed, it's a cycle.

### Per-Directory Variable Scope

Each Tupfile gets its own variable scope:

```cpp
struct BuilderContext {
    std::filesystem::path current_dir;
    parser::Variables local_vars;       // $(VAR) - local to this Tupfile
    std::unordered_map<std::string, BangMacroDef> macros;  // !macros
    std::unordered_map<std::string, std::vector<NodeId>> groups;  // {bins}
};
```

Variables defined in one Tupfile don't leak to others. Bang macros are inherited through `include_rules` from parent `Tuprules.tup` files.

### Cross-Directory Groups

Groups can be referenced across directories using path prefixes:

```tup
# In include/generated/Tupfile
: config.in |> gen-headers.sh |> headers.h <gen-headers>

# In src/Tupfile
: foo.c | $(ROOT)/include/generated/<gen-headers> |> $(CC) -c %f -o %o |> foo.o
```

Group keys are `(directory, name)` tuples:

```cpp
using GroupKey = std::pair<std::filesystem::path, std::string>;
std::map<GroupKey, std::vector<NodeId>> order_only_groups_;
```

### TUP_CWD Computation

`TUP_CWD` is the relative path from the current Tupfile back to the directory containing the root `Tuprules.tup`:

```cpp
// For include/generated/Tupfile:
// TUP_CWD = "../.."

// Common pattern:
ROOT = $(TUP_CWD)
CFLAGS += -I$(ROOT)/include
```

---

## Variant Builds

Variant builds allow out-of-tree compilation with different configurations.

### Directory Structure

```
project/
├── Tupfile.ini          # Project root marker
├── Tuprules.tup         # Shared rules
├── src/
│   └── Tupfile
└── build-debug/         # Variant directory
    ├── tup.config       # Variant configuration
    └── src/             # Output mirrors source structure
        └── foo.o
```

### Key Variables

| Variable | Value | Purpose |
|----------|-------|---------|
| `TUP_CWD` | Relative path to Tuprules.tup directory | Access shared files |
| `TUP_VARIANTDIR` | `build-debug` | Variant directory name |
| `TUP_VARIANT_OUTPUTDIR` | Relative path from source to variant output | Output paths |

### Path Resolution

**The fundamental challenge**: Commands run from their source directory, but outputs go to the variant directory.

```
Source:  src/Tupfile contains: : foo.c |> $(CC) -c %f -o %o |> foo.o
Command runs from: project/src/
Output goes to: project/build-debug/src/foo.o
```

### Path Transformation Pipeline

1. **Input expansion** (`expand_inputs`):
   - Glob patterns resolved against source directory
   - Results stored as project-root-relative paths

2. **Output expansion** (`expand_outputs`):
   - Outputs prefixed with variant directory
   - `foo.o` → `build-debug/src/foo.o`

3. **Command generation** (`expand_command`):
   - Pattern flags (`%f`, `%o`) substituted
   - Paths transformed from project-root-relative to source-dir-relative:

   ```cpp
   // From src/ (depth=1), accessing build-debug/src/foo.o:
   // Path: "build-debug/src/foo.o" → "../build-debug/src/foo.o"

   auto make_source_relative = [&](std::string const& path) {
       if (path.empty() || source_to_root.empty())
           return path;
       if (path starts with "../")  // Already relative
           return path;
       return source_to_root + path;  // e.g., "../" + path
   };
   ```

### tup.config Handling

The variant's `tup.config` exists only in the variant directory, not the source tree. When a Tupfile references `../../tup.config`, pup maps it to `<variant>/tup.config`:

```cpp
if (full_path.filename() == "tup.config" && !variant_dir.empty()) {
    auto variant_config = variant_dir / "tup.config";
    if (std::filesystem::exists(root_dir / variant_config))
        return variant_config;
}
```

### Working Directory

Commands execute from their **source directory**, not the variant directory:

```cpp
// In scheduler.cpp
auto working_dir = options_.root_dir;
if (!node->source_dir.empty())
    working_dir /= node->source_dir;  // e.g., "src"
```

This ensures:
- Relative paths in commands (like `$(ROOT)/scripts/tool`) resolve correctly
- `TUP_VARIANT_OUTPUTDIR` provides the path to variant outputs

### Example Flow

```
Tupfile in src/:
: foo.c |> $(CC) -c %f -o $(TUP_VARIANT_OUTPUTDIR)/foo.o |> $(TUP_VARIANT_OUTPUTDIR)/foo.o

Variables:
- TUP_CWD = ".."
- TUP_VARIANT_OUTPUTDIR = "../build-debug/src"

Generated command (runs from project/src/):
gcc -c foo.c -o ../build-debug/src/foo.o

Output path in graph:
build-debug/src/foo.o  (project-root-relative)
```

---

## Design Decisions

### Why Result<T> Instead of Exceptions?

- Explicit error paths at type level
- No hidden control flow
- Matches Rust's approach
- Zero-cost when no error

### Why Context-Aware Lexing?

Tupfile syntax is inherently context-sensitive:

```
: foo.c |> gcc -c %f -o %o |> foo.o
  ^^^^^    ^^^^^^^^^^^^^^^    ^^^^^
  inputs   command (text)     outputs
```

The same characters mean different things in different positions. Context-aware lexing simplifies the grammar significantly.

### Why Unified Node Representation?

All entities (files, commands, directories, groups) as nodes:

- Single traversal algorithm for all
- Uniform edge representation
- Simplified topological sort
- Easier incremental reasoning

### Why Binary Index Instead of SQLite?

- Simpler dependencies (no SQLite)
- Memory-mapped for speed
- Atomic updates via rename
- Portable binary format
- Direct structure access

### Why No FUSE?

Original tup uses FUSE to intercept file access. Pup instead:

- Computes dependencies from Tupfile declarations
- Uses content hashing for change detection
- Compares index state vs filesystem
- Trades implicit detection for explicit declaration

### Why Variant System?

Out-of-tree builds via variant directories:

```
project/
├── Tupfile
├── src/
└── build/           # Variant directory
    ├── tup.config   # Variant configuration
    ├── *.o          # Build artifacts
    └── pup          # Output binary
```

Benefits:
- Clean source tree
- Multiple configurations (debug/release)
- Easy cleanup (rm -rf build/)
- Parallel variant builds

### Why Parallel Execution with Dependency Map?

The work-queue algorithm with in-degree counting:

- True parallelism (no global lock on queue operations)
- Respects all dependencies exactly
- Automatic load balancing
- Order-only dependencies for sequencing without rebuild triggers
- Cancellation support

### Why Track All Headers Including System?

Unlike some build systems that filter out system headers:

- **Correctness** - System header changes (e.g., glibc update) should trigger recompilation
- **Simplicity** - No heuristics for deciding "what matters"
- **Reproducibility** - Same source + same headers = same build
- **Minimal overhead** - Header paths stored once per command, hash computed only when mtime differs

### Why (parent_dir, name) Path Storage?

Like tup, pup stores paths as `(parent_dir, basename)` pairs rather than full path strings:

- **No path format mismatches** - No canonicalization bugs from `./foo` vs `foo` vs `/abs/foo`
- **Smaller storage** - Basenames are short; parent relationship is a single NodeId
- **O(1) lookup** - Hash on `(parent_id, name)` gives instant lookup
- **Efficient caching** - `get_full_path()` caches reconstructed paths

The `find_by_path()` method remains for compatibility, but internally derives from the `(parent_dir, name)` model.

---

## Appendix: Tupfile Syntax Reference

### Rules

```
: [foreach] inputs [| order-only] |> command |> outputs [{group}]
```

### Variables

```
VAR = value           # Assignment
VAR += value          # Append
VAR := value          # No expansion
$(VAR)                # Regular reference
@(CONFIG_VAR)         # Config reference
&(NODE_VAR)           # Node reference
```

### Bang Macros

```
!cc = |> ^ CC %o^ $(CC) -c %f -o %o |> %B.o
: foreach *.c |> !cc |> {objs}
```

### Conditionals

```
ifdef VAR
endif

ifeq ($(VAR),value)
else
endif
```

### Directives

```
include path
include_rules
export VAR
import VAR[=default]
```

### Built-in Variables

| Variable | Description |
|----------|-------------|
| `TUP_CWD` | Current Tupfile directory |
| `TUP_PLATFORM` | Operating system |
| `TUP_ARCH` | CPU architecture |
| `TUP_VARIANTDIR` | Relative path to variant |
| `TUP_VARIANT_OUTPUTDIR` | Absolute variant path |
