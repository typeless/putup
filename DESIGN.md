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
using NodeId = std::uint32_t;
constexpr NodeId INVALID_NODE_ID = 0;  // Sentinel for "no parent" (source root level)
constexpr NodeId SOURCE_ROOT_ID = 0;   // Parent of source files and directories
constexpr NodeId BUILD_ROOT_ID = 1;    // Parent of Generated/Ghost nodes (variant builds)
```

Note: `INVALID_NODE_ID` and `SOURCE_ROOT_ID` are both 0, used interchangeably. Top-level
source files have parent=0, meaning they're at source root level. For variant builds,
Generated/Ghost nodes are stored under `BUILD_ROOT_ID` at source-relative paths.

### Content Hash

```cpp
using Hash256 = std::array<std::byte, 32>;  // SHA-256
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
    bool is_exclusion;          // !pattern for input exclusion
    bool is_output_exclusion;   // ^pattern for output exclusion (regex)
    bool is_group;              // {binname} - tup calls these "bins"
    bool is_order_only_group;   // <groupname> for order-only groups
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
    std::optional<Expression> display;  // From ^ ^ markers
    std::vector<PathPattern> outputs;
    std::vector<PathPattern> extra_outputs;
    std::optional<std::string> output_group;               // {binname} at end
    std::optional<std::string> output_order_only_group;    // <groupname> at end
    std::optional<Expression> output_order_only_group_dir; // path/ prefix for <group>
};
```

**Bang macro** - reusable command template:

```cpp
struct BangMacro {
    std::string name;       // !cc
    bool foreach_;
    std::vector<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display;
    std::vector<PathPattern> outputs;
    std::vector<PathPattern> extra_outputs;
    std::optional<std::string> output_group;               // {binname} at end
    std::optional<std::string> output_order_only_group;    // <groupname> at end
    std::optional<Expression> output_order_only_group_dir; // path/ prefix for <group>
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
    VarDb* vars;                // $(VAR)
    VarDb const* config_vars;   // @(VAR) - read-only
    VarDb* node_vars;           // &(VAR)
    std::string tup_cwd, tup_platform, tup_arch;
    std::string tup_variantdir, tup_variant_outputdir;

    // Callbacks for cross-directory resolution
    std::function<std::vector<std::string>(std::string_view)> resolve_group;           // {groupname}
    std::function<std::vector<std::string>(std::string_view)> resolve_order_only_group; // <groupname>
    std::function<Result<void>(std::filesystem::path const&)> request_directory;       // Demand-driven parsing
    std::set<std::filesystem::path> const* available_tupfile_dirs;

    // Callbacks for fine-grained dependency tracking
    std::function<void(std::string_view)> on_config_var_used;
    std::unordered_set<std::string> const* imported_vars;
    std::function<void(std::string_view)> on_env_var_used;
};
```

**Pattern flags** for command/output substitution:

| Flag | Meaning | Example |
|------|---------|---------|
| `%f` | All inputs | `main.c util.c` |
| `%i` | All inputs (alias) | `main.c util.c` |
| `%o` | All outputs | `main.o` |
| `%O` | Output basename | `main` |
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
    std::string source_dir; // For commands: Tupfile directory (relative to root)
    NodeId parent_dir;      // Parent directory node (used with name for lookup)
    Hash256 content_hash;
    std::vector<NodeId> inputs;
    std::vector<NodeId> outputs;
    std::vector<NodeId> order_only;
    std::set<std::string> exported_vars;  // Env vars to export to command
    std::optional<GeneratedOutput> generated_output;  // Output specification
    OutputAction output_action;           // What to do with output
    NodeId parent_command;                // Parent command for InjectImplicitDeps
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

### Ghost Nodes

Ghost nodes are placeholder nodes for files that don't exist yet during parsing. They enable cross-directory dependencies in variant builds where parse order matters.

**The problem**: When subdirectories are parsed alphabetically, a consumer directory (e.g., `aaa_consumer`) may be parsed before a producer directory (e.g., `zzz_producer`). If the consumer references a file that will be generated by the producer, that file doesn't exist yet.

**Solution**: When `resolve_input_node()` encounters a non-existent file:
1. Create a Ghost node as a placeholder
2. Establish dependency edges from the command to the ghost
3. Later, when the producer directory is parsed and creates the output, upgrade Ghost→Generated

**Ghost→Generated upgrade**: When a rule declares an output that already exists as a Ghost:
- The node's type changes from `Ghost` to `Generated`
- **All existing edges are preserved** - commands that depend on the ghost now depend on the generated file

**Why edges are preserved**: Unlike Tup (which deletes edges and re-parses dependent Tupfiles after upgrade), pup parses all Tupfiles fresh each build. The dependency edges created when the ghost was first referenced are already correct—they just point to a placeholder that becomes real.

**Validation**: Before build execution, the scheduler validates that no Ghost nodes remain with dependents. An unrealized ghost indicates a missing input file:

```cpp
// scheduler.cpp
if (node->type == NodeType::Ghost && !node->outputs.empty())
    return error("Missing input file (unresolved ghost): " + path);
