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
#include "ringbuffer.hpp"
#include "packet.hpp"
#include "defs.hpp"
#include "timer.hpp"

//todo
//1. need to create and destroy pthread shared objects in both sides?
//2.
namespace simple { namespace ipc {
        struct control_block_t {
            pthread_mutex_t c_lock;
            pthread_mutex_t s_lock;

            pthread_cond_t c_can_r_con;
            pthread_cond_t c_can_w_con;
            pthread_cond_t s_can_r_con;
            pthread_cond_t s_can_w_con;

            linear_ringbuffer_t c_buf;
            linear_ringbuffer_t s_buf;
        };

        constexpr static const size_t rw_buf_len = 1024 * 1024;
        constexpr static const uint32_t active_connection_lifetime_seconds = 60;

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
            using map_packid_2_callback_t = std::unordered_map<uint64_t, recv_callback_t >;
            using packet_list_t = std::list<std::pair<std::unique_ptr<packet>, recv_callback_t>>;
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
            , disconnected_callback(disconnected_cb)
            , receive_req_callback(receive_req_cb)
            , got_process_id_callback(got_process_id_cb)
            , process_id(proc_id) {
            }

            ~connection_t() {
                stop();
            }

            bool set_fd(int mem_fd) {
                stop();
                return start(mem_fd);
            }

            void send_packet(std::unique_ptr<packet> pack) {
                send_packet(std::move(pack), nullptr);
            }

            void send_packet(std::unique_ptr<packet> pack, recv_callback_t cb) {
                std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                waiting_for_sending_packets.emplace_back(std::move(pack), cb);
                waiting_for_sending_packets_cond.notify_one();
            }

            void cancel_sending(uint32_t cmd, uint32_t seq) {
                auto id = packet_id(cmd, seq);

                {
                    std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                    std::remove_if(waiting_for_sending_packets.begin(), waiting_for_sending_packets.end(), [id](const auto& item) {
                        return packet_id(item.first) == id;
                    });
                }

                {
                    std::unique_lock<std::mutex> lk(waiting_for_response_requests_mutex);
                    waiting_for_response_requests.erase(id);
                }
            }
        private:
            bool start(int mem_fd) {
                if (mem_fd == -1) {
                    return true;
                }

                auto shared_mem = map_shared_memory(mem_fd);
                if (!init_control_block(shared_mem)) {
                    return false;
                }

                writing_thread = std::thread([this]() {
                    write_proc();
                });

                reading_thread = std::thread([this]() {
                    read_proc();
                });

                timer.start_timer(std::bind(&connection_t::on_timer, this), 1000, false);

                inited = true;
                return true;
            }

            void stop() {
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

                if (inited) {
                    destroy_synchronization_objects();
                    munmap(control_block, mm_len);
                    inited = false;
                }
            }

            void *map_shared_memory(int mem_fd) {
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

            bool init_control_block(void *shared_mem) {
                if (shared_mem == nullptr) {
                    return false;
                }

                control_block = static_cast<control_block_t *>(shared_mem);

                if (is_server) {
                    if (create_synchronization_objects()) {
                        return false;
                    }

                    control_block->c_buf = linear_ringbuffer_t{(uint8_t *) control_block + sizeof(control_block_t),
                                                               rw_buf_len};
                    control_block->s_buf = linear_ringbuffer_t{
                            (uint8_t *) control_block + sizeof(control_block_t) + rw_buf_len, rw_buf_len};
                }

                return true;
            }

            bool create_synchronization_objects() {
                pthread_mutexattr_t mutex_attr;
                if (pthread_mutexattr_init(&mutex_attr) != 0) {
                    return false;
                }

                if (pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST) != 0) {
                    return false;
                }

                if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
                    return false;
                }

                if (pthread_mutex_init(&control_block->c_lock, &mutex_attr) != 0) {
                    return false;
                }

