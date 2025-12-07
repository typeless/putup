# Pup Design Document

A modern C++20 reimplementation of the [Tup build system](https://gittup.org/tup/).

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Core Types](#core-types)
4. [Parser Module](#parser-module)
5. [Graph Module](#graph-module)
6. [Index Module](#index-module)
7. [Execution Module](#execution-module)
8. [Build Pipeline](#build-pipeline)
9. [Design Decisions](#design-decisions)

---

## Overview

Pup reimplements tup with these goals:

- **Compatibility** - Parse existing Tupfiles without modification
- **Modern C++20** - Minimal dependencies, clean abstractions
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
    Group  = 3      // Group membership
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
    std::string path;       // For files
    std::string command;    // For commands
    std::string display;    // Display text
    NodeId parent_dir;
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

Graph operations:

```cpp
class BuildGraph {
    auto add_node(Node) -> NodeId;
    auto add_edge(from, to, type) -> void;
    auto find_by_path(path) -> std::optional<NodeId>;
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

// 2. Find changed files
auto changed = find_changed_files(root, *old_index);

// 3. Incremental build
auto stats = scheduler.build_incremental(*graph, *old_index, changed);
```

Change detection compares:
- File modification times
- File sizes
- Content hashes (SHA-256)

Only commands transitively depending on changed files are re-executed.

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
