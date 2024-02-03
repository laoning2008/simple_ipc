#pragma once
#include "connection_mgr.hpp"
#include "listener.hpp"
using namespace std::placeholders;

namespace simple { namespace ipc {
        class server_t {
        public:
            server_t(std::string server_name)
            : listener(std::move(server_name), std::bind(&server_t::on_new_connection, this, _1)) {

            }

            ~server_t() {

            }

            bool send_packet(std::unique_ptr<packet> pack) {
                return conn_mgr.send_packet(pack->process_id(), std::move(pack));
            }

            bool send_packet(std::unique_ptr<packet> pack, recv_callback_t cb) {
                return conn_mgr.send_packet(pack->process_id(), std::move(pack), cb);
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
            void on_new_connection(int fd) {
                conn_mgr.new_connection(fd);
            }
        private:
            listener_t listener;
            connection_mgr_t conn_mgr;
        };

}}