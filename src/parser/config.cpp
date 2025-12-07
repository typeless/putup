// SPDX-License-Identifier: MIT
// Copyright (c) 2024 pup authors

#include "pup/parser/config.hpp"

#include <fstream>
#include <sstream>

namespace pup::parser {

namespace {

auto trim(std::string_view s) -> std::string_view
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

constexpr auto CONFIG_PREFIX = std::string_view { "CONFIG_" };

} // namespace

auto parse_config_string(std::string_view content) -> Result<VarDb>
{
    auto db = VarDb {};

    auto pos = std::size_t { 0 };
    while (pos < content.size()) {

        auto end = content.find('\n', pos);
        if (end == std::string_view::npos)
            end = content.size();

        auto line = trim(content.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty() || line.front() == '#')
            continue;

        auto eq_pos = line.find('=');
        if (eq_pos == std::string_view::npos)
            continue;

        auto name = trim(line.substr(0, eq_pos));
        auto value = trim(line.substr(eq_pos + 1));

        if (!name.starts_with(CONFIG_PREFIX))
            continue;

        auto var_name = std::string { name.substr(CONFIG_PREFIX.size()) };
        db.set(var_name, std::string { value });
    }

    return db;
}

auto parse_config(std::filesystem::path const& path) -> Result<VarDb>
{
    auto file = std::ifstream { path };
    if (!file)
        return make_error<VarDb>(ErrorCode::NotFound, "Cannot open config file: " + path.string());

    auto ss = std::ostringstream {};
    ss << file.rdbuf();

    return parse_config_string(ss.str());
}

} // namespace pup::parser
