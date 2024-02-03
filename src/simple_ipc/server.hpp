#pragma once
#include "simple_ipc/detail/connection_mgr.hpp"
#include "simple_ipc/detail/listener.hpp"
using namespace std::placeholders;

namespace simple::ipc {
        class server_t {
        public:
            server_t(std::string server_name)
            : listener(std::move(server_name), std::bind(&server_t::on_new_connection, this, _1)) {

            }

            bool send_packet(std::unique_ptr<packet> pack) {
                return conn_mgr.send_packet(std::move(pack));
            }

            bool send_packet(std::unique_ptr<packet> pack, recv_callback_t cb) {
                return conn_mgr.send_packet(std::move(pack), cb);
            }

            bool cancel_sending(uint32_t process_id, uint32_t cmd, uint32_t seq) {
                return conn_mgr.cancel_sending(process_id, cmd, seq);
            }

            void register_request_processor(uint32_t cmd, recv_callback_t cb) {
                conn_mgr.register_request_processor(cmd, cb);
            }

            void unregister_request_processor(uint32_t cmd) {
                conn_mgr.unregister_request_processor(cmd);
            }
        private:
            bool on_new_connection(int fd) {
                return conn_mgr.new_connection(fd);
            }
        private:
            listener_t listener;
            connection_mgr_t conn_mgr;
        };

}