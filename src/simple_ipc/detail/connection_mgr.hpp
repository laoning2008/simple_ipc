#pragma once
#include <mutex>
#include <unordered_map>
#include <list>
#include <atomic>

#include "simple_ipc/detail/connection.hpp"
#include "simple_ipc/detail/timer.hpp"
#include <shared_mutex>

using namespace std::placeholders;

namespace simple::ipc {
        class connection_mgr_t {
        public:
            using map_process_2_connection_t = std::unordered_map<uint32_t , std::unique_ptr<connection_t>>;
            using map_cmd_2_callback_t = std::unordered_map<uint32_t, recv_callback_t >;
        public:
            bool new_connection(uint32_t connection_id, int fd) {
                auto connection = std::make_unique<connection_t>(true, timer
                        , [this](connection_t* conn, uint32_t connection_id){ on_disconnected(conn, connection_id);}
                        , [this](connection_t* conn, std::unique_ptr<packet> pack){on_receive_req(conn, std::move(pack));});

                if (!connection->start(connection_id, fd)) {
                    close(fd);
                    return false;
                }

                std::unique_lock lk_conns(connections_mutex);
                connections[connection_id] = std::move(connection);

                return true;
            }

            void remove_connection(uint32_t process_id) {
                std::unique_lock lk(connections_mutex);
                connections.erase(process_id);
            }

            bool send_packet(std::unique_ptr<packet> pack) {
                return send_packet(std::move(pack), nullptr, 0);
            }

            bool send_packet(std::unique_ptr<packet> pack, recv_callback_t cb, uint32_t timeout_secs) {
                uint32_t connection_id = pack->connection_id();
                std::shared_lock lk(connections_mutex);
                auto it = connections.find(connection_id);
                if (it == connections.end()) {
                    return false;
                }

                it->second->send_packet(std::move(pack), cb, timeout_secs);
                return true;
            }

            bool cancel_sending(uint32_t connection_id, uint32_t cmd, uint32_t seq) {
                std::shared_lock lk(connections_mutex);
                auto it = connections.find(connection_id);
                if (it == connections.end()) {
                    return false;
                }

                it->second->cancel_sending(cmd, seq);
                return true;
            }

            void register_request_processor(uint32_t cmd, recv_callback_t cb) {
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                req_processors[cmd] = cb;
            }

            void unregister_request_processor(uint32_t cmd) {
                std::unique_lock lk(req_processors_mutex);
                req_processors.erase(cmd);
            }
        private:
            void on_disconnected(connection_t* conn, uint32_t connection_id) {
                std::unique_lock lk(connections_mutex);
                connections.erase(connection_id);
            }

            void on_receive_req(connection_t* conn, std::unique_ptr<packet> pack) {
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                auto it = req_processors.find(pack->cmd());
                if (it != req_processors.end()) {
                    it->second(std::move(pack));
                }
            }
        private:
            timer_mgr_t timer;

            map_process_2_connection_t connections;
            std::shared_mutex connections_mutex;

            map_cmd_2_callback_t req_processors;
            std::mutex req_processors_mutex;
        };

    }