                if (pthread_mutex_init(&control_block->s_lock, &mutex_attr) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    return false;
                }

                pthread_condattr_t cond_attr;
                if (pthread_condattr_init(&cond_attr) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    pthread_mutex_destroy(&control_block->s_lock);
                    return false;
                }
                if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    pthread_mutex_destroy(&control_block->s_lock);
                    return false;
                }

                if (pthread_cond_init(&control_block->c_can_r_con, &cond_attr) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    pthread_mutex_destroy(&control_block->s_lock);
                    return false;
                }

                if (pthread_cond_init(&control_block->c_can_w_con, &cond_attr) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    pthread_mutex_destroy(&control_block->s_lock);
                    pthread_cond_destroy(&control_block->c_can_r_con);
                    return false;
                }

                if (pthread_cond_init(&control_block->s_can_r_con, &cond_attr) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    pthread_mutex_destroy(&control_block->s_lock);
                    pthread_cond_destroy(&control_block->c_can_r_con);
                    pthread_cond_destroy(&control_block->c_can_w_con);
                    return false;
                }

                if (pthread_cond_init(&control_block->s_can_w_con, &cond_attr) != 0) {
                    pthread_mutex_destroy(&control_block->c_lock);
                    pthread_mutex_destroy(&control_block->s_lock);
                    pthread_cond_destroy(&control_block->c_can_r_con);
                    pthread_cond_destroy(&control_block->c_can_w_con);
                    pthread_cond_destroy(&control_block->s_can_r_con);
                    return false;
                }

                return true;
            }

            void destroy_synchronization_objects() {
                pthread_mutex_destroy(&control_block->c_lock);
                pthread_mutex_destroy(&control_block->s_lock);
                pthread_cond_destroy(&control_block->c_can_r_con);
                pthread_cond_destroy(&control_block->c_can_w_con);
                pthread_cond_destroy(&control_block->s_can_r_con);
                pthread_cond_destroy(&control_block->s_can_w_con);
            }

            void write_proc() {
                while (!writing_thread_stopped) {
                    std::unique_lock<std::mutex> lk(waiting_for_sending_packets_mutex);
                    if (waiting_for_sending_packets.empty()) {
                        waiting_for_sending_packets_cond.wait(lk, [this]() {
                            return writing_thread_stopped || !waiting_for_sending_packets.empty();
                        });
                    }

                    if (writing_thread_stopped) {
                        break;
                    }

                    auto lock = is_server ? &control_block->s_lock : &control_block->c_lock;
                    auto cond = is_server ? &control_block->s_can_r_con : &control_block->c_can_r_con;
                    auto& shared_buf = is_server ?  control_block->s_buf : control_block->c_buf;

                    pthread_mutex_lock(lock);

                    for (auto& pack : waiting_for_sending_packets) {
                        auto pack_buf = encode_packet(pack.first);

                        if (shared_buf.free_size() < pack_buf.size()) {
                            pthread_cond_wait(cond, lock);
                        }

                        if (writing_thread_stopped) {
                            break;
                        }

                        if (shared_buf.free_size() < pack_buf.size()) {
                            continue;
                        }

                        memcpy(shared_buf.write_head(), pack_buf.data(), pack_buf.size());
                        shared_buf.commit(pack_buf.size());
                    }

                    pthread_mutex_unlock(lock);
                }
            }

            void read_proc() {
                while (!reading_thread_stopped) {
                    auto lock = is_server ? &control_block->c_lock : &control_block->s_lock;
                    auto cond = is_server ? &control_block->c_can_r_con : &control_block->s_can_r_con;
                    auto& shared_buf = is_server ?  control_block->c_buf : control_block->s_buf;

                    pthread_mutex_lock(lock);
                    if (shared_buf.empty()) {
                        pthread_cond_wait(cond, lock);
                    }

                    if (!reading_thread_stopped) {
                        process_packet(shared_buf);
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
                            it->second(std::move(pack));
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

    }}