#pragma once
#include "simple_ipc/ipc/ipc_server.hpp"
#include <google/protobuf/message.h>
#include "simple_ipc/detail/rpc_protocol.hpp"

namespace simple::ipc {
        class rpc_server_t {
        public:
            explicit rpc_server_t(ipc_server_t& ipc_server)
            : server(ipc_server) {
            }

            template<std::derived_from<google::protobuf::Message> T>
            uint32_t call(uint32_t connection_id, const std::string& rpc_name, const google::protobuf::Message& request_message, std::function<void(rpc_result<T> msg)> callback) {
                auto req = build_request_packet(rpc_name, request_message, connection_id);
                auto id = req->seq();
                bool result = server.send_packet(std::move(req), [this, callback](std::unique_ptr<packet> rsp) {
                    callback(parse_body<T>(rsp));
                }, 5);

                return result ? id : 0;
            }

            void cancel_call(uint32_t connection_id, const std::string& rpc_name, uint32_t id) {
                server.cancel_sending(connection_id, rpc_id(rpc_name), id);
            }

            template<std::derived_from<google::protobuf::Message> REQ, std::derived_from<google::protobuf::Message> RSP>
            void register_handler(const std::string& rpc_name, std::function<rpc_result<RSP>(REQ&)> handler) {
                server.register_request_processor(rpc_id(rpc_name), [this, handler](std::unique_ptr<packet> req) {
                    auto req_message = parse_body<REQ>(req);
                    if (!req_message) {
                        return;
                    }

                    auto rsp_result = handler(req_message.value());
                    auto rsp = build_response_packet(req, rsp_result);
                    server.send_packet(std::move(rsp));
                });
            }

            void unregister_handler(const std::string& rpc_name) {
                server.unregister_request_processor(rpc_id(rpc_name));
            }
        private:
            ipc_server_t& server;
        };

}