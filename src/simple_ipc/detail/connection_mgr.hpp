#pragma once
#include <mutex>
#include <unordered_map>
#include <list>
#include <atomic>

#include "simple_ipc/detail/connection.hpp"
#include "simple_ipc/detail/timer.hpp"

using namespace std::placeholders;

namespace simple::ipc {

        class connection_mgr_t {
        public:
            using map_process_2_connection_t = std::unordered_map<uint32_t , std::unique_ptr<connection_t>>;
            using connection_list_t = std::list<std::unique_ptr<connection_t>>;
        public:
            connection_mgr_t() {
            }

            ~connection_mgr_t() {
            }

            bool new_connection(int fd) {
                auto connection = std::make_unique<connection_t>(true, timer
                        , std::bind(&connection_mgr_t::on_disconnected, this, _1, _2)
                        , std::bind(&connection_mgr_t::on_recv_req, this, _1, _2)
                        , std::bind(&connection_mgr_t::on_got_process_id, this, _1, _2));

                if (!connection->start(fd)) {
                    close(fd);
                    return false;
                }

                {
                    std::unique_lock<std::mutex> lk(temp_connections_mutex);
                    temp_connections.push_back(std::move(connection));
                }

                return true;
            }

            void remove_connection(uint32_t process_id) {
                std::unique_lock<std::mutex> lk(connections_mutex);
                connections.erase(process_id);
            }

            bool send_packet(std::unique_ptr<packet> pack) {
                return send_packet(std::move(pack), nullptr, 0);
            }

            bool send_packet(std::unique_ptr<packet> pack, recv_callback_t cb, uint32_t timeout_secs) {
                uint32_t process_id = pack->process_id();
                std::unique_lock<std::mutex> lk(connections_mutex);
                auto it = connections.find(process_id);
                if (it == connections.end()) {
                    return false;
                }

                it->second->send_packet(std::move(pack), cb, timeout_secs);
                return true;
            }

            bool cancel_sending(uint32_t process_id, uint32_t cmd, uint32_t seq) {
                std::unique_lock<std::mutex> lk(connections_mutex);
                auto it = connections.find(process_id);
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
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                req_processors.erase(cmd);
            }
        private:
            void on_got_process_id(connection_t* conn, uint32_t process_id) {
                std::unique_lock<std::mutex> lk(temp_connections_mutex);
                for (auto it = temp_connections.begin(); it != temp_connections.end(); ++it) {
                    if ((*it).get() == conn) {
                        connections[process_id] = std::move(*it);
                        temp_connections.erase(it);
                        break;
                    }
                }
            }

            void on_disconnected(connection_t* conn, uint32_t process_id) {
                {
                    std::unique_lock<std::mutex> lk(connections_mutex);
                    connections.erase(process_id);
                }

                {
                    std::unique_lock<std::mutex> lk(temp_connections_mutex);
                    std::remove_if(temp_connections.begin(), temp_connections.end(), [conn](const std::unique_ptr<connection_t>& it) {
                        return it.get() == conn;
                    });
                }
            }

            void on_recv_req(connection_t* conn, std::unique_ptr<packet> pack) {
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                auto it = req_processors.find(pack->cmd());
                if (it != req_processors.end()) {
                    it->second(std::move(pack));
                }
            }


        private:
            timer_mgr_t timer;

            connection_list_t temp_connections;
            std::mutex temp_connections_mutex;

            map_process_2_connection_t connections;
            std::mutex connections_mutex;

            map_cmd_2_callback_t req_processors;
            std::mutex req_processors_mutex;
        };

    }