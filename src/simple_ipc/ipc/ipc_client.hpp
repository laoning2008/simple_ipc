#pragma once

#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <memory>
#include <utility>

#include "simple_ipc/detail/connector.hpp"
#include "simple_ipc/detail/connection.hpp"
#include "simple_ipc/detail/timer.hpp"

using namespace std::placeholders;

namespace simple::ipc {
        class ipc_client_t {
            constexpr static const int state_connected = 1;
            constexpr static const int state_disconnected = 2;
            constexpr static const int state_connecting = 3;

            constexpr static const uint32_t heartbeat_cmd = 0;
            constexpr static const uint64_t heartbeat_interval_ms = 2 * 1000;
            constexpr static const uint64_t reconnect_check_interval_ms = 1 * 1000;
        public:
            using recv_callback_t = std::function<void(std::unique_ptr<packet> pack)>;
            using map_cmd_2_callback_t = std::unordered_map<uint32_t, recv_callback_t >;
        public:
            explicit ipc_client_t(std::string server_name)
            : connection_state(state_connecting)
            , connector(std::move(server_name), [this](uint32_t connection_id, int fd){ on_connected(connection_id, fd);})
            , connection(false, timer, [this](connection_t* conn, uint32_t id){ on_disconnected(conn, id);}
                            , [this](connection_t* conn, std::unique_ptr<packet> pack){on_receive_req(conn, std::move(pack));}) {
                timer.start_timer([this](){on_heartbeat_timer();}, heartbeat_interval_ms, false);
                timer.start_timer([this](){on_reconnect_timer();}, reconnect_check_interval_ms, false);
            }

            void send_packet(std::unique_ptr<packet> pack) {
                connection.send_packet(std::move(pack));
            }

            void send_packet(std::unique_ptr<packet> pack, recv_callback_t cb, uint32_t timeout_secs) {
                connection.send_packet(std::move(pack), std::move(cb), timeout_secs);
            }

            void cancel_sending(uint32_t cmd, uint32_t seq) {
                connection.cancel_sending(cmd, seq);
            }

            void register_request_processor(uint32_t cmd, recv_callback_t cb) {
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                req_processors[cmd] = std::move(cb);
            }

            void unregister_request_processor(uint32_t cmd) {
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                req_processors.erase(cmd);
            }
        private:
            void on_connected(uint32_t connection_id, int fd) {
//                std::cout << "on_connected begin, fd = " << fd << std::endl;
                connection.stop();

                if (fd != -1 && connection.start(connection_id, fd)) {
                    connection_state = state_connected;
                } else {
                    connection_state = state_disconnected;
                }

//                std::cout << "on_connected end" << std::endl;
            }

            void on_disconnected(connection_t* conn, uint32_t id) {
                std::cout << "on_disconnected begin" << std::endl;
                connection.stop();
                connection_state = state_disconnected;
                std::cout << "on_disconnected end" << std::endl;
            }

            void on_receive_req(connection_t* conn,  std::unique_ptr<packet> pack) {
                std::unique_lock<std::mutex> lk(req_processors_mutex);
                auto it = req_processors.find(pack->cmd());
                if (it != req_processors.end()) {
                    it->second(std::move(pack));
                }
            }

            void on_heartbeat_timer() {
                auto heartbeat_pack = build_req_packet(0, heartbeat_cmd, nullptr, 0);
                connection.send_packet(std::move(heartbeat_pack));
            }

            void on_reconnect_timer() {
                if (connection_state != state_disconnected) {
                    return;
                }

                connection_state = state_connecting;
                connector.reconnect();
            }
        private:
            std::atomic<int> connection_state;

            timer_mgr_t timer;

            connector_t connector;
            connection_t connection;

            map_cmd_2_callback_t req_processors;
            std::mutex req_processors_mutex;
        };

}