// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

#include "token.hpp"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pup::parser {

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
struct VarRef : AstNode {
    enum class Kind { Regular,
                      Config,
                      Node };

    Kind kind = Kind::Regular;
    std::string name;

    VarRef() = default;
    VarRef(Kind k, std::string n, SourceLocation loc)
        : AstNode(loc)
        , kind(k)
        , name(std::move(n))
    {
    }
};

/// Expression that may contain literal text and variable references
struct Expression : AstNode {
    struct Literal {
        std::string value;
    };
    struct Variable {
        VarRef ref;
    };

    std::vector<std::variant<Literal, Variable>> parts;

    [[nodiscard]]
    auto is_literal() const -> bool
    {
        return parts.size() == 1 && std::holds_alternative<Literal>(parts[0]);
    }

    [[nodiscard]]
    auto as_literal() const -> std::string_view
    {
        if (is_literal()) {
            return std::get<Literal>(parts[0]).value;
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
struct PathPattern : AstNode {
    Expression path;
    bool is_foreach = false;          ///< Part of foreach expansion
    bool is_exclusion = false;        ///< Starts with ! for input exclusion
    bool is_output_exclusion = false; ///< Starts with ^ for output exclusion (regex pattern)
    bool is_group = false;            ///< References a bin {binname} (tup calls these "bins")
    bool is_order_only_group = false; ///< References an order-only group <groupname>
    std::string group_name;           ///< Group or bin name
};

/// Build rule: : [foreach] inputs [| order-only] |> command |> outputs [{group}] [<group>]
struct Rule : AstNode {
    bool foreach_ = false;
    std::vector<PathPattern> inputs;
    std::vector<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display; ///< Display text between ^ ^ in command
    std::vector<PathPattern> outputs;
    std::vector<PathPattern> extra_outputs;
    std::optional<std::string> output_group;               ///< {binname} at end
    std::optional<std::string> output_order_only_group;    ///< <groupname> at end
    std::optional<Expression> output_order_only_group_dir; ///< path/ prefix for <group>
};

/// Bang-macro definition: !name = |> command |> outputs
struct BangMacro : AstNode {
    std::string name;
    bool foreach_ = false;
    std::vector<PathPattern> order_only_inputs;
    Expression command;
    std::optional<Expression> display;
    std::vector<PathPattern> outputs;
    std::vector<PathPattern> extra_outputs;
    std::optional<std::string> output_group;               ///< {binname} at end
    std::optional<std::string> output_order_only_group;    ///< <groupname> at end
    std::optional<Expression> output_order_only_group_dir; ///< path/ prefix for <group>
};

/// Variable assignment: VAR = value, VAR += value, VAR := value, VAR ?= value, VAR ??= value
struct Assignment : AstNode {
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
struct Conditional : AstNode {
    enum class Kind { Ifdef,
                      Ifndef,
                      Ifeq,
                      Ifneq };

    Kind kind = Kind::Ifdef;
    std::string var_name; ///< For ifdef/ifndef
    Expression lhs;       ///< For ifeq/ifneq
    Expression rhs;       ///< For ifeq/ifneq
    std::vector<std::unique_ptr<Statement>> then_body;
    std::vector<std::unique_ptr<Statement>> else_body;

    ~Conditional() override;
    Conditional();
    Conditional(Conditional const&) = delete;
    auto operator=(Conditional const&) -> Conditional& = delete;
    Conditional(Conditional&&) noexcept;
    auto operator=(Conditional&&) noexcept -> Conditional&;
};

/// Include directive: include path, include_rules
struct Include : AstNode {
    Expression path;
    bool is_rules = false; ///< include_rules vs include
};

/// Export directive: export VAR
struct Export : AstNode {
    std::string var_name;
};

/// Import directive: import VAR[=default]
struct Import : AstNode {
    std::string var_name;
    std::optional<Expression> default_value;
};

/// .gitignore directive
struct Gitignore : AstNode { };

/// Union of all statement types
struct Statement : AstNode {
    std::variant<Rule, BangMacro, Assignment, Conditional, Include, Export, Import, Gitignore>
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
struct Tupfile : AstNode {
    std::string filename;
    std::vector<std::unique_ptr<Statement>> statements;
    std::vector<std::string> included_files;
    bool has_gitignore = false;
};

} // namespace pup::parser
