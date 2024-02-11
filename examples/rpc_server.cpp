#include <simple_ipc/rpc/rpc_server.hpp>
#include <thread>
#include <iostream>
#include "example.pb.h"
using namespace std::chrono;
using namespace simple::ipc;

const char* server_name = "simple_ipc";
volatile bool stoped = false;

int main(int argc, char** argv) {
    simple::ipc::ipc_server_t server{server_name};
    simple::ipc::rpc_server_t rpc_server{server};

    rpc_server.register_handler<add_req, add_rsp>("add", [](add_req& req) -> rpc_result<add_rsp> {
        add_rsp rsp;
        rsp.set_result(req.left() + req.right());
        return rpc_result<add_rsp>{rsp};
    });

    char c = 0;
    std::cin >> c;
    return 0;
}