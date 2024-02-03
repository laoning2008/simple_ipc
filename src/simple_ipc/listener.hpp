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

#include "connection_mgr.hpp"

namespace simple { namespace ipc {

        class listener_t {
            constexpr static char *memfd_name = "simple_ipc";
        public:
            class construct_failed_exception : public std::exception {
            };
        public:
            using callback_t = std::function<void(int fd)>;
        public:
            listener_t(std::string server_name, callback_t callback)
                    : s_name(std::move(server_name)), cb(std::move(callback)), should_stop(true), sock_fd(-1) {
                if (!init_listen()) {
                    throw construct_failed_exception{};
                }

                start();
            }

            ~listener_t() {
                stop();
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

                if (thread.joinable()) {
                    thread.join();
                }
            }

            void worker_proc() {
                int epoll_fd = epoll_create(1);
                if (epoll_fd == -1) {
                    return;
                }

                struct epoll_event poll_events{};
                poll_events.data.fd = sock_fd;
                poll_events.events = EPOLLIN;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &poll_events) == -1) {
                    close(epoll_fd);
                    return;
                }

                while (!should_stop) {
                    struct epoll_event processable_events{};
                    int poll_ret = epoll_wait(epoll_fd, &processable_events, 1, 50);

                    //stop signal
                    if (should_stop) {
                        break;
                    }

                    //error
                    if (poll_ret < 0) {
                        break;
                    }

                    //timeout
                    if (poll_ret == 0) {
                        continue;
                    }

                    struct sockaddr_un address;
                    socklen_t addr_len = sizeof(address);
                    int conn = accept(sock_fd, (struct sockaddr *) &address, &addr_len);
                    if (conn == -1) {
                        continue;
                    }

                    auto mem_fd = memfd_create(memfd_name, MFD_CLOEXEC);
                    if (mem_fd == -1) {
                        continue;
                    }

                    send_fd(conn, mem_fd);
                    cb(mem_fd);
                    close(mem_fd);
                }

                close(epoll_fd);
            }

            void send_fd(int conn, int fd) {
                struct msghdr msgh;
                struct iovec iov;
                union {
                    struct cmsghdr cmsgh;
                    /* Space large enough to hold an 'int' */
                    char control[CMSG_SPACE(sizeof(int))];
                } control_un;

                char placeholder = 'A';
                iov.iov_base = &placeholder;
                iov.iov_len = sizeof(char);

                msgh.msg_name = NULL;
                msgh.msg_namelen = 0;
                msgh.msg_iov = &iov;
                msgh.msg_iovlen = 1;
                msgh.msg_control = control_un.control;
                msgh.msg_controllen = sizeof(control_un.control);

                /* Write the fd as ancillary data */
                control_un.cmsgh.cmsg_len = CMSG_LEN(sizeof(int));
                control_un.cmsgh.cmsg_level = SOL_SOCKET;
                control_un.cmsgh.cmsg_type = SCM_RIGHTS;
                *((int *) CMSG_DATA(CMSG_FIRSTHDR(&msgh))) = fd;

                sendmsg(conn, &msgh, 0);
            }

            bool init_listen() {
                sock_fd = socket(PF_UNIX, SOCK_STREAM, 0);
                if (sock_fd == -1) {
                    return false;
                }

                struct sockaddr_un address;
                socklen_t addr_len = 0;
                memset(&address, 0, sizeof(address));
                address.sun_family = AF_UNIX;
                snprintf(address.sun_path, UNIX_PATH_MAX, "%s", s_name.c_str());

                int ret = unlink(s_name.c_str());
                if (ret != 0 && ret != -ENOENT && ret != -EPERM) {
                    return false;
                }

                if (bind(sock_fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
                    return false;
                }

                if (listen(sock_fd, 128) != 0) {
                    sock_fd = -1;
                }

                return sock_fd != -1;
            }
        private:
            std::string s_name;
            callback_t cb;
            std::thread thread;
            volatile std::atomic<bool> should_stop;
            int sock_fd;
            connection_mgr_t conn_mgr;
        };

}}