#pragma once
#include <memory>
#include <string>
#include <atomic>
#include <memory.h>

#include "crc.hpp"
#include "byte_order.hpp"
#include "ibuffer.hpp"

namespace simple { namespace ipc {

static std::atomic<uint32_t> seq_generator{0};
constexpr static uint8_t packet_begin_flag = 0x55;
constexpr static uint32_t device_id_size = 64;

#pragma pack(1)
struct packet_header {
    uint8_t         flag;
    uint32_t        process_id;
    uint32_t        cmd;
    uint32_t        seq;
    uint8_t         rsp;
    uint32_t        ec;
    uint32_t        body_len;
    uint8_t         crc;
};
#pragma pack()

constexpr static uint32_t header_length = sizeof(packet_header);
constexpr static uint32_t max_body_length = 16*1024;

class packet {
    friend ibuffer encode_packet(const packet& pack);
    friend std::unique_ptr<packet> decode_packet(uint8_t* buf, size_t buf_len, size_t& consume_len);
public:
    packet() : process_id_(0), cmd_(0), seq_(0), rsp_(false), ec_(0) {
    }
    
    packet(uint32_t process_id__, uint32_t cmd__, bool rsp__, uint8_t* body_buf__, uint32_t body_len__, uint32_t seq__, uint32_t ec__ = 0)
    : process_id_(process_id__), cmd_(cmd__), seq_((seq__==0)?next_seq():seq__), rsp_(rsp__), ec_(ec__), body_( body_len__, body_buf__) {
    }

    uint32_t process_id() const {
        return cmd_;
    }

    uint32_t cmd() const {
        return cmd_;
    }
    
    uint32_t seq() const {
        return seq_;
    }
    
    bool is_response() const {
        return rsp_;
    }
    
    uint32_t ec() const {
        return ec_;
    }
 
    const ibuffer& body() const {
        return body_;
    }
private:
    uint32_t next_seq() {
        if (seq_generator == UINT_MAX) {
            seq_generator = 0;
        }
        
        return ++seq_generator;
    }
private:
    uint32_t        process_id_;
    uint32_t        cmd_;
    uint32_t        seq_;
    bool            rsp_;
    uint32_t        ec_;
    ibuffer         body_;
};

static uint64_t packet_id(uint32_t cmd, uint32_t seq) {
    uint64_t id = cmd;
    id = id << 31 | seq;
    return id;
}

static uint64_t packet_id(const std::unique_ptr<packet>& pack) {
    return packet_id(pack->cmd(), pack->seq());
}

static std::unique_ptr<packet> build_req_packet(uint32_t process_id, uint32_t cmd, uint8_t* body_buf = nullptr, uint32_t body_len = 0) {
    return std::make_unique<packet>(process_id, cmd, false, body_buf, body_len, 0);
}

static std::unique_ptr<packet> build_rsp_packet(uint32_t process_id, uint32_t cmd, uint32_t seq, uint32_t ec, uint8_t* body_buf, uint32_t body_len) {
    return std::make_unique<packet>(process_id, cmd, true, body_buf, body_len, seq, ec);
}

ibuffer encode_packet(const std::unique_ptr<packet>& pack) {
    auto data_len = header_length + pack->body().size();
    auto data_buf = ibuffer{data_len};
    
    packet_header header;
    header.flag = packet_begin_flag;
    header.process_id = host_to_network_32(pack->process_id());
    header.cmd = host_to_network_32(pack->cmd());
    header.seq = host_to_network_32(pack->seq());
    header.ec = host_to_network_32(pack->ec());
    header.rsp = pack->is_response() ? 1 : 0;
    header.body_len = host_to_network_32(pack->body().size());
    header.crc = calc_crc8((uint8_t*)&header, header_length - 1);
    
    memcpy(data_buf.data(), &header, header_length);
    if (!pack->body().empty()) {
        memcpy(data_buf.data() + header_length, pack->body().data(), pack->body().size());
    }
    
    return data_buf;
}

std::unique_ptr<packet> decode_packet(uint8_t* buf, size_t buf_len, size_t& consume_len) {
    consume_len = 0;
    do {
        for (; consume_len < buf_len; ++consume_len) {
            if (buf[consume_len] == packet_begin_flag) {
                break;
            }
        }
        
        uint8_t* buf_valid = buf + consume_len;
        size_t buf_valid_len = buf_len - consume_len;
        
        if (buf_valid_len < header_length) {
            return nullptr;
        }
        
        packet_header* header = (packet_header*)buf_valid;
        
        auto crc = calc_crc8(buf_valid, header_length - 1);
        if (crc != header->crc) {
            ++consume_len;
            continue;
        }
        
        uint32_t body_len = network_to_host_32(header->body_len);
        
        if (body_len > max_body_length) {
            consume_len += header_length;
            continue;
        }
        
        if (buf_valid_len < header_length + body_len) {
            return nullptr;
        }
        consume_len += header_length + body_len;

        uint32_t process_id = network_to_host_32(header->process_id);
        uint32_t cmd = network_to_host_32(header->cmd);
        uint32_t seq = network_to_host_32(header->seq);
        uint32_t ec = network_to_host_32(header->ec);
        bool rsp = header->rsp != 0;
        return std::make_unique<packet>(process_id, cmd, rsp, buf_valid + header_length, seq, body_len, ec);
    } while (1);
}

}}
