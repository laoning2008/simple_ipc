#pragma once
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <utility>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <random>

namespace simple::ipc {

        inline bool set_nonblocking(int fd) {
            if (fd < 0) {
                return false;
            }

            int flags = fcntl(fd, F_GETFL, 0);
            if (flags == -1) {
                return false;
            }

            flags |= O_NONBLOCK;
            return fcntl(fd, F_SETFL, flags) == 0;
        }

        inline uint64_t unique_id() {
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::uniform_int_distribution<uint64_t> dis;
            return dis(gen);
        }
}
