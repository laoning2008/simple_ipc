#pragma once
#include <utility>

#include "simple_ipc/detail/connection_mgr.hpp"
#include "simple_ipc/detail/listener.hpp"
using namespace std::placeholders;

namespace simple::ipc {
        class server_t {
        public:
            explicit server_t(std::string server_name)
            : listener(std::move(server_name), [this](uint64_t connection_id, int fd){return on_new_connection(connection_id, fd);}) {
            }

            bool send_packet(std::unique_ptr<packet> pack) {
                return conn_mgr.send_packet(std::move(pack));
            }

            bool send_packet(std::unique_ptr<packet> pack, recv_callback_t cb, uint32_t timeout_secs) {
                return conn_mgr.send_packet(std::move(pack), std::move(cb), timeout_secs);
            }

            bool cancel_sending(uint32_t process_id, uint32_t cmd, uint32_t seq) {
                return conn_mgr.cancel_sending(process_id, cmd, seq);
            }

            void register_request_processor(uint32_t cmd, recv_callback_t cb) {
                conn_mgr.register_request_processor(cmd, std::move(cb));
            }

            void unregister_request_processor(uint32_t cmd) {
                conn_mgr.unregister_request_processor(cmd);
            }
        private:
            bool on_new_connection(uint64_t connection_id, int fd) {
                return conn_mgr.new_connection(connection_id, fd);
            }
        private:
            listener_t listener;
            connection_mgr_t conn_mgr;
        };

}