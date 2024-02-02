#pragma once
#include "connection_mgr.hpp"

namespace simple { namespace ipc {
        class server_t {
        public:
            server_t() {

            }

            ~server_t() {

            }

            bool send_packet(uint32_t process_id, std::unique_ptr<packet> pack) {
                retutn conn_mgr.send_packet(process_id, std::move(pack));
            }

            bool send_packet(uint32_t process_id, std::unique_ptr<packet> pack, recv_callback_t cb) {
                return conn_mgr.send_packet(process_id, std::move(pack), cb);
            }

            bool cancel_sending(uint32_t process_id, uint32_t cmd, uint32_t seq) {
                return conn_mgr.cancel_sending(process_id, cmd, seq);
            }

            void register_push_receiver(uint32_t cmd, recv_callback_t cb) {
                conn_mgr.register_push_receiver(cmd, cb);
            }

            void unregister_push_receiver(uint32_t cmd) {
                conn_mgr.unregister_push_receiver(cmd);
            }

        private:
            connection_mgr_t conn_mgr;
        };

}}