#include <simple_ipc/rpc/rpc_client.hpp>
#include <thread>
#include <csignal>
#include <iostream>
#include "example.pb.h"
using namespace std::chrono;
using namespace simple::ipc;

const char* server_name = "simple_ipc";
volatile bool stoped = false;

void signal_handler(int signal) {
    stoped = true;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);

    simple::ipc::ipc_client_t ipc_client{server_name};
    simple::ipc::rpc_client_t rpc_client{ipc_client};

    rpc_client.register_handler<add_req, add_rsp>("add_s", [](add_req& req) -> rpc_result<add_rsp> {
        add_rsp rsp;
        rsp.set_result(req.left() + req.right());
        return rpc_result<add_rsp>{rsp};
    });

    while(!stoped) {
        add_req req;
        req.set_left(1);
        req.set_right(2);

        rpc_client.call<add_rsp>("add", req, [](rpc_result<add_rsp> result) {
            if (!result) {
                std::cout << "add request failed" << std::endl;
            } else {
                std::cout << "add request result = " << result.value().result() << std::endl;
            }
        });

        std::this_thread::sleep_for(20ms);
    }
    return 0;
}
