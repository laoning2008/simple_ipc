#pragma once

#include <pthread.h>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <list>
#include <utility>

#include "simple_ipc/detail/ringbuffer.hpp"
#include "simple_ipc/detail/packet.hpp"
#include "simple_ipc/detail/timer.hpp"

namespace simple::ipc {
    using recv_callback_t = std::function<void(std::unique_ptr<packet> pack)>;

        struct control_block_t {
            pthread_mutex_t c_lock{};
            pthread_mutex_t s_lock{};

            pthread_cond_t c_can_r_con{};
            pthread_cond_t c_can_w_con{};
            pthread_cond_t s_can_r_con{};
            pthread_cond_t s_can_w_con{};

            linear_ringbuffer_t c_buf;
            linear_ringbuffer_t s_buf;
        };

        constexpr static const size_t rw_buf_len = 1024 * 1024;
        constexpr static const uint32_t active_connection_lifetime_seconds = 60;
        constexpr static const uint64_t cond_wait_time_ns = 100*1000*1000;

        static size_t cal_mem_size() {
            static const size_t PAGE_SIZE = sysconf(_SC_PAGESIZE);
            size_t min_size = sizeof(control_block_t) + 2 * rw_buf_len;

            // Round up to nearest multiple of page size.
            size_t actual_bytes = min_size & ~(PAGE_SIZE - 1);
            if (min_size % PAGE_SIZE) {
                actual_bytes += PAGE_SIZE;
            }

            return actual_bytes;
        }

        static const size_t mm_len = cal_mem_size();

        class connection_t {
            struct request {
                uint32_t timeout_secs{};
                recv_callback_t callback;
                std::chrono::steady_clock::time_point begin_time;

                [[nodiscard]] bool should_wait_for_response() const {
                    return callback != nullptr && timeout_secs != 0;
                }
            };

            using map_packid_2_callback_t = std::unordered_map<uint64_t, request >;
            using packet_list_t = std::list<std::pair<std::unique_ptr<packet>, request>>;
        public:
            using got_process_id_callback_t = std::function<void(connection_t* conn, uint32_t process_id)>;
            using disconnected_callback_t = std::function<void(connection_t* conn, uint32_t process_id)>;
            using receive_push_callback_t = std::function<void(connection_t* conn, std::unique_ptr<packet>)>;
        public:
            connection_t(bool server, timer_mgr_t& t
                         , disconnected_callback_t disconnected_cb
                         , receive_push_callback_t receive_req_cb
                         , got_process_id_callback_t got_process_id_cb
                         , uint32_t proc_id = 0)
            : is_server(server), inited(false), timer(t), timer_id(-1), control_block(nullptr)
            , writing_thread_stopped(false), reading_thread_stopped(false)
            , last_recv_time(std::chrono::steady_clock::now())
            , disconnected_callback(std::move(disconnected_cb))
            , receive_req_callback(std::move(receive_req_cb))
            , got_process_id_callback(std::move(got_process_id_cb))
            , process_id(proc_id) {
            }

            ~connection_t() {
                stop();
            }

            bool start(int mem_fd) {
                if (inited) {
                    return true;
                }

                if (mem_fd == -1) {
                    return false;
                }

                if (!init_control_block(mem_fd)) {
                    return false;
                }

                writing_thread_stopped = false;
                writing_thread = std::thread([this]() {
                    write_proc();
                });

                reading_thread_stopped = false;
                reading_thread = std::thread([this]() {
                    read_proc();
                });

                timer_id = timer.start_timer([this](){on_timer();}, 1000, false);

                inited = true;
                return true;
            }

            void stop() {
                if (!inited) {
                    return;
                }
                inited = false;

                timer.stop_timer(timer_id);
                timer_id = -1;

                writing_thread_stopped = true;
                reading_thread_stopped = true;
                waiting_for_sending_packets_cond.notify_one();
                if (writing_thread.joinable()) {
                    writing_thread.join();
                }
                if (reading_thread.joinable()) {
                    reading_thread.join();
                }

                uninit_control_block();
            }

            void send_packet(std::unique_ptr<packet> pack) {
                send_packet(std::move(pack), nullptr, 0);
            }

            void send_packet(std::unique_ptr<packet> pack, recv_callback_t cb, uint32_t timeout_secs) {
                std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                waiting_for_sending_packets.emplace_back(std::move(pack), request{timeout_secs, cb, std::chrono::steady_clock::now()});
                waiting_for_sending_packets_cond.notify_one();
            }

            void cancel_sending(uint32_t cmd, uint32_t seq) {
                auto id = packet_id(cmd, seq);

                {
                    std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                    waiting_for_sending_packets.erase(std::remove_if(waiting_for_sending_packets.begin(), waiting_for_sending_packets.end(), [id](const auto& item) {
                        return packet_id(item.first) == id;
                    }), waiting_for_sending_packets.end());
                }

                {
                    std::unique_lock<std::mutex> lk(waiting_for_response_requests_mutex);
                    waiting_for_response_requests.erase(id);
                }
            }
        private:
            bool init_control_block(int mem_fd) {
                auto shared_mem = map_shared_memory(mem_fd);
                if (shared_mem == nullptr) {
                    return false;
                }

                control_block = static_cast<control_block_t *>(shared_mem);

                control_block->c_buf.reset((uint8_t*) control_block + sizeof(control_block_t), rw_buf_len);
                control_block->s_buf.reset((uint8_t*) control_block + sizeof(control_block_t) + rw_buf_len, rw_buf_len);


                if (is_server && !init_synchronization_objects()) {
                    return false;
                }

                return true;
            }

