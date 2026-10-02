// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <cerrno>
#include <cstdio>
#include <cstdlib>
// Darwin uses the same timespec data under these member names.
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
// close-on-exec and SIGPIPE suppression are set by socket_cloexec below.
#define SOCK_CLOEXEC 0
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1U
#define RENAME_EXCHANGE 2U
#endif
#else
#include <sys/random.h>
#endif
#include <sys/xattr.h>

namespace trackknife::core {
inline int socket_cloexec(int domain, int type, int protocol) {
    const int fd = ::socket(domain, type, protocol);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef __APPLE__
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    }
    return fd;
}
inline int accept_cloexec(int listener) {
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef __APPLE__
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    }
    return fd;
}
inline int socketpair_cloexec(int domain, int type, int protocol, int* descriptors) {
    if (::socketpair(domain, type, protocol, descriptors) != 0) {
        return -1;
    }
    ::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    return 0;
}
inline int pipe_cloexec(int* descriptors) {
    if (::pipe(descriptors) != 0) {
        return -1;
    }
    ::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    return 0;
}
inline ssize_t random_bytes(void* bytes, std::size_t size) {
#ifdef __APPLE__
    ::arc4random_buf(bytes, size);
    return static_cast<ssize_t>(size);
#else
    return ::getrandom(bytes, size, 0);
#endif
}
inline int rename_with_flags(int from_fd, const char* from, int to_fd, const char* to,
                             unsigned flags) {
#ifdef __APPLE__
    const unsigned native = flags == RENAME_NOREPLACE  ? RENAME_EXCL
                            : flags == RENAME_EXCHANGE ? RENAME_SWAP
                                                       : 0;
    if (!native) {
        errno = EINVAL;
        return -1;
    }
    return ::renameatx_np(from_fd, from, to_fd, to, native);
#else
    return ::renameat2(from_fd, from, to_fd, to, flags);
#endif
}

inline ssize_t list_extended_attributes(const int descriptor, char* names, const std::size_t size) {
#ifdef __APPLE__
    return ::flistxattr(descriptor, names, size, 0);
#else
    return ::flistxattr(descriptor, names, size);
#endif
}

inline ssize_t get_extended_attribute(const int descriptor, const char* name, void* value,
                                      const std::size_t size) {
#ifdef __APPLE__
    return ::fgetxattr(descriptor, name, value, size, 0, 0);
#else
    return ::fgetxattr(descriptor, name, value, size);
#endif
}

inline int set_extended_attribute(const int descriptor, const char* name, const void* value,
                                  const std::size_t size) {
#ifdef __APPLE__
    return ::fsetxattr(descriptor, name, value, size, 0, 0);
#else
    return ::fsetxattr(descriptor, name, value, size, 0);
#endif
}

inline int remove_extended_attribute(const int descriptor, const char* name) {
#ifdef __APPLE__
    return ::fremovexattr(descriptor, name, 0);
#else
    return ::fremovexattr(descriptor, name);
#endif
}

inline ssize_t get_path_extended_attribute(const char* path, const char* name, void* value,
                                           const std::size_t size) {
#ifdef __APPLE__
    return ::getxattr(path, name, value, size, 0, 0);
#else
    return ::getxattr(path, name, value, size);
#endif
}

inline int set_path_extended_attribute(const char* path, const char* name, const void* value,
                                       const std::size_t size) {
#ifdef __APPLE__
    return ::setxattr(path, name, value, size, 0, 0);
#else
    return ::setxattr(path, name, value, size, 0);
#endif
}

inline bool user_extended_attribute(const std::string_view name) {
#ifdef __APPLE__
    // Darwin has no Linux-style user namespace. Preserve ordinary and
    // com.apple metadata, while leaving kernel-owned attributes alone.
    return !name.starts_with("com.apple.system.");
#else
    return name.starts_with("user.");
#endif
}
} // namespace trackknife::core
