#pragma once
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <sys/mman.h>

#include <sys/socket.h>
#include <linux/un.h>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <utility>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <sys/epoll.h>
#include "connection.hpp"
#include <list>
#include "connection.hpp"

static size_t rw_buf_len = 5 * 1024 * 1024;
const static char* g_memfd_name = "simple_ipc";

class connection_mgr_t {
public:
    class construct_failed_exception : public std::exception {};
    class invalid_state_exception : public std::exception {};
public:
    using callback_t = std::function<void(int fd)>;
public:
    connection_mgr_t() {
    }

    ~connection_mgr_t() {
    }

    int new_connection() {
        auto fd = memfd_create(g_memfd_name, MFD_CLOEXEC);
        if (fd == -1) {
            return -1;
        }

        auto connection = std::make_unique<connection_t>(fd, rw_buf_len, true);
        connections.push_back(std::move(connection));
        return fd;
    }

private:
    void worker_proc() {

    }
private:
    std::list<std::unique_ptr<connection_t>> connections;
};