```

**Index serialization**: Ghost nodes are transient—they're either upgraded to Generated during parsing or caught as errors. They're never written to the index.

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
    ├── Build PatternFlags once:
    │   - Transform inputs to Tupfile-relative paths
    │   - Extract glob_match (%g) from pattern + primary input
    │   - Populate input fields (%f, %B, %b, %e, %g)
    │
    ├── expand_outputs() with PatternFlags
    │   %B.o → main.o
    │
    ├── expand_command() with PatternFlags + outputs
    │   - Augment flags with output fields (%o, %O)
    │   - gcc -c %f -o %o → gcc -c main.c -o main.o
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

### Binary Format (v7)

```
┌─────────────────────────────────────┐
│ Header (40 bytes)                   │
│   magic: "PUPI" (4 bytes)           │
│   version: u32 (7)                  │
│   file_count: u32                   │
│   command_count: u32                │
│   edge_count: u32                   │
│   string_table_size: u32            │
│   file_offset: u32                  │
│   command_offset: u32               │
│   edge_offset: u32                  │
│   string_offset: u32                │
├─────────────────────────────────────┤
│ FileEntry[] (56 bytes each)         │
│   parent_id: u32                    │
│   src_id: u32                       │
│   name_offset: u32                  │
│   type: u8, flags_low/high: u16     │
│   reserved: 1 byte                  │
│   size: u64                         │
│   content_hash: [u8; 32]            │
│   (id computed from array index)    │
├─────────────────────────────────────┤
│ CommandEntry[] (16 bytes each)      │
│   dir_id: u32                       │
│   cmd_offset: u32                   │
│   display_offset: u32               │
│   env_offset: u32                   │
│   (id = index | 0x80000000)         │
├─────────────────────────────────────┤
│ Edge[] (16 bytes each)              │
│   from_id: u32                      │
│   to_id: u32                        │
│   type: u8, reserved: 3 bytes       │
│   group_cmd_id: u32                 │
├─────────────────────────────────────┤
│ String Table (length-prefixed)      │
│   [0]: u16(0) - empty string entry  │
│   [...]: u16(len) + data bytes      │
│   Deduplicated via offset reuse     │
├─────────────────────────────────────┤
│ Footer (32 bytes)                   │
│   checksum: [u8; 32] (SHA-256)      │
└─────────────────────────────────────┘
```

Version history:
- v1: Initial format with full path strings
- v2: Added name field for (parent_dir, name) identification
- v3: Removed path field, paths reconstructed from parent chain
- v4: Directory content_hash stores Merkle hash
- v5: Removed mtime, change detection uses size + content hash
- v6: Compact format: 32-bit IDs/offsets, length-prefixed strings
- v7: Tagged ID spaces (files vs commands), ID computed from array index

Design principles:
- Fixed-size entries for O(1) random access
- Parent-child hierarchy for path storage (like tup)
- Length-prefixed strings (u16 length, 64KB max per string)
- String table with deduplication
- SHA-256 checksum for corruption detection
- 8-byte aligned structures for efficient memory access
- Bounds checking on all offset/count fields
- 32-bit IDs sufficient for ~4B nodes (AOSP has <10M)
- 32-bit offsets sufficient for 4GB index files

### IndexReader

Memory-mapped for efficiency:

```cpp
class IndexReader {
    static auto open(path) -> Result<IndexReader>;
    auto read() -> Result<Index>;
    auto verify_checksum() -> bool;
    auto header() -> RawHeader const*;
    auto get_string(offset) -> std::string_view;  // length from string table
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
    int exit_code = 0;
    std::string stdout_output = {};
    std::string stderr_output = {};
    std::chrono::milliseconds duration = {};
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

struct RunOptions {
    std::filesystem::path working_dir = {};
    std::vector<std::string> env = {};           // Additional environment variables
    bool inherit_env = true;                     // Inherit parent environment
    std::optional<std::chrono::seconds> timeout = {};
    bool capture_stdout = true;
    bool capture_stderr = true;
    std::optional<std::string> stdin_data = {};  // Data to pipe to stdin
};

using OutputCallback = std::function<void(std::string_view, bool is_stderr)>;

class CommandRunner {
    auto run(command) -> Result<CommandResult>;
    auto run(command, options) -> Result<CommandResult>;
    auto run_with_output(command, callback, options) -> Result<CommandResult>;
    auto set_working_dir(dir) -> void;
    auto add_env(var) -> void;
    auto set_timeout(timeout) -> void;
};
```

### Scheduler

```cpp
struct BuildJob {
    NodeId id = 0;
    std::string command = {};
    std::string display = {};
    std::filesystem::path working_dir = {};
    std::vector<std::string> inputs = {};
    std::vector<std::string> outputs = {};
    std::vector<std::string> order_only_inputs = {};  // Order-only dependencies
    std::set<std::string> exported_vars = {};         // Env vars to export to command

