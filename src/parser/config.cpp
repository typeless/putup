// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/parser/config.hpp"

#include <fstream>

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

auto expand_escapes(std::string_view s) -> std::string
{
    auto result = std::string {};
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
    return result;
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
        auto expanded_value = expand_escapes(value);
        db.set(stripped_name, expanded_value);
        db.set(name, std::move(expanded_value));
    }

    return db;
}

auto parse_config(std::string const& path) -> Result<VarDb>
{
    auto file = std::ifstream { path };
    if (!file) {
        return make_error<VarDb>(ErrorCode::NotFound, "Cannot open config file: " + path);
    }

    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    auto content = std::string(static_cast<std::size_t>(size), '\0');
    file.read(content.data(), size);

    return parse_config_string(content);
}

} // namespace pup::parser