            void uninit_control_block() {
                if (is_server ) {
                    uninit_synchronization_objects();
                }

                unmap_shared_memory();
            }

            void* map_shared_memory(int mem_fd) const {
                if (is_server) {
                    if (ftruncate(mem_fd, mm_len) == -1) {
                        return nullptr;
                    }
                }

                void *shared_mem = mmap(nullptr, mm_len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
                if (shared_mem == MAP_FAILED) {
                    return nullptr;
                }

                return shared_mem;
            }

            void unmap_shared_memory() {
                munmap(control_block, mm_len);
            }

            bool init_synchronization_objects() {
                pthread_mutexattr_t mutex_attr;
                pthread_condattr_t cond_attr;

                bool mutex_attr_inited = false;
                bool cond_attr_inited = false;
                bool c_lock_inited = false;
                bool s_lock_inited = false;
                bool c_can_r_con_inited = false;
                bool c_can_w_con_inited = false;
                bool s_can_r_con_inited = false;
                bool s_can_w_con_inited = false;

                do {
                    if (pthread_mutexattr_init(&mutex_attr) != 0) {
                        break;
                    }
                    mutex_attr_inited = true;

                    if (pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST) != 0) {
                        break;
                    }

                    if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
                        break;
                    }

                    if (pthread_mutex_init(&control_block->c_lock, &mutex_attr) != 0) {
                        break;
                    }
                    c_lock_inited = true;

                    if (pthread_mutex_init(&control_block->s_lock, &mutex_attr) != 0) {
                        break;
                    }
                    s_lock_inited = true;

                    if (pthread_condattr_init(&cond_attr) != 0) {
                        break;
                    }
                    cond_attr_inited = true;

                    if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
                        break;
                    }

                    if (pthread_cond_init(&control_block->c_can_r_con, &cond_attr) != 0) {
                        break;
                    }
                    c_can_r_con_inited = true;

                    if (pthread_cond_init(&control_block->c_can_w_con, &cond_attr) != 0) {
                        break;
                    }
                    c_can_w_con_inited = true;

                    if (pthread_cond_init(&control_block->s_can_r_con, &cond_attr) != 0) {
                        break;
                    }
                    s_can_r_con_inited = true;

                    if (pthread_cond_init(&control_block->s_can_w_con, &cond_attr) != 0) {
                        break;
                    }
                    s_can_w_con_inited = true;
                } while (false);

                bool result = mutex_attr_inited&&cond_attr_inited&&c_lock_inited&&s_lock_inited&&c_can_r_con_inited&&c_can_w_con_inited&&s_can_r_con_inited&&s_can_w_con_inited;
                if (mutex_attr_inited) {
                    pthread_mutexattr_destroy(&mutex_attr);
                }
                if (cond_attr_inited) {
                    pthread_condattr_destroy(&cond_attr);
                }

                if (!result) {
                    if (c_lock_inited) {
                        pthread_mutex_destroy(&control_block->c_lock);
                    }
                    if (s_lock_inited) {
                        pthread_mutex_destroy(&control_block->s_lock);
                    }
                    if (c_can_r_con_inited) {
                        pthread_cond_destroy(&control_block->c_can_r_con);
                    }
                    if (c_can_w_con_inited) {
                        pthread_cond_destroy(&control_block->c_can_w_con);
                    }
                    if (s_can_r_con_inited) {
                        pthread_cond_destroy(&control_block->s_can_r_con);
                    }
                    if (s_can_w_con_inited) {
                        pthread_cond_destroy(&control_block->s_can_r_con);
                    }
                }

