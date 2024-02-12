#include <simple_ipc/rpc/rpc_server.hpp>
#include <thread>
#include <iostream>
#include <csignal>
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

    simple::ipc::ipc_server_t server{server_name};
    simple::ipc::rpc_server_t rpc_server{server};

    rpc_server.register_handler<add_req, add_rsp>("add", [](add_req& req) -> rpc_result<add_rsp> {
        add_rsp rsp;
        rsp.set_result(req.left() + req.right());
        return rpc_result<add_rsp>{rsp};
    });

    while(!stoped) {
        add_req req;
        req.set_left(1);
        req.set_right(2);

        auto ret = rpc_server.call<add_rsp>(1, "add_s", req, [](rpc_result<add_rsp> result) {
            if (!result) {
                std::cout << "add request failed" << std::endl;
            } else {
                std::cout << "add request result = " << result.value().result() << std::endl;
            }
        });

        if (ret == 0) {
            std::cout << "rpc call failed" << std::endl;
        }

        std::this_thread::sleep_for(1000ms);
    }
    return 0;
}