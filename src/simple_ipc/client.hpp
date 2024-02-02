#pragma once

#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <memory>

#include "connector.hpp"
#include "connection.hpp"

using namespace std::placeholders;

namespace simple { namespace ipc {
        class client_t {
        public:
            using recv_callback_t = std::function<void(std::unique_ptr<packet> pack)>;
            using map_cmd_2_callback_t = std::unordered_map<uint32_t, recv_callback_t >;
        public:
            client_t(std::string server_name)
            : connector(server_name, std::bind(&client_t::on_connected, this, _1))
            , connection(false, std::bind(&client_t::on_disconnected, this, _1, _2)
                    ,std::bind(&client_t::on_recv_push, this, _1, _2), nullptr, getpid()) {
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
        private:
            connector_t connector;
            connection_t connection;

            map_cmd_2_callback_t push_callbacks;
            std::mutex push_callbacks_mutex;
        };

}}