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
            pthread_cond_t c_should_stop_con{};
            pthread_mutex_t c_should_stop_lock{};
            pthread_mutex_t c_lifetime_lock{};

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
        constexpr static const uint32_t active_connection_lifetime_seconds = 6;
        constexpr static const uint64_t timer_interval_ms = 1 * 1000;

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
            struct request_t {
                uint32_t timeout_secs{};
                recv_callback_t callback;
                std::chrono::steady_clock::time_point begin_time;

                [[nodiscard]] bool should_wait_for_response() const {
                    return callback != nullptr && timeout_secs != 0;
                }
            };

            using map_packid_2_callback_t = std::unordered_map<uint64_t, request_t >;
            using packet_list_t = std::list<std::pair<std::unique_ptr<packet>, request_t>>;
        public:
            using disconnected_callback_t = std::function<void(connection_t* conn, uint32_t process_id)>;
            using receive_push_callback_t = std::function<void(connection_t* conn, std::unique_ptr<packet>)>;
        public:
            connection_t(bool server, timer_mgr_t& t
                         , disconnected_callback_t disconnected_cb
                         , receive_push_callback_t receive_req_cb)
            : is_server(server), inited(false), timer(t), timer_id(-1), control_block(nullptr)
            , writing_thread_stopped(false), reading_thread_stopped(false)
            , last_recv_time(std::chrono::steady_clock::now())
            , disconnected_callback(std::move(disconnected_cb))
            , receive_req_callback(std::move(receive_req_cb))
            , connection_id(0) {
            }

            ~connection_t() {
                stop();
            }

            bool start(uint32_t conn_id, int mem_fd) {
                if (inited) {
                    return true;
                }

                if (mem_fd == -1) {
                    return false;
                }

                void* shared_mem = map_shared_memory(mem_fd);
                if (!shared_mem) {
                    return false;
                }
                control_block = static_cast<control_block_t *>(shared_mem);

                if (is_server) {
                    if (!init_control_block()) {
                        return false;
                    }
                } else {
                    //only unlock when stop or process kill, server can check if client connection is alive
                    c_lifetime_thread = std::thread([this]() {
                        c_lifetime_proc();
                    });
                }

                connection_id = conn_id;
                writing_thread_stopped = false;
                writing_thread = std::thread([this]() {
                    write_proc();
                });

                reading_thread_stopped = false;
                reading_thread = std::thread([this]() {
                    read_proc();
                });

                timer_id = timer.start_timer([this](){on_timer();}, timer_interval_ms, false);

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

                auto cond_r_notify = is_server ? &control_block->c_can_r_con : &control_block->s_can_r_con;
                auto cond_w_notify = is_server ? &control_block->s_can_w_con : &control_block->c_can_w_con;

                pthread_cond_signal(cond_r_notify);
                pthread_cond_signal(cond_w_notify);

                if (writing_thread.joinable()) {
                    writing_thread.join();
                }
                if (reading_thread.joinable()) {
                    reading_thread.join();
                }

                if (is_server) {
                    uninit_control_block();
                } else {
                    pthread_cond_signal(&control_block->c_should_stop_con);
                    if (c_lifetime_thread.joinable()) {
                        c_lifetime_thread.join();
                    }
                    //pthread_mutex_unlock(&control_block->c_lifetime_lock);
                }

                unmap_shared_memory();
            }

            void send_packet(std::unique_ptr<packet> pack) {
                send_packet(std::move(pack), nullptr, 0);
            }

            void send_packet(std::unique_ptr<packet> pack, recv_callback_t cb, uint32_t timeout_secs) {
                std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                waiting_for_sending_packets.emplace_back(std::move(pack), request_t{timeout_secs, cb, std::chrono::steady_clock::now()});
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

            bool init_control_block() {
                if (!init_synchronization_objects()) {
                    return false;
                }

                control_block->c_buf = linear_ringbuffer_t{rw_buf_len};
                control_block->s_buf = linear_ringbuffer_t{rw_buf_len};

                return true;
            }

            void uninit_control_block() {
                control_block->c_buf.clear();
                control_block->s_buf.clear();

                uninit_synchronization_objects();
            }

            bool init_synchronization_objects() {
                pthread_mutexattr_t mutex_attr;
                pthread_condattr_t cond_attr;

                bool mutex_attr_inited = false;
                bool cond_attr_inited = false;
                bool c_lifetime_lock_inited = false;
                bool c_should_stop_cond_inited = false;
                bool c_should_stop_lock_inited = false;
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

                    if (pthread_condattr_init(&cond_attr) != 0) {
                        break;
                    }
                    cond_attr_inited = true;

                    if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
                        break;
                    }

                    if (pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST) != 0) {
                        break;
                    }

                    if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
                        break;
                    }

                    if (pthread_mutex_init(&control_block->c_lifetime_lock, &mutex_attr) != 0) {
                        break;
                    }
                    c_lifetime_lock_inited = true;

                    if (pthread_mutex_init(&control_block->c_should_stop_lock, &mutex_attr) != 0) {
                        break;
                    }
                    c_should_stop_lock_inited = true;

                    if (pthread_cond_init(&control_block->c_should_stop_con, &cond_attr) != 0) {
                        break;
                    }
                    c_should_stop_cond_inited = true;


                    if (pthread_mutex_init(&control_block->c_lock, &mutex_attr) != 0) {
                        break;
                    }
                    c_lock_inited = true;

                    if (pthread_mutex_init(&control_block->s_lock, &mutex_attr) != 0) {
                        break;
                    }
                    s_lock_inited = true;

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

                    if (c_lifetime_lock_inited) {
                        pthread_mutex_destroy(&control_block->c_lifetime_lock);
                    }

                    if (c_should_stop_lock_inited) {
                        pthread_mutex_destroy(&control_block->c_should_stop_lock);
                    }

                    if (c_should_stop_cond_inited) {
                        pthread_cond_destroy(&control_block->c_should_stop_con);
                    }

                    if (mutex_attr_inited) {
                        pthread_mutexattr_destroy(&mutex_attr);
                    }

                    if (cond_attr_inited) {
                        pthread_condattr_destroy(&cond_attr);
                    }
                }

                return result;
            }

            void uninit_synchronization_objects() {
                pthread_cond_broadcast(&control_block->c_can_r_con);
                pthread_cond_broadcast(&control_block->c_can_w_con);
                pthread_cond_broadcast(&control_block->s_can_r_con);
                pthread_cond_broadcast(&control_block->s_can_w_con);

                pthread_cond_broadcast(&control_block->c_should_stop_con);
                wait_for_client_finished_uninit_connection();

                //https://stackoverflow.com/questions/20439404/pthread-conditions-and-process-termination
                control_block->c_can_r_con.__data.__wrefs = 0;
                control_block->c_can_w_con.__data.__wrefs = 0;
                control_block->s_can_r_con.__data.__wrefs = 0;
                control_block->s_can_w_con.__data.__wrefs = 0;
                pthread_cond_destroy(&control_block->c_can_r_con);
                pthread_cond_destroy(&control_block->c_can_w_con);
                pthread_cond_destroy(&control_block->s_can_r_con);
                pthread_cond_destroy(&control_block->s_can_w_con);

                pthread_mutex_destroy(&control_block->c_lock);
                pthread_mutex_destroy(&control_block->s_lock);

                control_block->c_should_stop_con.__data.__wrefs = 0;
                pthread_cond_destroy(&control_block->c_should_stop_con);
                pthread_mutex_destroy(&control_block->c_should_stop_lock);
                pthread_mutex_destroy(&control_block->c_lifetime_lock);
            }

            void wait_for_client_finished_uninit_connection() {
                std::cout << "s lock" << std::endl;
                pthread_mutex_lock(&control_block->c_lifetime_lock);
            }

            void write_proc() {
                auto lock = is_server ? &control_block->s_lock : &control_block->c_lock;
                auto cond = is_server ? &control_block->s_can_w_con : &control_block->c_can_w_con;
                auto& shared_buf = is_server ?  control_block->s_buf : control_block->c_buf;
                auto cond_to_notify = is_server ? &control_block->s_can_r_con : &control_block->c_can_r_con;

                auto w_buf = is_server ? (uint8_t*) control_block + sizeof(control_block_t) + rw_buf_len : (uint8_t*) control_block + sizeof(control_block_t);

                while (!writing_thread_stopped) {
                    ibuffer pack_buf;
                    request_t req;
                    uint64_t pack_id = 0;
                    {
                        std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                        while (!writing_thread_stopped && waiting_for_sending_packets.empty()) {
                            waiting_for_sending_packets_cond.wait(lk);
                        }

                        if (writing_thread_stopped) {
                            break;
                        }

                        auto pack = std::move(waiting_for_sending_packets.front());
                        waiting_for_sending_packets.pop_front();

                        if (pack.first->connection_id() == 0) {
                            pack.first->set_connection_id(connection_id);
                        }

                        pack_buf = encode_packet(pack.first);
                        pack_id = packet_id(pack.first);
                        req = pack.second;
                    }

                    if (pthread_mutex_lock(lock) != 0) {
                        break;
                    }

                    while (!writing_thread_stopped && shared_buf.free_size() < pack_buf.size()) {
                        pthread_cond_wait(cond, lock);
                    }

                    if (writing_thread_stopped) {
                        pthread_mutex_unlock(lock);
                        break;
                    }

                    if (req.should_wait_for_response()) {
                        std::unique_lock<std::mutex> lk_req(waiting_for_response_requests_mutex);
                        waiting_for_response_requests[pack_id] = req;
                    }

                    memcpy(shared_buf.write_head(w_buf), pack_buf.data(), pack_buf.size());
                    shared_buf.commit(pack_buf.size());
                    pthread_cond_signal(cond_to_notify);

                    if (pthread_mutex_unlock(lock) != 0) {
                        break;
                    }
                }
            }

            void read_proc() {
                auto lock = is_server ? &control_block->c_lock : &control_block->s_lock;
                auto cond = is_server ? &control_block->c_can_r_con : &control_block->s_can_r_con;
                auto& shared_buf = is_server ?  control_block->c_buf : control_block->s_buf;
                auto cond_to_notify = is_server ? &control_block->c_can_w_con : &control_block->s_can_w_con;
                auto r_buf = is_server ? (uint8_t*) control_block + sizeof(control_block_t) : (uint8_t*) control_block + sizeof(control_block_t) + rw_buf_len;

                while (!reading_thread_stopped) {
                    if (pthread_mutex_lock(lock) != 0) {
                        break;
                    }

                    if (shared_buf.empty()) {
                        pthread_cond_wait(cond, lock);
                    }

                    if (!reading_thread_stopped && !shared_buf.empty()) {
                        process_packet(r_buf, shared_buf);
                        pthread_cond_signal(cond_to_notify);
                    }

                    if (pthread_mutex_unlock(lock) != 0) {
                        break;
                    }
                }
            }

            void c_lifetime_proc() {
                pthread_mutex_lock(&control_block->c_lifetime_lock);

                do {
                    if (pthread_mutex_lock(&control_block->c_should_stop_lock) != 0) {
                        return;
                    }
                    pthread_cond_wait(&control_block->c_should_stop_con, &control_block->c_should_stop_lock);
                    pthread_mutex_unlock(&control_block->c_should_stop_lock);
                } while (false);

                std::cout << "c unlock" << std::endl;
                pthread_mutex_unlock(&control_block->c_lifetime_lock);
                disconnected_callback(this, connection_id);
            }

            void process_packet(uint8_t* buf, linear_ringbuffer_t& recv_buffer) {
                for(;;) {
                    size_t consume_len = 0;
                    auto pack = decode_packet(recv_buffer.read_head(buf), recv_buffer.size(), consume_len);
                    recv_buffer.consume(consume_len);

                    if (!pack || pack->connection_id() != connection_id) {
                        return;
                    }

                    last_recv_time = std::chrono::steady_clock::now();

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
                    disconnected_callback(this, connection_id);
                    return;
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

            std::thread c_lifetime_thread;

            std::chrono::steady_clock::time_point last_recv_time;
            disconnected_callback_t disconnected_callback;
            receive_push_callback_t receive_req_callback;

            uint32_t connection_id;
        };

    }