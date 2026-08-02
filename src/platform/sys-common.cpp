// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/sys.hpp"

#ifndef _WIN32

#    include <cerrno>
#    include <cstddef>
#    include <cstdint>
#    include <cstring>

namespace pup::platform::sys {

namespace {

constexpr auto MAX_SYMLINK_HOPS = 40;

auto strip_last_component(char const* resolved, std::size_t len) -> std::size_t
{
    while (len > 0 && resolved[len - 1] != '/') {
        --len;
    }
    return len > 0 ? len - 1 : 0;
}

} // namespace

auto error_text(int err) -> std::string_view
{
    switch (err < 0 ? -err : err) {
    case EACCES:
        return "Permission denied";
    case EPERM:
        return "Operation not permitted";
    case ENOENT:
        return "No such file or directory";
    case ENOTEMPTY:
        return "Directory not empty";
    case EBUSY:
        return "Device or resource busy";
    case EROFS:
        return "Read-only file system";
    case EISDIR:
        return "Is a directory";
    case ENOTDIR:
        return "Not a directory";
    case EEXIST:
        return "File exists";
    case ENOSPC:
        return "No space left on device";
    case EIO:
        return "Input/output error";
    case ELOOP:
        return "Too many levels of symbolic links";
    case ENAMETOOLONG:
        return "File name too long";
    case EMFILE:
        return "Too many open files";
    case ENFILE:
        return "Too many open files in system";
    case EXDEV:
        return "Invalid cross-device link";
    case EBADF:
        return "Bad file descriptor";
    case ENOMEM:
        return "Cannot allocate memory";
    default:
        return {};
    }
}

auto realpath(char const* path, char (&out)[path_max]) -> std::int64_t
{
    if (path == nullptr || path[0] == '\0') {
        return -ENOENT;
    }
    auto const path_len = std::strlen(path);
    if (path_len >= path_max) {
        return -ENAMETOOLONG;
    }

    char resolved[path_max];
    auto len = std::size_t { 0 };

    char work[path_max];
    auto work_len = std::size_t { 0 };

    if (path[0] != '/') {
        auto cwd_len = getcwd(resolved, sizeof(resolved));
        if (cwd_len < 0) {
            return cwd_len;
        }
        len = static_cast<std::size_t>(cwd_len);
        if (len == 1 && resolved[0] == '/') {
            len = 0;
        }
    }
    std::memcpy(work, path, path_len);
    work_len = path_len;

    auto wpos = std::size_t { 0 };
    auto links = 0;

    while (wpos < work_len) {
        if (work[wpos] == '/') {
            ++wpos;
            continue;
        }
        auto const comp_start = wpos;
        while (wpos < work_len && work[wpos] != '/') {
            ++wpos;
        }
        auto const comp_len = wpos - comp_start;

        if (comp_len == 1 && work[comp_start] == '.') {
            continue;
        }
        if (comp_len == 2 && work[comp_start] == '.' && work[comp_start + 1] == '.') {
            len = strip_last_component(resolved, len);
            continue;
        }

        if (len + 1 + comp_len >= path_max) {
            return -ENAMETOOLONG;
        }
        resolved[len] = '/';
        std::memcpy(resolved + len + 1, work + comp_start, comp_len);
        len += 1 + comp_len;
        resolved[len] = '\0';

        auto st = Stat {};
        auto rc = lstat(resolved, st);
        if (rc != 0) {
            return rc;
        }
        if (!is_lnk(st.mode)) {
            continue;
        }

        if (++links > MAX_SYMLINK_HOPS) {
            return -ELOOP;
        }
        char target[path_max];
        auto tlen = readlink(resolved, target, sizeof(target));
        if (tlen < 0) {
            return tlen;
        }
        auto const target_len = static_cast<std::size_t>(tlen);
        if (target_len >= path_max) {
            return -ENAMETOOLONG;
        }

        len -= 1 + comp_len;
        if (target[0] == '/') {
            len = 0;
        }

        auto const rem = work_len - wpos;
        if (target_len + rem >= path_max) {
            return -ENAMETOOLONG;
        }
        std::memmove(work + target_len, work + wpos, rem);
        std::memcpy(work, target, target_len);
        work_len = target_len + rem;
        wpos = 0;
    }

    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }
    std::memcpy(out, resolved, len);
    out[len] = '\0';
    return static_cast<std::int64_t>(len);
}

} // namespace pup::platform::sys

#endif // !_WIN32
