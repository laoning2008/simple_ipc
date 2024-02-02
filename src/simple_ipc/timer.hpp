#pragma once
#include <chrono>
#include <functional>
#include <queue>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <thread>
#include <mutex>
#include <atomic>
using namespace std::chrono;

namespace simple { namespace ipc {
    using callback_t = std::function<void()>;

        class timer_mgr_t {
            struct timer_t {
                uint64_t id;
                std::chrono::time_point<std::chrono::system_clock> expire_time;
                callback_t callback;

                timer_t(uint64_t timer_id, std::chrono::time_point<std::chrono::system_clock> exp, callback_t cb)
                : id(timer_id), expire_time(exp), callback(std::move(cb)) {}
                bool operator<(const timer_t &other) const { return expire_time > other.expire_time; }
            };

        public:
            timer_mgr_t() : event_fd(-1), timer_fd(-1), to_stop(false), cur_id(0) {
                event_fd = eventfd(0, TFD_CLOEXEC);
                timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
                worker = std::thread([this]() {
                    worker_proc();
                });
            }

            ~timer_mgr_t() {
                stop();
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

                close(timer_fd);
                close(event_fd);
            }

            uint64_t set_timer(callback_t cb, uint64_t milseconds) {
                auto expire_time = std::chrono::system_clock::now() + milseconds * 1ms;
                auto id = ++cur_id;
                timer_t timer(id, expire_time, std::move(cb));

                std::unique_lock<std::mutex> lk(queue_mutex);
                if (queue.empty() || (expire_time < queue.top().expire_time)) {
                    reset_expire_time(expire_time);
                }
                queue.push(timer);
                return id;
            }

            void cancel_timer(uint64_t id) {
                std::unique_lock<std::mutex> lk(queue_mutex);
            }

        private:
            void worker_proc() {
                int epoll_fd = epoll_create1(EPOLL_CLOEXEC);

                struct epoll_event event_timer;
                event_timer.events = EPOLLIN;
                event_timer.data.fd = timer_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &event_timer);

                struct epoll_event event_event;
                event_event.events = EPOLLIN;
                event_event.data.fd = event_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &event_event);

                const int max_events = 2;
                struct epoll_event events[max_events];

                while (!to_stop) {
                    int nfd = epoll_wait(epoll_fd, events, max_events, -1);
                    if (nfd < 0) {
                        break;
                    }

                    if (nfd == 0) {
                        continue;
                    }

                    bool exit = false;
                    for (int i = 0; i < nfd; ++i) {
                        if (events[i].data.fd == timer_fd) {
                            uint64_t one;
                            if (read(timer_fd, &one, sizeof(one) <= 0)) {
                                exit = true;
                                break;
                            }

                            auto callbacks = expired_callbacks();
                            for (auto &callback: callbacks) {
                                callback();
                            }
                        } else if (events[i].data.fd == event_fd){
                            exit = true;
                            break;
                        } else {
                            exit = true;
                            break;
                        }
                    }

                    if (exit) {
                        break;
                    }
                }
            }

            std::vector<callback_t> expired_callbacks() {
                auto now = std::chrono::system_clock::now();
                std::vector<callback_t> callbacks;

                std::unique_lock<std::mutex> lk(queue_mutex);

                while (!queue.empty()) {
                    if (now < queue.top().expire_time) {
                        break;
                    }

                    auto cb = queue.top();
                    queue.pop();
                    callbacks.push_back(cb.callback);
                }

                if (!queue.empty()) {
                    auto min_expire_time = queue.top().expire_time;
                    reset_expire_time(min_expire_time);
                }
                return callbacks;
            }

            void reset_expire_time(std::chrono::time_point<std::chrono::system_clock> min_expire_time) const {
                auto diff = duration_cast<nanoseconds>(min_expire_time - system_clock::now()).count();

                itimerspec it{};
                timespec ts{};

                ts.tv_sec = diff / 1000000000;
                ts.tv_nsec = diff % 1000000000;

                struct itimerspec oldValue {};
                it.it_value = ts;

                timerfd_settime(timer_fd, 0, &it, &oldValue);
            }

        private:
            int event_fd;
            int timer_fd;
            volatile std::atomic<bool> to_stop;
            std::atomic<uint64_t> cur_id;

            std::priority_queue<timer_t> queue;
            std::mutex queue_mutex;

            std::thread worker;
        };

}}