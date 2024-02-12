#pragma once

#include <string_view>
#include <optional>

#include "simple_ipc/detail/packet.hpp"
#include "simple_ipc/detail/err_code.hpp"

#include <google/protobuf/message.h>


namespace simple::ipc {
    uint32_t rpc_id(const std::string& rpc_name) {
        std::hash<std::string> hasher;
        return (uint32_t)hasher(rpc_name);
    }

    std::unique_ptr<packet> build_request_packet(const std::string& rpc_name, const google::protobuf::Message& message, uint32_t connection_id = 0) {
        uint32_t size = (uint32_t)message.ByteSizeLong();
        ibuffer body{size};
        message.SerializeToArray(body.data(), (int)size);

        auto id = rpc_id(rpc_name);

        return build_req_packet(connection_id, id, body.data(), body.size());
    }

    template<std::derived_from<google::protobuf::Message> RSP>
    std::unique_ptr<packet> build_response_packet(std::unique_ptr<packet>& req, rpc_result<RSP>& rsp_message) {
        if (!rsp_message) {
            return build_rsp_packet(req->connection_id(), req->cmd(), req->seq(), rsp_message.error(), nullptr, 0);
        }

        auto& proto_message = rsp_message.value();
        auto size = (uint32_t)proto_message.ByteSizeLong();
        ibuffer body{size};
        proto_message.SerializeToArray(body.data(), (int)size);

        return build_rsp_packet(req->connection_id(), req->cmd(), req->seq(), 0, body.data(), body.size());
    }

    template<std::derived_from<google::protobuf::Message> T>
    rpc_result<T> parse_body(std::unique_ptr<packet>& pack) {
        if (pack == nullptr) {
            return rpc_unexpected_result{err_network};
        }
        auto ec = pack->ec();
        if (ec != 0) {
            return rpc_unexpected_result{ec};
        }

        T message;
        if (!message.ParseFromArray(pack->body().data(), pack->body().size())) {
            return rpc_unexpected_result{err_deserialize};
        }

        return rpc_result<T>{message};
    }

}

