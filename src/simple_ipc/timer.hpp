#pragma once
#include <chrono>
#include <functional>
#include <queue>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <thread>
#include <mutex>
#include <atomic>
using namespace std::chrono;

namespace simple { namespace ipc {
    using callback_t = std::function<void()>;

        class timer_mgr_t {
            struct timer_t {
                std::chrono::time_point<std::chrono::system_clock> expire_time;
                callback_t callback;

                timer_t(std::chrono::time_point<std::chrono::system_clock> exp, callback_t cb) : expire_time(exp), callback(std::move(cb)) {}
                bool operator<(const timer_t &other) const { return expire_time > other.expire_time; }
            };

            timer_mgr_t() : timer_fd(-1), to_stop(false) {
                timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
                worker = std::thread([this]() {
                    worker_proc();
                });
            }

            ~timer_mgr_t() {
                stop();
            }

            void stop() {
                to_stop = true;
                long v = 0;
                write(timer_fd, &v, sizeof(v));
                if (worker.joinable()) {
                    worker.join();
                }
            }

            void run_after(callback_t cb, uint64_t milseconds) {
                auto expire_time = std::chrono::system_clock::now() + milseconds * 1ms;
                timer_t timer(expire_time, std::move(cb));

                std::unique_lock<std::mutex> lk(queue_mutex);
                if (queue.empty() || (expire_time < queue.top().expire_time)) {
                    reset_expire_time(expire_time);
                }
                queue.push(timer);
            }

        private:
            void worker_proc() {
                int ep_fd = epoll_create1(EPOLL_CLOEXEC);
                epoll_event event{};
                event.events = EPOLLIN|EPOLLPRI;
                epoll_ctl(ep_fd, EPOLL_CTL_ADD, timer_fd, &event);

                while (!to_stop) {
                    struct epoll_event processable_events{};
                    if (epoll_wait(ep_fd, &processable_events, 1, -1) == -1) {
                        break;
                    }

                    long one;
                    if (read(timer_fd, &one, sizeof(one) < 0)) {
                        break;
                    }

                    auto callbacks = expired_callbacks();
                    for (auto &callback: callbacks) {
                        callback();
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
            int timer_fd;
            volatile std::atomic<bool> to_stop;

            std::priority_queue<timer_t> queue;
            std::mutex queue_mutex;

            std::thread worker;
        };

}}