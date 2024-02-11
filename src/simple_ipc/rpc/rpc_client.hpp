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
            void call(const std::string& rpc_name, const google::protobuf::Message& request_message, std::function<void(rpc_result<T> msg)> callback) {
                auto req = build_request_packet(rpc_name, request_message);
                client.send_packet(std::move(req), [this, callback](std::unique_ptr<packet> rsp) {
                    callback(parse_body<T>(rsp));
                }, 5);
            }
        private:
            ipc_client_t& client;
        };

}