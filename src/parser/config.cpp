// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/config.hpp"
#include "pup/core/buf.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/result.hpp"
#include "pup/core/string_id.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/eval.hpp"
#include "pup/platform/file_io.hpp"
#include <cctype>
#include <cstddef>
#include <string_view>

namespace pup::parser {

namespace {

auto trim(std::string_view s) -> std::string_view
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

auto expand_escapes(std::string_view s) -> StringId
{
    auto result = Buf {};
    result.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case 'n':
                result += '\n';
                ++i;
                break;
            case 't':
                result += '\t';
                ++i;
                break;
            case '\\':
                result += '\\';
                ++i;
                break;
            default:
                result += s[i];
                break;
            }
        } else {
            result += s[i];
        }
    }
    return result.intern(global_pool());
}

constexpr auto CONFIG_PREFIX = std::string_view { "CONFIG_" };

} // namespace

auto parse_config_string(std::string_view content) -> Result<VarDb>
{
    auto db = VarDb {};

    auto pos = std::size_t { 0 };
    while (pos < content.size()) {

        auto end = content.find('\n', pos);
        if (end == std::string_view::npos) {
            end = content.size();
        }

        auto line = trim(content.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty() || line.front() == '#') {
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string_view::npos) {
            continue;
        }

        auto name = trim(line.substr(0, eq_pos));
        auto value = trim(line.substr(eq_pos + 1));

        // Strip surrounding quotes if present (tup config files often quote string values)
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (!name.starts_with(CONFIG_PREFIX)) {
            continue;
        }

        // Store both forms for compatibility:
        // - Stripped form (FOO) for @(FOO) syntax
        // - Full form (CONFIG_FOO) for $(CONFIG_FOO) syntax
        auto stripped_name = name.substr(CONFIG_PREFIX.size());
        auto expanded_value_id = expand_escapes(value);
        auto expanded_value_sv = global_pool().get(expanded_value_id);
        db.set(stripped_name, expanded_value_sv);
        db.set(name, expanded_value_sv);
    }

    return db;
}

auto parse_config(std::string_view path) -> Result<VarDb>
{
    auto content = Buf {};
    if (!pup::platform::read_file(path, content)) {
        auto err = Buf {};
        err.fmt("Cannot open config file: {}", path);
        return make_error<VarDb>(ErrorCode::NotFound, err.view());
    }
    return parse_config_string(content.view());
}

} // namespace pup::parser