    // For auto-generated rules (from pattern matching)
    bool capture_stdout = false;             // Capture stdout for depfile parsing
    bool inject_implicit_deps = false;       // Parse stdout as depfile
    NodeId parent_command = INVALID_NODE_ID; // Parent command for implicit deps
};

struct JobResult {
    NodeId id = 0;
    bool success = false;
    int exit_code = 0;
    std::string output = {};
    std::chrono::milliseconds duration = {};
    std::vector<std::string> discovered_deps = {};  // Implicit deps from .d files
    NodeId deps_for_command = INVALID_NODE_ID;      // If set, deps belong to this command (not id)
};

struct BuildStats {
    std::size_t total_jobs = 0;
    std::size_t completed_jobs = 0;
    std::size_t failed_jobs = 0;
    std::size_t skipped_jobs = 0;
    std::chrono::milliseconds total_time = {};
    std::chrono::milliseconds build_time = {};  // Time spent in commands
};

struct SchedulerOptions {
    std::size_t jobs = 0;                             // 0 = auto-detect
    bool keep_going = false;                          // Continue after failures
    bool dry_run = false;                             // Print commands without executing
    bool verbose = false;                             // Print commands as they run
    std::filesystem::path source_root = {};           // Source tree root (where Tupfile.ini lives)
    std::filesystem::path output_root = {};           // Output tree root (where outputs/.pup go)
    std::optional<std::chrono::seconds> timeout = {}; // Per-command timeout
};

using JobStartCallback = std::function<void(BuildJob const&)>;
using JobCompleteCallback = std::function<void(BuildJob const&, JobResult const&)>;
using ProgressCallback = std::function<void(std::size_t completed, std::size_t total)>;

class Scheduler {
    auto build(graph) -> Result<BuildStats>;
    auto build_incremental(graph, old_index, changed) -> Result<BuildStats>;
    auto build_subset(graph, command_ids) -> Result<BuildStats>;
    auto on_job_start(callback) -> void;
    auto on_job_complete(callback) -> void;
    auto on_progress(callback) -> void;
    auto cancel() -> void;
    auto is_cancelled() const -> bool;
    auto stats() const -> BuildStats;
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

Commands execute from the Tupfile's source directory, so all paths must be transformed
to Tupfile-relative coordinates (what commands see).

**Node-traversal approach**: Instead of string manipulation, path resolution traverses
the graph structure:
- `".."` → walk to parent node
- `"name"` → find/create child node

This naturally unifies input and output path resolution because both traverse to the
same node when paths are equivalent (e.g., `$(B)/include/header.h` from an input and
the variant-mapped output both resolve to the same node).

**PathTransformContext** centralizes transformation parameters for command expansion:

```cpp
struct PathTransformContext {
    std::string source_to_root;   // "../" prefix sequence to reach project root
    std::string current_dir_str;  // Current Tupfile directory
    fs::path source_root;         // Source tree root
    fs::path output_root;         // Output tree root (variant directory)
};
```

**Dual-root architecture:**

The graph has two root hierarchies for variant builds:
- `SOURCE_ROOT_ID` (0): Parent of source files, directories, and Commands
- `BUILD_ROOT_ID` (1): Parent of Generated and Ghost nodes

This separation enables Ghost→Generated node unification. When a consumer references
`../producer/header.h` before the producer is parsed, a Ghost node is created under
BUILD_ROOT_ID. Later, when the producer's output `header.h` is expanded, it finds and
upgrades the Ghost to Generated—because both use the same source-relative path under
the same root.

**Pipeline stages:**

1. **Input expansion** (`expand_inputs`):
   - Glob patterns resolved against source directory
   - Results stored as source-root-relative paths

2. **PatternFlags construction** (in `expand_rule`):
   - Inputs transformed to Tupfile-relative paths
   - Glob match (`%g`) extracted from pattern + primary input
   - Single PatternFlags built and reused for both outputs and command

3. **Output expansion** (`expand_outputs`):
   - Uses node traversal under BUILD_ROOT_ID
   - Pattern substitution (`%B.o` → `main.o`)
   - Results stored as source-relative paths (e.g., `src/main.o`) under BUILD_ROOT_ID

4. **Command generation** (`expand_command`):
   - Receives PatternFlags, augments with output fields
   - All paths transformed to Tupfile-relative via `make_source_relative()`
   - Output paths get variant prefix (e.g., `src/main.o` → `../../build/src/main.o`)
   - Pattern flags (`%f`, `%o`, `%g`) substituted

**Path transformation helpers:**

```cpp
// Transform paths to Tupfile-relative for command expansion
// Inputs: source-root-relative (e.g., "src/foo.c") → "foo.c"
// Outputs: source-root-relative (e.g., "src/foo.o") → "../../build/src/foo.o"
//
// For outputs, transform_output_path adds the variant prefix automatically:
//   1. Compute variant_prefix = relative(output_root, source_root)  // e.g., "build"
//   2. Prepend: "src/foo.o" → "build/src/foo.o"
//   3. Apply make_source_relative: "build/src/foo.o" → "../../build/src/foo.o"
auto transform_input_path(ctx, tc, "src/foo.c") -> "foo.c"
auto transform_output_path(tc, "src/foo.o") -> "../../build/src/foo.o"

// Core conversion: project-root-relative → Tupfile-relative
auto make_source_relative(path, source_to_root, current_dir) {
    if (path starts with current_dir + "/")
        return path without prefix;  // Local file
    if (path starts with "../")
        return path;  // Already relative
    return source_to_root + path;  // Cross-directory reference
}
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

Graph storage:
- foo.o stored at "src/foo.o" under BUILD_ROOT_ID (source-relative)
- get_full_path() returns "build-debug/src/foo.o" (adds build root name)
- File exists at: project/build-debug/src/foo.o
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

### Why Preserve Edges During Ghost→Generated Upgrade?

When a Ghost node is upgraded to Generated (because a rule now declares it as output), pup preserves all existing dependency edges rather than deleting them:

**Tup's approach**: Tup deletes edges when upgrading a ghost, then re-parses dependent Tupfiles to recreate the edges. This works because tup maintains a persistent database and can track which Tupfiles referenced the ghost.

**Pup's approach**: Pup parses all Tupfiles fresh each build. When a consumer references `../producer/file.c` before the producer is parsed:
1. A Ghost node is created with the dependency edge
2. Later, the producer's rule creates the output, upgrading Ghost→Generated
3. The edge is already correct—it just needed the ghost to become real

Deleting edges would break dependencies:
```
aaa_consumer/Tupfile: : ../zzz_producer/helper.c |> gcc -c %f -o %o |> helper.o
zzz_producer/Tupfile: : |> echo 'int x;' > %o |> helper.c
```

Without edge preservation:
1. Parse aaa_consumer → Ghost for `zzz_producer/helper.c`, edge from command to ghost
2. Parse zzz_producer → upgrade Ghost→Generated, **delete edge** ❌
3. Result: aaa_consumer's command has no input dependency, build order is wrong

With edge preservation:
1. Parse aaa_consumer → Ghost for `zzz_producer/helper.c`, edge from command to ghost
2. Parse zzz_producer → upgrade Ghost→Generated, **keep edge** ✓
3. Result: aaa_consumer's command correctly depends on the generated file

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
VAR = value           # Assignment (expands RHS)
VAR += value          # Append (space-separated)
VAR := value          # Literal assignment (no expansion)
VAR ?= value          # Soft set (set if unset, immediate eval, first wins)
VAR ??= value         # Weak set (set if unset, deferred eval, last wins)
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
include path          # Include another Tupfile
include_rules         # Include Tuprules.tup from ancestor directories
export VAR            # Export environment variable to subprocesses
import VAR[=default]  # Import environment variable
preload path          # Preload directory for dependency tracking
error message         # Emit error and stop parsing
run script            # Execute shell script during parse
.gitignore            # Generate .gitignore for outputs
```

### Built-in Variables

| Variable | Description |
|----------|-------------|
| `TUP_CWD` | Current Tupfile directory |
| `TUP_PLATFORM` | Operating system (overridable via env var) |
| `TUP_ARCH` | CPU architecture |
| `TUP_VARIANTDIR` | Relative path to variant |
| `TUP_VARIANT_OUTPUTDIR` | Absolute variant path |
