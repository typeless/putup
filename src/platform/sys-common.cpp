// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "pup/platform/sys.hpp"

#ifndef _WIN32

#    include <cerrno>

namespace pup::platform::sys {

auto error_text(int err) -> char const*
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
    case ETXTBSY:
        return "Text file busy";
    case EBADF:
        return "Bad file descriptor";
    case ENOMEM:
        return "Cannot allocate memory";
    default:
        return nullptr;
    }
}

} // namespace pup::platform::sys

#endif // !_WIN32
