#pragma once

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace simple::ipc {
        class timer_mgr_t {
            using callback_t = std::function<void()>;
            struct timer_t {
                bool one_shot;
                callback_t callback;

                timer_t(bool once = true, callback_t cb = nullptr)
                        : one_shot(once), callback(std::move(cb)) {}
            };

            using map_fd_2_timer_t = std::unordered_map<int, timer_t>;
            constexpr static int max_fd_size = 1024;
        public:
            timer_mgr_t() : epoll_fd(-1), event_fd(-1), to_stop(false) {
            }

            ~timer_mgr_t() {
                stop();
            }

            bool start() {
                if (!to_stop) {
                    return true;
                }
                to_stop = false;

                epoll_fd = epoll_create1(EPOLL_CLOEXEC);

                event_fd = eventfd(0, TFD_CLOEXEC);
                struct epoll_event event_timer;
                event_timer.events = EPOLLIN;
                event_timer.data.fd = event_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &event_timer) == -1) {
                    return false;
                }

                worker = std::thread([this]() {
                    worker_proc();
                });

                return true;
            }

            void stop() {
                if (to_stop) {
                    return;
                }
                to_stop = true;

                uint64_t v = 1;
                write(event_fd, &v, sizeof(v));

                if (worker.joinable()) {
                    worker.join();
                }

                for (auto& timer : timers) {
                    close(timer.first);
                }
                timers.clear();

                close(event_fd);
                event_fd = -1;

                close(epoll_fd);
                epoll_fd = -1;
            }

            int start_timer(callback_t cb, uint64_t mills, bool once) {
                auto timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

                if (timer_fd == -1) {
                    return -1;
                }

                timespec ts{};
                if (mills) {
                    ts.tv_sec  =  mills / 1000;
                    ts.tv_nsec = (mills % 1000) * 1000000;
                } else {
                    ts.tv_sec  = 0;
                    ts.tv_nsec = 0;
                }

                itimerspec it{};
                it.it_value = ts;
                if (!once) {
                    it.it_interval = ts;
                }

                if (timerfd_settime(timer_fd, 0, &it, nullptr) == -1) {
                    return -1;
                }

                struct epoll_event event_event;
                event_event.events = EPOLLIN|EPOLLRDHUP;
                event_event.data.fd = timer_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &event_event) == -1) {
                    return -1;
                }

                {
                    std::unique_lock<std::mutex> lk(timers_mutex);
                    timers[timer_fd] = timer_t{once, std::move(cb)};
                }

                return timer_fd;
            }

            void stop_timer(int fd) {
                std::unique_lock<std::mutex> lk(timers_mutex);
                auto it = timers.find(fd);
                if (it == timers.end()) {
                    return;
                }

                if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, it->first, nullptr) == -1) {
                    return;
                }

                close(it->first);
                timers.erase(it);
            }

        private:
            void worker_proc() {
                struct epoll_event events[max_fd_size];

                while (!to_stop) {
                    int nfd = epoll_wait(epoll_fd, events, max_fd_size, -1);
                    if (nfd < 0) {
                        break;
                    }

                    if (nfd == 0) {
                        continue;
                    }

                    bool exit = false;
                    for (int i = 0; i < nfd; ++i) {
                        if (events[i].data.fd == event_fd) {
                            exit = true;
                            break;
                        } else {
                            uint64_t one = 0;
                            if (read(events[i].data.fd, &one, sizeof(one) <= 0)) {
                                stop_timer(events[i].data.fd);
                                continue;
                            }

                            bool one_shot = false;
                            {
                                std::unique_lock<std::mutex> lk(timers_mutex);
                                auto it = timers.find(events[i].data.fd);
                                if (it != timers.end()) {
                                    one_shot = it->second.one_shot;
                                    it->second.callback();
                                }
                            }

                            if (one_shot) {
                                stop_timer(events[i].data.fd);
                            }
                        }
                    }

                    if (exit) {
                        break;
                    }
                }
            }
        private:
            int epoll_fd;
            int event_fd;

            map_fd_2_timer_t timers;
            std::mutex timers_mutex;

            volatile std::atomic<bool> to_stop;
            std::thread worker;
        };
    }