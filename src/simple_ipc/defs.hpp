#pragma once

#include <cstdint>
#include <unordered_map>
#include <functional>
#include "packet.hpp"

namespace simple { namespace ipc {
        using recv_callback_t = std::function<void(std::unique_ptr<packet> pack)>;
        using map_cmd_2_callback_t = std::unordered_map<uint32_t, recv_callback_t >;
}}