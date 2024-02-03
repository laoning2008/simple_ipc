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
                    : s_name(std::move(server_name)), cb(std::move(callback)), should_stop(true) {
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

                cond.notify_one();
                if (thread.joinable()) {
                    thread.join();
                }
            }

            void worker_proc() {
                while (!should_stop) {
                    int conn_fd = connect_to_server();
                    if (should_stop) {
                        break;
                    }

                    if (conn_fd == -1) {
                        fire_callback(-1);
                        continue;
                    }

                    int mem_fd = receive_fd(conn_fd);
                    close(conn_fd);

                    if (should_stop) {
                        break;
                    }
                    fire_callback(mem_fd);

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

                if (!set_nonblocking(conn)) {
                    return -1;
                }

                int status = connect(conn, (struct sockaddr *) &address, sizeof(struct sockaddr_un));
                if (status == 0) {
                    return conn;
                }

                if (errno == EINPROGRESS) {
                    int epoll_fd = epoll_create(1);
                    if (epoll_fd == -1) {
                        close(conn);
                        return -1;
                    }

                    struct epoll_event poll_events{};
                    poll_events.data.fd = conn;
                    poll_events.events = EPOLLOUT | EPOLLIN | EPOLLERR;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn, &poll_events) == -1) {
                        close(conn);
                        close(epoll_fd);
                        return -1;
                    }

                    bool connected = false;
                    for (int i = 0; i < 100; ++i) {
                        struct epoll_event processable_events{};
                        int poll_ret = epoll_wait(epoll_fd, &processable_events, 1, 50);
                        //error
                        if (poll_ret < 0) {
                            break;
                        }

                        //stop signal
                        if (should_stop) {
                            break;
                        }

                        //timeout
                        if (poll_ret == 0) {
                            continue;
                        }

                        //seem success, let's check it
                        int ret_val = -1;
                        socklen_t ret_val_len = sizeof(ret_val);
                        if (getsockopt(conn, SOL_SOCKET, SO_ERROR, &ret_val, &ret_val_len) == 0 && ret_val == 0) {
                            connected = true;
                        }
                        break;
                    }

                    close(epoll_fd);
                    if (connected) {
                        return conn;
                    } else {
                        close(conn);
                        return -1;
                    }
                } else {
                    return -1;
                }
            }

            int receive_fd(int fd) {
                struct msghdr msgh{};
                struct iovec iov{};
                union {
                    struct cmsghdr cmsgh;
                    char control[CMSG_SPACE(sizeof(int))];
                } control_un{};
                struct cmsghdr *cmsgh;

                char placeholder = 0;
                iov.iov_base = &placeholder;
                iov.iov_len = sizeof(char);

                msgh.msg_name = nullptr;
                msgh.msg_namelen = 0;
                msgh.msg_iov = &iov;
                msgh.msg_iovlen = 1;
                msgh.msg_control = control_un.control;
                msgh.msg_controllen = sizeof(control_un.control);

                int epoll_fd = epoll_create(1);
                if (epoll_fd == -1) {
                    return -1;
                }

                struct epoll_event poll_events{};
                poll_events.data.fd = fd;
                poll_events.events = EPOLLIN;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &poll_events) == -1) {
                    close(epoll_fd);
                    return -1;
                }

                bool ready = false;
                for (int i = 0; i < 100; ++i) {
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

                    ready = true;
                    break;
                }

                close(epoll_fd);

                if (!ready) {
                    return -1;
                }

                if (recvmsg(fd, &msgh, 0) != 1) {
                    return -1;
                }

                cmsgh = CMSG_FIRSTHDR(&msgh);
                if (cmsgh == nullptr || cmsgh->cmsg_level != SOL_SOCKET || cmsgh->cmsg_type != SCM_RIGHTS) {
                    return -1;
                }

                return *((int *) CMSG_DATA(cmsgh));
            }


            void fire_callback(int fd) {
                cb(fd);
            }

        private:
            std::string s_name;
            callback_t cb;
            std::thread thread;
            volatile std::atomic<bool> should_stop;
            std::condition_variable cond;
            std::mutex mutex;
        };

}