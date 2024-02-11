#pragma once
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/un.h>
#include <fcntl.h>
#include <sys/epoll.h>
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

#include "simple_ipc/detail/utility.hpp"


namespace simple::ipc {
        class connector_t {
        public:
            using callback_t = std::function<void(int fd)>;
        public:
            connector_t(std::string server_name, callback_t callback)
                    : s_name(std::move(server_name)), conn_fd(-1), cb(std::move(callback)), should_stop(true) {
                start();
            }

            ~connector_t() {
                stop();
            }

            void reconnect() {
                cond.notify_one();
            }
        private:
            void start() {
                if (!should_stop) {
                    return;
                }
                should_stop = false;

                thread = std::thread([this]() {
                    worker_proc();
                });
            }

            void stop() {
                if (should_stop) {
                    return;
                }
                should_stop = true;

                if (conn_fd != -1) {
                    close(conn_fd);
                }

                cond.notify_one();
                if (thread.joinable()) {
                    thread.join();
                }

                conn_fd = -1;
            }

            void worker_proc() {
                while (!should_stop) {
                    conn_fd = connect_to_server();
                    if (should_stop) {
                        break;
                    }

                    if (conn_fd == -1) {
                        cb(-1);
                        continue;
                    }

                    int mem_fd = receive_fd();
                    close(conn_fd);
                    conn_fd = -1;

                    if (should_stop) {
                        if (mem_fd != -1) {
                            close(mem_fd);
                        }
                        break;
                    }
                    cb(mem_fd);
                    if (mem_fd != -1) {
                        close(mem_fd);
                    }

                    std::unique_lock lk(mutex);
                    cond.wait(lk);
                }
            }

            int connect_to_server() {
                struct sockaddr_un address{};
                memset(&address, 0, sizeof(address));
                address.sun_family = AF_UNIX;
                snprintf(address.sun_path, UNIX_PATH_MAX, "%s", s_name.c_str());

                int conn = socket(PF_UNIX, SOCK_STREAM, 0);
                if (conn == -1) {
                    return -1;
                }

                int status = connect(conn, (struct sockaddr *) &address, sizeof(struct sockaddr_un));
                if (status == 0) {
                    return conn;
                }

                close(conn);
                return -1;
            }

            [[nodiscard]] int receive_fd() const {
                if (conn_fd == -1) {
                    return -1;
                }

                struct msghdr msgh{};
                struct iovec iov{};
                union {
                    struct cmsghdr cmsgh;
                    char control[CMSG_SPACE(sizeof(int))];
                } control_un{};
                struct cmsghdr *cmsgh;

                uint64_t id = 0;
                iov.iov_base = &id;
                iov.iov_len = sizeof(id);

                msgh.msg_name = nullptr;
                msgh.msg_namelen = 0;
                msgh.msg_iov = &iov;
                msgh.msg_iovlen = 1;
                msgh.msg_control = control_un.control;
                msgh.msg_controllen = sizeof(control_un.control);

                if (recvmsg(conn_fd, &msgh, 0) == -1) {
                    return -1;
                }

                cmsgh = CMSG_FIRSTHDR(&msgh);
                if (cmsgh == nullptr || cmsgh->cmsg_level != SOL_SOCKET || cmsgh->cmsg_type != SCM_RIGHTS) {
                    return -1;
                }

                return *((int *) CMSG_DATA(cmsgh));
            }
        private:
            std::string s_name;
            int conn_fd;
            callback_t cb;
            std::thread thread;
            volatile std::atomic<bool> should_stop;
            std::condition_variable cond;
            std::mutex mutex;
        };

}