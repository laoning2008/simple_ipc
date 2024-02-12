#pragma once
#include "simple_ipc/ipc/ipc_client.hpp"
#include "simple_ipc/detail/rpc_protocol.hpp"
#include <google/protobuf/message.h>

namespace simple::ipc {
        class rpc_client_t {

        public:
            explicit rpc_client_t(ipc_client_t& ipc_client) : client(ipc_client) {
            }

            template<std::derived_from<google::protobuf::Message> T>
            uint32_t call(const std::string& rpc_name, const google::protobuf::Message& request_message, std::function<void(rpc_result<T> msg)> callback) {
                auto req = build_request_packet(rpc_name, request_message);
                auto id = req->seq();
                client.send_packet(std::move(req), [this, callback](std::unique_ptr<packet> rsp) {
                    callback(parse_body<T>(rsp));
                }, 5);

                return id;
            }

            void cancel_call(const std::string& rpc_name, uint32_t id) {
                client.cancel_sending(rpc_id(rpc_name), id);
            }

            template<std::derived_from<google::protobuf::Message> REQ, std::derived_from<google::protobuf::Message> RSP>
            void register_handler(const std::string& rpc_name, std::function<rpc_result<RSP>(REQ&)> handler) {
                client.register_request_processor(rpc_id(rpc_name), [this, handler](std::unique_ptr<packet> req) {
                    auto req_message = parse_body<REQ>(req);
                    if (!req_message) {
                        return;
                    }

                    auto rsp_result = handler(req_message.value());
                    auto rsp = build_response_packet(req, rsp_result);
                    client.send_packet(std::move(rsp));
                });
            }

            void unregister_handler(const std::string& rpc_name) {
                client.unregister_request_processor(rpc_id(rpc_name));
            }
        private:
            ipc_client_t& client;
        };

}