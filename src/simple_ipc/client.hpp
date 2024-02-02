#pragma once

#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <memory>

#include "connector.hpp"
#include "connection.hpp"
#include "timer.hpp"

using namespace std::placeholders;

namespace simple { namespace ipc {
        class client_t {
            constexpr static uint32_t heartbeat_cmd = 0;
        public:
            using recv_callback_t = std::function<void(std::unique_ptr<packet> pack)>;
            using map_cmd_2_callback_t = std::unordered_map<uint32_t, recv_callback_t >;
        public:
            client_t(std::string server_name)
            : process_id(getpid())
            , connector(server_name, std::bind(&client_t::on_connected, this, _1))
            , connection(false, timer, std::bind(&client_t::on_disconnected, this, _1, _2)
                    ,std::bind(&client_t::on_recv_push, this, _1, _2), nullptr, process_id) {
            }

            ~client_t() {

            }

            void send_packet(std::unique_ptr<packet> pack) {
                connection.send_packet(std::move(pack));
            }

            void send_packet(std::unique_ptr<packet> pack, recv_callback_t cb) {
                connection.send_packet(std::move(pack), cb);
            }

            void cancel_sending(uint32_t cmd, uint32_t seq) {
                connection.cancel_sending(cmd, seq);
            }

            void register_push_receiver(uint32_t cmd, recv_callback_t cb) {
                std::unique_lock<std::mutex> lk(push_callbacks_mutex);
                push_callbacks[cmd] = cb;
            }

            void unregister_push_receiver(uint32_t cmd) {
                std::unique_lock<std::mutex> lk(push_callbacks_mutex);
                push_callbacks.erase(cmd);
            }
        private:
            void on_connected(int fd) {
                if (!connection.set_fd(fd)) {
                    connector.start_connect();
                }
            }

            void on_disconnected(connection_t* conn, uint32_t process_id) {
                connector.start_connect();
            }

            void on_recv_push(connection_t* conn,  std::unique_ptr<packet> pack) {
                std::unique_lock<std::mutex> lk(push_callbacks_mutex);
                auto it = push_callbacks.find(pack->cmd());
                if (it != push_callbacks.end()) {
                    it->second(std::move(pack));
                }
            }

            void on_heartbeat_timer() {
                auto heartbeat_pack = build_req_packet(process_id, heartbeat_cmd, nullptr, 0);
                connection.send_packet(std::move(heartbeat_pack));
            }
        private:
            uint32_t process_id;
            connector_t connector;
            connection_t connection;
            timer_mgr_t timer;

            map_cmd_2_callback_t push_callbacks;
            std::mutex push_callbacks_mutex;
        };

}}