                return result;
            }

            void uninit_synchronization_objects() {
                pthread_cond_destroy(&control_block->c_can_r_con);
                pthread_cond_destroy(&control_block->c_can_w_con);
                pthread_cond_destroy(&control_block->s_can_r_con);
                pthread_cond_destroy(&control_block->s_can_w_con);

                pthread_mutex_destroy(&control_block->c_lock);
                pthread_mutex_destroy(&control_block->s_lock);
            }

            void write_proc() {
                auto lock = is_server ? &control_block->s_lock : &control_block->c_lock;
                auto cond = is_server ? &control_block->s_can_w_con : &control_block->c_can_w_con;
                auto& shared_buf = is_server ?  control_block->s_buf : control_block->c_buf;
                auto cond_to_notify = is_server ? &control_block->s_can_r_con : &control_block->c_can_r_con;

                while (!writing_thread_stopped) {
                    std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                    if (waiting_for_sending_packets.empty()) {
                        waiting_for_sending_packets_cond.wait(lk, [this]() {
                            return true;
                        });
                    }

                    if (writing_thread_stopped) {
                        break;
                    }

                    pthread_mutex_lock(lock);

                    while (!waiting_for_sending_packets.empty()) {
                        auto pack = std::move(waiting_for_sending_packets.front());
                        waiting_for_sending_packets.pop_front();

                        auto pack_buf = encode_packet(pack.first);

                        if (shared_buf.free_size() < pack_buf.size()) {
                            timespec ts{0,0};
                            timespec_get(&ts, TIME_UTC);
                            ts.tv_nsec += cond_wait_time_ns;
                            pthread_cond_timedwait(cond, lock, &ts);
                        }

                        if (writing_thread_stopped) {
                            break;
                        }

                        if (shared_buf.free_size() < pack_buf.size()) {
                            continue;
                        }

                        if (pack.second.should_wait_for_response()) {
                            auto pack_id = packet_id(pack.first);
                            std::unique_lock<std::mutex> lk_req(waiting_for_response_requests_mutex);
                            waiting_for_response_requests[pack_id] = pack.second;
                        }

                        memcpy(shared_buf.write_head(), pack_buf.data(), pack_buf.size());
                        shared_buf.commit(pack_buf.size());
                        pthread_cond_signal(cond_to_notify);
                    }

                    pthread_mutex_unlock(lock);
                }
            }

            void read_proc() {
                auto lock = is_server ? &control_block->c_lock : &control_block->s_lock;
                auto cond = is_server ? &control_block->c_can_r_con : &control_block->s_can_r_con;
                auto& shared_buf = is_server ?  control_block->c_buf : control_block->s_buf;
                auto cond_to_notify = is_server ? &control_block->c_can_w_con : &control_block->s_can_w_con;

                while (!reading_thread_stopped) {
                    pthread_mutex_lock(lock);

                    if (shared_buf.empty()) {
                        timespec ts{0,0};
                        timespec_get(&ts, TIME_UTC);
                        ts.tv_nsec += cond_wait_time_ns;
                        pthread_cond_timedwait(cond, lock, &ts);
                    }

                    if (!reading_thread_stopped && !shared_buf.empty()) {
                        process_packet(shared_buf);
                        pthread_cond_signal(cond_to_notify);
                    }

                    pthread_mutex_unlock(lock);
                }
            }

            void process_packet(linear_ringbuffer_t& recv_buffer) {
                for(;;) {
                    size_t consume_len = 0;
                    auto pack = decode_packet(recv_buffer.read_head(), recv_buffer.size(), consume_len);
                    recv_buffer.consume(consume_len);

                    if (!pack) {
                        return;
                    }

                    auto pack_process_id = pack->process_id();
                    if (pack_process_id == 0) {
                        continue;
                    }

                    if (process_id == 0) {
                        process_id = pack_process_id;
                        if (got_process_id_callback) {
                            got_process_id_callback(this, process_id);
                        }
                    } else if (process_id != pack_process_id) {
                        disconnected_callback(this, process_id);
                    }

                    last_recv_time = std::chrono::steady_clock::now();
                    process_id = pack->process_id();

                    if (pack->is_response()) {
                        uint64_t pack_id = packet_id(pack);

                        std::unique_lock<std::mutex> lk(waiting_for_response_requests_mutex);
                        auto it = waiting_for_response_requests.find(pack_id);
                        if (it != waiting_for_response_requests.end()) {
                            it->second.callback(std::move(pack));
                            waiting_for_response_requests.erase(it);
                        }
                    } else {
                        receive_req_callback(this, std::move(pack));
                    }
                }
            }

            void on_timer() {
                auto now = std::chrono::steady_clock::now();
                auto elapse = std::chrono::duration_cast<std::chrono::seconds>(now - last_recv_time).count();
                if (elapse > active_connection_lifetime_seconds) {
                    disconnected_callback(this, process_id);
                }

                std::unique_lock<std::mutex> lk(waiting_for_response_requests_mutex);
                for (auto it = waiting_for_response_requests.begin(); it != waiting_for_response_requests.end(); ) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.begin_time).count() < it->second.timeout_secs) {
                        ++it;
                        break;
                    }

                    it->second.callback(nullptr);
                    it = waiting_for_response_requests.erase(it);
                }
            }
        private:
            bool is_server;
            bool inited;

            timer_mgr_t& timer;
            int timer_id;

            control_block_t *control_block;

            packet_list_t waiting_for_sending_packets;
            std::mutex waiting_for_sending_packets_mutex;
            std::condition_variable waiting_for_sending_packets_cond;

            map_packid_2_callback_t waiting_for_response_requests;
            std::mutex waiting_for_response_requests_mutex;

            std::thread writing_thread;
            volatile std::atomic<bool> writing_thread_stopped;

            std::thread reading_thread;
            volatile std::atomic<bool> reading_thread_stopped;

            std::chrono::steady_clock::time_point last_recv_time;
            disconnected_callback_t disconnected_callback;
            receive_push_callback_t receive_req_callback;
            got_process_id_callback_t got_process_id_callback;

            uint32_t process_id;
        };

    }