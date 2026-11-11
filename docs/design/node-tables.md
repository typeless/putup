# Node tables: live-node handles and column storage

Status: proposed, not built. Base: e294e76 (main after #568).

## Requirements

- **R1 — #496 part 2.** "The id-keyed graph accessors answer an id that resolves to no node with a default that is also a legitimate answer." The invariant "callers pass live ids" is "conventional, not structural." Part 1 (#568) separated the source root from "no node". Part 2 is the accessors.
- **R2 — keep the id-keyed seam.** Commits 645ef5d, 89df4c7, 714cc9d, 3620075 (2026-04-13) moved callers from node pointers to "ECS-style property accessor[s]". get_file_node/get_command_node are internal; builder.cpp and test_graph.cpp declare them locally.
- **R3 — the user's request (2026-11-03 session):** "give the SoA/data-oriented design a shot."
- **R4 — CLAUDE.md:** invariants are structural (constructor-enforced), not conventional. Validate once at construction; observers never re-check.

## Measurements (BSP example, 3544 commands, 5158 files)

- Null build (`-n`): parse 285 ms, job list 110 ms, total 513 ms.
- Node storage: about 0.27 MB of files (FileNode is 52 bytes) and 0.96 MB of commands (CommandNode is 272 bytes). It all fits in cache.
- `sample` of `putup -n`, top of stack: lstat 155, StringPool::intern 88, sha256_transform 27, memset 21, stat 15. No node accessor or node scan is among the hot frames.

**Consequence:** no requirement asks for column storage on speed grounds. Column storage stands on R3 and on what it removes (below). It is not claimed to make anything faster. CI metrics (parse 1547 M instructions, dry-run 2339 M) will be reported before and after, as observation.

## Field inventory: what the code does with node fields

- **Identity is position.** Ids are assigned densely (`next_file_id++`, `next_command_id+1`), and no slot is ever removed. `FileNode::id` and `CommandNode::id` restate the slot index. `validate_node_id` re-checks `files[i].id == i` on every lookup, so that convention gets verified at run time instead of being impossible to break.
- **Every loop over all nodes reads only id and type** (`all_nodes`, `nodes_of_type`). It then makes a per-id accessor call for any other field.
- **Writes after creation:**
  - `type`: Ghost or File becomes Generated (ensure_file_node, 4 copies).
  - `name`, `path_id`, `content_hash`: ensure_env_var_node.
  - `name` of the build root: set_build_root_name.
  - The command operand lists, `generated_output`, `output_action`, `parent_command` and `instruction`: builder.cpp, through the mutable get_command_node.
- **Dead fields, with no reader in src/:**
  - `FileNode::flags`: it is never written, so `get<NodeFlags>` always answers None. Its one reader, cmd_build.cpp:615, copies that None into index entries. The index-side `FileEntry` flags are a separate field and stay. test_graph.cpp:428-449 sets and checks the graph field, and that test section goes with it.
  - `ConditionNode::expression`
  - `ConditionNode::config_var_dep`
  - the whole `PhiNode` kind (no caller of add_phi_node or get_phi_node)
  - `root_nodes` and `leaf_nodes` (no production caller)
- **No memcpy, sizeof or struct comparison** of FileNode or CommandNode anywhere. The index is serialized field by field.

## Design

### Step 1 — dead-field deletion (precursor; its own commit)

Delete `FileNode::flags`, `ConditionNode::{expression, config_var_dep}`, `PhiNode` with its storage and accessors, and `root_nodes`/`leaf_nodes`. The phi tag in NodeId and `NodeType::Phi` stay: the 2-bit tag decodes every uint32 to one kind, and `NodeType::Phi` is an index-format value. NodeIdMap32 keeps its phi bucket for the same reason. Step 3 has no phi table. This shrinks the column set in step 3 and has no behavior change.

### Step 2 — live-node handles (R1, R4)

- Add `FileId` and `CommandId`: strong index types that only the graph can construct.
- `add_file_node` and `add_command_node` return them. `find_file(graph, NodeId) -> std::optional<FileId>` and `find_command(...)` resolve an untrusted NodeId (from an edge, the index, or a path lookup) once.
- Every accessor takes a handle and is total:
  - `get<Tag>(graph, FileId)` for file tags;
  - `get<Tag>(graph, CommandId)` for command tags;
  - `view<Tag>(graph, CommandId)`;
  - `get_parent_dir(graph, FileId) -> FileId`;
  - `get_full_path(graph, FileId)`.
  
  The other id-keyed members that #496 names also take a handle:
  - `is_stdout_capture`, `get_parent_command` (which returns `optional<CommandId>`), `is_guard_satisfied`, `compute_command_key`, `compute_command_signature`, `expand_instruction` (3 overloads) and `command_label` (3 overloads) take a `CommandId`;
  - `find_by_dir_name` takes a `FileId` parent.

  A miss cannot reach an accessor, so the defaults in `read_file_field` and `read_command_field` are deleted. Passing a FileId to a command accessor no longer compiles. That fixes the second class that #496 lists (a command-tagged id answering `Command` without a lookup, and `find_by_dir_name` reading a file slot with a command id's index).
- `all_nodes` and `nodes_of_type` split into `file_ids(graph)` and `command_ids(graph)`. These are iota ranges over the dense table, so they produce handles without a lookup. A type filter reads the type column.
- `NodeId` stays the tagged identity for edges and the index. Converting between NodeId and a handle happens at those two boundaries only.
- **Residual:** a handle from one Graph used on another Graph is still accepted. The program holds one Graph per variant. This is recorded as a residual, not fenced.

### Step 3 — column storage (R3; lands behind step 2's seam)

- Replace `StableVec<FileNode>` with a `FileTable`: one `StableVec` per field (`type`, `name`, `parent_dir`, `path_id`, `content_hash`).
- Replace `StableVec<CommandNode>` with a `CommandTable`: one `StableVec` per field.
- The `id` fields are gone, because the column index is the identity. `validate_node_id` reduces to a bounds check inside `find_file` and `find_command`.
- `FileNode` and `CommandNode` are deleted. `add_file_node(graph, type, name, parent_dir, content_hash) -> Result<FileId>` and `add_command_node(graph, …fields…) -> Result<CommandId>` take the fields as parameters. `path_id` is derived inside, as it is today.
- The post-creation writes listed above become column writes through five mutators, each taking a handle. No mutable node pointer escapes dag.cpp, and the local white-box declarations in builder.cpp, test_graph.cpp and test_builder.cpp go away.

  | Mutator | Replaces the write at |
  |---|---|
  | `set_type(FileId, NodeType)` | ensure_file_node |
  | `rebind_env_var(FileId, name, content_hash)`, which derives `path_id` the way add_file_node does | builder.cpp:1533-1535 |
  | `set_operands(CommandId, inputs, order_only_inputs, outputs)` | builder.cpp:2519-2526 |
  | `set_generated(CommandId, generated_output, output_action, parent_command, inputs)` | builder.cpp:2090-2094 |
  | `set_instruction(CommandId, Instruction)` | builder.cpp:3137 |

  `set_build_root_name` keeps its current signature.
- `Region` reserves 1 GiB of address space per `StableVec`, so the columns add about 17 GiB of virtual reservation. Before step 3, check this against the Windows CI runner and decide on a smaller per-column reserve.
- **Granularity:** one column per field, with no hot/cold grouping. A grouping would need a profile that shows a hot field set, and the measurement above shows none.
- `StableVec` per column keeps a `view<Inputs>` reference valid while further commands are added. That matches the guarantee callers have today.

### Pins

- Each step keeps the full suite green. Self-parse stays at 125 commands and BSP parse at 1851 commands.
- Step 3 is a pure layout change. Its differential pin is a byte-identical `.pup/index` for the self-build and the BSP, before and after.

## Decision record

Design-attorney round 1 (2026-11-03) found the design OVER-BUILT on C3 only. The entries below record how each point was settled.

- **C-full / #496 option 3 rejected under R3.** Re-exposing node pointers and deleting `get<Tag>` is smaller (2 fewer types, 14 fewer operations) and meets R1. It needs a record in storage, though, and R3 asks for columns. R2 records what the April commits did; it does not require that it stay.
- **Step 2 is independent of step 3.** Step 2 alone closes #496. #496 found no break reachable from production, so step 2 stands on R4: the live-id invariant is conventional today and becomes structural.
- **R3 overrides R4 for storage.** R4 says "parallel containers kept in step → one struct per row", which favors AoS. The user's later, specific request for SoA overrides it. FileTable and CommandTable confine the in-step invariant to one insert site.
- **C3: insertion records withdrawn.** The add_* functions take fields as parameters. That gives 2 fewer types, and it removes `FileNode::path_id`, which is dead at insertion.
- **C4: one column per field.** This reads R3 ("SoA") literally. Grouping hot and cold fields would be a trade with no profile to back it.
- **C5: one StableVec per column.** This keeps a pointer held across growth valid (DESIGN.md:261-263).
