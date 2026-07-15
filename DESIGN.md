# Putup Design Document

A reimplementation of the [Tup build system](https://gittup.org/tup/).

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Core Types](#core-types)
4. [Parser Module](#parser-module)
5. [Graph Module](#graph-module)
6. [Rule Generation Module](#rule-generation-module)
7. [Index Module](#index-module)
8. [Execution Module](#execution-module)
9. [Build Pipeline](#build-pipeline)
10. [Multi-Directory Builds](#multi-directory-builds)
11. [Variant Builds](#variant-builds)
12. [Design Decisions](#design-decisions)

---

## Overview

Putup reimplements tup with these goals:

- **Compatibility** - Parse existing Tupfiles without modification
- **Content hashing** - SHA-256 for precise change detection
- **Custom index format** - Binary format instead of SQLite
- **No FUSE** - Compute changes from index comparison
- **No Lua** - Traditional Tupfile syntax only
- **No libstdc++** - Links with `-nostdlib++`, zero runtime dependency on libstdc++/libc++

### Runtime Primitives

With libstdc++ eliminated, putup provides its own runtime primitives:

| Primitive | Replaces | Purpose |
|-----------|----------|---------|
| `StringId` (4B) | `std::string` | Interned string handle (storage) |
| `string_view` | `std::string const&` | Non-owning string reference (reading) |
| `Buf` / `HeapBuf` | `std::string` (building) | Stack/heap string builders |
| `Vec<T>` | `std::vector<T>` | Dynamic array |
| `Function<Sig>` | `std::function<Sig>` | Move-only type-erased callable (32-byte SBO) |
| `SteadyClock` / `SystemClock` | `std::chrono` clocks | Platform-implemented clocks |
| `CPath` | manual `.c_str()` | Null-termination wrapper for string_view at C API boundary |
| `throw_stubs.cpp` | libstdc++ runtime | operator new/delete, `__cxa_guard_*`, `__throw_*` stubs, macOS `__libcpp_verbose_abort` |

All threading (`std::thread`, `std::mutex`, `std::atomic`, `std::future`) has been eliminated. The scheduler uses a single-threaded `poll()`-based event loop. Multi-variant parallelism uses `fork()`+`waitpid()` via `platform::run_parallel_tasks()`.

### Module Organization

```
pup/
├── include/pup/
│   ├── core/       # Fundamental types: Result, Hash, NodeId
│   ├── parser/     # Lexer, AST, Parser, Evaluator, Glob
│   ├── graph/      # DAG, Builder, Topological sort
│   ├── index/      # Binary format, Reader, Writer
│   ├── exec/       # Scheduler, CommandRunner
│   ├── platform/   # OS abstraction: env, file_io, path, process (POSIX + Win32 backends)
│   └── cli/        # Command-line interface, options, output, subcommands
├── src/            # Implementation files (incl. *-posix.cpp / *-win32.cpp backends)
└── third_party/    # sha256, expected-lite, Catch2
```

Dependencies flow one direction: `cli → exec → graph → parser → core`, with `platform` under
everything as the OS-abstraction layer. The `platform/` module isolates OS specifics behind a
uniform API: POSIX backends (`*-posix.cpp`) use `fork`/`waitpid`/`poll`; Win32 backends
(`*-win32.cpp`) use `CreateProcessW`. The Windows binary is cross-compiled from Linux with
clang-cl + lld-link against an xwin-splatted MSVC CRT and tested under Wine.

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
    Root,           // Project root
    Condition,      // Conditional guard (ifeq/ifdef)
    Phi             // Merges outputs from conditional branches
};
```

### Node State Flags

```cpp
enum class NodeFlags : std::uint16_t {
    None = 0,
    Modified = 1 << 0,  // Content changed since last build
    Created = 1 << 1,   // Newly created
    Deleted = 1 << 2,   // Marked for deletion
    ConfigDep = 1 << 3, // Depends on configuration
    Transient = 1 << 4, // Temporary file
};
```

### Edge Classification

```cpp
enum class LinkType : std::uint8_t {
    Normal = 1,     // Standard dependency
    Sticky = 2,     // Explicit Tupfile dependency
    Group  = 3,     // Group membership
    Implicit = 4,   // Header deps from .d files
    OrderOnly = 5,  // Order-only dependency (must build first, but not a data dep)
};
```

### Error Handling

Pup uses `Result<T>` (alias for `expected<T, Error>`) for explicit error propagation:

```cpp
struct Error {
    ErrorCode code;
    StringId message;
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
    StringId name;
};

struct Expression {
    struct Literal { StringId value; };
    struct Variable { VarRef ref; };
    Vec<std::variant<Literal, Variable>> parts;
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
    StringId group_name;
};
```

**Rule** - the core build statement:

```cpp
struct Rule {
    bool foreach_;
    Vec<PathPattern> inputs;
    Vec<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display;  // From ^ ^ markers
    Vec<PathPattern> outputs;
    Vec<PathPattern> extra_outputs;
    std::optional<StringId> output_group;               // {binname} at end
    std::optional<StringId> output_order_only_group;    // <groupname> at end
    std::optional<Expression> output_order_only_group_dir; // path/ prefix for <group>
};
```

**Bang macro** - reusable command template:

```cpp
struct BangMacro {
    StringId name;          // !cc
    bool foreach_;
    Vec<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display;
    Vec<PathPattern> outputs;
    Vec<PathPattern> extra_outputs;
    std::optional<StringId> output_group;               // {binname} at end
    std::optional<StringId> output_order_only_group;    // <groupname> at end
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

### Evaluator

**Variable database** with three namespaces:

```cpp
class VarDb {
    explicit VarDb(StringPool* pool);
    auto set(name, value) -> void;       // Interns both name and value
    auto append(name, value) -> void;    // Space-separated concatenation
    auto get(name) -> std::string_view;  // O(log n) via interned StringId lookup
};
```

**Variable lookup abstraction** - The `VarContext` struct groups variable sources for lookup, while `VarBank` tracks which source a value came from:

```cpp
enum class VarBank {
    Builtin,  // TUP_CWD, TUP_PLATFORM, etc.
    Regular,  // $(VAR)
    Config,   // @(VAR) or $(CONFIG_VAR)
    Node,     // &(VAR)
    Env,      // Imported environment variable
    NotFound
};

struct VarContext {
    VarDb* vars = nullptr;         // Regular variables
    VarDb const* config = nullptr; // Config variables
    VarDb* node = nullptr;         // Node variables
    std::string_view tup_cwd, tup_platform, tup_arch;
    std::string_view tup_variantdir, tup_variant_outputdir;
    std::string_view tup_srcdir, tup_outdir;
    SortedIdVec const* imported_vars;  // Interned env var names
    StringPool const* string_pool;
};

// Lookup with bank tracking (for dependency recording)
auto lookup_var_with_bank(VarContext const&, name, kind) -> VarLookupResult;
```

**EvalContext** owns the data and provides callbacks for cross-directory resolution and dependency tracking:

```cpp
struct EvalContext {
    // Variable storage (ownership - VarContext provides views)
    VarDb* vars;                // $(VAR)
    VarDb const* config_vars;   // @(VAR) - read-only
    VarDb* node_vars;           // &(VAR)
    StringId tup_cwd, tup_platform, tup_arch;
    StringId tup_variantdir, tup_variant_outputdir;
    StringId tup_srcdir, tup_outdir;

    // Callbacks for cross-directory resolution (pup::Function, not std::function)
    Function<Vec<StringId>(std::string_view)> resolve_group;           // {groupname}
    Function<Vec<StringId>(std::string_view)> resolve_order_only_group; // <groupname>
    Function<Result<void>(std::string_view)> request_directory;         // Demand-driven parsing
    Vec<StringId> const* available_tupfile_dirs;

    // Callbacks for fine-grained dependency tracking
    Function<void(std::string_view)> on_config_var_used;
    SortedIdVec const* imported_vars;         // Interned env var names
    StringPool const* string_pool;
    Function<void(std::string_view)> on_env_var_used;
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

auto glob_expand(pattern, base_dir, options) -> Result<Vec<StringId>>;
```

Supported patterns: `*`, `?`, `[abc]`, `**`, `!pattern` (exclusion)

---

## Graph Module

### BuildGraph

The graph uses separate structs for file and command nodes, with centralized edge storage and string interning for memory efficiency.

**String interning:**

All string fields use `StringId` handles (4 bytes) instead of `std::string` (32 bytes). Strings are stored in a central `StringPool` with deduplication:

```cpp
/// 4-byte handle to an interned string
enum class StringId : std::uint32_t { Empty = 0 };

/// Arena-based string pool with deduplication
class StringPool {
    auto intern(std::string_view str) -> StringId;  // Deduplicated insert
    auto get(StringId id) const -> std::string_view;
    auto find(std::string_view str) const -> StringId;  // Lookup without insert
};
```

**Node types:**

```cpp
/// File node - files, directories, groups, variables, ghosts
struct FileNode {
    NodeId id;
    NodeType type;          // File, Directory, Generated, Ghost, Group, Variable, etc.
    NodeFlags flags;        // Modified, Created, Deleted, etc.
    StringId name;          // Basename only (interned, tup-style identification)
    NodeId parent_dir;      // Parent directory node (used with name for lookup)
    Hash256 content_hash;   // Content hash for files (directories have zero hash)
};

/// Guard entry - condition + required polarity
struct Guard {
    NodeId condition = INVALID_NODE_ID;  // Condition node ID
    bool polarity = true;                // True if condition must be true
};

/// Command node - build commands
/// Type is determined by node_id::is_command(), not a stored field.
struct CommandNode {
    NodeId id = 0;
    StringId display = StringId::Empty;        // Display text (from ^ ^ markers)
    StringId source_dir = StringId::Empty;     // Tupfile directory (relative to root)
    StringId instruction_id = StringId::Empty; // Instruction pattern (e.g., "gcc -c %f -o %o")
    Vec<NodeId> inputs = {};           // Operand file NodeIds for %f expansion
    Vec<NodeId> outputs = {};          // Operand file NodeIds for %o expansion
    SortedIdVec exported_vars = {};            // Env vars to export (interned StringIds)
    std::optional<GeneratedOutput> generated_output = {};  // Output specification
    OutputAction output_action = {};           // What to do with output
    NodeId parent_command = INVALID_NODE_ID;   // Parent command for InjectImplicitDeps

    // Guards for conditional execution (phi-node model)
    // ALL guards must be satisfied for command to execute
    // For nested conditionals: ifeq(A,x) { ifeq(B,y) { cmd } }
    //   → guards = [Guard{condA, true}, Guard{condB, true}]
    // Guards exist only for conditions that can flip without a Tupfile edit
    // (config or env reads). A static ifeq/ifneq is processed textually like
    // tup: the losing branch is skipped and no condition node is created.
    Vec<Guard> guards = {};
};

/// Condition node - represents an ifeq/ifdef/ifneq/ifndef condition
/// Type is determined by node_id::is_condition(), not a stored field.
struct ConditionNode {
    NodeId id = 0;
    StringId expression = StringId::Empty;     // String representation of condition
    bool current_value = false;                // Evaluated at graph time
    NodeId config_var_dep = INVALID_NODE_ID;   // Config var this depends on (if any)
};

/// Phi node - merges outputs from conditional branches
/// Created when same output name appears in both branches of a conditional.
/// Type is determined by node_id::is_phi(), not a stored field.
struct PhiNode {
    NodeId id = 0;
    StringId name = StringId::Empty;       // Canonical output name
    NodeId parent_dir = 0;                 // Parent directory
    NodeId condition = INVALID_NODE_ID;    // The condition determining which output
    NodeId then_output = INVALID_NODE_ID;  // Output when condition is true
    NodeId else_output = INVALID_NODE_ID;  // Output when condition is false
};

struct Edge {
    NodeId from, to;
    LinkType type;
};
```

**ID spaces:** The top 2 bits of a NodeId are a kind tag, so each kind gets its
own dense 30-bit index space (~1.07B ids per kind) and every uint32 decodes to
exactly one kind — mixed-flag junk states are unrepresentable:
- File IDs: 1, 2, 3, ... (tag 0)
- Command IDs: 0x40000001, 0x40000002, ... (tag 1)
- Condition IDs: 0x80000001, ... (tag 2)
- Phi IDs: 0xC0000001, ... (tag 3)

The `node_id` namespace provides type detection and ID construction:

```cpp
namespace node_id {
    enum class Tag : NodeId { File = 0, Command = 1, Condition = 2, Phi = 3 };

    constexpr auto tag(NodeId id) -> Tag;            // Top 2 bits
    constexpr auto is_file(NodeId id) -> bool;       // tag == File && id != 0
    constexpr auto is_command(NodeId id) -> bool;    // tag == Command
    constexpr auto is_condition(NodeId id) -> bool;  // tag == Condition
    constexpr auto is_phi(NodeId id) -> bool;        // tag == Phi
    constexpr auto index(NodeId id) -> std::size_t;  // Strips the tag (low 30 bits)

    constexpr auto make_command(std::size_t idx) -> NodeId;
    constexpr auto make_condition(std::size_t idx) -> NodeId;
    constexpr auto make_phi(std::size_t idx) -> NodeId;
}
```

**Edge storage:** Edges are stored centrally with indices for O(1) lookup:

```cpp
struct Graph {
    StringPool strings;               // Interned string storage

    Vec<FileNode> files;              // Files, directories, groups
    Vec<CommandNode> commands;        // Command nodes only
    Vec<Edge> edges;                  // Central edge storage

    // Phi-node model structures
    Vec<ConditionNode> conditions;    // Conditional guards
    Vec<PhiNode> phi_nodes;           // Output merges from conditional branches

    // Edge indices: NodeId -> ArenaSlice (indices into edges vector)
    Arena32 edge_arena;
    NodeIdArenaIndex edges_to_index;
    NodeIdArenaIndex edges_from_index;

    // Order-only edges use LinkType::OrderOnly in the unified edges vector

    // Node lookup indices
    Vec<SortedPairVec> dir_children;  // Per-directory name -> NodeId (indexed by parent dir)
    SortedPairVec path_to_node;      // PathId -> NodeId (reverse of FileNode::path_id)
    StringPool command_strings;               // Interned expanded command strings
    SortedPairVec command_index;              // StringId(command) -> NodeId

    // ID generators (next available ID for each type)
    NodeId next_file_id = 2;                               // Starts at 2 (BUILD_ROOT is 1)
    NodeId next_command_id = node_id::make_command(1);     // First command ID
    NodeId next_condition_id = node_id::make_condition(1); // First condition ID
    NodeId next_phi_id = node_id::make_phi(1);             // First phi ID
};

// Path cache is stored externally in BuildGraph for const-correctness
struct PathCache {
    NodeIdMap32 ids;    // NodeId -> StringId (path interned in pool)
    StringPool pool;    // Owns the full path strings
};

class BuildGraph {
    Graph graph_;
    mutable PathCache path_cache_;  // Cache for get_full_path()
};
```

**Path storage model**: Nodes store only their basename in `name`. Full paths are reconstructed by walking the `parent_dir` chain via `get_full_path()`, which caches results for efficiency.

**Node creation**: All file/directory/ghost/generated nodes are created through `ensure_file_node(graph, path_id, type)`, which takes a grounded `PathId` and walks the PathPool trie to create intermediate directory nodes as needed. It deduplicates via `path_to_node` (PathId→NodeId map) and handles type upgrades (Ghost→Generated). The builder's `resolve_input_node` performs filesystem probing and heuristics to determine the correct root (SourceRoot vs BuildRoot) and NodeType, then delegates to `ensure_file_node` for actual node creation.

**Graph operations:**

```cpp
// Free functions (functional style)
auto add_file_node(Graph&, FileNode) -> Result<NodeId>;
auto ensure_file_node(Graph&, PathId, NodeType) -> Result<NodeId>; // Primary creation path
auto add_command_node(Graph&, CommandNode) -> Result<NodeId>;
auto add_edge(Graph&, from, to, type) -> Result<void>;
auto get_full_path(Graph const&, id) -> StringId;
auto find_by_dir_name(Graph const&, parent_id, name) -> std::optional<NodeId>;
auto find_by_path(Graph const&, path) -> std::optional<NodeId>;
auto get_inputs(Graph const&, id) -> Vec<NodeId>;
auto get_outputs(Graph const&, id) -> Vec<NodeId>;
auto get_sticky_outputs(Graph const&, id) -> Vec<NodeId>;  // Tupfile/config deps
auto nodes_of_type(Graph const&, type) -> Vec<NodeId>;

// Type-specific accessors
auto get_file_node(Graph&, id) -> FileNode*;           // nullptr for command IDs
auto get_command_node(Graph&, id) -> CommandNode*;     // nullptr for file IDs
auto get_condition_node(Graph&, id) -> ConditionNode*; // nullptr for non-condition IDs
auto get_phi_node(Graph&, id) -> PhiNode*;             // nullptr for non-phi IDs

// Phi-node model helpers
auto add_condition_node(Graph&, ConditionNode) -> Result<NodeId>;
auto add_phi_node(Graph&, PhiNode) -> Result<NodeId>;
auto resolve_phi_node(Graph const&, phi_id) -> NodeId; // Returns active output
auto is_guard_satisfied(Graph const&, NodeId) -> bool;

// String access helpers (resolve StringId -> string_view)
auto get_name(Graph const&, id) -> std::string_view;
auto get_display_str(Graph const&, id) -> std::string_view;
auto get_source_dir(Graph const&, id) -> std::string_view;
auto get_instruction_pattern(Graph const&, id) -> std::string_view;
auto expand_instruction(Graph const&, id, PathCache&) -> StringId;
auto expand_instruction(Graph const&, id) -> StringId;

// Command index for find_by_command() lookups
auto build_command_index(Graph&, PathCache&) -> void;

// Build root management (for variant builds)
auto set_build_root_name(Graph&, name) -> void;
auto get_build_root_name(Graph const&) -> std::string_view;
auto is_under_build_root(Graph const&, id) -> bool;
```

### Topological Sort

```cpp
struct TopoSortResult {
    Vec<NodeId> order;
    bool has_cycle;
    Vec<NodeId> cycle;  // Path if cycle found
};

auto topological_sort(graph) -> TopoSortResult;
auto reverse_topological_sort(graph) -> TopoSortResult;
auto detect_cycles(graph) -> std::optional<Vec<NodeId>>;
```

Additional graph analysis:

```cpp
auto reachable_from(graph, start) -> NodeIdMap32;
auto node_depth(graph, id) -> std::size_t;
auto critical_path(graph) -> Vec<NodeId>;
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
if (node->type == NodeType::Ghost && !graph.get_outputs(id).empty())
    return error("Missing input file (unresolved ghost): " + path);
```

**Index serialization**: Ghost nodes are transient—they're either upgraded to Generated during parsing or caught as errors. They're never written to the index.

### GraphBuilder

Transforms AST to BuildGraph:

```cpp
struct BuilderOptions {
    StringId source_root;
    StringId config_root;
    StringId output_root;
    StringId config_path;          // Path to tup.config (for sticky edge tracking)
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
    Vec<std::pair<std::uint32_t, BangMacroDef>> macros;  // Sorted by interned name key
    GroupMemberTable groups;  // Interned group name → member NodeIds
};
```

---

## Rule Generation Module

Pup can auto-generate auxiliary rules from user commands, primarily for implicit
dependency extraction. When a compilation command is detected, pup generates a
parallel "DEP" command that extracts header dependencies.

### Core Types

```cpp
/// Command metadata for pattern matching
struct CommandInfo {
    NodeId node_id;
    StringId command;
    StringId display;
    Vec<StringId> inputs;
    Vec<StringId> order_only_inputs;
    Vec<PathId> outputs;            // BuildRoot-grounded PathIds
    StringId working_dir;
};

/// Output specification for generated rules
struct GeneratedOutput {
    enum class Type { File, Stdout, Stderr };
    Type type = Type::File;
    StringId path;
};

/// How to process generated rule output
enum class OutputAction {
    Normal,            // Regular file output
    InjectImplicitDeps // Parse as depfile, add edges to parent command
};

/// Complete specification of an auto-generated rule
struct GeneratedRule {
    Vec<StringId> inputs;
    Vec<StringId> order_only_inputs;
    StringId command;
    StringId display;
    Vec<GeneratedOutput> outputs;
    OutputAction action = OutputAction::Normal;
    NodeId parent_command = INVALID_NODE_ID;
};
```

### Dep Scanner Interface

Scanners detect compiler commands and generate dependency extraction commands:

```cpp
class DepScanner {
    virtual auto matches(CommandInfo const&) const -> bool = 0;
    virtual auto has_dep_flags(std::string_view) const -> bool = 0;
    virtual auto build_dep_command(CommandInfo const&) -> std::optional<StringId> = 0;
};

class DepScannerRegistry {
    auto register_scanner(std::unique_ptr<DepScanner>) -> void;
    auto match_and_generate(CommandInfo const&) -> Vec<GeneratedRule>;
};
```

The `GccScanner` implementation handles GCC, Clang, and compatible compilers.

### Generation Flow

```
User rule: : main.c |> gcc -c %f -o %o |> main.o
                │
                ▼ Scanner matches "gcc ... -c ..."
        ┌───────────────────┐
        │ Generate DEP rule │
        │ gcc -M main.c     │
        └───────────────────┘
                │
                ▼ Add to graph
        ┌───────────────────┐
        │   DEP command     │──────┐
        └───────────────────┘      │
                │                  │ edge (DEP runs before compile)
                ▼                  ▼
        ┌───────────────────┐
        │ Compile command   │
        └───────────────────┘
```

### Execution Integration

When the scheduler executes a command with `OutputAction::InjectImplicitDeps`:

1. Capture stdout (DEP command outputs dependency list)
2. Parse as depfile format: `target: dep1 dep2 dep3`
3. Add discovered files as implicit edges to parent command
4. Store in index for incremental rebuilds

Commands with existing `-MD` flags are skipped (user handles deps manually).

---

## Index Module

### Binary Format (v11)

The index uses **instruction-based command storage** for significant space savings (introduced in v8). Instead of storing fully-expanded command strings, commands store an instruction pattern (e.g., `gcc -c %f -o %o`) plus operand NodeIds; full commands are reconstructed lazily when needed. Each file entry also carries an mtime stat-cache (v9) and each command carries a structural identity hash (v11).

```
┌─────────────────────────────────────┐
│ Header (56 bytes)                   │
│   magic: "PUPI" (4 bytes)           │
│   version: u32 (11)                 │
│   file_count: u32                   │
│   command_count: u32                │
│   edge_count: u32                   │
│   string_table_size: u32            │
│   file_offset: u32                  │
│   command_offset: u32               │
│   edge_offset: u32                  │
│   operand_table_offset: u32         │
│   operand_data_offset: u32          │
│   string_offset: u32                │
│   save_time_ns: u64                 │  ← v9 (racy-clean detection)
├─────────────────────────────────────┤
│ FileEntry[] (64 bytes each)         │
│   parent_id: u32                    │
│   src_id: u32                       │
│   name_offset: u32                  │
│   type: u8, flags_low/high: u16     │
│   reserved: 1 byte                  │
│   size: u64                         │
│   content_hash: [u8; 32]            │
│   mtime_ns: u64                     │  ← v9 (stat cache)
│   (id computed from array index)    │
├─────────────────────────────────────┤
│ CommandEntry[] (48 bytes each)      │
│   dir_id: u32                       │
│   cmd_offset: u32                   │  ← Template string with %f/%o patterns (v8)
│   display_offset: u32               │
│   env_offset: u32                   │
│   identity: [u8; 32]                │  ← Structural identity hash (v11)
│   (id = index | 0x40000000)         │
├─────────────────────────────────────┤
│ Edge[] (16 bytes each)              │
│   from_id: u32                      │
│   to_id: u32                        │
│   type: u8, reserved: 7 bytes       │
├─────────────────────────────────────┤
│ Operand Offset Table (4 bytes × N)  │  ← NEW
│   offset[i] = position in data      │
├─────────────────────────────────────┤
│ Operand Data (variable length)      │  ← NEW
│   Per command: u8 in_count,         │
│                u8 out_count,        │
│                NodeId inputs[],     │
│                NodeId outputs[]     │
├─────────────────────────────────────┤
│ String Table (length-prefixed)      │
│   [0]: u16(0) - empty string entry  │
│   [...]: u16(len) + data bytes      │
│   Deduplicated via offset reuse     │
│   Instructions stored here (deduped)│
├─────────────────────────────────────┤
│ Footer (32 bytes)                   │
│   checksum: [u8; 32] (SHA-256)      │
└─────────────────────────────────────┘
```

**Instruction deduplication**: Bang macros like `!cc = |> $(CC) -c %f -o %o |>` produce the same instruction for all source files. With 1000 C files, instead of storing 1000 nearly-identical command strings, v8 stores 1 instruction + 1000 operand records.

**Lazy reconstruction**: Full command strings are computed on demand via `expand_instruction()`, which substitutes operand paths into the instruction pattern. CommandNode stores `instruction_id` (the pattern) plus explicit `inputs`/`outputs` operand vectors. This keeps index loading fast and avoids storing redundant expanded strings.

Version history:
- v1: Initial format with full path strings
- v2: Added name field for (parent_dir, name) identification
- v3: Removed path field, paths reconstructed from parent chain
- v4: (removed) Directory Merkle hashes - not useful for change detection
- v5: Removed mtime, change detection uses size + content hash
- v6: Compact format: 32-bit IDs/offsets, length-prefixed strings
- v7: Tagged ID spaces (files vs commands), ID computed from array index
- v8: Instruction-based command storage with operand sections
- v9: Stat cache — mtime_ns per file, save_time_ns header for racy-clean detection
- v10: Removed unused group_cmd_id field from edges
- v11: Per-command structural identity hash (command text + values of vars it depends
  on); change detection keys on identity, not the rendered command string

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
    StringId stdout_output = StringId::Empty;
    StringId stderr_output = StringId::Empty;
    std::chrono::milliseconds duration = {};
    bool timed_out = false;
    bool signaled = false;
    int signal = 0;
};

struct RunOptions {
    StringId working_dir = StringId::Empty;
    Vec<StringId> env = {};                     // Additional environment variables
    bool inherit_env = true;                    // Inherit parent environment
    std::optional<std::chrono::seconds> timeout = {};
    bool capture_stdout = true;
    bool capture_stderr = true;
    std::optional<StringId> stdin_data = {};    // Data to pipe to stdin
};

using OutputCallback = Function<void(std::string_view, bool is_stderr)>;

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
    StringId command = StringId::Empty;
    StringId display = StringId::Empty;
    StringId working_dir = StringId::Empty;
    Vec<StringId> inputs = {};
    Vec<StringId> outputs = {};
    Vec<StringId> order_only_inputs = {};  // Order-only dependencies
    Vec<StringId> exported_vars = {};      // Env vars to export to command

    // For auto-generated rules (from pattern matching)
    bool capture_stdout = false;             // Capture stdout for depfile parsing
    bool inject_implicit_deps = false;       // Parse stdout as depfile
    NodeId parent_command = INVALID_NODE_ID; // Parent command for implicit deps

    // Phi-node model: evaluated at schedule time from CommandNode.guards
    bool guard_active = true;  // True if all guards satisfied, job skipped if false
};

struct JobResult {
    NodeId id = 0;
    bool success = false;
    int exit_code = 0;
    StringId output = StringId::Empty;
    std::chrono::milliseconds duration = {};
    Vec<StringId> discovered_deps = {};        // Implicit deps from .d files
    NodeId deps_for_command = INVALID_NODE_ID; // If set, deps belong to this command (not id)
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
    StringId source_root = StringId::Empty;
    StringId config_root = StringId::Empty;
    StringId output_root = StringId::Empty;           // Output tree root (where outputs/.pup go)
    std::optional<std::chrono::seconds> timeout = {}; // Per-command timeout
};

using JobStartCallback = Function<void(BuildJob const&)>;
using JobCompleteCallback = Function<void(BuildJob const&, JobResult const&)>;
using ProgressCallback = Function<void(std::size_t completed, std::size_t total)>;

class Scheduler {
    auto build(state, filter = nullptr) -> Result<BuildStats>;
    auto on_job_start(callback) -> void;
    auto on_job_complete(callback) -> void;
    auto on_progress(callback) -> void;
    auto cancel() -> void;
    auto is_cancelled() const -> bool;
    auto stats() const -> BuildStats;
};
```

The scheduler has a single `build()` method. An optional `NodeIdMap32` filter selects which commands to execute; `nullptr` means build everything.

**Composable filters — closed algebra over intersection:**

Build constraints are computed as independent `NodeIdMap32` sets, then combined with a single operation: `intersect_with()`.

| Filter | Source | Graph algorithm |
|--------|--------|-----------------|
| **affected** | Changed files (incremental) | `collect_affected_commands()` — forward traversal from inputs |
| **required** | Output targets on CLI | `collect_required_commands()` — backward traversal from outputs |
| **scope** | `-a` scoped build | Commands in scoped dirs + upstream deps |
| **non-config** | Config commands exist | Complement set: all commands except config-generating rules |

The algebra is closed under one operation (∩) with four independent operands:
- **Identity** — no filter (`nullptr`) means all commands; adding no constraint changes nothing.
- **Composition** — each `intersect_with()` narrows the set; applying all four yields the minimal executable set.
- **Commutativity** — order of intersection doesn't matter; the result is the same regardless of which constraint is applied first.

Config exclusion uses the same mechanism as every other constraint: intersect with the complement set (non-config commands) rather than subtracting config commands. There is no `subtract()` operation.

For example, `putup -B build build/putup` intersects the **affected** filter (only changed files) with the **required** filter (only the binary's deps), building exactly the minimal set.

**Parallel execution algorithm (single-threaded, poll-based):**

```
1. Compute in_degree[job] = count of dependencies
2. Queue jobs where in_degree == 0
3. Single-threaded event loop:
   a. Launch ready jobs as child processes (up to -j limit)
   b. poll() on stdout/stderr pipes of all active children
   c. On child exit (SIGCHLD / waitpid):
      - Collect output, record result
      - For each dependent job:
        - Decrement in_degree
        - If in_degree == 0: enqueue
4. Repeat until queue empty or cancelled
```

This ensures:
- Maximum parallelism within dependency constraints via non-blocking I/O
- Order-only dependencies respected for ordering only
- No threads, no mutexes, no atomics — deterministic single-threaded control flow

The scheduler drives all process work through the `platform/` process API
(`spawn_async` / `poll_fds` / `read_nonblocking`) rather than raw syscalls, so it is portable
across backends: the POSIX backend implements it with `fork`/`waitpid` and `poll`, while the
Win32 backend uses `CreateProcessW`.

---

## Build Pipeline

### Complete Build Flow

```cpp
// 1. Find project root
auto root = find_project_root();

// 2. Parse Tupfile
auto parser = Parser { read_file(pup::path::join(root, "Tupfile")), resolver };
auto tupfile = parser.parse();

// 3. Load configuration
auto config = parse_config(pup::path::join(variant_dir, "tup.config"));

// 4. Create evaluation context
auto vars = VarDb {};
auto ctx = EvalContext {
    .vars = &vars,
    .config_vars = &config,
    .tup_cwd = root,
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
IndexWriter::write(pup::path::join(pup::path::join(root, ".pup"), "index"), make_index(*graph));
```

### Incremental Build

The incremental build pipeline performs seven steps to determine what needs rebuilding:

```cpp
// 1. Build command index for find_by_command() lookups
//    Must happen after parsing when operands are set
graph.build_command_index();

// 2. Compute build scopes (if scoped build via -d or CWD)
auto scopes = compute_build_scopes(opts, layout);
auto upstream_files = collect_upstream_files(graph, scopes);

// 3. Find changed files with scope filtering
//    Compares size then hash against old index
auto changed = find_changed_files_with_implicit(
    source_root, old_index, scopes, upstream_files);

// 4. Expand implicit dependencies
//    Header changes → affected command outputs added
changed = expand_implicit_deps(changed, old_index, graph);

// 5. Force output targets to rebuild (if --output-targets specified)
for (auto const& target : output_targets) {
    changed.push_back(build_root_prefix + target);
}

// 6. Detect new commands (in graph but not index)
auto new_outputs = detect_new_commands(graph, old_index);
changed.insert(changed.end(), new_outputs.begin(), new_outputs.end());

// 7. Remove stale outputs from deleted commands
remove_stale_outputs(graph, old_index, source_root);

// Build with composable filter (affected ∩ required ∩ scope ∩ non_config)
auto filter = BuildFilter {};
filter.intersect_with(collect_affected_commands(graph, changed));
// ... intersect with required/scope/non_config if active ...
auto stats = scheduler.build(state, filter.ptr());
```

**Change detection algorithm:**
1. Compare file sizes - if different, rebuild
2. If size matches, compute SHA-256 hash - if different, rebuild

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
auto discover_tupfile_dirs(root) -> Vec<StringId>
{
    // Skip variant directories (contain tup.config)
    // Return sorted vector of relative paths to Tupfile directories
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
    Vec<StringId> available;  // Dirs with Tupfiles (sorted)
    Vec<StringId> parsed;     // Already processed (sorted)
    Vec<StringId> parsing;    // Currently processing (cycle detection, sorted)
};
```

The `parsing` set detects circular dependencies - if a directory appears while still being parsed, it's a cycle.

### Per-Directory Variable Scope

Each Tupfile gets its own variable scope:

```cpp
struct BuilderContext {
    BuildGraph* graph;
    parser::EvalContext* eval;
    parser::VarDb* vars;                // $(VAR) - local to this Tupfile (StringPool-backed)
    Vec<std::pair<std::uint32_t, BangMacroDef>> macros;  // !macros (sorted by interned name)
    GroupMemberTable groups;            // {bins} (interned group name → member NodeIds)
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

Group keys use interned `"directory/name"` StringIds:

```cpp
// BuilderState::group_nodes — interned "dir/name" StringId → NodeId
SortedPairVec group_nodes;
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
Path transformation is handled inline within the builder using `StringId`-based path operations (`pup::path::join`, `pup::path::relative`, etc.).
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
if (pup::path::filename(full_path) == "tup.config" && !output_root.empty()) {
    auto variant_config = pup::path::join(output_root, "tup.config");
    if (pup::platform::exists(variant_config))
        return variant_config;
}
```

### Working Directory

Commands execute from their **source directory**, not the variant directory:

```cpp
// In scheduler.cpp
auto working_dir = options_.root_dir;
if (!node->source_dir.empty())
    working_dir = pup::path::join(working_dir, node->source_dir);  // e.g., "src"
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

## Three-Tree Architecture

The three-tree architecture extends variant builds to support out-of-tree configuration files, enabling builds of third-party code without modification.

### Motivation

Traditional build systems require configuration files (Tupfiles, Makefiles, CMakeLists.txt) to live alongside source code. This creates friction when:

- **Building third-party code**: Can't modify upstream repositories
- **Multiple independent configs**: Want different build rules for same source
- **Pristine source trees**: Git submodules, vendored code, read-only filesystems

### The Three Trees

| Tree | CLI Flag | Environment | Purpose |
|------|----------|-------------|---------|
| **Source root** | `-S` | `PUP_SOURCE_DIR` | Where source files live (`.c`, `.h`) |
| **Config root** | `-C` | `PUP_CONFIG_DIR` | Where Tupfiles live |
| **Output root** | `-B` | `PUP_BUILD_DIR` | Where outputs, `.pup/`, `tup.config` go |

**Directory structure example:**

```
busybox/                 # Source tree (read-only, upstream)
├── coreutils/
│   └── cat.c
└── shell/
    └── ash.c

config/                  # Config tree (Tupfiles)
├── Tupfile.ini
├── Tuprules.tup
├── coreutils/
│   └── Tupfile
└── shell/
    └── Tupfile

build/                   # Output tree
├── .pup/
├── tup.config
├── coreutils/
│   └── cat.o
└── shell/
    └── ash.o
```

**Command:**
```bash
pup -S busybox -C config -B build
```

### Path Resolution Rules

| Operation | Resolved Against |
|-----------|-----------------|
| Glob patterns (`*.c`) | `source_root/current_dir` |
| Tupfile discovery | `config_root` |
| Command execution CWD | `source_root/current_dir` |
| Output files | `output_root/current_dir` |
| `tup.config` | `output_root` |
| `.pup/index` | `output_root` |

### TUP_SRCDIR and TUP_OUTDIR Variables

When config and source trees differ, Tupfiles need to reference source and output locations. Two built-in variables provide these paths:

| Variable | Value | Purpose |
|----------|-------|---------|
| `TUP_SRCDIR` | Relative path from Tupfile to source dir | Reference source files explicitly |
| `TUP_OUTDIR` | Relative path from Tupfile to output dir | Reference output files explicitly |

**Example:** For `config/coreutils/Tupfile` with source at `busybox/` and output at `build/`:

```tup
# TUP_SRCDIR = ../../busybox/coreutils
# TUP_OUTDIR = ../../build/coreutils

CFLAGS = -I$(TUP_SRCDIR)/../include

# Globs automatically resolve against source_root
: foreach *.c |> $(CC) $(CFLAGS) -c %f -o %o |> %B.o
```

**Computation:**

```cpp
// In EvalContext setup:
tup_srcdir = pup::path::relative(pup::path::join(source_root, current_dir),
                                 pup::path::join(config_root, current_dir));
tup_outdir = pup::path::relative(pup::path::join(output_root, current_dir),
                                 pup::path::join(config_root, current_dir));
```

### Relationship to Graph Roots

The three-tree model maps onto the existing dual-root graph architecture:

- `SOURCE_ROOT_ID` (0): Parent of Source files, directories, and Commands
- `BUILD_ROOT_ID` (1): Parent of Generated and Ghost nodes

In three-tree builds:
- **Tupfile discovery** scans `config_root`, but paths stored under SOURCE_ROOT_ID
- **Glob expansion** reads from `source_root`, results stored under SOURCE_ROOT_ID
- **Output expansion** creates nodes under BUILD_ROOT_ID with `output_root` prefix
- **Command execution** runs from `source_root/current_dir`

### Config Root Discovery

When `-C` is not specified, `discover_layout()` determines config_root:

1. **CLI argument** (`-C`): Use as config_root
2. **Environment** (`PUP_CONFIG_DIR`): Use if set
3. **Source has Tupfile.ini**: Traditional mode, `config_root = source_root`
4. **Output has Tupfile.ini**: Two-tree mode, `config_root = output_root`
5. **Fallback**: Simple projects with only `Tupfile`, `config_root = source_root`

### Shared Configs with Multiple Variants

The three-tree model enables a powerful pattern: shared Tupfiles with variant-specific `tup.config`:

```
source/                  # Source code
config/                  # Shared Tupfiles
├── Tupfile.ini
└── Tupfile

build-debug/             # Debug variant
├── tup.config           # CONFIG_CFLAGS=-g -O0
└── hello

build-release/           # Release variant
├── tup.config           # CONFIG_CFLAGS=-O2 -DNDEBUG
└── hello
```

**Commands:**
```bash
pup -S source -C config -B build-debug
pup -S source -C config -B build-release
```

The same Tupfiles produce different outputs based on each variant's `tup.config`.

### Glob Matching in Three-Tree Builds

When matching glob patterns against Generated nodes (e.g., `*.o` finding objects from earlier rules), special handling is needed because Generated nodes are stored with the build root prefix:

```cpp
// Node stored as: "../build-debug/hello.o" (under BUILD_ROOT_ID)
// Glob pattern:   "*.o" (relative to config dir)

// Solution: Strip build root prefix before matching
auto match_path = node_path;
if (node_path.starts_with(build_root_name + "/")) {
    match_path = node_path.substr(build_root_name.size() + 1);
}
// Now match_path = "hello.o", which matches "*.o"
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

### Why Split FileNode and CommandNode?

Files and commands have different fields and lifecycles. Splitting them:

- **Memory efficiency** - No wasted fields (commands don't need content_hash, files don't need exported_vars)
- **Type safety** - `get_file_node()` vs `get_command_node()` prevents type confusion
- **O(1) type detection** - High bit in ID determines type without field access
- **Uniform edges** - Both types participate in the same edge graph
- **Single traversal** - Topological sort works identically for both

### Why String Interning?

String fields dominate memory in large builds. String interning provides:

- **4-byte handles** - `StringId` replaces all owned string fields, reducing per-field overhead by 28 bytes vs a typical string type
- **Deduplication** - Repeated strings (e.g., common source_dir values) stored once
- **Zero-allocation lookup** - `find_by_dir_name()` uses transparent comparators to lookup by `string_view` without allocation
- **~40% memory reduction** - Benchmarks show ~205 bytes saved per node

The `StringPool` uses a custom arena with Robin Hood hashing for O(1) deduplication. Helper functions like `get_name()` and `get_source_dir()` transparently resolve `StringId` to `string_view`. Path functions (`pup::path::join`, `pup::path::parent`, etc.) return `StringId` by interning results in a global pool.

### Why Binary Index Instead of SQLite?

- Simpler dependencies (no SQLite)
- Memory-mapped for speed
- Atomic updates via rename
- Portable binary format
- Direct structure access

### Why No FUSE?

Original tup uses FUSE to intercept file access. Putup instead:

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
    └── putup        # Output binary
```

Benefits:
- Clean source tree
- Multiple configurations (debug/release)
- Easy cleanup (rm -rf build/)
- Parallel variant builds

### Why Single-Threaded poll()-Based Scheduler?

The scheduler uses a single-threaded event loop with `poll()` for I/O multiplexing and `fork()`/`waitpid()` for child process management:

- **No threading** - Eliminates mutexes, atomics, and thread-safety concerns entirely
- **True parallelism** - Child processes run in parallel; the event loop only manages I/O
- **Deterministic** - Single control flow makes debugging and reasoning straightforward
- Respects all dependencies via in-degree counting
- Order-only dependencies for sequencing without rebuild triggers
- Cancellation support

### Why Track All Headers Including System?

Unlike some build systems that filter out system headers:

- **Correctness** - System header changes (e.g., glibc update) should trigger recompilation
- **Simplicity** - No heuristics for deciding "what matters"
- **Reproducibility** - Same source + same headers = same build
- **Minimal overhead** - Header paths stored once per command, hash computed only when mtime differs

### Why Structural Command Identity?

A command must re-run when its *effective definition* changes. Earlier, a command's
identity was its fully-expanded command string, and change detection asked "is this
exact string in the previous index?" That conflates two distinct questions:

- **Stable key** — "is there still a command that produces this output?" (used to detect
  stale outputs from removed rules)
- **Change signal** — "has this command's definition changed, so it must re-run?"

The rendered string answers the first but is a *lossy* proxy for the second: any input
that affects the output without appearing in the text is invisible. The canonical case
is an exported environment variable the subprocess reads as `$VAR` — the command line is
byte-identical across values, so a string-keyed detector never re-runs it (stale output,
"Nothing to do"). The same hole applies to a config var that gates an `export`.

v11 separates the two. Each command carries a **structural identity** — a SHA-256 over the
expanded command text *plus* the values (content hashes) of every variable it depends on,
collected from its `Sticky` edges. Exported vars now contribute a sticky edge, so their
values fold into the identity even when they never appear in the text. Change detection
keys on identity (env/config change → identity changes → rebuild); stale-output detection
keeps using the rendered string (the structural-shape key, correctly env-insensitive so an
env change rebuilds in place rather than deleting and recreating). Two builds yield the
same identity iff the command's effective definition is unchanged, which also makes it a
sound cross-build join key — unlike a positional NodeId or a lossy string.

### Why Preserve Edges During Ghost→Generated Upgrade?

When a Ghost node is upgraded to Generated (because a rule now declares it as output), putup preserves all existing dependency edges rather than deleting them:

**Tup's approach**: Tup deletes edges when upgrading a ghost, then re-parses dependent Tupfiles to recreate the edges. This works because tup maintains a persistent database and can track which Tupfiles referenced the ghost.

**Putup's approach**: Putup parses all Tupfiles fresh each build. When a consumer references `../producer/file.c` before the producer is parsed:
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

### Why Phi-Node Model for Conditionals?

When building with different conditional values (e.g., `-D TESTS=n` vs `-D TESTS=y`), a naive approach skips statements in inactive branches entirely. This causes problems:

1. Commands disappear from graph when condition is false
2. Index prunes disappeared commands
3. Toggling condition back rebuilds everything from scratch

**Solution**: Inspired by SSA (Static Single Assignment) form in compilers, use a phi-node model where both branches always exist in the graph with guards determining execution.

**Example:**
```tup
ifeq (@(DEBUG),y)
  : foo.c |> gcc -g -o %o %f |> app
else
  : foo.c |> gcc -O2 -o %o %f |> app
endif
```

**Graph structure:**
```
foo.c ─────┬─→ [cmd1: gcc -g]  ─→ app@DEBUG=y ──┐
           │       ↑ guard                      │
           │   DEBUG=y                          ├─→ φ(app) ─→ downstream
           │                                    │
           └─→ [cmd2: gcc -O2] ─→ app@DEBUG=n ──┘
                   ↑ guard
               DEBUG=n
```

**Key properties:**

1. **Both branches always in graph** - Stable structure across condition toggles
2. **Commands are guarded** - Have `Guard{condition, polarity}` entries
3. **Phi-nodes merge outputs** - Represent the "canonical" file for downstream consumers
4. **Execution filters by guard** - Scheduler checks `guard_active` before running
5. **Index stays constant** - No churn when toggling configurations

**Guard evaluation in scheduler:**

```cpp
// When building job list, evaluate guards via helper
auto guard_active = is_guard_satisfied(graph, cmd_id);

// is_guard_satisfied() checks ALL guards:
for (auto const& guard : cmd.guards) {
    auto const* cond = get_condition_node(graph, guard.condition);
    if (cond->current_value != guard.polarity) {
        return false;  // Guard not satisfied
    }
}
return true;  // All guards satisfied

// Jobs with inactive guards are skipped, not executed
if (!job.guard_active) {
    mark_job_skipped(i);
    continue;
}
```

**Nested conditionals**: Guards are a list of `Guard{condition, polarity}` entries. For nested ifeq/ifdef blocks, ALL must be satisfied:

```tup
ifeq (@(A),x)           # cond1
  ifeq (@(B),y)         # cond2
    : s.c |> ... |> out # guards = [Guard{cond1,true}, Guard{cond2,true}]
  else
    : s.c |> ... |> out # guards = [Guard{cond1,true}, Guard{cond2,false}]
  endif
endif
```

### Why (parent_dir, name) Path Storage?

Like tup, pup stores paths as `(parent_dir, basename)` pairs rather than full path strings:

- **No path format mismatches** - No canonicalization bugs from `./foo` vs `foo` vs `/abs/foo`
- **Smaller storage** - Basenames are short; parent relationship is a single NodeId
- **O(1) lookup** - Hash on `(parent_id, name)` gives instant lookup
- **Efficient caching** - `get_full_path()` caches reconstructed paths

The `find_by_path()` method remains for compatibility, but internally derives from the `(parent_dir, name)` model.

### Why No libstdc++?

Putup links with `-nostdlib++` and provides its own runtime primitives:

- **Binary size** - .text dropped from 1,060 KB to 447 KB (-58%) by eliminating exception handling tables, RTTI, and unused template instantiations
- **Startup time** - No global constructors from `iostream`, `locale`, or allocator infrastructure
- **Portability** - No dependency on a specific C++ standard library version; only requires a C runtime (libc)
- **Control** - Custom `StringPool`, `Vec<T>`, `Function<Sig>`, and clock types are purpose-built for the build system's needs, with no overhead from generality

The `throw_stubs.cpp` file provides the minimal runtime surface: `operator new`/`delete` (delegating to `malloc`/`free`), `__cxa_guard_*` (for thread-safe static initialization), and `__throw_*` stubs that abort on use. On macOS, libc++ also requires `std::__1::__libcpp_verbose_abort` (used by `variant`/`optional` failure paths); the stub matches libc++'s `__1` inline ABI namespace directly. Multi-variant parallelism uses `fork()`+`waitpid()` via `platform::run_parallel_tasks()` instead of `std::async`.

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
import VAR             # Import environment variable
import VAR ?= default  # Optional default (=, ?=, ??= equivalent) when env var is unset;
                       #   reverts to the default when the env var becomes unset on a warm build
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
