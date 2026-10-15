// SPDX-License-Identifier: GPL-3.0-only

// For engine_launcher_test: holds argv[1] locked, then ends its main thread
// while a second thread lives on, waiting for a byte on stdin. Until then the
// process shows as a zombie that still holds its files -- as melodyd, stopped,
// can for a moment.

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <unistd.h>

int main(const int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const int held = ::open(argv[1], O_RDWR | O_CREAT, 0600);
    if (held < 0 || ::flock(held, LOCK_EX) != 0) {
        return 1;
    }
    pthread_t waiting{};
    const auto wait_for_release = [](void*) -> void* {
        char byte = 0;
        static_cast<void>(::read(STDIN_FILENO, &byte, 1));
        return nullptr;
    };
    if (::pthread_create(&waiting, nullptr, wait_for_release, nullptr) != 0) {
        return 1;
    }
    ::pthread_exit(nullptr);
}
