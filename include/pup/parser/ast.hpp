// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "pup/core/global_pool.hpp"
#include "pup/core/source_location.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/core/vec.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace pup::parser {

using pup::SourceLocation;

/// Base for all AST nodes with source location tracking
struct AstNode {
    SourceLocation location;

    AstNode() = default;
    explicit AstNode(SourceLocation loc)
        : location(loc)
    {
    }
    virtual ~AstNode() = default;

    AstNode(AstNode const&) = default;
    AstNode(AstNode&&) = default;
    auto operator=(AstNode const&) -> AstNode& = default;
    auto operator=(AstNode&&) -> AstNode& = default;
};

/// Variable reference: $(VAR), @(CONFIG), &(NODE)
struct VarRef final : AstNode {
    enum class Kind { Regular,
                      Config,
                      Node };

    Kind kind = Kind::Regular;
    StringId name = StringId::Empty;

    VarRef() = default;
    VarRef(Kind k, StringId n, SourceLocation loc)
        : AstNode(loc)
        , kind(k)
        , name(n)
    {
    }
};

/// Expression that may contain literal text and variable references
struct Expression final : AstNode {
    struct Literal {
        StringId value = StringId::Empty;
    };
    struct Variable {
        VarRef ref;
    };

    Vec<std::variant<Literal, Variable>> parts;

    [[nodiscard]]
    auto is_literal() const -> bool
    {
        return parts.size() == 1 && std::holds_alternative<Literal>(parts[0]);
    }

    [[nodiscard]]
    auto as_literal() const -> std::string_view
    {
        if (is_literal()) {
            return global_pool().get(std::get<Literal>(parts[0]).value);
        }
        return {};
    }

    [[nodiscard]]
    auto empty() const -> bool
    {
        return parts.empty();
    }
};

/// Input/output path pattern (may include globs, groups, exclusions)
struct PathPattern final : AstNode {
    Expression path;
    bool is_foreach = false;               ///< Part of foreach expansion
    bool is_exclusion = false;             ///< Starts with ! for input exclusion
    bool is_output_exclusion = false;      ///< Starts with ^ for output exclusion (regex pattern)
    bool is_group = false;                 ///< References a bin {binname} (tup calls these "bins")
    bool is_order_only_group = false;      ///< References an order-only group <groupname>
    StringId group_name = StringId::Empty; ///< Group or bin name
};

/// Build rule: : [foreach] inputs [| order-only] |> command |> outputs [{group}] [<group>]
struct Rule final : AstNode {
    bool foreach_ = false;
    Vec<PathPattern> inputs;
    Vec<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display; ///< Display text between ^ ^ in command
    Vec<PathPattern> outputs;
    Vec<PathPattern> extra_outputs;
    std::optional<StringId> output_group;                  ///< {binname} at end
    std::optional<StringId> output_order_only_group;       ///< <groupname> at end
    std::optional<Expression> output_order_only_group_dir; ///< path/ prefix for <group>
};

/// Bang-macro definition: !name = |> command |> outputs
struct BangMacro final : AstNode {
    StringId name = StringId::Empty;
    bool foreach_ = false;
    Vec<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display;
    Vec<PathPattern> outputs;
    Vec<PathPattern> extra_outputs;
    std::optional<StringId> output_group;                  ///< {binname} at end
    std::optional<StringId> output_order_only_group;       ///< <groupname> at end
    std::optional<Expression> output_order_only_group_dir; ///< path/ prefix for <group>
};

/// Variable assignment: VAR = value, VAR += value, VAR := value, VAR ?= value, VAR ??= value
struct Assignment final : AstNode {
    enum class Op {
        Set,     // =
        Append,  // +=
        Define,  // :=
        SoftSet, // ?=  - set if unset (immediate, first wins)
        WeakSet  // ??= - set if unset (deferred, last wins)
    };

    Expression name; ///< Variable name (may contain variable refs like foo-$(BAR))
    Op op = Op::Set;
    Expression value;
    VarRef::Kind var_kind = VarRef::Kind::Regular; ///< For &var = assignments
};

// Forward declaration
struct Statement;

/// Conditional: ifdef, ifndef, ifeq, ifneq
struct Conditional final : AstNode {
    enum class Kind { Ifdef,
                      Ifndef,
                      Ifeq,
                      Ifneq };

    Kind kind = Kind::Ifdef;
    StringId var_name = StringId::Empty; ///< For ifdef/ifndef
    Expression lhs;                      ///< For ifeq/ifneq
    Expression rhs;                      ///< For ifeq/ifneq
    Vec<std::unique_ptr<Statement>> then_body;
    Vec<std::unique_ptr<Statement>> else_body;

    ~Conditional() override;
    Conditional();
    Conditional(Conditional const&) = delete;
    auto operator=(Conditional const&) -> Conditional& = delete;
    Conditional(Conditional&&) noexcept;
    auto operator=(Conditional&&) noexcept -> Conditional&;
};

/// Include directive: include path, include_rules
struct Include final : AstNode {
    Expression path;
    bool is_rules = false; ///< include_rules vs include
};

/// Export directive: export VAR
struct Export final : AstNode {
    StringId var_name = StringId::Empty;
};

/// Import directive: import VAR[=default]
struct Import final : AstNode {
    StringId var_name = StringId::Empty;
    std::optional<Expression> default_value;
};

/// Union of all statement types
struct Statement final : AstNode {
    std::variant<Rule, BangMacro, Assignment, Conditional, Include, Export, Import>
        content;

    template<typename T>
    [[nodiscard]]
    auto is() const -> bool
    {
        return std::holds_alternative<T>(content);
    }

    template<typename T>
    [[nodiscard]]
    auto as() -> T*
    {
        return std::get_if<T>(&content);
    }

    template<typename T>
    [[nodiscard]]
    auto as() const -> T const*
    {
        return std::get_if<T>(&content);
    }
};

/// A complete Tupfile AST
struct Tupfile final : AstNode {
    StringId filename = StringId::Empty;
    Vec<std::unique_ptr<Statement>> statements;
    Vec<StringId> included_files;
};

} // namespace pup::parser
